import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
SOURCE = ROOT / "tools" / "rebuild_diskann_from_base.cpp"


class RebuildDiskannFromBaseTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build_dir = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.build_dir.name) / "rebuild_diskann_from_base"
        subprocess.run(
            [
                "g++",
                "-std=c++17",
                "-O2",
                "-I",
                str(ROOT),
                str(SOURCE),
                "-o",
                str(cls.binary),
            ],
            check=True,
            cwd=ROOT,
        )

    @classmethod
    def tearDownClass(cls):
        cls.build_dir.cleanup()

    def make_fixture(self, root: Path):
        base = root / "base.fbin"
        id_map = root / "new_to_old.bin"
        graph = root / "graph.bin"
        rows = [(1.0, 2.0), (3.0, 4.0), (5.0, 6.0)]
        neighbors = [(1, 2), (2, 0), (0, 1)]
        base.write_bytes(struct.pack("<II6f", 3, 2, *(v for row in rows for v in row)))
        id_map.write_bytes(struct.pack("<3I", 2, 0, 1))
        graph.write_bytes(struct.pack("<6I", *(v for row in neighbors for v in row)))
        return base, id_map, graph, rows, neighbors

    def command(self, base: Path, id_map: Path, graph: Path, out: Path):
        return [
            str(self.binary),
            "--base",
            str(base),
            "--id-map",
            str(id_map),
            "--graph",
            str(graph),
            "--out",
            str(out),
            "--entry-id",
            "7",
            "--entry-nodes",
            "9",
            "--R",
            "2",
            "--stride",
            "64",
        ]

    def test_rebuilds_logical_records_from_original_base(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            base, id_map, graph, rows, neighbors = self.make_fixture(root)
            out = root / "diskann.bin"
            subprocess.run(self.command(base, id_map, graph, out), check=True)

            image = out.read_bytes()
            self.assertEqual(len(image), 4096 + 3 * 64)
            self.assertEqual(struct.unpack_from("<Q", image, 0)[0], 0x314E415843)
            self.assertEqual(struct.unpack_from("<7I", image, 8), (2, 3, 2, 2, 0, 4, 7))
            self.assertEqual(struct.unpack_from("<I", image, 36)[0], 9)
            self.assertEqual(struct.unpack_from("<Q", image, 76)[0], 4096)
            self.assertEqual(struct.unpack_from("<Q", image, 84)[0], 3 * 64)

            new_to_old = (2, 0, 1)
            for logical_id, old_id in enumerate(new_to_old):
                off = 4096 + logical_id * 64
                self.assertEqual(struct.unpack_from("<2f", image, off), rows[old_id])
                self.assertEqual(struct.unpack_from("<I", image, off + 8)[0], 2)
                self.assertEqual(struct.unpack_from("<2I", image, off + 12), neighbors[logical_id])

    def test_refuses_to_overwrite_existing_output(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            base, id_map, graph, _, _ = self.make_fixture(root)
            out = root / "diskann.bin"
            out.write_bytes(b"keep-me")
            result = subprocess.run(
                self.command(base, id_map, graph, out), capture_output=True, text=True
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(out.read_bytes(), b"keep-me")

    def test_rejects_non_permutation_map_before_creating_output(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            base, id_map, graph, _, _ = self.make_fixture(root)
            id_map.write_bytes(struct.pack("<3I", 0, 0, 2))
            out = root / "diskann.bin"
            result = subprocess.run(
                self.command(base, id_map, graph, out), capture_output=True, text=True
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(out.exists())


if __name__ == "__main__":
    unittest.main()
