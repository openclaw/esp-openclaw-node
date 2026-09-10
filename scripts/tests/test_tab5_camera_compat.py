"""Exercise exact component guards and the real upstream camera format mapper."""

from contextlib import contextmanager, redirect_stderr, redirect_stdout
import copy
import difflib
import io
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
    patch_file.write_text(
        f"diff --git a/{camera.SOURCE_PATH} b/{camera.SOURCE_PATH}\n"
        + "".join(difflib.unified_diff(
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


def resolve_alternate_camera(component, build, dependencies):
    """A clean synthetic registry revision with a changed mapper layout."""
    (component / camera.SOURCE_PATH).rename(component / "replacement_mapper.c")
    manifest = component / "idf_component.yml"
    data = json.loads(manifest.read_text())
    data["version"] = "2.4.2"
    manifest.write_text(json.dumps(data))
    (build / "compile_commands.json").write_text("[]")
    component_hash = directory_hash(component)
    (component / ".component_hash").write_text(component_hash)
    dependencies["dependencies"][camera.COMPONENT].update(
        version=data["version"], component_hash=component_hash,
    )


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

    def verify_unpatched(self):
        camera.verify_unpatched_component(self.project, self.component, self.dependencies)

    def test_ordinary_integrity_rejects_changed_bytes_markers_and_lock_hashes(self):
        self.verify_unpatched()
        original = copy.deepcopy(self.dependencies)
        marker = self.component / ".component_hash"
        for source_bytes, marker_hash, lock_hash in (
            (PATCHED, camera.COMPONENT_HASH, camera.COMPONENT_HASH),
            (ORIGINAL, "0" * 64, camera.COMPONENT_HASH),
            (ORIGINAL, camera.COMPONENT_HASH, "0" * 64),
            (PATCHED, camera.PATCHED_COMPONENT_HASH, camera.COMPONENT_HASH),
            (ORIGINAL, "0" * 64, "0" * 64),
        ):
            with self.subTest(source=source_bytes, marker=marker_hash, lock=lock_hash):
                self.dependencies = copy.deepcopy(original)
                self.dependencies["dependencies"][camera.COMPONENT]["component_hash"] = lock_hash
                self.source.write_bytes(source_bytes)
                marker.write_text(marker_hash)
                with self.assertRaisesRegex(ValueError, "component bytes"):
                    self.verify_unpatched()

    def test_ordinary_identity_and_path_checks_remain_required(self):
        original = copy.deepcopy(self.dependencies)
        for key, value in (
            ("version", ""), ("version", "2.4.2"), ("component_hash", "../not-a-hash"),
            ("source", {"type": "local", "path": "."}),
            ("source", {"type": "service", "service_url": REGISTRY_SOURCE["registry_url"]}),
        ):
            with self.subTest(key=key, value=value):
                self.dependencies = copy.deepcopy(original)
                self.dependencies["dependencies"][camera.COMPONENT][key] = value
                with self.assertRaises(ValueError):
                    self.verify_unpatched()
        self.dependencies = original
        for bad in (None, {"dependencies": []}, {"dependencies": {}}):
            with self.subTest(lock=bad), self.assertRaises(ValueError):
                camera.verify_unpatched_component(self.project, self.component, bad)
        link = self.component / "alias.c"
        link.symlink_to(self.source)
        with self.assertRaisesRegex(ValueError, "symbolic links"):
            self.verify_unpatched()
        link.unlink()
        self.component.rename(self.project / "external-video")
        self.component.symlink_to(self.project / "external-video")
        with self.assertRaisesRegex(ValueError, "managed esp_video"):
            self.verify_unpatched()

    def assert_cli_refusal(self, code, canary):
        (self.project / "dependencies.lock").write_text(json.dumps(self.dependencies))
        before = {p: p.read_bytes() for p in self.component.rglob("*") if p.is_file()}
        sdk_before = sdk.git(self.idf, "diff")
        sdk_status = sdk.git(self.idf, "status", "--porcelain=v1")
        stdout, stderr = io.StringIO(), io.StringIO()
        arguments = [
            "tab5_camera_compat.py", "--project-path", str(self.project),
            "--component-path", str(self.component), "--build-path", str(self.build),
            "--idf-path", str(self.idf),
        ]
        with patch.object(sys, "argv", arguments), redirect_stdout(stdout), redirect_stderr(stderr):
            with self.assertRaises(SystemExit) as result:
                camera.main()
        self.assertEqual(result.exception.code, 1)
        self.assertEqual(stdout.getvalue(), "")
        self.assertIn(f"guard={code}", stderr.getvalue())
        for value in (canary, str(self.root), "Traceback"):
            self.assertNotIn(value, stdout.getvalue() + stderr.getvalue())
        self.assertEqual(
            {p: p.read_bytes() for p in self.component.rglob("*") if p.is_file()}, before)
        self.assertEqual(sdk.git(self.idf, "diff"), sdk_before)
        self.assertEqual(sdk.git(self.idf, "status", "--porcelain=v1"), sdk_status)

    def test_cli_classifies_real_camera_and_sdk_guards_before_apply(self):
        canary = "synthetic-private-value-must-not-print"
        self.dependencies["dependencies"][camera.COMPONENT]["version"] = canary
        self.assert_cli_refusal("component_identity", canary)
        self.dependencies["dependencies"][camera.COMPONENT]["version"] = camera.VERSION
        config = self.config.read_text()
        self.config.write_text(config.replace('CONFIG_IDF_TARGET="esp32p4"',
                                              f'CONFIG_IDF_TARGET="{canary}"'))
        self.assert_cli_refusal("early_p4_profile", canary)
        self.config.write_text(config)
        for module, name, code in (
            (camera, "MANIFEST_SHA256", "component_manifest"),
            (camera, "SDK_SOURCE_SHA256", "sdk_csi_contract"),
            (sdk, "BASE_COMMIT", "sdk_base"),
            (sdk, "PATCH_SHA256", "sdk_patch_bytes"),
            (sdk, "ORIGINAL_SHA256", "sdk_base_source"),
            (sdk, "PATCHED_SHA256", "sdk_patch_state"),
        ):
            with self.subTest(code=code), patch.object(module, name, canary):
                self.assert_cli_refusal(code, canary)

    def test_cli_unknown_errors_have_a_fixed_nonleaking_fallback(self):
        canary = "synthetic-private-value-must-not-print"
        for error in (
            ValueError(canary),
            ValueError("Build must compile exactly one camera format mapper " + canary),
            OSError(5, canary, str(self.root / canary)),
            subprocess.CalledProcessError(1, [canary], output=canary, stderr=canary),
        ):
            with self.subTest(error=type(error).__name__), patch.object(
                    camera, "apply_component_patch", side_effect=error):
                self.assert_cli_refusal("unclassified", canary)

    def test_exact_application_idempotence_integrity_markers_and_metadata(self):
        tracked = self.root / "tracked.txt"
        tracked.write_text("Original caller-owned file.\n")
        sdk.git(self.root, "add", "tracked.txt")
        sdk.git(self.root, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.com",
                "-c", "commit.gpgsign=false", "commit", "-qm", "Caller-owned file")
        tracked.write_text("Preserve this caller edit.\n")
        root_before = sdk.git(self.root, "diff", "--binary")
        before = {p: p.read_bytes() for p in self.component.rglob("*") if p.is_file()}
        sdk_before = sdk.git(self.idf, "diff")
        expected = self.apply()
        self.assertEqual(self.apply(), expected)
        self.assertEqual(self.source.read_bytes(), PATCHED)
        for path, data in before.items():
            if path != self.source:
                self.assertEqual(path.read_bytes(), data)
        self.assertEqual(sdk.git(self.idf, "diff"), sdk_before)
        self.assertEqual(sdk.git(self.root, "diff", "--binary"), root_before)
        output = write_object(self.build, self.source)
        verified = self.verify()
        self.assertEqual(verified["sdk_base_commit"], self.base)
        self.assertEqual(verified["patched_component_hash"], directory_hash(self.component))
        self.assertEqual(verified["patched_source_sha256"], camera.digest(PATCHED))
        self.assertEqual(verified["compiled_object"]["sha256"], camera.digest(output.read_bytes()))

    def test_standalone_application_preserves_bytes_and_idempotence(self):
        (self.root / ".git").rename(self.root / "saved-git-metadata")
        before = {p: p.read_bytes() for p in self.component.rglob("*") if p.is_file()}
        sdk_before = sdk.git(self.idf, "diff")
        result = self.apply()
        self.assertEqual(self.apply(), result)
        self.assertEqual(self.source.read_bytes(), PATCHED)
        for path, data in before.items():
            if path != self.source:
                self.assertEqual(path.read_bytes(), data)
        self.assertEqual(sdk.git(self.idf, "diff"), sdk_before)

    def test_cli_rejects_invalid_owner_config_instead_of_standalone_fallback(self):
        canary = "synthetic-private-value-must-not-print"
        (self.root / ".git/config").write_text(f"[invalid\n{canary}\n")
        self.assert_cli_refusal("component_worktree", canary)

    def test_cli_rejects_bare_repository_instead_of_standalone_fallback(self):
        (self.root / ".git").rename(self.root / "saved-git-metadata")
        sdk.git(self.root, "init", "--bare", "-q")
        self.assert_cli_refusal("component_worktree", "synthetic-private-value-must-not-print")

    def test_cli_rejects_broken_git_marker_instead_of_standalone_fallback(self):
        (self.root / ".git").rename(self.root / "saved-git-metadata")
        (self.root / ".git").mkdir()
        self.assert_cli_refusal("component_worktree", "synthetic-private-value-must-not-print")

    def test_cli_requires_component_containment_in_discovered_worktree(self):
        outside = self.root / "other-worktree"
        outside.mkdir()
        sdk.git(self.root, "config", "core.worktree", str(outside))
        self.assert_cli_refusal("component_worktree_path", "synthetic-private-value-must-not-print")

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
