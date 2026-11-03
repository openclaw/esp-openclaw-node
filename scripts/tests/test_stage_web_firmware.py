"""Browser catalog integrity and release lineage tests; no physical device required."""

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location(
    "stage_web_firmware", Path(__file__).resolve().parents[1] / "stage_web_firmware.py")
web = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(web)
COMMIT = "a" * 40


class WebFirmwareTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.input = self.root / "bundles"
        self.output = self.root / "web-flash"
        self.bundles = {}
        for example in web.BOARDS:
            root = self.input / f"firmware-{example}-esp32s3-{COMMIT}-1"
            (root / "images").mkdir(parents=True)
            files = {"0x0": "images/00000000.bin", "0x8000": "images/00008000.bin",
                     "0x10000": "images/00010000.bin", "0x200000": "images/00200000.bin"}
            flash = {
                "flash_settings": {"flash_mode": "dio", "flash_size": "4MB", "flash_freq": "40m"},
                "write_flash_args": ["--flash_mode", "dio", "--flash_size", "4MB", "--flash_freq", "40m"],
                "extra_esptool_args": {"chip": "esp32s3", "before": "default_reset",
                                       "after": "hard_reset", "stub": True},
                "flash_files": files,
            }
            for name, offset in (("bootloader", "0x0"), ("partition-table", "0x8000"), ("app", "0x10000")):
                flash[name] = {"offset": offset, "file": files[offset], "encrypted": "false"}
            for image in files.values():
                (root / image).write_bytes(b"synthetic image" * 37)
            (root / "flasher_args.json").write_text(json.dumps(flash))
            manifest = {"schema_version": 1, "example": example, "target": "esp32s3",
                        "source": {"repository": web.REPOSITORY, "commit": COMMIT},
                        "ci": {"event": "push", "event_sha": COMMIT, "pr_head_sha": "",
                               "run_id": "123", "run_attempt": "1"}}
            self.bundles[example] = (root, flash, manifest)
            self.seal(example)

    def seal(self, example):
        root, flash, manifest = self.bundles[example]
        (root / "flasher_args.json").write_text(json.dumps(flash))
        manifest["files"] = {
            p.relative_to(root).as_posix(): {"sha256": web.digest(p), "size": p.stat().st_size}
            for p in root.rglob("*") if p.is_file() and p.name not in {"manifest.json", "SHA256SUMS"}}
        (root / "manifest.json").write_text(json.dumps(manifest))
        (root / "SHA256SUMS").write_text("".join(
            f"{web.digest(p)}  {p.relative_to(root).as_posix()}\n"
            for p in sorted(root.rglob("*")) if p.is_file() and p.name != "SHA256SUMS"))

    def stage(self, **kwargs):
        return web.stage(self.input, self.output, kwargs.get("version", "v1.3.1"),
                         kwargs.get("commit", COMMIT), "123", "1")

    def test_catalog_preserves_sparse_images_hashes_settings_and_provenance(self):
        result = self.stage()
        self.assertEqual(result["source"]["commit"], COMMIT)
        self.assertEqual(result["version"], "v1.3.1")
        self.assertEqual([b["id"] for b in result["boards"]], list(web.BOARDS))
        for board in result["boards"]:
            self.assertEqual(board["qualification"], "ci-build")
            self.assertEqual(board["flashSize"], "4MB")
            self.assertEqual(board["flashFrequency"], "40m")
            self.assertEqual(len(board["parts"]), 4)
            self.assertIn(COMMIT, board["documentationUrl"])
            for part in board["parts"]:
                self.assertTrue(part["path"].startswith("v1.3.1/"))
                self.assertEqual(web.digest(self.output / part["path"]), part["sha256"])
                self.assertEqual((self.output / part["path"]).stat().st_size, part["size"])
            copied = self.output / "v1.3.1" / board["id"]
            for line in (copied / "SHA256SUMS").read_text().splitlines():
                digest, name = line.split("  ", 1)
                self.assertEqual(web.digest(copied / name), digest)

    def test_rejects_pr_wrong_commit_or_wrong_run_without_partial_output(self):
        for field, value in (("event", "pull_request"), ("pr_head_sha", COMMIT),
                             ("event_sha", "b" * 40), ("run_id", "456"), ("run_attempt", "2")):
            with self.subTest(field=field):
                _, _, manifest = self.bundles["esp32-node"]
                old = manifest["ci"][field]
                manifest["ci"][field] = value
                self.seal("esp32-node")
                with self.assertRaises(ValueError):
                    self.stage()
                self.assertFalse(self.output.exists())
                manifest["ci"][field] = old
                self.seal("esp32-node")

    def test_rejects_corrupt_image_before_any_output(self):
        root, _, _ = self.bundles["m5stack-dial-node"]
        (root / "images/00010000.bin").write_bytes(b"corrupt")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            self.stage()
        self.assertFalse(self.output.exists())

    def test_rejects_unsupported_transport_and_settings(self):
        _, flash, _ = self.bundles["esp32-node"]
        for key, value in (("stub", False), ("before", "no_reset"), ("chip", "esp32")):
            original = flash["extra_esptool_args"][key]
            flash["extra_esptool_args"][key] = value
            self.seal("esp32-node")
            with self.assertRaises(ValueError):
                self.stage()
            flash["extra_esptool_args"][key] = original
        flash["flash_settings"]["flash_size"] = "detect"
        self.seal("esp32-node")
        with self.assertRaises(ValueError):
            self.stage()

    def test_rejects_sector_overlap_and_out_of_range_images(self):
        _, flash, _ = self.bundles["esp32-node"]
        original = copy.deepcopy(flash["flash_files"])
        for offset in ("0x1", "0x400000", "0x1000"):
            flash["flash_files"] = dict(original)
            flash["flash_files"][offset] = "images/00200000.bin"
            if offset == "0x1000":
                root, _, _ = self.bundles["esp32-node"]
                (root / "images/00000000.bin").write_bytes(b"a" * 4097)
            self.seal("esp32-node")
            with self.assertRaises(ValueError):
                self.stage()
        self.assertFalse(self.output.exists())

    def test_rejects_path_escape_and_symlink(self):
        root, _, _ = self.bundles["esp32-node"]
        for name in ("../outside.bin", "/outside.bin", "images\\outside.bin"):
            with self.assertRaises(ValueError):
                web.file_at(root, name)
        outside = self.root / "outside.bin"
        outside.write_bytes(b"external")
        image = root / "images/00010000.bin"
        image.unlink()
        image.symlink_to(outside)
        with self.assertRaises(ValueError):
            self.stage()
        self.assertFalse(self.output.exists())

    def test_stable_version_and_existing_output_are_required(self):
        for version in ("v1.3.1-rc.1", "main", "../v1.3.1"):
            with self.assertRaises(ValueError):
                self.stage(version=version)
        self.output.mkdir()
        with self.assertRaises(ValueError):
            self.stage()


if __name__ == "__main__":
    unittest.main()
