"""Deterministically expand and optionally execute the frozen evaluation matrix."""

from __future__ import annotations

import argparse
import hashlib
import json
import random
import shlex
from pathlib import Path
from typing import Any

from experiments.eval.flashanns.config import REMOVED_FLAGS, load_configs


class RunnerError(ValueError):
    pass


def _anchor_l(anchors: dict[str, Any] | None, system: str) -> int:
    if not anchors:
        raise RunnerError("phase requires frozen recall anchors")
    value = anchors.get(system, anchors.get("L"))
    if isinstance(value, dict):
        value = value.get("L")
    if not isinstance(value, int) or value <= 0:
        raise RunnerError(f"missing L anchor for {system}")
    return value


def _internal_command(root: Path, dataset: dict[str, Any], system: dict[str, Any], spec: dict[str, Any]) -> list[str]:
    a, staging = dataset["artifacts"], dataset["staging"]
    command = [str(root / "serving" / "search_beam")]
    if "--oracle-dram" in system["flags"]:
        command += ["--image", a["oracle_image"]]
    else:
        command += ["--vmem-dev", "/dev/vmem0", "--vmem-offset", str(staging["offset"]), "--vmem-len", str(staging["length"]), "--id-slot-map", a["slot_map"]]
    command += [
        "--diskann-layout", "--pq-nav", "--pq-pivots", a["pq64_pivots"],
        "--pq-compressed", a["pq64_codes"], "--metric", dataset["metric"],
        "--nav-graph", a["nav_graph"], "--graph-file", a["graph"], "--entry", a["entry"],
        "--queries", a["query_subset"], "--gt", a["ground_truth"], "--id-map", a["id_map"],
        "--beam", str(spec["L"]), "--k", str(spec["k"]), "--iters", "0", "--max-q", str(spec["nq"]),
        "--shuffle-seed", "42", "--threads", str(system["threads"]), "--cpu-affinity", "--policy", "P3",
        "--no-hide-warm-entry", "--no-direct-install", "--no-score-cache", "--no-stripe-fill",
        "--expand-batch", "8", "--issue-ahead", "1", "--eval-trace-dir", str(Path(spec["run_dir"]) / "trace"),
    ]
    command += list(system["flags"])
    if system.get("per_thread_window"):
        command += ["--dram-bytes", str(system["per_thread_window"]), "--host-bytes", str(system["per_thread_window"])]
    return command


def _pipeann_command(dataset_id: str, dataset: dict[str, Any], spec: dict[str, Any]) -> list[str]:
    if dataset_id != "t2i10m":
        raise RunnerError(f"{dataset_id}: external PipeANN index is not admitted")
    base = "/mnt/disk0/chukexin_motivation"
    return [
        f"{base}/DiskANN_cpp/build/apps/search_disk_index", "--data_type", "float",
        "--dist_fn", dataset["metric"], "--index_path_prefix", f"{base}/pipeann_t2i10m/idx_t2i_disk",
        "--result_path", str(Path(spec["run_dir"]) / "pipeann-result"), "--query_file", dataset["artifacts"]["query_subset"],
        "--gt_file", dataset["artifacts"]["ground_truth"], "-K", str(spec["k"]), "-L", str(spec["L"]),
        "-W", "8", "-T", "8",
    ]


def expand_runs(root: Path, dataset_id: str, phase: str, anchors: dict[str, Any] | None = None, out: Path | None = None) -> list[dict[str, Any]]:
    root = Path(root)
    datasets, systems, matrix = load_configs(root)
    dataset = datasets[dataset_id]
    if not dataset.get("ready"):
        raise RunnerError(f"{dataset_id}: dataset is not admitted")
    out = Path(out or root / "results" / "eval" / "flashanns" / "raw" / dataset_id / phase)
    if phase == "smoke":
        phase_cfg, system_ids, levels, states = matrix["smoke"], [s for s in matrix["q2"]["systems"] if systems[s]["kind"] == "internal"], [400], ["proof"]
    elif phase == "calibration":
        phase_cfg, system_ids, levels, states = matrix["calibration"], matrix["q2"]["systems"], matrix["base_L"] + matrix["extended_L"], ["cold"]
    elif phase in ("q2", "q3_t1", "q3_t8", "q4"):
        phase_cfg, system_ids, states = matrix[phase], matrix[phase]["systems"], matrix[phase].get("states", ["cold"])
        levels = sorted({_anchor_l(anchors, system) for system in system_ids})
    else:
        raise RunnerError(f"unknown phase {phase}")
    runs: list[dict[str, Any]] = []
    for level in levels:
        for repeat in range(int(phase_cfg["repeats"])):
            ordered = list(system_ids)
            seed_text = f"{matrix['seed']}:{dataset_id}:{phase}:{level}:{repeat}"
            random.Random(int(hashlib.sha256(seed_text.encode()).hexdigest()[:16], 16)).shuffle(ordered)
            for state in states:
                for system_id in ordered:
                    if phase not in ("smoke", "calibration") and _anchor_l(anchors, system_id) != level:
                        continue
                    run_id = f"{dataset_id}-{phase}-L{level}-r{repeat}-{state}-{system_id}"
                    spec: dict[str, Any] = {
                        "run_id": run_id, "dataset": dataset_id, "metric": dataset["metric"], "phase": phase,
                        "system": system_id, "state": state, "L": level, "k": matrix["k"], "nq": phase_cfg["nq"],
                        "repeat": repeat, "cache_limit": matrix["cache_limit"], "run_dir": str(out / run_id),
                        "external": systems[system_id]["kind"] == "external-pipeann",
                    }
                    spec["command"] = _pipeann_command(dataset_id, dataset, spec) if spec["external"] else _internal_command(root, dataset, systems[system_id], spec)
                    bad = set(spec["command"]) & REMOVED_FLAGS
                    if bad:
                        raise RunnerError(f"removed flags in {run_id}: {sorted(bad)}")
                    runs.append(spec)
    return runs


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--phase", required=True)
    parser.add_argument("--anchors", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    anchors = json.loads(args.anchors.read_text()) if args.anchors else None
    runs = expand_runs(root, args.dataset, args.phase, anchors, args.out)
    if args.dry_run:
        for spec in runs:
            print(spec["run_id"] + "\t" + shlex.join(spec["command"]))
        return 0
    from experiments.eval.flashanns.run_one import run_spec
    for spec in runs:
        run_spec(root, spec)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
