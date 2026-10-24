"""Compare sampling quality without accepting loss of distant fog coverage."""
import json
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1] / "artifacts/city-fog"


def image(label):
    with Image.open(ROOT / (label + ".png")) as source:
        return np.asarray(source.convert("RGB"), dtype=np.float32) / 255.0


def mae(a, b):
    return float(np.abs(image(a) - image(b)).mean())


if __name__ == "__main__":
    runtime = json.loads((ROOT / "results.json").read_text(encoding="utf-8"))
    result = {"gpu": runtime["gpu"], "runtime_passed": runtime["passed"], "views": {}}
    for view in (0, 1, 2):
        result["views"][str(view)] = {
            "high_reference_mae": mae(f"{view}_high", f"{view}_high_reference"),
            "balanced_reference_mae": mae(f"{view}_balanced", f"{view}_balanced_reference"),
            "matched_reference_mae": mae(f"{view}_high_reference", f"{view}_balanced_reference"),
        }
    result.update(zero_off_mae=mae("zero", "off"),
                  restored_mae=mae("restored", "returned"), motion_mae=mae("moving", "moving_settled"))
    result["passed"] = (runtime["passed"]
                        and result["zero_off_mae"] < 0.001 and result["restored_mae"] < 0.005
                        and result["motion_mae"] < 0.015
                        and all(v["balanced_reference_mae"] < 0.006 and v["matched_reference_mae"] < 0.003 for v in result["views"].values()))
    (ROOT / "comparison.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["passed"] else 1)
