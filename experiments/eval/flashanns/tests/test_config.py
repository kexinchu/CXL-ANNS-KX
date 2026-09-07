import copy
import json
import unittest
from pathlib import Path

from experiments.eval.flashanns.config import ConfigError, load_configs, validate_configs


class ConfigTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo_root = Path(__file__).resolve().parents[4]

    def setUp(self):
        self.datasets, self.systems, self.matrix = load_configs(self.repo_root)

    def test_live_backings_use_reboot_stable_device_ids(self):
        contract = json.loads(
            (self.repo_root / "experiments/eval/flashanns/live-contract.json").read_text()
        )
        self.assertEqual(
            contract["nvme_dev"],
            [
                "/dev/disk/by-id/nvme-Dell_DC_NVMe_CD8P_E3.S_1.92TB_7EU0A01P0XK1",
                "/dev/disk/by-id/nvme-Dell_DC_NVMe_CD8P_E3.S_1.92TB_2F50A1360XK3",
            ],
        )

    def test_frozen_dataset_metrics_and_constants(self):
        self.assertEqual(self.datasets["t2i10m"]["metric"], "mips")
        self.assertEqual(self.datasets["laion10m"]["metric"], "mips")
        self.assertTrue(self.datasets["t2i10m"]["pipeann_index_prefix"].endswith("/idx_t2i"))
        self.assertTrue(self.datasets["yfcc10m"]["pipeann_index_prefix"].endswith("/pipeann"))
        self.assertTrue(self.datasets["laion10m"]["pipeann_index_prefix"].endswith("/pipeann"))
        self.assertEqual(self.datasets["yfcc10m"]["metric"], "l2")
        self.assertTrue(self.datasets["t2i10m"]["ready"])
        self.assertTrue(self.datasets["yfcc10m"]["ready"])
        self.assertTrue(self.datasets["laion10m"]["ready"])
        self.assertEqual(self.matrix["cache_limit"], 4 * 1024**3)
        self.assertEqual(
            self.matrix["window_miss_recovery"],
            "refill_committed_pages_when_idle",
        )
        for dataset_id, dataset in self.datasets.items():
            self.assertTrue(
                dataset["pipeann_index_prefix"],
                f"{dataset_id} must declare its native PipeANN index prefix",
            )
            self.assertEqual(dataset["staging"]["offset"], 1100 * 1024**3)
            self.assertEqual(dataset["staging"]["host_artifact"], "extent_image")
            self.assertEqual(dataset["staging"]["magic"], 0x314E415843)
        self.assertEqual(self.datasets["yfcc10m"]["staging"]["length"], 4096 + 10_000_000 * 2048)
        self.assertEqual(self.datasets["laion10m"]["staging"]["length"], 4096 + 10_000_000 * 4096)

    def test_built_dataset_pq_paths_match_builder_outputs(self):
        for dataset_id in ("yfcc10m", "laion10m"):
            artifacts = self.datasets[dataset_id]["artifacts"]
            self.assertTrue(
                artifacts["pq64_pivots"].endswith("/index_pq64_pq_pivots.bin")
            )
            self.assertTrue(
                artifacts["pq64_codes"].endswith("/index_pq64_pq_compressed.bin")
            )

    def test_frozen_flashanns_shape(self):
        flashanns = self.systems["flashanns"]
        self.assertEqual(flashanns["threads"], 8)
        self.assertEqual(flashanns["pipe_depth"], 2)
        self.assertEqual(flashanns["issue_qd"], 0)
        self.assertEqual(flashanns["per_thread_window"], 128 * 1024**2)
        self.assertEqual(self.systems["pipeann"]["kind"], "external-pipeann")
        self.assertEqual(self.systems["pipeann"]["threads"], 8)
        self.assertEqual(
            self.systems["pipeann"]["block_devices"],
            [
                "/dev/disk/by-id/nvme-Dell_Ent_NVMe_PM1733a_RI_3.84TB_S6USNE0TA08224"
            ],
        )

    def test_demand_is_the_blocking_single_depth_baseline(self):
        demand = self.systems["demand"]
        self.assertEqual(demand["pipe_depth"], 1)
        self.assertIn("--no-vmem-prefetch", demand["flags"])
        self.assertIn("--no-steal-sched", demand["flags"])

        systems = copy.deepcopy(self.systems)
        systems["demand"]["pipe_depth"] = 2
        with self.assertRaisesRegex(ConfigError, "demand pipe_depth.*1"):
            validate_configs(self.datasets, systems, self.matrix)

    def test_original_layout_demand_is_supplemental(self):
        system = self.systems["demand-orc"]
        self.assertEqual(system["kind"], "internal")
        self.assertEqual(system["layout"], "original")
        self.assertEqual(system["threads"], 8)
        self.assertEqual(system["pipe_depth"], 1)
        self.assertIn("--no-vmem-prefetch", system["flags"])
        self.assertIn("--no-extent-run", system["flags"])
        self.assertIn("--no-steal-sched", system["flags"])
        self.assertEqual(
            self.matrix["q2"]["systems"], ["demand", "pipeann", "flashanns"]
        )
        self.assertEqual(
            self.matrix["smoke_original"]["systems"], ["demand-orc"]
        )
        self.assertEqual(
            self.matrix["q2_original"]["systems"], ["demand-orc"]
        )

    def test_rejects_original_layout_on_non_orc_system(self):
        systems = copy.deepcopy(self.systems)
        systems["demand"]["layout"] = "original"
        with self.assertRaisesRegex(ConfigError, "only demand-orc"):
            validate_configs(self.datasets, systems, self.matrix)

    def test_q2_excludes_oracle(self):
        self.assertEqual(
            self.matrix["q2"]["systems"],
            ["demand", "pipeann", "flashanns"],
        )
        self.assertNotIn("oracle", self.systems)

    def test_six_figure_matrix_is_complete(self):
        self.assertTrue(self.matrix["q2"]["sweep_L"])
        self.assertEqual(
            self.matrix["q3_t8"]["systems"], ["wise-only", "flashanns"]
        )
        self.assertEqual(self.matrix["q3_t8"]["threads"], [1, 2, 4, 8, 16])
        self.assertEqual(
            self.matrix["q3_load"]["systems"],
            ["wise-only", "pipeann", "flashanns"],
        )
        self.assertEqual(self.matrix["q4_hide"]["systems"], ["demand", "flashanns"])
        self.assertEqual(
            self.matrix["q4_cold_warm"]["states"], ["cold", "warm"]
        )
        self.assertEqual(self.matrix["q4_cache"]["cache_gib"], [1, 2, 4, 8])
        self.assertEqual(self.systems["wise-only"]["threads"], "matrix")

    def test_overall_plan_excludes_oracle_as_an_executable_system(self):
        plan = (
            self.repo_root
            / "docs/superpowers/plans/2026-09-03-flashanns-evaluation.md"
        ).read_text()
        self.assertIn("Oracle is excluded from execution", plan)
        for stale_contract in (
            "Oracle, FlashANNS, and Demand",
            "Encode Oracle, FlashANNS, Demand",
            "Continuous, and Oracle",
            "Oracle-gap closure",
        ):
            with self.subTest(stale_contract=stale_contract):
                self.assertNotIn(stale_contract, plan)

    def test_rejects_oracle_system_definition(self):
        systems = copy.deepcopy(self.systems)
        systems["oracle"] = {"kind": "internal", "threads": 8, "flags": []}
        with self.assertRaisesRegex(ConfigError, "oracle system is excluded"):
            validate_configs(self.datasets, systems, self.matrix)

    def test_ready_dataset_requires_every_artifact(self):
        datasets = copy.deepcopy(self.datasets)
        del datasets["t2i10m"]["artifacts"]["ground_truth"]
        with self.assertRaisesRegex(ConfigError, "ground_truth"):
            validate_configs(datasets, self.systems, self.matrix)

    def test_rejects_unknown_metric(self):
        datasets = copy.deepcopy(self.datasets)
        datasets["yfcc10m"]["metric"] = "cosine"
        with self.assertRaisesRegex(ConfigError, "metric"):
            validate_configs(datasets, self.systems, self.matrix)

    def test_rejects_staging_length_that_differs_from_fixed_stride_image(self):
        datasets = copy.deepcopy(self.datasets)
        datasets["laion10m"]["staging"]["length"] -= 4096
        with self.assertRaisesRegex(ConfigError, "staging length"):
            validate_configs(datasets, self.systems, self.matrix)

    def test_rejects_removed_live_flags(self):
        removed = [
            "--early-cl",
            "--lookahead-k",
            "--spec-beam-nbrs",
            "--score-page",
            "--pipe-drive",
            "--admit-gap",
            "--oracle-dram",
        ]
        for flag in removed:
            with self.subTest(flag=flag):
                systems = copy.deepcopy(self.systems)
                systems["flashanns"]["flags"].append(flag)
                with self.assertRaisesRegex(ConfigError, "removed flag"):
                    validate_configs(self.datasets, systems, self.matrix)

    def test_rejects_non_eight_core_q2_system(self):
        systems = copy.deepcopy(self.systems)
        systems["demand"]["threads"] = 4
        with self.assertRaisesRegex(ConfigError, "q2.*8 threads"):
            validate_configs(self.datasets, systems, self.matrix)

    def test_rejects_mismatched_internal_q2_window_budget(self):
        systems = copy.deepcopy(self.systems)
        systems["demand"]["per_thread_window"] = 2 * 1024**3
        with self.assertRaisesRegex(ConfigError, "q2 internal.*window"):
            validate_configs(self.datasets, systems, self.matrix)

    def test_rejects_unbounded_internal_expansion_budget(self):
        matrix = copy.deepcopy(self.matrix)
        matrix["internal_iters"] = "unbounded"
        with self.assertRaisesRegex(ConfigError, "internal_iters.*L"):
            validate_configs(self.datasets, self.systems, matrix)

    def test_rejects_changed_window_miss_recovery(self):
        matrix = copy.deepcopy(self.matrix)
        matrix["window_miss_recovery"] = "spin"
        with self.assertRaisesRegex(ConfigError, "window_miss_recovery"):
            validate_configs(self.datasets, self.systems, matrix)

    def test_rejects_non_four_gib_cache(self):
        matrix = copy.deepcopy(self.matrix)
        matrix["cache_limit"] = 100 * 1024**2
        with self.assertRaisesRegex(ConfigError, "4294967296"):
            validate_configs(self.datasets, self.systems, matrix)


if __name__ == "__main__":
    unittest.main()
