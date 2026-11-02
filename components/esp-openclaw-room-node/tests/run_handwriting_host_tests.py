#!/usr/bin/env python3
"""Exercise the production character classifier with independent synthetic strokes."""

import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    component = Path(__file__).resolve().parents[1]
    model = component / "assets/handwriting_english36.bin"
    metrics = json.loads((component / "assets/handwriting_english36/metrics.json").read_text())
    data = model.read_bytes()
    if len(data) != 106288 or hashlib.sha256(data).hexdigest() != metrics["model_sha256"]:
        raise SystemExit("handwriting model does not match its recorded size/hash")
    with tempfile.TemporaryDirectory(prefix="room-handwriting-host-") as directory:
        binary = Path(directory) / "handwriting-test"
        subprocess.run([
            "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-I", str(component), str(component / "tests/test_room_handwriting.c"),
            str(component / "room_handwriting.c"), "-lm", "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary), str(model)], check=True)


if __name__ == "__main__":
    main()
