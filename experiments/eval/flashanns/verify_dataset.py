"""Structural and sampled-content verification for FlashANNS datasets."""

from __future__ import annotations

import argparse
import array
import heapq
import json
import random
import struct
import sys
from pathlib import Path
from typing import Any, Iterable, Sequence

from experiments.eval.flashanns.config import load_configs


class DatasetError(ValueError):
    """A dataset artifact violates its declared format or identity."""


CXAN_MAGIC = 0x314E415843
CXAN_HEADER = struct.Struct("<Q9I9Q")


def read_bin_header(path: Path, item_size: int) -> tuple[int, int]:
    path = Path(path)
    try:
        size = path.stat().st_size
        with path.open("rb") as src:
            raw = src.read(8)
    except OSError as exc:
        raise DatasetError(f"{path.name}: {exc}") from exc
    if len(raw) != 8 or item_size <= 0:
        raise DatasetError(f"{path.name}: missing header or invalid item size")
    n, dim = struct.unpack("<II", raw)
    expected = 8 + n * dim * item_size
    if not n or not dim or size != expected:
        raise DatasetError(f"{path.name}: size {size} != expected {expected} for {n}x{dim}")
    return n, dim


def validate_query_ids(ids: list[int], available: int, required: int = 10000) -> None:
    if len(ids) != required:
        raise DatasetError(f"query_ids: count {len(ids)} != required {required}")
    if len(set(ids)) != len(ids):
        raise DatasetError("query_ids: duplicate ID")
    bad = [value for value in ids if value < 0 or value >= available]
    if bad:
        raise DatasetError(f"query_ids: out-of-range ID {bad[0]} for {available}")


def validate_gt_subset(
    source_gt: Path, query_ids: list[int], output_gt: Path, k: int = 10
) -> None:
    source_gt = Path(source_gt)
    n, source_k = read_bin_header(source_gt, 4)
    if k <= 0 or k > source_k:
        raise DatasetError(f"{source_gt.name}: requested k={k} exceeds {source_k}")
    validate_query_ids(query_ids, n, required=len(query_ids))
    rows: list[tuple[int, ...]] = []
    try:
        with source_gt.open("rb") as src:
            for query_id in query_ids:
                src.seek(8 + query_id * source_k * 4)
                raw = src.read(k * 4)
                if len(raw) != k * 4:
                    raise DatasetError(f"{source_gt.name}: short GT row {query_id}")
                rows.append(struct.unpack(f"<{k}I", raw))
        output_gt = Path(output_gt)
        output_gt.parent.mkdir(parents=True, exist_ok=True)
        with output_gt.open("wb") as dst:
            dst.write(struct.pack("<II", len(rows), k))
            for row in rows:
                dst.write(struct.pack(f"<{k}I", *row))
    except OSError as exc:
        raise DatasetError(f"{source_gt.name}: {exc}") from exc
    read_bin_header(output_gt, 4)
    with output_gt.open("rb") as check:
        check.seek(8)
        reread = check.read()
    expected = b"".join(struct.pack(f"<{k}I", *row) for row in rows)
    if reread != expected:
        raise DatasetError(f"{output_gt.name}: GT reread mismatch")


def validate_permutation(values: Iterable[int], n: int) -> None:
    values = list(values)
    if len(values) != n:
        raise DatasetError(f"permutation: count {len(values)} != {n}")
    seen = bytearray(n)
    for value in values:
        if value < 0 or value >= n or seen[value]:
            raise DatasetError(f"permutation: invalid or duplicate value {value}")
        seen[value] = 1


def _matrix_row(path: Path, item_size: int, code: str, row: int, dim: int) -> tuple[Any, ...]:
    with Path(path).open("rb") as src:
        src.seek(8 + row * dim * item_size)
        raw = src.read(dim * item_size)
    if len(raw) != dim * item_size:
        raise DatasetError(f"{Path(path).name}: short row {row}")
    return struct.unpack(f"<{dim}{code}", raw)


def compare_widened_u8(
    native_path: Path, float_path: Path, sample_ids: list[int], dim: int
) -> None:
    native_n, native_dim = read_bin_header(native_path, 1)
    float_n, float_dim = read_bin_header(float_path, 4)
    if (native_n, native_dim) != (float_n, float_dim) or native_dim != dim:
        raise DatasetError("widened coordinates: header mismatch")
    for row_id in sample_ids:
        if row_id < 0 or row_id >= native_n:
            raise DatasetError(f"widened coordinates: sample ID {row_id} out of range")
        native = _matrix_row(native_path, 1, "B", row_id, dim)
        widened = _matrix_row(float_path, 4, "f", row_id, dim)
        for column, (left, right) in enumerate(zip(native, widened)):
            if float(left) != right:
                raise DatasetError(
                    f"widened coordinates: row {row_id} column {column}: {left} != {right}"
                )


