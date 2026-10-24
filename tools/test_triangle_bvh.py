"""Check the traversal ABI and nearest/any-hit results against brute force."""
import unittest

import numpy as np

from triangle_bvh import build_bvh


def intersections(triangles, origin, direction):
    a, b, c = triangles[:, 0, :3], triangles[:, 1, :3], triangles[:, 2, :3]
    e1, e2 = b - a, c - a
    h = np.cross(direction, e2)
    det = np.einsum("ij,ij->i", e1, h)
    inv = np.divide(1.0, det, out=np.zeros_like(det), where=np.abs(det) > 1e-9)
    delta = origin - a
    u = np.einsum("ij,ij->i", delta, h) * inv
    q = np.cross(delta, e1)
    v = (q @ direction) * inv
    t = np.einsum("ij,ij->i", e2, q) * inv
    return np.where((np.abs(det) > 1e-9) & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 0.004), t, np.inf)


def traverse(nodes, triangles, origin, direction, limit, any_hit=False):
    metadata = nodes.view("<u4")[:, 8:12]
    inverse = 1.0 / np.where(np.abs(direction) > 1e-20, direction, 1e-20)
    index, found = 0, False
    while index < len(nodes):
        a, b = (nodes[index, :3] - origin) * inverse, (nodes[index, 4:7] - origin) * inverse
        if max(np.minimum(a, b).max(), 0.001) > min(np.maximum(a, b).min(), limit):
            index = int(metadata[index, 2])
            continue
        start, count, escape = map(int, metadata[index, :3])
        if count == 0:
            index += 1
            continue
        distance = intersections(triangles[start:start + count], origin, direction).min()
        if distance < limit:
            limit, found = distance, True
            if any_hit:
                return found, limit
        index = escape
    return found, limit


class TriangleBVHTests(unittest.TestCase):
    def test_geometry_and_escape_links(self):
        rng = np.random.default_rng(702)
        triangles = np.zeros((257, 6, 4), dtype="<f4")
        triangles[:, :3, :3] = rng.uniform(-4, 4, (257, 3, 3))
        # Include a planar cluster, zero-area triangle, and coincident centroids.
        triangles[0, :3, :3] = 0
        triangles[1:33, :3, 2] = 0
        for method in ("median", "sah"):
            nodes, order, _ = build_bvh(triangles, method)
            np.testing.assert_array_equal(np.sort(order), np.arange(len(triangles)))
            metadata = nodes.view("<u4")[:, 8:12]
            self.assertEqual(nodes.strides[0], 48)
            self.assertEqual(metadata[0, 2], len(nodes))
            covered = []
            reordered = triangles[order]
            for index, (start, count, escape, _) in enumerate(metadata):
                self.assertTrue(index < escape <= len(nodes))
                if count:
                    covered.extend(range(start, start + count))
                    points = reordered[start:start + count, :3, :3]
                    self.assertTrue((points >= nodes[index, :3]).all())
                    self.assertTrue((points <= nodes[index, 4:7]).all())
                else:
                    right = metadata[index + 1, 2]
                    self.assertLess(right, escape)
                    self.assertEqual(metadata[right, 2], escape)
            self.assertEqual(covered, list(range(len(triangles))))
            for ray in range(100):
                origin = rng.uniform(-8, 8, 3)
                direction = rng.normal(size=3)
                direction /= np.linalg.norm(direction)
                if ray == 0:
                    direction = np.array([0.0, 0.0, -1.0])
                limit = 3.0 if ray % 2 else 80.0
                expected = min(intersections(triangles, origin, direction).min(), limit)
                found, actual = traverse(nodes, reordered, origin, direction, limit)
                self.assertEqual(found, expected < limit)
                self.assertAlmostEqual(actual, expected, places=5)
                self.assertEqual(traverse(nodes, reordered, origin, direction, limit, True)[0], found)

    def test_degenerate_centroids_and_invalid_input(self):
        triangles = np.zeros((65, 6, 4), dtype="<f4")
        for method in ("sah", "median"):
            nodes, order, stats = build_bvh(triangles, method)
            self.assertEqual(len(order), 65)
            self.assertLess(stats["max_depth"], 10)
            self.assertEqual(nodes.view("<u4")[0, 10], len(nodes))
        with self.assertRaises(ValueError):
            build_bvh(triangles[:0])
        triangles[0, 0, 0] = np.nan
        with self.assertRaises(ValueError):
            build_bvh(triangles)


if __name__ == "__main__":
    unittest.main()
