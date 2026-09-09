#!/usr/bin/env python3
"""Package an existing CI example build; never build or access a device."""

import argparse
import copy
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
from urllib.parse import urlsplit

from idf_tab5_compat import verify_sdk_patch
from tab5_sdio_diagnostics import COMPONENT as SDIO_COMPONENT, verify_component_patch


EXAMPLES = {
    "esp32-node": "esp32s3",
    "esp-box-3-display": "esp32s3",
    "waveshare-esp32-s3-touch-amoled-2.06-room-node": "esp32s3",
    "m5stack-tab5-room-node": "esp32p4",
}
SECURITY_CONFIG = {
    "CONFIG_SECURE_BOOT",
    "CONFIG_SECURE_FLASH_ENC_ENABLED",
    "CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT",
    "CONFIG_SECURE_SIGNED_APPS",
    "CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES",
}
PROVENANCE_NOTE = (
    "Sanitized metadata is provenance, not exact rebuild input. Original input "
    "hashes identify the generated files before sanitization. Floating SDK and "
    "dependency selectors do not guarantee reproducible rebuilds."
)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git(root, *arguments):
    return subprocess.check_output(
        ["git", "-C", str(root), *arguments], text=True, stderr=subprocess.PIPE
    ).rstrip("\n")


def checked_file(path, roots):
    resolved = path.resolve(strict=True)
    if not resolved.is_file() or not any(resolved.is_relative_to(root) for root in roots):
        raise ValueError("Input file is outside the approved project/build roots")
    return resolved


def sanitize(value, roots):
    if isinstance(value, dict):
        return {key: sanitize(item, roots) for key, item in value.items()}
    if isinstance(value, list):
        return [sanitize(item, roots) for item in value]
    if not isinstance(value, str):
        return value
    for root, label in roots:
        value = value.replace(str(root), label)
    if re.match(r"^(?:/|[A-Za-z]:[\\/]|~[/\\])", value):
        return "<external-path>"
    if "://" in value:
        url = urlsplit(value)
        if (
            url.scheme != "https"
            or url.hostname not in {
                "github.com", "api.github.com", "components.espressif.com",
                "api.components.espressif.com", "dl.espressif.com",
            }
            or url.username or url.password or url.query or url.fragment
        ):
            return "<private-url>"
    return value


def sdkconfig_values(text):
    values = {}
    for line in text.splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            values[key] = json.loads(value) if value.startswith('"') else value
    for key, value in values.items():
        if key in SECURITY_CONFIG and value == "y":
            raise ValueError("Secure boot/encrypted builds are not supported")
        credential = re.search(r"(?:SSID|PASSWORD|PASSPHRASE|TOKEN|SECRET|API_KEY|SETUP_CODE)$", key)
        if (credential or key == "CONFIG_OPENCLAW_ROOM_GATEWAY_HTTP_BASE_URL") and value != "":
            raise ValueError("Credential-bearing configuration must be empty for public firmware")
    return values


def read_dependency_lock(path):
    # Use the same safe YAML reader as ESP-IDF's component manager.
    from ruamel.yaml import YAML
    dependencies = YAML(typ="safe").load(path.read_text())
    if not isinstance(dependencies, dict):
        raise ValueError("Resolved dependency lock must be a mapping")
    return dependencies


def configuration_inputs(project, build, idf, description, config, normalize):
    selected = []
    # IDF applies each defaults file, then its target-specific companion if present.
    for name in filter(None, description["config_defaults"].split(";")):
        defaults = project / name
        selected.append(("defaults", checked_file(defaults, (project, build))))
        companion = defaults.with_name(defaults.name + "." + description["target"])
        if companion.exists():
            selected.append(("target_defaults", checked_file(companion, (project, build))))
    partition_root = project if config.get("CONFIG_PARTITION_TABLE_CUSTOM") == "y" else idf / "components/partition_table"
    partition = checked_file(partition_root / config["CONFIG_PARTITION_TABLE_FILENAME"], (project, build, idf))
    selected.append(("partition_table", partition))
    return [
        {"kind": kind, "path": sanitize(str(path), normalize), "sha256": sha256(path)}
        for kind, path in selected
    ]


