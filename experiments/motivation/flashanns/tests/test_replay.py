from array import array
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from experiments.motivation.flashanns.run_replay import (
    blind_16k_pages,
    build_policy_pages,
    derive_useful_pages,
    page_metrics,
    require_backings,
    two_hop_ids,
)


class ReplayPolicyTest(unittest.TestCase):
    def test_probe_dry_run_exercises_mmap_fallback(self):
        root = Path(__file__).resolve().parents[4]
        binary = root / "tools" / "motivation-replay-probe"
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            image = work / "image.bin"
            image.write_bytes(bytes([17]) * 4096 + bytes([29]) * 4096)
            offsets = array("Q", [0, 2])
            pages = array("Q", [0, 4096])
            (work / "query_offsets.u64").write_bytes(offsets.tobytes())
            (work / "pages.u64").write_bytes(pages.tobytes())
            output = work / "timing.json"
            subprocess.run([
                str(binary), "--device", str(image), "--image-offset", "0",
                "--image-bytes", "8192",
                "--wave-pages", "1",
                "--query-offsets", str(work / "query_offsets.u64"),
                "--pages", str(work / "pages.u64"), "--output", str(output),
                "--dry-run",
            ], check=True)
            result = json.loads(output.read_text())
            self.assertEqual(result["fallback_waves"], 2)
            self.assertNotEqual(result["checksum"], 0)

    def test_live_replay_requires_explicit_backings(self):
        self.assertEqual(require_backings(["nvme3n1", "nvme4n1"]),
                         ("nvme3n1", "nvme4n1"))
        with self.assertRaisesRegex(ValueError, "backing"):
            require_backings(None)
        with self.assertRaisesRegex(ValueError, "distinct"):
            require_backings(["nvme3n1", "nvme3n1"])

    def test_useful_pages_follow_physical_slots_and_remove_duplicates(self):
        slot_map = [2, 0, 1, 3]
        pages = derive_useful_pages(
            [0, 2, 0], slot_map, vector_base=4096, vector_stride=4096,
            vector_bytes=2048, image_bytes=5 * 4096,
        )
        self.assertEqual(pages, [3 * 4096, 2 * 4096])

    def test_blind_16k_expands_and_clamps_groups(self):
        pages = blind_16k_pages([4096, 5 * 4096], image_bytes=7 * 4096)
        self.assertEqual(pages, [0, 4096, 8192, 12288, 16384, 20480, 24576])

    def test_two_hop_expansion_is_ordered_and_deduplicated(self):
        graph = {0: [1, 2], 1: [2, 3], 2: [3, 4], 3: [], 4: []}
        self.assertEqual(two_hop_ids([0], graph), [0, 1, 2, 3, 4])

    def test_topn_uses_optimistic_final_roots_but_still_fetches_useful_set(self):
        graph = {0: [1], 1: [2], 2: [], 3: []}
        slot_map = [0, 1, 2, 3]
        useful = derive_useful_pages(
            [2, 3], slot_map, vector_base=4096, vector_stride=4096,
            vector_bytes=2048, image_bytes=5 * 4096,
        )
        pages = build_policy_pages(
            "top1", candidates=[2, 3], results=[0, 3], graph=graph,
            slot_map=slot_map, vector_base=4096, vector_stride=4096,
            vector_bytes=2048, image_bytes=5 * 4096,
        )
        self.assertTrue(set(useful).issubset(pages))
        self.assertEqual(pages, [4096, 8192, 12288, 16384])

    def test_metrics_report_page_use_and_physical_amplification(self):
        metrics = page_metrics([4096, 8192], [0, 4096, 8192, 12288])
        self.assertEqual(metrics["useful_page_pct"], 50.0)
        self.assertEqual(metrics["physical_amplification"], 2.0)

    def test_rejects_bad_ids_and_out_of_bounds_pages(self):
        with self.assertRaisesRegex(ValueError, "ID"):
            derive_useful_pages([2], [0, 1], 4096, 4096, 2048, 12288)
        with self.assertRaisesRegex(ValueError, "bounds"):
            derive_useful_pages([1], [0, 2], 4096, 4096, 4096, 12288)


if __name__ == "__main__":
    unittest.main()
