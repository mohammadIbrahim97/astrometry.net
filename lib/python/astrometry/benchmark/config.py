from __future__ import annotations

import tomllib
from pathlib import Path
from typing import Any


def load_config(path: Path | None) -> dict[str, Any]:
    if path is None:
        path = default_config_path()

    if not path.is_file():
        raise FileNotFoundError(f"Config file not found: {path}")

    with path.open("rb") as file:
        return tomllib.load(file)


def default_config_path() -> Path:
    return Path(__file__).resolve().parents[4] / "examples" / "benchmark" / "astrobench.toml"
