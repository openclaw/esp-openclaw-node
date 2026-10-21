#!/usr/bin/env python3
"""Check the public trust anchors in an ESP-IDF generated firmware bundle."""

import argparse
import hashlib
from pathlib import Path
import struct


# The SDK stores only DER subject + SubjectPublicKeyInfo, not full certificates.
# These hashes identify the two removed roots in that compact representation.
REMOVED_ANCHORS = {
    "4216f920b2a81ec8f89bccd93195b5cc05a6a121e269310bff0a37722f13c553": "AAA Certificate Services",
    "98ad944fa2a5bb2dddb76050690d9acffa4af0a9684fab3c1acc3e5e9ce58425": "emSign ECC Root CA - G3",
}


def check_bundle(data):
    if len(data) < 4:
        raise ValueError("Empty or invalid certificate bundle")
    first_offset, = struct.unpack_from("<I", data)
    if first_offset < 4 or first_offset % 4 or first_offset >= len(data):
        raise ValueError("Invalid certificate bundle offset table")
    offsets = struct.unpack_from(f"<{first_offset // 4}I", data)
    for offset, end in zip(offsets, (*offsets[1:], len(data))):
        if offset < first_offset or offset + 4 > end or end > len(data):
            raise ValueError("Invalid certificate bundle entry")
        subject_length, key_length = struct.unpack_from("<HH", data, offset)
        if offset + 4 + subject_length + key_length != end:
            raise ValueError("Invalid certificate bundle entry lengths")
        anchor = hashlib.sha256(data[offset + 4:end]).hexdigest()
        if anchor in REMOVED_ANCHORS:
            raise ValueError(f"Removed public trust anchor still present: {REMOVED_ANCHORS[anchor]}")
    return len(offsets)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        count = check_bundle(arguments.bundle.read_bytes())
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f"Certificate bundle check failed: {error}\n")
    print(f"Checked {count} public trust anchors; removed incident roots are absent")


if __name__ == "__main__":
    main()
