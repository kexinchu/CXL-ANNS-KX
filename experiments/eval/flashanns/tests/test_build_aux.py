import struct
import tempfile
import unittest
from pathlib import Path

from experiments.eval.flashanns.build_aux import write_entry_graph, write_identity_map


class BuildAuxTest(unittest.TestCase):
    def test_identity_map_is_exact_little_endian_permutation(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "identity.bin"
            write_identity_map(path, 5)
            self.assertEqual(path.read_bytes(), struct.pack("<5I", 0, 1, 2, 3, 4))

    def test_entry_graph_uses_deterministic_bfs_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            graph = root / "graph.bin"
            # 0 -> 1,2; 1 -> 3,0; remaining rows keep the graph connected.
            graph.write_bytes(struct.pack("<8I", 1, 2, 3, 0, 3, 0, 0, 1))
            output = root / "entry.bin"
            write_entry_graph(graph, output, n=4, degree=2, entry_id=0, limit=3)
            en, degree, entry_id, *payload = struct.unpack("<12I", output.read_bytes())
            self.assertEqual((en, degree, entry_id), (3, 2, 0))
            self.assertEqual(payload[:3], [0, 1, 2])
            self.assertEqual(payload[3:], [1, 2, 3, 0, 3, 0])

    def test_aux_writers_refuse_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "existing.bin"
            path.write_bytes(b"keep")
            with self.assertRaises(FileExistsError):
                write_identity_map(path, 2)


if __name__ == "__main__":
    unittest.main()
