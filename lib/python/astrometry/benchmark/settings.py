from __future__ import annotations

import json
import tomllib
from pathlib import Path
from typing import Any

from .config import local_config_path


# default machine-local TOML template
DEFAULT_LOCAL_CONFIG = """# Astrobench local configuration
# Machine-specific. Do not commit.

[input]
default_dir = ""
recursive = false
limit = 0

[output]
root = ""

[solver]
timeout_seconds = 180
cpulimit_seconds = 120

[execution]
workers = "auto"
"""


def init_local_config(repo_root: Path, *, force: bool = False) -> Path:
    path = local_config_path(repo_root)

    # preserve existing config unless forced
    if path.exists() and not force:
        return path

    path.write_text(DEFAULT_LOCAL_CONFIG, encoding="utf-8")
    return path


def read_local_config(repo_root: Path) -> dict[str, Any]:
    path = local_config_path(repo_root)

    # absent config -> empty override set
    if not path.is_file():
        return {}

    # TOML requires binary read
    with path.open("rb") as file:
        return tomllib.load(file)


def write_local_config(repo_root: Path, config: dict[str, Any]) -> Path:
    path = local_config_path(repo_root)

    # full config rewrite
    path.write_text(dump_toml(config), encoding="utf-8")
    return path


def get_config_value(config: dict[str, Any], dotted_key: str) -> Any:
    current: Any = config

    # dotted path traversal
    for part in dotted_key.split("."):
        if not isinstance(current, dict) or part not in current:
            raise KeyError(dotted_key)
        current = current[part]

    return current


def set_config_value(config: dict[str, Any], dotted_key: str, value: Any) -> dict[str, Any]:
    parts = dotted_key.split(".")
    current = config

    # create missing branches
    for part in parts[:-1]:
        next_value = current.get(part)
        if not isinstance(next_value, dict):
            next_value = {}
            current[part] = next_value
        current = next_value

    # leaf assignment
    current[parts[-1]] = value
    return config


def parse_config_value(raw: str) -> Any:
    text = raw.strip()

    # bool coercion
    if text.lower() in {"true", "false"}:
        return text.lower() == "true"

    # CLI null alias -> empty string
    if text.lower() in {"none", "null"}:
        return ""

    # int before float to preserve type
    try:
        return int(text)
    except ValueError:
        pass

    try:
        return float(text)
    except ValueError:
        pass

    # JSON array/object input
    if text.startswith("[") or text.startswith("{"):
        try:
            return json.loads(text)
        except json.JSONDecodeError:
            pass

    return raw


def dump_toml(config: dict[str, Any]) -> str:
    lines: list[str] = []

    # shallow section writer
    for section, values in config.items():
        if isinstance(values, dict):
            lines.append(f"[{section}]")
            for key, value in values.items():
                lines.append(f"{key} = {format_toml_value(value)}")
            lines.append("")
        else:
            lines.append(f"{section} = {format_toml_value(values)}")

    # POSIX newline
    return "\n".join(lines).rstrip() + "\n"


def format_toml_value(value: Any) -> str:
    # TOML scalar encoding
    if isinstance(value, bool):
        return "true" if value else "false"

    if isinstance(value, int | float):
        return str(value)

    # inline array encoding
    if isinstance(value, list | tuple):
        return "[" + ", ".join(format_toml_value(item) for item in value) + "]"

    # string fallback
    return json.dumps(str(value))
