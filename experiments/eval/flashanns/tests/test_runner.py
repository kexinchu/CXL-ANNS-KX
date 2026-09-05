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
    def test_warm_evidence_marks_the_exact_cold_parent(self):
        evidence = run_one.make_warm_evidence({"cache_used": 7}, "cold-run")
        self.assertTrue(evidence["accepted"])
        self.assertTrue(evidence["cold_parent_accepted"])
        self.assertEqual(evidence["cold_parent_run_id"], "cold-run")

    def test_runtime_emits_required_latency_percentiles(self):
        source = (ROOT / "serving" / "search_beam.cpp").read_text()
        for token in ("p50=", "p95=", "p99="):
            self.assertIn(token, source)

    def test_log_parser_does_not_confuse_latency_and_page_occupancy_mean(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "stdout.log"
            log.write_text(
                "latency_ms mean=33.700 p50=30.000 p90=40.000 p95=45.000 p99=50.000\n"
                "page_occ mean=64.160% pages=10\n"
            )
            metrics = run_one._parse_metrics(log, 10, 0)
        self.assertEqual(metrics["mean_latency_ms"], 33.7)
        self.assertEqual(metrics["latency_p95_ms"], 45.0)
        self.assertEqual(metrics["mean"], 64.16)

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
        self.assertEqual(len(first), 90)
        self.assertEqual(len({run["run_id"] for run in first}), 90)

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
        self.assertEqual(levels, {
            "demand": {50, 100, 200, 400, 800, 1600},
            "pipeann": {50, 100, 200, 400, 800, 1600},
            "flashanns": {50, 100, 200, 400, 800, 1600},
        })

    def test_q3_t8_expands_concurrency_dimension(self):
        runs = expand_runs(ROOT, "t2i10m", "q3_t8", anchors={"L": 400})
        self.assertEqual({run["threads"] for run in runs}, {1, 2, 4, 8, 16})
        self.assertEqual({run["system"] for run in runs}, {"wise-only", "flashanns"})
        self.assertEqual(len(runs), 2 * 5 * 5)
        for run in runs:
            command = run["command"]
            self.assertEqual(
                int(command[command.index("--threads") + 1]), run["threads"]
            )

    def test_ablation_systems_share_flashanns_recall_anchor(self):
        anchors = {"primary": {"flashanns": {"L": 400}}}
        t1 = expand_runs(ROOT, "t2i10m", "q3_t1", anchors=anchors)
        t8 = expand_runs(ROOT, "t2i10m", "q3_t8", anchors=anchors)
        self.assertEqual({run["L"] for run in t1 + t8}, {400})

    def test_q4_cache_expands_cache_dimension_without_changing_it(self):
        runs = expand_runs(ROOT, "t2i10m", "q4_cache", anchors={"L": 400})
        self.assertEqual({run["cache_gib"] for run in runs}, {1, 2, 4, 8})
        self.assertEqual(len(runs), 4 * 5)
        for run in runs:
            self.assertEqual(run["required_cache_limit"], run["cache_gib"] * 1024**3)

    def test_q4_cold_warm_has_stable_pair_links(self):
        runs = expand_runs(ROOT, "t2i10m", "q4_cold_warm", anchors={"L": 400})
        self.assertEqual(len(runs), 10)
        cold = {run["repeat"]: run for run in runs if run["state"] == "cold"}
        warm = {run["repeat"]: run for run in runs if run["state"] == "warm"}
        for repeat in range(5):
            self.assertEqual(warm[repeat]["cold_parent_run_id"], cold[repeat]["run_id"])

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

    def test_live_selectors_reduce_matrix_to_one_run(self):
        run = expand_runs(
            ROOT, "t2i10m", "q3_t8", anchors={"L": 400},
            system_id="wise-only", level=400, repeat_id=2, threads_value=8,
        )
        self.assertEqual(len(run), 1)
        self.assertEqual((run[0]["repeat"], run[0]["threads"]), (2, 8))
        cached = expand_runs(
            ROOT, "t2i10m", "q4_cache", anchors={"L": 400},
            system_id="flashanns", level=400, repeat_id=3, cache_gib_value=2,
        )
        self.assertEqual(len(cached), 1)
        self.assertEqual((cached[0]["repeat"], cached[0]["cache_gib"]), (3, 2))

    def test_calibration_defaults_to_base_levels_and_allows_conditional_extension(self):
        base = expand_runs(ROOT, "t2i10m", "calibration")
        self.assertEqual({run["L"] for run in base}, {50, 100, 200, 400, 800, 1600})

        extended = expand_runs(
            ROOT,
            "t2i10m",
            "calibration",
            system_id="demand",
            level=2400,
        )
        self.assertEqual([run["L"] for run in extended], [2400])

    def test_internal_commands_bound_expansions_to_l(self):
        for system in ("demand", "flashanns"):
            with self.subTest(system=system):
                run = expand_runs(
                    ROOT,
                    "t2i10m",
                    "calibration",
                    system_id=system,
                    level=800,
                )[0]
                command = run["command"]
                self.assertEqual(command[command.index("--iters") + 1], "800")

    def test_demand_uses_blocking_single_depth_materialization(self):
        run = expand_runs(
            ROOT,
            "t2i10m",
            "calibration",
            system_id="demand",
            level=800,
        )[0]
        command = run["command"]
        self.assertIn("--no-vmem-prefetch", command)
        self.assertIn("--no-steal-sched", command)
        self.assertIn("--pipe-depth", command)
        self.assertEqual(command[command.index("--pipe-depth") + 1], "1")

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
        with self.assertRaisesRegex(RunnerError, "identity"):
            validate_live_invocation(one, None, None)
        with self.assertRaisesRegex(RunnerError, "volatile"):
            validate_live_invocation(one, identity, None)
        with self.assertRaisesRegex(RunnerError, "capture_id"):
            validate_live_invocation(one, identity, {"accepted": True})

        warm = expand_runs(
            ROOT, "t2i10m", "q4_cold_warm", anchors={"L": 400},
            system_id="flashanns", level=400, repeat_id=0, state_value="warm",
        )
        validate_live_invocation(
            warm, {
                "accepted": True,
                "cold_parent_accepted": True,
                "cold_parent_run_id": warm[0]["cold_parent_run_id"],
            }, None
        )

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
