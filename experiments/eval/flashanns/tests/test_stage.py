import struct
import tempfile
import unittest
from pathlib import Path

from experiments.eval.flashanns.layout import select_dataset_layout
from experiments.eval.flashanns.stage import StageError, stage_image


class StageImageTest(unittest.TestCase):
    def _dataset(self, root: Path) -> tuple[dict, bytes]:
        stride = 4096
        header = struct.pack("<QIIII", 0x314E415843, 2, 3, 512, 32)
        payload = header + bytes(4096 - len(header)) + bytes(range(256)) * (3 * stride // 256)
        source = root / "image.bin"
        source.write_bytes(payload)
        original = root / "original.bin"
        original_payload = bytearray(payload)
        original_payload[4096] = 0xA5
        original.write_bytes(original_payload)
        return {
            "count": 3,
            "dimension": 512,
            "artifacts": {
                "extent_image": str(source),
                "oracle_image": str(original),
            },
            "staging": {
                "offset": 8192,
                "length": len(payload),
                "host_artifact": "extent_image",
                "magic": 0x314E415843,
            },
        }, payload

    def test_stage_selects_original_image_and_reports_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dataset, payload = self._dataset(root)
            selected = select_dataset_layout(dataset, "original")
            target = root / "vmem"
            target.write_bytes(bytes(selected["staging"]["offset"] + len(payload)))
            record = stage_image(
                selected, target, require_char_device=False, chunk_bytes=4096
            )
            offset = selected["staging"]["offset"]
            expected = Path(selected["artifacts"]["oracle_image"]).read_bytes()
            self.assertEqual(target.read_bytes()[offset : offset + len(expected)], expected)
            self.assertEqual(record["layout"], "original")
            self.assertEqual(record["host_artifact"], "oracle_image")

    def test_stage_copies_exact_image_at_configured_offset(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dataset, payload = self._dataset(root)
            target = root / "vmem"
            target.write_bytes(bytes(dataset["staging"]["offset"] + len(payload)))
            record = stage_image(dataset, target, require_char_device=False, chunk_bytes=4096)
            raw = target.read_bytes()
            offset = dataset["staging"]["offset"]
            self.assertEqual(raw[offset : offset + len(payload)], payload)
            self.assertEqual(record["copied_bytes"], len(payload))
            self.assertEqual(record["verified_slots"], 3)

    def test_stage_rejects_source_length_mismatch_before_write(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dataset, _ = self._dataset(root)
            dataset["staging"]["length"] += 4096
            target = root / "vmem"
            target.write_bytes(bytes(65536))
            before = target.read_bytes()
            with self.assertRaisesRegex(StageError, "source length"):
                stage_image(dataset, target, require_char_device=False)
            self.assertEqual(target.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
