import unittest

from experiments.eval.flashanns.aggregate import AggregationError, aggregate_records, bootstrap_ci


def make_records(phase="q2", states=("cold",)):
    rows = []
    for repeat in range(5):
        cold_id = f"cold-{repeat}"
        for state in states:
            rows.append(
                {
                    "run_id": cold_id if state == "cold" else f"warm-{repeat}",
                    "dataset": "t2i10m", "phase": phase, "system": "flashanns", "state": state,
                    "L": 400, "repeat": repeat, "validation": {"status": "accepted"},
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

    def test_q4_requires_five_cold_warm_pairs(self):
        rows = aggregate_records(make_records("q4", ("cold", "warm")))
        self.assertEqual(len(rows), 2)
        records = make_records("q4", ("cold", "warm"))
        records[-1]["cold_parent_run_id"] = "missing"
        with self.assertRaisesRegex(AggregationError, "cold-parent"):
            aggregate_records(records)


if __name__ == "__main__":
    unittest.main()
