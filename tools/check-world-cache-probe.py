"""Generate independent surface/age boundary cases, then verify real GPU queries."""
import json
import sys
from pathlib import Path

import numpy as np


def key(position, normal):
    axis = int(np.argmax(np.abs(normal)))
    return (*np.floor(np.asarray(position) / .25).astype(int), axis * 2 + int(normal[axis] < 0))


def slot(cell):
    h = 0
    for value, prime in zip(cell, (73856093, 19349663, 83492791, 2654435761)):
        h ^= (int(value) * prime) & 0xffffffff
    return (h ^ (h >> 16)) & 65535


def prepare(root):
    root.mkdir(parents=True, exist_ok=True)
    entries = np.zeros((65536, 16), dtype='<f4')
    integer = entries.view('<u4')
    for position in [(0.1, 0.1, 0.1), (-0.1, 0.1, -0.1)]:
        cell = key(position, (0, 1, 0))
        index = slot(cell)
        entries.view('<i4')[index, :4] = cell
        entries[index, 4:7] = position
        integer[index, 7] = 100
        entries[index, 8:11] = [0, 1, 0]
        integer[index, 11] = 1
        entries[index, 12:15] = [.2, .4, .8]
    # Poison a matching bucket with a different exact key, but matching position
    # and normal: distance checks alone cannot reject this synthetic collision.
    collision_position = (1.1, .1, 1.1)
    target = key(collision_position, (0, 1, 0))
    collision = next((x, 5, 7, 2) for x in range(200000) if slot((x, 5, 7, 2)) == slot(target))
    index = slot(target)
    assert index not in [slot(key(p, (0, 1, 0))) for p in [(0.1, .1, .1), (-.1, .1, -.1)]]
    entries.view('<i4')[index, :4] = collision
    entries[index, 4:7] = collision_position
    integer[index, 7] = 100
    entries[index, 8:11] = [0, 1, 0]
    integer[index, 11] = 1
    entries[index, 12:15] = [9, 9, 9]
    cases = []

    def add(name, position=(.1, .1, .1), normal=(0, 1, 0), epoch=1, tick=101, weight=1):
        expected = [v * weight for v in [.2, .4, .8]] + [int(weight > 0)]
        cases.append(dict(name=name, position=position, normal=normal, epoch=epoch, tick=tick, expected=expected))

    add('same_surface')
    add('same_plane', position=(.22, .1, .22))
    add('within_plane_tolerance', position=(.1, .109, .1))
    add('outside_plane_tolerance', position=(.1, .111, .1), weight=0)
    add('four_centimeter_parallel_wall', position=(.1, .14, .1), weight=0)
    add('opposite_face', normal=(0, -1, 0), weight=0)
    add('different_normal_same_bucket', normal=(.6, .8, 0), weight=0)
    add('epoch_changed', epoch=2, weight=0)
    add('future_record', tick=99, weight=0)
    add('age_90', tick=190)
    add('age_105', tick=205, weight=.5)
    t = 29 / 30
    add('age_119', tick=219, weight=1 - t * t * (3 - 2 * t))
    add('age_120', tick=220, weight=0)
    add('negative_cell', position=(-.1, .1, -.1))
    add('different_cell', position=(.2501, .1, .1), weight=0)
    add('exact_hash_collision', position=collision_position, weight=0)
    queries = np.zeros((len(cases), 12), dtype='<f4')
    for index, case in enumerate(cases):
        queries[index, :3] = case['position']
        queries[index, 4:7] = case['normal']
        queries.view('<u4')[index, 8:10] = [case['epoch'], case['tick']]
    entries.tofile(root / 'entries.bin')
    queries.tofile(root / 'queries.bin')
    (root / 'expected.json').write_text(json.dumps(cases, indent=2))


def check(root):
    cases = json.loads((root / 'expected.json').read_text())
    actual = np.fromfile(root / 'actual.bin', '<f4').reshape(-1, 4)
    for case, value in zip(cases, actual, strict=True):
        case['actual'] = value.tolist()
        case['passed'] = bool(np.allclose(value, case['expected'], atol=1e-6, rtol=1e-5))
    report = {'cases': cases, 'passed': all(c['passed'] for c in cases)}
    (root / 'results.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({'passed': report['passed'], 'cases': len(cases), 'failed': [c['name'] for c in cases if not c['passed']]}))
    return report['passed']


if __name__ == '__main__':
    if sys.argv[1] == 'prepare':
        prepare(Path(sys.argv[2]))
    else:
        sys.exit(0 if check(Path(sys.argv[2])) else 1)
