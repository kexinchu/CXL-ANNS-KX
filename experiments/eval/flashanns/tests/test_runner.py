import tempfile
import unittest
from pathlib import Path

from experiments.eval.flashanns.config import REMOVED_FLAGS
from experiments.eval.flashanns.run_matrix import expand_runs


ROOT = Path(__file__).resolve().parents[4]


class RunnerTest(unittest.TestCase):
    def test_smoke_expands_all_internal_proof_systems(self):
        runs = expand_runs(ROOT, "t2i10m", "smoke")
        self.assertEqual({run["system"] for run in runs}, {"demand", "oracle", "flashanns"})
        self.assertEqual(len(runs), 3)
        for run in runs:
            command = run["command"]
            self.assertIn("--metric", command)
            self.assertEqual(command[command.index("--metric") + 1], "mips")
            self.assertIn("--threads", command)
            self.assertIn("--eval-trace-dir", command)
            self.assertEqual(run["cache_limit"], 4294967296)
            self.assertTrue(set(command).isdisjoint(REMOVED_FLAGS))

    def test_order_and_ids_are_deterministic(self):
        first = expand_runs(ROOT, "t2i10m", "q2", anchors={"L": 400})
        second = expand_runs(ROOT, "t2i10m", "q2", anchors={"L": 400})
        self.assertEqual(first, second)
        self.assertEqual(len(first), 20)
        self.assertEqual(len({run["run_id"] for run in first}), 20)

    def test_external_pipeann_uses_native_binary(self):
        runs = expand_runs(ROOT, "t2i10m", "q2", anchors={"L": 400})
        pipeann = next(run for run in runs if run["system"] == "pipeann")
        self.assertTrue(pipeann["external"])
        self.assertTrue(pipeann["command"][0].endswith("search_disk_index"))
        self.assertNotIn("search_beam", " ".join(pipeann["command"]))


if __name__ == "__main__":
    unittest.main()
