"""Deterministically expand and optionally execute the frozen evaluation matrix."""

from __future__ import annotations

import argparse
import hashlib
import json
import random
import re
import shlex
import uuid
from pathlib import Path
from typing import Any

from experiments.eval.flashanns.config import REMOVED_FLAGS, load_configs


class RunnerError(ValueError):
    pass


def _anchor_l(anchors: dict[str, Any] | None, system: str) -> int:
    if not anchors:
        raise RunnerError("phase requires frozen recall anchors")
    selected = anchors.get("primary", anchors)
    anchor_system = {
        "serial-t1": "flashanns",
        "batch-t1": "flashanns",
        "extent-t1": "flashanns",
        "wise-only": "flashanns",
    }.get(system, system)
    value = selected.get(anchor_system, selected.get("L"))
    if isinstance(value, dict):
        value = value.get("L")
    if not isinstance(value, int) or value <= 0:
        raise RunnerError(f"missing L anchor for {system}")
    return value


def _internal_command(root: Path, dataset: dict[str, Any], system: dict[str, Any], spec: dict[str, Any]) -> list[str]:
    a, staging = dataset["artifacts"], dataset["staging"]
    command = [
        str(root / "serving" / "search_beam"),
        "--vmem-dev", "/dev/vmem0",
        "--vmem-offset", str(staging["offset"]),
        "--vmem-len", str(staging["length"]),
        "--id-slot-map", a["slot_map"],
    ]
    command += [
        "--diskann-layout", "--pq-nav", "--pq-pivots", a["pq64_pivots"],
        "--pq-compressed", a["pq64_codes"], "--metric", dataset["metric"],
        "--nav-graph", a["nav_graph"], "--graph-file", a["graph"], "--entry", a["entry"],
        "--queries", a["query_subset"], "--gt", a["ground_truth"], "--id-map", a["id_map"],
        "--beam", str(spec["L"]), "--k", str(spec["k"]), "--iters", str(spec["iters"]), "--max-q", str(spec["nq"]),
        "--shuffle-seed", "42", "--threads", str(spec.get("threads", system["threads"])), "--cpu-affinity", "--policy", "P3",
        "--dram-numa", "0",
        "--no-hide-warm-entry", "--no-direct-install", "--no-score-cache", "--no-stripe-fill",
        "--expand-batch", "8", "--issue-ahead", "1", "--eval-trace-dir", str(Path(spec["run_dir"]) / "trace"),
    ]
    command += list(system["flags"])
    if "arrival_rate" in spec:
        command += ["--arrival-rate", str(spec["arrival_rate"])]
    if system.get("per_thread_window"):
        command += ["--dram-bytes", str(system["per_thread_window"]), "--host-bytes", str(system["per_thread_window"])]
    return command


def _pipeann_command(root: Path, dataset_id: str, dataset: dict[str, Any], spec: dict[str, Any]) -> list[str]:
    if "arrival_rate" not in spec:
        query_file = dataset["artifacts"]["query_subset"]
        gt_file = dataset["artifacts"]["ground_truth"]
        if int(spec["nq"]) < 10000:
            query_file = str(Path(spec["run_dir"]) / "pipeann-query.fbin")
            gt_file = str(Path(spec["run_dir"]) / "pipeann-gt.ibin")
        return [
            str(root / "tools" / "eval-bin" / "search_disk_index"),
            "float", dataset["pipeann_index_prefix"], "8", "8",
            query_file, gt_file,
            str(spec["k"]), dataset["metric"], "pq", "2", "0", str(spec["L"]),
        ]
    binary = str(root / "tools" / "pipeann_open_loop")
    command = [
        binary, "--data_type", "float",
        "--dist_fn", dataset["metric"], "--index_path_prefix", dataset["pipeann_index_prefix"],
        "--result_path", str(Path(spec["run_dir"]) / "pipeann-result"), "--query_file", dataset["artifacts"]["query_subset"],
        "--gt_file", dataset["artifacts"]["ground_truth"], "-K", str(spec["k"]), "-L", str(spec["L"]),
        "-W", "8", "-T", "8",
    ]
    if "arrival_rate" in spec:
        command += ["--arrival-rate", str(spec["arrival_rate"])]
        command += ["--trace-dir", str(Path(spec["run_dir"]) / "trace")]
        command += ["--shuffle-seed", "42"]
    return command


