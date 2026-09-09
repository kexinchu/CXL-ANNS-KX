import copy
import json
from pathlib import Path
import tempfile
import unittest

from experiments.motivation.flashanns.records import (
    RecordValidationError,
    seal_record,
    validate_block,
    validate_c2_same_trace,
    validate_record,
)


HASH = "a" * 64


def valid_record(**updates):
    record = {
        "schema_version": 1,
        "run_id": "c1-laion-host-r0",
        "phase": "c1_tier",
        "dataset": "laion10m",
        "condition": "host",
        "repeat": 0,
        "state": "complete",
        "identities": {
            "binary_sha256": HASH,
            "image_sha256": "b" * 64,
            "query_ids_sha256": "c" * 64,
        },
        "geometry": {
            "cache_bytes": 4 * 1024**3,
            "page_bytes": 4096,
            "stripe_bytes": 2 * 1024**2,
        },
        "operations": {"requested": 2000, "completed": 2000},
        "counters": {
            "before": {"vmem_faults": 0, "nvme_read_bytes": 0},
            "after": {"vmem_faults": 0, "nvme_read_bytes": 0},
        },
        "exclusive": {"overlapping_processes": [], "other_io_detected": False},
        "metrics": {"latency_us_p50": 1.0},
        "validation": {"status": "pending"},
    }
    for key, value in updates.items():
        record[key] = value
    return record


class RecordTest(unittest.TestCase):
    def assert_rejected(self, record, fragment):
        with self.assertRaisesRegex(RecordValidationError, fragment):
            validate_record(record)

    def test_accepts_complete_host_record(self):
        validate_record(valid_record())

    def test_requires_binary_image_and_query_hashes(self):
        for field in ("binary_sha256", "image_sha256", "query_ids_sha256"):
            record = valid_record()
            del record["identities"][field]
            self.assert_rejected(record, field)

    def test_rejects_wrong_geometry(self):
        for field, value in (("cache_bytes", 1), ("page_bytes", 8192), ("stripe_bytes", 4096)):
            record = valid_record()
            record["geometry"][field] = value
            self.assert_rejected(record, field)

    def test_rejects_incomplete_operations_and_negative_delta(self):
        record = valid_record()
        record["operations"]["completed"] = 1999
        self.assert_rejected(record, "operations")
        record = valid_record()
        record["counters"]["after"]["vmem_faults"] = -1
        self.assert_rejected(record, "negative counter delta")

    def test_rejects_overlapping_or_other_device_io(self):
        record = valid_record()
        record["exclusive"]["overlapping_processes"] = ["fio"]
        self.assert_rejected(record, "overlapping")
        record = valid_record()
        record["exclusive"]["other_io_detected"] = True
        self.assert_rejected(record, "other I/O")

    def test_tier_must_agree_with_counter_deltas(self):
        record = valid_record()
        record["condition"] = "cxl_cache"
        self.assert_rejected(record, "tier classification")
        record["counters"]["after"]["vmem_faults"] = 2000
        validate_record(record)
        record["condition"] = "flash"
        self.assert_rejected(record, "tier classification")
        record["counters"]["after"]["nvme_read_bytes"] = 8192000
        validate_record(record)

    def test_block_requires_exactly_five_unique_repeats(self):
        records = []
        for repeat in range(5):
            item = valid_record()
            item["repeat"] = repeat
            item["run_id"] = f"c1-laion-host-r{repeat}"
            records.append(item)
        validate_block(records)
        with self.assertRaisesRegex(RecordValidationError, "five"):
            validate_block(records[:4])
        duplicate = copy.deepcopy(records)
        duplicate[-1]["repeat"] = 3
        with self.assertRaisesRegex(RecordValidationError, "repeat"):
            validate_block(duplicate)

    def test_c2_comparisons_require_identical_trace_and_recall(self):
        records = []
        for condition in ("demand", "top1"):
            item = valid_record(
                phase="c2_coverage",
                condition=condition,
                sidecars={
                    "query_ids_sha256": "1" * 64,
                    "candidate_offsets_sha256": "2" * 64,
                    "candidate_ids_sha256": "3" * 64,
                    "result_ids_sha256": "4" * 64,
                    "useful_pages_sha256": "5" * 64,
                },
                metrics={"recall_at_10": 0.92},
            )
            records.append(item)
        validate_c2_same_trace(records)
        records[1]["sidecars"]["candidate_ids_sha256"] = "6" * 64
        with self.assertRaisesRegex(RecordValidationError, "candidate_ids_sha256"):
            validate_c2_same_trace(records)

    def test_seal_copies_validated_record_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            raw = root / "raw" / "run.json"
            raw.parent.mkdir()
            original = valid_record()
            raw.write_text(json.dumps(original), encoding="utf-8")
            accepted = seal_record(raw, root / "accepted")
            self.assertEqual(json.loads(raw.read_text()), original)
            sealed = json.loads(accepted.read_text())
            self.assertEqual(sealed["validation"]["status"], "accepted")
            with self.assertRaisesRegex(RecordValidationError, "refusing existing"):
                seal_record(raw, root / "accepted")


if __name__ == "__main__":
    unittest.main()
