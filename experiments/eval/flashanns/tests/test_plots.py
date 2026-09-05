import csv
import subprocess
import tempfile
import unittest
from pathlib import Path

from experiments.eval.flashanns.plot_q2_q4 import COLORS, render_figures


class PlotTest(unittest.TestCase):
    def test_oracle_has_no_plot_style(self):
        self.assertNotIn("oracle", COLORS)

    def test_writes_three_one_page_vector_pdfs(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            csv_path = root / "validated.csv"
            fields = ["dataset", "phase", "system", "state", "L", "qps_median", "qps_ci_low", "qps_ci_high", "latency_p99_ms_median"]
            with csv_path.open("w", newline="") as dst:
                writer = csv.DictWriter(dst, fieldnames=fields)
                writer.writeheader()
                for phase in ("q2", "q3_t1", "q3_t8", "q4"):
                    for state in (("cold", "warm") if phase == "q4" else ("cold",)):
                        writer.writerow(dict(dataset="t2i10m", phase=phase, system="flashanns", state=state, L=400, qps_median=100, qps_ci_low=90, qps_ci_high=110, latency_p99_ms_median=12))
            render_figures(csv_path, root)
            for name in ("q2-main.pdf", "q3-ablation.pdf", "q4-cold-warm.pdf"):
                path = root / name
                self.assertTrue(path.read_bytes().startswith(b"%PDF"))
                info = subprocess.check_output(["pdfinfo", str(path)], text=True)
                self.assertIn("Pages:           1", info)


if __name__ == "__main__":
    unittest.main()
