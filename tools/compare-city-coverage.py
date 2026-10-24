"""Only score pixels where the full-range reference has a visible fog signal.

An all-image error can hide a completely clear distant city against a large sky.
"""
import json
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1] / "artifacts/city-coverage"


def image(folder, label):
    with Image.open(ROOT / folder / (label + ".png")) as source:
        return np.asarray(source.convert("RGB"), dtype=np.float32) / 255.0


if __name__ == "__main__":
    result = {"views": {}, "passed": True}
    for index in range(4):
        label = f"view_{index}"
        reference, clear = image("after", label + "_reference"), image("after", label + "_clear")
        signal = np.abs(reference - clear).mean(axis=2)
        mask = signal > 0.03
        expected = float(signal[mask].mean())
        after = image("after", label)
        error = float(np.abs(after - reference).mean(axis=2)[mask].mean())
        actual_signal = float(np.abs(after - clear).mean(axis=2)[mask].mean())
        case = {"fog_pixels": int(mask.sum()), "reference_fog_signal": expected,
                "relative_reference_error": error / max(expected, 1e-8),
                "fog_signal_ratio": actual_signal / max(expected, 1e-8)}
        if (ROOT / "before" / (label + ".png")).exists():
            old = image("before", label)
            case["before_relative_error"] = float(np.abs(old - reference).mean(axis=2)[mask].mean()) / max(expected, 1e-8)
        result["passed"] &= bool(mask.sum() > 1000 and case["relative_reference_error"] < 0.10 and 0.9 < case["fog_signal_ratio"] < 1.1)
        result["views"][label] = case
    (ROOT / "comparison.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["passed"] else 1)