def expand_runs(
    root: Path,
    dataset_id: str,
    phase: str,
    anchors: dict[str, Any] | None = None,
    out: Path | None = None,
    system_id: str | None = None,
    level: int | None = None,
    repeat_id: int | None = None,
    threads_value: int | None = None,
    cache_gib_value: int | None = None,
    state_value: str | None = None,
    arrival_rate_value: float | None = None,
    run_tag: str | None = None,
) -> list[dict[str, Any]]:
    root = Path(root)
    datasets, systems, matrix = load_configs(root)
    dataset = datasets[dataset_id]
    if run_tag is not None and not re.fullmatch(r"[a-z0-9][a-z0-9_-]*", run_tag):
        raise RunnerError("run tag must use lowercase letters, digits, underscore, or hyphen")
    if not dataset.get("ready"):
        raise RunnerError(f"{dataset_id}: dataset is not admitted")
    out = Path(out or root / "results" / "eval" / "flashanns" / "raw" / dataset_id / phase)
    if phase == "smoke":
        phase_cfg, system_ids, levels, states = matrix["smoke"], [s for s in matrix["q2"]["systems"] if systems[s]["kind"] == "internal"], [400], ["proof"]
    elif phase == "calibration":
        phase_cfg, system_ids, levels, states = matrix["calibration"], matrix["q2"]["systems"], matrix["base_L"], ["cold"]
    elif phase in ("q2", "q3_t1", "q3_t8", "q3_load", "q4_hide", "q4_cold_warm", "q4_cache"):
        phase_cfg, system_ids, states = matrix[phase], matrix[phase]["systems"], matrix[phase].get("states", ["cold"])
        levels = matrix["base_L"] if phase == "q2" else sorted({_anchor_l(anchors, system) for system in system_ids})
    else:
        raise RunnerError(f"unknown phase {phase}")
    if system_id is not None:
        if system_id not in system_ids:
            raise RunnerError(f"{system_id}: system is not part of {phase}")
        system_ids = [system_id]
    if level is not None:
        declared_levels = list(levels)
        if phase in ("calibration", "q2"):
            declared_levels += matrix["extended_L"]
        if level not in declared_levels:
            raise RunnerError(f"{level}: not a declared {phase} L")
        levels = [level]
    runs: list[dict[str, Any]] = []
    thread_values = phase_cfg.get("threads", [None])
    cache_values = phase_cfg.get("cache_gib", [None])
    arrival_values: list[float | None] = [None]
    if phase == "q3_load":
        raw_rates = anchors.get("arrival_rates") if anchors else None
        if not isinstance(raw_rates, list) or not raw_rates or any(
            not isinstance(rate, (int, float)) or rate <= 0 for rate in raw_rates
        ):
            raise RunnerError("q3_load requires positive frozen arrival_rates")
        arrival_values = sorted({float(rate) for rate in raw_rates})
        if arrival_rate_value is not None:
            if float(arrival_rate_value) not in arrival_values:
                raise RunnerError(
                    f"arrival rate {arrival_rate_value} is not declared for q3_load"
                )
            arrival_values = [float(arrival_rate_value)]
    elif arrival_rate_value is not None:
        raise RunnerError("--arrival-rate is only valid for q3_load")
    if repeat_id is not None and repeat_id not in range(int(phase_cfg["repeats"])):
        raise RunnerError(f"repeat {repeat_id} is not declared for {phase}")
    if threads_value is not None:
        if threads_value not in thread_values:
            raise RunnerError(f"threads {threads_value} is not declared for {phase}")
        thread_values = [threads_value]
    if cache_gib_value is not None:
        if cache_gib_value not in cache_values:
            raise RunnerError(f"cache {cache_gib_value} GiB is not declared for {phase}")
        cache_values = [cache_gib_value]
    if state_value is not None:
        if state_value not in states:
            raise RunnerError(f"state {state_value} is not declared for {phase}")
        states = [state_value]
    for level in levels:
        for repeat in range(int(phase_cfg["repeats"])):
            if repeat_id is not None and repeat != repeat_id:
                continue
            ordered = list(system_ids)
            seed_text = f"{matrix['seed']}:{dataset_id}:{phase}:{level}:{repeat}"
            random.Random(int(hashlib.sha256(seed_text.encode()).hexdigest()[:16], 16)).shuffle(ordered)
            for state in states:
              for threads in thread_values:
               for cache_gib in cache_values:
                for arrival_rate in arrival_values:
                 for system_id in ordered:
                    if phase not in ("smoke", "calibration", "q2") and _anchor_l(anchors, system_id) != level:
                        continue
                    rate_suffix = ""
                    if arrival_rate is not None:
                        rate_suffix = f"-R{arrival_rate:g}".replace(".", "p")
                    suffix = (f"-T{threads}" if threads is not None else "") + (f"-C{cache_gib}G" if cache_gib is not None else "") + rate_suffix
                    tag_suffix = f"-{run_tag}" if run_tag else ""
                    run_id = f"{dataset_id}-{phase}-L{level}-r{repeat}-{state}-{system_id}{suffix}{tag_suffix}"
                    spec: dict[str, Any] = {
                        "run_id": run_id, "dataset": dataset_id, "metric": dataset["metric"], "phase": phase,
                        "system": system_id, "state": state, "L": level, "k": matrix["k"], "nq": phase_cfg["nq"],
                        "repeat": repeat, "cache_limit": matrix["cache_limit"], "run_dir": str(out / run_id),
                        "external": systems[system_id]["kind"] == "external-pipeann",
                    }
                    if threads is not None:
                        spec["threads"] = threads
                    if cache_gib is not None:
                        spec["cache_gib"] = cache_gib
                        spec["required_cache_limit"] = cache_gib * 1024**3
                    if arrival_rate is not None:
                        spec["arrival_rate"] = arrival_rate
                    if run_tag is not None:
                        spec["campaign_tag"] = run_tag
                    if state == "warm":
                        spec["cold_parent_run_id"] = f"{dataset_id}-{phase}-L{level}-r{repeat}-cold-{system_id}{suffix}{tag_suffix}"
                    spec["iters"] = level if matrix["internal_iters"] == "L" else None
                    spec["command"] = _pipeann_command(root, dataset_id, dataset, spec) if spec["external"] else _internal_command(root, dataset, systems[system_id], spec)
                    bad = set(spec["command"]) & REMOVED_FLAGS
                    if bad:
                        raise RunnerError(f"removed flags in {run_id}: {sorted(bad)}")
                    runs.append(spec)
    return runs


