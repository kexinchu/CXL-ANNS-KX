"""Load and validate the immutable Motivation experiment contract."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
from types import MappingProxyType
from typing import Any, Mapping


_REQUIRED_PHASES = {
    "c1_tier",
    "c2_admission",
    "c2_coverage",
    "c3_capability",
    "c3_qd",
    "c3_batching",
}
_SEQUENCE_KEYS = {"datasets", "tiers", "policies", "concurrency", "threads", "conditions"}


@dataclass(frozen=True)
class Contract:
    schema_version: int
    cache_bytes: int
    page_bytes: int
    stripe_bytes: int
    repeats: tuple[int, ...]
    phases: Mapping[str, Mapping[str, Any]]


def _freeze_phase(phase: Mapping[str, Any]) -> Mapping[str, Any]:
    frozen = {
        key: tuple(value) if key in _SEQUENCE_KEYS and isinstance(value, list) else value
        for key, value in phase.items()
    }
    return MappingProxyType(frozen)


def _validate(contract: Contract) -> None:
    if contract.schema_version != 1:
        raise ValueError("unsupported contract schema")
    if contract.repeats != (0, 1, 2, 3, 4):
        raise ValueError("Motivation requires exactly repetition IDs 0..4")
    if contract.cache_bytes != 4 * 1024**3:
        raise ValueError("CXL-side cache must be 4 GiB")
    if contract.page_bytes != 4096 or contract.stripe_bytes != 2 * 1024**2:
        raise ValueError("unexpected page or stripe geometry")
    if set(contract.phases) != _REQUIRED_PHASES:
        raise ValueError("contract phase set is incomplete or contains extras")


def load_contract(path: Path | str | None = None) -> Contract:
    contract_path = Path(path) if path else Path(__file__).with_name("contract.json")
    raw = json.loads(contract_path.read_text(encoding="utf-8"))
    contract = Contract(
        schema_version=int(raw["schema_version"]),
        cache_bytes=int(raw["cache_bytes"]),
        page_bytes=int(raw["page_bytes"]),
        stripe_bytes=int(raw["stripe_bytes"]),
        repeats=tuple(int(value) for value in raw["repeats"]),
        phases=MappingProxyType(
            {name: _freeze_phase(phase) for name, phase in raw["phases"].items()}
        ),
    )
    _validate(contract)
    return contract
