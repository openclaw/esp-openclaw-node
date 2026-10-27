#!/usr/bin/env python3
"""Validate handshake parsing with a constrained heap and sanitizers."""

import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cjson-dir", type=Path, required=True)
    args = parser.parse_args()
    component = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="handshake-test-") as directory:
        work = Path(directory)
        cjson = work / "cjson.o"
        executable = work / "handshake-test"
        subprocess.run([
            "cc", "-std=c11", "-g", "-fsanitize=address,undefined",
            "-Wno-deprecated-declarations", "-c", str(args.cjson_dir / "cJSON.c"),
            "-o", str(cjson),
        ], check=True)
        subprocess.run([
            "cc", "-std=c11", "-g", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-I", str(component / "private_include"), "-I", str(args.cjson_dir),
            str(component / "src/esp_openclaw_node_handshake.c"),
            str(component / "tests/test_handshake.c"), str(cjson), "-lm", "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
