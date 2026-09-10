#!/usr/bin/env python3
"""Apply or verify the pinned Tab5 post-ISP CSI format compatibility patch."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

from idf_tab5_compat import BASE_COMMIT, git, verify_sdk_patch


COMPONENT = "espressif/esp_video"
VERSION = "2.4.1"
REVISION = "67a555a517b7aa4753836432453659abfaca393a"
COMPONENT_HASH = "ef614cc04e69e117fd08272a8b347ae5972eff2d0c8659af6444f8484931c7c1"
PATCHED_COMPONENT_HASH = "106b93e92355f7577ee03f2ea3208d43b8fad15780c438b51633e20c81d10ccf"
MANIFEST_SHA256 = "7ebdab70659a0b198ab04bf1baaacf0b97a6b3e90200f26190c48f51e0e90f62"
SOURCE_PATH = "src/device/esp_video_csi_format.c"
ORIGINAL_SHA256 = "1564837860a5af41f0e3191508e41b5c1dcbec87518b1c4b4d1a549a66a3aa3b"
PATCHED_SHA256 = "99e47cbdbb6fc8d218ea17ac8ac4fd02c7b03aad89e06c48bbc1e5df4d4c3dfb"
PATCH_RELATIVE = "patches/esp-video/tab5-csi-post-isp-format.patch"
PATCH_PATH = Path(__file__).resolve().parents[1] / PATCH_RELATIVE
PATCH_SHA256 = "725f6309fe77fd532a69330c7f3c9632b45675fe05a2489f76895d154098cd36"
SDK_SOURCE = "components/esp_driver_cam/csi/src/esp_cam_ctlr_csi.c"
SDK_SOURCE_SHA256 = "2ec4c6e9fa2f6b83760e04d8dce269b02f5eafaae3feed7d761de5699fac152b"
PROFILE = {
    "CONFIG_IDF_TARGET": '"esp32p4"',
    "CONFIG_ESP32P4_SELECTS_REV_LESS_V3": "y",
    "CONFIG_ESP32P4_REV_MIN_FULL": "0",
    "CONFIG_ESP32P4_REV_MAX_FULL": "199",
}

# Only these exact static refusals may cross the CLI boundary.
REFUSAL_CODES = {
    "Select the configured Tab5 project and its managed esp_video": "project_path",
    "Managed component must not contain symbolic links": "component_symlink",
    "Dependency lock must contain a dependency mapping": "dependency_mapping",
    "Resolved camera component must have a registry source": "registry_source",
    "Resolved esp_video identity differs from the approved 2.4.1 component": "component_identity",
    "Camera component manifest or registry revision differs": "component_manifest",
    "Tracked camera patch has unexpected contents": "tracked_patch",
    "Camera build does not select the explicit SDK and project": "build_identity",
    "Camera configuration is outside the selected project": "config_path",
    "Camera compatibility requires the unchanged early-P4 Tab5 profile": "early_p4_profile",
    "SDK CSI source does not match the approved backported contract": "sdk_csi_contract",
    "Build must compile exactly one camera format mapper": "mapper_count",
    "Build selects a different camera format mapper": "mapper_path",
    "Camera compiler invocation selects a different source": "compiler_source",
    "Camera compile command must identify its output object": "compiler_output",
    "Camera object must be inside the selected build": "object_path",
    "Camera component bytes do not match the selected profile": "component_integrity",
    "Camera source does not match the explicitly selected profile": "source_profile",
    "Camera mapper needs a current ELF32 RISC-V relocatable object": "compiled_object",
    "Select the SDK repository root explicitly": "sdk_root",
    "SDK is not at the approved compatibility-patch base": "sdk_base",
    "Tracked SDK patch has unexpected contents": "sdk_patch_bytes",
    "SDK base source does not match the approved whole-file hash": "sdk_base_source",
    "SDK source must be a regular file at the approved path": "sdk_source_path",
    "SDK is not modified by exactly the selected Tab5 patches": "sdk_patch_state",
    "Cannot determine the camera component Git worktree": "component_worktree",
    "Camera component is outside its Git worktree": "component_worktree_path",
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def managed_component_path(project, component):
    project = Path(project).resolve(strict=True)
    component = Path(component).absolute()
    expected = project / "managed_components/espressif__esp_video"
    if (project.name != "m5stack-tab5-room-node" or project.parent.name != "examples"
            or component != expected or component.resolve(strict=True) != expected):
        raise ValueError("Select the configured Tab5 project and its managed esp_video")
    if any(path.is_symlink() for path in component.rglob("*")):
        raise ValueError("Managed component must not contain symbolic links")
    return project, component


def inspect_component(project, component, build, idf, dependencies, patched):
    project, component = managed_component_path(project, component)
    build = Path(build).resolve(strict=True)
    idf = Path(idf).resolve(strict=True)
    if not build.is_relative_to(project):
        raise ValueError("Select the configured Tab5 project and its managed esp_video")
    if (not isinstance(dependencies, dict)
            or not isinstance(dependencies.get("dependencies"), dict)):
        raise ValueError("Dependency lock must contain a dependency mapping")
    dependency = dependencies["dependencies"].get(COMPONENT, {})
    if not isinstance(dependency, dict) or not isinstance(dependency.get("source"), dict):
        raise ValueError("Resolved camera component must have a registry source")
    source = dependency["source"]
    if (dependencies.get("target") != "esp32p4"
            or dependency.get("version") != VERSION
            or dependency.get("component_hash") != COMPONENT_HASH
            or source.get("type") != "service"
            or source.get("registry_url") != "https://components.espressif.com/"):
        raise ValueError("Resolved esp_video identity differs from the approved 2.4.1 component")
    if digest((component / "idf_component.yml").read_bytes()) != MANIFEST_SHA256:
        raise ValueError("Camera component manifest or registry revision differs")
    if PATCH_PATH.is_symlink() or digest(PATCH_PATH.read_bytes()) != PATCH_SHA256:
        raise ValueError("Tracked camera patch has unexpected contents")
    description = json.loads((build / "project_description.json").read_text())
    if (Path(description["idf_path"]).resolve() != idf
            or Path(description["project_path"]).resolve() != project
            or Path(description["build_dir"]).resolve() != build
            or description["target"] != "esp32p4"):
        raise ValueError("Camera build does not select the explicit SDK and project")
    config_file = Path(description["config_file"]).resolve(strict=True)
    if not config_file.is_relative_to(project):
        raise ValueError("Camera configuration is outside the selected project")
    config = dict(line.split("=", 1) for line in config_file.read_text().splitlines()
                  if line.startswith("CONFIG_") and "=" in line)
    if patched and any(config.get(key) != value for key, value in PROFILE.items()):
        raise ValueError("Camera compatibility requires the unchanged early-P4 Tab5 profile")
    if patched:
        # The version number alone does not identify this backported CSI contract.
        verify_sdk_patch(idf, nvs_diagnostics=True)
        sdk_source = idf / SDK_SOURCE
        if (sdk_source.resolve(strict=True) != sdk_source
                or digest(sdk_source.read_bytes()) != SDK_SOURCE_SHA256
                or digest(git(idf, "show", f"HEAD:{SDK_SOURCE}")) != SDK_SOURCE_SHA256):
            raise ValueError("SDK CSI source does not match the approved backported contract")
    commands = json.loads((build / "compile_commands.json").read_text())
    selected = [entry for entry in commands if Path(entry["file"]).name == Path(SOURCE_PATH).name]
    if len(selected) != 1:
        raise ValueError("Build must compile exactly one camera format mapper")
    command = selected[0]
    directory = Path(command["directory"])
    if (directory / command["file"]).resolve(strict=True) != component / SOURCE_PATH:
        raise ValueError("Build selects a different camera format mapper")
    arguments = command.get("arguments")
    if arguments is None:
        arguments = shlex.split(command["command"])
    if (arguments.count("-c") != 1 or arguments.index("-c") + 1 == len(arguments)
            or (directory / arguments[arguments.index("-c") + 1]).resolve(strict=True)
            != component / SOURCE_PATH):
        raise ValueError("Camera compiler invocation selects a different source")
    if arguments.count("-o") != 1 or arguments.index("-o") + 1 == len(arguments):
        raise ValueError("Camera compile command must identify its output object")
    output = (directory / arguments[arguments.index("-o") + 1]).resolve()
    if not output.is_relative_to(build):
        raise ValueError("Camera object must be inside the selected build")
    return component, output


def validate_component_hash(component, expected, stored_hash=None):
    from idf_component_tools.errors import ProcessingError
    from idf_component_tools.hash_tools.errors import ValidatingHashError
    from idf_component_tools.hash_tools.validate import (
        validate_hash_eq_hashdir,
        validate_hash_eq_hashfile,
    )
    try:
        validate_hash_eq_hashfile(component, COMPONENT_HASH if stored_hash is None else stored_hash)
        validate_hash_eq_hashdir(component, expected)
    except (ProcessingError, ValidatingHashError) as error:
        raise ValueError("Camera component bytes do not match the selected profile") from error


def verify_unpatched_component(project, component, dependencies):
    """Ordinary builds follow their resolved lock, not the opt-in repair pins."""
    from ruamel.yaml import YAML
    from ruamel.yaml.error import YAMLError

    _, component = managed_component_path(project, component)
    if (not isinstance(dependencies, dict)
            or not isinstance(dependencies.get("dependencies"), dict)):
        raise ValueError("Dependency lock must contain a dependency mapping")
    dependency = dependencies["dependencies"].get(COMPONENT)
    if not isinstance(dependency, dict) or not isinstance(dependency.get("source"), dict):
        raise ValueError("Resolved camera component must have a registry source")
    source = dependency["source"]
    version, expected = dependency.get("version"), dependency.get("component_hash")
    if (dependencies.get("target") != "esp32p4"
            or not isinstance(version, str) or not version
            or not isinstance(expected, str) or re.fullmatch(r"[0-9a-f]{64}", expected) is None
            or source.get("type") != "service"
            or not isinstance(source.get("registry_url"), str) or not source["registry_url"]):
        raise ValueError("Resolved camera component has an invalid lock identity")
    validate_component_hash(component, expected, stored_hash=expected)
    try:
        manifest = YAML(typ="safe").load((component / "idf_component.yml").read_text())
    except YAMLError as error:
        raise ValueError("Camera component manifest is invalid") from error
    if not isinstance(manifest, dict) or manifest.get("version") != version:
        raise ValueError("Camera component manifest version differs from the lock")


def verify_component_patch(project, component, build, idf, dependencies,
                           patched=True, compiled=True):
    component, output = inspect_component(project, component, build, idf, dependencies, patched)
    source = component / SOURCE_PATH
    source_hash = digest(source.read_bytes())
    if source_hash != (PATCHED_SHA256 if patched else ORIGINAL_SHA256):
        raise ValueError("Camera source does not match the explicitly selected profile")
    validate_component_hash(component, PATCHED_COMPONENT_HASH if patched else COMPONENT_HASH)
    result = {"component": COMPONENT, "version": VERSION, "modified": patched}
    if patched:
        result.update(
            profile="tab5-early-p4-post-isp-csi", sdk_base_commit=BASE_COMMIT,
            sdk_contract_source=SDK_SOURCE, sdk_contract_sha256=SDK_SOURCE_SHA256,
            registry_revision=REVISION, registry_path="esp_video",
            original_component_hash=COMPONENT_HASH,
            patched_component_hash=PATCHED_COMPONENT_HASH,
            source=SOURCE_PATH, original_source_sha256=ORIGINAL_SHA256,
            patched_source_sha256=source_hash,
            patch=PATCH_RELATIVE, patch_sha256=PATCH_SHA256,
        )
    if compiled:
        with output.open("rb") as stream:
            header = stream.read(20)
        if (header[:6] != b"\x7fELF\x01\x01" or header[16:20] != b"\x01\x00\xf3\x00"
                or output.stat().st_mtime_ns < source.stat().st_mtime_ns):
            raise ValueError("Camera mapper needs a current ELF32 RISC-V relocatable object")
        result["compiled_object"] = {
            "path": output.relative_to(Path(build).resolve()).as_posix(),
            "sha256": digest(output.read_bytes()),
        }
    return result


def apply_component_patch(project, component, build, idf, dependencies):
    component, _ = inspect_component(project, component, build, idf, dependencies, patched=True)
    if digest((component / SOURCE_PATH).read_bytes()) != PATCHED_SHA256:
        verify_component_patch(project, component, build, idf, dependencies,
                               patched=False, compiled=False)
        discovery = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"], cwd=component,
            capture_output=True, text=True, env={**os.environ, "LC_ALL": "C"},
        )
        root, directory = component, []
        if discovery.returncode == 0:
            root = Path(discovery.stdout.strip()).resolve(strict=True)
            if not component.is_relative_to(root):
                raise ValueError("Camera component is outside its Git worktree")
            directory = ["--directory=" + component.relative_to(root).as_posix()]
        elif (discovery.returncode != 128
              or discovery.stderr.strip()
              != "fatal: not a git repository (or any of the parent directories): .git"
              or any((parent / ".git").exists() or (parent / ".git").is_symlink()
                     for parent in (component, *component.parents))):
            raise ValueError("Cannot determine the camera component Git worktree")
        # Git-style patch headers are filtered by the current repository prefix.
        subprocess.run(["git", "apply", "--check", *directory, str(PATCH_PATH)],
                       cwd=root, check=True)
        subprocess.run(["git", "apply", *directory, str(PATCH_PATH)], cwd=root, check=True)
    return verify_component_patch(project, component, build, idf, dependencies, compiled=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("project-path", "component-path", "build-path", "idf-path"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    try:
        from ruamel.yaml import YAML
        from ruamel.yaml.error import YAMLError
    except ImportError:
        parser.exit(1, "Run this helper in the configured ESP-IDF Python environment.\n")
    try:
        dependencies = YAML(typ="safe").load((args.project_path / "dependencies.lock").read_text())
        operation = verify_component_patch if args.verify_only else apply_component_patch
        result = operation(args.project_path, args.component_path, args.build_path,
                           args.idf_path, dependencies)
    except (ValueError, TypeError, KeyError, OSError, ImportError,
            YAMLError, subprocess.CalledProcessError) as error:
        code = REFUSAL_CODES.get(str(error), "unclassified") if type(error) is ValueError else "unclassified"
        parser.exit(1, f"Camera compatibility patch refused: guard={code}. Check the pinned profile and build inputs.\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
