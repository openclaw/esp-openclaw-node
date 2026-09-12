"""Exercise SDK patch refusal and idempotence using task-owned Git fixtures."""

from contextlib import ExitStack, contextmanager
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import idf_tab5_compat as compat


ORIGINAL = b"""/* Synthetic SDK fixture, not a copy of the complete SDK source. */
static inline bool esp_task_stack_is_sane_cache_disabled(void)
{
    const void *sp = (const void *)esp_cpu_get_sp();

    return esp_ptr_in_dram(sp)
#if CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP
           || esp_ptr_in_rtc_dram_fast(sp)
#endif
           ;
}
"""
PATCHED = ORIGINAL.replace(
    b"           ;",
    b"#if SOC_MEM_SPM_SUPPORTED\n           || esp_ptr_in_spm(sp)\n#endif\n           ;",
)


def seed_sdk(idf):
    source = idf / compat.SOURCE_PATH
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_bytes(ORIGINAL)
    diagnostic = idf / compat.DIAGNOSTIC_SOURCE_PATH
    diagnostic.parent.mkdir(parents=True, exist_ok=True)
    diagnostic.write_bytes((Path(__file__).parent / "fixtures/nvs_partition.cpp").read_bytes())
    (idf / "other.c").write_text("/* unchanged SDK fixture */\n")
    compat.git(idf, "add", ".")
    compat.git(
        idf, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.com",
        "-c", "commit.gpgsign=false", "commit", "-qm", "SDK fixture",
    )
    return compat.git(idf, "rev-parse", "HEAD").decode().strip()


@contextmanager
def fixture_profile(base):
    # Use real Git state and the real tracked patch with a small synthetic base.
    with patch.multiple(
        compat, BASE_COMMIT=base,
        ORIGINAL_SHA256=compat.digest(ORIGINAL),
        PATCHED_SHA256=compat.digest(PATCHED),
    ):
        yield


class SdkCompatibilityTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="sdk-compat-test-")
        self.addCleanup(temporary.cleanup)
        self.idf = Path(temporary.name).resolve()
        contexts = ExitStack()
        self.addCleanup(contexts.close)
        contexts.enter_context(patch.dict(os.environ, {
            "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1",
        }))
        compat.git(self.idf, "init", "-q")
        self.base = seed_sdk(self.idf)
        contexts.enter_context(fixture_profile(self.base))
        self.source = self.idf / compat.SOURCE_PATH

    def test_apply_and_repeat_preserve_exact_delta_and_report_actual_hashes(self):
        metadata = compat.apply_sdk_patch(self.idf)
        self.assertEqual(self.source.read_bytes(), PATCHED)
        self.assertEqual(compat.git(self.idf, "status", "--porcelain=v1", "-z"), compat.PATCHED_STATUS)
        self.assertEqual(metadata["compatibility_patch"]["base_commit"], self.base)
        self.assertEqual(metadata["compatibility_patch"]["patch_sha256"], compat.digest(compat.PATCH_PATH.read_bytes()))
        self.assertEqual(metadata["compatibility_patch"]["patched_source_sha256"], compat.digest(self.source.read_bytes()))
        self.assertEqual(compat.apply_sdk_patch(self.idf), metadata)
        self.assertEqual(compat.verify_sdk_patch(self.idf), metadata)
        self.assertEqual(self.source.read_bytes(), PATCHED)

    def test_verify_does_not_apply_to_clean_sdk(self):
        with self.assertRaises(ValueError):
            compat.verify_sdk_patch(self.idf)
        self.assertEqual(self.source.read_bytes(), ORIGINAL)

    def test_wrong_base_is_rejected_before_mutation(self):
        with patch.object(compat, "BASE_COMMIT", "0" * 40), self.assertRaises(ValueError):
            compat.apply_sdk_patch(self.idf)
        self.assertEqual(self.source.read_bytes(), ORIGINAL)

    def test_unrelated_tracked_dirt_is_preserved_and_rejected(self):
        other = self.idf / "other.c"
        other.write_text("unrelated edit\n")
        with self.assertRaises(ValueError):
            compat.apply_sdk_patch(self.idf)
        self.assertEqual(self.source.read_bytes(), ORIGINAL)
        self.assertEqual(other.read_text(), "unrelated edit\n")

    def test_untracked_file_is_preserved_and_rejected(self):
        extra = self.idf / "extra.c"
        extra.write_text("unrelated file\n")
        with self.assertRaises(ValueError):
            compat.apply_sdk_patch(self.idf)
        self.assertEqual(self.source.read_bytes(), ORIGINAL)
        self.assertTrue(extra.exists())

    def test_patch_tampering_is_rejected_before_mutation(self):
        with tempfile.TemporaryDirectory(prefix="patch-tamper-") as root:
            altered = Path(root) / "altered.patch"
            altered.write_bytes(compat.PATCH_PATH.read_bytes() + b"\n")
            with patch.object(compat, "PATCH_PATH", altered), self.assertRaises(ValueError):
                compat.apply_sdk_patch(self.idf)
        self.assertEqual(self.source.read_bytes(), ORIGINAL)

    def test_unexpected_original_whole_file_is_rejected(self):
        with patch.object(compat, "ORIGINAL_SHA256", "0" * 64), self.assertRaises(ValueError):
            compat.apply_sdk_patch(self.idf)
        self.assertEqual(self.source.read_bytes(), ORIGINAL)

    def test_patched_source_tampering_is_not_idempotent_success(self):
        compat.apply_sdk_patch(self.idf)
        self.source.write_bytes(PATCHED + b"/* unrelated change */\n")
        for operation in (compat.apply_sdk_patch, compat.verify_sdk_patch):
            with self.subTest(operation=operation.__name__), self.assertRaises(ValueError):
                operation(self.idf)
        self.assertEqual(self.source.read_bytes(), PATCHED + b"/* unrelated change */\n")

    def test_staged_patch_is_not_the_expected_worktree_delta(self):
        compat.apply_sdk_patch(self.idf)
        compat.git(self.idf, "add", compat.SOURCE_PATH)
        with self.assertRaises(ValueError):
            compat.verify_sdk_patch(self.idf)

    def test_nested_sdk_path_is_rejected(self):
        with self.assertRaises(ValueError):
            compat.apply_sdk_patch(self.source.parent)
        self.assertEqual(self.source.read_bytes(), ORIGINAL)

    def test_diagnostics_apply_from_clean_or_compatibility_only_and_are_idempotent(self):
        compat.apply_sdk_patch(self.idf)
        metadata = compat.apply_sdk_patch(self.idf, nvs_diagnostics=True)
        self.assertEqual(compat.git(self.idf, "status", "--porcelain=v1", "-z"), compat.DIAGNOSTIC_STATUS)
        self.assertEqual(metadata["diagnostic_patch"]["patched_source_sha256"],
                         compat.digest((self.idf / compat.DIAGNOSTIC_SOURCE_PATH).read_bytes()))
        self.assertEqual(compat.apply_sdk_patch(self.idf, nvs_diagnostics=True), metadata)
        self.assertEqual(compat.verify_sdk_patch(self.idf, nvs_diagnostics=True), metadata)
        with self.assertRaises(ValueError):
            compat.verify_sdk_patch(self.idf)

    def test_diagnostics_reject_wrong_base_dirt_and_tampering_before_mutation(self):
        source = self.idf / compat.DIAGNOSTIC_SOURCE_PATH
        original = source.read_bytes()
        with patch.object(compat, "BASE_COMMIT", "0" * 40), self.assertRaises(ValueError):
            compat.apply_sdk_patch(self.idf, nvs_diagnostics=True)
        with patch.object(compat, "DIAGNOSTIC_PATCH_SHA256", "0" * 64), self.assertRaises(ValueError):
            compat.apply_sdk_patch(self.idf, nvs_diagnostics=True)
        source.write_bytes(original + b"\n")
        with self.assertRaises(ValueError):
            compat.apply_sdk_patch(self.idf, nvs_diagnostics=True)
        self.assertEqual(self.source.read_bytes(), ORIGINAL)
        source.write_bytes(original)
        metadata = compat.apply_sdk_patch(self.idf, nvs_diagnostics=True)
        self.assertEqual(metadata["diagnostic_patch"]["base_commit"], self.base)
        source.write_bytes(source.read_bytes() + b"\n")
        for operation in (compat.apply_sdk_patch, compat.verify_sdk_patch):
            with self.subTest(operation=operation.__name__), self.assertRaises(ValueError):
                operation(self.idf, nvs_diagnostics=True)
        (self.idf / "other.c").write_text("unrelated\n")
        with self.assertRaises(ValueError):
            compat.verify_sdk_patch(self.idf, nvs_diagnostics=True)


if __name__ == "__main__":
    unittest.main()
