import copy
import unittest
from pathlib import Path

from experiments.motivation.flashanns.run_batching import (
    batching_schedule,
    build_batching_record,
    build_batching_spec,
    empirical_peak_gib_s,
    physical_bandwidth_gib_s,
    validate_batching_campaign,
)


HASHES = {
    "binary": "1" * 64,
    "image": "2" * 64,
    "query": "3" * 64,
    "candidate_offsets": "4" * 64,
    "candidate_ids": "5" * 64,
    "result_ids": "6" * 64,
}


def record(condition="single", repeat=0, recall=0.9293):
    command = ["search_beam", "--threads", "1" if condition == "single" else "8"]
    if condition == "static":
        command += ["--static-cohort", "8"]
    return build_batching_record(
        run_id=f"test-{condition}-r{repeat}", condition=condition, repeat=repeat,
        identities=HASHES, requested_queries=10_000, completed_queries=10_000,
        before_faults=0, after_faults=100, before_read_bytes=1_000,
        after_read_bytes=2_000, block_delta={
            "nvme3n1": {"read_sectors": 100, "write_sectors": 0},
            "nvme4n1": {"read_sectors": 100, "write_sectors": 0},
            "total_read_bytes": 1_073_741_824,
            "total_write_bytes": 0,
        }, overlapping_processes=[], metrics={
            "recall@10": recall, "throughput_QPS": 100.0, "wall_s": 2.0,
            "completed_queries": 10_000,
        }, command=command, cold_evidence="cold.json",
        driver_sha256="7" * 64, empirical_peak=1.0,
        qd_samples={
            "intervals": [
                {"seconds": 0.1, "aqu_sz": 0.0, "read_kib_s": 262_144.0},
                {"seconds": 0.1, "aqu_sz": 1.0, "read_kib_s": 524_288.0},
            ],
            "mean_aqu_sz": 0.5,
            "active_only": False,
            "source": "linux_block_weighted_io_ticks_100ms",
        },
    )


class BatchingCampaignTest(unittest.TestCase):
    def test_schedule_rotates_all_three_conditions_five_times(self):
        schedule = batching_schedule(repeats=5)
        self.assertEqual(len(schedule), 15)
        self.assertEqual(schedule[:3], [(0, "single"), (0, "static"), (0, "dynamic")])
        self.assertEqual(schedule[3:6], [(1, "static"), (1, "dynamic"), (1, "single")])
        self.assertEqual(len(set(schedule)), 15)

    def test_commands_differ_only_by_threads_and_static_gate(self):
        repo = Path(__file__).resolve().parents[4]
        specs = {
            condition: build_batching_spec(
                repo=repo, condition=condition, repeat=0, tag="test",
                run_root=repo / "results" / f"test-{condition}",
                binary=repo / "serving" / "search_beam",
            )
            for condition in ("single", "static", "dynamic")
        }
        self.assertEqual(specs["single"]["threads"], 1)
        self.assertEqual(specs["static"]["threads"], 8)
        self.assertEqual(specs["dynamic"]["threads"], 8)
        self.assertIn("--steal-sched", specs["static"]["command"])
        self.assertIn("--steal-sched", specs["dynamic"]["command"])
        self.assertEqual(
            specs["static"]["command"][specs["static"]["command"].index("--static-cohort") + 1],
            "8",
        )
        self.assertNotIn("--static-cohort", specs["dynamic"]["command"])

    def test_physical_bandwidth_and_empirical_peak_are_measured(self):
        self.assertEqual(physical_bandwidth_gib_s(2 * 1024**3, 4.0), 0.5)
        capability = [
            {"metrics": {"bandwidth_gib_s": value}}
            for value in (0.8, 1.0, 0.9, 1.1, 1.0)
        ]
        self.assertEqual(empirical_peak_gib_s(capability), 1.0)

    def test_campaign_requires_exact_block_and_matched_recall(self):
        records = [record(condition, repeat) for repeat, condition in batching_schedule(5)]
        validate_batching_campaign(records)
        with self.assertRaisesRegex(ValueError, "complete"):
            validate_batching_campaign(records[:-1])
        drift = copy.deepcopy(records)
        drift[-1]["metrics"]["recall_at_10"] = 0.90
        with self.assertRaisesRegex(ValueError, "recall"):
            validate_batching_campaign(drift)

    def test_campaign_rejects_write_contamination(self):
        item = record()
        item["counters"]["block_delta"]["total_write_bytes"] = 4096
        item["exclusive"]["other_io_detected"] = True
        with self.assertRaisesRegex(ValueError, "other I/O"):
            validate_batching_campaign([item], require_complete=False)

    def test_record_keeps_frozen_100ms_timeline_samples(self):
        item = record()
        self.assertEqual(item["qd_samples"]["intervals"][0]["read_kib_s"], 262_144.0)
        validate_batching_campaign([item], require_complete=False)

        missing = copy.deepcopy(item)
        missing.pop("qd_samples")
        with self.assertRaisesRegex(ValueError, "100-ms sampler"):
            validate_batching_campaign([missing], require_complete=False)

        active_only = copy.deepcopy(item)
        active_only["qd_samples"]["active_only"] = True
        with self.assertRaisesRegex(ValueError, "entire interval"):
            validate_batching_campaign([active_only], require_complete=False)

        malformed = copy.deepcopy(item)
        malformed["qd_samples"]["intervals"][0]["read_kib_s"] = -1.0
        with self.assertRaisesRegex(ValueError, "read rate"):
            validate_batching_campaign([malformed], require_complete=False)


if __name__ == "__main__":
    unittest.main()
