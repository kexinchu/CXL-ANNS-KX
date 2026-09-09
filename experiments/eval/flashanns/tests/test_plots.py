import csv
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

from experiments.eval.flashanns.plot_six_figures import (
    COLORS,
    FigureDataError,
    _frontier,
    _physical_nand_mib_per_query,
    _replace_phase_rows,
    _t2i_phase,
    render_figures,
    validate_figure_rows,
)
from experiments.eval.flashanns.plot_q2_q4 import render_figures as render_provisional


class PlotTest(unittest.TestCase):
    def _row(self, **updates):
        row = {
            "dataset": "t2i10m", "phase": "q2", "system": "flashanns",
            "state": "cold", "L": 400, "threads": 8, "arrival_rate": 0,
            "cache_gib": 4, "recall_at_10_median": 0.92,
            "mean_latency_ms_median": 10, "mean_latency_ms_ci_low": 9,
            "mean_latency_ms_ci_high": 11, "qps_median": 100,
            "qps_ci_low": 90, "qps_ci_high": 110,
            "latency_p99_ms_median": 15, "nand_occupancy_pct_median": 60,
            "nand_read_bytes_median": 104857600, "nq_median": 100,
            "useful_bandwidth_mib_s_median": 500, "mean_inflight_median": 4,
            "committed_candidates_median": 400, "missing_pages_median": 300,
            "issued_pages_per_query_median": 320, "nand_mib_per_query_median": 1.5,
            "nand_commands_per_query_median": 7, "vector_slot_use_pct_median": 55,
            "pq_nav_ms_median": 3,
            "critical_wait_ms_median": 4, "exact_rerank_ms_median": 2,
            "coverage_wait_ms_median": 3.25,
            "slot_backpressure_wait_ms_median": 0.75,
            "host_data_stall_ms_median": 4,
            "score_triggered_flash_fills_median": 0,
            "score_prematerialized_pct_median": 100,
            "queue_wait_ms_median": 1, "score_host_pct_median": 75,
            "score_cxl_pct_median": 25, "score_bounce_pct_median": 0,
            "score_flash_pct_median": 0, "flash_fills_per_query_median": 2,
            "run_ids": ";".join(f"accepted-run-{index}" for index in range(5)),
        }
        row.update(updates)
        return row

    def test_oracle_has_no_plot_style(self):
        self.assertNotIn("oracle", COLORS)

    def test_replaces_only_the_declared_phase(self):
        old_hide = self._row(phase="q4_hide", run_ids="old;old;old;old;old")
        keep = self._row(phase="q4_cold_warm")
        new_hide = self._row(
            phase="q4_hide", run_ids="new;new;new;new;new",
            host_data_stall_ms_median=1,
        )
        merged = _replace_phase_rows([old_hide, keep], [new_hide], "q4_hide")
        self.assertEqual(merged, [keep, new_hide])


    def test_physical_nand_mib_uses_device_bytes_not_legacy_alias(self):
        row = self._row(nand_mib_per_query_median=999)
        self.assertEqual(_physical_nand_mib_per_query(row), 1)

    def test_frontier_distinguishes_original_and_optimized_demand(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "frontier.pdf"
            rows = [
                self._row(system="demand", L=100, recall_at_10_median=0.85),
                self._row(system="demand", L=400, recall_at_10_median=0.92),
                self._row(phase="q2_original", system="demand-orc", L=100,
                          recall_at_10_median=0.85, mean_latency_ms_median=20,
                          qps_median=50),
                self._row(phase="q2_original", system="demand-orc", L=400,
                          recall_at_10_median=0.92, mean_latency_ms_median=30,
                          qps_median=35),
            ]
            _frontier(rows, "t2i10m", path)
            text = subprocess.check_output(["pdftotext", str(path), "-"], text=True)
            self.assertIn("Demand (original layout)", text)
            self.assertIn("Demand (optimized layout)", text)

    def test_representative_ablation_rows_are_t2i_only(self):
        rows = [
            self._row(dataset="t2i10m", phase="q3_t8"),
            self._row(dataset="yfcc10m", phase="q3_t8"),
            self._row(dataset="laion10m", phase="q4_hide"),
        ]
        self.assertEqual(_t2i_phase(rows, "q3_t8"), [rows[0]])

    def test_rejects_q3_load_and_non_five_repeat_marks(self):
        with self.assertRaisesRegex(FigureDataError, "q3_load"):
            validate_figure_rows([self._row(phase="q3_load")])
        with self.assertRaisesRegex(FigureDataError, "five accepted runs"):
            validate_figure_rows([self._row(run_ids="run-0;run-1")])

    def test_requires_t8_evidence_for_all_three_datasets(self):
        rows = []
        for dataset in ("t2i10m", "yfcc10m", "laion10m"):
            for system in ("wise-only", "flashanns"):
                rows.append(self._row(dataset=dataset, phase="q3_t8",
                                      system=system, threads=8))
        validate_figure_rows(rows, require_complete=False)
        with self.assertRaisesRegex(FigureDataError, "LAION-10M.*T=8"):
            validate_figure_rows(rows[:-1], require_complete=False)

    def test_writes_six_one_page_vector_pdfs(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            csv_path = root / "validated.csv"
            rows = []
            for dataset in ("t2i10m", "yfcc10m", "laion10m"):
                for system in ("demand", "pipeann", "flashanns"):
                    for L, recall in ((100, 0.85), (400, 0.92)):
                        rows.append(self._row(dataset=dataset, system=system, L=L,
                                              recall_at_10_median=recall))
                for L, recall in ((100, 0.85), (400, 0.92)):
                    rows.append(self._row(dataset=dataset, phase="q2_original",
                                          system="demand-orc", L=L,
                                          recall_at_10_median=recall))
            for system in ("serial-t1", "batch-t1", "extent-t1"):
                rows.append(self._row(phase="q3_t1", system=system, threads=1))
            for dataset in ("t2i10m", "yfcc10m", "laion10m"):
                for threads in (1, 2, 4, 8, 16):
                    rows.append(self._row(dataset=dataset, phase="q3_t8",
                                          system="wise-only", threads=threads))
                    rows.append(self._row(dataset=dataset, phase="q3_t8",
                                          system="flashanns", threads=threads,
                                          qps_median=100 * threads))
            for dataset in ("t2i10m", "yfcc10m", "laion10m"):
                for system in ("demand", "flashanns"):
                    rows.append(self._row(dataset=dataset, phase="q4_hide",
                                          system=system))
            for dataset in ("t2i10m", "yfcc10m", "laion10m"):
                for state in ("cold", "warm"):
                    rows.append(self._row(dataset=dataset, phase="q4_cold_warm", state=state))
            for cache_gib in (1, 2, 4, 8):
                rows.append(self._row(phase="q4_cache", cache_gib=cache_gib))

            fields = sorted({key for row in rows for key in row})
            with csv_path.open("w", newline="") as dst:
                writer = csv.DictWriter(dst, fieldnames=fields)
                writer.writeheader()
                writer.writerows(rows)

            render_figures(csv_path, root)
            names = (
                "frontier-t2i.pdf", "frontier-yfcc.pdf", "frontier-laion.pdf",
                "wise-ablation.pdf", "batching-ablation.pdf", "hide-robustness.pdf",
            )
            for name in names:
                path = root / name
                self.assertTrue(path.read_bytes().startswith(b"%PDF"), name)
                info = subprocess.check_output(["pdfinfo", str(path)], text=True)
                self.assertIn("Pages:           1", info, name)

            hide_text = subprocess.check_output(
                ["pdftotext", str(root / "hide-robustness.pdf"), "-"], text=True
            )
            normalized_hide = " ".join(hide_text.split())
            self.assertIn("Host data-dependency stall", normalized_hide)
            self.assertIn("Waiting for required pages", normalized_hide)
            self.assertIn("I/O admission backpressure", normalized_hide)
            self.assertIn("Score-source audit", normalized_hide)
            self.assertIn("Flash fills: 0/query; pre-materialized: 100%", normalized_hide)

            batching_bbox = subprocess.check_output(
                ["pdftotext", "-bbox", str(root / "batching-ablation.pdf"), "-"],
                text=True,
            )
            legend = re.search(
                r'<word xMin="[^"]+" yMin="([^"]+)" xMax="[^"]+" yMax="([^"]+)">Wise</word>',
                batching_bbox,
            )
            title = re.search(
                r'<word xMin="[^"]+" yMin="([^"]+)" xMax="[^"]+" yMax="([^"]+)">YFCC-10M</word>',
                batching_bbox,
            )
            self.assertIsNotNone(legend)
            self.assertIsNotNone(title)
            self.assertGreater(float(title.group(1)), float(legend.group(2)) + 2.0)

    def test_provisional_plots_use_current_phase_dimensions(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            csv_path = root / "validated.csv"
            rows = [
                self._row(phase="q2", system="flashanns", L=400,
                          vector_slot_use_pct_median=64),
                self._row(phase="q3_t1", system="serial-t1", threads=1),
                self._row(phase="q3_t1", system="batch-t1", threads=1),
                self._row(phase="q3_t1", system="extent-t1", threads=1),
                self._row(phase="q3_t8", system="wise-only", threads=1),
                self._row(phase="q3_t8", system="wise-only", threads=8,
                          qps_median=800, qps_ci_low=790, qps_ci_high=810),
                self._row(phase="q3_t8", system="flashanns", threads=1),
                self._row(phase="q3_t8", system="flashanns", threads=8,
                          qps_median=850, qps_ci_low=840, qps_ci_high=860),
                self._row(phase="q4_cold_warm", state="cold"),
                self._row(phase="q4_cold_warm", state="warm", qps_median=110),
            ]
            fields = sorted({key for row in rows for key in row})
            with csv_path.open("w", newline="") as dst:
                writer = csv.DictWriter(dst, fieldnames=fields)
                writer.writeheader()
                writer.writerows(rows)

            render_provisional(csv_path, root)
            q2_text = subprocess.check_output(
                ["pdftotext", str(root / "q2-main.pdf"), "-"], text=True
            )
            q2_bbox = subprocess.check_output(
                ["pdftotext", "-bbox", str(root / "q2-main.pdf"), "-"], text=True
            )
            q3_text = subprocess.check_output(
                ["pdftotext", str(root / "q3-ablation.pdf"), "-"], text=True
            )
            q4_text = subprocess.check_output(
                ["pdftotext", str(root / "q4-cold-warm.pdf"), "-"], text=True
            )
            self.assertIn("Useful vector slots", q2_text)
            title = re.search(
                r'<word xMin="[^"]+" yMin="([^"]+)" xMax="[^"]+" yMax="([^"]+)">Throughput</word>',
                q2_bbox,
            )
            legend = re.search(
                r'<word xMin="[^"]+" yMin="([^"]+)" xMax="[^"]+" yMax="([^"]+)">flashanns</word>',
                q2_bbox,
            )
            self.assertIsNotNone(title)
            self.assertIsNotNone(legend)
            title_y_min = float(title.group(1))
            legend_y_max = float(legend.group(2))
            self.assertGreaterEqual(title_y_min, 0.0)
            self.assertGreater(title_y_min, legend_y_max)
            self.assertIn("Concurrent queries", q3_text)
            self.assertIn("cold", q4_text)
            self.assertIn("warm", q4_text)

    def test_wise_uses_physical_io_metrics_not_invalid_extent_counter(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "wise.pdf"
            rows = [self._row(phase="q3_t1", system=system, threads=1)
                    for system in ("serial-t1", "batch-t1", "extent-t1")]
            from experiments.eval.flashanns.plot_six_figures import _wise
            _wise(rows, path)
            text = subprocess.check_output(["pdftotext", str(path), "-"], text=True)
            self.assertIn("NAND commands", text)
            self.assertIn("NAND MiB", text)
            self.assertNotIn("Extent utilization", text)


if __name__ == "__main__":
    unittest.main()
