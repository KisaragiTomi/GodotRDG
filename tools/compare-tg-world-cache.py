"""Validate offscreen retention, deterministic fallbacks and fog-free cache data."""
import json
import sys
from pathlib import Path

import numpy as np


def load(directory, meta, field):
    if field == 'cache':
        return np.fromfile(directory / (meta['name'] + '_cache.bin'), '<u4').reshape(-1, 16)
    w, h = meta['width'], meta['height']
    if field in ('candidate', 'radiance'):
        w, h = meta[field + '_width'], meta[field + '_height']
    if field == 'sh':
        w *= 3
    dtype, channels = ('<f4' if field == 'position' else '<f2'), (1 if field == 'age' else 4)
    return np.fromfile(directory / (meta['name'] + '_' + field + '.bin'), dtype).astype(np.float32).reshape(h, w, channels)


def compare(root):
    reports = {v: json.loads((root / v / 'results.json').read_text()) for v in ['off', 'on', 'fog']}
    assert all(r['passed'] and len(r['cases']) == 13 for r in reports.values())
    assert not reports['off']['enabled'] and reports['on']['enabled'] and reports['fog']['enabled']
    cases = {v: {c['name']: c for c in r['cases']} for v, r in reports.items()}
    isolation, retention = [], []
    for name, on in cases['on'].items():
        off, fog = cases['off'][name], cases['fog'][name]
        assert on['frame'] == off['frame'] == fog['frame'], name + ': different sample sequence'
        assert on['epoch'] == off['epoch'] == fog['epoch'] and on['tick'] == off['tick'] == fog['tick']
        assert np.array_equal(load(root / 'on', on, 'position'), load(root / 'off', off, 'position'))
        row = {'name': name, 'passed': True}
        for field in ['position', 'normal', 'age', 'sh', 'radiance', 'candidate', 'cache']:
            a, b = load(root / 'on', on, field), load(root / 'fog', fog, field)
            row[field + '_equal'] = bool(np.array_equal(a, b))
            row['passed'] &= row[field + '_equal']
        isolation.append(row)
        entries = load(root / 'on', on, 'cache')
        epoch_valid = entries[:, 11] == on['epoch']
        active = epoch_valid & ((np.uint32(on['tick']) - entries[:, 7]) < 120)
        valid = entries[active]
        values = valid.view('<f4')
        if len(valid):
            assert np.isfinite(values[:, [4, 5, 6, 8, 9, 10, 12, 13, 14]]).all()
            assert np.all((values[:, 12:15] >= 0) & (values[:, 12:15] <= 10))
            assert np.allclose(np.linalg.norm(values[:, 8:11], axis=-1), 1, atol=.002)
            cell = valid[:, :4].view('<i4')
            assert np.array_equal(cell[:, :3], np.floor(values[:, 4:7] / .25).astype(np.int32))
            k = cell.astype(np.uint32)
            h = k[:, 0] * np.uint32(73856093) ^ k[:, 1] * np.uint32(19349663) ^ k[:, 2] * np.uint32(83492791) ^ k[:, 3] * np.uint32(2654435761)
            assert np.array_equal((h ^ (h >> 16)) & 65535, np.flatnonzero(active))
        last_writes = epoch_valid & (entries[:, 7] == on['tick'] - 1)
        assert int(last_writes.sum()) == on['counters'][16], name + ': update count'
        a = load(root / 'on', on, 'candidate')[..., :3]
        b = load(root / 'off', off, 'candidate')[..., :3]
        if name in ['expired', 'lights_off', 'blocker_reset', 'lamp_motion', 'no_history']:
            assert np.array_equal(a, b), name + ': stale cache affected fallback'
        added = np.any((a - b) > 1e-5, axis=-1)
        if name in ['warm_a', 'turned_frozen']:
            assert added.sum() > 100
            assert np.all(a >= b), name + ': unexpectedly removed source energy'
        retention.append({'name': name, 'cache_hits': on['counters'][14], 'offscreen_hits': on['counters'][15],
                          'updates': on['counters'][16], 'active_entries': int(active.sum()),
                          'candidates_with_added_indirect': int(added.sum()), 'candidate_mae': float(np.abs(a - b).mean())})
    assert np.array_equal(load(root / 'on', cases['on']['turned_frozen'], 'cache'), load(root / 'on', cases['on']['expired'], 'cache'))
    assert all(row['passed'] for row in isolation)
    result = {'passed': True, 'retention': retention, 'fog_isolation': isolation,
              'note': 'Cache retains previously observed diffuse GI; unseen surfaces still use direct BVH fallback. This is functional retention evidence, not a path-traced accuracy or speed claim.'}
    (root / 'comparison.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    compare(Path(sys.argv[1]))
