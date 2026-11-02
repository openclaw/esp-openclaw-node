#!/usr/bin/env python3
"""Reproduce the English character classifier from separately acquired EMNIST data."""

import argparse
import ctypes
import gzip
import hashlib
import json
from pathlib import Path
import resource
import struct
import sys
import time

import numpy as np
import torch
from torch import nn


ALPHABET = "0123456789abcdefghijklmnopqrstuvwxyz"
SEED = 20261102
FOLD = np.array(list(range(36)) + [10, 11, 13, 14, 15, 16, 17, 23, 26, 27, 29])


def images(directory, split, count, library):
    data = gzip.decompress((directory / f"emnist-balanced-{split}-images-idx3-ubyte.gz").read_bytes())
    assert struct.unpack(">4I", data[:16]) == (2051, count, 28, 28)
    assert len(data) == 16 + count * 784
    # IDX is transposed once. Runtime strokes are already upright, row-major.
    raw = np.frombuffer(data, dtype=np.uint8, offset=16).reshape(count, 28, 28).transpose(0, 2, 1).copy()
    result = np.empty_like(raw)
    for source, target in zip(raw, result):
        library.room_handwriting_normalize(source.ctypes.data, target.ctypes.data)
    return result.reshape(count, 784)


def labels(directory, split, count):
    data = gzip.decompress((directory / f"emnist-balanced-{split}-labels-idx1-ubyte.gz").read_bytes())
    assert struct.unpack(">2I", data[:8]) == (2049, count)
    assert len(data) == 8 + count
    return FOLD[np.frombuffer(data, dtype=np.uint8, offset=8)]


