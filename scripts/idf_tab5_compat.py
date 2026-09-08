#!/usr/bin/env python3
"""Apply or verify the single approved Tab5 patch in an explicitly selected SDK."""

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


def digest(data):
    return hashlib.sha256(data).hexdigest()


def git(idf, *arguments):
    return subprocess.check_output(
        ["git", "--no-optional-locks", "-C", str(idf), *arguments],
        stderr=subprocess.PIPE,
    )


def inspect_sdk(idf):
    idf = Path(idf).resolve(strict=True)
    root = Path(git(idf, "rev-parse", "--show-toplevel").decode().strip()).resolve()
    if root != idf:
        raise ValueError("Select the SDK repository root explicitly")
    if git(idf, "rev-parse", "HEAD").decode().strip() != BASE_COMMIT:
        raise ValueError("SDK is not at the approved compatibility-patch base")
    if PATCH_PATH.is_symlink() or digest(PATCH_PATH.read_bytes()) != PATCH_SHA256:
        raise ValueError("Tracked compatibility patch has unexpected contents")
    if digest(git(idf, "show", f"HEAD:{SOURCE_PATH}")) != ORIGINAL_SHA256:
        raise ValueError("SDK base source does not match the approved whole-file hash")
    source = idf / SOURCE_PATH
    if source.resolve(strict=True) != source or not source.is_file():
        raise ValueError("SDK source must be a regular file at the approved path")
    status = git(
        idf, "status", "--porcelain=v1", "-z",
        "--untracked-files=all", "--ignore-submodules=none",
    )
    return idf, status, digest(source.read_bytes())


def verify_sdk_patch(idf):
    _, status, source_hash = inspect_sdk(idf)
    # Exact base, sole dirty path and whole-file bytes prove the actual SDK delta.
    if status != PATCHED_STATUS or source_hash != PATCHED_SHA256:
        raise ValueError("SDK is not modified by exactly the approved Tab5 patch")
    return {
        "base_commit": BASE_COMMIT,
        "patch": PATCH_RELATIVE,
        "patch_sha256": PATCH_SHA256,
        "source": SOURCE_PATH,
        "original_source_sha256": ORIGINAL_SHA256,
        "patched_source_sha256": source_hash,
    }


def apply_sdk_patch(idf):
    idf, status, source_hash = inspect_sdk(idf)
    if status == PATCHED_STATUS and source_hash == PATCHED_SHA256:
        return verify_sdk_patch(idf)
    if status or source_hash != ORIGINAL_SHA256:
        raise ValueError("SDK must be clean or contain exactly the verified Tab5 patch")
    git(idf, "apply", "--check", str(PATCH_PATH))
    git(idf, "apply", str(PATCH_PATH))
    return verify_sdk_patch(idf)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, required=True)
    parser.add_argument("--verify-only", action="store_true")
    arguments = parser.parse_args()
    try:
        operation = verify_sdk_patch if arguments.verify_only else apply_sdk_patch
        result = operation(arguments.idf_path)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"SDK compatibility patch refused: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
