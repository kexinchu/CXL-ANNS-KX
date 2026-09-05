import unittest

from experiments.eval.flashanns.validate_run import RunValidationError, validate_record, validate_same_search


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

    def test_rejects_oracle_record(self):
        with self.assertRaisesRegex(RunValidationError, "Oracle system is excluded"):
            validate_record(record("oracle"))

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


if __name__ == "__main__":
    unittest.main()
