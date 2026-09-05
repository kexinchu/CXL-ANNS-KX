import json
import tempfile
import unittest
from pathlib import Path

from experiments.eval.flashanns import validate_run


RunValidationError = validate_run.RunValidationError
validate_record = validate_run.validate_record
validate_same_search = validate_run.validate_same_search


def record(system="flashanns"):
    return {
        "run_id": f"run-{system}",
        "dataset": "t2i10m",
        "metric": "mips",
        "phase": "smoke",
        "system": system,
        "state": "proof",
        "L": 400,
        "k": 10,
        "nq": 100,
        "repeat": 0,
        "command": ["binary"],
        "git": "abc",
        "binary_sha256": "b",
        "artifact_manifest_sha256": "a",
        "preflight_before": {"cache_limit": 4294967296},
        "preflight_after": {"cache_limit": 4294967296},
        "device_before": {},
        "device_after": {},
        "metrics": {"completed_queries": 100, "nvme_read_B": 1, "score_bounce": 0, "score_flash": 0, "recall@10": 0.938},
        "sidecars": {"query_ids_sha256": "q", "candidate_offsets_sha256": "o", "candidate_ids_sha256": "c", "result_ids_sha256": "r"},
        "validation": {"status": "pending"},
    }


class ValidateRunTest(unittest.TestCase):
    def test_accepts_complete_record(self):
        validate_record(record())

    def test_rejects_missing_queries_non_4gib_and_forbidden_paths(self):
        bad = record()
        bad["metrics"].update(completed_queries=99, score_bounce=1, score_flash=2)
        bad["preflight_before"]["cache_limit"] = 1
        with self.assertRaises(RunValidationError) as caught:
            validate_record(bad)
        text = str(caught.exception)
        for needle in ("completed_queries", "cache_limit", "score_bounce", "score_flash"):
            self.assertIn(needle, text)

    def test_cache_sensitivity_accepts_only_declared_capacity(self):
        item = record()
        item.update(phase="q4_cache", cache_gib=2)
        item["preflight_before"]["cache_limit"] = 2 * 1024**3
        item["preflight_after"]["cache_limit"] = 2 * 1024**3
        validate_record(item)
        item["preflight_after"]["cache_limit"] = 3 * 1024**3
        with self.assertRaisesRegex(RunValidationError, "cache_limit"):
            validate_record(item)

    def test_rejects_oracle_record(self):
        with self.assertRaisesRegex(RunValidationError, "Oracle system is excluded"):
            validate_record(record("oracle"))

    def test_external_pipeann_does_not_claim_internal_candidate_trace(self):
        item = record("pipeann")
        item["external"] = True
        item["sidecars"]["candidate_offsets_sha256"] = None
        item["sidecars"]["candidate_ids_sha256"] = None
        item["preflight_before"] = {"external": True}
        item["preflight_after"] = {"external": True}
        validate_record(item)

    def test_open_loop_run_requires_rate_tail_latency_and_latency_trace(self):
        item = record("pipeann")
        item.update(phase="q3_load", external=True, arrival_rate=500.0)
        item["preflight_before"] = {"external": True}
        item["preflight_after"] = {"external": True}
        item["sidecars"].update(candidate_offsets_sha256=None,
                                candidate_ids_sha256=None,
                                latency_ns_sha256="lat")
        item["metrics"].update(latency_p99_ms=15.0, queue_wait_ms_mean=2.0,
                               offered_QPS=500.0)
        validate_record(item)
        del item["metrics"]["latency_p99_ms"]
        with self.assertRaisesRegex(RunValidationError, "latency_p99_ms"):
            validate_record(item)

    def test_same_search_requires_all_identity_hashes(self):
        left, right = record("demand"), record("flashanns")
        validate_same_search([left, right])
        right["sidecars"]["candidate_ids_sha256"] = "different"
        with self.assertRaisesRegex(RunValidationError, "candidate_ids_sha256"):
            validate_same_search([left, right])

    def test_same_search_requires_equal_recall(self):
        left, right = record("demand"), record("flashanns")
        right["metrics"]["recall@10"] = 0.937
        with self.assertRaisesRegex(RunValidationError, "recall@10"):
            validate_same_search([left, right])

    def test_load_records_recurses_over_run_json_only(self):
        self.assertTrue(
            hasattr(validate_run, "load_records"),
            "validator must load run records from directories",
        )
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for index, system in enumerate(("demand", "flashanns")):
                path = root / str(index) / "run.json"
                path.parent.mkdir()
                path.write_text(json.dumps(record(system)))
            (root / "ignore.json").write_text("not a run")
            loaded = validate_run.load_records([root])
        self.assertEqual([item["system"] for item in loaded], ["demand", "flashanns"])

    def test_load_records_reparses_internal_stdout_observation_fields(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "run"
            root.mkdir()
            item = record()
            item["command"] = ["/repo/serving/search_beam"]
            item["metrics"]["mean"] = 64.16
            (root / "run.json").write_text(json.dumps(item))
            (root / "stdout.log").write_text(
                "latency_ms mean=33.700 p50=30.000 p90=40.000 p95=45.000 p99=50.000\n"
                "recall@10=0.938\npage_occ mean=64.160% pages=10\n"
            )
            loaded = validate_run.load_records([root])
        self.assertEqual(loaded[0]["metrics"]["mean_latency_ms"], 33.7)

    def test_freeze_anchors_selects_nearest_measurement_at_or_above_target(self):
        self.assertTrue(
            hasattr(validate_run, "freeze_anchors"),
            "validator must freeze recall anchors",
        )
        rows = []
        for system, points in {
            "demand": ((50, 0.88), (100, 0.91), (200, 0.94)),
            "flashanns": ((50, 0.87), (200, 0.905), (400, 0.93)),
        }.items():
            for level, recall in points:
                item = record(system)
                item.update(phase="calibration", L=level, nq=500)
                item["metrics"].update(completed_queries=500, **{"recall@10": recall})
                item["run_id"] = f"cal-{system}-{level}"
                rows.append(item)

        frozen = validate_run.freeze_anchors(rows, 0.90, 0.92)
        self.assertEqual(frozen["primary"]["demand"]["L"], 100)
        self.assertEqual(frozen["primary"]["flashanns"]["L"], 200)
        self.assertEqual(frozen["extra"]["demand"]["L"], 200)
        self.assertEqual(frozen["extra"]["flashanns"]["L"], 400)

    def test_freeze_anchors_rejects_uncovered_target(self):
        self.assertTrue(
            hasattr(validate_run, "freeze_anchors"),
            "validator must freeze recall anchors",
        )
        item = record("demand")
        item.update(phase="calibration", L=50, nq=500)
        item["metrics"].update(completed_queries=500, **{"recall@10": 0.89})
        with self.assertRaisesRegex(RunValidationError, "does not cover"):
            validate_run.freeze_anchors([item], 0.90)

    def test_seal_records_writes_accepted_copies_without_mutating_raw(self):
        item = record()
        with tempfile.TemporaryDirectory() as tmp:
            paths = validate_run.seal_records([item], Path(tmp))
            sealed = json.loads(paths[0].read_text())
        self.assertEqual(item["validation"]["status"], "pending")
        self.assertEqual(sealed["validation"]["status"], "accepted")
        self.assertEqual(sealed["validation"]["source_status"], "pending")


if __name__ == "__main__":
    unittest.main()
