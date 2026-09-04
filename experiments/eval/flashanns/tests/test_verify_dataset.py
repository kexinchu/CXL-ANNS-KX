import struct
import tempfile
import unittest
from pathlib import Path

from experiments.eval.flashanns.verify_dataset import (
    DatasetError,
    compare_widened_u8,
    read_bin_header,
    validate_gt_subset,
    validate_l2_topk_equivalence,
    validate_permutation,
    validate_query_ids,
    verify_packed_readback,
)


def write_matrix(path: Path, rows, code: str) -> None:
    dim = len(rows[0]) if rows else 0
    with path.open("wb") as out:
        out.write(struct.pack("<II", len(rows), dim))
        for row in rows:
            out.write(struct.pack(f"<{len(row)}{code}", *row))


class VerifyDatasetTest(unittest.TestCase):
    def test_headers_require_exact_length(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            for code, size in (("f", 4), ("B", 1), ("I", 4)):
                path = root / code
                write_matrix(path, [[1, 2], [3, 4]], code)
                self.assertEqual(read_bin_header(path, size), (2, 2))
                path.write_bytes(path.read_bytes() + b"x")
                with self.assertRaisesRegex(DatasetError, path.name):
                    read_bin_header(path, size)

    def test_query_ids_and_permutation_fail_closed(self):
        validate_query_ids([3, 1, 4], available=5, required=3)
        for ids in ([1, 1, 2], [0, 1], [0, 1, 5]):
            with self.assertRaises(DatasetError):
                validate_query_ids(list(ids), available=5, required=3)
        validate_permutation([2, 0, 1], 3)
        with self.assertRaises(DatasetError):
            validate_permutation([0, 0, 2], 3)

    def test_gt_subset_is_aligned_and_reread(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "source.ibin"
            output = root / "subset.ibin"
            write_matrix(source, [[0, 1, 2], [10, 11, 12], [20, 21, 22], [30, 31, 32]], "I")
            validate_gt_subset(source, [2, 0], output, k=2)
            self.assertEqual(read_bin_header(output, 4), (2, 2))
            self.assertEqual(struct.unpack("<4I", output.read_bytes()[8:]), (20, 21, 0, 1))

    def test_widening_preserves_coordinates_and_l2_topk(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            native = root / "base.u8bin"
            widened = root / "base.fbin"
            queries = root / "query.u8bin"
            write_matrix(native, [[0, 0], [4, 4], [10, 10]], "B")
            write_matrix(widened, [[0.0, 0.0], [4.0, 4.0], [10.0, 10.0]], "f")
            write_matrix(queries, [[3, 3], [9, 9]], "B")
            compare_widened_u8(native, widened, [0, 1, 2], dim=2)
            validate_l2_topk_equivalence(native, widened, queries, [0, 1], k=2)
            data = bytearray(widened.read_bytes())
            struct.pack_into("<f", data, 8 + 2 * 4, 4.5)
            widened.write_bytes(data)
            with self.assertRaises(DatasetError):
                compare_widened_u8(native, widened, [1], dim=2)

    def test_packed_readback_checks_vectors_and_neighbors(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            base = root / "base.fbin"
            graph = root / "graph.bin"
            extent = root / "extent.bin"
            id_map = root / "new_to_old.bin"
            slot_map = root / "id_to_slot.bin"
            rows = [[1.0, 2.0], [3.0, 4.0], [5.0, 6.0]]
            neighbors = [[1, 2], [2, 0], [0, 1]]
            write_matrix(base, rows, "f")
            graph.write_bytes(b"".join(struct.pack("<2I", *row) for row in neighbors))
            id_map.write_bytes(struct.pack("<3I", 2, 0, 1))
            slot_map.write_bytes(struct.pack("<3I", 1, 2, 0))
            stride = 32
            header = struct.pack(
                "<Q9I9Q",
                0x314E415843,
                2,
                3,
                2,
                2,
                0,
                4,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                4096,
                3 * stride,
                0,
                0,
                0,
            )
            image = bytearray(4096 + 3 * stride)
            image[: len(header)] = header
            new_to_old = [2, 0, 1]
            id_to_slot = [1, 2, 0]
            for logical in range(3):
                off = 4096 + id_to_slot[logical] * stride
                struct.pack_into("<2fI2I", image, off, *rows[new_to_old[logical]], 2, *neighbors[logical])
            extent.write_bytes(image)
            dataset = {
                "count": 3,
                "dimension": 2,
                "execution_dtype": "float32",
                "R": 2,
                "artifacts": {
                    "execution_base": str(base),
                    "extent_image": str(extent),
                    "graph": str(graph),
                    "id_map": str(id_map),
                    "slot_map": str(slot_map),
                },
            }
            proof = verify_packed_readback(dataset, sample_count=3, seed=7)
            self.assertEqual(proof["sampled_records"], 3)


if __name__ == "__main__":
    unittest.main()
