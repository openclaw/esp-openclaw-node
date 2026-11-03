#!/usr/bin/env python3
"""Stage verified CI firmware for the browser installer; never flash a device."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import tempfile


REPOSITORY = "openclaw/esp-openclaw-node"
BOARDS = {
    "esp32-node": ("ESP32-S3 development board", [
        "Use the native USB Serial/JTAG port on an ESP32-S3 board with at least 2 MB flash.",
        "This generic image does not initialize a board-specific screen or peripherals.",
    ]),
    "esp-box-3-display": ("ESP-BOX-3", [
        "Use an ESP-BOX-3 with its 16 MB flash and octal PSRAM.",
    ]),
    "m5stack-dial-node": ("M5Stack Dial", [
        "Use an M5Dial with its 8 MB flash; other M5Stack models need a different image.",
    ]),
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def file_at(root, name):
    if not isinstance(name, str) or not name or "\\" in name:
        raise ValueError("Invalid bundle path")
    relative = Path(name)
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError("Bundle path escapes its directory")
    path = root / relative
    if path.is_symlink() or not path.resolve().is_relative_to(root.resolve()) or not path.is_file():
        raise ValueError("Bundle path is missing or escapes its directory")
    return path


def verified_bundle(root, example, commit, run_id, run_attempt):
    manifest_path = file_at(root, "manifest.json")
    manifest = json.loads(manifest_path.read_text())
    if (manifest.get("schema_version") != 1 or manifest.get("example") != example
            or manifest.get("target") != "esp32s3"
            or manifest.get("source", {}).get("repository") != REPOSITORY
            or manifest.get("source", {}).get("commit") != commit):
        raise ValueError("Firmware identity does not match the selected release")
    ci = manifest.get("ci", {})
    if (ci.get("event") != "push" or ci.get("event_sha") != commit
            or ci.get("pr_head_sha") or str(ci.get("run_id")) != run_id
            or str(ci.get("run_attempt")) != run_attempt):
        raise ValueError("Firmware must come from the selected successful main CI run")
    checksums = {}
    for line in file_at(root, "SHA256SUMS").read_text().splitlines():
        checksum, separator, name = line.partition("  ")
        if not separator or not re.fullmatch(r"[0-9a-f]{64}", checksum) or name in checksums:
            raise ValueError("Invalid bundle checksums")
        if digest(file_at(root, name)) != checksum:
            raise ValueError("Bundle checksum mismatch")
        checksums[name] = checksum
    if checksums.get("manifest.json") != digest(manifest_path):
        raise ValueError("Bundle manifest is not checksummed")
    for name, record in manifest["files"].items():
        path = file_at(root, name)
        if (checksums.get(name) != record["sha256"] or digest(path) != record["sha256"]
                or path.stat().st_size != record["size"]):
            raise ValueError("Bundle provenance does not match its files")
    flash = json.loads(file_at(root, "flasher_args.json").read_text())
    extra = flash["extra_esptool_args"]
    if extra != {"chip": "esp32s3", "before": "default_reset", "after": "hard_reset", "stub": True}:
        raise ValueError("This flash transport is not supported by the web installer")
    settings = flash["flash_settings"]
    if (settings.get("flash_mode") not in {"dio", "qio", "dout", "qout"}
            or settings.get("flash_size") not in {"2MB", "4MB", "8MB", "16MB"}
            or settings.get("flash_freq") not in {"40m", "80m"}):
        raise ValueError("Unsupported web flash settings")
    expected = ["--flash_mode", settings["flash_mode"], "--flash_size", settings["flash_size"],
                "--flash_freq", settings["flash_freq"]]
    if flash["write_flash_args"] != expected:
        raise ValueError("Unsupported web flash arguments")
    for name in ("bootloader", "partition-table", "app"):
        entry = flash[name]
        if (entry.get("encrypted") not in (False, "false")
                or flash["flash_files"].get(entry["offset"]) != entry["file"]):
            raise ValueError("Required flash image is missing or encrypted")
    images = []
    capacity = int(settings["flash_size"].removesuffix("MB")) * 1024 * 1024
    for offset, name in flash["flash_files"].items():
        if not re.fullmatch(r"0x[0-9a-fA-F]+", offset) or not re.fullmatch(r"images/[0-9a-f]{8}\.bin", name):
            raise ValueError("Invalid flash image map")
        record = manifest["files"][name]
        start = int(offset, 16)
        if record["size"] <= 0 or start % 4096 or start + record["size"] > capacity:
            raise ValueError("Flash image exceeds its aligned memory range")
        images.append((start, name, record))
    images.sort()
    if not images:
        raise ValueError("Empty flash image map")
    for previous, current in zip(images, images[1:]):
        # Flash erases whole sectors, even if the previous image ends mid-sector.
        if (previous[0] + previous[2]["size"] + 4095) // 4096 * 4096 > current[0]:
            raise ValueError("Flash image erase regions overlap")
    return settings, images, [*manifest["files"], "manifest.json", "SHA256SUMS"]


def stage(input_root, output, version, commit, run_id, run_attempt):
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("Select a stable release tag")
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("Select the exact release commit")
    if not re.fullmatch(r"[1-9][0-9]*", run_id) or not re.fullmatch(r"[1-9][0-9]*", run_attempt):
        raise ValueError("Select the exact CI run and attempt")
    if output.exists():
        raise ValueError("Output directory already exists")
    catalog = {"schemaVersion": 1, "version": version,
               "source": {"repository": REPOSITORY, "commit": commit}, "boards": []}
    verified = []
    for example, (name, requirements) in BOARDS.items():
        bundle = input_root / f"firmware-{example}-esp32s3-{commit}-{run_attempt}"
        settings, images, files = verified_bundle(bundle, example, commit, run_id, run_attempt)
        prefix = f"{version}/{example}"
        catalog["boards"].append({
            "id": example, "name": name, "chipFamily": "ESP32-S3",
            "flashSize": settings["flash_size"], "flashMode": settings["flash_mode"],
            "flashFrequency": settings["flash_freq"], "qualification": "ci-build",
            "documentationUrl": f"https://github.com/{REPOSITORY}/blob/{commit}/examples/{example}/README.md",
            "requirements": requirements,
            "parts": [{"path": f"{prefix}/{image}", "offset": offset, **record}
                      for offset, image, record in images],
        })
        verified.append((bundle, prefix, files))
    output.parent.mkdir(parents=True, exist_ok=True)
    # Validation finishes before staging, so failure cannot publish a partial catalog.
    with tempfile.TemporaryDirectory(prefix=".web-flash-", dir=output.parent) as temporary:
        staged = Path(temporary) / "web-flash"
        staged.mkdir()
        for bundle, prefix, files in verified:
            destination = staged / prefix
            for name in files:
                target = destination / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(file_at(bundle, name), target)
        (staged / "manifest.json").write_text(json.dumps(catalog, indent=2) + "\n")
        staged.rename(output)
    return catalog


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    for name in ("version", "commit", "run-id", "run-attempt"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    try:
        catalog = stage(args.input, args.output, args.version, args.commit, args.run_id, args.run_attempt)
    except (ValueError, KeyError, TypeError, OSError) as error:
        parser.exit(1, f"Web firmware staging failed: {error}\n")
    print(f"Staged {catalog['version']} for {len(catalog['boards'])} boards")


if __name__ == "__main__":
    main()
