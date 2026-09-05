import csv
import subprocess
import tempfile
import unittest
from pathlib import Path

from experiments.eval.flashanns.plot_six_figures import COLORS, render_figures


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
            "useful_bandwidth_mib_s_median": 500, "mean_inflight_median": 4,
            "committed_candidates_median": 400, "unique_pages_median": 300,
            "missing_pages_median": 200, "nand_mib_per_query_median": 1.5,
            "nand_commands_per_query_median": 7, "vector_slot_use_pct_median": 55,
            "extent_use_pct_median": 70, "pq_nav_ms_median": 3,
            "critical_wait_ms_median": 4, "exact_rerank_ms_median": 2,
            "queue_wait_ms_median": 1, "score_host_pct_median": 75,
            "score_cxl_pct_median": 25, "score_bounce_pct_median": 0,
            "score_flash_pct_median": 0, "flash_fills_per_query_median": 2,
        }
        row.update(updates)
        return row

    def test_oracle_has_no_plot_style(self):
        self.assertNotIn("oracle", COLORS)

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
            for system in ("serial-t1", "batch-t1", "extent-t1"):
                rows.append(self._row(phase="q3_t1", system=system, threads=1))
            for threads in (1, 2, 4, 8, 16):
                rows.append(self._row(phase="q3_t8", system="wise-only", threads=threads))
                rows.append(self._row(phase="q3_t8", system="flashanns", threads=threads,
                                      qps_median=100 * threads))
            for system in ("wise-only", "pipeann", "flashanns"):
                for offered in (100, 500):
                    rows.append(self._row(phase="q3_load", system=system,
                                          arrival_rate=offered, qps_median=offered * 0.9))
            for system in ("demand", "flashanns"):
                rows.append(self._row(phase="q4_hide", system=system))
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


if __name__ == "__main__":
    unittest.main()
