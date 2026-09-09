import unittest

from experiments.motivation.flashanns.run_c2_campaign import (
    build_schedule,
    latency_metrics,
    phase_for_policy,
)


class C2CampaignTest(unittest.TestCase):
    def test_policy_phase_mapping(self):
        self.assertEqual(phase_for_policy("selective_4k"), "c2_admission")
        self.assertEqual(phase_for_policy("blind_16k"), "c2_admission")
        self.assertEqual(phase_for_policy("demand"), "c2_coverage")
        self.assertEqual(phase_for_policy("top8"), "c2_coverage")
        with self.assertRaisesRegex(ValueError, "policy"):
            phase_for_policy("unknown")

    def test_schedule_has_five_rotated_repeats_per_policy(self):
        policies = ("demand", "top1", "top2", "top8")
        schedule = build_schedule(policies, repeats=5)
        self.assertEqual(len(schedule), 20)
        for policy in policies:
            repeats = sorted(repeat for repeat, item in schedule if item == policy)
            self.assertEqual(repeats, list(range(5)))
        repeat_one = [policy for repeat, policy in schedule if repeat == 1]
        self.assertEqual(repeat_one, ["top1", "top2", "top8", "demand"])

    def test_latency_metrics_uses_microseconds(self):
        metrics = latency_metrics([1000, 2000, 3000, 4000])
        self.assertEqual(metrics["latency_us_mean"], 2.5)
        self.assertEqual(metrics["latency_us_p50"], 2.5)


if __name__ == "__main__":
    unittest.main()
