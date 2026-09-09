"""Construct and run controlled C2 page-admission replays."""

from __future__ import annotations

from array import array
import argparse
import json
import mmap
from pathlib import Path
import subprocess
import sys
from typing import Iterable, Mapping, Sequence

from .observe import block_delta, read_block_snapshot, sha256_file


def require_backings(values: Sequence[str] | None) -> tuple[str, str]:
    if values is None or len(values) != 2:
        raise ValueError("live replay requires two explicit backing devices")
    if values[0] == values[1]:
        raise ValueError("backing devices must be distinct")
    return values[0], values[1]


def _ordered_unique(values: Iterable[int]) -> list[int]:
    seen: set[int] = set()
    result: list[int] = []
    for value in values:
        if value not in seen:
            seen.add(value)
            result.append(value)
    return result


def derive_useful_pages(
    candidate_ids: Iterable[int],
    slot_map: Sequence[int],
    vector_base: int,
    vector_stride: int,
    vector_bytes: int,
    image_bytes: int,
    page_bytes: int = 4096,
) -> list[int]:
    pages: list[int] = []
    for candidate_id in candidate_ids:
        if candidate_id < 0 or candidate_id >= len(slot_map):
            raise ValueError(f"candidate ID out of range: {candidate_id}")
        begin = vector_base + int(slot_map[candidate_id]) * vector_stride
        end = begin + vector_bytes
        if begin < 0 or end > image_bytes:
            raise ValueError(f"vector page outside image bounds: {candidate_id}")
        first = begin // page_bytes * page_bytes
        last = (end - 1) // page_bytes * page_bytes
        pages.extend(range(first, last + page_bytes, page_bytes))
    return _ordered_unique(pages)


def blind_16k_pages(
    useful_pages: Iterable[int], image_bytes: int, page_bytes: int = 4096
) -> list[int]:
    admission_bytes = 4 * page_bytes
    expanded: list[int] = []
    for page in useful_pages:
        group = page // admission_bytes * admission_bytes
        expanded.extend(
            offset for offset in range(group, group + admission_bytes, page_bytes)
            if offset < image_bytes
        )
    return _ordered_unique(expanded)


def two_hop_ids(roots: Iterable[int], graph: Mapping[int, Sequence[int]]) -> list[int]:
    roots_list = _ordered_unique(roots)
    first = _ordered_unique(neighbor for root in roots_list for neighbor in graph[root])
    second = _ordered_unique(neighbor for node in first for neighbor in graph[node])
    return _ordered_unique([*roots_list, *first, *second])


def build_policy_pages(
    policy: str,
    *,
    candidates: Sequence[int],
    results: Sequence[int],
    graph: Mapping[int, Sequence[int]],
    slot_map: Sequence[int],
    vector_base: int,
    vector_stride: int,
    vector_bytes: int,
    image_bytes: int,
    page_bytes: int = 4096,
) -> list[int]:
    useful = derive_useful_pages(
        candidates, slot_map, vector_base, vector_stride, vector_bytes, image_bytes, page_bytes
    )
    if policy in ("demand", "selective_4k"):
        return useful
    if policy == "blind_16k":
        return blind_16k_pages(useful, image_bytes, page_bytes)
    if policy.startswith("top") and policy[3:].isdigit():
        width = int(policy[3:])
        if width not in (1, 2, 8):
            raise ValueError(f"undeclared top-N policy: {policy}")
        # Deliberately optimistic for blind coverage: seed it with final top-k
        # results. If this lower-bound policy still wastes pages, an online
        # pre-commit predictor cannot claim a stronger information advantage.
        expanded_ids = two_hop_ids(results[:width], graph)
        expanded_pages = derive_useful_pages(
            expanded_ids, slot_map, vector_base, vector_stride, vector_bytes,
            image_bytes, page_bytes,
        )
        return _ordered_unique([*expanded_pages, *useful])
    raise ValueError(f"unknown replay policy: {policy}")


def page_metrics(useful_pages: Iterable[int], fetched_pages: Iterable[int]) -> dict[str, float]:
    useful = set(useful_pages)
    fetched = set(fetched_pages)
    if not useful or not fetched:
        raise ValueError("page metrics require non-empty sets")
    used = len(useful & fetched)
    return {
        "useful_page_pct": 100.0 * used / len(fetched),
        "physical_amplification": len(fetched) / len(useful),
    }


class GraphView(Mapping[int, Sequence[int]]):
    def __init__(self, path: Path, node_count: int, degree: int):
        self.node_count = node_count
        self.degree = degree
        self._stream = path.open("rb")
        expected = node_count * degree * 4
        if path.stat().st_size != expected:
            self._stream.close()
            raise ValueError("graph size differs from n * R * 4")
        self._map = mmap.mmap(self._stream.fileno(), 0, access=mmap.ACCESS_READ)

    def __getitem__(self, node: int) -> Sequence[int]:
        if node < 0 or node >= self.node_count:
            raise KeyError(node)
        begin = node * self.degree * 4
        values = array("I")
        values.frombytes(self._map[begin:begin + self.degree * 4])
        if sys.byteorder != "little":
            values.byteswap()
        return values

    def __iter__(self):
        return iter(range(self.node_count))

    def __len__(self):
        return self.node_count

    def close(self) -> None:
        self._map.close()
        self._stream.close()


