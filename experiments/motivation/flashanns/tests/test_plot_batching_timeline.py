from pathlib import Path
import tempfile
import unittest

import matplotlib.image as mpimg

from experiments.motivation.flashanns.plot_batching_timeline import (
    plot,
    select_campaign,
    select_representative,
    select_stable_window,
    utilization_series,
)


def sample_record(repeat, aggregate_utilization, rates=(262_144.0, 524_288.0)):
    return {
        "repeat": repeat,
        "metrics": {
            "bandwidth_utilization_pct": aggregate_utilization,
            "empirical_peak_gib_s": 0.5,
        },
        "qd_samples": {
            "intervals": [
                {"seconds": 0.1, "aqu_sz": 0.0, "read_kib_s": rate}
                for rate in rates
            ],
            "active_only": False,
            "source": "linux_block_weighted_io_ticks_100ms",
        },
    }


class BatchingTimelinePlotTest(unittest.TestCase):
    def test_campaign_selection_does_not_mix_preserved_runs(self):
        records = [
            {"run_id": "old-single-r0"},
            {"run_id": "mot-time-single-r0"},
            {"run_id": "mot-time-static-r0"},
        ]
        self.assertEqual(
            [row["run_id"] for row in select_campaign(records, "mot-time")],
            ["mot-time-single-r0", "mot-time-static-r0"],
        )

    def test_representative_is_repeat_closest_to_five_run_median(self):
        records = [
            sample_record(0, 10.0), sample_record(1, 50.0),
            sample_record(2, 30.0), sample_record(3, 20.0),
            sample_record(4, 40.0),
        ]
        self.assertEqual(select_representative(records)["repeat"], 2)

    def test_timeline_converts_kib_per_second_to_percent_of_peak(self):
        elapsed, utilization = utilization_series(sample_record(0, 25.0))
        self.assertEqual(elapsed, [0.0, 0.1, 0.2])
        self.assertEqual(utilization, [50.0, 100.0, 100.0])

    def test_stable_window_is_ten_seconds_inside_middle_sixty_percent(self):
        elapsed = [float(value) for value in range(31)]
        utilization = [
            50.0 if 12 <= value < 22 else (0.0 if value % 2 == 0 else 100.0)
            for value in range(30)
        ]
        utilization.append(utilization[-1])
        start, window_time, window_utilization = select_stable_window(
            elapsed, utilization
        )
        self.assertEqual(start, 12.0)
        self.assertEqual(window_time[0], 0.0)
        self.assertEqual(window_time[-1], 10.0)
        self.assertEqual(set(window_utilization), {50.0})

    def test_plot_writes_three_640_by_288_pngs(self):
        rates = tuple(262_144.0 if index % 2 else 524_288.0 for index in range(300))
        records = []
        for condition in ("single", "static", "dynamic"):
            for repeat in range(5):
                item = sample_record(repeat, 20.0 + repeat, rates=rates)
                item["condition"] = condition
                records.append(item)
        with tempfile.TemporaryDirectory() as directory:
            outputs = plot(records, Path(directory) / "timeline.png")
            self.assertEqual(
                [path.name for path in outputs],
                ["timeline-single.png", "timeline-static.png", "timeline-dynamic.png"],
            )
            for path in outputs:
                self.assertEqual(mpimg.imread(path).shape[:2], (288, 640))

    def test_timeline_rejects_non_frozen_or_invalid_samples(self):
        record = sample_record(0, 25.0)
        record["qd_samples"]["source"] = "unknown"
        with self.assertRaisesRegex(ValueError, "100-ms sampler"):
            utilization_series(record)

        record = sample_record(0, 25.0, rates=(-1.0,))
        with self.assertRaisesRegex(ValueError, "read rate"):
            utilization_series(record)


if __name__ == "__main__":
    unittest.main()
