"""Compile the actual configured streaming allocator, not a transport simulation."""

import hashlib
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import unittest


FIXTURES = Path(__file__).parent / "fixtures"
SOURCE_HASHES = {
    "5d3772ff65d6aeb837019adc4f03513ed766e045101d1b225cb80d17d66cc986",
    "60d7a83b15ca883539744da370aa0267060ab605a822ec50a9c47b67d94dc78e",
}


class StreamingRxAllocatorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        component = Path(os.environ.get(
            "ESP_HOSTED_COMPONENT_DIR", "managed_components/espressif__esp_hosted"))
        source = component / "host/drivers/transport/sdio/sdio_drv.c"
        if not source.is_file():
            raise unittest.SkipTest("requires the configured esp_hosted source")
        data = source.read_bytes()
        if hashlib.sha256(data).hexdigest() not in SOURCE_HASHES:
            raise AssertionError("allocator input must match the pinned diagnostic or repaired source")
        section = data.decode().split("#else // H_SDIO_HOST_STREAMING_MODE\n", 1)[1]
        section = section.split("// this frees the buffer", 1)[0]
        wrapper = (component / "host/port/include/os_wrapper.h").read_text()
        macro = "#define MEM_ALLOC" + wrapper.split("#define MEM_ALLOC", 1)[1].split("#define FREE", 1)[0]
        temporary = tempfile.TemporaryDirectory(prefix="sdio-rx-allocator-")
        cls.addClassCleanup(temporary.cleanup)
        cls.root = Path(temporary.name)
        (cls.root / "sdio_rx_under_test.inc").write_text(macro + section)
        for name, psram, dma in (("supported", 1, 1), ("no-psram", 0, 1), ("no-dma", 1, 0)):
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=gnu11", "-D_POSIX_C_SOURCE=200809L",
                "-Wall", "-Wextra", "-Werror", "-I", str(cls.root),
                f"-DCONFIG_SPIRAM={psram}", f"-DSOC_SDMMC_PSRAM_DMA_CAPABLE={dma}",
                str(FIXTURES / "test_sdio_rx_allocator.c"), "-o", str(cls.root / name),
            ], check=True)

    def run_case(self, binary, scenario):
        return subprocess.run([str(self.root / binary), scenario],
                              text=True, capture_output=True)

    def test_observed_growth_reuse_and_reader_ownership(self):
        result = self.run_case("supported", "growth")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_block_and_dma_alignment(self):
        result = self.run_case("supported", "alignment")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_psram_failure_keeps_assertion_and_dma_sample_meaning(self):
        result = self.run_case("supported", "failure")
        self.assertEqual(result.returncode, -signal.SIGABRT)
        self.assertEqual(result.stdout.count("alloc "), 1)
        self.assertIn("alloc caps=6 size=6144 alignment=64", result.stdout)
        self.assertIn("dma_free=10915 dma_largest=3072", result.stdout)

    def test_unsupported_profiles_retain_dma_allocation_and_assertion(self):
        for binary in ("no-psram", "no-dma"):
            with self.subTest(binary=binary):
                result = self.run_case(binary, "legacy")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = self.run_case(binary, "growth")
                self.assertEqual(result.returncode, -signal.SIGABRT)
                self.assertIn("alloc caps=1 size=6144 alignment=64", result.stdout)
                self.assertNotIn("alloc caps=6", result.stdout)


if __name__ == "__main__":
    unittest.main()
