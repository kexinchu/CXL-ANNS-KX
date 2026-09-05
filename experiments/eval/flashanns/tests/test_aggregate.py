import unittest

from experiments.eval.flashanns.aggregate import AggregationError, aggregate_records, bootstrap_ci


def make_records(phase="q2", states=("cold",), **dimensions):
    rows = []
    for repeat in range(5):
        cold_id = f"cold-{repeat}"
        for state in states:
            rows.append(
                {
                    "run_id": cold_id if state == "cold" else f"warm-{repeat}",
                    "dataset": "t2i10m", "phase": phase, "system": "flashanns", "state": state,
                    "L": 400, "repeat": repeat, "validation": {"status": "accepted"},
                    **dimensions,
                    "artifact_manifest_sha256": "artifact",
                    "sidecars": {"query_ids_sha256": "q", "candidate_ids_sha256": "c"},
                    "cold_parent_run_id": cold_id if state == "warm" else None,
                    "metrics": {"qps": 100 + repeat, "latency_p99_ms": 10 + repeat},
                }
            )
    return rows


class AggregateTest(unittest.TestCase):
    def test_bootstrap_is_deterministic(self):
        self.assertEqual(bootstrap_ci([1, 2, 3, 4, 5]), bootstrap_ci([1, 2, 3, 4, 5]))

    def test_requires_exactly_five_repeats(self):
        rows = aggregate_records(make_records())
        self.assertEqual(rows[0]["qps_median"], 102)
        with self.assertRaisesRegex(AggregationError, "five"):
            aggregate_records(make_records()[:-1])

    def test_rejects_hash_drift_and_rejected_runs(self):
        records = make_records()
        records[-1]["sidecars"]["candidate_ids_sha256"] = "drift"
        with self.assertRaisesRegex(AggregationError, "candidate"):
            aggregate_records(records)
        records = make_records()
        records[-1]["validation"]["status"] = "rejected"
        with self.assertRaisesRegex(AggregationError, "accepted"):
            aggregate_records(records)

    def test_rejects_oracle_records(self):
        records = make_records()
        for record in records:
            record["system"] = "oracle"
        with self.assertRaisesRegex(AggregationError, "Oracle system is excluded"):
            aggregate_records(records)

    def test_q4_requires_five_cold_warm_pairs(self):
        rows = aggregate_records(make_records("q4", ("cold", "warm")))
        self.assertEqual(len(rows), 2)
        records = make_records("q4", ("cold", "warm"))
        records[-1]["cold_parent_run_id"] = "missing"
        with self.assertRaisesRegex(AggregationError, "cold-parent"):
            aggregate_records(records)

    def test_preserves_phase_dimensions_in_separate_groups(self):
        records = make_records("q3_t8", threads=1) + make_records("q3_t8", threads=8)
        rows = aggregate_records(records)
        self.assertEqual([row["threads"] for row in rows], [1, 8])

    def test_normalizes_runtime_metric_names_for_the_figures(self):
        records = make_records()
        for record in records:
            record["nq"] = 100
            record["metrics"] = {
                "recall@10": 0.92,
                "throughput_QPS": 250,
                "mean": 4,
                "p50": 3,
                "p95": 7,
                "p99": 9,
                "crit_wait_ns": 200_000_000,
                "nvme_read_B": 104_857_600,
            }
        row = aggregate_records(records)[0]
        self.assertEqual(row["recall_at_10_median"], 0.92)
        self.assertEqual(row["qps_median"], 250)
        self.assertEqual(row["mean_latency_ms_median"], 4)
        self.assertEqual(row["latency_p50_ms_median"], 3)
        self.assertEqual(row["latency_p95_ms_median"], 7)
        self.assertEqual(row["latency_p99_ms_median"], 9)
        self.assertEqual(row["critical_wait_ms_median"], 2)
        self.assertEqual(row["nand_mib_per_query_median"], 1)


if __name__ == "__main__":
    unittest.main()
