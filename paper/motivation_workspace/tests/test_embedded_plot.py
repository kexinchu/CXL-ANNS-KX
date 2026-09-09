"""Tests for the device-free Motivation plotting entry point."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).parents[1] / "motivation_figures_embedded.py"
REPO = Path(__file__).resolve().parents[3]
CANONICAL_CSV = REPO / "results/motivation/flashanns-10m/aggregate/motivation.csv"


def _load_script():
    spec = importlib.util.spec_from_file_location("motivation_figures_embedded", SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class EmbeddedPlotTest(unittest.TestCase):
    def test_wrapper_has_no_embedded_measurement_copy(self) -> None:
        module = _load_script()
        self.assertFalse(hasattr(module, "DATA"))
        self.assertTrue(CANONICAL_CSV.is_file())

    def test_default_renderer_can_emit_only_figure5(self) -> None:
        module = _load_script()
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            module.plot_figure5(CANONICAL_CSV, output)
            pdf = output / "mot-c3-io-concurrency.pdf"
            self.assertTrue(pdf.read_bytes().startswith(b"%PDF"))
            self.assertEqual([path.name for path in output.glob("*.pdf")], [pdf.name])


if __name__ == "__main__":
    unittest.main()
