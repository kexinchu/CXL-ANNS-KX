import hashlib
import json
from pathlib import Path
from array import array
import struct
import tempfile
import unittest

from experiments.motivation.flashanns.freeze_trace import TraceError, freeze_trace


def write_u32(path, values):
    payload = array("I", values)
    path.write_bytes(payload.tobytes())


def write_u64(path, values):
    path.write_bytes(struct.pack(f"<{len(values)}Q", *values))


class FreezeTraceTest(unittest.TestCase):
    def make_trace(self, root, nq=10000, width=2):
        trace = root / "trace"
        trace.mkdir()
        write_u32(trace / "query_ids.u32", range(nq))
        offsets = [index * width for index in range(nq + 1)]
        write_u64(trace / "candidate_offsets.u64", offsets)
        write_u32(trace / "candidate_ids.u32", (index % 100000 for index in range(nq * width)))
        write_u32(trace / "result_ids.u32", (index % 100000 for index in range(nq * 10)))
        return trace

    def test_freezes_aligned_trace_and_recomputes_hashes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            trace = self.make_trace(root)
            artifacts = {}
            for name in ("graph", "slot_map", "binary", "image"):
                path = root / name
                path.write_bytes(name.encode())
                artifacts[name] = path
            manifest_path = freeze_trace(trace, root / "frozen", artifacts, L=400, k=10)
            manifest = json.loads(manifest_path.read_text())
            self.assertEqual(manifest["nq"], 10000)
            self.assertEqual(manifest["L"], 400)
            self.assertEqual(manifest["k"], 10)
            for name in ("query_ids.u32", "candidate_offsets.u64", "candidate_ids.u32", "result_ids.u32"):
                copied = root / "frozen" / name
                self.assertEqual(
                    manifest["trace_sha256"][name], hashlib.sha256(copied.read_bytes()).hexdigest()
                )

    def test_rejects_wrong_query_count_duplicate_ids_and_bad_offsets(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            trace = self.make_trace(root, nq=2)
            with self.assertRaisesRegex(TraceError, "10,000"):
                freeze_trace(trace, root / "out", {}, L=400, k=10)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            trace = self.make_trace(root)
            write_u32(trace / "query_ids.u32", [0] * 10000)
            with self.assertRaisesRegex(TraceError, "query IDs"):
                freeze_trace(trace, root / "out", {}, L=400, k=10)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            trace = self.make_trace(root)
            write_u64(trace / "candidate_offsets.u64", [0] * 10001)
            with self.assertRaisesRegex(TraceError, "offset"):
                freeze_trace(trace, root / "out", {}, L=400, k=10)

    def test_rejects_candidate_width_above_L(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            trace = self.make_trace(root, width=401)
            with self.assertRaisesRegex(TraceError, "L"):
                freeze_trace(trace, root / "out", {}, L=400, k=10)


if __name__ == "__main__":
    unittest.main()