def _l2_topk(matrix_path: Path, dtype: str, query: Any, k: int) -> list[int]:
    try:
        import numpy as np
    except ImportError as exc:
        raise DatasetError("L2 top-k equivalence requires NumPy") from exc
    item_size = 1 if dtype == "uint8" else 4
    n, dim = read_bin_header(matrix_path, item_size)
    matrix = np.memmap(matrix_path, mode="r", dtype=np.uint8 if dtype == "uint8" else "<f4", offset=8, shape=(n, dim))
    query64 = np.asarray(query, dtype=np.float64)
    best: list[tuple[float, int]] = []
    for start in range(0, n, 65536):
        block = np.asarray(matrix[start : start + 65536], dtype=np.float64)
        distances = np.square(block - query64).sum(axis=1)
        take = min(k, len(distances))
        if take:
            local = np.argpartition(distances, take - 1)[:take]
            candidates = best + [(float(distances[i]), start + int(i)) for i in local]
            best = heapq.nsmallest(k, candidates)
    return [item_id for _, item_id in best]


def validate_l2_topk_equivalence(
    native_path: Path, float_path: Path, queries_path: Path, query_ids: list[int], k: int = 10
) -> None:
    _, dim = read_bin_header(native_path, 1)
    query_n, query_dim = read_bin_header(queries_path, 1)
    if query_dim != dim:
        raise DatasetError("L2 top-k equivalence: query dimension mismatch")
    validate_query_ids(query_ids, query_n, required=len(query_ids))
    for query_id in query_ids:
        query = _matrix_row(queries_path, 1, "B", query_id, dim)
        native_ids = _l2_topk(native_path, "uint8", query, k)
        widened_ids = _l2_topk(float_path, "float32", query, k)
        if native_ids != widened_ids:
            raise DatasetError(f"L2 top-k equivalence: query {query_id} IDs differ")


def _read_u32_file(path: Path, count: int, name: str) -> array.array:
    try:
        raw = Path(path).read_bytes()
    except OSError as exc:
        raise DatasetError(f"{name}: {exc}") from exc
    if len(raw) != count * 4:
        raise DatasetError(f"{name}: size {len(raw)} != {count * 4}")
    values = array.array("I")
    values.frombytes(raw)
    if sys.byteorder != "little":
        values.byteswap()
    return values


