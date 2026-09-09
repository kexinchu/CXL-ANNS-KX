import csv
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from experiments.eval.flashanns.supplemental_figures import (
    SupplementalDataError,
    curate_records,
    render_figures,
    write_bundle,
)


class SupplementalFigureTest(unittest.TestCase):
    def _record(self, *, dataset="t2i10m", phase="q3_load",
                system="flashanns", rate=10.0, repeat=0, tag="tag",
                binary="binary", level=400):
        return {
            "run_id": f"{dataset}-{phase}-{system}-{rate}-r{repeat}-{tag}",
            "dataset": dataset, "phase": phase, "system": system,
            "arrival_rate": rate, "repeat": repeat, "campaign_tag": tag,
            "binary_sha256": binary, "artifact_manifest_sha256": "artifact",
            "L": level, "nq": 10000, "validation": {"status": "accepted"},
            "sidecars": {"query_ids_sha256": "queries",
                         "candidate_ids_sha256": "candidates"},
            "metrics": {
                "completed_queries": 10000, "offered_QPS": rate,
                "throughput_QPS": rate - 1, "mean_latency_ms": 2,
                "latency_p50_ms": 1, "latency_p95_ms": 3,
                "latency_p99_ms": 4, "queue_wait_ms_mean": 0.1,
                "queue_wait_ms_p95": 0.2, "recall@10": 0.92,
                "threads": 1,
                "requested_pages": 1000, "nand_read_bytes": 4096000,
                "nand_read_commands": 900, "slots_on_pages": 1000,
                "slots_scored": 500, "slot_use_pct": 50,
            },
        }

    def _contract(self):
        return {
            "expected_nq": 10000, "repeats": [0, 1, 2, 3, 4],
            "load": {"systems": ["flashanns"], "datasets": {
                "t2i10m": {"campaign_tag": "tag", "L": {"flashanns": 400},
                            "rates": [10.0],
                            "binary_sha256": {"flashanns": "binary"}}
            }},
            "io": {"dataset": "t2i10m", "L": 400, "dimension": 200,
                   "element_bytes": 4, "stages": []},
        }

    def test_curates_exact_five_repeat_load_mark(self):
        rows, provenance, report = curate_records(
            [self._record(repeat=i) for i in range(5)], self._contract()
        )
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["kind"], "load")
        self.assertEqual(rows[0]["latency_p99_ms_median"], 4)
        self.assertEqual(len(provenance["marks"][0]["run_ids"]), 5)
        self.assertEqual(report["load_marks"], 1)

    def test_load_selector_uses_per_system_recall_matched_L(self):
        contract = self._contract()
        contract["load"]["systems"] = ["flashanns", "pipeann"]
        contract["load"]["datasets"]["t2i10m"]["L"]["pipeann"] = 2400
        contract["load"]["datasets"]["t2i10m"]["binary_sha256"]["pipeann"] = "pipe"
        records = [self._record(repeat=i) for i in range(5)]
        records.extend(self._record(system="pipeann", repeat=i, binary="pipe",
                                    level=2400) for i in range(5))
        rows, _, _ = curate_records(records, contract)
        self.assertEqual({(row["system"], row["L"]) for row in rows},
                         {("flashanns", 400), ("pipeann", 2400)})

    def test_rejects_missing_repeat_and_incomplete_query(self):
        with self.assertRaisesRegex(SupplementalDataError, "repeats"):
            curate_records([self._record(repeat=i) for i in range(4)], self._contract())
        records = [self._record(repeat=i) for i in range(5)]
        records[-1]["metrics"]["completed_queries"] = 9999
        with self.assertRaisesRegex(SupplementalDataError, "completed_queries"):
            curate_records(records, self._contract())

    def test_rejects_identity_drift(self):
        records = [self._record(repeat=i) for i in range(5)]
        records[-1]["sidecars"]["query_ids_sha256"] = "other"
        with self.assertRaisesRegex(SupplementalDataError, "query_ids_sha256"):
            curate_records(records, self._contract())

    def test_derives_physical_read_amplification(self):
        contract = self._contract()
        contract["load"]["datasets"] = {}
        contract["io"]["stages"] = [{
            "name": "stage", "label": "Stage", "phase": "q3_t1",
            "system": "flashanns", "campaign_tag": "tag",
            "threads": 1, "binary_sha256": "binary",
        }]
        records = [self._record(phase="q3_t1", rate=0, repeat=i)
                   for i in range(5)]
        rows, _, report = curate_records(records, contract)
        self.assertEqual(rows[0]["physical_read_amplification_median"], 10.24)
        self.assertAlmostEqual(rows[0]["physical_kib_per_read_median"],
                               4096000 / 900 / 1024)
        self.assertEqual(report["io_marks"], 1)

    def test_io_selector_rejects_wrong_concurrency(self):
        contract = self._contract()
        contract["load"]["datasets"] = {}
        contract["io"]["stages"] = [{
            "name": "stage", "label": "Stage", "phase": "q3_t1",
            "system": "flashanns", "campaign_tag": "tag",
            "threads": 8, "binary_sha256": "binary",
        }]
        with self.assertRaisesRegex(SupplementalDataError, "repeats"):
            curate_records([self._record(phase="q3_t1", rate=0, repeat=i)
                            for i in range(5)], contract)

    def test_writes_csv_provenance_report_and_two_vector_pdfs(self):
        rows = []
        for dataset in ("t2i10m", "yfcc10m", "laion10m"):
            for system in ("wise-only", "pipeann", "flashanns"):
                for rate in (1.0, 2.0):
                    rows.append({"kind": "load", "dataset": dataset,
                                 "system": system, "offered_qps": rate,
                                 "qps_median": rate * 0.9,
                                 "latency_p99_ms_median": 10 * rate,
                                 "latency_p99_ms_ci_low": 9 * rate,
                                 "latency_p99_ms_ci_high": 11 * rate,
                                 "run_ids": ";".join(f"r{i}" for i in range(5))})
        for index, label in enumerate(("Original layout", "Co-use layout",
                                       "Serialized", "Post-commit batch",
                                       "Extent reads")):
            rows.append({"kind": "io", "stage_order": index,
                         "stage_label": label, "requested_pages_per_query_median": 100-index,
                         "nand_mib_per_query_median": 1-index/10,
                         "vector_slot_use_pct_median": 60+index,
                         "physical_read_amplification_median": 5-index/10,
                         "nand_commands_per_query_median": 200-index*10,
                         "physical_kib_per_read_median": 4+index/10,
                         "qps_median": 30+index*20,
                         "run_ids": ";".join(f"i{index}r{i}" for i in range(5))})
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            write_bundle(rows, {"marks": []}, {"load_marks": 18, "io_marks": 4}, root)
            render_figures(root / "validated.csv", root / "figures")
            self.assertTrue((root / "provenance.json").exists())
            self.assertTrue((root / "quality-report.json").exists())
            with (root / "validated.csv").open(newline="") as src:
                self.assertEqual(len(list(csv.DictReader(src))), 23)
            for name in ("load-tail-latency.pdf", "prefetch-io-efficiency.pdf"):
                path = root / "figures" / name
                self.assertTrue(path.read_bytes().startswith(b"%PDF"))
                info = subprocess.check_output(["pdfinfo", str(path)], text=True)
                self.assertIn("Pages:           1", info)


if __name__ == "__main__":
    unittest.main()
