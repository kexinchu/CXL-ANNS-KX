import copy
import unittest

from experiments.motivation.flashanns.run_c3_qd_campaign import (
    build_c3_qd_record,
    build_wise_spec,
    c3_qd_schedule,
    validate_c3_qd_campaign,
)


HASHES = {
    "binary": "1" * 64,
    "image": "2" * 64,
    "query": "3" * 64,
    "candidate_offsets": "4" * 64,
    "candidate_ids": "5" * 64,
    "result_ids": "6" * 64,
}


def record(dataset="t2i10m", threads=1, repeat=0):
    return build_c3_qd_record(
        run_id=f"test-{dataset}-t{threads}-r{repeat}",
        dataset=dataset,
        threads=threads,
        repeat=repeat,
        level={"t2i10m": 400, "yfcc10m": 50, "laion10m": 400}[dataset],
        identities=HASHES,
        requested_queries=10_000,
        completed_queries=10_000,
        elapsed_seconds=12.5,
        before_faults=10,
        after_faults=20,
        before_read_bytes=1_000,
        after_read_bytes=2_000,
        block_delta={
            "nvme3n1": {"read_sectors": 1, "write_sectors": 0},
            "nvme4n1": {"read_sectors": 1, "write_sectors": 0},
            "total_read_bytes": 1_000,
            "total_write_bytes": 0,
        },
        overlapping_processes=[],
        qd={
            "intervals": [
                {"seconds": 0.1, "aqu_sz": 0.0, "read_kib_s": 0.0},
                {"seconds": 0.1, "aqu_sz": 2.0, "read_kib_s": 10.0},
            ],
            "mean_aqu_sz": 1.0,
            "active_only": False,
            "source": "linux_block_weighted_io_ticks_100ms",
        },
        metrics={"recall@10": 0.92, "throughput_QPS": 100.0},
        command=["search_beam", "--threads", str(threads)],
        cold_evidence="cold.json",
        driver_sha256="7" * 64,
    )


class C3QdCampaignTest(unittest.TestCase):
    def test_wise_spec_uses_representative_recall_point_and_only_wise_prefetch(self):
        repo = __import__("pathlib").Path(__file__).resolve().parents[4]
        spec = build_wise_spec(
            repo=repo,
            dataset="yfcc10m",
            threads=16,
            repeat=3,
            tag="mot-c3qd-test",
            run_root=repo / "results" / "motivation" / "test-run",
        )
        self.assertEqual(spec["system"], "wise-only")
        self.assertEqual(spec["phase"], "q3_t8")
        self.assertEqual(spec["L"], 50)
        self.assertEqual(spec["nq"], 10_000)
        self.assertEqual(spec["threads"], 16)
        command = spec["command"]
        self.assertEqual(command[command.index("--threads") + 1], "16")
        self.assertIn("--no-steal-sched", command)
        self.assertIn("--extent-run", command)
        self.assertNotIn("--arrival-rate", command)

    def test_schedule_rotates_threads_and_covers_every_mark_five_times(self):
        schedule = c3_qd_schedule(
            ("t2i10m", "yfcc10m", "laion10m"), (1, 2, 4, 8, 16), repeats=5
        )
        self.assertEqual(len(schedule), 75)
        self.assertEqual(schedule[:5], [
            ("t2i10m", 0, 1), ("t2i10m", 0, 2),
            ("t2i10m", 0, 4), ("t2i10m", 0, 8),
            ("t2i10m", 0, 16),
        ])
        self.assertEqual(schedule[5:10], [
            ("t2i10m", 1, 2), ("t2i10m", 1, 4),
            ("t2i10m", 1, 8), ("t2i10m", 1, 16),
            ("t2i10m", 1, 1),
        ])
        marks = {(dataset, threads, repeat) for dataset, repeat, threads in schedule}
        self.assertEqual(len(marks), 75)

    def test_record_keeps_idle_qd_samples_and_frozen_identities(self):
        item = record()
        self.assertEqual(item["condition"], "t1")
        self.assertEqual(item["metrics"]["mean_aqu_sz"], 1.0)
        self.assertEqual(item["qd_samples"]["intervals"][0]["aqu_sz"], 0.0)
        self.assertEqual(item["sidecars"]["result_ids_sha256"], HASHES["result_ids"])
        self.assertEqual(item["identities"]["binary_sha256"], HASHES["binary"])

    def test_campaign_requires_all_three_datasets_threads_and_repeats(self):
        records = [
            record(dataset, threads, repeat)
            for dataset in ("t2i10m", "yfcc10m", "laion10m")
            for threads in (1, 2, 4, 8, 16)
            for repeat in range(5)
        ]
        validate_c3_qd_campaign(records)
        with self.assertRaisesRegex(ValueError, "complete"):
            validate_c3_qd_campaign(records[:-1])

    def test_campaign_rejects_active_only_qd_identity_and_recall_drift(self):
        records = [
            record(dataset, threads, repeat)
            for dataset in ("t2i10m", "yfcc10m", "laion10m")
            for threads in (1, 2, 4, 8, 16)
            for repeat in range(5)
        ]
        active_only = copy.deepcopy(records)
        active_only[0]["qd_samples"]["active_only"] = True
        with self.assertRaisesRegex(ValueError, "active-only"):
            validate_c3_qd_campaign(active_only)
        identity_drift = copy.deepcopy(records)
        identity_drift[1]["sidecars"]["candidate_ids_sha256"] = "8" * 64
        with self.assertRaisesRegex(ValueError, "candidate_ids_sha256"):
            validate_c3_qd_campaign(identity_drift)
        recall_drift = copy.deepcopy(records)
        recall_drift[1]["metrics"]["recall_at_10"] = 0.91
        with self.assertRaisesRegex(ValueError, "recall"):
            validate_c3_qd_campaign(recall_drift)

    def test_campaign_rejects_short_or_write_contaminated_points(self):
        item = record()
        item["operations"] = {"requested": 1_000, "completed": 1_000}
        item["metrics"]["elapsed_seconds"] = 29.9
        with self.assertRaisesRegex(ValueError, "2,000 queries or 30 seconds"):
            validate_c3_qd_campaign([item], require_complete=False)
        item = record()
        item["counters"]["block_delta"]["total_write_bytes"] = 4096
        item["exclusive"]["other_io_detected"] = True
        with self.assertRaisesRegex(ValueError, "other I/O"):
            validate_c3_qd_campaign([item], require_complete=False)


if __name__ == "__main__":
    unittest.main()
