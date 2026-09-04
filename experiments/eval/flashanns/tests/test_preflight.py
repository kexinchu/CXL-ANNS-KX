import tempfile
import unittest
from pathlib import Path

from experiments.eval.flashanns.preflight import (
    PreflightError,
    atomic_json_write,
    sampled_layout_digest,
    validate,
)


class PreflightTest(unittest.TestCase):
    def setUp(self):
        self.contract = {
            "cache_limit": 4294967296,
            "required_dirty_bytes": 0,
            "required_io_errors": 0,
            "backend": "software",
            "backing_count": 2,
            "nvme_dev": ["/dev/nvme1n1", "/dev/nvme2n1"],
            "target_bdf": ["0000:d8:00.0", "0000:d9:00.0"],
        }
        self.dataset = {"staging": {"magic": 0x314E415843}}
        self.good = {
            "device_exists": True,
            "backend": "software",
            "backing_count": 2,
            "nvme_dev": ["/dev/nvme1n1", "/dev/nvme2n1"],
            "target_bdf": ["0000:d8:00.0", "0000:d9:00.0"],
            "cache_limit": 4294967296,
            "cache_used": 0,
            "dirty_bytes": 0,
            "io_errors": 0,
            "open_users": [],
            "image_magic": 0x314E415843,
            "host_digest": "same",
            "device_digest": "same",
            "cold_parent_accepted": False,
        }

    def test_accepts_clean_cold_snapshot(self):
        validate(self.good, self.contract, self.dataset, "cold")

    def test_accumulates_all_identity_and_cold_failures(self):
        bad = dict(self.good)
        bad.update(
            device_exists=False,
            backend="wrong",
            backing_count=1,
            nvme_dev=["wrong"],
            target_bdf=["wrong"],
            cache_limit=1,
            cache_used=1,
            dirty_bytes=2,
            io_errors=3,
            open_users=[123],
            image_magic=0,
            device_digest="different",
        )
        with self.assertRaises(PreflightError) as caught:
            validate(bad, self.contract, self.dataset, "cold")
        text = str(caught.exception)
        for field in (
            "device",
            "backend",
            "backing_count",
            "nvme_dev",
            "target_bdf",
            "cache_limit",
            "cache_used",
            "dirty_bytes",
            "io_errors",
            "open_users",
            "image_magic",
            "sampled staged-image",
        ):
            self.assertIn(field, text)

    def test_warm_requires_accepted_cold_parent(self):
        warm = dict(self.good, cache_used=4096)
        with self.assertRaisesRegex(PreflightError, "cold-parent"):
            validate(warm, self.contract, self.dataset, "warm")
        validate(dict(warm, cold_parent_accepted=True), self.contract, self.dataset, "warm")

    def test_digest_is_seeded_and_offset_bounded(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "image.bin"
            path.write_bytes(bytes(range(256)) * 64)
            first = sampled_layout_digest(path, 4096, 8192, pages=2, seed=7)
            self.assertEqual(first, sampled_layout_digest(path, 4096, 8192, pages=2, seed=7))
            self.assertNotEqual(first, sampled_layout_digest(path, 4096, 8192, pages=2, seed=8))

    def test_atomic_json_write_leaves_no_tmp(self):
        with tempfile.TemporaryDirectory() as td:
            output = Path(td) / "record.json"
            atomic_json_write(output, {"b": 2, "a": 1})
            self.assertEqual(output.read_text(), '{\n  "a": 1,\n  "b": 2\n}\n')
            self.assertFalse(Path(str(output) + ".tmp").exists())


if __name__ == "__main__":
    unittest.main()
