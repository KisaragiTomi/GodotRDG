"""Validate native fog equivalence and isolation of pre-fog GI history."""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def png(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.float32) / 255


def load(directory, meta, field):
    name, w, h = meta['name'], meta['width'], meta['height']
    if field == 'radiance':
        w, h = meta['radiance_width'], meta['radiance_height']
    if field == 'sh':
        w *= 3
    dtype = '<f4' if field == 'position' else '<f2'
    channels = 1 if field == 'age' else 4
    return np.fromfile(directory / f'{name}_{field}.bin', dtype).astype(np.float32).reshape(h, w, channels)


def compare(root):
    reports = {v: json.loads((root / v / 'results.json').read_text()) for v in ['native', 'separated', 'control']}
    assert not reports['native']['separated'] and reports['separated']['separated'] and reports['control']['separated']
    assert all(len(v['composition']) == 7 and len(v['integration']) == 10 for v in reports.values())
    rows = []
    for native, separated in zip(reports['native']['composition'], reports['separated']['composition'], strict=True):
        assert native['name'] == separated['name'] and native['frame'] == separated['frame']
        name = native['name']
        delta = np.abs(png(root / 'native' / f'{name}.png') - png(root / 'separated' / f'{name}.png'))
        rows.append({'name': name, 'mae': float(delta.mean()), 'p99': float(np.quantile(delta, 0.99)),
                     'passed': bool(delta.mean() < 0.002 and np.quantile(delta, 0.99) < 0.015)})
    isolation = []
    for separated, control in zip(reports['separated']['integration'], reports['control']['integration'], strict=True):
        assert separated['name'] == control['name'] and separated['frame'] == control['frame']
        name = separated['name']
        row = {'name': name, 'screen_hits': separated['counters'][1], 'bvh_rays': separated['counters'][2], 'passed': True}
        for field in ['position', 'normal', 'age', 'sh', 'radiance']:
            a, b = load(root / 'separated', separated, field), load(root / 'control', control, field)
            delta = np.abs(a - b)
            row[field + '_max_difference'] = float(delta.max())
            row[field + '_mae'] = float(delta.mean())
            row['passed'] &= bool(np.isfinite(a).all() and np.isfinite(b).all() and delta.max() == 0.0)
        row['display_mae'] = float(np.abs(png(root / 'separated' / f'{name}.png') - png(root / 'control' / f'{name}.png')).mean())
        if name not in ['zero_density', 'lights_off']:
            row['passed'] &= row['display_mae'] > 0.002
        if name == 'lights_off':
            # This case now disables sky as well as both lamps. No fog source
            # remains; require black displays as well as zero surface history.
            row['display_maximum'] = float(max(png(root / variant / f'{name}.png').max() for variant in ['separated', 'control']))
            row['passed'] &= row['display_maximum'] == 0.0
        isolation.append(row)
    result = {'composition': rows, 'fog_isolation': isolation,
              'note': 'Composition compares the same BVH GI with no temporal jitter. Integration uses matching input sequences; only fog differs. GPU timings are diagnostic.',
              'passed': bool(all(v['passed'] for v in reports.values()) and all(r['passed'] for r in rows + isolation))}
    (root / 'comparison.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))
    return result['passed']


if __name__ == '__main__':
    sys.exit(0 if compare(Path(sys.argv[1])) else 1)
