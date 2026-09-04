import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
SOURCE = ROOT / "tools" / "apply_diskann_slot_map.cpp"


class ApplyDiskannSlotMapTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build_dir = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.build_dir.name) / "apply_diskann_slot_map"
        subprocess.run(
            ["g++", "-std=c++17", "-O2", "-I", str(ROOT), str(SOURCE), "-o", str(cls.binary)],
            check=True,
            cwd=ROOT,
        )

    @classmethod
    def tearDownClass(cls):
        cls.build_dir.cleanup()

    def make_image(self, root: Path):
        source = root / "source.bin"
        slot_map = root / "slots.bin"
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
            7,
            9,
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
        for logical in range(3):
            image[4096 + logical * stride : 4096 + (logical + 1) * stride] = bytes(
                [logical + 1]
            ) * stride
        source.write_bytes(image)
        slot_map.write_bytes(struct.pack("<3I", 1, 2, 0))
        return source, slot_map, stride

    def test_applies_existing_logical_id_to_slot_permutation(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source, slot_map, stride = self.make_image(root)
            output = root / "extent.bin"
            subprocess.run(
                [str(self.binary), "--src", str(source), "--slot-map", str(slot_map), "--out", str(output)],
                check=True,
            )
            data = output.read_bytes()
            self.assertEqual(data[:4096], source.read_bytes()[:4096])
            self.assertEqual(data[4096 : 4096 + stride], bytes([3]) * stride)
            self.assertEqual(data[4096 + stride : 4096 + 2 * stride], bytes([1]) * stride)
            self.assertEqual(data[4096 + 2 * stride :], bytes([2]) * stride)

    def test_rejects_invalid_map_without_creating_output(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source, slot_map, _ = self.make_image(root)
            slot_map.write_bytes(struct.pack("<3I", 0, 0, 2))
            output = root / "extent.bin"
            result = subprocess.run(
                [str(self.binary), "--src", str(source), "--slot-map", str(slot_map), "--out", str(output)]
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(output.exists())

    def test_refuses_to_overwrite_output(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source, slot_map, _ = self.make_image(root)
            output = root / "extent.bin"
            output.write_bytes(b"keep-me")
            result = subprocess.run(
                [str(self.binary), "--src", str(source), "--slot-map", str(slot_map), "--out", str(output)]
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(output.read_bytes(), b"keep-me")


if __name__ == "__main__":
    unittest.main()
