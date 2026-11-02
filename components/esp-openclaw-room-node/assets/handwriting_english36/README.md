# English character classifier

`../handwriting_english36.bin` is a newly trained, fixed-label classifier for
individual English letters and digits. It returns three candidates from
`0123456789abcdefghijklmnopqrstuvwxyz`. The caller must require explicit
candidate acceptance before using a character, and provide its own uppercase
choice. This is not cursive or word recognition. This core has no input UI or
message sender; native pad integration is separate work.

The 106,288-byte model has SHA-256
`485e9a768457ed132ca46ea30d94375a0f4596d9558c8aa80f59b096c83049c8`.
It uses a 784 → 128 ReLU → 36 multilayer perceptron with per-output-row int8
weights and float32 scales/biases. The production implementation is
[`room_handwriting.c`](../../room_handwriting.c); no ML runtime is required.

## Source and rights

Training used EMNIST Balanced, from Gregory Cohen, Saeed Afshar, Jonathan Tapson,
and André van Schaik (2017), *Extended MNIST (EMNIST) dataset*, Western Sydney
University, [dataset DOI](https://doi.org/10.26183/zn7s-gh79).
The [publisher record](https://research-data.westernsydney.edu.au/published/2d9cc060519411ecb15399911543e199/)
states **Copyright Western Sydney University** and
[CC BY-ND 4.0](https://creativecommons.org/licenses/by-nd/4.0/).
The conversion and dataset are described in
[EMNIST: an extension of MNIST to handwritten letters](https://arxiv.org/abs/1702.05373v2).

Source pixels and modified training images are not distributed here. The
dataset is not relicensed under the repository license. This artifact contains
new statistical parameters and produces only 36 labels: it cannot retrieve or
generate a source image. Treating these parameters separately from sharing
protected images or adaptations is an inference about this artifact, not a
general license conclusion for trained models. The
[CC training guidance](https://creativecommons.org/using-cc-licensed-works-for-ai-training-2/)
distinguishes its conservative training advice from its analysis of when license
conditions apply; see also [BY-ND section 2(a)(1) and section 4](https://creativecommons.org/licenses/by-nd/4.0/legalcode.en).
Do not add source images to fixtures, screenshots, or this directory.

The [NIST source page](https://www.nist.gov/itl/products-and-services/emnist-dataset)
identifies the upstream dataset. The acquired data came from the published
[Neuromatch Academy tutorial](https://deeplearning.neuromatch.io/tutorials/W2D2_ConvnetsAndDlThinking/student/W2D2_Tutorial1.html)
and its [OSF mirror](https://osf.io/download/xwfaj/).
[`source.json`](source.json) records the four acquired gzip members, their
verified sizes/SHA-256 hashes and ZIP CRCs, and the publisher's archive metadata.
Only those members were retrieved with HTTP Range under a pinned ETag. The full
archive hash was **not verified**. The mirror is a repack; its archive hash is
not the original NIST archive hash.

## Training and validation

Training transposes raw IDX pixels once into upright row-major images. Runtime
strokes are already upright and receive no transpose or mirror. Both paths use
the production normalizer: ink threshold 32, aspect-preserving bilinear resize
to a 20-pixel maximum extent, and center-of-mass alignment in 28 × 28 pixels.
Separate pen strokes stay separate.

Balanced IDs 0–35 map directly to the output alphabet. IDs 36–46 map to
`a b d e f g h n q r t`; case folding doubles those classes. Inverse-frequency
loss weights account for that imbalance. SHA-256 groups of normalized images
split the training source into 107,302 training and 5,498 validation examples
before augmentation. Exact normalized train/test overlaps were removed before
training (zero found). This is not a writer-disjoint split.

One deterministic CPU run used seed 20261102, two threads, PyTorch 2.10.0,
NumPy 2.4.2, 18 epochs and small training-only translations. Validation macro
recall selected epoch 17. All training choices and exported weights were frozen
before reading the official test labels. No test-guided retuning followed.

On 18,800 official test examples, the quantized model achieved:

| Measure | Result |
| --- | ---: |
| Top-1 accuracy | 82.65% |
| Top-3 accuracy | 96.45% |
| Macro recall | 82.98% |
| Lowest per-class top-3 recall (`g`) | 92.38% |

[`metrics.json`](metrics.json) includes every class and epoch. Ambiguous
`l`/`i`/`1` and `o`/`0` remain separate candidates. These benchmark results do
not measure finger input on a device.

The production C implementation matched all 18,800 reference normalized images
exactly. Quantized C/reference logits differed by at most 0.00001526, with zero
top-1 or ordered top-3 disagreements. Host inference measured 0.00575 ms per
character; training took 14.4 seconds with peak RSS 1,088,552,960 bytes on the
training host. These are host measurements, not ESP measurements.

Embedding the model consumes 106,288 flash bytes; a caller's full 512-point array
consumes 3,072 bytes. Host compiler static frame sizes total at most 2,848 bytes along the
recognize → raster → normalize call chain, plus its caller. This excludes LVGL
objects/draw tasks and does not qualify device stack or heap use. ESP latency,
stack/heap under load, and physical finger accuracy require Studio testing.

Blank input, taps, very small strokes, and dense ink floods return distinct
results for the caller to explain. This is not general detection of unknown
symbols. The core rejects more than 512 points. A caller must expose overflow
instead of submitting a silently truncated stroke, and require explicit
candidate selection and acceptance.

## Reproduce

Acquire the four unmodified gzip members named in `source.json` into an external
directory. The script checks their recorded hashes before training. Keep the
dataset and outputs outside the checkout. Compile the production preprocessor
and inference implementation, then run the pinned environment:

```sh
HANDWRITING_TMP=$(mktemp -d)
cc -std=c11 -O2 -shared -fPIC \
  components/esp-openclaw-room-node/room_handwriting.c -lm \
  -o "$HANDWRITING_TMP/handwriting.so"
uv run --isolated --no-project --with torch==2.10.0 --with numpy==2.4.2 \
  python scripts/handwriting/train.py --dataset-dir /path/to/emnist \
  --library "$HANDWRITING_TMP/handwriting.so" --output-dir "$HANDWRITING_TMP/output"
```

The recipe exports parameters, per-class metrics and actual C/reference parity.
It never downloads data or publishes examples. Platform/library versions can
affect floating-point training results; compare the recorded model hash and
metrics rather than assuming bit identity on another host. Runtime inference
does not require Python, PyTorch or NumPy.

## Parameter format

All multi-byte values are little-endian. Weights are signed int8, with float32
scales and biases. Matrices are output-row-major. The first layer accumulates
integer pixel values before multiplying by its row scale / 255; the second
layer uses the ReLU hidden values and its row scale. Candidate ranking uses
logits directly; no confidence score is presented.

| Offset | Bytes | Value |
| --- | ---: | --- |
| 0 | 8 | `OCHW01\0\0` |
| 8 | 8 | uint16 dimensions 784, 128, 36; reserved 0 |
| 16 | 100352 | First-layer weights (128 × 784) |
| 100368 | 512 | First-layer scales |
| 100880 | 512 | First-layer biases |
| 101392 | 4608 | Second-layer weights (36 × 128) |
| 106000 | 144 | Second-layer scales |
| 106144 | 144 | Second-layer biases |
