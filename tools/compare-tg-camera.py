"""Compare identical camera poses against a higher-sample GI approximation.

The reference is this renderer at 32 rays per half-resolution receiver, not a
path-traced ground truth. Geometry matching excludes raster edge mismatches.
"""
import json
import sys
from pathlib import Path

import numpy as np


def load_sample(directory, meta):
    w, h = meta['width'], meta['height']
    name = meta['name']
    sh = np.fromfile(directory / f'{name}_sh.bin', '<f2').astype(np.float32).reshape(h, w * 3, 4)
    normal = np.fromfile(directory / f'{name}_normal.bin', '<f2').astype(np.float32).reshape(h, w, 4)[..., :3]
    position = np.fromfile(directory / f'{name}_position.bin', '<f4').reshape(h, w, 4)
    age = np.fromfile(directory / f'{name}_age.bin', '<f2').astype(np.float32).reshape(h, w)
    basis = np.concatenate([np.full((h, w, 1), 0.282094792), normal * 0.325735008], axis=-1)
    coefficients = np.stack(np.split(sh, 3, axis=1), axis=-2)
    incoming = np.maximum(np.einsum('...ck,...k->...c', coefficients, basis), 0.0)
    return incoming, position, normal, age


root = Path(sys.argv[1])
reports = {name: json.loads((root / name / 'results.json').read_text()) for name in ('sparse', 'quality', 'reference')}
rows = []
for i, meta in enumerate(reports['reference']['samples']):
    reference, rp, rn, ra = load_sample(root / 'reference', meta)
    sparse, sp, sn, sa = load_sample(root / 'sparse', reports['sparse']['samples'][i])
    quality, qp, qn, qa = load_sample(root / 'quality', reports['quality']['samples'][i])
    valid = (rp[..., 3] > 0) & (sp[..., 3] > 0) & (qp[..., 3] > 0)
    matched = valid & (np.linalg.norm(sp[..., :3] - rp[..., :3], axis=-1) < 0.03) & (np.linalg.norm(qp[..., :3] - rp[..., :3], axis=-1) < 0.03)
    matched &= (np.sum(sn * rn, axis=-1) > 0.97) & (np.sum(qn * rn, axis=-1) > 0.97)
    row = {'name': meta['name'], 'matched_fraction': float(matched.sum() / max(1, valid.sum()))}
    for region, mask in [('all', matched), ('young_history', matched & (qa < 8))]:
        if not mask.any():
            continue
        a = np.abs(sparse - reference)[mask]
        b = np.abs(quality - reference)[mask]
        row[region] = {'pixels': int(mask.sum()), 'sparse_mae': float(a.mean()), 'quality_mae': float(b.mean()), 'error_ratio': float(b.mean() / max(a.mean(), 1e-9))}
    rows.append(row)
motion = [r['all'] for r in rows if r['name'].startswith('pan_')]
summary = {
    'reference': '32 rays per half-resolution receiver; same denoiser and camera poses; BVH-only to isolate sampling',
    'rows': rows,
    'cold_error_ratio': rows[0]['all']['error_ratio'],
    'camera_motion_error_ratio': float(sum(r['quality_mae'] for r in motion) / sum(r['sparse_mae'] for r in motion)),
    'static_gpu_ms': {name: value['static_gpu_ms_median'] for name, value in reports.items()},
}
summary['passed'] = bool(all(v['passed'] for v in reports.values()) and all(r['matched_fraction'] > 0.8 for r in rows) and summary['cold_error_ratio'] < 0.7 and summary['camera_motion_error_ratio'] < 0.7)
(root / 'comparison.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
print(json.dumps(summary, indent=2))
sys.exit(0 if summary['passed'] else 1)