def submodule_provenance(repo):
    result = []
    for line in git(repo, "submodule", "status", "--recursive").splitlines():
        if line[0] != " ":
            raise ValueError("Submodules must be initialized at their pinned commits")
        commit, path, *_ = line[1:].split()
        if git(repo / path, "status", "--porcelain", "--untracked-files=normal"):
            raise ValueError("Submodule contains modified source")
        result.append({"path": path, "commit": commit})
    return result


def tab5_provenance(project, build):
    bridge = project / "components/m5stack_tab5/CMakeLists.txt"
    text = bridge.read_text()
    pin = re.search(
        r"URL (https://github\.com/espressif/esp-bsp/archive/([0-9a-f]{40})\.tar\.gz)"
        r"\s+URL_HASH SHA256=([0-9a-f]{64})", text,
    )
    if not pin:
        raise ValueError("Cannot identify the Tab5 BSP source pin")
    cache = (build / "CMakeCache.txt").read_text()
    if os.environ.get("OPENCLAW_TAB5_BSP_LOCAL_PATH") or re.search(
        r"^OPENCLAW_TAB5_BSP_LOCAL_PATH:[^=]+=.+$", cache, re.MULTILINE
    ):
        raise ValueError("CI artifacts require the pinned Tab5 archive, not a local BSP override")
    return {
        "upstream_url": pin[1],
        "upstream_commit": pin[2],
        "upstream_archive_sha256": pin[3],
        "integration": "Repository-local source-only BSP bridge; not an unmodified upstream component",
        "bridge": "components/m5stack_tab5/CMakeLists.txt",
        "bridge_sha256": sha256(bridge),
    }


