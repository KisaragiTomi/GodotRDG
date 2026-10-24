"""Check captured Vulkan candidates against the source, direction and occlusion."""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def compare(root):
    report = json.loads((root / 'results.json').read_text())
    cases = {case['name']: case for case in report['cases']}
    assert len(cases) == 11 and report['passed']

    def load(name, field):
        meta = cases[name]
        w, h = meta['width'], meta['height']
        if field in ('candidate', 'direction'):
            w, h = meta['candidate_width'], meta['candidate_height']
        if field == 'sh':
            w *= 3
        dtype = '<f4' if field == 'position' else '<f2'
        return np.fromfile(root / f'{name}_{field}.bin', dtype).astype(np.float32).reshape(h, w, 4)

    checks = {}
    base = load('sky', 'candidate')[..., :3]
    lit = np.any(base > 0, axis=-1)
    for name in ['dark', 'sky', 'half_energy', 'warm', 'far_roof', 'restored']:
        # Same geometry, sample sequence and ray budget, regardless of lighting.
        assert cases[name]['frame'] == cases['sky']['frame']
        assert np.array_equal(load(name, 'position'), load('sky', 'position'))
        assert np.array_equal(load(name, 'direction'), load('sky', 'direction'))
        rgb = load(name, 'candidate')[..., :3]
        direction = load(name, 'direction')
        source = np.asarray(cases[name]['sky'][:3], dtype=np.float32)
        hits = np.any(rgb > 0, axis=-1)
        expected = np.broadcast_to(source.astype(np.float16).astype(np.float32), rgb.shape)
        # GPU half stores may round toward zero. Allow one half ULP.
        tolerance = max(float(np.max(source)) * 0.001, 1e-6)
        passed = np.isfinite(rgb).all() and not np.any(hits & (direction[..., 1] <= 0))
        passed &= int(hits.sum()) == cases[name]['counters'][12]
        if name not in ['dark', 'far_roof']:
            passed &= np.array_equal(hits, lit) and np.max(np.abs(rgb[hits] - expected[hits])) <= tolerance
        else:
            passed &= np.count_nonzero(rgb) == 0
        checks[name] = {'passed': bool(passed), 'sky_rays': int(hits.sum()),
                        'total_rays': cases[name]['counters'][0], 'source_linear': source.tolist()}
    assert np.count_nonzero(load('off_after3', 'sh')) == 0
    assert np.array_equal(load('sky', 'candidate'), load('restored', 'candidate'))
    half_error = float(np.max(np.abs(load('half_energy', 'candidate')[..., :3] - base * 0.5)))
    checks['energy_scaling'] = {'max_error': half_error, 'passed': half_error < 1e-5}

    def png(name):
        return np.asarray(Image.open(root / (name + '.png')).convert('RGB'), dtype=np.float32) / 255

    a, b = png('full_without_sky'), png('full_with_sky')
    assert np.array_equal(load('full_without_sky', 'position'), load('full_with_sky', 'position'))
    assert np.array_equal(load('full_without_sky', 'direction'), load('full_with_sky', 'direction'))
    geometry = load('full_with_sky', 'position')[..., 3] > 0
    # Restrict display comparison to geometry to exclude the changed sky background.
    mask = np.repeat(np.repeat(geometry, 2, 0), 2, 1)[:a.shape[0], :a.shape[1]]
    display_delta = float(np.abs(a - b)[mask].mean())
    checks['display'] = {'geometry_mae': display_delta, 'passed': display_delta > 0.0001}
    result = {'checks': checks, 'passed': all(c['passed'] for c in checks.values()),
              'note': 'Sky-only uses BVH without screen feedback. The roof is 150 m high and hidden only from rasterization; it must occlude GI. Timings are not benchmarked.'}
    (root / 'comparison.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))
    return result['passed']


if __name__ == '__main__':
    sys.exit(0 if compare(Path(sys.argv[1])) else 1)
