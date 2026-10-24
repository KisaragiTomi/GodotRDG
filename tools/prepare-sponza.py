"""Prepare world-space mesh surfels and a stackless triangle BVH for Sponza.

Input is exported by prepare_sponza.gd; no lighting is baked into these files.
"""
import argparse
import json
import time
from pathlib import Path

import numpy as np
from PIL import Image
from scipy.ndimage import distance_transform_edt
from triangle_bvh import build_bvh

ROOT = Path(__file__).resolve().parents[1] / "generated/sponza"


def linear(rgb):
    return np.where(rgb <= 0.04045, rgb / 12.92, ((rgb + 0.055) / 1.055) ** 2.4)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bvh", choices=("median", "sah"), default="sah")
    args = parser.parse_args()
    started = time.monotonic()
    metadata = json.loads((ROOT / "input.json").read_text())
    triangles = np.fromfile(ROOT / "input.bin", dtype="<f4").reshape(-1, 9, 4)
    width, height = metadata["atlas"]
    owner = np.full((height, width), -1, dtype=np.int32)
    bary = np.zeros((height, width, 3), dtype=np.float32)
    # Rasterize only UV chart interiors. Padding is filled from valid chart texels.
    for index, triangle in enumerate(triangles):
        uv = triangle[6:9, 2:4] * (width, height) - 0.5
        low = np.maximum(np.ceil(uv.min(axis=0)).astype(int), 0)
        high = np.minimum(np.floor(uv.max(axis=0)).astype(int), (width - 1, height - 1))
        if np.any(high < low):
            continue
        ab, ac = uv[1] - uv[0], uv[2] - uv[0]
        determinant = ab[0] * ac[1] - ab[1] * ac[0]
        if abs(determinant) < 1e-8:
            continue
        yy, xx = np.mgrid[low[1]:high[1]+1, low[0]:high[0]+1]
        delta = np.stack((xx, yy), axis=-1) - uv[0]
        b = (delta[..., 0] * ac[1] - delta[..., 1] * ac[0]) / determinant
        c = (ab[0] * delta[..., 1] - ab[1] * delta[..., 0]) / determinant
        inside = (b >= -1e-5) & (c >= -1e-5) & (b + c <= 1.00001)
        x, y = xx[inside], yy[inside]
        owner[y, x] = index
        bary[y, x] = np.stack((1 - b[inside] - c[inside], b[inside], c[inside]), axis=-1)
    y, x = np.nonzero(owner >= 0)
    chosen = triangles[owner[y, x]]
    weights = bary[y, x]
    positions = np.einsum("ni,nij->nj", weights, chosen[:, :3, :3])
    normals = np.einsum("ni,nij->nj", weights, chosen[:, 3:6, :3])
    normals /= np.maximum(np.linalg.norm(normals, axis=1, keepdims=True), 1e-10)
    uv = np.einsum("ni,nij->nj", weights, chosen[:, 6:9, :2])
    material_ids = chosen[:, 0, 3].astype(int)
    albedo = np.zeros_like(positions)
    valid = np.ones(len(x), dtype=bool)
    for material_id, material in enumerate(metadata["materials"]):
        mask = material_ids == material_id
        coords = np.floor((uv[mask] % 1) * 512).astype(int).clip(0, 511)
        texture = np.asarray(Image.open(ROOT / f"albedo_{material_id}.png")) / 255.0
        pixels = texture[coords[:, 1], coords[:, 0]]
        metal_image = np.asarray(Image.open(ROOT / f"metal_{material_id}.png")) / 255.0
        metal = metal_image[coords[:, 1], coords[:, 0], material["metal_channel"]] * material["metallic"]
        albedo[mask] = linear(pixels[:, :3]) * material["color"][:3] * (1 - metal[:, None])
        valid[mask] = pixels[:, 3] * material["color"][3] >= material["alpha_cutoff"]
    # Masked foliage is absent as receiver where the material discards pixels.
    owner[y[~valid], x[~valid]] = -1
    x, y, positions, normals, albedo = [item[valid] for item in (x, y, positions, normals, albedo)]
    surfels = np.zeros((len(x), 3, 4), dtype="<f4")
    surfels[:, 0, :3] = positions
    surfels[:, 0, 3] = y * width + x
    surfels[:, 1, :3] = normals
    surfels[:, 2, :3] = albedo.clip(0, 0.99)
    surfels.tofile(ROOT / "surfels.bin")
    distance, nearest = distance_transform_edt(owner < 0, return_indices=True)
    mapping = (nearest[0] * width + nearest[1]).astype("<i4")
    mapping[distance > 4] = -1
    mapping.tofile(ROOT / "padding.bin")
    print(f"Rasterized {len(x)} surfels in {time.monotonic() - started:.2f}s", flush=True)
    node_data, ordered, bvh_stats = build_bvh(triangles, args.bvh)
    node_data.tofile(ROOT / "bvh.bin")
    gpu_triangles = triangles[np.asarray(ordered)][:, [0, 1, 2, 6, 7, 8]]
    gpu_triangles.tofile(ROOT / "triangles.bin")
    metadata.update(surfel_count=len(x), bvh_nodes=len(node_data), bvh_build=bvh_stats, preprocessing_seconds=time.monotonic()-started)
    (ROOT / "cache.json").write_text(json.dumps(metadata, indent=2))
    print(json.dumps({key: metadata[key] for key in ("triangles", "atlas", "surfel_count", "bvh_nodes", "preprocessing_seconds")}), flush=True)


if __name__ == "__main__":
    main()
