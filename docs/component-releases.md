# Component releases

`.github/workflows/upload_component.yml` owns the component registry and GitHub
Release as one tag-driven pipeline. Prepare a matching string version in
`components/esp-openclaw-node/idf_component.yml` and a dated `CHANGELOG.md`
section, merge to protected main, and require green push CI on that exact commit.
With explicit release authorization, push an annotated SSH-signed `vX.Y.Z` tag.
The workflow verifies the allowed signer, builds the component test application,
retains a checksummed source archive, refuses an existing registry version,
uploads using `IDF_COMPONENT_API_TOKEN`, verifies the registry archive's embedded
commit identity, then publishes the GitHub Release with the exact changelog notes.

Dispatch from main with `dry_run=true` (default) to validate, build and retain
the source archive without any registry upload, tag creation, or GitHub Release.
The latest dated changelog version must match the component manifest. A dispatch
cannot publish; production publication requires the signed tag push.

The upstream uploader retains its own allow-existing behavior, so the independent
preflight is mandatory and post-upload identity verification remains mandatory.
Do not retry a successful upload to repair a later failure or overwrite a version.
Use the read-only checker instead:

```sh
uv run --no-project --with PyYAML==6.0.3 python scripts/check_component_release.py \
  verify --expected-version '<version>' --commit-sha '<full-sha>' --wait-for-propagation
```

The registry publishes the core component; the checksummed GitHub source archive
also includes room examples and Talk code. Example and board qualification remain
in the exact-commit push CI matrix. This workflow does not publish firmware images
or modify an existing release or version tag.
