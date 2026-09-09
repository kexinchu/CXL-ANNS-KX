import unittest

from experiments.motivation.flashanns.contract import load_contract


class ContractTest(unittest.TestCase):
    def setUp(self):
        self.contract = load_contract()

    def test_global_invariants(self):
        self.assertEqual(self.contract.repeats, (0, 1, 2, 3, 4))
        self.assertEqual(self.contract.cache_bytes, 4 * 1024**3)
        self.assertEqual(self.contract.page_bytes, 4096)
        self.assertEqual(self.contract.stripe_bytes, 2 * 1024**2)

    def test_required_phases_and_dimensions(self):
        self.assertEqual(
            set(self.contract.phases),
            {"c1_tier", "c2_admission", "c2_coverage", "c3_capability", "c3_qd",
             "c3_batching"},
        )
        datasets = ("t2i10m", "yfcc10m", "laion10m")
        self.assertEqual(self.contract.phases["c1_tier"]["datasets"], datasets)
        self.assertEqual(self.contract.phases["c1_tier"]["tiers"], ("host", "cxl_cache", "flash"))
        self.assertEqual(self.contract.phases["c1_tier"]["distinct_pages"], 2000)
        self.assertEqual(self.contract.phases["c2_admission"]["datasets"], ("laion10m",))
        self.assertEqual(
            self.contract.phases["c2_admission"]["policies"],
            ("selective_4k", "blind_16k"),
        )
        self.assertEqual(self.contract.phases["c2_coverage"]["datasets"], ("laion10m",))
        self.assertEqual(
            self.contract.phases["c2_coverage"]["policies"],
            ("demand", "top1", "top2", "top8"),
        )
        self.assertEqual(
            self.contract.phases["c3_capability"]["concurrency"],
            (1, 2, 4, 8, 16, 32, 64, 128),
        )
        self.assertEqual(self.contract.phases["c3_qd"]["datasets"], datasets)
        self.assertEqual(self.contract.phases["c3_qd"]["threads"], (1, 2, 4, 8, 16))
        self.assertEqual(
            self.contract.phases["c3_qd"]["representative_L"],
            {"t2i10m": 400, "yfcc10m": 50, "laion10m": 400},
        )
        self.assertEqual(
            self.contract.phases["c3_batching"]["conditions"],
            ("single", "static", "dynamic"),
        )
        self.assertEqual(
            self.contract.phases["c3_batching"]["threads"],
            {"single": 1, "static": 8, "dynamic": 8},
        )


if __name__ == "__main__":
    unittest.main()
