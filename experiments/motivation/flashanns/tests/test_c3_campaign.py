import unittest

from experiments.motivation.flashanns.records import validate_record
from experiments.motivation.flashanns.run_c3_capability_campaign import (
    build_capability_record,
    relative_ram_ranges,
)


class C3CapabilityCampaignTest(unittest.TestCase):
    def test_ram_segments_are_converted_to_image_relative_ranges(self):
        segments = [
            {"logical_offset": 1200, "length": 200},
            {"logical_offset": 1800, "length": 100},
        ]
        self.assertEqual(relative_ram_ranges(1000, 1000, segments),
                         [(200, 400), (800, 900)])

    def test_capability_record_passes_common_fail_closed_validator(self):
        digest = "a" * 64
        record = build_capability_record(
            run_id="tag-laion10m-c3_capability-c8-r2",
            repeat=2,
            concurrency=8,
            binary_sha256=digest,
            image_sha256="b" * 64,
            pages_sha256="c" * 64,
            requested_queries=131073,
            completed_queries=131073,
            before_faults=10,
            after_faults=20,
            before_read_bytes=100,
            after_read_bytes=200,
            block_delta={"nvme3n1": {}, "nvme4n1": {},
                         "total_read_bytes": 100, "total_write_bytes": 0},
            overlapping_processes=[],
            bandwidth_gib_s=1.25,
            mean_aqu_sz=7.5,
            working_set_bytes=4 * 1024**3 + 4096,
            cold_evidence="cold.json",
            driver_sha256="d" * 64,
        )
        validate_record(record)
        self.assertEqual(record["phase"], "c3_capability")
        self.assertEqual(record["condition"], "c8")
        self.assertGreater(record["metrics"]["working_set_bytes"], 4 * 1024**3)


if __name__ == "__main__":
    unittest.main()
