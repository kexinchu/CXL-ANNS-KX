import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]


class RemapDiskannPagesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build_dir = tempfile.TemporaryDirectory()
        cls.rebuild = Path(cls.build_dir.name) / "rebuild"
        cls.remap = Path(cls.build_dir.name) / "remap"
        for source, binary in (
            (ROOT / "tools/rebuild_diskann_from_base.cpp", cls.rebuild),
            (ROOT / "tools/remap_diskann_pages.cpp", cls.remap),
        ):
            subprocess.run(
                ["g++", "-std=c++17", "-O2", "-I", str(ROOT), str(source), "-o", str(binary)],
                check=True,
                cwd=ROOT,
            )

    @classmethod
    def tearDownClass(cls):
        cls.build_dir.cleanup()

    def test_extent_map_uses_one_record_per_page_for_4096_stride(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            base = root / "base.fbin"
            graph = root / "graph.bin"
            identity = root / "identity.bin"
            image = root / "logical.bin"
            out_map = root / "extent.map"
            base.write_bytes(struct.pack("<II", 4, 512) + bytes(4 * 512 * 4))
            graph.write_bytes(struct.pack("<8I", 3, 1, 0, 0, 0, 0, 2, 2))
            identity.write_bytes(struct.pack("<4I", 0, 1, 2, 3))
            subprocess.run(
                [
                    str(self.rebuild), "--base", str(base), "--id-map", str(identity),
                    "--graph", str(graph), "--out", str(image), "--entry-id", "0",
                    "--entry-nodes", "4", "--R", "2", "--stride", "4096",
                ],
                check=True,
                capture_output=True,
            )
            subprocess.run(
                [
                    str(self.remap), "--src", str(image), "--out", str(root / "unused.bin"),
                    "--map", str(out_map), "--mode", "extent", "--map-in", str(identity),
                    "--map-only",
                ],
                check=True,
                capture_output=True,
            )
            self.assertEqual(struct.unpack("<4I", out_map.read_bytes()), (2, 0, 3, 1))

    def test_extent_map_preserves_two_record_page_pairs_for_2048_stride(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            base = root / "base.fbin"
            graph = root / "graph.bin"
            identity = root / "identity.bin"
            image = root / "logical.bin"
            out_map = root / "extent.map"
            base.write_bytes(struct.pack("<II8f", 4, 2, *([0.0] * 8)))
            graph.write_bytes(struct.pack("<8I", 3, 1, 0, 0, 0, 0, 2, 2))
            identity.write_bytes(struct.pack("<4I", 0, 1, 2, 3))
            subprocess.run(
                [
                    str(self.rebuild), "--base", str(base), "--id-map", str(identity),
                    "--graph", str(graph), "--out", str(image), "--entry-id", "0",
                    "--entry-nodes", "4", "--R", "2", "--stride", "2048",
                ],
                check=True,
                capture_output=True,
            )
            subprocess.run(
                [
                    str(self.remap), "--src", str(image), "--out", str(root / "unused.bin"),
                    "--map", str(out_map), "--mode", "extent", "--map-in", str(identity),
                    "--map-only",
                ],
                check=True,
                capture_output=True,
            )
            slots = struct.unpack("<4I", out_map.read_bytes())
            self.assertEqual(set(slots), set(range(4)))
            self.assertEqual(slots[0] // 2, slots[1] // 2)
            self.assertEqual(slots[2] // 2, slots[3] // 2)


if __name__ == "__main__":
    unittest.main()
