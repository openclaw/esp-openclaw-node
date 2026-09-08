#!/usr/bin/env python3
"""Apply or verify the pinned Tab5 SDIO probe after IDF configuration."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess


COMPONENT = "espressif/esp_hosted"
VERSION = "1.4.0"
REVISION = "6040085eefe908de68fcd3438bf10b8ecd6de0c6"
COMPONENT_HASH = "ef6bc803451fab3c3f9a679bbc1c48fba0ceeeabc5af4178dd7d2300fafa423e"
PATCHED_COMPONENT_HASH = "cd2679547dfbc73f0d71d526fb5f42b6814c92f270acdc76e485118663ff6241"
MANIFEST_SHA256 = "eb3cad8e876545eb28684e2648e83f73645bce7a260123776f36dff2efbe8027"
SOURCE_PATH = "host/drivers/transport/sdio/sdio_drv.c"
ORIGINAL_SHA256 = "9868f97b7c68e5d8f1a3ac904dc6922b8ba0cc160c28c6a1402cd5c4147a652a"
PATCHED_SHA256 = "5d3772ff65d6aeb837019adc4f03513ed766e045101d1b225cb80d17d66cc986"
PATCH_RELATIVE = "patches/esp-hosted/tab5-sdio-first-allocation-failure.patch"
PATCH_PATH = Path(__file__).resolve().parents[1] / PATCH_RELATIVE
PATCH_SHA256 = "430b99399fc6ab7d9d624855c1dd80ef3f8c3739776480ceae2dedeb1cc91f84"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def inspect_component(project, component, build, dependencies):
    project = Path(project).resolve(strict=True)
    component = Path(component).absolute()
    build = Path(build).resolve(strict=True)
    expected = project / "managed_components/espressif__esp_hosted"
    if (project.name != "m5stack-tab5-room-node" or project.parent.name != "examples"
            or component != expected or component.resolve(strict=True) != expected
            or not build.is_relative_to(project)):
        raise ValueError("Select the configured Tab5 project and its managed component")
    for path in component.rglob("*"):
        if path.is_symlink():
            raise ValueError("Managed component must not contain symbolic links")
    if (not isinstance(dependencies, dict)
            or not isinstance(dependencies.get("dependencies"), dict)):
        raise ValueError("Dependency lock must contain a dependency mapping")
    dependency = dependencies["dependencies"].get(COMPONENT, {})
    if not isinstance(dependency, dict) or not isinstance(dependency.get("source"), dict):
        raise ValueError("Resolved component must contain a registry source")
    source = dependency.get("source", {})
    if (dependencies.get("target") != "esp32p4"
            or dependency.get("version") != VERSION
            or dependency.get("component_hash") != COMPONENT_HASH
            or source.get("type") != "service"
            or source.get("service_url") != "https://components.espressif.com/"):
        raise ValueError("Resolved esp_hosted identity is not the approved 1.4.0 component")
    if digest((component / "idf_component.yml").read_bytes()) != MANIFEST_SHA256:
        raise ValueError("Component manifest or registry revision differs")
    if (PATCH_PATH.is_symlink()
            or digest(PATCH_PATH.read_bytes()) != PATCH_SHA256):
        raise ValueError("Tracked SDIO patch has unexpected contents")
    commands = json.loads((build / "compile_commands.json").read_text())
    compiled_sources = []
    for command in commands:
        filename = Path(command["file"])
        if filename.name == "sdio_drv.c":
            compiled_sources.append((Path(command["directory"]) / filename).resolve(strict=True))
    if compiled_sources != [component / SOURCE_PATH]:
        raise ValueError("Build must compile exactly the verified SDIO source")
    return component, digest((component / SOURCE_PATH).read_bytes())


def validate_component_hash(component, expected):
    # Use the same manifest-aware hashing as the resolved IDF component manager.
    from idf_component_tools.errors import ProcessingError
    from idf_component_tools.hash_tools.errors import ValidatingHashError
    from idf_component_tools.hash_tools.validate import (
        validate_hash_eq_hashdir,
        validate_hash_eq_hashfile,
    )
    try:
        validate_hash_eq_hashfile(component, COMPONENT_HASH)
        validate_hash_eq_hashdir(component, expected)
    except (ProcessingError, ValidatingHashError) as error:
        raise ValueError("Managed component bytes do not match the selected profile") from error


def verify_component_patch(project, component, build, dependencies, patched=True):
    component, source_hash = inspect_component(project, component, build, dependencies)
    expected_source = PATCHED_SHA256 if patched else ORIGINAL_SHA256
    expected_component = PATCHED_COMPONENT_HASH if patched else COMPONENT_HASH
    if source_hash != expected_source:
        raise ValueError("SDIO source does not match the explicitly selected profile")
    validate_component_hash(component, expected_component)
    if not patched:
        return {"component": COMPONENT, "version": VERSION, "modified": False}
    return {
        "component": COMPONENT,
        "version": VERSION,
        "registry_revision": REVISION,
        "registry_path": ".",
        "original_component_hash": COMPONENT_HASH,
        "modified": patched,
        "patch": PATCH_RELATIVE,
        "patch_sha256": PATCH_SHA256,
        "source": SOURCE_PATH,
        "original_source_sha256": ORIGINAL_SHA256,
        "patched_source_sha256": source_hash,
        "patched_component_hash": expected_component,
    }


def apply_component_patch(project, component, build, dependencies):
    component, source_hash = inspect_component(project, component, build, dependencies)
    if source_hash == PATCHED_SHA256:
        return verify_component_patch(project, component, build, dependencies)
    verify_component_patch(project, component, build, dependencies, patched=False)
    subprocess.run(["git", "apply", "--check", str(PATCH_PATH)], cwd=component, check=True)
    subprocess.run(["git", "apply", str(PATCH_PATH)], cwd=component, check=True)
    return verify_component_patch(project, component, build, dependencies)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-path", type=Path, required=True)
    parser.add_argument("--component-path", type=Path, required=True)
    parser.add_argument("--build-path", type=Path, required=True)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    try:
        from ruamel.yaml import YAML
        from ruamel.yaml.error import YAMLError
    except ImportError:
        parser.exit(1, "Run this helper in the configured ESP-IDF Python environment.\n")
    try:
        dependencies = YAML(typ="safe").load(
            (args.project_path / "dependencies.lock").read_text()
        )
        operation = verify_component_patch if args.verify_only else apply_component_patch
        result = operation(args.project_path, args.component_path, args.build_path, dependencies)
    except (ValueError, TypeError, KeyError, OSError, ImportError,
            YAMLError, subprocess.CalledProcessError):
        parser.exit(1, "SDIO diagnostic patch refused: verify the pinned component and build inputs.\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
