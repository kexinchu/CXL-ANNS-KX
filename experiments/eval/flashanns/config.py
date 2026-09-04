"""Load and fail-closed validate the frozen FlashANNS evaluation matrix."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any


class ConfigError(ValueError):
    """The evaluation configuration violates its frozen contract."""


DATASET_IDS = {"t2i10m", "yfcc10m", "laion10m"}
METRICS = {"mips", "l2"}
REQUIRED_ARTIFACTS = {
    "source_base",
    "source_queries",
    "source_gt",
    "execution_base",
    "oracle_image",
    "extent_image",
    "graph",
    "nav_graph",
    "entry",
    "query_subset",
    "ground_truth",
    "id_map",
    "slot_map",
    "pq64_pivots",
    "pq64_codes",
}
REMOVED_FLAGS = {
    "--early-cl",
    "--lookahead-k",
    "--spec-beam-nbrs",
    "--score-page",
    "--pipe-drive",
    "--admit-gap",
}
Q2_SYSTEMS = ["demand", "pipeann", "oracle", "flashanns"]


def _read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ConfigError(f"cannot load {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ConfigError(f"{path} must contain a JSON object")
    return value


def validate_configs(
    datasets: dict[str, Any], systems: dict[str, Any], matrix: dict[str, Any]
) -> None:
    if set(datasets) != DATASET_IDS:
        raise ConfigError(f"datasets must be exactly {sorted(DATASET_IDS)}")

    for dataset_id, dataset in datasets.items():
        metric = dataset.get("metric")
        if metric not in METRICS:
            raise ConfigError(f"{dataset_id}: metric must be one of {sorted(METRICS)}")
        if dataset.get("count") != 10_000_000:
            raise ConfigError(f"{dataset_id}: count must be 10000000")
        artifacts = dataset.get("artifacts")
        if not isinstance(artifacts, dict):
            raise ConfigError(f"{dataset_id}: artifacts must be an object")
        if dataset.get("ready"):
            missing = sorted(
                key for key in REQUIRED_ARTIFACTS if not isinstance(artifacts.get(key), str) or not artifacts[key]
            )
            if missing:
                raise ConfigError(f"{dataset_id}: ready dataset missing artifacts: {', '.join(missing)}")

    if matrix.get("cache_limit") != 4 * 1024**3:
        raise ConfigError("cache_limit must be exactly 4294967296")
    if matrix.get("seed") != 20260904 or matrix.get("query_seed") != 42:
        raise ConfigError("evaluation and query seeds are frozen")
    if matrix.get("k") != 10:
        raise ConfigError("k must be 10")
    if matrix.get("base_L") != [50, 100, 200, 400, 800, 1600]:
        raise ConfigError("base_L sweep differs from the frozen contract")
    if matrix.get("extended_L") != [2400, 3200]:
        raise ConfigError("extended_L sweep differs from the frozen contract")

    referenced = set()
    for phase in ("q2", "q3_t1", "q3_t8", "q4"):
        phase_cfg = matrix.get(phase)
        if not isinstance(phase_cfg, dict):
            raise ConfigError(f"missing matrix phase {phase}")
        if phase_cfg.get("nq") != 10_000 or phase_cfg.get("repeats") != 5:
            raise ConfigError(f"{phase}: require 10000 queries and 5 repeats")
        phase_systems = phase_cfg.get("systems")
        if not isinstance(phase_systems, list) or not phase_systems:
            raise ConfigError(f"{phase}: systems must be a nonempty list")
        referenced.update(phase_systems)
    if matrix["q2"]["systems"] != Q2_SYSTEMS:
        raise ConfigError(f"q2 systems must be {Q2_SYSTEMS}")
    if matrix["q4"].get("states") != ["cold", "warm"]:
        raise ConfigError("q4 states must be cold then warm")

    missing_systems = sorted(referenced - set(systems))
    if missing_systems:
        raise ConfigError(f"undefined systems: {', '.join(missing_systems)}")
    for system_id, system in systems.items():
        flags = system.get("flags")
        if not isinstance(flags, list) or not all(isinstance(flag, str) for flag in flags):
            raise ConfigError(f"{system_id}: flags must be a string list")
        bad_flags = sorted(set(flags) & REMOVED_FLAGS)
        if bad_flags:
            raise ConfigError(f"{system_id}: removed flag is live: {', '.join(bad_flags)}")

    for system_id in matrix["q2"]["systems"]:
        if systems[system_id].get("threads") != 8:
            raise ConfigError(f"q2 system {system_id} must use 8 threads")

    flashanns = systems.get("flashanns", {})
    expected_flashanns = {
        "threads": 8,
        "per_thread_window": 128 * 1024**2,
        "pipe_depth": 2,
        "issue_qd": 0,
    }
    for key, expected in expected_flashanns.items():
        if flashanns.get(key) != expected:
            raise ConfigError(f"flashanns {key} must be {expected}")
    if systems.get("pipeann", {}).get("kind") != "external-pipeann":
        raise ConfigError("pipeann must use the external-pipeann adapter")


def load_configs(repo_root: Path | str) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
    root = Path(repo_root)
    config_dir = root / "experiments" / "eval" / "flashanns"
    datasets = _read_json(config_dir / "datasets.json")
    systems = _read_json(config_dir / "systems.json")
    matrix = _read_json(config_dir / "matrix.json")
    validate_configs(datasets, systems, matrix)
    return datasets, systems, matrix
