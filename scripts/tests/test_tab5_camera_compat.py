"""Exercise exact component guards and the real upstream camera format mapper."""

from contextlib import contextmanager
import copy
import difflib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from test_idf_tab5_compat import fixture_profile, seed_sdk
from test_tab5_sdio_diagnostics import HAS_MANAGER, directory_hash, REGISTRY_SOURCE
import idf_tab5_compat as sdk
import tab5_camera_compat as camera


ORIGINAL = b"/* Synthetic camera component, not a pipeline simulation. */\nint original;\n"
PATCHED = ORIGINAL.replace(b"original", b"patched")
SDK_SOURCE = b"/* Synthetic pinned SDK CSI contract fixture. */\n"
OBJECT_PATH = "esp-idf/esp_video/CMakeFiles/esp_video.dir/src/device/esp_video_csi_format.c.obj"


def seed_camera_sdk(idf):
    seed_sdk(idf)
    source = idf / camera.SDK_SOURCE
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_bytes(SDK_SOURCE)
    sdk.git(idf, "add", ".")
    sdk.git(idf, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.com",
            "-c", "commit.gpgsign=false", "commit", "-qm", "CSI contract fixture")
    return sdk.git(idf, "rev-parse", "HEAD").decode().strip()


def write_object(build, source):
    output = build / OBJECT_PATH
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(b"\x7fELF\x01\x01" + b"\x00" * 10 + b"\x01\x00\xf3\x00fixture-object")
    stamp = max(output.stat().st_mtime_ns, source.stat().st_mtime_ns + 1)
    os.utime(output, ns=(stamp, stamp))
    return output


@contextmanager
def camera_fixture(project, build, idf, dependencies):
    component = project / "managed_components/espressif__esp_video"
    source = component / camera.SOURCE_PATH
    source.parent.mkdir(parents=True)
    source.write_bytes(ORIGINAL)
    (component / "other.c").write_text("/* Unrelated component source. */\n")
    manifest = component / "idf_component.yml"
    manifest.write_text(json.dumps({
        "version": camera.VERSION,
        "repository": "git://github.com/espressif/esp-video-components.git",
        "repository_info": {"commit_sha": camera.REVISION, "path": "esp_video"},
        "dependencies": {"idf": ">=5.4"},
    }))
    original_hash = directory_hash(component)
    (component / ".component_hash").write_text(original_hash)
    source.write_bytes(PATCHED)
    patched_hash = directory_hash(component)
    source.write_bytes(ORIGINAL)
    patch_file = build / "camera-fixture.patch"
    patch_file.write_text("".join(difflib.unified_diff(
        ORIGINAL.decode().splitlines(keepends=True), PATCHED.decode().splitlines(keepends=True),
        fromfile="a/" + camera.SOURCE_PATH, tofile="b/" + camera.SOURCE_PATH,
    )))
    commands_file = build / "compile_commands.json"
    commands = json.loads(commands_file.read_text()) if commands_file.exists() else []
    commands.append({
        "directory": str(build), "file": str(source),
        "arguments": ["riscv32-esp-elf-gcc", "-o", OBJECT_PATH, "-c", str(source)],
    })
    commands_file.write_text(json.dumps(commands))
    dependencies["target"] = "esp32p4"
    dependencies.setdefault("dependencies", {})[camera.COMPONENT] = {
        "version": camera.VERSION, "component_hash": original_hash,
        "source": copy.deepcopy(REGISTRY_SOURCE),
    }
    with patch.multiple(
        camera, COMPONENT_HASH=original_hash, PATCHED_COMPONENT_HASH=patched_hash,
        MANIFEST_SHA256=camera.digest(manifest.read_bytes()),
        ORIGINAL_SHA256=camera.digest(ORIGINAL), PATCHED_SHA256=camera.digest(PATCHED),
        PATCH_PATH=patch_file, PATCH_SHA256=camera.digest(patch_file.read_bytes()),
        SDK_SOURCE_SHA256=camera.digest(SDK_SOURCE),
        BASE_COMMIT=sdk.git(idf, "rev-parse", "HEAD").decode().strip(),
    ):
        yield component


