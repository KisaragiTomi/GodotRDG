"""Geometry-only, stackless BVHs shared by preparation and GPU A/B runs."""

import argparse
import hashlib
import json
import time
from pathlib import Path

import numpy as np


def surface_area(lo, hi):
    extent = np.maximum(hi - lo, 0.0)
    return 2.0 * (extent[..., 0] * extent[..., 1]
                  + extent[..., 1] * extent[..., 2]
                  + extent[..., 2] * extent[..., 0])


def build_bvh(triangles, method="sah", bins=16, leaf_size=8):
    """Return GPU nodes, triangle permutation and build statistics.

    Keep the existing 48-byte node ABI: bounds, leaf range, subtree escape.
    SAH changes spatial partitioning only, not intersections or shading.
    """
    if method not in ("median", "sah") or bins < 2 or leaf_size < 1:
        raise ValueError("Invalid BVH build settings")
    if len(triangles) == 0 or not np.isfinite(triangles[:, :3, :3]).all():
        raise ValueError("BVH needs nonempty, finite triangle positions")
    started = time.monotonic()
    minimum = triangles[:, :3, :3].min(axis=1)
    maximum = triangles[:, :3, :3].max(axis=1)
    centers = (minimum + maximum) * 0.5
    nodes, ordered = [], []
    max_depth = 0

    def split_sah(indices):
        local = centers[indices]
        cmin, cmax = local.min(axis=0), local.max(axis=0)
        best_cost, best_partition = np.inf, None
        for axis in range(3):
            extent = cmax[axis] - cmin[axis]
            if extent <= 1e-8:
                continue
            bucket = np.minimum(((local[:, axis] - cmin[axis]) * (bins / extent)).astype(int), bins - 1)
            counts = np.bincount(bucket, minlength=bins)
            lo = np.full((bins, 3), np.inf, dtype=np.float32)
            hi = np.full((bins, 3), -np.inf, dtype=np.float32)
            np.minimum.at(lo, bucket, minimum[indices])
            np.maximum.at(hi, bucket, maximum[indices])
            left_count = np.cumsum(counts)[:-1]
            right_count = np.cumsum(counts[::-1])[::-1][1:]
            left_area = surface_area(np.minimum.accumulate(lo)[:-1], np.maximum.accumulate(hi)[:-1])
            right_area = surface_area(np.minimum.accumulate(lo[::-1])[::-1][1:], np.maximum.accumulate(hi[::-1])[::-1][1:])
            costs = left_area * left_count + right_area * right_count
            costs[(left_count == 0) | (right_count == 0)] = np.inf
            split = int(np.argmin(costs))
            if costs[split] < best_cost:
                best_cost, best_partition = costs[split], bucket <= split
        return best_partition

    def build(indices, depth=0):
        nonlocal max_depth
        max_depth = max(max_depth, depth)
        node_id = len(nodes)
        lo, hi = minimum[indices].min(axis=0), maximum[indices].max(axis=0)
        nodes.append(None)
        if len(indices) <= leaf_size:
            start, count = len(ordered), len(indices)
            ordered.extend(indices)
        else:
            # Bound recursion even for coincident centroids / highly skewed SAH.
            mask = split_sah(indices) if method == "sah" and depth < 48 else None
            if mask is None:
                axis = np.ptp(centers[indices], axis=0).argmax()
                split = len(indices) // 2
                partition = np.argpartition(centers[indices, axis], split)
                left, right = indices[partition[:split]], indices[partition[split:]]
            else:
                left, right = indices[mask], indices[~mask]
            build(left, depth + 1)
            build(right, depth + 1)
            start, count = 0, 0
        nodes[node_id] = (*lo, 0, *hi, 0, start, count, len(nodes), 0)

    build(np.arange(len(triangles)))
    node_data = np.asarray(nodes, dtype="<f4")
    node_data.view("<u4")[:, 8:12] = np.asarray(nodes, dtype=np.float64)[:, 8:12].astype("<u4")
    stats = {"method": method, "bins": bins if method == "sah" else 0,
             "leaf_size": leaf_size, "nodes": len(nodes), "max_depth": max_depth,
             "build_seconds": time.monotonic() - started}
    return node_data, np.asarray(ordered), stats


def main():
    parser = argparse.ArgumentParser(description="Rebuild geometry acceleration without UV export or lighting work.")
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1] / "generated/sponza")
    parser.add_argument("--output", type=Path, required=True, help="Separate directory for bvh.bin, triangles.bin and cache.json")
    parser.add_argument("--method", choices=("median", "sah"), default="sah")
    args = parser.parse_args()
    if args.output.resolve() == args.source.resolve():
        parser.error("Use a separate output directory to preserve the source baseline")
    source = args.source / "triangles.bin"
    triangles = np.fromfile(source, dtype="<f4").reshape(-1, 6, 4)
    nodes, order, stats = build_bvh(triangles, args.method)
    args.output.mkdir(parents=True, exist_ok=True)
    nodes.tofile(args.output / "bvh.bin")
    triangles[order].tofile(args.output / "triangles.bin")
    metadata = json.loads((args.source / "cache.json").read_text(encoding="utf-8"))
    metadata.update(bvh_nodes=len(nodes), bvh_build=stats,
                    source_triangle_sha256=hashlib.sha256(source.read_bytes()).hexdigest())
    (args.output / "cache.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(stats), flush=True)


if __name__ == "__main__":
    main()
