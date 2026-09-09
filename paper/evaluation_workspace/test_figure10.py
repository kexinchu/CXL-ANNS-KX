import csv
import subprocess
import tempfile
import unittest
from pathlib import Path

import figure10


class Figure10Test(unittest.TestCase):
    def _write_rows(self, path: Path) -> None:
        rows = [
            {
                "kind": "io", "stage_name": "original-layout",
                "requested_pages_per_query_median": "375.3843",
                "requested_pages_per_query_ci_low": "375.2393",
                "requested_pages_per_query_ci_high": "375.4341",
                "nand_mib_per_query_median": "1.02243828125",
                "nand_mib_per_query_ci_low": "1.022391796875",
                "nand_mib_per_query_ci_high": "1.02254375",
                "vector_slot_use_pct_median": "61.46",
            },
            {
                "kind": "io", "stage_name": "co-use-layout",
                "requested_pages_per_query_median": "356.0319",
                "requested_pages_per_query_ci_low": "355.7982",
                "requested_pages_per_query_ci_high": "356.131",
                "nand_mib_per_query_median": "0.963377734375",
                "nand_mib_per_query_ci_low": "0.963317578125",
                "nand_mib_per_query_ci_high": "0.96346796875",
                "vector_slot_use_pct_median": "64.16",
            },
            {
                "kind": "io", "stage_name": "post-commit",
                "nand_commands_per_query_median": "237.321",
                "nand_commands_per_query_ci_low": "237.3201",
                "nand_commands_per_query_ci_high": "237.3213",
                "physical_kib_per_read_median": "4.045020973746474",
            },
            {
                "kind": "io", "stage_name": "extent-reads",
                "nand_commands_per_query_median": "220.6605",
                "nand_commands_per_query_ci_low": "220.6594",
                "nand_commands_per_query_ci_high": "220.6613",
                "physical_kib_per_read_median": "4.350444234468788",
            },
        ]
        fields = sorted({key for row in rows for key in row})
        with path.open("w", newline="") as dst:
            writer = csv.DictWriter(dst, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)

    def test_selects_only_the_three_independent_controls(self):
        with tempfile.TemporaryDirectory() as td:
            source = Path(td) / "validated.csv"
            self._write_rows(source)
            controls = figure10.load_controls(source)
        self.assertEqual(set(controls), {"placement", "traffic", "extent"})
        self.assertEqual(controls["placement"].values, (375.3843, 356.0319))
        self.assertEqual(controls["traffic"].values,
                         (1.02243828125, 0.963377734375))
        self.assertEqual(controls["extent"].values, (237.321, 220.6605))

    def test_renders_three_one_page_vector_pdfs(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "validated.csv"
            output = root / "out"
            self._write_rows(source)
            figure10.render(source, output)
            expected = {
                "figure10a-placement.pdf": ("Placement", "Host-window requests"),
                "figure10b-traffic.pdf": ("Physical NAND", "NAND traffic"),
                "figure10c-extents.pdf": ("Extent", "NAND read commands"),
            }
            for name, phrases in expected.items():
                path = output / name
                self.assertTrue(path.read_bytes().startswith(b"%PDF"))
                info = subprocess.check_output(["pdfinfo", str(path)], text=True)
                self.assertIn("Pages:           1", info)
                text = subprocess.check_output(
                    ["pdftotext", str(path), "-"], text=True
                )
                for phrase in phrases:
                    self.assertIn(phrase, text)
                self.assertNotIn("Serialized", text)
                images = subprocess.check_output(
                    ["pdfimages", "-list", str(path)], text=True
                ).splitlines()
                self.assertEqual(len(images), 2, "PDF must contain no raster images")


if __name__ == "__main__":
    unittest.main()
