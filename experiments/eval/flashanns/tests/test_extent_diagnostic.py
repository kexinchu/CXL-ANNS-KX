import tempfile
import unittest
from pathlib import Path

from experiments.eval.flashanns.extent_diagnostic import (
    DiagnosticError,
    classify_limit,
    locality_metrics,
    validate_pairs,
)


class ExtentDiagnosticTest(unittest.TestCase):
    def test_locality_metrics_measure_contiguous_physical_runs(self):
        # Logical IDs map to slots; two 2-KiB slots share one 4-KiB page.
        slot_map = [0, 2, 4, 8, 10, 12]
        offsets = [0, 4, 6]
        ids = [0, 1, 2, 3, 4, 5]
        result = locality_metrics(offsets, ids, slot_map)
        self.assertEqual(result["queries"], 2)
        self.assertEqual(result["unique_pages"], 6)
        self.assertEqual(result["contiguous_runs"], 3)
        self.assertAlmostEqual(result["ideal_command_reduction_pct"], 50.0)
        self.assertAlmostEqual(result["pages_in_multipage_runs_pct"], 100.0 * 5 / 6)

    def _record(self, level, system, repeat, query_hash="q", candidate_hash="c"):
        commands = 1000 if system == "batch-t1" else 900
        return {
            "run_id": f"L{level}-{system}-r{repeat}",
            "dataset": "t2i10m", "phase": "extent_diagnostic",
            "system": system, "L": level, "nq": 1000, "repeat": repeat,
            "binary_sha256": "binary", "state": "cold", "threads": 1,
            "validation": {"returncode": 0},
            "preflight_before": {"cache_used": 0, "dirty_bytes": 0,
                                  "io_errors": 0},
            "metrics": {"completed_queries": 1000, "score_flash": 0,
                        "score_bounce": 0, "nand_read_commands": commands,
                        "nand_read_bytes": 4096 * commands,
                        "extent_extra_pages": 0, "throughput_QPS": 10,
                        "recall@10": 0.92},
            "sidecars": {"query_ids_sha256": query_hash,
                         "candidate_ids_sha256": candidate_hash},
        }

    def test_validate_pairs_requires_complete_matched_matrix(self):
        records = [
            self._record(level, system, repeat)
            for level in (400, 800, 1600)
            for system in ("batch-t1", "extent-t1")
            for repeat in range(3)
        ]
        groups = validate_pairs(records, expected_binary="binary")
        self.assertEqual(len(groups), 9)
        records[-1]["sidecars"]["candidate_ids_sha256"] = "drift"
        with self.assertRaisesRegex(DiagnosticError, "candidate hash"):
            validate_pairs(records, expected_binary="binary")

    def test_classification_distinguishes_locality_and_implementation_limits(self):
        self.assertEqual(classify_limit(11.0, 7.0), "locality-limited")
        self.assertEqual(classify_limit(30.0, 4.0), "implementation-limited")
        self.assertEqual(classify_limit(18.0, 7.0), "mixed")


if __name__ == "__main__":
    unittest.main()