def _read_array(path: Path, typecode: str) -> array:
    values = array(typecode)
    with path.open("rb") as stream:
        values.fromfile(stream, path.stat().st_size // values.itemsize)
    if sys.byteorder != "little":
        values.byteswap()
    return values


def _write_u64(path: Path, values: Iterable[int]) -> None:
    payload = array("Q", values)
    if sys.byteorder != "little":
        payload.byteswap()
    path.write_bytes(payload.tobytes())


def materialize_policy(
    trace: Path,
    output: Path,
    policy: str,
    graph_path: Path,
    slot_map_path: Path,
    *,
    node_count: int,
    degree: int,
    vector_base: int,
    vector_stride: int,
    vector_bytes: int,
    image_bytes: int,
) -> dict[str, object]:
    candidate_offsets = _read_array(trace / "candidate_offsets.u64", "Q")
    candidate_ids = _read_array(trace / "candidate_ids.u32", "I")
    result_ids = _read_array(trace / "result_ids.u32", "I")
    slot_map = _read_array(slot_map_path, "I")
    nq = len(candidate_offsets) - 1
    if len(result_ids) != nq * 10 or len(slot_map) != node_count:
        raise ValueError("trace or slot-map dimensions changed")
    graph = GraphView(graph_path, node_count, degree)
    policy_offsets = [0]
    useful_offsets = [0]
    policy_pages: list[int] = []
    useful_pages_flat: list[int] = []
    try:
        for query in range(nq):
            left, right = candidate_offsets[query], candidate_offsets[query + 1]
            candidates = candidate_ids[left:right]
            results = result_ids[query * 10:(query + 1) * 10]
            useful = derive_useful_pages(
                candidates, slot_map, vector_base, vector_stride, vector_bytes, image_bytes
            )
            pages = build_policy_pages(
                policy, candidates=candidates, results=results, graph=graph, slot_map=slot_map,
                vector_base=vector_base, vector_stride=vector_stride,
                vector_bytes=vector_bytes, image_bytes=image_bytes,
            )
            useful_pages_flat.extend(useful)
            policy_pages.extend(pages)
            useful_offsets.append(len(useful_pages_flat))
            policy_offsets.append(len(policy_pages))
    finally:
        graph.close()
    output.mkdir(parents=True, exist_ok=False)
    _write_u64(output / "query_offsets.u64", policy_offsets)
    _write_u64(output / "pages.u64", policy_pages)
    _write_u64(output / "useful_offsets.u64", useful_offsets)
    _write_u64(output / "useful_pages.u64", useful_pages_flat)
    if not useful_pages_flat or not policy_pages:
        raise ValueError("materialized policy contains no pages")
    overall = {
        "useful_page_pct": 100.0 * len(useful_pages_flat) / len(policy_pages),
        "physical_amplification": len(policy_pages) / len(useful_pages_flat),
    }
    manifest = {
        "policy": policy,
        "nq": nq,
        "logical_pages": len(policy_pages),
        "useful_pages": len(useful_pages_flat),
        **overall,
        "sidecars": {
            name: sha256_file(output / name)
            for name in ("query_offsets.u64", "pages.u64", "useful_offsets.u64", "useful_pages.u64")
        },
        "root_information": "final_topk_optimistic_lower_bound" if policy.startswith("top") else "none",
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--policy", choices=("selective_4k", "blind_16k", "demand", "top1", "top2", "top8"), required=True)
    parser.add_argument("--graph", type=Path, required=True)
    parser.add_argument("--slot-map", type=Path, required=True)
    parser.add_argument("--node-count", type=int, default=10_000_000)
    parser.add_argument("--degree", type=int, default=32)
    parser.add_argument("--vector-base", type=int, default=4096)
    parser.add_argument("--vector-stride", type=int, default=4096)
    parser.add_argument("--vector-bytes", type=int, default=2048)
    parser.add_argument("--image-bytes", type=int, required=True)
    parser.add_argument("--device", type=Path)
    parser.add_argument("--backings", nargs=2)
    parser.add_argument("--binary", type=Path, default=Path("tools/motivation-replay-probe"))
    parser.add_argument("--image-offset", type=int, default=0)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    manifest = materialize_policy(
        args.trace, args.output, args.policy, args.graph, args.slot_map,
        node_count=args.node_count, degree=args.degree, vector_base=args.vector_base,
        vector_stride=args.vector_stride, vector_bytes=args.vector_bytes,
        image_bytes=args.image_bytes,
    )
    if args.device:
        backings = require_backings(args.backings) if not args.dry_run else None
        command = [
            str(args.binary), "--device", str(args.device), "--image-offset", str(args.image_offset),
            "--query-offsets", str(args.output / "query_offsets.u64"),
            "--pages", str(args.output / "pages.u64"), "--output", str(args.output / "timing.json"),
        ]
        if args.dry_run:
            command.append("--dry-run")
        before = read_block_snapshot(backings) if backings else None
        subprocess.run(command, check=True)
        after = read_block_snapshot(backings) if backings else None
        manifest["block_delta"] = block_delta(before, after) if before and after else None
        (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(json.dumps(manifest, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
