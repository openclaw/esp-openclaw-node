"""Exercise the release guard at its CLI and registry-read boundaries."""

from contextlib import redirect_stderr, redirect_stdout
import copy
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import urllib.error
import zipfile

import yaml


SCRIPT = Path(__file__).resolve().parents[1] / "check_component_release.py"
SPEC = importlib.util.spec_from_file_location("check_component_release", SCRIPT)
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)

VERSION = "1.0.1"
SHA = "a" * 40
ARTIFACT_URL = (
    "https://components-file.espressif.com/components/espressif/esp-openclaw-node/"
    "1.0.1/espressif__esp-openclaw-node-v1.0.1.zip"
)
MANIFEST = {
    "version": VERSION,
    "repository": "https://github.com/openclaw/esp-openclaw-node.git",
    "repository_info": {"path": "components/esp-openclaw-node", "commit_sha": SHA},
}


def registry(*versions):
    return json.dumps({
        "name": "esp-openclaw-node", "namespace": "espressif", "versions": list(versions),
    }).encode()


def version_entry(**changes):
    return {"version": VERSION, "url": ARTIFACT_URL, "yanked_at": None, **changes}


def archive_bytes(manifest=MANIFEST, filename="idf_component.yml"):
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        data = manifest if isinstance(manifest, bytes) else yaml.safe_dump(manifest).encode()
        archive.writestr(filename, data)
    return output.getvalue()


class ReleaseCheckTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="component-release-test-")
        self.addCleanup(self.directory.cleanup)
        self.manifest = Path(self.directory.name) / "idf_component.yml"
        self.manifest.write_text(yaml.safe_dump(MANIFEST), encoding="utf-8")

    def run_check(self, mode="preflight", expected_version=VERSION, commit_sha=SHA):
        stdout, stderr = io.StringIO(), io.StringIO()
        args = [mode, "--manifest", str(self.manifest), "--expected-version", expected_version]
        if mode == "verify":
            args.extend(["--commit-sha", commit_sha])
        with redirect_stdout(stdout), redirect_stderr(stderr):
            status = release.main(args)
        return status, stdout.getvalue(), stderr.getvalue()

    def test_version_mismatch_refuses_before_network_access(self):
        with patch.object(release, "download") as read:
            status, output, error = self.run_check(expected_version="1.0.2")
        self.assertEqual(status, 1)
        self.assertEqual(output, "")
        self.assertIn("version does not match", error)
        read.assert_not_called()

    def test_preflight_allows_absent_version_but_refuses_existing_or_yanked(self):
        cases = [
            (registry(), 0, "preflight passed"),
            (registry(version_entry()), 1, "already exists"),
            (registry(version_entry(yanked_at="2026-01-01T00:00:00Z")), 1, "already exists"),
        ]
        for response, expected_status, message in cases:
            with self.subTest(message=message), patch.object(
                release, "download", return_value=response
            ) as read:
                status, output, error = self.run_check()
            self.assertEqual(status, expected_status)
            self.assertIn(message, output + error)
            self.assertEqual(read.call_count, 1)

    def test_registry_network_failures_are_not_absence(self):
        for failure in (
            urllib.error.URLError("unavailable"),
            TimeoutError("timed out"),
            urllib.error.HTTPError(release.REGISTRY_URL, 404, "missing", {}, None),
            urllib.error.HTTPError(release.REGISTRY_URL, 302, "redirect", {}, None),
        ):
            with self.subTest(failure=type(failure).__name__), patch(
                "urllib.request.OpenerDirector.open", side_effect=failure
            ):
                status, output, error = self.run_check()
            self.assertEqual(status, 1)
            self.assertEqual(output, "")
            self.assertIn("registry request failed", error)

    def test_malformed_registry_metadata_fails_closed(self):
        malformed = [
            b"not json", b"\xff", b"[]", b"{}", b'{"name":1,"name":2}',
            registry(None), registry({}), registry(version_entry(version=1)),
            registry(version_entry(url=None)), registry(version_entry(), version_entry()),
            b'{"name":"esp-openclaw-node","namespace":"espressif","versions":{}}',
            b'{"name":"other","namespace":"espressif","versions":[]}',
        ]
        for response in malformed:
            with self.subTest(response=response), patch.object(
                release, "download", return_value=response
            ):
                status, output, error = self.run_check()
            self.assertEqual(status, 1)
            self.assertEqual(output, "")
            self.assertIn("check failed", error)

    def test_manifest_parser_rejects_ambiguous_or_invalid_yaml(self):
        for data in (
            b"[", b"[]", b"version: 1.0\n", b"!!python/object:invalid {}",
            b"version: 1.0.1\nversion: 1.0.2\n",
            yaml.safe_dump({**MANIFEST, "repository": []}).encode(),
            yaml.safe_dump({**MANIFEST, "repository": {}}).encode(),
        ):
            with self.subTest(data=data):
                self.manifest.write_bytes(data)
                with patch.object(release, "download") as read:
                    status, output, error = self.run_check()
                self.assertEqual(status, 1)
                self.assertEqual(output, "")
                self.assertIn("check failed", error)
                read.assert_not_called()

    def test_verifies_embedded_identity_not_registry_listing_claims(self):
        entry = version_entry(repository="ignored listing metadata", commit_sha="b" * 40)
        with patch.object(release, "download", side_effect=[
            registry(entry), archive_bytes(),
        ]) as read:
            status, output, error = self.run_check("verify")
        self.assertEqual((status, error), (0, ""))
        self.assertIn("registry artifact verified", output)
        self.assertIn(SHA, output)
        self.assertNotIn("uploaded", output)
        self.assertEqual(read.call_args_list[1].args[0], ARTIFACT_URL)

    def test_verify_rejects_each_artifact_identity_mismatch(self):
        mutations = [
            {"version": "1.0.2"},
            {"repository": "https://github.com/example/other.git"},
            {"repository_info": {"path": "components/other", "commit_sha": SHA}},
            {"repository_info": {"path": "components/esp-openclaw-node", "commit_sha": "b" * 40}},
            {"repository_info": {"path": "components/esp-openclaw-node"}},
        ]
        for mutation in mutations:
            manifest = {**copy.deepcopy(MANIFEST), **mutation}
            with self.subTest(mutation=mutation), patch.object(
                release, "download",
                side_effect=[registry(version_entry()), archive_bytes(manifest)],
            ):
                status, output, error = self.run_check("verify")
            self.assertEqual(status, 1)
            self.assertEqual(output, "")
            self.assertIn("check failed", error)

    def test_verify_refuses_missing_yanked_or_untrusted_download(self):
        for response in (
            registry(), registry(version_entry(yanked_at="2026-01-01T00:00:00Z")),
            registry(version_entry(url="https://example.com/archive.zip")),
            registry(version_entry(url=ARTIFACT_URL.replace("https:", "http:"))),
            registry(version_entry(url=ARTIFACT_URL + "?redirect=other")),
            registry(version_entry(url="https://[invalid/archive.zip")),
        ):
            with self.subTest(response=response), patch.object(
                release, "download", return_value=response
            ) as read:
                status, output, error = self.run_check("verify")
            self.assertEqual(status, 1)
            self.assertEqual(output, "")
            self.assertIn("check failed", error)
            self.assertEqual(read.call_count, 1)

    def test_verify_requires_full_dispatch_sha_before_network(self):
        for sha in ("main", "", "a" * 7):
            with self.subTest(sha=sha), patch.object(release, "download") as read:
                status, output, error = self.run_check("verify", commit_sha=sha)
            self.assertEqual(status, 1)
            self.assertEqual(output, "")
            self.assertIn("dispatch commit SHA", error)
            read.assert_not_called()

    def test_archive_is_bounded_and_requires_root_manifest(self):
        archives = [
            b"not a zip",
            archive_bytes(filename="../idf_component.yml"),
            archive_bytes(b"x" * (release.MAX_MANIFEST_BYTES + 1)),
            archive_bytes(b"["),
        ]
        for data in archives:
            with self.subTest(length=len(data)), patch.object(
                release, "download", side_effect=[registry(version_entry()), data]
            ):
                status, output, error = self.run_check("verify")
            self.assertEqual(status, 1)
            self.assertEqual(output, "")
            self.assertIn("check failed", error)

    def test_network_reads_at_most_limit_plus_one(self):
        response = io.BytesIO(b"x" * (release.MAX_JSON_BYTES + 2))
        response.status = 200
        with patch(
            "urllib.request.OpenerDirector.open", return_value=response
        ), patch.object(response, "read", wraps=response.read) as read:
            status, output, error = self.run_check()
        self.assertEqual(status, 1)
        self.assertEqual(output, "")
        self.assertIn("exceeds the read limit", error)
        read.assert_called_once_with(release.MAX_JSON_BYTES + 1)


if __name__ == "__main__":
    unittest.main()