def validate_live_invocation(
    runs: list[dict[str, Any]],
    identity_evidence: dict[str, Any] | None,
    volatile_evidence: dict[str, Any] | None,
) -> None:
    internal = [run for run in runs if not run["external"]]
    if internal and len(internal) != 1:
        raise RunnerError(
            "live invocation must contain exactly one internal run; select --system and --L"
        )
    if internal and not (identity_evidence and identity_evidence.get("accepted")):
        raise RunnerError(
            "live internal invocation requires accepted identity evidence"
        )
    needs_cold = bool(internal and internal[0]["state"] in ("cold", "proof"))
    if needs_cold and not (volatile_evidence and volatile_evidence.get("accepted")):
        raise RunnerError("live cold invocation requires accepted volatile evidence")
    if internal and internal[0]["state"] == "warm" and not identity_evidence.get("cold_parent_accepted"):
        raise RunnerError("live warm invocation requires accepted cold-parent evidence")
    if internal and internal[0]["state"] == "warm" and identity_evidence.get("cold_parent_run_id") != internal[0].get("cold_parent_run_id"):
        raise RunnerError("live warm invocation cold-parent run ID mismatch")
    if needs_cold:
        try:
            uuid.UUID(str(volatile_evidence.get("capture_id")))
        except (AttributeError, TypeError, ValueError):
            raise RunnerError(
                "live internal invocation requires a valid volatile capture_id"
            ) from None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--phase", required=True)
    parser.add_argument("--anchors", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--system")
    parser.add_argument("--L", dest="level", type=int)
    parser.add_argument("--repeat", dest="repeat_id", type=int)
    parser.add_argument("--threads", dest="threads_value", type=int)
    parser.add_argument("--cache-gib", dest="cache_gib_value", type=int)
    parser.add_argument("--state", dest="state_value")
    parser.add_argument("--arrival-rate", dest="arrival_rate_value", type=float)
    parser.add_argument("--run-tag")
    parser.add_argument("--identity-evidence", type=Path)
    parser.add_argument("--volatile-evidence", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    anchors = json.loads(args.anchors.read_text()) if args.anchors else None
    runs = expand_runs(
        root, args.dataset, args.phase, anchors, args.out, args.system, args.level,
        args.repeat_id, args.threads_value, args.cache_gib_value, args.state_value,
        args.arrival_rate_value,
        args.run_tag,
    )
    if args.dry_run:
        for spec in runs:
            print(spec["run_id"] + "\t" + shlex.join(spec["command"]))
        return 0
    from experiments.eval.flashanns.run_one import run_spec
    identity = json.loads(args.identity_evidence.read_text()) if args.identity_evidence else None
    volatile = json.loads(args.volatile_evidence.read_text()) if args.volatile_evidence else None
    validate_live_invocation(runs, identity, volatile)
    for spec in runs:
        run_spec(root, spec, identity, volatile)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
