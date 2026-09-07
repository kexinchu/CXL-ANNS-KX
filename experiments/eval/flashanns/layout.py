"""Select the physical VMEM image used by an internal evaluation system."""

from __future__ import annotations

import copy
from typing import Any


class LayoutError(ValueError):
    """A system requested an unsupported or unavailable physical layout."""


LAYOUT_ARTIFACT = {"extent": "extent_image", "original": "oracle_image"}


def select_dataset_layout(dataset: dict[str, Any], layout: str) -> dict[str, Any]:
    if layout not in LAYOUT_ARTIFACT:
        raise LayoutError(f"unsupported internal layout {layout!r}")
    selected = copy.deepcopy(dataset)
    artifact = LAYOUT_ARTIFACT[layout]
    path = selected.get("artifacts", {}).get(artifact)
    if not isinstance(path, str) or not path:
        raise LayoutError(f"layout {layout} lacks artifact {artifact}")
    selected["staging"]["host_artifact"] = artifact
    selected["selected_layout"] = layout
    return selected
