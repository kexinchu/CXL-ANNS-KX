import copy
import unittest
from pathlib import Path

from experiments.eval.flashanns.config import ConfigError, load_configs, validate_configs


class ConfigTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo_root = Path(__file__).resolve().parents[4]

    def setUp(self):
        self.datasets, self.systems, self.matrix = load_configs(self.repo_root)

    def test_frozen_dataset_metrics_and_constants(self):
        self.assertEqual(self.datasets["t2i10m"]["metric"], "mips")
        self.assertEqual(self.datasets["laion10m"]["metric"], "mips")
        self.assertEqual(self.datasets["yfcc10m"]["metric"], "l2")
        self.assertTrue(self.datasets["t2i10m"]["ready"])
        self.assertFalse(self.datasets["yfcc10m"]["ready"])
        self.assertFalse(self.datasets["laion10m"]["ready"])
        self.assertEqual(self.matrix["cache_limit"], 4 * 1024**3)

    def test_frozen_flashanns_shape(self):
        flashanns = self.systems["flashanns"]
        self.assertEqual(flashanns["threads"], 8)
        self.assertEqual(flashanns["pipe_depth"], 2)
        self.assertEqual(flashanns["issue_qd"], 0)
        self.assertEqual(flashanns["per_thread_window"], 128 * 1024**2)
        self.assertEqual(self.systems["pipeann"]["kind"], "external-pipeann")
        self.assertEqual(self.systems["pipeann"]["threads"], 8)

    def test_q2_excludes_oracle(self):
        self.assertEqual(
            self.matrix["q2"]["systems"],
            ["demand", "pipeann", "flashanns"],
        )
        self.assertNotIn("oracle", self.systems)

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

    def test_rejects_non_four_gib_cache(self):
        matrix = copy.deepcopy(self.matrix)
        matrix["cache_limit"] = 100 * 1024**2
        with self.assertRaisesRegex(ConfigError, "4294967296"):
            validate_configs(self.datasets, self.systems, matrix)


if __name__ == "__main__":
    unittest.main()
