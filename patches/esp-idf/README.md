# ESP-IDF patches

## Security2 salt width

Native ESP32-S3 NimBLE/Security2 builds apply
[`security2-srp-salt-width.patch`](security2-srp-salt-width.patch) during
provisioning component configuration, before compilation. This includes the
Watch and M5Dial defaults. Remote-radio and other targets do not apply it.

The approved base is ESP-IDF v5.5.5, commit
`b774170ff46c393eeb5e495ea37936038d3f4f4f`. The SDK's
[`esp_srp.c`](https://github.com/espressif/esp-idf/blob/b774170ff46c393eeb5e495ea37936038d3f4f4f/components/protocomm/src/crypto/srp6a/esp_srp.c)
exports the random salt as a minimal-width MPI byte array. A leading zero can
therefore produce fewer bytes than the requested salt length, which callers
cannot discover through the public helper API. The patch rejects that result
before hashing or publishing it, using the existing error cleanup. Successful
generation, crypto algorithms and public APIs stay unchanged. A failed setup
preparation requires an explicit operator retry; there is no generation loop.

[`scripts/idf_srp_salt.py`](../../scripts/idf_srp_salt.py) owns the approved
base, whole-file source hashes and patch hash. It accepts a clean SDK or the
exact already-applied patch, and refuses other SDK edits, staged changes,
different revisions, changed patches and symlinked source. It never downloads
or resets an SDK. For an explicitly selected SDK:

```sh
python3 scripts/idf_srp_salt.py --idf-path /path/to/esp-idf
python3 scripts/idf_srp_salt.py --idf-path /path/to/esp-idf --verify-only
```

The Dial CI lane verifies the production patch and runs the actual SDK
`esp_srp.c` and `esp_srp_mpi.c` with its pinned mbedTLS submodule
(`9d669eadb1955d348986b9280156710aaaadf79f`). The host regression substitutes
only platform logging, allocation accounting and deterministic random input;
SHA-512, MPI export and modular exponentiation use the vendor sources.
ASan/UBSan and allocation accounting cover a full-width success and every
short salt width, including zero. An unmodified SDK fails the short-salt
rejection assertion. When given an original source cache, the runner applies
the maintained patch to a disposable source copy, leaving that cache intact:

```sh
python3 -m unittest discover -s scripts/tests -p test_idf_srp_salt.py -v
python3 scripts/tests/run_idf_srp_host_tests.py --idf-path /path/to/esp-idf
```

Firmware packaging accepts this exact verified SDK edit only for native Watch
and M5Dial Security2 configurations, recording its identity as
`idf.security2_salt_patch` in the bundle manifest. Other SDK changes remain
errors; this does not enable a general dirty-SDK override.

The optional `--mbedtls-path` selects the same pinned source when using a
minimal read-only SDK source cache. Host software crypto does not qualify
physical BLE exchanges or hardware acceleration. SDK upgrades must recheck
this defect and requalify the source/patch hashes; remove this patch when the
selected upstream implementation guarantees the requested salt width.
