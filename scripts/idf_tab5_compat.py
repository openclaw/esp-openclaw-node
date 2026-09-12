#!/usr/bin/env python3
"""Apply or verify the approved Tab5 patches in an explicitly selected SDK."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess


BASE_COMMIT = "362a1776ec212788fda95f75b733bfdde3a0c394"
SOURCE_PATH = "components/spi_flash/cache_utils.c"
ORIGINAL_SHA256 = "15bb5f74f4deb8dbe24ba18eadf90e417592f3d2e40988c8bf0c093144e500fe"
PATCHED_SHA256 = "4e8074e6d6320a86451b9ae4af8bfb50584a2ad97dc0e1d29ff171d83ead33b8"
PATCH_RELATIVE = "patches/esp-idf/tab5-spm-stack-sanity.patch"
PATCH_PATH = Path(__file__).resolve().parents[1] / PATCH_RELATIVE
PATCH_SHA256 = "9f2b51789034460f6b74ca3b0a20934512837d8c126fb3fc8286aaaac5dfa8fe"
PATCHED_STATUS = b" M " + SOURCE_PATH.encode() + b"\0"
DIAGNOSTIC_SOURCE_PATH = "components/nvs_flash/src/nvs_partition.cpp"
DIAGNOSTIC_ORIGINAL_SHA256 = "84ebf8ce5f76de96e839f3d43b1b5a75704617c9050598ee1c10f19ed45a42f3"
DIAGNOSTIC_PATCHED_SHA256 = "e2dc534f82d1b0f47d8b7236f697b075ea034e85b55ef0d09b08297a00eca5ee"
DIAGNOSTIC_PATCH_RELATIVE = "patches/esp-idf/tab5-nvs-first-failure.patch"
DIAGNOSTIC_PATCH_PATH = Path(__file__).resolve().parents[1] / DIAGNOSTIC_PATCH_RELATIVE
DIAGNOSTIC_PATCH_SHA256 = "a921a244bd79a5a6a29a0b184787e95d08a3fc4e6e71fb73f4dd5b92e9d94968"
DIAGNOSTIC_STATUS = b" M " + DIAGNOSTIC_SOURCE_PATH.encode() + b"\0" + PATCHED_STATUS


def digest(data):
    return hashlib.sha256(data).hexdigest()


def git(idf, *arguments):
    return subprocess.check_output(
        ["git", "--no-optional-locks", "-C", str(idf), *arguments],
        stderr=subprocess.PIPE,
    )


def inspect_source(idf, source_path, original_hash, patch_path, patch_hash):
    if patch_path.is_symlink() or digest(patch_path.read_bytes()) != patch_hash:
        raise ValueError("Tracked SDK patch has unexpected contents")
    if digest(git(idf, "show", f"HEAD:{source_path}")) != original_hash:
        raise ValueError("SDK base source does not match the approved whole-file hash")
    source = idf / source_path
    if source.resolve(strict=True) != source or not source.is_file():
        raise ValueError("SDK source must be a regular file at the approved path")
    return digest(source.read_bytes())


def inspect_sdk(idf, nvs_diagnostics=False):
    idf = Path(idf).resolve(strict=True)
    root = Path(git(idf, "rev-parse", "--show-toplevel").decode().strip()).resolve()
    if root != idf:
        raise ValueError("Select the SDK repository root explicitly")
    if git(idf, "rev-parse", "HEAD").decode().strip() != BASE_COMMIT:
        raise ValueError("SDK is not at the approved compatibility-patch base")
    source_hash = inspect_source(idf, SOURCE_PATH, ORIGINAL_SHA256, PATCH_PATH, PATCH_SHA256)
    diagnostic_hash = None
    if nvs_diagnostics:
        diagnostic_hash = inspect_source(
            idf, DIAGNOSTIC_SOURCE_PATH, DIAGNOSTIC_ORIGINAL_SHA256,
            DIAGNOSTIC_PATCH_PATH, DIAGNOSTIC_PATCH_SHA256,
        )
    status = git(
        idf, "status", "--porcelain=v1", "-z",
        "--untracked-files=all", "--ignore-submodules=none",
    )
    return idf, status, source_hash, diagnostic_hash


def verify_sdk_patch(idf, nvs_diagnostics=False):
    _, status, source_hash, diagnostic_hash = inspect_sdk(idf, nvs_diagnostics)
    expected_status = DIAGNOSTIC_STATUS if nvs_diagnostics else PATCHED_STATUS
    if (status != expected_status or source_hash != PATCHED_SHA256 or
            (nvs_diagnostics and diagnostic_hash != DIAGNOSTIC_PATCHED_SHA256)):
        raise ValueError("SDK is not modified by exactly the selected Tab5 patches")
    result = {"compatibility_patch": {
        "base_commit": BASE_COMMIT,
        "patch": PATCH_RELATIVE,
        "patch_sha256": PATCH_SHA256,
        "source": SOURCE_PATH,
        "original_source_sha256": ORIGINAL_SHA256,
        "patched_source_sha256": source_hash,
    }}
    if nvs_diagnostics:
        result["diagnostic_patch"] = {
            "base_commit": BASE_COMMIT,
            "patch": DIAGNOSTIC_PATCH_RELATIVE,
            "patch_sha256": DIAGNOSTIC_PATCH_SHA256,
            "source": DIAGNOSTIC_SOURCE_PATH,
            "original_source_sha256": DIAGNOSTIC_ORIGINAL_SHA256,
            "patched_source_sha256": diagnostic_hash,
        }
    return result


def apply_sdk_patch(idf, nvs_diagnostics=False):
    idf, status, source_hash, diagnostic_hash = inspect_sdk(idf, nvs_diagnostics)
    expected_status = DIAGNOSTIC_STATUS if nvs_diagnostics else PATCHED_STATUS
    if status == expected_status:
        return verify_sdk_patch(idf, nvs_diagnostics)
    patches = [str(PATCH_PATH)]
    if nvs_diagnostics:
        if diagnostic_hash != DIAGNOSTIC_ORIGINAL_SHA256:
            raise ValueError("Diagnostic source must match the approved original")
        patches.append(str(DIAGNOSTIC_PATCH_PATH))
        if status == PATCHED_STATUS and source_hash == PATCHED_SHA256:
            patches = [str(DIAGNOSTIC_PATCH_PATH)]
        elif status or source_hash != ORIGINAL_SHA256:
            raise ValueError("SDK must be clean or contain the exact compatibility patch")
    elif status or source_hash != ORIGINAL_SHA256:
        raise ValueError("SDK must be clean or contain exactly the verified Tab5 patch")
    git(idf, "apply", "--check", *patches)
    git(idf, "apply", *patches)
    return verify_sdk_patch(idf, nvs_diagnostics)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, required=True)
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--nvs-diagnostics", action="store_true")
    arguments = parser.parse_args()
    try:
        operation = verify_sdk_patch if arguments.verify_only else apply_sdk_patch
        result = operation(arguments.idf_path, arguments.nvs_diagnostics)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"SDK compatibility patch refused: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