def verify_packed_readback(
    dataset: dict[str, Any], sample_count: int = 1024, seed: int = 20260904
) -> dict[str, Any]:
    artifacts = dataset.get("artifacts", {})
    required = (
        "execution_base",
        "oracle_image",
        "extent_image",
        "graph",
        "id_map",
        "slot_map",
    )
    for key in required:
        if not artifacts.get(key):
            raise DatasetError(f"{key}: missing packed-readback artifact")
    n = int(dataset["count"])
    dim = int(dataset["dimension"])
    degree = int(dataset.get("R", 32))
    item_size = 4 if dataset.get("execution_dtype") == "float32" else 1
    code = "f" if item_size == 4 else "B"
    if read_bin_header(Path(artifacts["execution_base"]), item_size) != (n, dim):
        raise DatasetError("execution_base: header differs from dataset")
    id_map = _read_u32_file(Path(artifacts["id_map"]), n, "id_map")
    slot_map = _read_u32_file(Path(artifacts["slot_map"]), n, "slot_map")
    validate_permutation(id_map, n)
    validate_permutation(slot_map, n)
    graph_path = Path(artifacts["graph"])
    if graph_path.stat().st_size != n * degree * 4:
        raise DatasetError(f"graph: size differs from {n}x{degree}")
    sample_n = min(sample_count, n)
    sample_ids = random.Random(seed).sample(range(n), sample_n)

    def verify_image(image_key: str, slots: Sequence[int]) -> dict[str, Any]:
        image_path = Path(artifacts[image_key])
        with image_path.open("rb") as image:
            raw_header = image.read(CXAN_HEADER.size)
        if len(raw_header) != CXAN_HEADER.size:
            raise DatasetError(f"{image_key}: short packed header")
        fields = CXAN_HEADER.unpack(raw_header)
        magic, version, hn, hdim, hdegree, _, vec_bytes = fields[:7]
        off_vectors, len_vectors = fields[14], fields[15]
        if magic != CXAN_MAGIC or version < 2:
            raise DatasetError(f"{image_key}: bad magic or version")
        if (hn, hdim, hdegree, vec_bytes) != (n, dim, degree, item_size):
            raise DatasetError(f"{image_key}: header differs from dataset")
        if not n or len_vectors % n:
            raise DatasetError(f"{image_key}: invalid vector region")
        stride = len_vectors // n
        if stride < dim * item_size + 4 + degree * 4:
            raise DatasetError(f"{image_key}: record stride is too small")
        if image_path.stat().st_size != off_vectors + len_vectors:
            raise DatasetError(f"{image_key}: file size differs from declared vector region")
        with (
            Path(artifacts["execution_base"]).open("rb") as base,
            graph_path.open("rb") as graph,
            image_path.open("rb") as image,
        ):
            for logical_id in sample_ids:
                old_id = id_map[logical_id]
                slot = slots[logical_id]
                base.seek(8 + old_id * dim * item_size)
                expected_vector = base.read(dim * item_size)
                graph.seek(logical_id * degree * 4)
                expected_neighbors = graph.read(degree * 4)
                image.seek(off_vectors + slot * stride)
                actual_vector = image.read(dim * item_size)
                raw_count = image.read(4)
                actual_neighbors = image.read(degree * 4)
                if expected_vector != actual_vector:
                    raise DatasetError(
                        f"{image_key}: vector mismatch for logical ID {logical_id}"
                    )
                if len(raw_count) != 4 or struct.unpack("<I", raw_count)[0] != degree:
                    raise DatasetError(
                        f"{image_key}: neighbor count mismatch for logical ID {logical_id}"
                    )
                if expected_neighbors != actual_neighbors:
                    raise DatasetError(
                        f"{image_key}: neighbors mismatch for logical ID {logical_id}"
                    )
        return {
            "sampled_records": sample_n,
            "sample_seed": seed,
            "sample_ids": sample_ids,
            "stride": stride,
        }

    oracle_proof = verify_image("oracle_image", range(n))
    extent_proof = verify_image("extent_image", slot_map)
    return {
        **extent_proof,
        "oracle_image": oracle_proof,
        "extent_image": extent_proof,
    }


def verify_dataset(dataset: dict[str, Any], full: bool) -> dict[str, Any]:
    artifacts = dataset["artifacts"]
    source_size = 1 if dataset["source_dtype"] == "uint8" else 4
    execution_size = 1 if dataset["execution_dtype"] == "uint8" else 4
    source = read_bin_header(Path(artifacts["source_base"]), source_size)
    queries = read_bin_header(Path(artifacts["source_queries"]), source_size)
    source_count = int(dataset.get("source_count", dataset["count"]))
    if source != (source_count, dataset["dimension"]):
        raise DatasetError("source_base: header differs from dataset")
    if queries[1] != dataset["dimension"]:
        raise DatasetError("source_queries: dimension differs from dataset")
    proof: dict[str, Any] = {"source": source, "queries": queries, "full": full}
    if full:
        if not dataset.get("ready"):
            raise DatasetError("dataset is not marked ready")
        if read_bin_header(Path(artifacts["execution_base"]), execution_size) != (dataset["count"], dataset["dimension"]):
            raise DatasetError("execution_base: header differs from execution subset")
        if read_bin_header(Path(artifacts["query_subset"]), 4) != (10000, dataset["dimension"]):
            raise DatasetError("query_subset: expected 10000 rows")
        if read_bin_header(Path(artifacts["ground_truth"]), 4) != (10000, 10):
            raise DatasetError("ground_truth: expected 10000x10")
        proof["packed_readback"] = verify_packed_readback(dataset)
    return proof


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--source-only", action="store_true")
    mode.add_argument("--full", action="store_true")
    args = parser.parse_args()
    repo_root = Path(__file__).resolve().parents[3]
    datasets, _, _ = load_configs(repo_root)
    if args.dataset not in datasets:
        raise DatasetError(f"unknown dataset {args.dataset}")
    proof = verify_dataset(datasets[args.dataset], full=args.full)
    print(json.dumps(proof, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except DatasetError as exc:
        print(f"dataset verification failed: {exc}", file=sys.stderr)
        raise SystemExit(2)
