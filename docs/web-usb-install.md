# Browser USB installation

The OpenClaw device installer uses `web-flash/manifest.json` on this repository's
[existing Pages site](https://openclaw.github.io/esp-openclaw-node/) to select
firmware. It writes the board over Web Serial in a supported desktop browser;
no ESP-IDF installation is required. iPhone browsers cannot use this USB flow.
After flashing, provision Wi-Fi and the Gateway using the selected example's
USB console instructions. Flash completion does not mean the node is paired.

## Initial board profiles

- ESP32-S3 development board: the generic `esp32-node` image; native USB
  Serial/JTAG and sufficient flash are required. No board-specific screen or
  peripherals are initialized.
- ESP-BOX-3: its display image, 16 MB flash and octal PSRAM.
- M5Stack Dial: its dedicated image and 8 MB flash. Other M5Stack models cannot
  use this image merely because they contain the same chip.

These profiles have CI build qualification. This does not claim physical USB,
board startup, display, Wi-Fi, or Gateway pairing qualification. The board
picker must show that distinction. Other source-supported examples remain
available through their documented firmware download/build paths; they are
not implicitly eligible for this initial web installer.

## Stage or publish firmware

The **Stage web USB firmware** manual workflow runs only from `main`.
Its version input selects an existing stable release; blank selects GitHub's
latest stable release. It verifies the annotated SSH-signed tag using the same
allowed signers as the component release workflow, then selects a successful
`main` push CI run on that exact source commit. Missing or expired CI artifacts
stop staging; the workflow never substitutes another commit or PR build.

The default `publish=false` produces a downloadable preview artifact containing
`web-flash/manifest.json` and versioned firmware bundles. It does not publish a
release, deploy Pages, or flash a device. Serve the extracted artifact over a
local HTTP server for installer development, or map the fixed manifest/image
requests to it in a browser test.

With a separately authorized `publish=true` dispatch, the publish job adds only
`web-flash/` on `gh-pages`, retaining the existing Launchpad index, configuration,
and historical firmware versions. It refuses version rollback and mutation of
already published versioned bytes, requests a build through the existing Pages
API, and verifies public manifest/image checksums and CORS. A failed public
verification is a failed deployment, even if the branch push succeeded. This
workflow does not change Pages settings or create a new firmware release.

The public endpoint is intended to be:

```text
https://openclaw.github.io/esp-openclaw-node/web-flash/manifest.json
```

The endpoint does not exist until an authorized publish completes. The installer
must show firmware unavailable rather than silently using the older Launchpad
catalog or a source archive. The initial default is the latest published stable
release, not the latest commit on `main`. Publishing a new component release and
promoting it into this installer are separate, explicit operations.

## Manifest contract

`schemaVersion: 1` has a stable release `version`, source `repository` and exact
`commit`, and a `boards` array. Each board declares its `id`, `name`,
`chipFamily`, actual generated `flashSize`, `flashMode`, and `flashFrequency`,
plus `documentationUrl`, `requirements`, and `qualification: "ci-build"`.

Each `parts` entry has a version-prefixed relative `path`, byte `offset`, byte
`size`, and SHA-256 `sha256`. These are the original sparse images and offsets
from ESP-IDF's flash map, including any additional model partitions. The staging
script checks provenance, every bundle checksum, flash capacity, aligned erase
regions, encryption flags, and supported reset/stub settings before emitting a
catalog. It does not merge images across unallocated gaps or request a full
chip erase. The browser verifies downloaded bytes before writing, validates the
detected chip/flash capacity, and reports device-side verification separately.

The copied bundle retains its original `manifest.json`, `SHA256SUMS`, flash
arguments, and sanitized provenance. Hashes detect corruption, not independent
authenticity: the fixed HTTPS source, signed release identity and trusted CI
workflow form the distribution boundary.
