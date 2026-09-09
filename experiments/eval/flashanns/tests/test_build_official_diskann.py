import math
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

from experiments.eval.flashanns.build_official_diskann import (
    prepare_mips_base,
    write_vamana_from_flat,
)


class OfficialDiskannIndexBuilderTest(unittest.TestCase):
    def test_wrap_cli_creates_loadable_vamana_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            graph = root / "graph.bin"
            graph.write_bytes(struct.pack("<4I", 1, 0, 0, 1))
            output = root / "mem.index"
            completed = subprocess.run(
                [
                    sys.executable,
                    "-m",
                    "experiments.eval.flashanns.build_official_diskann",
                    "wrap-graph",
                    "--graph",
                    str(graph),
                    "--output",
                    str(output),
                    "--n",
                    "2",
                    "--degree",
                    "2",
                    "--entry-id",
                    "0",
                ],
                text=True,
                capture_output=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(struct.unpack_from("<QIIQ", output.read_bytes())[1:], (2, 0, 0))

    def test_vamana_wrapper_preserves_every_flat_neighbor(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            graph = root / "graph.bin"
            graph.write_bytes(struct.pack("<6I", 1, 2, 2, 0, 0, 1))
            output = root / "mem.index"

            write_vamana_from_flat(graph, output, n=3, degree=2, entry_id=1)

            raw = output.read_bytes()
            expected_size = 24 + 3 * (4 + 2 * 4)
            self.assertEqual(len(raw), expected_size)
            self.assertEqual(
                struct.unpack_from("<QIIQ", raw),
                (expected_size, 2, 1, 0),
            )
            offset = 24
            rows = []
            for _ in range(3):
                row_degree = struct.unpack_from("<I", raw, offset)[0]
                offset += 4
                rows.append(struct.unpack_from("<2I", raw, offset))
                offset += 8
                self.assertEqual(row_degree, 2)
            self.assertEqual(rows, [(1, 2), (2, 0), (0, 1)])

    def test_vamana_wrapper_rejects_neighbor_outside_dataset(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            graph = root / "graph.bin"
            graph.write_bytes(struct.pack("<2I", 1, 2))
            with self.assertRaisesRegex(ValueError, "neighbor.*outside"):
                write_vamana_from_flat(
                    graph, root / "mem.index", n=2, degree=1, entry_id=0
                )

    def test_mips_preparation_matches_diskann_transformation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "base.fbin"
            with source.open("wb") as stream:
                stream.write(struct.pack("<II", 2, 2))
                stream.write(struct.pack("<4f", 3.0, 4.0, 0.0, 0.0))

            maximum = prepare_mips_base(
                source, root / "prepped.fbin", root / "max_norm.bin"
            )

            self.assertEqual(maximum, 5.0)
            raw = (root / "prepped.fbin").read_bytes()
            self.assertEqual(struct.unpack_from("<II", raw), (2, 3))
            values = struct.unpack_from("<6f", raw, 8)
            for got, want in zip(values, (0.6, 0.8, 0.0, 0.0, 0.0, 1.0)):
                self.assertTrue(math.isclose(got, want, abs_tol=1e-6))
            self.assertEqual(
                (root / "max_norm.bin").read_bytes(),
                struct.pack("<IIf", 1, 1, 5.0),
            )


if __name__ == "__main__":
    unittest.main()
