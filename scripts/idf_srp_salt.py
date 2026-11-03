#!/usr/bin/env python3
"""Apply or verify the approved fixed-width SRP salt guard in a selected SDK."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess


BASE_COMMIT = "b774170ff46c393eeb5e495ea37936038d3f4f4f"
SOURCE_PATH = "components/protocomm/src/crypto/srp6a/esp_srp.c"
ORIGINAL_SHA256 = "eca11a0e2e61913465971e43807cb000f08c018b2d46a23a3d0e8c24c6866001"
PATCHED_SHA256 = "d0dea614c9a56800ab9e16f1362e410001cacb6299876169a2167580f8316e3d"
PATCH_PATH = Path(__file__).resolve().parents[1] / "patches/esp-idf/security2-srp-salt-width.patch"
PATCH_SHA256 = "bd1488cabedd84b270c01b04d1ae0e16a67612e8424064ed753123ef90e50a46"
PATCHED_STATUS = b" M " + SOURCE_PATH.encode() + b"\0"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def git(idf, *arguments):
    return subprocess.check_output(
        ["git", "--no-optional-locks", "-C", str(idf), *arguments], stderr=subprocess.PIPE)


def inspect_sdk(idf):
    idf = Path(idf).resolve(strict=True)
    root = Path(git(idf, "rev-parse", "--show-toplevel").decode().strip()).resolve()
    if root != idf or git(idf, "rev-parse", "HEAD").decode().strip() != BASE_COMMIT:
        raise ValueError("Select the approved ESP-IDF v5.5.5 repository root")
    if PATCH_PATH.is_symlink() or digest(PATCH_PATH.read_bytes()) != PATCH_SHA256:
        raise ValueError("Tracked SRP patch has unexpected contents")
    if digest(git(idf, "show", f"HEAD:{SOURCE_PATH}")) != ORIGINAL_SHA256:
        raise ValueError("SDK base source differs from the approved whole-file hash")
    source = idf / SOURCE_PATH
    if source.resolve(strict=True) != source or not source.is_file():
        raise ValueError("SDK source must be a regular file at the approved path")
    status = git(idf, "status", "--porcelain=v1", "-z",
                 "--untracked-files=all", "--ignore-submodules=none")
    return idf, status, digest(source.read_bytes())


def verify_sdk_patch(idf):
    _, status, source_hash = inspect_sdk(idf)
    if status != PATCHED_STATUS or source_hash != PATCHED_SHA256:
        raise ValueError("SDK must contain exactly the approved SRP salt guard")
    return {"base_commit": BASE_COMMIT, "source": SOURCE_PATH,
            "patch_sha256": PATCH_SHA256, "patched_source_sha256": source_hash}


def apply_sdk_patch(idf):
    idf, status, source_hash = inspect_sdk(idf)
    if status == PATCHED_STATUS and source_hash == PATCHED_SHA256:
        return verify_sdk_patch(idf)
    if status or source_hash != ORIGINAL_SHA256:
        raise ValueError("SDK must be clean or contain exactly the approved SRP salt guard")
    git(idf, "apply", "--check", str(PATCH_PATH))
    git(idf, "apply", str(PATCH_PATH))
    return verify_sdk_patch(idf)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, required=True)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    try:
        result = (verify_sdk_patch if args.verify_only else apply_sdk_patch)(args.idf_path)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"SDK SRP salt patch refused: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
