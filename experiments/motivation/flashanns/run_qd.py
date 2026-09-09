"""C3 device-capability and workload queue-depth measurements."""

from __future__ import annotations

from array import array
import argparse
import json
import math
from pathlib import Path
import statistics
import subprocess
import threading
import time
from typing import Iterable, Sequence

from .contract import load_contract
from .observe import block_delta, read_block_snapshot


def validate_capability_block(records: Iterable[dict], levels: Sequence[int]) -> None:
    items = list(records)
    expected = {(level, repeat) for level in levels for repeat in range(5)}
    observed = {(int(row["concurrency"]), int(row["repeat"])) for row in items}
    if observed != expected or len(items) != len(expected):
        raise ValueError("capability block is not complete for five repeats and all levels")
    if any(not isinstance(row.get("bandwidth_gib_s"), (int, float)) or
           row["bandwidth_gib_s"] <= 0 for row in items):
        raise ValueError("capability block has invalid bandwidth")


def capability_knee(points: Iterable[dict]) -> int:
    grouped: dict[int, list[float]] = {}
    for point in points:
        grouped.setdefault(int(point["concurrency"]), []).append(float(point["bandwidth_gib_s"]))
    if not grouped:
        raise ValueError("no capability points")
    medians = {level: statistics.median(values) for level, values in grouped.items()}
    threshold = 0.9 * max(medians.values())
    return min(level for level, bandwidth in medians.items() if bandwidth >= threshold)


def capability_schedule(levels: Sequence[int], repeats: int = 5) -> list[tuple[int, int]]:
    if not levels or repeats <= 0:
        raise ValueError("capability schedule requires levels and repeats")
    result: list[tuple[int, int]] = []
    for repeat in range(repeats):
        rotation = repeat % len(levels)
        ordered = [*levels[rotation:], *levels[:rotation]]
        result.extend((repeat, level) for level in ordered)
    return result


