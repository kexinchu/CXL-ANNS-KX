"""Stream-hash and atomically freeze an admitted dataset's artifacts."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path
from typing import Any

from experiments.eval.flashanns.config import load_configs
from experiments.eval.flashanns.verify_dataset import DatasetError, verify_dataset


class ArtifactError(ValueError):
    """An artifact cannot be frozen under the declared contract."""


def hash_file(path: Path, block_bytes: int = 8_388_608) -> dict[str, Any]:
    path = Path(path)
    if block_bytes <= 0:
        raise ArtifactError(f"{path.name}: block_bytes must be positive")
    digest = hashlib.sha256()
    total = 0
    try:
        with path.open("rb") as src:
            while True:
                block = src.read(block_bytes)
                if not block:
                    break
                digest.update(block)
                total += len(block)
    except OSError as exc:
        raise ArtifactError(f"{path.name}: {exc}") from exc
    return {"path": str(path.resolve()), "bytes": total, "sha256": digest.hexdigest()}


def _atomic_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    payload = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode()
    try:
        with tmp.open("wb") as dst:
            dst.write(payload)
            dst.flush()
            os.fsync(dst.fileno())
        os.replace(tmp, path)
        dir_fd = os.open(path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(dir_fd)
        finally:
            os.close(dir_fd)
    except OSError as exc:
        try:
            tmp.unlink()
        except OSError:
            pass
        raise ArtifactError(f"{path.name}: {exc}") from exc


def freeze_dataset(dataset: dict[str, Any], output: Path) -> dict[str, Any]:
    if not dataset.get("ready"):
        raise ArtifactError(f"{dataset.get('id', 'dataset')}: dataset is not ready")
    artifacts = dataset.get("artifacts")
    if not isinstance(artifacts, dict) or not artifacts:
        raise ArtifactError(f"{dataset.get('id', 'dataset')}: no artifacts")
    expected_sizes = dataset.get("expected_sizes", {})
    frozen: dict[str, Any] = {}
    for name in sorted(artifacts):
        value = artifacts[name]
        if not isinstance(value, str) or not value:
            raise ArtifactError(f"{name}: path is missing")
        path = Path(value)
        if not path.is_absolute():
            raise ArtifactError(f"{name}: path must be absolute")
        record = hash_file(path)
        if name in expected_sizes and record["bytes"] != expected_sizes[name]:
            raise ArtifactError(
                f"{name}: size {record['bytes']} != expected {expected_sizes[name]}"
            )
        frozen[name] = record
    manifest = {
        "dataset": dataset.get("id"),
        "metric": dataset.get("metric"),
        "count": dataset.get("count"),
        "dimension": dataset.get("dimension"),
        "artifacts": frozen,
    }
    _atomic_json(Path(output), manifest)
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    repo_root = Path(__file__).resolve().parents[3]
    datasets, _, _ = load_configs(repo_root)
    if args.dataset not in datasets:
        raise ArtifactError(f"unknown dataset {args.dataset}")
    dataset = dict(datasets[args.dataset])
    dataset["id"] = args.dataset
    try:
        verify_dataset(dataset, full=True)
    except DatasetError as exc:
        raise ArtifactError(f"{args.dataset}: verification failed: {exc}") from exc
    freeze_dataset(dataset, args.out)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ArtifactError as exc:
        print(f"artifact freeze failed: {exc}", file=sys.stderr)
        raise SystemExit(2)
