# Example firmware public CA bundle

The maintained board examples select `public-roots.pem` through ESP-IDF's existing
custom certificate bundle support. ESP-IDF still generates the compact bundle and
owns TLS verification. The component's `tls_cert_pem`, `use_cert_bundle`, and
caller-selected SDK bundle settings are unchanged.

This data starts with the FULL public roots from ESP-IDF v5.5.5
(`b774170ff46c393eeb5e495ea37936038d3f4f4f`) and the selected Tab5 SDK
(`362a1776ec212788fda95f75b733bfdde3a0c394`). Both SDKs have identical inputs:

| Input | SHA256 |
| --- | --- |
| `cacrt_all.pem` | `50a6277ec69113f00c5fd45f09e8b97a4b3e32daa35d3a95ab30137a55386cef` |
| `cacrt_local.pem` | `98154a7a09134484dc889e725b95a030d423b7b09cb0b3d219ac3fcc8e07f29e` |

Following [Debian DSA-6572-1](https://lists.debian.org/debian-security-announce/2026/msg00487.html),
it removes only these certificate identities:

| Root | Certificate SHA256 |
| --- | --- |
| AAA Certificate Services | `d7a7a0fb5d7e2731d771e9484ebcdef71d5f0c3e0a2948782bc83ee0ea699ef4` |
| emSign ECC Root CA - G3 | `86a1ecba089c4a8d3bbe2734c612ba341d813e043cf9e8a862cd5c57a36bbe6b` |

The other 148 certificates retain their DER, PEM bytes and ordering. No unrelated
roots are added or removed. Legitimate endpoints relying exclusively on either
removed root also lose trust. The data retains its upstream Mozilla/curl
provenance; Mozilla's source root-store data is distributed under
[MPL 2.0](https://www.mozilla.org/MPL/2.0/).

Regenerate after reviewing the selected SDK's public root changes:

```sh
python3 scripts/update_certificate_bundle.py --idf-path "$IDF_PATH" --output certificates/public-roots.pem
```

The script only derives PEM data; ESP-IDF's `gen_crt_bundle.py` performs the normal
firmware bundle generation during the build. It does not download data or change
the SDK. Do not add deprecated roots back through the SDK default bundle.

Existing `sdkconfig` files take precedence over `sdkconfig.defaults`. For an
existing example checkout, select `MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_NONE`, enable
`MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE`, and set its path to
`../../certificates/public-roots.pem`, then rebuild and upgrade the device.
New example configurations use those settings by default.

External component consumers must update their own selected CA data or SDK; this
example configuration does not change their FULL, COMMON, custom or explicit PEM
trust. In particular, do not replace a COMMON subset with this FULL-derived file.
Keep private roots through `tls_cert_pem` where appropriate. If an application
needs public and private roots in its custom bundle, concatenate this public PEM
with its existing private roots and select the combined file using the SDK's
existing custom bundle path. An explicitly restored removed root changes the
application's trust policy and can restore affected certificate paths.
