"""Execute one already-expanded evaluation run with pre/post evidence."""

from __future__ import annotations

import array
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
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


def _block_counters_for_devices(devices: list[str]) -> dict[str, str | None]:
    result: dict[str, str | None] = {}
    for device in devices:
        name = Path(device).name
        path = Path("/sys/class/block") / name / "stat"
        try:
            result[name] = path.read_text().strip()
        except OSError:
            result[name] = None
    return result


def _block_counters(contract: dict[str, Any]) -> dict[str, str | None]:
    return _block_counters_for_devices(list(contract["nvme_dev"]))


def block_counter_deltas(
    before: dict[str, str | None], after: dict[str, str | None]
) -> dict[str, int]:
    totals = {
        "nand_read_commands": 0,
        "nand_read_bytes": 0,
        "nand_read_time_ms": 0,
        "nand_busy_time_ms": 0,
    }
    if set(before) != set(after):
        return {}
    for device in before:
        if before[device] is None or after[device] is None:
            return {}
        left = [int(value) for value in str(before[device]).split()]
        right = [int(value) for value in str(after[device]).split()]
        if len(left) < 10 or len(right) < 10 or any(r < l for l, r in zip(left, right)):
            return {}
        totals["nand_read_commands"] += right[0] - left[0]
        totals["nand_read_bytes"] += (right[2] - left[2]) * 512
        totals["nand_read_time_ms"] += right[3] - left[3]
        totals["nand_busy_time_ms"] += right[9] - left[9]
    return totals


def parse_resource_usage(text: str) -> dict[str, float | int]:
    patterns = {
        "cpu_user_s": r"^\s*User time \(seconds\):\s*([0-9.]+)$",
        "cpu_system_s": r"^\s*System time \(seconds\):\s*([0-9.]+)$",
        "cpu_utilization_pct": r"^\s*Percent of CPU this job got:\s*([0-9.]+)%$",
        "peak_rss_kib": r"^\s*Maximum resident set size \(kbytes\):\s*([0-9]+)$",
    }
    result: dict[str, float | int] = {}
    for key, pattern in patterns.items():
        match = re.search(pattern, text, re.MULTILINE)
        if match:
            result[key] = int(match.group(1)) if key == "peak_rss_kib" else float(match.group(1))
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


def parse_pipeann_metrics(text: str, nq: int, returncode: int) -> dict[str, Any]:
    metrics: dict[str, Any] = {"completed_queries": nq if returncode == 0 else 0}
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if "Mean Latency" not in line or "99.9 Latency" not in line:
            continue
        for candidate in lines[index + 1 :]:
            fields = candidate.split()
            if len(fields) < 8 or not fields[0].isdigit():
                continue
            try:
                metrics.update({
                    "throughput_QPS": float(fields[2]),
                    "mean_latency_ms": float(fields[3]) / 1000.0,
                    "latency_p999_ms": float(fields[4]) / 1000.0,
                    "mean_ios": float(fields[5]),
                    "mean_io_latency_us": float(fields[6]),
                    "cpu_time_s": float(fields[7]),
                })
                if "Recall@" in line and len(fields) >= 9:
                    recall = float(fields[8])
                    metrics["recall@10"] = recall / 100.0 if recall > 1 else recall
                return metrics
            except ValueError:
                continue
    return metrics


def parse_open_loop_pipeann_metrics(text: str, nq: int, returncode: int) -> dict[str, Any]:
    """Merge native service counters with queue-inclusive replay latency."""
    metrics = parse_pipeann_metrics(text, nq, returncode)
    for key, raw in re.findall(
        r"\b([A-Za-z][A-Za-z0-9_@]*)=(-?[0-9]+(?:\.[0-9]+)?)", text
    ):
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
    metrics["completed_queries"] = nq if returncode == 0 else 0
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


def _pipeann_sidecars(run_dir: Path, spec: dict[str, Any]) -> tuple[dict[str, Any], int]:
    result_path = run_dir / f"pipeann-result_{spec['L']}_idx_uint32.bin"
    completed = 0
    if result_path.is_file():
        with result_path.open("rb") as src:
            raw = src.read(8)
        if len(raw) == 8:
            completed, result_k = struct.unpack("<II", raw)
            if result_k != spec["k"] or result_path.stat().st_size != 8 + completed * result_k * 4:
                completed = 0
    trace = run_dir / "trace"
    trace.mkdir(exist_ok=True)
    query_ids = trace / "query_ids.u32"
    if not query_ids.is_file():
        values = array.array("I", range(completed))
        if sys.byteorder != "little":
            values.byteswap()
        query_ids.write_bytes(values.tobytes())
    sidecars = _sidecars(trace)
    sidecars.update({
        "candidate_offsets_sha256": None,
        "candidate_ids_sha256": None,
        "result_ids_sha256": _sha256(result_path) if completed else None,
    })
    return sidecars, completed


