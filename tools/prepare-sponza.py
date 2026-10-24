"""Prepare the stackless triangle BVH and GPU triangles of Sponza for TGScreenGI.

Input is exported by prepare_sponza.gd; no lighting is baked into these files.
"""
import argparse
import json
import time
from pathlib import Path

import numpy as np
from triangle_bvh import build_bvh

ROOT = Path(__file__).resolve().parents[1] / "generated/sponza"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bvh", choices=("median", "sah"), default="sah")
    args = parser.parse_args()
    started = time.monotonic()
    metadata = json.loads((ROOT / "input.json").read_text())
    triangles = np.fromfile(ROOT / "input.bin", dtype="<f4").reshape(-1, 9, 4)
    node_data, ordered, bvh_stats = build_bvh(triangles, args.bvh)
    node_data.tofile(ROOT / "bvh.bin")
    # Corner positions (w: material id) and texture UVs: the 96-byte triangle TGScreenGI reads.
    gpu_triangles = triangles[np.asarray(ordered)][:, [0, 1, 2, 6, 7, 8]]
    gpu_triangles.tofile(ROOT / "triangles.bin")
    metadata.update(bvh_nodes=len(node_data), bvh_build=bvh_stats, preprocessing_seconds=time.monotonic()-started)
    (ROOT / "cache.json").write_text(json.dumps(metadata, indent=2))
    print(json.dumps({key: metadata[key] for key in ("triangles", "bvh_nodes", "preprocessing_seconds")}), flush=True)


if __name__ == "__main__":
    main()
