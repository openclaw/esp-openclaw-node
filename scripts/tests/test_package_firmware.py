"""Exercise firmware bundle contents and rejection boundaries without ESP-IDF."""

import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "package_firmware.py"
SPEC = importlib.util.spec_from_file_location("package_firmware", SCRIPT)
firmware = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(firmware)
HAS_YAML = importlib.util.find_spec("ruamel") is not None


class FirmwareBundleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="firmware-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.repo = self.root / "repository"
        self.idf = self.root / "idf"
        self.project = self.repo / "examples/esp32-node"
        self.build = self.project / "build"
        self.output = self.build / "firmware"
        self.build.mkdir(parents=True)
        self.idf.mkdir()
        partition = self.idf / "components/partition_table/partitions_singleapp_large.csv"
        partition.parent.mkdir(parents=True)
        partition.write_text("factory,app,factory,0x10000,3M,\n")
        self.environment = patch.dict(os.environ, {
            "IDF_PATH": str(self.idf),
            "OPENCLAW_TAB5_BSP_LOCAL_PATH": "",
            "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_CONFIG_NOSYSTEM": "1",
        })
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.versions = patch.object(
            firmware.importlib.metadata, "version",
            side_effect=lambda name: {"esptool": "4.11.0", "idf-component-manager": "2.4.3"}[name],
        )
        self.versions.start()
        self.addCleanup(self.versions.stop)
        for repo in (self.repo, self.idf):
            self.git(repo, "init", "-q")
            (repo / ".gitignore").write_text("examples/\n")
            self.git(repo, "add", ".")
            self.git(repo, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.com",
                     "-c", "commit.gpgsign=false", "commit", "-qm", "fixture")
        self.config = (
            'CONFIG_IDF_TARGET="esp32s3"\nCONFIG_OPENCLAW_ROOM_WIFI_PASSWORD=""\n'
            'CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp_large.csv"\n'
        )
        (self.project / "sdkconfig.defaults").write_text("CONFIG_LOG_DEFAULT_LEVEL_INFO=y\n")
        (self.project / "sdkconfig.defaults.esp32s3").write_text("CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y\n")
        # Contract fields from ESP-IDF v5.5.5 tools/cmake/project_description.json.in
        # and components/esptool_py/flasher_args.json.in, not a captured device build.
        self.description = {
            "version": "1.2",
            "project_name": "test_node",
            "project_version": "1.0.0",
            "project_path": str(self.project),
            "build_dir": str(self.build),
            "config_file": str(self.project / "sdkconfig"),
            "config_defaults": str(self.project / "sdkconfig.defaults"),
            "idf_path": str(self.idf),
            "git_revision": "v5.5.5",
            "target": "esp32s3",
            "min_rev": "0",
            "max_rev": "199",
            "monitor_baud": "115200",
            "app_bin": "app.bin",
            "build_components": ["main", "esp-sr"],
        }
        self.flash = {
            "write_flash_args": ["--flash_mode", "dio", "--flash_size", "16MB", "--flash_freq", "80m"],
            "flash_settings": {"flash_mode": "dio", "flash_size": "16MB", "flash_freq": "80m"},
            "extra_esptool_args": {
                "chip": "esp32s3", "before": "default_reset", "after": "hard_reset", "stub": True,
            },
            "flash_files": {
                "0x0": "bootloader/bootloader.bin",
                "0x8000": "partition_table/partition-table.bin",
                "0x10000": "app.bin",
                "0x810000": "../managed_components/speech models/model.bin",
            },
        }
        for name, offset in (("bootloader", "0x0"), ("partition-table", "0x8000"), ("app", "0x10000"), ("model", "0x810000")):
            filename = self.flash["flash_files"][offset]
            self.flash[name] = {"offset": offset, "file": filename, "encrypted": "false"}
            path = self.build / filename
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes((name + "-fixture").encode())
        self.dependencies = {
            "version": "2.0.0",
            "target": "esp32s3",
            "dependencies": {
                "espressif/example": {
                    "version": "1.2.3", "component_hash": "a" * 64,
                    "source": {"type": "service", "service_url": "https://api.components.espressif.com/"},
                },
                "local": {"version": "*", "source": {"type": "local", "path": str(self.project)}},
            },
        }
        self.provenance = {
            "repository": "openclaw/esp-openclaw-node", "idf_image": "espressif/idf:release-v5.5",
            "event": "pull_request", "event_sha": "1" * 40, "pr_head_sha": "2" * 40,
            "run_id": "123", "run_attempt": "2",
        }

    def git(self, repo, *arguments):
        return subprocess.check_output(
            ["git", "-C", str(repo), *arguments], text=True, stderr=subprocess.PIPE
        ).strip()

    def write_metadata(self):
        (self.project / "sdkconfig").write_text(self.config)
        (self.project / "dependencies.lock").write_text(json.dumps(self.dependencies))
        (self.build / "project_description.json").write_text(json.dumps(self.description))
        (self.build / "flasher_args.json").write_text(json.dumps(self.flash))

    def package(self):
        self.write_metadata()
        return firmware.package_firmware(
            self.project, self.build, self.output, self.description["target"],
            self.provenance, self.dependencies,
        )

    def test_relocated_bundle_preserves_every_image_and_original_inputs(self):
        self.write_metadata()
        originals = {path: path.read_bytes() for path in self.project.rglob("*") if path.is_file()}
        self.package()
        for path, content in originals.items():
            self.assertEqual(path.read_bytes(), content)
        relocated = self.root / "extracted bundle"
        shutil.copytree(self.output, relocated)
        metadata = json.loads((relocated / "flasher_args.json").read_text())
        self.assertEqual(set(metadata["flash_files"]), set(self.flash["flash_files"]))
        for offset, filename in metadata["flash_files"].items():
            self.assertEqual(
                (relocated / filename).read_bytes(),
                (self.build / self.flash["flash_files"][offset]).read_bytes(),
            )
        self.assertEqual(metadata["model"]["file"], metadata["flash_files"]["0x810000"])
        arguments = shlex.split((relocated / "flash_args").read_text())
        self.assertEqual(arguments[:6], self.flash["write_flash_args"])
        self.assertEqual(dict(zip(arguments[6::2], arguments[7::2])), metadata["flash_files"])
        checksummed = set()
        for line in (relocated / "SHA256SUMS").read_text().splitlines():
            digest, name = line.split("  ", 1)
            self.assertEqual(hashlib.sha256((relocated / name).read_bytes()).hexdigest(), digest)
            checksummed.add(name)
        self.assertEqual(checksummed, {
            path.relative_to(relocated).as_posix() for path in relocated.rglob("*")
            if path.is_file() and path.name != "SHA256SUMS"
        })
        manifest = json.loads((relocated / "manifest.json").read_text())
        self.assertEqual(manifest["source"]["commit"], self.git(self.repo, "rev-parse", "HEAD"))
        self.assertEqual(manifest["idf"]["commit"], self.git(self.idf, "rev-parse", "HEAD"))
        self.assertEqual(manifest["idf"]["build_revision"], "v5.5.5")
        self.assertEqual(manifest["ci"]["pr_head_sha"], "2" * 40)
        self.assertEqual(manifest["ci"]["event_sha"], "1" * 40)
        inputs = manifest["configuration_inputs"]
        self.assertEqual([item["kind"] for item in inputs], [
            "defaults", "target_defaults", "partition_table",
        ])
        self.assertEqual(inputs[1]["sha256"], firmware.sha256(self.project / "sdkconfig.defaults.esp32s3"))
        self.assertEqual(inputs[2]["path"], "<idf>/components/partition_table/partitions_singleapp_large.csv")
        for path in relocated.glob("*"):
            if path.is_file():
                self.assertNotIn(str(self.root), path.read_text())
        for name, record in manifest["files"].items():
            self.assertEqual(record["sha256"], firmware.sha256(relocated / name))
            self.assertEqual(record["size"], (relocated / name).stat().st_size)
        (relocated / metadata["app"]["file"]).write_bytes(b"damaged")
        self.assertNotEqual(
            firmware.sha256(relocated / metadata["app"]["file"]),
            manifest["files"][metadata["app"]["file"]]["sha256"],
        )

    def test_invalid_inputs_never_leave_uploadable_output(self):
        cases = (
            ("missing", lambda: (self.build / "app.bin").unlink()),
            ("escape", lambda: self.flash["flash_files"].update({"0x900000": str(self.root / "outside.bin")})),
            ("target", lambda: self.flash["extra_esptool_args"].update(chip="esp32p4")),
            ("config target", lambda: setattr(self, "config", 'CONFIG_IDF_TARGET="esp32p4"\n')),
            ("lock target", lambda: self.dependencies.update(target="esp32p4")),
            ("encrypted", lambda: self.flash["app"].update(encrypted="true")),
            ("signed config", lambda: setattr(self, "config", self.config + "CONFIG_SECURE_BOOT=y\n")),
            ("credentials", lambda: setattr(self, "config", self.config + 'CONFIG_OPENCLAW_ROOM_SETUP_CODE="synthetic-secret"\n')),
            ("overlap", lambda: self.flash["flash_files"].update({"0x1": "app.bin"})),
            ("named entry", lambda: self.flash["app"].update(file="other.bin")),
            ("unsupported args", lambda: self.flash["write_flash_args"].append("--force")),
            ("dirty source", lambda: (self.repo / ".gitignore").write_text("changed\n")),
        )
        baseline = copy.deepcopy((self.flash, self.config, self.dependencies))
        (self.root / "outside.bin").write_bytes(b"outside")
        for name, change in cases:
            with self.subTest(name=name):
                self.flash, self.config, self.dependencies = copy.deepcopy(baseline)
                (self.build / "app.bin").write_bytes(b"app-fixture")
                (self.repo / ".gitignore").write_text("examples/\n")
                change()
                with self.assertRaises((ValueError, FileNotFoundError)):
                    self.package()
                self.assertFalse(self.output.exists())

    def test_symlink_escape_is_rejected(self):
        external = self.root / "outside.bin"
        external.write_bytes(b"outside")
        (self.build / "app.bin").unlink()
        (self.build / "app.bin").symlink_to(external)
        with self.assertRaises(ValueError):
            self.package()
        self.assertFalse(self.output.exists())

    def test_in_root_symlink_is_relocated_as_a_regular_image(self):
        (self.build / "app.bin").rename(self.build / "actual.bin")
        (self.build / "app.bin").symlink_to("actual.bin")
        self.package()
        image = self.output / "images/00010000.bin"
        self.assertFalse(image.is_symlink())
        self.assertEqual(image.read_bytes(), b"app-fixture")

    @unittest.skipUnless(HAS_YAML, "real lock reader runs in the existing IDF environment")
    def test_lock_reader_preserves_yaml_dependency_identity(self):
        lock = self.project / "dependencies.lock"
        lock.write_text(
            "# IDF component-manager 2.x lock schema\n"
            "version: 2.0.0\ntarget: esp32s3\nmanifest_hash: abcdef\n"
            "dependencies:\n"
            "  espressif/example:\n"
            "    version: 1.2.3\n"
            "    component_hash: abcdef\n"
            "    source:\n"
            "      type: service\n"
            "      service_url: https://api.components.espressif.com/\n"
            "  local:\n"
            "    version: '*'\n"
            "    source:\n"
            "      type: local\n"
            "      path: ../../components/local\n"
        )
        original = lock.read_bytes()
        dependencies = firmware.read_dependency_lock(lock)
        self.assertEqual(dependencies["target"], "esp32s3")
        self.assertEqual(dependencies["dependencies"]["espressif/example"]["version"], "1.2.3")
        self.assertEqual(dependencies["dependencies"]["local"]["source"]["path"], "../../components/local")
        self.assertEqual(lock.read_bytes(), original)
        lock.write_text("- not-a-lock\n")
        with self.assertRaises(ValueError):
            firmware.read_dependency_lock(lock)

    def test_reset_and_no_stub_settings_reach_flash_command(self):
        self.flash["extra_esptool_args"].update(stub=False, before="no_reset", after="no_reset")
        self.package()
        instructions = (self.output / "FLASHING.md").read_text()
        self.assertIn("--before no_reset --after no_reset --no-stub", instructions)
        self.assertIn('write_flash "@flash_args"', instructions)

    def test_metadata_sanitization_keeps_dependency_identity(self):
        source = self.dependencies["dependencies"]["local"]["source"]
        source["path"] = "/private/build-owner/components/example"
        source["url"] = "https://fixture:synthetic-secret@github.com/example/repo"
        self.config += 'CONFIG_EXAMPLE_PATH="/private/build-owner/config"\n'
        self.package()
        text = (self.output / "dependencies.lock.sanitized.json").read_text()
        self.assertNotIn("build-owner", text)
        self.assertNotIn("synthetic-secret", text)
        resolved = json.loads(text)["dependencies"]["espressif/example"]
        self.assertEqual(resolved, self.dependencies["dependencies"]["espressif/example"])

    def test_pinned_submodule_commit_is_recorded(self):
        module = self.repo / "third_party/sdk"
        module.parent.mkdir()
        self.git(self.repo, "-c", "protocol.file.allow=always", "submodule", "add", "-q", str(self.idf), "third_party/sdk")
        self.git(self.repo, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.com",
                 "-c", "commit.gpgsign=false", "commit", "-qam", "submodule fixture")
        self.package()
        source = json.loads((self.output / "manifest.json").read_text())["source"]
        self.assertEqual(source["submodules"], [
            {"path": "third_party/sdk", "commit": self.git(self.idf, "rev-parse", "HEAD")}
        ])

    def test_tab5_records_upstream_and_local_bridge_identity(self):
        destination = self.repo / "examples/m5stack-tab5-room-node"
        self.project.rename(destination)
        self.project = destination
        self.build = destination / "build"
        self.output = self.build / "firmware"
        self.description.update(
            target="esp32p4", project_path=str(destination), build_dir=str(self.build),
            config_file=str(destination / "sdkconfig"),
            config_defaults=str(destination / "sdkconfig.defaults"),
        )
        self.dependencies["target"] = "esp32p4"
        self.config = (
            'CONFIG_IDF_TARGET="esp32p4"\nCONFIG_PARTITION_TABLE_CUSTOM=y\n'
            'CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"\n'
        )
        (destination / "partitions.csv").write_text("factory,app,factory,0x10000,8M,\n")
        self.flash["extra_esptool_args"]["chip"] = "esp32p4"
        bridge = destination / "components/m5stack_tab5/CMakeLists.txt"
        bridge.parent.mkdir(parents=True)
        bridge.write_text(
            "FetchContent_Declare(tab5_bsp_source\n"
            f" URL https://github.com/espressif/esp-bsp/archive/{'a' * 40}.tar.gz\n"
            f" URL_HASH SHA256={'b' * 64})\n"
        )
        (self.build / "CMakeCache.txt").write_text("OPENCLAW_TAB5_BSP_LOCAL_PATH:PATH=\n")
        self.package()
        manifest = json.loads((self.output / "manifest.json").read_text())
        self.assertEqual(manifest["tab5_bsp"]["upstream_commit"], "a" * 40)
        self.assertEqual(manifest["tab5_bsp"]["upstream_archive_sha256"], "b" * 64)
        self.assertEqual(manifest["tab5_bsp"]["bridge_sha256"], firmware.sha256(bridge))
        self.assertIn("not an unmodified upstream", manifest["tab5_bsp"]["integration"])
        self.assertEqual(manifest["configuration_inputs"][-1], {
            "kind": "partition_table", "path": "<project>/partitions.csv",
            "sha256": firmware.sha256(destination / "partitions.csv"),
        })


if __name__ == "__main__":
    unittest.main()
