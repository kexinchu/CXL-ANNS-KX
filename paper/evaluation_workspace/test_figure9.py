from __future__ import annotations

import importlib.util
import math
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parent
SCRIPT = ROOT / "figure9.py"


def load_figure9():
    spec = importlib.util.spec_from_file_location("figure9_standalone", SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import {SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Figure9StandaloneTest(unittest.TestCase):
    def test_embeds_complete_accepted_evidence_matrix(self):
        module = load_figure9()
        rows = module.FIGURE9_ROWS
        self.assertEqual(len(rows), 33)
        self.assertEqual(sum(row["phase"] == "q3_t1" for row in rows), 3)
        self.assertEqual(sum(row["phase"] == "q3_t8" for row in rows), 30)
        self.assertTrue(all(len(row["run_ids"]) == 5 for row in rows))
        run_ids = {run_id for row in rows for run_id in row["run_ids"]}
        self.assertEqual(len(run_ids), 165)
        module.validate_rows(rows)

    def test_has_no_runtime_evidence_file_dependency(self):
        source = SCRIPT.read_text()
        for forbidden in ("read_csv", "csv.DictReader", "accepted/", "validated.csv"):
            self.assertNotIn(forbidden, source)

    def test_embedded_primary_t8_values_match_paper(self):
        module = load_figure9()
        actual = {
            (row["dataset"], row["system"]): row["qps_median"]
            for row in module.FIGURE9_ROWS
            if row["phase"] == "q3_t8" and row["threads"] == 8
        }
        expected = {
            ("t2i10m", "flashanns"): 600.78,
            ("yfcc10m", "flashanns"): 3016.23,
            ("laion10m", "flashanns"): 421.24,
            ("t2i10m", "wise-only"): 559.17,
            ("yfcc10m", "wise-only"): 2933.21,
            ("laion10m", "wise-only"): 406.01,
        }
        self.assertEqual(set(actual), set(expected))
        for key, value in expected.items():
            self.assertTrue(math.isclose(actual[key], value, abs_tol=0.005), key)


if __name__ == "__main__":
    unittest.main()
