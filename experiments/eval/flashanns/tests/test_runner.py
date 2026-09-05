import tempfile
import inspect
import unittest
import uuid
from pathlib import Path

from experiments.eval.flashanns.config import REMOVED_FLAGS
from experiments.eval.flashanns import run_matrix, run_one


RunnerError = run_matrix.RunnerError
expand_runs = run_matrix.expand_runs


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

    def test_q2_consumes_primary_frozen_anchor_for_each_system(self):
        anchors = {
            "primary": {
                "demand": {"L": 100},
                "pipeann": {"L": 200},
                "flashanns": {"L": 400},
            }
        }
        try:
            runs = expand_runs(ROOT, "t2i10m", "q2", anchors=anchors)
        except RunnerError as exc:
            self.fail(f"runner rejected validator anchor output: {exc}")
        levels = {
            system: {run["L"] for run in runs if run["system"] == system}
            for system in ("demand", "pipeann", "flashanns")
        }
        self.assertEqual(
            levels,
            {"demand": {100}, "pipeann": {200}, "flashanns": {400}},
        )

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

    def test_calibration_can_select_one_system_and_one_declared_level(self):
        self.assertIn(
            "level",
            inspect.signature(expand_runs).parameters,
            "runner must expose a single-level selector",
        )
        runs = expand_runs(
            ROOT,
            "t2i10m",
            "calibration",
            system_id="demand",
            level=400,
        )
        self.assertEqual(len(runs), 1)
        self.assertEqual(runs[0]["run_id"], "t2i10m-calibration-L400-r0-cold-demand")
        self.assertEqual(runs[0]["nq"], 500)
        self.assertEqual(runs[0]["state"], "cold")

        with self.assertRaisesRegex(RunnerError, "declared.*L"):
            expand_runs(
                ROOT,
                "t2i10m",
                "calibration",
                system_id="demand",
                level=401,
            )

    def test_live_internal_invocation_requires_exactly_one_run_and_evidence(self):
        self.assertTrue(
            hasattr(run_matrix, "validate_live_invocation"),
            "runner must expose fail-closed live invocation validation",
        )
        validate_live_invocation = run_matrix.validate_live_invocation
        one = expand_runs(
            ROOT,
            "t2i10m",
            "calibration",
            system_id="demand",
            level=400,
        )
        identity = {"accepted": True}
        volatile = {"accepted": True, "capture_id": str(uuid.uuid4())}
        validate_live_invocation(one, identity, volatile)

        many = expand_runs(ROOT, "t2i10m", "calibration", level=400)
        with self.assertRaisesRegex(RunnerError, "exactly one internal"):
            validate_live_invocation(many, identity, volatile)
        with self.assertRaisesRegex(RunnerError, "identity.*volatile"):
            validate_live_invocation(one, None, None)
        with self.assertRaisesRegex(RunnerError, "capture_id"):
            validate_live_invocation(one, identity, {"accepted": True})

    def test_cold_evidence_claim_is_one_time(self):
        self.assertTrue(
            hasattr(run_one, "claim_volatile_evidence"),
            "runner must claim each volatile capture exactly once",
        )
        evidence = {"accepted": True, "capture_id": str(uuid.uuid4())}
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            claim = run_one.claim_volatile_evidence(root, evidence, "run-a")
            self.assertEqual(claim["run_id"], "run-a")
            with self.assertRaisesRegex(ValueError, "already consumed"):
                run_one.claim_volatile_evidence(root, evidence, "run-b")


if __name__ == "__main__":
    unittest.main()
