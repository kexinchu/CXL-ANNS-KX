from pathlib import Path
import tempfile
import unittest

from experiments.motivation.flashanns.observe import (
    block_delta,
    find_open_users,
    parse_iostat,
    read_block_snapshot,
    read_vmem_snapshot,
    verify_dual_backing,
)


class ObserveTest(unittest.TestCase):
    def test_reads_vmem_attributes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "cache_limit").write_text("4294967296\n")
            (root / "cache_used").write_text("4096\n")
            (root / "dirty_bytes").write_text("0\n")
            (root / "io_errors").write_text("0\n")
            self.assertEqual(
                read_vmem_snapshot(root),
                {"cache_limit": 4294967296, "cache_used": 4096, "dirty_bytes": 0, "io_errors": 0},
            )

    def test_block_snapshot_and_delta_cover_both_backings(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name, values in {
                "nvme1n1": "10 0 100 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
                "nvme2n1": "20 0 200 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
            }.items():
                path = root / name
                path.mkdir()
                (path / "stat").write_text(values)
            before = read_block_snapshot(("nvme1n1", "nvme2n1"), root)
            (root / "nvme1n1" / "stat").write_text("12 0 108 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n")
            (root / "nvme2n1" / "stat").write_text("23 0 216 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n")
            after = read_block_snapshot(("nvme1n1", "nvme2n1"), root)
            delta = block_delta(before, after)
            self.assertEqual(delta["nvme1n1"]["read_ios"], 2)
            self.assertEqual(delta["nvme2n1"]["read_sectors"], 16)
            self.assertEqual(delta["total_read_bytes"], 24 * 512)

    def test_block_delta_allows_in_flight_gauge_to_decrease(self):
        fields = {
            "read_ios": 10, "read_merges": 0, "read_sectors": 100,
            "read_ticks_ms": 5, "write_ios": 0, "write_merges": 0,
            "write_sectors": 0, "write_ticks_ms": 0, "in_flight": 7,
            "io_ticks_ms": 5, "weighted_io_ticks_ms": 8,
        }
        before = {"nvme": fields}
        after = {"nvme": {**fields, "read_ios": 11, "in_flight": 2}}
        delta = block_delta(before, after)
        self.assertEqual(delta["nvme"]["in_flight"], -5)

    def test_block_delta_still_rejects_cumulative_counter_decrease(self):
        before = {"nvme": {"read_ios": 10, "in_flight": 0}}
        after = {"nvme": {"read_ios": 9, "in_flight": 0}}
        with self.assertRaisesRegex(ValueError, "negative"):
            block_delta(before, after)

    def test_dual_backing_identity_rejects_aliases(self):
        good = [
            {"name": "nvme1n1", "serial": "A", "bdf": "0000:d8:00.0"},
            {"name": "nvme2n1", "serial": "B", "bdf": "0000:d9:00.0"},
        ]
        verify_dual_backing(good)
        bad = [good[0], {**good[1], "serial": "A"}]
        with self.assertRaisesRegex(ValueError, "distinct"):
            verify_dual_backing(bad)

    def test_detects_process_with_open_device(self):
        with tempfile.TemporaryDirectory() as tmp:
            proc = Path(tmp)
            device = proc / "vmem0"
            device.touch()
            fd_dir = proc / "123" / "fd"
            fd_dir.mkdir(parents=True)
            (proc / "123" / "cmdline").write_bytes(b"search_beam\0--x\0")
            (fd_dir / "7").symlink_to(device)
            users = find_open_users(device, proc)
            self.assertEqual(users, [{"pid": 123, "fd": 7, "command": "search_beam --x"}])

    def test_iostat_parser_keeps_idle_intervals(self):
        sample = """
Device            r/s   rkB/s aqu-sz await
nvme1n1          0.00    0.00   0.00  0.00
nvme2n1          0.00    0.00   0.00  0.00

Device            r/s   rkB/s aqu-sz await
nvme1n1        100.00  400.00   2.00  1.00
nvme2n1         50.00  200.00   4.00  1.00
"""
        parsed = parse_iostat(sample, ("nvme1n1", "nvme2n1"))
        self.assertEqual(len(parsed["intervals"]), 2)
        self.assertEqual(parsed["intervals"][0]["aqu_sz"], 0.0)
        self.assertEqual(parsed["intervals"][1]["aqu_sz"], 6.0)
        self.assertEqual(parsed["mean_aqu_sz"], 3.0)
        self.assertFalse(parsed["active_only"])


if __name__ == "__main__":
    unittest.main()
