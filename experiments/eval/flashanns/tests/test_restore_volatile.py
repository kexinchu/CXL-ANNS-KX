import unittest
import uuid
from datetime import datetime

from experiments.eval.flashanns import restore_volatile


ram_segments = restore_volatile.ram_segments


class RestoreVolatileTest(unittest.TestCase):
    def test_t2i_interval_contains_exactly_152_mib_of_ram(self):
        segments = ram_segments(
            offset=1_181_116_006_400,
            length=20_480_004_096,
            ram_size=30_064_771_072,
            ssd_size=3_840_766_820_352,
            stripe_size=2_097_152,
        )
        self.assertEqual(sum(segment.length for segment in segments), 152 * 1024**2)
        self.assertTrue(all(segment.length <= 2_097_152 for segment in segments))

    def test_clips_first_and_last_ram_stripes_to_dataset_interval(self):
        segments = ram_segments(
            offset=3,
            length=18,
            ram_size=16,
            ssd_size=16,
            stripe_size=8,
        )
        self.assertEqual(
            [(segment.logical_offset, segment.source_offset, segment.length) for segment in segments],
            [(8, 5, 8)],
        )

    def test_rejects_invalid_or_out_of_range_layout(self):
        bad = (
            dict(offset=-1, length=1, ram_size=8, ssd_size=8, stripe_size=8),
            dict(offset=0, length=17, ram_size=8, ssd_size=8, stripe_size=8),
            dict(offset=0, length=1, ram_size=8, ssd_size=8, stripe_size=0),
        )
        for values in bad:
            with self.subTest(values=values), self.assertRaises(ValueError):
                ram_segments(**values)

    def test_capture_identity_is_unique_and_parseable(self):
        self.assertTrue(
            hasattr(restore_volatile, "new_capture_identity"),
            "RAM restoration must mint a unique capture identity",
        )
        first = restore_volatile.new_capture_identity()
        second = restore_volatile.new_capture_identity()
        self.assertNotEqual(first["capture_id"], second["capture_id"])
        uuid.UUID(first["capture_id"])
        datetime.fromisoformat(first["captured_at_utc"].replace("Z", "+00:00"))


if __name__ == "__main__":
    unittest.main()
