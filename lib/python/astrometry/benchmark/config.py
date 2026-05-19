from __future__ import annotations

import tomllib
from pathlib import Path
from typing import Any


def default_config_path(repo_root: Path | None = None) -> Path:
    if repo_root is not None:
        return repo_root / "examples" / "benchmark" / "astrobench.toml"

    return Path(__file__).resolve().parents[4] / "examples" / "benchmark" / "astrobench.toml"


def local_config_path(repo_root: Path) -> Path:
    return repo_root / ".astrobench.toml"


def load_config(path: Path | None, repo_root: Path) -> dict[str, Any]:
    base_path = path if path is not None else default_config_path(repo_root)

    if not base_path.is_file():
        raise FileNotFoundError(f"Config file not found: {base_path}")

    config = read_toml(base_path)

    # Local config overrides the committed default config.
    local_path = local_config_path(repo_root)
    if local_path.is_file():
        config = deep_merge(config, read_toml(local_path))

    return config


def read_toml(path: Path) -> dict[str, Any]:
    with path.open("rb") as file:
        return tomllib.load(file)


def deep_merge(base: dict[str, Any], override: dict[str, Any]) -> dict[str, Any]:
    result = dict(base)

    for key, value in override.items():
        if isinstance(value, dict) and isinstance(result.get(key), dict):
            result[key] = deep_merge(result[key], value)
        else:
            result[key] = value

    return result
