# Component releases

The registry upload workflow is manual. A push to `main` does not publish a
component. Release authority and a separately reviewed version change are
prerequisites; this workflow does not change the component version.

Pull requests changing the release workflow, checker, tests, or this guide run
the focused host tests with read-only repository access and no OIDC permission.
Manual releases must pass the same tests before the upload job can run.

## Release an approved version

1. Review the intended `main` commit, component manifest, and release notes.
   `components/esp-openclaw-node/idf_component.yml` must contain the intended
   string version, this repository, and `repository_info.path:
   components/esp-openclaw-node`.
2. Dispatch **Release esp-openclaw-node to IDF Component Registry** on `main`.
   Supply `expected_version` exactly as written in the manifest. Dispatches on
   other refs do not run the upload job.
3. The workflow checks out the immutable dispatch SHA, not a moving `main`
   reference. A public registry read must succeed, identify the component, and
   show that the version does not already exist. Existing versions, including
   yanked versions, are refused. Network errors and malformed responses are not
   treated as absence.
4. The existing `espressif/upload-components-ci-action@v2` uploads through
   GitHub OIDC. No stored registry token is required. The repository URL and
   dispatch commit SHA are supplied explicitly.
5. The final read-only check downloads the registered ZIP and validates its
   root `idf_component.yml`: version, repository, component path, and
   `repository_info.commit_sha` must match the intended source identity.
   Success is reported as **registry artifact verified**.

The upstream uploader uses `--allow-existing`. The preflight reduces accidental
reuse, but another publisher can race between the check and upload. Verification
therefore describes the registered artifact, never claims that this run created
it, and fails if the embedded identity differs. Repository aliases predating the
move to `openclaw` are not accepted as current release identity.

Release runs are serialized and have a ten-minute job timeout. Public reads use
15-second socket timeouts, reject redirects, and cap responses at 1 MiB for
registry JSON and 16 MiB for ZIPs. Only one root manifest is read, up to 64 KiB,
from an archive with at most 4096 entries; archive paths are never extracted.
The checker uses the registry-provided HTTPS download URL on
`components-file.espressif.com`, not a guessed version URL.

## Diagnose without uploading

From the repository root, run the checker in an isolated environment. PyYAML
6.0.3 is a CI/checker-only dependency, not a firmware dependency. The workflow
uses its own temporary Python virtual environment; these local commands use
`uv` without modifying the system Python installation.

```sh
uv run --no-project --with PyYAML==6.0.3 python scripts/check_component_release.py \
  preflight --expected-version 1.0.0

uv run --no-project --with PyYAML==6.0.3 python scripts/check_component_release.py \
  verify --expected-version '<approved-version>' --commit-sha '<full-dispatch-sha>'

uv run --no-project --with PyYAML==6.0.3 python -m unittest discover \
  -s scripts/tests -p 'test_check_component_release.py'
```

The `1.0.0` preflight is expected to refuse the existing registry version. Both
checker modes perform only public reads. Neither dispatches an upload, requests
OIDC credentials, changes manifests, or writes an extracted archive.

If upload succeeds but verification fails, preserve the run and dispatch SHA,
inspect the reported version, and repeat only the read-only `verify` command
after any registry propagation delay. Do not rerun publication to repair a
verification error or attempt to overwrite an existing version. A matching
embedded identity is not a substitute for clean consumer builds, firmware
qualification, or provenance verification beyond these manifest fields.