@unittest.skipUnless(HAS_MANAGER, "requires the real IDF component-manager environment")
class CameraPatchTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="camera-compat-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.project = self.root / "examples/m5stack-tab5-room-node"
        self.build = self.project / "build"
        self.build.mkdir(parents=True)
        self.idf = self.root / "idf"
        self.idf.mkdir()
        self.enterContext(patch.dict(os.environ, {
            "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1",
        }))
        for root in (self.root, self.idf):
            sdk.git(root, "init", "-q")
        self.base = seed_camera_sdk(self.idf)
        self.enterContext(fixture_profile(self.base))
        sdk.apply_sdk_patch(self.idf, nvs_diagnostics=True)
        self.dependencies = {}
        self.component = self.enterContext(camera_fixture(
            self.project, self.build, self.idf, self.dependencies))
        self.source = self.component / camera.SOURCE_PATH
        self.config = self.project / "sdkconfig"
        self.config.write_text("".join(f"{k}={v}\n" for k, v in camera.PROFILE.items()))
        (self.build / "project_description.json").write_text(json.dumps({
            "idf_path": str(self.idf), "project_path": str(self.project),
            "build_dir": str(self.build), "target": "esp32p4", "config_file": str(self.config),
        }))

    def apply(self):
        return camera.apply_component_patch(
            self.project, self.component, self.build, self.idf, self.dependencies)

    def verify(self, **options):
        return camera.verify_component_patch(
            self.project, self.component, self.build, self.idf, self.dependencies, **options)

    def test_exact_application_idempotence_integrity_markers_and_metadata(self):
        before = {p: p.read_bytes() for p in self.component.rglob("*") if p.is_file()}
        sdk_before = sdk.git(self.idf, "diff")
        expected = self.apply()
        self.assertEqual(self.apply(), expected)
        self.assertEqual(self.source.read_bytes(), PATCHED)
        for path, data in before.items():
            if path != self.source:
                self.assertEqual(path.read_bytes(), data)
        self.assertEqual(sdk.git(self.idf, "diff"), sdk_before)
        output = write_object(self.build, self.source)
        verified = self.verify()
        self.assertEqual(verified["sdk_base_commit"], self.base)
        self.assertEqual(verified["patched_component_hash"], directory_hash(self.component))
        self.assertEqual(verified["patched_source_sha256"], camera.digest(PATCHED))
        self.assertEqual(verified["compiled_object"]["sha256"], camera.digest(output.read_bytes()))

    def test_verification_never_mutates_and_requires_native_current_object(self):
        with self.assertRaises(ValueError):
            self.verify()
        self.assertEqual(self.source.read_bytes(), ORIGINAL)
        self.assertFalse(self.verify(patched=False, compiled=False)["modified"])
        self.apply()
        with self.assertRaises(FileNotFoundError):
            self.verify()
        output = write_object(self.build, self.source)
        output.write_bytes(b"not-an-ELF")
        with self.assertRaises(ValueError):
            self.verify()
        write_object(self.build, self.source)
        os.utime(output, ns=(1, 1))
        with self.assertRaises(ValueError):
            self.verify()
        write_object(self.build, self.source)
        with self.assertRaises(ValueError):
            self.verify(patched=False)

    def test_wrong_sdk_or_tampered_contract_refuses_before_mutation(self):
        for module, key in ((sdk, "BASE_COMMIT"), (camera, "SDK_SOURCE_SHA256")):
            with self.subTest(key=key), patch.object(module, key, "0" * 64):
                with self.assertRaises(ValueError):
                    self.apply()
                self.assertEqual(self.source.read_bytes(), ORIGINAL)
        (self.idf / camera.SDK_SOURCE).write_bytes(SDK_SOURCE + b"/* tampered */\n")
        with self.assertRaises(ValueError):
            self.apply()
        self.assertEqual(self.source.read_bytes(), ORIGINAL)

    def test_wrong_profile_lock_and_registry_key_fail_closed(self):
        original = copy.deepcopy(self.dependencies)
        for key, value in (("version", "2.4.0"), ("component_hash", "0" * 64),
                           ("source", {"type": "service", "service_url": REGISTRY_SOURCE["registry_url"]})):
            with self.subTest(key=key):
                self.dependencies = copy.deepcopy(original)
                self.dependencies["dependencies"][camera.COMPONENT][key] = value
                with self.assertRaises(ValueError):
                    self.apply()
        self.dependencies = original
        before = self.config.read_text()
        for key in camera.PROFILE:
            self.config.write_text(before.replace(f"{key}={camera.PROFILE[key]}", f"{key}=unexpected"))
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.apply()
        self.assertEqual(self.source.read_bytes(), ORIGINAL)

    def test_patch_manifest_and_component_tampering_are_not_idempotence(self):
        for name in ("PATCH_SHA256", "MANIFEST_SHA256"):
            with self.subTest(name=name), patch.object(camera, name, "0" * 64):
                with self.assertRaises(ValueError):
                    self.apply()
                self.assertEqual(self.source.read_bytes(), ORIGINAL)
        self.apply()
        write_object(self.build, self.source)
        other = self.component / "other.c"
        other.write_text("/* Changed after the build. */\n")
        with self.assertRaises(ValueError):
            self.verify()
        with self.assertRaises(ValueError):
            self.apply()
        (self.component / ".component_hash").write_text(camera.PATCHED_COMPONENT_HASH)
        with self.assertRaises(ValueError):
            self.verify()

    def test_native_compile_path_cannot_select_another_mapper(self):
        path = self.build / "compile_commands.json"
        original = json.loads(path.read_text())
        wrong = self.build / Path(camera.SOURCE_PATH).name
        wrong.write_bytes(ORIGINAL)
        wrong_arguments = original[0]["arguments"][:-1] + [str(wrong)]
        for commands in ([], original * 2, [{**original[0], "file": str(wrong)}],
                         [{**original[0], "arguments": wrong_arguments}]):
            path.write_text(json.dumps(commands))
            with self.subTest(count=len(commands)), self.assertRaises(ValueError):
                self.apply()
        self.assertEqual(self.source.read_bytes(), ORIGINAL)

    def test_native_command_string_resolves_the_same_source_and_output(self):
        path = self.build / "compile_commands.json"
        commands = json.loads(path.read_text())
        commands[0]["command"] = shlex.join(commands[0].pop("arguments"))
        path.write_text(json.dumps(commands))
        self.apply()
        write_object(self.build, self.source)
        self.assertEqual(self.verify()["compiled_object"]["path"], OBJECT_PATH)

    def test_symlink_and_post_build_source_changes_are_rejected(self):
        link = self.component / "alias.c"
        link.symlink_to(self.source)
        with self.assertRaises(ValueError):
            self.apply()
        link.unlink()
        self.apply()
        write_object(self.build, self.source)
        self.source.write_bytes(PATCHED + b"/* unexpected */\n")
        with self.assertRaises(ValueError):
            self.verify()


class ActualMapperTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        component = Path(os.environ.get(
            "ESP_VIDEO_COMPONENT_DIR", "managed_components/espressif__esp_video"))
        source = component / camera.SOURCE_PATH
        if not source.is_file():
            raise unittest.SkipTest("requires the configured esp_video 2.4.1 source")
        data = source.read_bytes()
        source_hash = camera.digest(data)
        if source_hash not in (camera.ORIGINAL_SHA256, camera.PATCHED_SHA256):
            raise AssertionError("mapper fixture requires the pinned original or patched source")
        temporary = tempfile.TemporaryDirectory(prefix="camera-mapper-test-")
        cls.addClassCleanup(temporary.cleanup)
        cls.root = Path(temporary.name)
        original = cls.root / "original" / camera.SOURCE_PATH
        original.parent.mkdir(parents=True)
        original.write_bytes(data)
        if source_hash == camera.PATCHED_SHA256:
            subprocess.run(["git", "apply", "--reverse", str(camera.PATCH_PATH)],
                           cwd=cls.root / "original", check=True)
        if camera.digest(original.read_bytes()) != camera.ORIGINAL_SHA256:
            raise AssertionError("original mapper reconstruction differs")
        repaired = cls.root / "patched" / camera.SOURCE_PATH
        repaired.parent.mkdir(parents=True)
        repaired.write_bytes(original.read_bytes())
        subprocess.run(["git", "apply", str(camera.PATCH_PATH)], cwd=cls.root / "patched", check=True)
        if camera.digest(repaired.read_bytes()) != camera.PATCHED_SHA256:
            raise AssertionError("patched mapper differs")
        cls.binaries = {}
        for name, path in (("original", original), ("patched", repaired)):
            # Keep the complete upstream implementation; replace only its SDK includes.
            include = cls.root / name / "camera_mapper_under_test.inc"
            include.write_text("".join(
                line for line in path.read_text().splitlines(keepends=True)
                if not line.startswith("#include ")
            ))
            for version in ("0x050505", "0x060000"):
                output = cls.root / f"{name}-{version}"
                subprocess.run([
                    os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-sign-compare", f"-DESP_IDF_VERSION={version}",
                    "-I", str(include.parent),
                    str(Path(__file__).parent / "fixtures/test_tab5_camera_format.c"),
                    "-o", str(output),
                ], check=True)
                cls.binaries[name, version] = output

    def run_case(self, name, case, version="0x050505"):
        return subprocess.run([str(self.binaries[name, version]), case],
                              capture_output=True, text=True)

    def test_processing_is_red_on_original_and_green_on_repaired_mapper(self):
        original = self.run_case("original", "processing")
        self.assertEqual(original.returncode, 1, original.stdout + original.stderr)
        self.assertIn("post-ISP CSI input mismatch", original.stderr)
        repaired = self.run_case("patched", "processing")
        self.assertEqual(repaired.returncode, 0, repaired.stdout + repaired.stderr)

    def test_raw_bypass_and_unsupported_formats_are_unchanged(self):
        for name in ("original", "patched"):
            for case in ("bypass", "unsupported"):
                with self.subTest(name=name, case=case):
                    result = self.run_case(name, case)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_existing_six_point_zero_mapping_is_unchanged(self):
        for name in ("original", "patched"):
            for case in ("processing", "bypass", "unsupported"):
                with self.subTest(name=name, case=case):
                    result = self.run_case(name, case, "0x060000")
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
