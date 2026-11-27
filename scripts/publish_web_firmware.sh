#!/usr/bin/env bash
set -euo pipefail

# Use the queued stage job's verified tag object, release identity and CI attempt.
# Read fresh GitHub state immediately before each remote write.
verify_release() {
  gh api "repos/$GITHUB_REPOSITORY/actions/runs/$RUN_ID/attempts/$ATTEMPT" |
    jq -e --arg repo "$GITHUB_REPOSITORY" --arg commit "$COMMIT" \
      --argjson run "$RUN_ID" --argjson attempt "$ATTEMPT" '
      .id == $run and .run_attempt == $attempt and .path == ".github/workflows/ci.yml" and
      .repository.full_name == $repo and .head_repository.full_name == $repo and
      .event == "push" and .head_branch == "main" and .head_sha == $commit and
      .status == "completed" and .conclusion == "success"' >/dev/null
  gh api "repos/$GITHUB_REPOSITORY/git/ref/tags/$VERSION" |
    jq -e --arg ref "refs/tags/$VERSION" --arg oid "$TAG_OID" '
      .ref == $ref and .object.type == "tag" and .object.sha == $oid' >/dev/null
  gh api "repos/$GITHUB_REPOSITORY/releases/tags/$VERSION" |
    jq -e --arg version "$VERSION" --argjson id "$RELEASE_ID" '
      .id == $id and .tag_name == $version and .draft == false and
      .prerelease == false and .published_at != null' >/dev/null
}

jq -e --arg version "$VERSION" --arg repo "$GITHUB_REPOSITORY" --arg commit "$COMMIT" '
  .version == $version and .source.repository == $repo and .source.commit == $commit
' "$PAYLOAD/web-flash/manifest.json" >/dev/null

gh api "repos/$GITHUB_REPOSITORY/pages" \
  --jq '.build_type == "legacy" and .source.branch == "gh-pages" and .source.path == "/"' | grep -qx true
python3 - <<'PYCODE'
import json
import os
from pathlib import Path
import shutil
incoming = Path(os.environ["PAYLOAD"]) / "web-flash"
new = json.loads((incoming / "manifest.json").read_text())
existing = Path("web-flash/manifest.json")
if existing.exists():
    old = json.loads(existing.read_text())
    version = lambda value: tuple(map(int, value.removeprefix("v").split(".")))
    if version(old["version"]) > version(new["version"]):
        raise SystemExit("Refusing to move the latest firmware backwards")
    if old["version"] == new["version"] and old != new:
        raise SystemExit("Refusing to change an already published firmware version")
for source in incoming.rglob("*"):
    if not source.is_file():
        continue
    relative = source.relative_to(incoming)
    target = Path("web-flash") / relative
    if relative.as_posix() != "manifest.json" and target.exists() and target.read_bytes() != source.read_bytes():
        raise SystemExit("Refusing to overwrite immutable firmware bytes")
# Only this subtree changes; keep the existing Launchpad index and old versions.
shutil.copytree(incoming, "web-flash", dirs_exist_ok=True)
PYCODE
git add -- web-flash
if ! git diff --cached --quiet; then
  git -c user.name=github-actions[bot] \
    -c user.email=41898282+github-actions[bot]@users.noreply.github.com \
    commit -m "chore(firmware): publish web installer $VERSION"
  verify_release
  git push origin HEAD:gh-pages
fi
# GITHUB_TOKEN branch pushes do not trigger Pages; request the existing legacy build.
verify_release
gh api --method POST "repos/$GITHUB_REPOSITORY/pages/builds" --silent
