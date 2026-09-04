import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from experiments.eval.flashanns.freeze_artifacts import ArtifactError, freeze_dataset, hash_file


class FreezeArtifactsTest(unittest.TestCase):
    def test_stream_hash_and_sorted_manifest(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            a = root / "a.bin"
            b = root / "b.bin"
            out = root / "manifest.json"
            a.write_bytes(b"abc")
            b.write_bytes(b"defg")
            self.assertEqual(hash_file(a, block_bytes=2)["sha256"], hashlib.sha256(b"abc").hexdigest())
            dataset = {
                "id": "tiny",
                "ready": True,
                "expected_sizes": {"a": 3, "b": 4},
                "artifacts": {"b": str(b), "a": str(a)},
            }
            manifest = freeze_dataset(dataset, out)
            self.assertEqual(list(manifest["artifacts"]), ["a", "b"])
            self.assertEqual(json.loads(out.read_text()), manifest)
            self.assertFalse((root / "manifest.json.tmp").exists())

    def test_rejects_missing_or_wrong_size_artifact(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            path = root / "a.bin"
            path.write_bytes(b"abc")
            with self.assertRaisesRegex(ArtifactError, "a"):
                freeze_dataset(
                    {"id": "tiny", "ready": True, "expected_sizes": {"a": 4}, "artifacts": {"a": str(path)}},
                    root / "bad.json",
                )
            with self.assertRaisesRegex(ArtifactError, "missing"):
                hash_file(root / "missing")


if __name__ == "__main__":
    unittest.main()
