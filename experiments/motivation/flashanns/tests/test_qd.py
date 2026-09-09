import inspect
from pathlib import Path
import time
import unittest
from unittest.mock import patch

from experiments.motivation.flashanns.run_qd import (
    BlockQdSampler,
    capability_probe_command,
    capability_schedule,
    capability_knee,
    mean_entire_interval_qd,
    run_capability,
    select_ssd_pages,
    validate_capability_block,
)


class QdTest(unittest.TestCase):
    def test_sampler_propagates_thread_failure(self):
        before = {"nvme": {"read_ios": 10, "in_flight": 0}}
        after = {"nvme": {"read_ios": 9, "in_flight": 0}}
        with patch(
            "experiments.motivation.flashanns.run_qd.read_block_snapshot",
            side_effect=[before, after],
        ):
            sampler = BlockQdSampler(("nvme",), interval_s=0.001)
            sampler.start()
            time.sleep(0.01)
            with self.assertRaisesRegex(ValueError, "negative"):
                sampler.stop()

    def test_capability_schedule_rotates_all_five_repeats(self):
        schedule = capability_schedule((1, 2, 4), repeats=5)
        self.assertEqual(len(schedule), 15)
        self.assertEqual(schedule[:3], [(0, 1), (0, 2), (0, 4)])
        self.assertEqual(schedule[3:6], [(1, 2), (1, 4), (1, 1)])

    def test_ssd_page_selection_is_distinct_and_excludes_ram_ranges(self):
        pages = select_ssd_pages(
            image_bytes=16 * 4096,
            ram_ranges=[(2 * 4096, 4 * 4096), (9 * 4096, 10 * 4096)],
            page_count=10,
            seed=7,
        )
        self.assertEqual(len(pages), len(set(pages)))
        self.assertTrue(all(page % 4096 == 0 for page in pages))
        self.assertTrue(all(not (2 * 4096 <= page < 4 * 4096) for page in pages))
        self.assertTrue(all(not (9 * 4096 <= page < 10 * 4096) for page in pages))

    def test_ssd_page_selection_rejects_cache_sized_working_set(self):
        with self.assertRaisesRegex(ValueError, "larger than cache"):
            select_ssd_pages(
                image_bytes=8 * 4096, ram_ranges=[], page_count=4,
                seed=0, cache_bytes=4 * 4096,
            )

    def test_capability_probe_command_bounds_the_mapped_image(self):
        command = capability_probe_command(
            binary=Path("probe"), device=Path("/dev/vmem0"),
            image_offset=4096, image_bytes=8192,
            query_offsets=Path("query_offsets.u64"), pages=Path("pages.u64"),
            output=Path("probe.json"),
            wave_pages=128,
        )
        self.assertEqual(command[command.index("--image-bytes") + 1], "8192")
        self.assertEqual(command[command.index("--wave-pages") + 1], "128")

    def test_capability_requires_explicit_backing_devices(self):
        parameter = inspect.signature(run_capability).parameters["devices"]
        self.assertIs(parameter.default, inspect.Parameter.empty)

    def test_knee_is_smallest_level_reaching_90_percent_of_peak(self):
        points = [
            {"concurrency": 1, "bandwidth_gib_s": 0.2},
            {"concurrency": 2, "bandwidth_gib_s": 0.7},
            {"concurrency": 4, "bandwidth_gib_s": 0.91},
            {"concurrency": 8, "bandwidth_gib_s": 1.0},
            {"concurrency": 16, "bandwidth_gib_s": 0.98},
        ]
        self.assertEqual(capability_knee(points), 4)

    def test_capability_block_requires_all_levels_and_five_repeats(self):
        levels = (1, 2, 4, 8, 16, 32, 64, 128)
        records = [
            {"concurrency": level, "repeat": repeat, "bandwidth_gib_s": float(level)}
            for level in levels for repeat in range(5)
        ]
        validate_capability_block(records, levels)
        with self.assertRaisesRegex(ValueError, "complete"):
            validate_capability_block(records[:-1], levels)

    def test_entire_interval_mean_keeps_idle_samples(self):
        parsed = {"intervals": [{"aqu_sz": 0.0}, {"aqu_sz": 0.0}, {"aqu_sz": 6.0}],
                  "active_only": False}
        self.assertEqual(mean_entire_interval_qd(parsed), 2.0)

    def test_rejects_active_only_average(self):
        with self.assertRaisesRegex(ValueError, "active-only"):
            mean_entire_interval_qd({"intervals": [{"aqu_sz": 6.0}], "active_only": True})


if __name__ == "__main__":
    unittest.main()
