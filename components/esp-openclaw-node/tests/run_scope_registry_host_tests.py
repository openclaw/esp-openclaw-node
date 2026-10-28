#!/usr/bin/env python3
"""Exercise production scope removal and handshake serialization with sanitizers."""

import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cjson-dir", type=Path, required=True)
    args = parser.parse_args()
    component = Path(__file__).resolve().parents[1]
    source = (component / "src/esp_openclaw_node_registry.c").read_text()
    selections = [
        ("void esp_openclaw_node_add_registered_string_array(", "void esp_openclaw_node_add_registered_command_array("),
        ("esp_err_t esp_openclaw_node_unregister_scope_internal(", "esp_err_t esp_openclaw_node_register_command_internal("),
    ]
    with tempfile.TemporaryDirectory(prefix="scope-registry-test-") as directory:
        work = Path(directory)
        (work / "production.inc").write_text("\n".join(
            source[source.index(start):source.index(end)] for start, end in selections
        ))
        binary = work / "scope-registry-test"
        subprocess.run([
            "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-g", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-I", str(work), "-I", str(args.cjson_dir),
            str(component / "tests/test_scope_registry.c"), str(args.cjson_dir / "cJSON.c"),
            "-lm", "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
