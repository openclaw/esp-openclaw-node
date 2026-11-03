#!/usr/bin/env python3
"""Run the SDK salt regression with actual IDF SRP/MPI and mbedTLS sources."""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import idf_srp_salt as guard


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, required=True)
    parser.add_argument("--mbedtls-path", type=Path)
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    srp = args.idf_path / "components/protocomm/src/crypto/srp6a"
    mbedtls = args.mbedtls_path or args.idf_path / "components/mbedtls/mbedtls"
    with tempfile.TemporaryDirectory(prefix="idf-srp-test-") as directory:
        work = Path(directory)
        source = srp / "esp_srp.c"
        source_hash = guard.digest(source.read_bytes())
        if source_hash == guard.ORIGINAL_SHA256:
            # Apply the maintained patch only to a disposable copy, never the cache.
            source = work / guard.SOURCE_PATH
            source.parent.mkdir(parents=True)
            source.write_bytes((srp / "esp_srp.c").read_bytes())
            if guard.digest(guard.PATCH_PATH.read_bytes()) != guard.PATCH_SHA256:
                raise ValueError("Unexpected SRP patch")
            subprocess.run(["patch", "--silent", "-p1", "-d", str(work),
                            "-i", str(guard.PATCH_PATH)], check=True)
        if guard.digest(source.read_bytes()) != guard.PATCHED_SHA256:
            raise ValueError("SRP source does not match the approved patched SDK")
        for name in ("esp_err.h", "esp_log.h", "esp_random.h"):
            (work / name).write_text('#include "srp_sdk.h"\n')
        (work / "srp_config.h").write_text(
            "#define MBEDTLS_BIGNUM_C\n#define MBEDTLS_SHA512_C\n")
        compiler = ["cc", "-std=c11", "-g", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", '-DMBEDTLS_CONFIG_FILE="srp_config.h"',
                    "-I", str(work), "-I", str(tests / "host"),
                    "-I", str(args.idf_path / "components/protocomm/include/crypto/srp6a"),
                    "-I", str(srp), "-I", str(mbedtls / "include"),
                    "-I", str(mbedtls / "library")]
        sources = [source, srp / "esp_srp_mpi.c"] + [
            mbedtls / "library" / f"{name}.c" for name in (
                "bignum", "bignum_core", "bignum_mod", "bignum_mod_raw",
                "constant_time", "platform_util", "sha512")]
        objects = []
        for number, source in enumerate(sources):
            output = work / f"vendor-{number}.o"
            subprocess.run([*compiler, "-include", str(tests / "host/srp_sdk.h"),
                            "-Dmalloc=srp_test_malloc", "-Dcalloc=srp_test_calloc",
                            "-Dfree=srp_test_free", "-c", str(source), "-o", str(output)], check=True)
            objects.append(str(output))
        binary = work / "srp-test"
        subprocess.run([*compiler, str(tests / "test_idf_srp_salt.c"),
                        *objects, "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
