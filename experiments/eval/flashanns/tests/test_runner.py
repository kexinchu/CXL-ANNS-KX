import tempfile
import unittest
from pathlib import Path

from experiments.eval.flashanns.config import REMOVED_FLAGS
from experiments.eval.flashanns.run_matrix import expand_runs


ROOT = Path(__file__).resolve().parents[4]


class RunnerTest(unittest.TestCase):
    def test_smoke_expands_all_internal_proof_systems(self):
        runs = expand_runs(ROOT, "t2i10m", "smoke")
        self.assertEqual({run["system"] for run in runs}, {"demand", "flashanns"})
        self.assertEqual(len(runs), 2)
        for run in runs:
            command = run["command"]
            self.assertIn("--metric", command)
            self.assertEqual(command[command.index("--metric") + 1], "mips")
            self.assertIn("--threads", command)
            self.assertIn("--eval-trace-dir", command)
            self.assertEqual(run["cache_limit"], 4294967296)
            self.assertTrue(set(command).isdisjoint(REMOVED_FLAGS))
            self.assertIn("--vmem-dev", command)
            self.assertNotIn("--oracle-dram", command)
            self.assertNotIn("--dram-backend", command)
            self.assertNotIn("--image", command)

        demand_only = expand_runs(ROOT, "t2i10m", "smoke", system_id="demand")
        self.assertEqual([run["system"] for run in demand_only], ["demand"])

    def test_order_and_ids_are_deterministic(self):
        first = expand_runs(ROOT, "t2i10m", "q2", anchors={"L": 400})
        second = expand_runs(ROOT, "t2i10m", "q2", anchors={"L": 400})
        self.assertEqual(first, second)
        self.assertEqual(len(first), 15)
        self.assertEqual(len({run["run_id"] for run in first}), 15)

    def test_external_pipeann_uses_native_binary(self):
        runs = expand_runs(ROOT, "t2i10m", "q2", anchors={"L": 400})
        pipeann = next(run for run in runs if run["system"] == "pipeann")
        self.assertTrue(pipeann["external"])
        self.assertTrue(pipeann["command"][0].endswith("search_disk_index"))
        self.assertNotIn("search_beam", " ".join(pipeann["command"]))

    def test_q2_internal_commands_use_the_same_128_mib_window(self):
        runs = expand_runs(ROOT, "t2i10m", "q2", anchors={"L": 400})
        internal = {
            run["system"]: run["command"]
            for run in runs
            if run["system"] in {"demand", "flashanns"}
        }
        self.assertEqual(set(internal), {"demand", "flashanns"})
        for system, command in internal.items():
            with self.subTest(system=system):
                self.assertIn("--dram-bytes", command)
                self.assertIn("--host-bytes", command)
                self.assertEqual(
                    command[command.index("--dram-bytes") + 1], "134217728"
                )
                self.assertEqual(
                    command[command.index("--host-bytes") + 1], "134217728"
                )


if __name__ == "__main__":
    unittest.main()
