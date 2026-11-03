"""Exercise SDK mutation admission with real Git and the maintained patch."""

from contextlib import ExitStack
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import idf_srp_salt as guard


ORIGINAL = b"""/* Synthetic SDK fragment for patch admission, not crypto proof. */

    esp_mpi_get_rand(hd->s, 8 * salt_len, -1, 0);
    hd->bytes_s = esp_mpi_to_bin(hd->s, &str_salt_len);
    if (!hd->bytes_s) {
        ESP_LOGE(TAG, "Failed to generate salt of len %d", salt_len);
        goto error;
    }
"""
PATCHED = ORIGINAL.replace(
    b"    if (!hd->bytes_s) {",
    b"    /* MPI export omits leading zeros; callers require the requested width. */\n"
    b"    if (!hd->bytes_s || str_salt_len != salt_len) {",
)


class SaltPatchTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="srp-patch-test-")
        self.addCleanup(temporary.cleanup)
        self.idf = Path(temporary.name).resolve()
        contexts = ExitStack()
        self.addCleanup(contexts.close)
        contexts.enter_context(patch.dict(os.environ, {
            "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1"}))
        guard.git(self.idf, "init", "-q")
        self.source = self.idf / guard.SOURCE_PATH
        self.source.parent.mkdir(parents=True)
        self.source.write_bytes(ORIGINAL)
        guard.git(self.idf, "add", ".")
        guard.git(self.idf, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.com",
                  "-c", "commit.gpgsign=false", "commit", "-qm", "SDK fixture")
        base = guard.git(self.idf, "rev-parse", "HEAD").decode().strip()
        contexts.enter_context(patch.multiple(
            guard, BASE_COMMIT=base, ORIGINAL_SHA256=guard.digest(ORIGINAL),
            PATCHED_SHA256=guard.digest(PATCHED)))

    def test_apply_verify_and_repeat_have_one_exact_delta(self):
        with self.assertRaises(ValueError):
            guard.verify_sdk_patch(self.idf)
        metadata = guard.apply_sdk_patch(self.idf)
        self.assertEqual(self.source.read_bytes(), PATCHED)
        self.assertEqual(metadata["patched_source_sha256"], guard.digest(PATCHED))
        self.assertEqual(guard.apply_sdk_patch(self.idf), metadata)
        self.assertEqual(guard.verify_sdk_patch(self.idf), metadata)

    def test_wrong_base_source_or_patch_refuses_without_mutation(self):
        for field in ("BASE_COMMIT", "ORIGINAL_SHA256", "PATCH_SHA256"):
            with self.subTest(field=field), patch.object(guard, field, "0" * 64):
                with self.assertRaises(ValueError):
                    guard.apply_sdk_patch(self.idf)
                self.assertEqual(self.source.read_bytes(), ORIGINAL)

    def test_unrelated_dirt_and_staged_sdk_edits_are_preserved_and_refused(self):
        other = self.idf / "untracked.c"
        other.write_text("unrelated\n")
        with self.assertRaises(ValueError):
            guard.apply_sdk_patch(self.idf)
        self.assertEqual(other.read_text(), "unrelated\n")
        other.unlink()
        guard.apply_sdk_patch(self.idf)
        guard.git(self.idf, "add", guard.SOURCE_PATH)
        with self.assertRaises(ValueError):
            guard.verify_sdk_patch(self.idf)
        self.assertEqual(self.source.read_bytes(), PATCHED)

    def test_changed_or_symlinked_source_is_preserved_and_refused(self):
        self.source.write_bytes(ORIGINAL + b"unexpected\n")
        with self.assertRaises(ValueError):
            guard.apply_sdk_patch(self.idf)
        self.assertTrue(self.source.read_bytes().endswith(b"unexpected\n"))
        target = self.idf / "external.c"
        target.write_bytes(ORIGINAL)
        self.source.unlink()
        self.source.symlink_to(target)
        with self.assertRaises(ValueError):
            guard.apply_sdk_patch(self.idf)
        self.assertEqual(target.read_bytes(), ORIGINAL)


if __name__ == "__main__":
    unittest.main()
