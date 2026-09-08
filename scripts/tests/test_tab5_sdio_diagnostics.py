"""Exercise the component patch boundary using the actual IDF hash API."""

from contextlib import contextmanager
import copy
import difflib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import tab5_sdio_diagnostics as sdio

HAS_MANAGER = importlib.util.find_spec("idf_component_tools") is not None
ORIGINAL = b"/* Synthetic component fixture, not a transport simulator. */\nint fixture;\n"
PATCHED = ORIGINAL + b"/* Diagnostic fixture change. */\n"


def directory_hash(component):
    from idf_component_tools.hash_tools.calculate import hash_dir
    from idf_component_tools.manager import ManifestManager
    manifest = ManifestManager(component, "test").load()
    return hash_dir(
        component, use_gitignore=manifest.use_gitignore,
        include=manifest.include_set,
        exclude=set(manifest.exclude_set) | {"**/.component_hash", "**/CHECKSUMS.json"},
        exclude_default=False,
    )


@contextmanager
def component_fixture(project, build, dependencies):
    component = project / "managed_components/espressif__esp_hosted"
    source = component / sdio.SOURCE_PATH
    source.parent.mkdir(parents=True)
    source.write_bytes(ORIGINAL)
    (component / "other.c").write_text("/* Unrelated component source. */\n")
    manifest = component / "idf_component.yml"
    manifest.write_text(json.dumps({
        "version": sdio.VERSION,
        "repository": "git://github.com/espressif/esp-hosted-mcu.git",
        "repository_info": {"commit_sha": sdio.REVISION, "path": "."},
        "dependencies": {"idf": ">=5.3"},
    }))
    original_hash = directory_hash(component)
    (component / ".component_hash").write_text(original_hash)
    source.write_bytes(PATCHED)
    patched_hash = directory_hash(component)
    source.write_bytes(ORIGINAL)
    patch_file = build / "fixture.patch"
    patch_file.write_text("".join(difflib.unified_diff(
        ORIGINAL.decode().splitlines(keepends=True),
        PATCHED.decode().splitlines(keepends=True),
        fromfile="a/" + sdio.SOURCE_PATH, tofile="b/" + sdio.SOURCE_PATH,
    )))
    (build / "compile_commands.json").write_text(json.dumps([{
        "directory": str(build), "file": str(source), "command": "cc -c fixture.c",
    }]))
    dependencies["target"] = "esp32p4"
    dependencies.setdefault("dependencies", {})[sdio.COMPONENT] = {
        "version": sdio.VERSION, "component_hash": original_hash,
        "source": {"type": "service", "service_url": "https://components.espressif.com/"},
    }
    with patch.multiple(
        sdio, COMPONENT_HASH=original_hash, PATCHED_COMPONENT_HASH=patched_hash,
        MANIFEST_SHA256=sdio.digest(manifest.read_bytes()),
        ORIGINAL_SHA256=sdio.digest(ORIGINAL), PATCHED_SHA256=sdio.digest(PATCHED),
        PATCH_PATH=patch_file, PATCH_SHA256=sdio.digest(patch_file.read_bytes()),
    ):
        yield component


