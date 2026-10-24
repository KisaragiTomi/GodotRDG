"""Equal-ray denoiser comparison against saved, geometry-matched 32-ray GI.

The high-sample approximation uses the basic filter and is not path-traced truth.
Frame differences measure stability; reference MAE also checks smoothing bias.
"""
import json
import sys
from pathlib import Path

import numpy as np


def load(directory, meta):
    w, h, name = meta['width'], meta['height'], meta['name']
    sh = np.fromfile(directory / f'{name}_sh.bin', '<f2').astype(np.float32).reshape(h, w * 3, 4)
    normal = np.fromfile(directory / f'{name}_normal.bin', '<f2').astype(np.float32).reshape(h, w, 4)[..., :3]
    position = np.fromfile(directory / f'{name}_position.bin', '<f4').reshape(h, w, 4)
    age = np.fromfile(directory / f'{name}_age.bin', '<f2').astype(np.float32).reshape(h, w)
    basis = np.concatenate([np.full((h, w, 1), .282094792), normal * .325735008], axis=-1)
    incoming = np.maximum(np.einsum('...ck,...k->...c', np.stack(np.split(sh, 3, axis=1), axis=-2), basis), 0)
    return incoming, position, normal, age


root, reference = map(Path, sys.argv[1:3])
reports = {name: json.loads((directory / 'results.json').read_text()) for name, directory in
           [('basic', root / 'basic-camera'), ('post', root / 'post-camera'), ('reference', reference)]}
noise = {name: json.loads((root / f'{name}-noise' / 'results.json').read_text()) for name in ('basic', 'post')}
rows = []
equal_budget = True
for i, meta in enumerate(reports['reference']['samples']):
    r, rp, rn, _ = load(reference, meta)
    a, ap, an, aa = load(root / 'basic-camera', reports['basic']['samples'][i])
    b, bp, bn, ba = load(root / 'post-camera', reports['post']['samples'][i])
    valid = (rp[..., 3] > 0) & (ap[..., 3] > 0) & (bp[..., 3] > 0)
    mask = valid & (np.linalg.norm(ap[..., :3] - rp[..., :3], axis=-1) < .03) & (np.linalg.norm(bp[..., :3] - rp[..., :3], axis=-1) < .03)
    mask &= (np.sum(an * rn, axis=-1) > .97) & (np.sum(bn * rn, axis=-1) > .97)
    equal_budget &= (reports['basic']['samples'][i]['counters'][0] == reports['post']['samples'][i]['counters'][0])
    equal_budget &= bool(np.array_equal(aa, ba))
    # A band around normal/depth discontinuities checks edges separately.
    edge = np.zeros(mask.shape, dtype=bool)
    for axis in (0, 1):
        nn, pp = np.roll(rn, 1, axis), np.roll(rp, 1, axis)
        plane = np.abs(np.sum((pp[..., :3] - rp[..., :3]) * rn, axis=-1))
        edge |= (np.sum(nn * rn, axis=-1) < .85) | (plane > .03 + rp[..., 3] * .002)
    band = edge.copy()
    for axis in (0, 1):
        for shift in (-2, -1, 1, 2): band |= np.roll(edge, shift, axis)
    row = {'name': meta['name'], 'matched_fraction': float(mask.sum() / max(1, valid.sum()))}
    for name, region in [('all', mask), ('young_history', mask & (ba < 8)), ('edges', mask & band)]:
        if not region.any(): continue
        before, after = float(np.abs(a - r)[region].mean()), float(np.abs(b - r)[region].mean())
        row[name] = {'basic_mae': before, 'post_mae': after, 'ratio': after / max(before, 1e-9)}
    rows.append(row)
motion = [r['all'] for r in rows if r['name'].startswith('pan_')]
edge_motion = [r['edges'] for r in rows if r['name'].startswith('pan_')]
result = {
    'reference': str(reference), 'same_ray_counts_and_history_ages': bool(equal_budget), 'rows': rows,
    'camera_motion_error_ratio': sum(r['post_mae'] for r in motion) / sum(r['basic_mae'] for r in motion),
    'camera_edge_error_ratio': sum(r['post_mae'] for r in edge_motion) / sum(r['basic_mae'] for r in edge_motion),
    'static_sh_delta_ratio': noise['post']['static']['sh_delta'] / noise['basic']['static']['sh_delta'],
    'static_frame_delta_ratio': noise['post']['static']['frame_delta'] / noise['basic']['static']['frame_delta'],
    'moving_light_sh_delta_ratio': noise['post']['moving']['sh_delta'] / noise['basic']['moving']['sh_delta'],
    'gpu_ms': {name: reports[name]['static_gpu_ms_median'] for name in ('basic', 'post')},
}
result['passed'] = bool(equal_budget and all(r['passed'] for r in reports.values()) and all(r['passed'] for r in noise.values())
                        and all(r['matched_fraction'] > .95 for r in rows)
                        and result['camera_motion_error_ratio'] < .9 and result['camera_edge_error_ratio'] < 1.05
                        # Raw SH frame deltas also include TAA raster/normal changes
                        # at edges; require a gain without treating them as pure noise.
                        and result['static_sh_delta_ratio'] < .95 and result['static_frame_delta_ratio'] <= 1.02
                        and rows[1]['all']['ratio'] < 1.0 and result['gpu_ms']['post'] < 4.0)
(root / 'comparison.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print(json.dumps(result, indent=2))
sys.exit(0 if result['passed'] else 1)
