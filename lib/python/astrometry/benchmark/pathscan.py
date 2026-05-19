from __future__ import annotations

import os
import shutil
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class ResolvedPaths:
    # resolved project topology
    repo_root: Path
    astrometry_install: Path
    solve_field: Path
    astrometry_cfg: Path
    workspace_root: Path
    dataset_root: Path
    ser_root: Path | None
    output_root: Path


class PathResolutionError(RuntimeError):
    # path discovery failure
    pass


def resolve_project_paths(
    config: dict,
    cli_overrides: dict[str, str | None],
    start: Path | None = None,
) -> ResolvedPaths:
    # repo anchor + adjacent scan scope
    repo_root = find_repo_root(start or Path.cwd())
    scan_roots = [repo_root, repo_root.parent]

    # astrometry runtime install
    astrometry_install = resolve_dir(
        name="astrometry-install",
        cli_value=cli_overrides.get("astrometry_install"),
        env_name="ASTROBENCH_ASTROMETRY_INSTALL",
        config_value=config.get("paths", {}).get("astrometry_install"),
        scan_roots=scan_roots,
        required_markers=[Path("bin/solve-field"), Path("etc/astrometry.cfg")],
    )

    # workspace: datasets, manifests, runs, reports
    workspace_root = resolve_dir(
        name="profiling",
        cli_value=cli_overrides.get("workspace_root"),
        env_name="ASTROBENCH_WORKSPACE_ROOT",
        config_value=config.get("paths", {}).get("workspace_root"),
        scan_roots=scan_roots,
        required_markers=[Path("runs")],
    )

    # dataset locations relative to workspace
    dataset_rel = Path(config.get("dataset", {}).get("categorized_relative", "data/categorized_5img_set"))
    ser_rel = Path(config.get("dataset", {}).get("ser_relative", "data/ser_data"))

    # concrete runtime paths
    solve_field = astrometry_install / "bin" / "solve-field"
    astrometry_cfg = astrometry_install / "etc" / "astrometry.cfg"
    dataset_root = workspace_root / dataset_rel
    ser_root = workspace_root / ser_rel
    output_root = resolve_output_root(config, workspace_root)

    # mandatory binary + cfg
    require_file(solve_field, "solve-field")
    require_file(astrometry_cfg, "astrometry.cfg")

    # ensure run sink
    output_root.mkdir(parents=True, exist_ok=True)

    return ResolvedPaths(
        repo_root=repo_root,
        astrometry_install=astrometry_install,
        solve_field=solve_field,
        astrometry_cfg=astrometry_cfg,
        workspace_root=workspace_root,
        dataset_root=dataset_root,
        ser_root=ser_root if ser_root.is_dir() else None,
        output_root=output_root,
    )


def resolve_output_root(config: dict, workspace_root: Path) -> Path:
    # absolute/custom output override
    output_root_value = config.get("output", {}).get("root")
    if output_root_value:
        return Path(str(output_root_value)).expanduser().resolve()

    # workspace-relative default
    output_rel = Path(config.get("benchmark", {}).get("output_relative", "runs/astrobench"))
    return workspace_root / output_rel


def find_repo_root(start: Path) -> Path:
    # normalize file input to parent dir
    current = start.resolve()
    if current.is_file():
        current = current.parent

    # upward repo marker scan
    for candidate in [current, *current.parents]:
        if is_astrometry_repo(candidate):
            return candidate

    raise PathResolutionError(
        "Could not find astrometry.net repository root. "
        "Run astrobench from inside the repository."
    )


def is_astrometry_repo(path: Path) -> bool:
    # astrometry.net repo signature
    return (
        (path / "bin").is_dir()
        and (path / "lib").is_dir()
        and ((path / ".git").exists() or (path / "configure").exists())
    )


def resolve_dir(
    name: str,
    cli_value: str | None,
    env_name: str,
    config_value: str | None,
    scan_roots: list[Path],
    required_markers: list[Path],
) -> Path:
    # priority-ordered directory candidates
    candidates = candidate_dirs(name, cli_value, env_name, config_value, scan_roots)

    # first marker-complete hit wins
    for candidate in candidates:
        if directory_matches(candidate, required_markers):
            return candidate.resolve()

    searched = "\n".join(f"  - {path}" for path in candidates)
    markers = ", ".join(str(marker) for marker in required_markers)

    raise PathResolutionError(
        f"Could not resolve required directory '{name}'.\n"
        f"Required markers: {markers}\n"
        f"Searched:\n{searched}"
    )


def candidate_dirs(
    name: str,
    cli_value: str | None,
    env_name: str,
    config_value: str | None,
    scan_roots: list[Path],
) -> list[Path]:
    raw_candidates: list[Path] = []

    # explicit sources: CLI > env > config
    for value in [cli_value, os.environ.get(env_name), config_value]:
        if value:
            raw_candidates.append(Path(value).expanduser())

    # convention-based sibling scans
    for root in scan_roots:
        raw_candidates.extend([
            root / name,
            root / name.lower(),
            root / name.replace("-", "_"),
        ])

    # PATH fallback via solve-field
    if name == "astrometry-install":
        found = shutil.which("solve-field")
        if found:
            raw_candidates.append(Path(found).resolve().parents[1])

    return deduplicate_paths(raw_candidates)


def directory_matches(path: Path, required_markers: list[Path]) -> bool:
    # marker-complete directory check
    return path.is_dir() and all((path / marker).exists() for marker in required_markers)


def require_file(path: Path, label: str) -> None:
    # hard file precondition
    if not path.is_file():
        raise PathResolutionError(f"Missing required file for {label}: {path}")


def deduplicate_paths(paths: list[Path]) -> list[Path]:
    seen: set[Path] = set()
    result: list[Path] = []

    # stable-order path normalization
    for path in paths:
        try:
            normalized = path.expanduser().resolve()
        except FileNotFoundError:
            normalized = path.expanduser().absolute()

        if normalized not in seen:
            seen.add(normalized)
            result.append(normalized)

    return result