def select_ssd_pages(
    *, image_bytes: int, ram_ranges: Sequence[tuple[int, int]], page_count: int,
    seed: int, cache_bytes: int | None = None, page_bytes: int = 4096,
) -> list[int]:
    if image_bytes <= 0 or image_bytes % page_bytes or page_count <= 0:
        raise ValueError("invalid image or page count")
    if cache_bytes is not None and page_count * page_bytes <= cache_bytes:
        raise ValueError("capability working set must be larger than cache")
    total_pages = image_bytes // page_bytes
    excluded: set[int] = set()
    for start, stop in ram_ranges:
        if (start < 0 or stop < start or stop > image_bytes or
                start % page_bytes or stop % page_bytes):
            raise ValueError("RAM range is invalid or unaligned")
        excluded.update(range(start // page_bytes, stop // page_bytes))
    if page_count > total_pages - len(excluded):
        raise ValueError("not enough SSD pages in image")
    stride = 104729 + 2 * seed
    while math.gcd(stride, total_pages) != 1:
        stride += 2
    page = (seed * 2654435761) % total_pages
    selected: list[int] = []
    for _ in range(total_pages):
        if page not in excluded:
            selected.append(page * page_bytes)
            if len(selected) == page_count:
                return selected
        page = (page + stride) % total_pages
    raise ValueError("could not select the requested SSD pages")


def mean_entire_interval_qd(parsed: dict) -> float:
    if parsed.get("active_only") is not False:
        raise ValueError("active-only QD averages are forbidden")
    intervals = parsed.get("intervals", [])
    if not intervals:
        raise ValueError("no QD intervals")
    return statistics.mean(float(row["aqu_sz"]) for row in intervals)


class BlockQdSampler:
    """Sample aggregate outstanding work from Linux weighted I/O time."""

    def __init__(self, devices: Sequence[str], interval_s: float = 0.1):
        self.devices = tuple(devices)
        self.interval_s = interval_s
        self.intervals: list[dict[str, float]] = []
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._error: BaseException | None = None

    def start(self) -> None:
        before = read_block_snapshot(self.devices)
        started = time.monotonic()

        def sample() -> None:
            nonlocal before, started
            try:
                while not self._stop.wait(self.interval_s):
                    after = read_block_snapshot(self.devices)
                    ended = time.monotonic()
                    elapsed = ended - started
                    delta = block_delta(before, after)
                    aqu = sum(delta[device]["weighted_io_ticks_ms"] for device in self.devices) / (1000.0 * elapsed)
                    read_kib_s = delta["total_read_bytes"] / 1024.0 / elapsed
                    self.intervals.append({"seconds": elapsed, "aqu_sz": aqu, "read_kib_s": read_kib_s})
                    before, started = after, ended
            except BaseException as error:
                self._error = error

        self._thread = threading.Thread(target=sample, name="block-qd-sampler", daemon=True)
        self._thread.start()

    def stop(self) -> dict:
        self._stop.set()
        if self._thread is not None:
            self._thread.join()
        if self._error is not None:
            raise self._error
        if not self.intervals:
            self.intervals.append({"seconds": 0.0, "aqu_sz": 0.0, "read_kib_s": 0.0})
        return {
            "intervals": self.intervals,
            "mean_aqu_sz": statistics.mean(row["aqu_sz"] for row in self.intervals),
            "active_only": False,
            "source": "linux_block_weighted_io_ticks_100ms",
        }


def _write_u64(path: Path, values: Iterable[int]) -> None:
    payload = array("Q", values)
    path.write_bytes(payload.tobytes())


def capability_probe_command(
    *, binary: Path, device: Path, image_offset: int, image_bytes: int,
    query_offsets: Path, pages: Path, output: Path, wave_pages: int,
) -> list[str]:
    return [
        str(binary), "--device", str(device), "--image-offset", str(image_offset),
        "--image-bytes", str(image_bytes),
        "--wave-pages", str(wave_pages),
        "--query-offsets", str(query_offsets), "--pages", str(pages),
        "--output", str(output),
    ]


def run_capability(
    *,
    device: Path,
    binary: Path,
    output: Path,
    image_offset: int,
    image_bytes: int,
    repeat: int,
    devices: Sequence[str],
    operations_per_level: int = 8192,
) -> list[dict]:
    levels = load_contract().phases["c3_capability"]["concurrency"]
    output.mkdir(parents=True, exist_ok=False)
    cursor = repeat * len(levels) * operations_per_level
    records: list[dict] = []
    for level in levels:
        query_count = operations_per_level // level
        page_count = query_count * level
        pages = [((cursor + index) * 4096) % (image_bytes // 4096 * 4096) for index in range(page_count)]
        cursor += page_count
        point = output / f"c{level}"
        point.mkdir()
        _write_u64(point / "query_offsets.u64", range(0, page_count + 1, level))
        _write_u64(point / "pages.u64", pages)
        before = read_block_snapshot(devices)
        sampler = BlockQdSampler(devices)
        sampler.start()
        subprocess.run(capability_probe_command(
            binary=binary, device=device, image_offset=image_offset,
            image_bytes=image_bytes, query_offsets=point / "query_offsets.u64",
            pages=point / "pages.u64", output=point / "probe.json",
            wave_pages=level,
        ), check=True)
        qd = sampler.stop()
        after = read_block_snapshot(devices)
        probe = json.loads((point / "probe.json").read_text())
        elapsed_ns = sum(probe["latency_ns"])
        bandwidth = page_count * 4096 / (1024**3) / (elapsed_ns / 1e9)
        record = {
            "concurrency": level,
            "repeat": repeat,
            "pages": page_count,
            "bandwidth_gib_s": bandwidth,
            "mean_aqu_sz": mean_entire_interval_qd(qd),
            "qd_samples": qd,
            "block_delta": block_delta(before, after),
        }
        (point / "record.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
        records.append(record)
    (output / "records.json").write_text(json.dumps(records, indent=2, sort_keys=True) + "\n")
    return records


def run_workload(command: Sequence[str], output: Path,
                 devices: Sequence[str] = ("nvme1n1", "nvme2n1")) -> dict:
    output.mkdir(parents=True, exist_ok=False)
    before = read_block_snapshot(devices)
    sampler = BlockQdSampler(devices)
    sampler.start()
    started = time.monotonic()
    completed = subprocess.run(command, text=True, capture_output=True)
    elapsed = time.monotonic() - started
    qd = sampler.stop()
    after = read_block_snapshot(devices)
    (output / "stdout.log").write_text(completed.stdout)
    (output / "stderr.log").write_text(completed.stderr)
    record = {
        "command": list(command),
        "returncode": completed.returncode,
        "elapsed_seconds": elapsed,
        "qd_samples": qd,
        "mean_aqu_sz": mean_entire_interval_qd(qd),
        "block_delta": block_delta(before, after),
    }
    (output / "record.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    if completed.returncode:
        raise subprocess.CalledProcessError(completed.returncode, command)
    return record


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="mode", required=True)
    capability = subparsers.add_parser("capability")
    capability.add_argument("--device", type=Path, default=Path("/dev/vmem0"))
    capability.add_argument("--binary", type=Path, default=Path("tools/motivation-replay-probe"))
    capability.add_argument("--output", type=Path, required=True)
    capability.add_argument("--image-offset", type=int, required=True)
    capability.add_argument("--image-bytes", type=int, required=True)
    capability.add_argument("--backings", nargs=2, required=True)
    capability.add_argument("--repeat", type=int, choices=range(5), required=True)
    workload = subparsers.add_parser("workload")
    workload.add_argument("--command-json", type=Path, required=True)
    workload.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.mode == "capability":
        run_capability(device=args.device, binary=args.binary, output=args.output,
                       image_offset=args.image_offset, image_bytes=args.image_bytes,
                       repeat=args.repeat, devices=args.backings)
    else:
        command = json.loads(args.command_json.read_text())
        if not isinstance(command, list) or not all(isinstance(item, str) for item in command):
            raise ValueError("command JSON must be a string list")
        run_workload(command, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
