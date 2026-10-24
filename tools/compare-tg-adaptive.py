"""Compare bounded sampling against sparse and uniform-extra controls.

All variants use the same post denoiser. The 32-ray reference is this renderer,
not path-traced ground truth. Raw budget maps verify every 8x8 trace tile.
"""
import json
import sys
from pathlib import Path

import numpy as np


def load_sample(directory, meta):
    w, h, name = meta['width'], meta['height'], meta['name']
    sh = np.fromfile(directory / f'{name}_sh.bin', '<f2').astype(np.float32).reshape(h, w * 3, 4)
    n = np.fromfile(directory / f'{name}_normal.bin', '<f2').astype(np.float32).reshape(h, w, 4)[..., :3]
    p = np.fromfile(directory / f'{name}_position.bin', '<f4').reshape(h, w, 4)
    age = np.fromfile(directory / f'{name}_age.bin', '<f2').astype(np.float32).reshape(h, w)
    basis = np.concatenate([np.full((h, w, 1), 0.282094792), n * 0.325735008], axis=-1)
    coefficients = np.stack(np.split(sh, 3, axis=1), axis=-2)
    incoming = np.maximum(np.einsum('...ck,...k->...c', coefficients, basis), 0.0)
    assert np.isfinite(incoming).all()
    return incoming, p, n, age


def budget_metrics(directory, meta, position, uniform):
    data = np.fromfile(directory / f"{meta['name']}_budget.bin", np.uint8).reshape(meta['trace_height'], meta['trace_width'])
    rays, reason = data & 3, data >> 2
    valid, extra = rays > 0, rays == 2
    assert np.isin(rays, [0, 1, 2]).all()
    assert np.array_equal(valid, position[::2, ::2, 3] > 0)
    h, w = rays.shape
    padded = np.pad(rays, ((0, -h % 8), (0, -w % 8)))
    tiles = padded.reshape(padded.shape[0] // 8, 8, padded.shape[1] // 8, 8)
    receivers = (tiles > 0).sum(axis=(1, 3))
    extras = (tiles == 2).sum(axis=(1, 3))
    assert np.all(extras <= receivers // 2), 'Per-tile extra ray cap exceeded'
    counts = meta['counters']
    assert int(rays.sum()) == counts[0]
    assert int(extra.sum()) == counts[7]
    assert int(valid.sum()) == counts[8]
    for code in [1, 2, 3]:
        assert int((extra & (reason == code)).sum()) == counts[8 + code]
    if uniform:
        assert np.all(extras == receivers // 2)
    else:
        assert np.all(reason[extra] > 0), 'Extra ray allocated to a converged receiver'
    return {'rays': int(rays.sum()), 'base_receivers': int(valid.sum()),
            'extra_fraction': float(extra.sum() / max(valid.sum(), 1)),
            'extra_young': counts[9], 'extra_variance': counts[10], 'extra_lamp': counts[11],
            'tile_budget_ok': True}


def compare(root):
    variants = ['sparse', 'uniform', 'adaptive', 'reference']
    reports = {v: json.loads((root / v / 'results.json').read_text()) for v in variants}
    rows = []
    for index, meta in enumerate(reports['reference']['samples']):
        samples = {v: load_sample(root / v, reports[v]['samples'][index]) for v in variants}
        reference, rp, rn, _ = samples['reference']
        assert all(reports[v]['samples'][index]['name'] == meta['name'] and reports[v]['samples'][index]['frame'] == meta['frame'] for v in variants)
        assert all(np.array_equal(samples[v][3], samples['sparse'][3]) for v in variants), 'History ages changed between profiles'
        matched = rp[..., 3] > 0
        for _, position, normal, _ in samples.values():
            matched &= (position[..., 3] > 0) & (np.linalg.norm(position[..., :3] - rp[..., :3], axis=-1) < 0.03)
            matched &= np.sum(normal * rn, axis=-1) > 0.97
        row = {'name': meta['name'], 'matched_fraction': float(matched.sum() / max(1, (rp[..., 3] > 0).sum()))}
        edge = np.zeros(matched.shape, dtype=bool)
        for axis in [0, 1]:
            pp, nn = np.roll(rp, 1, axis), np.roll(rn, 1, axis)
            plane = np.abs(np.sum((pp[..., :3] - rp[..., :3]) * rn, axis=-1))
            discontinuity = (np.sum(nn * rn, axis=-1) < 0.85) | (plane > 0.03 + rp[..., 3] * 0.002)
            # Ignore the wrap boundary introduced by roll.
            if axis == 0:
                discontinuity[0, :] = False
            else:
                discontinuity[:, 0] = False
            edge |= discontinuity
        band = edge.copy()
        for axis in [0, 1]:
            for shift in [-2, -1, 1, 2]:
                band |= np.roll(edge, shift, axis)
        for region, mask in [('all', matched), ('young', matched & (samples['sparse'][3] < 8)), ('edges', matched & band)]:
            if mask.any():
                row[region] = {v: float(np.abs(samples[v][0] - reference)[mask].mean()) for v in variants[:-1]}
        for v in ['uniform', 'adaptive']:
            row[v + '_budget'] = budget_metrics(root / v, reports[v]['samples'][index], samples[v][1], v == 'uniform')
        rows.append(row)
    groups = {'cold': rows[:1], 'motion': [r for r in rows if r['name'].startswith('pan_')],
              'static': [r for r in rows if r['name'] in ['warm', 'settled']]}
    ratios = {}
    for group, items in groups.items():
        ratios[group] = {v + '_to_sparse': float(sum(r['all'][v] for r in items) / sum(r['all']['sparse'] for r in items)) for v in ['uniform', 'adaptive']}
        ratios[group]['adaptive_to_uniform'] = float(sum(r['all']['adaptive'] for r in items) / sum(r['all']['uniform'] for r in items))
    for region in ['young', 'edges']:
        ratios['motion'][region + '_adaptive_to_sparse'] = float(sum(r[region]['adaptive'] for r in groups['motion']) / sum(r[region]['sparse'] for r in groups['motion']))
    result = {'reference': 'Same renderer, 32 rays per half-resolution receiver; all profiles use post denoising and BVH-only error captures.',
              'rows': rows, 'error_ratios': ratios, 'same_history_ages_and_sample_frames': True,
              'static_gpu_ms': {v: reports[v]['static_gpu_ms_median'] for v in variants},
              'passed': bool(all(r['passed'] for r in reports.values()) and all(r['matched_fraction'] > 0.98 for r in rows)
                             and ratios['cold']['adaptive_to_sparse'] < 0.95 and ratios['motion']['adaptive_to_sparse'] < 0.95
                             and ratios['motion']['edges_adaptive_to_sparse'] < 1.0 and ratios['static']['adaptive_to_sparse'] <= 1.02)}
    (root / 'comparison.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))
    return result['passed']


if __name__ == '__main__':
    sys.exit(0 if compare(Path(sys.argv[1])) else 1)
