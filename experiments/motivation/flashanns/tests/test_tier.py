import unittest

from experiments.motivation.flashanns.run_tier import (
    classify_tier,
    percentile,
    select_offsets,
    validate_probe_geometry,
)


class TierTest(unittest.TestCase):
    def test_select_offsets_is_deterministic_distinct_and_bounded(self):
        args = dict(region_offset=2 * 1024**2, region_length=64 * 1024**2,
                    record_stride=2048, count=2000, seed=42)
        first = select_offsets(**args)
        self.assertEqual(first, select_offsets(**args))
        self.assertEqual(len(first), 2000)
        self.assertEqual(len({offset // 4096 for offset in first}), 2000)
        self.assertTrue(all(args["region_offset"] <= offset for offset in first))
        self.assertTrue(all(offset + args["record_stride"] <=
                            args["region_offset"] + args["region_length"] for offset in first))

    def test_record_stride_and_payload_are_independent(self):
        validate_probe_geometry(record_stride=2048, payload_bytes=800)
        validate_probe_geometry(record_stride=4096, payload_bytes=2048)
        with self.assertRaisesRegex(ValueError, "payload"):
            validate_probe_geometry(record_stride=2048, payload_bytes=4096)

    def test_percentile_uses_linear_interpolation(self):
        values = [1.0, 2.0, 3.0, 4.0]
        self.assertEqual(percentile(values, 0), 1.0)
        self.assertEqual(percentile(values, 50), 2.5)
        self.assertEqual(percentile(values, 100), 4.0)

    def test_classification_gates(self):
        self.assertEqual(classify_tier(0, 0), "host")
        self.assertEqual(classify_tier(1, 0), "cxl_cache")
        self.assertEqual(classify_tier(1, 4096), "flash")
        with self.assertRaisesRegex(ValueError, "inconsistent"):
            classify_tier(0, 4096)

    def test_selection_rejects_impossible_count(self):
        with self.assertRaisesRegex(ValueError, "distinct"):
            select_offsets(0, 4096, 800, 2, 42)


if __name__ == "__main__":
    unittest.main()
