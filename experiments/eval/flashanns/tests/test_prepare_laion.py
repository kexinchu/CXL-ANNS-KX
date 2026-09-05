import json
import struct
import tempfile
import unittest
from pathlib import Path

import numpy as np

from experiments.eval.flashanns.prepare_laion import prepare_prefix_and_queries


class PrepareLaionTest(unittest.TestCase):
    def test_deterministic_prefix_and_unique_query_selection(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            base = np.arange(25 * 4, dtype="<f4").reshape(25, 4)
            queries = (1000 + np.arange(20 * 4, dtype="<f4")).reshape(20, 4)
            base_path, query_path, out = root / "base.fbin", root / "query.fbin", root / "out"
            base_path.write_bytes(struct.pack("<II", 25, 4) + base.tobytes())
            query_path.write_bytes(struct.pack("<II", 20, 4) + queries.tobytes())
            first = prepare_prefix_and_queries(base_path, query_path, out, 10, 7, 42)
            ids = first["query_ids"]
            self.assertEqual(len(ids), len(set(ids)))
            self.assertTrue(all(0 <= value < 20 for value in ids))
            raw = (out / "base.10M.fbin").read_bytes()
            np.testing.assert_array_equal(np.frombuffer(raw, dtype="<f4", offset=8).reshape(10, 4), base[:10])
            second_out = root / "out2"
            second = prepare_prefix_and_queries(base_path, query_path, second_out, 10, 7, 42)
            self.assertEqual(ids, second["query_ids"])
            self.assertEqual(first["base_prefix_sha256"], second["base_prefix_sha256"])
            self.assertEqual(json.loads((out / "subset-manifest.json").read_text())["corpus_ids"], [0, 10])


if __name__ == "__main__":
    unittest.main()
