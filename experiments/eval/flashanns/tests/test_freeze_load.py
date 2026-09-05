import unittest

from experiments.eval.flashanns.freeze_load import freeze_load_contract


def accepted(system, phase, repeat, qps, *, level=400, threads=None, recall=0.93):
    record = {
        "run_id": f"{system}-{phase}-{level}-{threads}-{repeat}",
        "dataset": "t2i10m",
        "phase": phase,
        "system": system,
        "L": level,
        "repeat": repeat,
        "metrics": {"throughput_QPS": qps, "recall@10": recall},
        "validation": {"status": "accepted"},
    }
    if threads is not None:
        record["threads"] = threads
    return record


class FreezeLoadTest(unittest.TestCase):
    def test_freezes_common_rates_from_slowest_measured_saturation(self):
        records = []
        for repeat in range(5):
            records += [
                accepted("wise-only", "q3_t8", repeat, 640 + repeat, threads=8),
                accepted("flashanns", "q3_t8", repeat, 690 + repeat, threads=8),
                accepted("pipeann", "q2", repeat, 900 + repeat, level=200, recall=0.91),
                accepted("pipeann", "q2", repeat, 950 + repeat, level=400, recall=0.94),
            ]
        base = {"accepted": True, "primary": {"flashanns": {"L": 400}}}
        frozen = freeze_load_contract(records, base, target_recall=0.90)
        self.assertEqual(frozen["primary"]["pipeann"]["L"], 200)
        self.assertEqual(frozen["saturation_qps"]["wise-only"], 642)
        self.assertEqual(frozen["saturation_qps"]["flashanns"], 692)
        self.assertEqual(frozen["saturation_qps"]["pipeann"], 902)
        self.assertEqual(frozen["arrival_rates"], [160.5, 321.0, 481.5, 642.0, 802.5])

    def test_rejects_incomplete_five_repeat_block(self):
        records = [
            accepted("wise-only", "q3_t8", repeat, 640, threads=8)
            for repeat in range(4)
        ]
        records += [
            accepted("flashanns", "q3_t8", repeat, 690, threads=8)
            for repeat in range(5)
        ]
        records += [
            accepted("pipeann", "q2", repeat, 900, level=200, recall=0.91)
            for repeat in range(5)
        ]
        with self.assertRaisesRegex(ValueError, "five repeats"):
            freeze_load_contract(records, {"primary": {"flashanns": {"L": 400}}}, 0.90)


if __name__ == "__main__":
    unittest.main()
