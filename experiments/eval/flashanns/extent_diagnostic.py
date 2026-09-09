"""Run and aggregate the bounded T2I extent/locality diagnostic sweep."""

from __future__ import annotations

import argparse
import array
import csv
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any, Sequence


LEVELS = (400, 800, 1600)
SYSTEMS = ("batch-t1", "extent-t1")
REPEATS = (0, 1, 2)
EXPECTED_NQ = 1000


class DiagnosticError(ValueError):
    """Raised when a diagnostic run or trace violates the frozen contract."""


def locality_metrics(
    offsets: Sequence[int], ids: Sequence[int], slot_map: Sequence[int]
) -> dict[str, float | int]:
    if len(offsets) < 2 or offsets[0] != 0 or offsets[-1] != len(ids):
        raise DiagnosticError("candidate offsets do not delimit the ID array")
    unique_pages = contiguous_runs = pages_in_multi = maximum_run = 0
    for begin, end in zip(offsets, offsets[1:]):
        pages = sorted({slot_map[node] // 2 for node in ids[begin:end]})
        if not pages:
            continue
        run_lengths: list[int] = []
        length = 1
        for left, right in zip(pages, pages[1:]):
            if right == left + 1:
                length += 1
            else:
                run_lengths.append(length)
                length = 1
        run_lengths.append(length)
        unique_pages += len(pages)
        contiguous_runs += len(run_lengths)
        pages_in_multi += sum(value for value in run_lengths if value > 1)
        maximum_run = max(maximum_run, max(run_lengths))
    if not unique_pages:
        raise DiagnosticError("trace contains no physical vector pages")
    return {
        "queries": len(offsets) - 1,
        "unique_pages": unique_pages,
        "contiguous_runs": contiguous_runs,
        "pages_in_multipage_runs_pct": 100.0 * pages_in_multi / unique_pages,
        "ideal_command_reduction_pct": 100.0 * (1.0 - contiguous_runs / unique_pages),
        "maximum_contiguous_run_pages": maximum_run,
    }


def _require_number(record: dict[str, Any], section: str, key: str) -> float:
    value = record.get(section, {}).get(key)
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise DiagnosticError(f"{record.get('run_id')}: missing {section}.{key}")
    return float(value)


def validate_pairs(
    records: list[dict[str, Any]], *, expected_binary: str
) -> dict[tuple[int, int], dict[str, dict[str, Any]]]:
    groups: dict[tuple[int, int], dict[str, dict[str, Any]]] = defaultdict(dict)
    seen: set[str] = set()
    for record in records:
        run_id = str(record.get("run_id", ""))
        if not run_id or run_id in seen:
            raise DiagnosticError(f"missing or duplicate run_id {run_id}")
        seen.add(run_id)
        level, repeat, system = record.get("L"), record.get("repeat"), record.get("system")
        if level not in LEVELS or repeat not in REPEATS or system not in SYSTEMS:
            raise DiagnosticError(f"{run_id}: outside diagnostic matrix")
        if record.get("dataset") != "t2i10m" or record.get("phase") != "extent_diagnostic":
            raise DiagnosticError(f"{run_id}: wrong dataset or phase")
        if record.get("binary_sha256") != expected_binary:
            raise DiagnosticError(f"{run_id}: binary hash mismatch")
        if record.get("state") != "cold" or record.get("threads") != 1:
            raise DiagnosticError(f"{run_id}: require cold T=1")
        if record.get("nq") != EXPECTED_NQ:
            raise DiagnosticError(f"{run_id}: require {EXPECTED_NQ} queries")
        if record.get("validation", {}).get("returncode") != 0:
            raise DiagnosticError(f"{run_id}: nonzero return code")
        before = record.get("preflight_before", {})
        if before.get("cache_used") != 0 or before.get("dirty_bytes") != 0 or before.get("io_errors") != 0:
            raise DiagnosticError(f"{run_id}: invalid cold preflight")
        metrics = record.get("metrics", {})
        if metrics.get("completed_queries") != EXPECTED_NQ:
            raise DiagnosticError(f"{run_id}: incomplete run")
        if metrics.get("score_flash", 0) != 0 or metrics.get("score_bounce", 0) != 0:
            raise DiagnosticError(f"{run_id}: score-source contract failed")
        for key in ("nand_read_commands", "nand_read_bytes", "throughput_QPS", "recall@10"):
            _require_number(record, "metrics", key)
        if system in groups[(level, repeat)]:
            raise DiagnosticError(f"duplicate pair member L={level} repeat={repeat} {system}")
        groups[(level, repeat)][system] = record
    expected = {(level, repeat) for level in LEVELS for repeat in REPEATS}
    if set(groups) != expected:
        raise DiagnosticError("incomplete L/repeat matrix")
    for key, pair in groups.items():
        if set(pair) != set(SYSTEMS):
            raise DiagnosticError(f"incomplete system pair {key}")
        hashes = {
            name: (
                record.get("sidecars", {}).get("query_ids_sha256"),
                record.get("sidecars", {}).get("candidate_ids_sha256"),
            )
            for name, record in pair.items()
        }
        if hashes[SYSTEMS[0]][0] != hashes[SYSTEMS[1]][0]:
            raise DiagnosticError(f"{key}: query hash mismatch")
        if hashes[SYSTEMS[0]][1] != hashes[SYSTEMS[1]][1]:
            raise DiagnosticError(f"{key}: candidate hash mismatch")
    return dict(groups)


def classify_limit(ideal_pct: float, actual_pct: float) -> str:
    captured = actual_pct / ideal_pct if ideal_pct > 0 else 0.0
    if ideal_pct <= 15.0 and captured >= 0.5:
        return "locality-limited"
    if ideal_pct >= 20.0 and captured < 0.5:
        return "implementation-limited"
    return "mixed"


def _read_unsigned(path: Path, typecode: str) -> array.array[int]:
    values: array.array[int] = array.array(typecode)
    values.frombytes(Path(path).read_bytes())
    if sys.byteorder != "little":
        values.byteswap()
    return values


def run_one(
    root: Path, *, level: int, system: str, repeat: int, tag: str,
    identity_path: Path, volatile_path: Path, out_root: Path,
) -> dict[str, Any]:
    from experiments.eval.flashanns.config import load_configs
    from experiments.eval.flashanns.layout import LAYOUT_ARTIFACT, select_dataset_layout
    from experiments.eval.flashanns.run_matrix import _internal_command
    from experiments.eval.flashanns.run_one import run_spec

    if level not in LEVELS or system not in SYSTEMS or repeat not in REPEATS:
        raise DiagnosticError("requested run is outside the frozen matrix")
    datasets, systems, matrix = load_configs(root)
    dataset = select_dataset_layout(datasets["t2i10m"], "extent")
    run_id = f"t2i10m-extent_diagnostic-L{level}-T1-r{repeat}-cold-{system}-{tag}"
    run_dir = Path(out_root) / run_id
    spec: dict[str, Any] = {
        "run_id": run_id, "dataset": "t2i10m", "metric": dataset["metric"],
        "phase": "extent_diagnostic", "system": system, "state": "cold",
        "L": level, "k": matrix["k"], "nq": EXPECTED_NQ, "repeat": repeat,
        "threads": 1, "cache_limit": matrix["cache_limit"],
        "run_dir": str(run_dir), "external": False, "layout": "extent",
        "staged_artifact": LAYOUT_ARTIFACT["extent"], "iters": level,
        "campaign_tag": tag,
    }
    spec["command"] = _internal_command(root, dataset, systems[system], spec)
    identity = json.loads(Path(identity_path).read_text())
    volatile = json.loads(Path(volatile_path).read_text())
    return run_spec(root, spec, identity, volatile)


def aggregate(
    raw_root: Path, *, expected_binary: str, slot_map_path: Path,
    out_dir: Path,
) -> list[dict[str, Any]]:
    records = [json.loads(path.read_text()) for path in sorted(Path(raw_root).glob("*/run.json"))]
    pairs = validate_pairs(records, expected_binary=expected_binary)
    slot_map = _read_unsigned(slot_map_path, "I")
    rows: list[dict[str, Any]] = []
    for level in LEVELS:
        ideal: list[float] = []
        page_fraction: list[float] = []
        max_runs: list[float] = []
        actual: list[float] = []
        command_values: dict[str, list[float]] = defaultdict(list)
        byte_values: dict[str, list[float]] = defaultdict(list)
        kib_values: dict[str, list[float]] = defaultdict(list)
        qps_values: dict[str, list[float]] = defaultdict(list)
        recall_values: dict[str, list[float]] = defaultdict(list)
        extra_values: list[float] = []
        run_ids: list[str] = []
        for repeat in REPEATS:
            pair = pairs[(level, repeat)]
            base = pair["batch-t1"]
            trace = Path(base["run_dir"]) / "trace" if base.get("run_dir") else Path(raw_root) / base["run_id"] / "trace"
            offsets = _read_unsigned(trace / "candidate_offsets.u64", "Q")
            ids = _read_unsigned(trace / "candidate_ids.u32", "I")
            loc = locality_metrics(offsets, ids, slot_map)
            ideal.append(float(loc["ideal_command_reduction_pct"]))
            page_fraction.append(float(loc["pages_in_multipage_runs_pct"]))
            max_runs.append(float(loc["maximum_contiguous_run_pages"]))
            per_pair_commands: dict[str, float] = {}
            for system, record in pair.items():
                nq = float(record["nq"])
                commands = _require_number(record, "metrics", "nand_read_commands") / nq
                nbytes = _require_number(record, "metrics", "nand_read_bytes") / nq
                per_pair_commands[system] = commands
                command_values[system].append(commands)
                byte_values[system].append(nbytes / 1_048_576.0)
                kib_values[system].append(nbytes / commands / 1024.0)
                qps_values[system].append(_require_number(record, "metrics", "throughput_QPS"))
                recall_values[system].append(_require_number(record, "metrics", "recall@10"))
                run_ids.append(record["run_id"])
            actual.append(100.0 * (1.0 - per_pair_commands["extent-t1"] / per_pair_commands["batch-t1"]))
            extra_values.append(
                float(pair["extent-t1"]["metrics"].get("extent_extra_pages", 0))
                / EXPECTED_NQ
            )
        ideal_median = statistics.median(ideal)
        actual_median = statistics.median(actual)
        row: dict[str, Any] = {
            "L": level,
            "ideal_command_reduction_pct_median": ideal_median,
            "actual_command_reduction_pct_median": actual_median,
            "captured_ideal_pct": 100.0 * actual_median / ideal_median if ideal_median else 0.0,
            "pages_in_multipage_runs_pct_median": statistics.median(page_fraction),
            "maximum_contiguous_run_pages_median": statistics.median(max_runs),
            "extent_extra_pages_per_query_median": statistics.median(extra_values),
            "classification": classify_limit(ideal_median, actual_median),
            "run_ids": ";".join(run_ids),
        }
        for system in SYSTEMS:
            prefix = "off" if system == "batch-t1" else "on"
            row[f"{prefix}_commands_per_query_median"] = statistics.median(command_values[system])
            row[f"{prefix}_nand_mib_per_query_median"] = statistics.median(byte_values[system])
            row[f"{prefix}_kib_per_read_median"] = statistics.median(kib_values[system])
            row[f"{prefix}_qps_median"] = statistics.median(qps_values[system])
            row[f"{prefix}_recall_median"] = statistics.median(recall_values[system])
        rows.append(row)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    fields = sorted({key for row in rows for key in row})
    with (out_dir / "validated.csv").open("w", newline="") as dst:
        writer = csv.DictWriter(dst, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    classifications = [row["classification"] for row in rows]
    overall = classifications[0] if len(set(classifications)) == 1 else "mixed"
    diagnosis = {
        "status": "diagnostic_validated", "binary_sha256": expected_binary,
        "run_count": len(records), "matrix": {"L": list(LEVELS), "T": 1,
        "systems": list(SYSTEMS), "repeats": len(REPEATS), "nq": EXPECTED_NQ},
        "overall_classification": overall, "rows": rows,
    }
    (out_dir / "diagnosis.json").write_text(json.dumps(diagnosis, indent=2) + "\n")
    (out_dir / "provenance.json").write_text(json.dumps({
        "run_ids": sorted(record["run_id"] for record in records),
        "slot_map": str(slot_map_path), "binary_sha256": expected_binary,
    }, indent=2) + "\n")
    return rows


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    one = sub.add_parser("run-one")
    one.add_argument("--root", type=Path, required=True)
    one.add_argument("--L", type=int, required=True)
    one.add_argument("--system", choices=SYSTEMS, required=True)
    one.add_argument("--repeat", type=int, required=True)
    one.add_argument("--tag", required=True)
    one.add_argument("--identity", type=Path, required=True)
    one.add_argument("--volatile", type=Path, required=True)
    one.add_argument("--out-root", type=Path, required=True)
    agg = sub.add_parser("aggregate")
    agg.add_argument("--raw-root", type=Path, required=True)
    agg.add_argument("--binary-sha256", required=True)
    agg.add_argument("--slot-map", type=Path, required=True)
    agg.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    if args.action == "run-one":
        run_one(args.root, level=args.L, system=args.system, repeat=args.repeat,
                tag=args.tag, identity_path=args.identity,
                volatile_path=args.volatile, out_root=args.out_root)
    else:
        aggregate(args.raw_root, expected_binary=args.binary_sha256,
                  slot_map_path=args.slot_map, out_dir=args.out_dir)


if __name__ == "__main__":
    main()
