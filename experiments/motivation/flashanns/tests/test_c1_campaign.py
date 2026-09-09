import unittest

from experiments.motivation.flashanns.run_c1_campaign import (
    latency_metrics,
    partition_offsets,
)


class C1CampaignTest(unittest.TestCase):
    def test_partition_offsets_is_disjoint_and_complete(self):
        groups = partition_offsets(
            region_offset=4096,
            region_length=64 * 1024**2,
            record_stride=2048,
            repeats=5,
            count=2000,
            seed=42,
        )
        self.assertEqual([len(group) for group in groups], [2000] * 5)
        pages = [{offset // 4096 for offset in group} for group in groups]
        self.assertEqual(len(set().union(*pages)), 10000)

    def test_latency_metrics_preserves_mean_and_tail(self):
        metrics = latency_metrics([1000, 2000, 3000, 4000])
        self.assertEqual(metrics["latency_us_mean"], 2.5)
        self.assertEqual(metrics["latency_us_p50"], 2.5)
        self.assertAlmostEqual(metrics["latency_us_p95"], 3.85)
        self.assertAlmostEqual(metrics["latency_us_p99"], 3.97)


if __name__ == "__main__":
    unittest.main()
