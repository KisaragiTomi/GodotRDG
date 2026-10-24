"""Summarize ordered GPU runs and verify identical fixed-ray BVH shading."""
import json
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1] / "artifacts/bvh"


def main():
    runs = {name: json.loads((ROOT / name / "results.json").read_text(encoding="utf-8"))
            for name in ("median-run", "sah-run", "sah-repeat", "median-repeat")}
    result = {"gpu": runs["median-run"]["gpu"], "cases": {}, "passed": all(r["passed"] for r in runs.values())}
    for case in runs["median-run"]["cases"]:
        summary = {}
        for method in ("median", "sah"):
            summary[method] = {metric: float(np.mean([runs[method + suffix]["cases"][case][metric] for suffix in ("-run", "-repeat")]))
                               for metric in ("trace_ms", "total_ms", "viewport_ms")}
        summary["trace_reduction"] = 1.0 - summary["sah"]["trace_ms"] / summary["median"]["trace_ms"]
        summary["gi_reduction"] = 1.0 - summary["sah"]["total_ms"] / summary["median"]["total_ms"]
        # Images/SH are captured after locking a common ray index in BVH-only
        # mode. The screen case above measures screen tracing performance only.
        base = np.fromfile(ROOT / "median-run" / (case + ".sh"), dtype="<f2").astype(np.float32)
        differences = []
        pixel_differences = []
        for run in runs:
            sh = np.fromfile(ROOT / run / (case + ".sh"), dtype="<f2").astype(np.float32)
            differences.append(float(np.abs(base - sh).max()))
            with Image.open(ROOT / "median-run" / (case + ".png")) as original, Image.open(ROOT / run / (case + ".png")) as candidate:
                pixel_differences.append(float(np.abs(np.asarray(original, dtype=np.float32) - np.asarray(candidate, dtype=np.float32)).mean() / 255.0))
        summary["fixed_ray_sh_max_difference"] = max(differences)
        summary["fixed_ray_image_mae"] = max(pixel_differences)
        result["passed"] &= max(differences) < 0.001 and max(pixel_differences) < 0.001
        result["cases"][case] = summary
    (ROOT / "comparison.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
