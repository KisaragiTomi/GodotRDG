"""Check fog reconstruction against a denser GPU volume and motion captures."""

import json
from pathlib import Path

import numpy as np
from PIL import Image


ROOT = Path(__file__).resolve().parents[1] / "artifacts/atmosphere-quality"


def check_scene(name):
    folder = ROOT / name
    runtime = json.loads((folder / "runtime.json").read_text(encoding="utf-8"))
    images = {}
    for label in ("before", "after", "reference", "restored", "moving",
                  "motion_settled", "light_off_fast", "light_off_settled"):
        with Image.open(folder / f"{label}.png") as source:
            images[label] = np.asarray(source.convert("RGB"), dtype=np.float32) / 255.0
    assert len({image.shape for image in images.values()}) == 1, "Capture dimensions differ"

    def difference(a, b):
        return float(np.abs(images[a] - images[b]).mean())

    before = difference("before", "reference")
    after = difference("after", "reference")
    restored = difference("after", "restored")
    motion = difference("moving", "motion_settled")
    light_off = difference("light_off_fast", "light_off_settled")
    result = {
        "before_reference_mae": before,
        "after_reference_mae": after,
        "error_reduction": 1.0 - after / max(before, 1e-8),
        "restore_mae": restored,
        "motion_settle_mae": motion,
        "six_frame_light_off_mae": light_off,
        "motion_reprojected": runtime["motion_reprojected"],
    }
    # The denser volume is a reconstruction reference, not path-traced truth.
    # Image-space tolerances include viewport TAA and stochastic surface GI.
    result["passed"] = (runtime["passed"] and after < before * 0.45 and after < 0.006
                        and restored < 0.005 and motion < 0.012 and light_off < 0.008)
    (folder / "quality.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


if __name__ == "__main__":
    results = {name: check_scene(name) for name in ("sponza", "courtyard")}
    print(json.dumps(results, indent=2))
    raise SystemExit(0 if all(result["passed"] for result in results.values()) else 1)
