#!/usr/bin/env python3
"""Output-contract tests for the Motivation figures."""

from __future__ import annotations

import tempfile
import unittest
import importlib.util
from pathlib import Path

import fitz

MODULE_PATH = Path(__file__).with_name("plot_motivation.py")
SPEC = importlib.util.spec_from_file_location("plot_motivation", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
plot_motivation = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(plot_motivation)


class Fig4OutputContractTest(unittest.TestCase):
    def test_fig4_writes_two_separate_full_size_vector_pdfs(self) -> None:
        """Catch regressions back to one compressed, three-panel Fig. 4."""
        with tempfile.TemporaryDirectory() as tmpdir:
            old_out = plot_motivation.OUT
            plot_motivation.OUT = Path(tmpdir)
            try:
                plot_motivation.fig4_pathology()
            finally:
                plot_motivation.OUT = old_out

            outputs = [
                Path(tmpdir) / "mot-pathology-admission.pdf",
                Path(tmpdir) / "mot-pathology-prefetch.pdf",
            ]
            for output in outputs:
                self.assertTrue(output.exists(), output)
                with fitz.open(output) as document:
                    self.assertEqual(document.page_count, 1)
                    page = document[0]
                    self.assertAlmostEqual(page.rect.width, 6.4 * 0.8 * 72, places=1)
                    self.assertAlmostEqual(page.rect.height, 4.8 * 0.6 * 72, places=1)

                    spans = [
                        span
                        for block in page.get_text("dict")["blocks"]
                        if "lines" in block
                        for line in block["lines"]
                        for span in line["spans"]
                        if span["text"].strip()
                    ]
                    self.assertTrue(spans)
                    self.assertGreaterEqual(min(span["size"] for span in spans), 11.5)


if __name__ == "__main__":
    unittest.main()