def make_warm_evidence(postflight: dict[str, Any], cold_run_id: str) -> dict[str, Any]:
    return {
        **postflight,
        "accepted": True,
        "cold_parent_accepted": True,
        "cold_parent_run_id": cold_run_id,
    }


def run_spec(
    root: Path,
    spec: dict[str, Any],
    identity_evidence: dict[str, Any] | None = None,
    volatile_evidence: dict[str, Any] | None = None,
) -> dict[str, Any]:
    root = Path(root)
    datasets, systems, _ = load_configs(root)
    dataset = datasets[spec["dataset"]]
    external = bool(spec.get("external"))
    contract_path = root / "experiments" / "eval" / "flashanns" / "live-contract.json"
    contract = json.loads(contract_path.read_text())
    if spec.get("phase") == "q4_cache":
        contract = dict(contract)
        contract["cache_limit"] = int(spec["required_cache_limit"])
    preflight_state = "external" if external else ("cold" if spec["state"] == "proof" else spec["state"])
    if external:
        before = {
            "external": True,
            "block_devices": list(systems[spec["system"]].get("block_devices", [])),
        }
    else:
        before = snapshot_and_validate(
            contract, dataset, preflight_state, identity_evidence, volatile_evidence
        )
    evidence_claim = None
    if preflight_state == "cold":
        assert volatile_evidence is not None
        evidence_claim = claim_volatile_evidence(root, volatile_evidence, spec["run_id"])
    device_before = (
        _block_counters_for_devices(before["block_devices"])
        if external else _block_counters(contract)
    )

    run_dir = Path(spec["run_dir"])
    run_dir.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.mkdir(run_dir)
    except FileExistsError as exc:
        raise ValueError(f"refusing existing run ID {spec['run_id']}") from exc
    log = run_dir / "stdout.log"
    resource_log = run_dir / "resource.txt"
    timed_command = spec["command"]
    if Path("/usr/bin/time").is_file():
        timed_command = ["/usr/bin/time", "-v", "-o", str(resource_log), "--", *spec["command"]]
    with log.open("w") as stream:
        completed = subprocess.run(timed_command, stdout=stream, stderr=subprocess.STDOUT, text=True, check=False)
        stream.flush()
        os.fsync(stream.fileno())
    after = dict(before) if external else snapshot_and_validate(contract, dataset, "post", identity_evidence)
    device_after = (
        _block_counters_for_devices(before["block_devices"])
        if external else _block_counters(contract)
    )
    manifest = root / "results" / "eval" / "flashanns" / "manifests" / f"{spec['dataset']}.json"
    binary = Path(spec["command"][0])
    if external:
        parse_external = (
            parse_open_loop_pipeann_metrics
            if spec.get("phase") == "q3_load"
            else parse_pipeann_metrics
        )
        metrics = parse_external(log.read_text(errors="replace"), spec["nq"], completed.returncode)
        sidecars, result_count = _pipeann_sidecars(run_dir, spec)
        metrics["completed_queries"] = result_count
    else:
        metrics = _parse_metrics(log, spec["nq"], completed.returncode)
        sidecars = _sidecars(run_dir / "trace")
    record = {
        **{key: spec[key] for key in ("run_id", "dataset", "metric", "phase", "system", "state", "L", "k", "nq", "repeat", "command")},
        "git": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
        "binary_sha256": _sha256(binary),
        "artifact_manifest_sha256": _sha256(manifest),
        "preflight_before": before,
        "preflight_after": after,
        "device_before": device_before,
        "device_after": device_after,
        "metrics": metrics,
        "sidecars": sidecars,
        "validation": {"status": "pending", "returncode": completed.returncode},
        "external": external,
    }
    record["metrics"].update(block_counter_deltas(device_before, device_after))
    if resource_log.is_file():
        record["metrics"].update(parse_resource_usage(resource_log.read_text(errors="replace")))
    for key in ("threads", "arrival_rate", "cache_gib", "required_cache_limit", "cold_parent_run_id"):
        if key in spec:
            record[key] = spec[key]
    if evidence_claim is not None:
        record["volatile_evidence_claim"] = evidence_claim
    atomic_json_write(run_dir / "run.json", record)
    if preflight_state == "cold" and completed.returncode == 0:
        atomic_json_write(
            run_dir / "warm-evidence.json",
            make_warm_evidence(after, spec["run_id"]),
        )
    return record
