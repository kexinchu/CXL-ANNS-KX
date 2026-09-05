import errno
import os
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from experiments.eval.flashanns import preflight
from experiments.eval.flashanns.preflight import (
    PreflightError,
    atomic_json_write,
    full_layout_digest,
    sampled_layout_digest,
    snapshot,
    snapshot_and_validate,
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

    def test_full_digest_honors_exact_offset_and_length(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "image.bin"
            path.write_bytes(b"prefix" + b"payload" + b"suffix")
            import hashlib

            self.assertEqual(
                full_layout_digest(path, 6, 7, block_bytes=3),
                hashlib.sha256(b"payload").hexdigest(),
            )

    def test_digest_uses_mmap_for_nonseekable_character_device(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "image.bin"
            path.write_bytes(bytes(range(256)) * 32)
            with mock.patch.object(
                os, "pread", side_effect=OSError(errno.ESPIPE, "Illegal seek")
            ):
                digest = sampled_layout_digest(path, 0, 8192, pages=2, seed=7)
            self.assertEqual(len(digest), 64)

    def test_snapshot_reads_magic_without_pread(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            sysfs = root / "sysfs"
            sysfs.mkdir()
            device = root / "vmem0"
            device.write_bytes(struct.pack("<Q", 0x314E415843) + bytes(4088))
            dataset = {"staging": {"offset": 0}}
            with mock.patch.object(
                os, "pread", side_effect=OSError(errno.ESPIPE, "Illegal seek")
            ):
                record = snapshot(sysfs, device, dataset)
            self.assertEqual(record["image_magic"], 0x314E415843)

    def test_accepted_identity_evidence_does_not_read_device_or_warm_cold_cache(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            sysfs = root / "sysfs"
            sysfs.mkdir()
            fields = {
                "backend": "software",
                "backing_count": "2",
                "nvme_dev": "/dev/nvme1n1,/dev/nvme2n1",
                "target_bdf": "0000:d8:00.0,0000:d9:00.0",
                "cache_limit": "4294967296",
                "cache_used": "0",
                "dirty_bytes": "0",
                "io_errors": "0",
                "evictions": "0",
                "ram_size": "30064771072",
                "ssd_size": "3840766820352",
                "stripe_size": "2097152",
            }
            for name, value in fields.items():
                (sysfs / name).write_text(value)
            device = root / "vmem0"
            device.write_bytes(bytes(4096))
            host = root / "host.bin"
            host.write_bytes(bytes(4096))
            contract = dict(
                self.contract,
                device=str(device),
                sysfs=str(sysfs),
                sample_pages=1,
                sample_seed=7,
            )
            dataset = {
                "artifacts": {"extent_image": str(host)},
                "staging": {
                    "offset": 0,
                    "length": 4096,
                    "host_artifact": "extent_image",
                    "magic": 0x314E415843,
                },
            }
            digest = sampled_layout_digest(host, 0, 4096, 1, 7)
            evidence = {
                "accepted": True,
                "identity_scope": "full",
                "full_host_sha256": "f" * 64,
                "full_device_sha256": "f" * 64,
                "image_magic": 0x314E415843,
                "host_digest": digest,
                "device_digest": digest,
                "backend": "software",
                "backing_count": 2,
                "nvme_dev": ["/dev/nvme1n1", "/dev/nvme2n1"],
                "target_bdf": ["0000:d8:00.0", "0000:d9:00.0"],
            }
            volatile = {
                "accepted": True,
                "dataset": "t2i10m",
                "source": str(host),
                "staging_offset": 0,
                "staging_length": 4096,
                "source_ram_sha256": "ram",
                "device_ram_sha256": "ram",
                "cache_used": 0,
                "dirty_bytes": 0,
                "io_errors": 0,
                "layout": {
                    "ram_size": 30064771072,
                    "ssd_size": 3840766820352,
                    "stripe_size": 2097152,
                },
            }
            real_mmap_page = preflight._mmap_page

            def reject_device_read(path, offset):
                if Path(path) == device:
                    raise AssertionError("device must not be read")
                return real_mmap_page(path, offset)

            with mock.patch.object(preflight, "_mmap_page", side_effect=reject_device_read):
                record = snapshot_and_validate(
                    contract, dataset, "cold", evidence, volatile
                )
            self.assertEqual(record["cache_used"], 0)
            self.assertTrue(record["identity_evidence_reused"])
            self.assertTrue(record["volatile_evidence_reused"])

            with self.assertRaisesRegex(PreflightError, "volatile RAM-stripe evidence"):
                snapshot_and_validate(contract, dataset, "cold", evidence)

            sampled = dict(evidence, identity_scope="sampled")
            with self.assertRaisesRegex(PreflightError, "full-stage identity"):
                snapshot_and_validate(contract, dataset, "cold", sampled, volatile)

    def test_atomic_json_write_leaves_no_tmp(self):
        with tempfile.TemporaryDirectory() as td:
            output = Path(td) / "record.json"
            atomic_json_write(output, {"b": 2, "a": 1})
            self.assertEqual(output.read_text(), '{\n  "a": 1,\n  "b": 2\n}\n')
            self.assertFalse(Path(str(output) + ".tmp").exists())


if __name__ == "__main__":
    unittest.main()
