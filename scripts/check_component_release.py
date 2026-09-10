#!/usr/bin/env python3
"""Read-only preflight and artifact identity checks for component releases."""

import argparse
import http.client
import io
import json
from pathlib import Path
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
import zipfile

import yaml


COMPONENT = "esp-openclaw-node"
NAMESPACE = "espressif"
COMPONENT_PATH = f"components/{COMPONENT}"
REGISTRY_URL = f"https://components.espressif.com/api/components/{NAMESPACE}/{COMPONENT}"
REPOSITORIES = {
    "https://github.com/openclaw/esp-openclaw-node",
    "https://github.com/openclaw/esp-openclaw-node.git",
    "git://github.com/openclaw/esp-openclaw-node.git",
}
MAX_JSON_BYTES = 1024 * 1024
MAX_ARCHIVE_BYTES = 16 * 1024 * 1024
MAX_MANIFEST_BYTES = 64 * 1024
MAX_ARCHIVE_ENTRIES = 4096
REQUEST_TIMEOUT = 15


class ReleaseError(Exception):
    pass


def unique_mapping(pairs):
    result = {}
    for key, value in pairs:
        if not isinstance(key, str) or key in result:
            raise ReleaseError("metadata contains a non-string or duplicate mapping key")
        result[key] = value
    return result


class ManifestLoader(yaml.SafeLoader):
    pass


def construct_mapping(loader, node):
    return unique_mapping(
        (loader.construct_object(key), loader.construct_object(value))
        for key, value in node.value
    )


ManifestLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, construct_mapping
)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def download(url, limit):
    request = urllib.request.Request(
        url, headers={"User-Agent": "esp-openclaw-node-release-check"}
    )
    try:
        with urllib.request.build_opener(NoRedirect).open(
            request, timeout=REQUEST_TIMEOUT
        ) as response:
            if response.status != 200:
                raise ReleaseError("registry request did not return HTTP 200")
            data = response.read(limit + 1)
    except (urllib.error.URLError, OSError, http.client.HTTPException) as error:
        raise ReleaseError("registry request failed; release is not verified") from error
    if len(data) > limit:
        raise ReleaseError("registry response exceeds the read limit")
    return data


def parse_manifest(data, expected_version, commit_sha=None):
    if len(data) > MAX_MANIFEST_BYTES:
        raise ReleaseError("component manifest exceeds the read limit")
    try:
        manifest = yaml.load(data, Loader=ManifestLoader)
    except (yaml.YAMLError, UnicodeError, RecursionError) as error:
        raise ReleaseError("component manifest is malformed YAML") from error
    if not isinstance(manifest, dict):
        raise ReleaseError("component manifest must be a mapping")
    if not isinstance(manifest.get("version"), str) or manifest["version"] != expected_version:
        raise ReleaseError("component manifest version does not match expected version")
    repository = manifest.get("repository")
    if not isinstance(repository, str) or repository not in REPOSITORIES:
        raise ReleaseError("component manifest repository does not match this repository")
    info = manifest.get("repository_info")
    if not isinstance(info, dict) or info.get("path") != COMPONENT_PATH:
        raise ReleaseError("component manifest repository path does not match component")
    if commit_sha is not None and info.get("commit_sha") != commit_sha:
        raise ReleaseError("registry artifact commit SHA does not match dispatch SHA")


def registry_versions():
    try:
        metadata = json.loads(
            download(REGISTRY_URL, MAX_JSON_BYTES), object_pairs_hook=unique_mapping
        )
    except (ValueError, UnicodeError, RecursionError) as error:
        raise ReleaseError("registry response is malformed JSON") from error
    if not isinstance(metadata, dict) or (
        metadata.get("name") != COMPONENT or metadata.get("namespace") != NAMESPACE
    ):
        raise ReleaseError("registry response does not identify the expected component")
    versions = metadata.get("versions")
    if not isinstance(versions, list):
        raise ReleaseError("registry response has no versions list")
    result = {}
    for entry in versions:
        if not isinstance(entry, dict):
            raise ReleaseError("registry version entry is malformed")
        version = entry.get("version")
        if not isinstance(version, str) or not version or version in result:
            raise ReleaseError("registry version is missing, invalid, or duplicated")
        if not isinstance(entry.get("url"), str) or not entry["url"]:
            raise ReleaseError("registry version has no artifact URL")
        result[version] = entry
    return result


def archive_manifest(entry):
    try:
        url = urllib.parse.urlsplit(entry["url"])
    except ValueError as error:
        raise ReleaseError("registry artifact URL is malformed") from error
    if (
        url.scheme != "https"
        or url.netloc != "components-file.espressif.com"
        or not url.path.startswith(f"/components/{NAMESPACE}/{COMPONENT}/")
        or not url.path.endswith(".zip")
        or url.query
        or url.fragment
    ):
        raise ReleaseError("registry artifact URL is outside the expected download location")
    data = download(entry["url"], MAX_ARCHIVE_BYTES)
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            entries = archive.infolist()
            manifests = [item for item in entries if item.filename == "idf_component.yml"]
            if len(entries) > MAX_ARCHIVE_ENTRIES or len(manifests) != 1:
                raise ReleaseError("registry archive must contain one root component manifest")
            manifest = manifests[0]
            if manifest.file_size > MAX_MANIFEST_BYTES:
                raise ReleaseError("archived component manifest exceeds the read limit")
            # Read only the bounded root manifest; never extract registry archive paths.
            with archive.open(manifest) as stream:
                return stream.read(MAX_MANIFEST_BYTES + 1)
    except (zipfile.BadZipFile, RuntimeError, NotImplementedError, OSError) as error:
        raise ReleaseError("registry archive could not be read") from error


def check_release(mode, manifest_path, expected_version, commit_sha=None):
    if not expected_version or len(expected_version) > 128:
        raise ReleaseError("expected version must be a nonempty version string")
    if mode == "verify" and (
        commit_sha is None or re.fullmatch(r"[0-9a-f]{40}", commit_sha) is None
    ):
        raise ReleaseError("verification requires the full checked-out dispatch commit SHA")
    with manifest_path.open("rb") as stream:
        parse_manifest(stream.read(MAX_MANIFEST_BYTES + 1), expected_version)
    versions = registry_versions()
    if mode == "preflight":
        if expected_version in versions:
            raise ReleaseError(f"version {expected_version} already exists; refusing release")
        return f"release preflight passed: {NAMESPACE}/{COMPONENT} {expected_version} is absent"
    entry = versions.get(expected_version)
    if entry is None:
        raise ReleaseError("expected version is not visible in the registry; artifact not verified")
    if entry.get("yanked_at") is not None:
        raise ReleaseError("registry artifact is yanked")
    parse_manifest(archive_manifest(entry), expected_version, commit_sha)
    return f"registry artifact verified: {NAMESPACE}/{COMPONENT} {expected_version} at {commit_sha}"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("preflight", "verify"))
    parser.add_argument("--expected-version", required=True)
    parser.add_argument("--commit-sha")
    parser.add_argument(
        "--manifest", type=Path, default=Path(COMPONENT_PATH) / "idf_component.yml"
    )
    args = parser.parse_args(argv)
    try:
        print(check_release(args.mode, args.manifest, args.expected_version, args.commit_sha))
    except (ReleaseError, OSError) as error:
        print(f"component release check failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
