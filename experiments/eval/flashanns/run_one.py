"""Execute one already-expanded evaluation run with pre/post evidence."""

from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
from pathlib import Path
from typing import Any

from experiments.eval.flashanns.config import load_configs
from experiments.eval.flashanns.preflight import atomic_json_write, snapshot_and_validate


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as src:
        for block in iter(lambda: src.read(8 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def claim_volatile_evidence(
    root: Path, evidence: dict[str, Any], run_id: str
) -> dict[str, str]:
    capture_id = str(evidence.get("capture_id", ""))
    if not capture_id:
        raise ValueError("volatile evidence lacks capture_id")
    payload = json.dumps(evidence, sort_keys=True, separators=(",", ":")).encode()
    claim = {
        "capture_id": capture_id,
        "evidence_sha256": hashlib.sha256(payload).hexdigest(),
        "run_id": run_id,
    }
    directory = Path(root) / "results" / "eval" / "flashanns" / "evidence-claims"
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f"{capture_id}.json"
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
    except FileExistsError as exc:
        raise ValueError(f"volatile evidence {capture_id} was already consumed") from exc
    try:
        os.write(fd, (json.dumps(claim, indent=2, sort_keys=True) + "\n").encode())
        os.fsync(fd)
    finally:
        os.close(fd)
    directory_fd = os.open(directory, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(directory_fd)
    finally:
        os.close(directory_fd)
    return claim


def _block_counters(contract: dict[str, Any]) -> dict[str, str | None]:
    result: dict[str, str | None] = {}
    for device in contract["nvme_dev"]:
        name = Path(device).name
        path = Path("/sys/class/block") / name / "stat"
        try:
            result[name] = path.read_text().strip()
        except OSError:
            result[name] = None
    return result


def _parse_metrics(log: Path, nq: int, returncode: int) -> dict[str, Any]:
    text = log.read_text(errors="replace")
    metrics: dict[str, Any] = {"completed_queries": nq if returncode == 0 else 0}
    for key, raw in re.findall(r"\b([A-Za-z][A-Za-z0-9_@]*)=(-?[0-9]+(?:\.[0-9]+)?)", text):
        metrics[key] = float(raw) if "." in raw else int(raw)
    latency = re.search(
        r"^latency_ms\s+mean=([0-9.]+)\s+p50=([0-9.]+)\s+p90=([0-9.]+)\s+p95=([0-9.]+)\s+p99=([0-9.]+)$",
        text,
        re.MULTILINE,
    )
    if latency:
        for key, raw in zip(
            ("mean_latency_ms", "latency_p50_ms", "latency_p90_ms", "latency_p95_ms", "latency_p99_ms"),
            latency.groups(),
        ):
            metrics[key] = float(raw)
    metrics.setdefault("score_bounce", metrics.get("from_bounce", 0))
    metrics.setdefault("score_flash", metrics.get("score_flash", 0))
    metrics.setdefault("nvme_read_B", metrics.get("nvme_read_B", 0))
    return metrics


def _sidecars(trace_dir: Path) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for name in ("query_ids.u32", "latency_ns.u64", "candidate_offsets.u64", "candidate_ids.u32", "result_ids.u32"):
        path = trace_dir / name
        key = name.replace(".", "_") + "_sha256"
        canonical = {
            "query_ids.u32": "query_ids_sha256",
            "candidate_offsets.u64": "candidate_offsets_sha256",
            "candidate_ids.u32": "candidate_ids_sha256",
            "result_ids.u32": "result_ids_sha256",
            "latency_ns.u64": "latency_ns_sha256",
        }[name]
        result[canonical] = _sha256(path) if path.is_file() else None
        result[key] = result[canonical]
    return result


def run_spec(
    root: Path,
    spec: dict[str, Any],
    identity_evidence: dict[str, Any] | None = None,
    volatile_evidence: dict[str, Any] | None = None,
) -> dict[str, Any]:
    root = Path(root)
    datasets, _, _ = load_configs(root)
    dataset = datasets[spec["dataset"]]
    contract_path = root / "experiments" / "eval" / "flashanns" / "live-contract.json"
    contract = json.loads(contract_path.read_text())
    preflight_state = "cold" if spec["state"] == "proof" else spec["state"]
    before = snapshot_and_validate(
        contract, dataset, preflight_state, identity_evidence, volatile_evidence
    )
    evidence_claim = None
    if preflight_state == "cold":
        assert volatile_evidence is not None
        evidence_claim = claim_volatile_evidence(root, volatile_evidence, spec["run_id"])
    device_before = _block_counters(contract)

    run_dir = Path(spec["run_dir"])
    run_dir.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.mkdir(run_dir)
    except FileExistsError as exc:
        raise ValueError(f"refusing existing run ID {spec['run_id']}") from exc
    log = run_dir / "stdout.log"
    with log.open("w") as stream:
        completed = subprocess.run(spec["command"], stdout=stream, stderr=subprocess.STDOUT, text=True, check=False)
        stream.flush()
        os.fsync(stream.fileno())
    after = snapshot_and_validate(contract, dataset, "post", identity_evidence)
    device_after = _block_counters(contract)
    manifest = root / "results" / "eval" / "flashanns" / "manifests" / f"{spec['dataset']}.json"
    binary = Path(spec["command"][0])
    record = {
        **{key: spec[key] for key in ("run_id", "dataset", "metric", "phase", "system", "state", "L", "k", "nq", "repeat", "command")},
        "git": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
        "binary_sha256": _sha256(binary),
        "artifact_manifest_sha256": _sha256(manifest),
        "preflight_before": before,
        "preflight_after": after,
        "device_before": device_before,
        "device_after": device_after,
        "metrics": _parse_metrics(log, spec["nq"], completed.returncode),
        "sidecars": _sidecars(run_dir / "trace"),
        "validation": {"status": "pending", "returncode": completed.returncode},
    }
    if evidence_claim is not None:
        record["volatile_evidence_claim"] = evidence_claim
    atomic_json_write(run_dir / "run.json", record)
    return record
