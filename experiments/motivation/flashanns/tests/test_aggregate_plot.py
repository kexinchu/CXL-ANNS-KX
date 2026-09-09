import csv
import json
from pathlib import Path
import tempfile
import unittest

import fitz

from experiments.motivation.flashanns.aggregate import aggregate_records, write_bundle
from experiments.motivation.flashanns.plot import FIGURE_NAMES, plot_all


class AggregatePlotTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo = Path(__file__).resolve().parents[4]
        cls.accepted = cls.repo / "results/motivation/flashanns-10m/accepted"

    def test_aggregate_uses_only_complete_five_run_marks(self):
        rows, provenance, quality = aggregate_records(self.accepted)
        self.assertEqual(len(rows), 41)
        self.assertTrue(all(row["n"] == 5 for row in rows))
        self.assertTrue(all(len(mark["run_ids"]) == 5 for mark in provenance["marks"]))
        self.assertEqual(quality["accepted_runs"], 205)
        self.assertEqual(quality["marks"], 41)
        self.assertEqual(quality["rejected_marks"], [])
        all_ids = [run_id for mark in provenance["marks"] for run_id in mark["run_ids"]]
        self.assertTrue(all(run_id.startswith("mot-") for run_id in all_ids))
        batching = [row for row in rows if row["phase"] == "c3_batching"]
        self.assertEqual([row["condition"] for row in batching],
                         ["dynamic", "single", "static"])
        self.assertTrue(all("physical_bandwidth_gib_s_median" in row for row in batching))
        self.assertTrue(all("bandwidth_utilization_pct_median" in row for row in batching))

    def test_bundle_is_rerunnable_without_raw_device_access(self):
        rows, provenance, quality = aggregate_records(self.accepted)
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            write_bundle(rows, provenance, quality, out)
            with (out / "motivation.csv").open(encoding="utf-8") as stream:
                csv_rows = list(csv.DictReader(stream))
            self.assertEqual(len(csv_rows), 41)
            self.assertEqual(
                json.loads((out / "provenance.json").read_text())["marks"],
                provenance["marks"],
            )

    def test_plot_writes_four_one_page_vector_pdfs_from_csv(self):
        rows, provenance, quality = aggregate_records(self.accepted)
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            write_bundle(rows, provenance, quality, out)
            figures = out / "figures"
            plot_all(out / "motivation.csv", figures)
            self.assertEqual({path.name for path in figures.glob("*.pdf")}, set(FIGURE_NAMES))
            for name in FIGURE_NAMES:
                with fitz.open(figures / name) as document:
                    self.assertEqual(document.page_count, 1)
                    page = document[0]
                    self.assertTrue(page.get_text().strip())
                    self.assertFalse(page.get_images(full=True), name)
            with fitz.open(figures / "mot-c3-io-concurrency.pdf") as document:
                page = document[0]
                text = page.get_text()
                self.assertAlmostEqual(page.rect.width, 3.35 * 72, places=1)
                self.assertAlmostEqual(page.rect.height, 2.45 * 72, places=1)
                for label in ("Single", "Static", "Dynamic", "Bandwidth utilization",
                              "Throughput"):
                    self.assertIn(label, text)
                self.assertNotIn("Effective queue depth", text)
            for name in ("mot-pathology-admission.pdf", "mot-pathology-prefetch.pdf"):
                with fitz.open(figures / name) as document:
                    page = document[0]
                    self.assertAlmostEqual(page.rect.width, 5.12 * 72, places=1)
                    self.assertAlmostEqual(page.rect.height, 2.88 * 72, places=1)
                    spans = [
                        span
                        for block in page.get_text("dict")["blocks"] if "lines" in block
                        for line in block["lines"] for span in line["spans"]
                        if span["text"].strip()
                    ]
                    self.assertGreaterEqual(min(span["size"] for span in spans), 11.5)


if __name__ == "__main__":
    unittest.main()
