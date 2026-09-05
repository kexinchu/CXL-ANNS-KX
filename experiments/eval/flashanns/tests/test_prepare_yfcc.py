import struct
import tempfile
import unittest
from pathlib import Path

import numpy as np

from experiments.eval.flashanns.prepare_yfcc import (
    extract_fbin_prefix,
    extract_gt_topk,
    widen_u8bin,
)


class PrepareYfccTest(unittest.TestCase):
    def test_query_prefix_is_exact_and_resumable(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src, dst = root / "queries.fbin", root / "queries10.fbin"
            values = np.arange(15, dtype="<f4").reshape(5, 3)
            src.write_bytes(struct.pack("<II", 5, 3) + values.tobytes())
            extract_fbin_prefix(src, dst, rows=2, expected_dim=3)
            extract_fbin_prefix(src, dst, rows=2, expected_dim=3)
            raw = dst.read_bytes()
            self.assertEqual(struct.unpack_from("<II", raw), (2, 3))
            np.testing.assert_array_equal(
                np.frombuffer(raw, dtype="<f4", offset=8).reshape(2, 3), values[:2]
            )

    def test_streaming_widening_preserves_every_coordinate(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src, dst = root / "x.u8bin", root / "x.fbin"
            values = np.array([[0, 1, 255], [17, 23, 42]], dtype=np.uint8)
            src.write_bytes(struct.pack("<II", 2, 3) + values.tobytes())
            manifest = widen_u8bin(src, dst, expected_rows=2, expected_dim=3, chunk_rows=1)
            raw = dst.read_bytes()
            self.assertEqual(struct.unpack_from("<II", raw), (2, 3))
            widened = np.frombuffer(raw, dtype="<f4", offset=8).reshape(2, 3)
            np.testing.assert_array_equal(widened, values.astype(np.float32))
            self.assertEqual(manifest["rows"], 2)
            self.assertEqual(
                widen_u8bin(src, dst, expected_rows=2, expected_dim=3, chunk_rows=1),
                manifest,
            )

    def test_top10_gt_extraction_keeps_row_alignment(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src, dst = root / "gt.ibin", root / "gt10.ibin"
            gt = np.arange(3 * 100, dtype="<u4").reshape(3, 100)
            src.write_bytes(struct.pack("<II", 3, 100) + gt.tobytes())
            extract_gt_topk(src, dst, rows=2, k=10)
            extract_gt_topk(src, dst, rows=2, k=10)
            raw = dst.read_bytes()
            self.assertEqual(struct.unpack_from("<II", raw), (2, 10))
            out = np.frombuffer(raw, dtype="<u4", offset=8).reshape(2, 10)
            np.testing.assert_array_equal(out, gt[:2, :10])

    def test_top10_gt_accepts_official_ids_plus_distances_format(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src, dst = root / "gt-with-dists.ibin", root / "gt10.ibin"
            gt = np.arange(3 * 100, dtype="<u4").reshape(3, 100)
            distances = np.arange(3 * 100, dtype="<f4").reshape(3, 100)
            src.write_bytes(
                struct.pack("<II", 3, 100) + gt.tobytes() + distances.tobytes()
            )
            extract_gt_topk(src, dst, rows=2, k=10)
            raw = dst.read_bytes()
            self.assertEqual(struct.unpack_from("<II", raw), (2, 10))
            out = np.frombuffer(raw, dtype="<u4", offset=8).reshape(2, 10)
            np.testing.assert_array_equal(out, gt[:2, :10])

    def test_native_and_widened_l2_rankings_are_identical(self):
        base = np.array([[0, 0], [3, 4], [10, 10]], dtype=np.uint8)
        query = np.array([2, 3], dtype=np.uint8)
        native = np.sum((base.astype(np.int32) - query.astype(np.int32)) ** 2, axis=1)
        widened = np.sum((base.astype(np.float32) - query.astype(np.float32)) ** 2, axis=1)
        np.testing.assert_array_equal(np.argsort(native), np.argsort(widened))


if __name__ == "__main__":
    unittest.main()