@unittest.skipUnless(HAS_MANAGER, "requires the real IDF component-manager environment")
class SdioPatchTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="sdio-patch-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.project = self.root / "examples/m5stack-tab5-room-node"
        self.build = self.project / "build"
        self.build.mkdir(parents=True)
        # Exercise git apply inside a real repository, as in CI's managed directory.
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        self.dependencies = {}
        context = component_fixture(self.project, self.build, self.dependencies)
        self.component = context.__enter__()
        self.addCleanup(context.__exit__, None, None, None)
        self.source = self.component / sdio.SOURCE_PATH

    def apply(self):
        return sdio.apply_component_patch(
            self.project, self.component, self.build, self.dependencies,
        )

    def verify(self, patched=True):
        return sdio.verify_component_patch(
            self.project, self.component, self.build, self.dependencies, patched=patched,
        )

    def test_apply_idempotence_and_metadata_preserve_unrelated_bytes(self):
        before = {p: p.read_bytes() for p in self.component.rglob("*") if p.is_file()}
        metadata = self.apply()
        self.assertEqual(self.source.read_bytes(), PATCHED)
        self.assertTrue(metadata["modified"])
        self.assertEqual(metadata["patched_source_sha256"], sdio.digest(PATCHED))
        self.assertEqual(metadata["patched_component_hash"], directory_hash(self.component))
        self.assertEqual(metadata["patch_sha256"], sdio.digest(sdio.PATCH_PATH.read_bytes()))
        self.assertEqual(self.apply(), metadata)
        self.assertEqual(self.verify(), metadata)
        for path, data in before.items():
            if path != self.source:
                self.assertEqual(path.read_bytes(), data)

    def test_verify_never_applies_and_unpatched_mode_rejects_patch(self):
        with self.assertRaises(ValueError):
            self.verify()
        self.assertEqual(self.source.read_bytes(), ORIGINAL)
        self.assertFalse(self.verify(patched=False)["modified"])
        self.apply()
        with self.assertRaises(ValueError):
            self.verify(patched=False)

    def test_wrong_or_malformed_lock_refuses_before_mutation(self):
        original = copy.deepcopy(self.dependencies)
        cases = (
            [],
            {"target": "esp32p4", "dependencies": []},
            {**original, "target": "esp32s3"},
        )
        for value in cases:
            with self.subTest(value=value):
                self.dependencies = value
                with self.assertRaises(ValueError):
                    self.apply()
        for key, value in (("version", "2.6.0"), ("component_hash", "0" * 64),
                           ("source", {"type": "git"}), ("source", [])):
            self.dependencies = copy.deepcopy(original)
            self.dependencies["dependencies"][sdio.COMPONENT][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                self.apply()
        self.assertEqual(self.source.read_bytes(), ORIGINAL)

    def test_unrelated_changes_and_extra_source_are_rejected(self):
        other = self.component / "other.c"
        other.write_text("/* Unexpected edit. */\n")
        with self.assertRaises(ValueError):
            self.apply()
        self.assertEqual(self.source.read_bytes(), ORIGINAL)
        other.write_text("/* Unrelated component source. */\n")
        extra = self.component / "extra.c"
        extra.write_text("/* Unexpected file. */\n")
        with self.assertRaises(ValueError):
            self.apply()
        self.assertTrue(extra.exists())

    def test_patch_manifest_and_source_tampering_are_rejected(self):
        for name in ("PATCH_SHA256", "MANIFEST_SHA256", "ORIGINAL_SHA256"):
            with self.subTest(name=name), patch.object(sdio, name, "0" * 64):
                with self.assertRaises(ValueError):
                    self.apply()
                self.assertEqual(self.source.read_bytes(), ORIGINAL)
        self.apply()
        self.source.write_bytes(PATCHED + b"/* unrelated */\n")
        with self.assertRaises(ValueError):
            self.apply()
        with self.assertRaises(ValueError):
            self.verify()

    def test_hash_marker_alone_cannot_authorize_changed_bytes(self):
        self.source.write_bytes(PATCHED)
        (self.component / "other.c").write_text("/* hidden modification */\n")
        with self.assertRaises(ValueError):
            self.verify()
        (self.component / ".component_hash").write_text(sdio.PATCHED_COMPONENT_HASH)
        with self.assertRaises(ValueError):
            self.verify()

    def test_symbolic_links_and_wrong_component_root_are_rejected(self):
        extra = self.component / "link.c"
        extra.symlink_to(self.source)
        with self.assertRaises(ValueError):
            self.apply()
        extra.unlink()
        with self.assertRaises(ValueError):
            sdio.apply_component_patch(
                self.project, self.component.parent, self.build, self.dependencies,
            )
        self.assertEqual(self.source.read_bytes(), ORIGINAL)

    def test_compile_input_must_select_only_the_verified_source(self):
        commands = self.build / "compile_commands.json"
        original = json.loads(commands.read_text())
        wrong = self.root / "sdio_drv.c"
        wrong.write_bytes(ORIGINAL)
        for entries in ([], original * 2, [{**original[0], "file": str(wrong)}]):
            commands.write_text(json.dumps(entries))
            with self.subTest(entries=len(entries)), self.assertRaises(ValueError):
                self.apply()
            self.assertEqual(self.source.read_bytes(), ORIGINAL)


if __name__ == "__main__":
    unittest.main()