def package_firmware(project, build, output, target, provenance, dependencies,
                     nvs_diagnostics=False, sdio_diagnostics=False, sdio_psram_rx=False):
    project, build, output = project.resolve(), build.resolve(), output.resolve()
    repo = Path(git(project, "rev-parse", "--show-toplevel")).resolve()
    if project.parent != repo / "examples" or EXAMPLES.get(project.name) != target:
        raise ValueError("Only the four CI example/target combinations may be packaged")
    if output.exists() or not output.is_relative_to(build):
        raise ValueError("Output must be a new directory inside the build directory")
    roots = (project, build)
    description_file = checked_file(build / "project_description.json", roots)
    flash_file = checked_file(build / "flasher_args.json", roots)
    description = json.loads(description_file.read_text())
    flash = json.loads(flash_file.read_text())
    config_file = checked_file(Path(description["config_file"]), roots)
    config = sdkconfig_values(config_file.read_text())
    if (
        description["version"] != "1.2"
        or description["target"] != target
        or config.get("CONFIG_IDF_TARGET") != target
        or dependencies.get("target") != target
        or flash["extra_esptool_args"]["chip"] != target
        or Path(description["project_path"]).resolve() != project
        or Path(description["build_dir"]).resolve() != build
    ):
        raise ValueError("Build metadata schema, project or target mismatch")
    idf = Path(description["idf_path"]).resolve()
    if idf != Path(os.environ["IDF_PATH"]).resolve():
        raise ValueError("Build metadata does not match the active IDF")
    if git(repo, "status", "--porcelain", "--untracked-files=normal"):
        raise ValueError("Repository contains modified source")
    idf_dirty = bool(git(idf, "status", "--porcelain", "--untracked-files=all"))
    idf_compatibility = None
    if nvs_diagnostics and (project.name != "m5stack-tab5-room-node" or not idf_dirty):
        raise ValueError("NVS diagnostics require the explicitly patched Tab5 SDK")
    if idf_dirty:
        if project.name != "m5stack-tab5-room-node":
            raise ValueError("IDF contains modified source")
        idf_compatibility = verify_sdk_patch(idf, nvs_diagnostics)
    if not dependencies.get("dependencies"):
        raise ValueError("Resolved dependency lock is empty")
    sdio_patch = None
    if sdio_psram_rx and (not sdio_diagnostics
                         or config.get("CONFIG_SPIRAM") != "y"
                         or config.get("CONFIG_SOC_SDMMC_PSRAM_DMA_CAPABLE") != "y"):
        raise ValueError("Streaming RX PSRAM requires diagnostics and supported PSRAM SDMMC")
    if sdio_diagnostics and (project.name != "m5stack-tab5-room-node" or not nvs_diagnostics):
        raise ValueError("SDIO diagnostics require the explicitly selected Tab5 diagnostic build")
    if sdio_diagnostics or (
            project.name == "m5stack-tab5-room-node"
            and SDIO_COMPONENT in dependencies["dependencies"]):
        sdio_patch = verify_component_patch(
            project, project / "managed_components/espressif__esp_hosted",
            build, dependencies, patched=sdio_diagnostics, psram_rx=sdio_psram_rx,
        )
    normalize = [(build, "<build>"), (project, "<project>"), (repo, "<repository>"), (idf, "<idf>")]
    lock_file = checked_file(project / "dependencies.lock", roots)
    manifest = {
        "schema_version": 1,
        "example": project.name,
        "target": target,
        "source": {
            "repository": provenance["repository"],
            "commit": git(repo, "rev-parse", "HEAD"),
            "submodules": submodule_provenance(repo),
        },
        "idf": {
            "commit": git(idf, "rev-parse", "HEAD"),
            "build_revision": description["git_revision"],
            "image_requested": provenance["idf_image"],
            "dirty": idf_dirty,
        },
        "tools": {
            "esptool": importlib.metadata.version("esptool"),
            "idf_component_manager": importlib.metadata.version("idf-component-manager"),
        },
        "ci": {key: value for key, value in provenance.items() if key not in ("repository", "idf_image")},
        "metadata_note": PROVENANCE_NOTE,
        "configuration_inputs": configuration_inputs(project, build, idf, description, config, normalize),
        "original_input_sha256": {
            "sdkconfig": sha256(config_file),
            "dependencies.lock": sha256(lock_file),
            "project_description.json": sha256(description_file),
            "flasher_args.json": sha256(flash_file),
        },
    }
    if idf_compatibility is not None:
        manifest["idf"].update(idf_compatibility)
    if sdio_psram_rx:
        manifest["component_compatibility_patch"] = sdio_patch
    elif sdio_diagnostics:
        manifest["component_diagnostic_patch"] = sdio_patch
    if project.name == "m5stack-tab5-room-node":
        manifest["tab5_bsp"] = tab5_provenance(project, build)
    extra = flash["extra_esptool_args"]
    if type(extra["stub"]) is not bool:
        raise ValueError("Invalid esptool stub setting")
    if extra["before"] not in ("default_reset", "no_reset", "no_reset_no_sync") or extra["after"] not in ("hard_reset", "no_reset", "no_reset_stub"):
        raise ValueError("Unsupported esptool reset mode")
    settings = flash["flash_settings"]
    expected_args = [
        "--flash_mode", settings["flash_mode"], "--flash_size", settings["flash_size"],
        "--flash_freq", settings["flash_freq"],
    ]
    if flash["write_flash_args"] != expected_args:
        raise ValueError("Unsupported write_flash arguments")
    if any(not re.fullmatch(r"[A-Za-z0-9]+", item) for item in settings.values()):
        raise ValueError("Invalid flash setting")
    images = []
    for offset, filename in flash["flash_files"].items():
        if not re.fullmatch(r"0x[0-9a-fA-F]+", offset):
            raise ValueError("Invalid image offset")
        source = checked_file(build / filename, roots)
        if source.suffix != ".bin" or source.stat().st_size == 0:
            raise ValueError("Flash images must be nonempty .bin files")
        images.append((int(offset, 16), source, offset, f"images/{int(offset, 16):08x}.bin"))
    images.sort()
    if not images:
        raise ValueError("Empty flash image map")
    for previous, current in zip(images, images[1:]):
        if previous[0] + previous[1].stat().st_size > current[0]:
            raise ValueError("Overlapping flash images")
    relocated = copy.deepcopy(flash)
    relocated["flash_files"] = {offset: name for _, _, offset, name in images}
    for key, entry in flash.items():
        if isinstance(entry, dict) and "encrypted" in entry:
            if entry["encrypted"] is not False and entry["encrypted"] != "false":
                raise ValueError("Encrypted image entries are not supported")
            offset = entry["offset"]
            if flash["flash_files"].get(offset) != entry["file"]:
                raise ValueError("Named image does not match the flash map")
            relocated[key]["file"] = relocated["flash_files"][offset]
        elif key not in ("flash_files", "flash_settings", "write_flash_args", "extra_esptool_args"):
            raise ValueError("Unsupported flash metadata entry")
    for name in ("bootloader", "partition-table", "app"):
        if name not in relocated:
            raise ValueError("Missing required bootloader, partition table or app image")
    arguments = expected_args + [item for _, _, offset, name in images for item in (offset, name)]
    global_args = ["--chip", target, "--before", extra["before"], "--after", extra["after"]]
    if not extra["stub"]:
        global_args.append("--no-stub")
    command = "python -m esptool " + shlex.join(global_args) + ' --port PORT write_flash "@flash_args"'
    instructions = (
        f"# {project.name} ({target})\n\n"
        f"Source commit: {manifest['source']['commit']}\n\n"
        "Verify SHA256SUMS before flashing. From this extracted directory, install "
        "the recorded esptool version in a Python virtual environment:\n\n"
        f"    python -m pip install esptool=={manifest['tools']['esptool']}\n\n"
        "Replace PORT with the explicitly selected serial port, then run:\n\n"
        f"    {command}\n\n"
        "No source checkout or ESP-IDF installation is needed. Do not add --force "
        "or erase-all. Existing device data may be incompatible with this partition "
        "layout; back it up before changing firmware. Provision Wi-Fi and the Gateway "
        "through the serial console; no credentials are supplied here.\n\n"
        "Tab5 bundles contain P4 firmware only; the C6 must already have compatible "
        "esp_hosted 1.4.0 / esp_wifi_remote 0.8.5 firmware.\n\n"
        f"{PROVENANCE_NOTE}\n"
    )
    # Stage only after validation; failed packaging must not leave an uploadable partial bundle.
    with tempfile.TemporaryDirectory(prefix=".firmware-", dir=build) as temporary:
        stage = Path(temporary) / "firmware"
        (stage / "images").mkdir(parents=True)
        for _, source, _, name in images:
            shutil.copyfile(source, stage / name)
        files = {
            "flasher_args.json": relocated,
            "project_description.sanitized.json": sanitize({
                key: description[key] for key in (
                    "version", "project_name", "project_version", "git_revision", "target",
                    "min_rev", "max_rev", "monitor_baud", "app_bin", "build_components",
                )
            }, normalize),
            "sdkconfig.sanitized.json": sanitize(config, normalize),
            "dependencies.lock.sanitized.json": sanitize(dependencies, normalize),
        }
        for name, value in files.items():
            (stage / name).write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
        (stage / "flash_args").write_text(shlex.join(arguments) + "\n")
        (stage / "FLASHING.md").write_text(instructions)
        manifest["files"] = {
            path.relative_to(stage).as_posix(): {"sha256": sha256(path), "size": path.stat().st_size}
            for path in sorted(stage.rglob("*")) if path.is_file()
        }
        (stage / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        (stage / "SHA256SUMS").write_text("".join(
            f"{sha256(path)}  {path.relative_to(stage).as_posix()}\n"
            for path in sorted(stage.rglob("*")) if path.is_file()
        ))
        stage.rename(output)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True, choices=("esp32s3", "esp32p4"))
    for name in ("repository", "event", "event-sha", "run-id", "run-attempt", "idf-image"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--pr-head-sha", default="")
    parser.add_argument("--nvs-diagnostics", action="store_true")
    parser.add_argument("--sdio-diagnostics", action="store_true")
    parser.add_argument("--sdio-psram-rx", action="store_true")
    args = parser.parse_args()
    provenance = vars(args).copy()
    provenance.pop("target")
    provenance.pop("nvs_diagnostics")
    provenance.pop("sdio_diagnostics")
    provenance.pop("sdio_psram_rx")
    project = Path.cwd()
    # ruamel.yaml is already part of ESP-IDF's component-manager environment.
    try:
        from ruamel.yaml import YAMLError
    except ImportError:
        parser.exit(1, "Run packaging inside the configured ESP-IDF Python environment.\n")
    try:
        dependencies = read_dependency_lock(project / "dependencies.lock")
        output = package_firmware(
            project, project / "build", project / "build/firmware",
            args.target, provenance, dependencies, args.nvs_diagnostics,
            args.sdio_diagnostics, args.sdio_psram_rx,
        )
    except ValueError as error:
        parser.exit(1, f"Firmware packaging failed: {error}\n")
    except (YAMLError, KeyError, TypeError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Firmware packaging failed: {type(error).__name__}. Check build metadata and public defaults.\n")
    print(f"Firmware bundle: {output.relative_to(project)}")


if __name__ == "__main__":
    main()