def evaluate(model, pixels, labels):
    model.eval()
    correct = np.zeros(36, dtype=np.int64)
    correct_three = np.zeros(36, dtype=np.int64)
    totals = np.bincount(labels.numpy(), minlength=36)
    logits = []
    with torch.inference_mode():
        for start in range(0, len(pixels), 1024):
            output = model(pixels[start:start + 1024])
            actual = labels[start:start + 1024]
            correct += np.bincount(actual[output.argmax(1) == actual].numpy(), minlength=36)
            found = (output.topk(3).indices == actual[:, None]).any(1)
            correct_three += np.bincount(actual[found].numpy(), minlength=36)
            logits.append(output)
    return {
        "top1": float(correct.sum() / len(pixels)),
        "top3": float(correct_three.sum() / len(pixels)),
        "macro_recall": float((correct / totals).mean()),
        "per_class_recall": dict(zip(ALPHABET, map(float, correct / totals))),
        "per_class_top3": dict(zip(ALPHABET, map(float, correct_three / totals))),
    }, torch.cat(logits)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--library", type=Path, required=True, help="Compiled production room_handwriting.c")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parents[2] / "components/esp-openclaw-room-node/assets/handwriting_english36/source.json"
    for member in json.loads(source.read_text())["members"]:
        data = (args.dataset_dir / Path(member["name"]).name).read_bytes()
        assert len(data) == member["size"] and hashlib.sha256(data).hexdigest() == member["sha256"]
    start = time.monotonic()
    np.random.seed(SEED)
    torch.manual_seed(SEED)
    torch.set_num_threads(2)
    torch.set_num_interop_threads(1)
    torch.use_deterministic_algorithms(True)
    library = ctypes.CDLL(str(args.library.resolve()))
    library.room_handwriting_normalize.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    library.room_handwriting_logits.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p]
    library.room_handwriting_logits.restype = ctypes.c_bool
    pixels = images(args.dataset_dir, "train", 112800, library)
    test_pixels = images(args.dataset_dir, "test", 18800, library)
    actual = labels(args.dataset_dir, "train", 112800)
    hashes = [hashlib.sha256(row.tobytes()).digest() for row in pixels]
    test_hashes = {hashlib.sha256(row.tobytes()).digest() for row in test_pixels}
    clean = np.array([value not in test_hashes for value in hashes])
    validation = np.array([int.from_bytes(value[:8], "little") % 20 == 0 for value in hashes]) & clean
    training = clean & ~validation
    # Exact duplicates stay in one split; no writer-disjoint split is claimed.
    train_x = torch.from_numpy(pixels[training].copy()).float().div_(255)
    train_y = torch.from_numpy(actual[training].copy())
    val_x = torch.from_numpy(pixels[validation].copy()).float().div_(255)
    val_y = torch.from_numpy(actual[validation].copy())
    counts = np.bincount(actual[training], minlength=36)
    weights = torch.tensor(1 / counts, dtype=torch.float32)
    weights /= weights.mean()
    model = nn.Sequential(nn.Linear(784, 128), nn.ReLU(), nn.Dropout(0.1), nn.Linear(128, 36))
    optimizer = torch.optim.AdamW(model.parameters(), lr=0.001, weight_decay=0.0001)
    loss_fn = nn.CrossEntropyLoss(weight=weights)
    best = -1
    epochs = []
    checkpoint = args.output_dir / "best.pt"
    for epoch in range(18):
        model.train()
        loss_total = 0
        for indices in torch.randperm(len(train_x)).split(512):
            batch = train_x[indices]
            if epoch % 2:
                shift = (int(torch.randint(-1, 2, ()).item()), int(torch.randint(-1, 2, ()).item()))
                batch = torch.roll(batch.reshape(-1, 28, 28), shift, (1, 2)).reshape(-1, 784)
            optimizer.zero_grad(set_to_none=True)
            loss = loss_fn(model(batch), train_y[indices])
            loss.backward()
            optimizer.step()
            loss_total += loss.item() * len(indices)
        metrics, _ = evaluate(model, val_x, val_y)
        epochs.append({"epoch": epoch + 1, "loss": loss_total / len(train_x),
                       **{key: metrics[key] for key in ("top1", "top3", "macro_recall")}})
        print(epochs[-1], flush=True)
        if metrics["macro_recall"] > best:
            best = metrics["macro_recall"]
            best_epoch = epoch + 1
            torch.save(model.state_dict(), checkpoint)
        if epoch in (11, 15):
            for group in optimizer.param_groups:
                group["lr"] *= 0.3
    model.load_state_dict(torch.load(checkpoint, weights_only=True))
    validation_metrics, _ = evaluate(model, val_x, val_y)
    # Choices and weights are frozen before reading the official test labels.
    payload = b"OCHW01\0\0" + struct.pack("<4H", 784, 128, 36, 0)
    for layer in (model[0], model[3]):
        weights = layer.weight.detach().numpy()
        scale = np.maximum(np.abs(weights).max(axis=1) / 127, 1e-9).astype("<f4")
        quantized = np.rint(weights / scale[:, None]).clip(-127, 127).astype(np.int8)
        payload += quantized.tobytes() + scale.tobytes() + layer.bias.detach().numpy().astype("<f4").tobytes()
        with torch.no_grad():
            layer.weight.copy_(torch.from_numpy(quantized.astype(np.float32) * scale[:, None]))
    (args.output_dir / "handwriting_english36.bin").write_bytes(payload)
    test_y = torch.from_numpy(labels(args.dataset_dir, "test", 18800).copy())
    test_metrics, reference = evaluate(model, torch.from_numpy(test_pixels.copy()).float().div_(255), test_y)
    actual_logits = np.empty_like(reference.numpy())
    data = (ctypes.c_uint8 * len(payload)).from_buffer_copy(payload)
    for row, output in zip(test_pixels, actual_logits):
        assert library.room_handwriting_logits(data, len(data), row.ctypes.data, output.ctypes.data)
    parity = {
        "samples": len(test_pixels),
        "max_absolute_logit_error": float(np.max(np.abs(actual_logits - reference.numpy()))),
        "top1_mismatches": int(np.count_nonzero(actual_logits.argmax(1) != reference.numpy().argmax(1))),
        "top3_order_mismatches": int(np.any(np.argsort(-actual_logits, axis=1)[:, :3] !=
                                          np.argsort(-reference.numpy(), axis=1)[:, :3], axis=1).sum()),
    }
    assert parity["top1_mismatches"] == parity["top3_order_mismatches"] == 0
    result = {
        "seed": SEED, "torch": torch.__version__, "numpy": np.__version__, "threads": 2,
        "training_count": int(training.sum()), "validation_count": int(validation.sum()),
        "normalized_train_test_duplicate_rows_removed": int((~clean).sum()),
        "best_epoch": best_epoch, "validation_float": validation_metrics,
        "test_quantized": test_metrics, "production_c_parity": parity, "epochs": epochs,
        "model_bytes": len(payload), "model_sha256": hashlib.sha256(payload).hexdigest(),
        "wall_seconds": time.monotonic() - start,
        "peak_rss_bytes": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * (1 if sys.platform == "darwin" else 1024),
    }
    (args.output_dir / "metrics.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: result[key] for key in ("model_sha256", "test_quantized", "production_c_parity")}), flush=True)


if __name__ == "__main__":
    main()
