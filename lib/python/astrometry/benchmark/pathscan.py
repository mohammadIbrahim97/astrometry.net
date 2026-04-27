from __future__ import annotations

import os
import shutil
from dataclasses import dataclass
from pathlib import Path

@dataclass(frozen=True) # TODO: Move to model
class ResolvedPaths:
    repo_root: Path
    astrometry_install: Path
    solve_field: Path
    astrometry_cfg: Path
    profiling_root: Path
    dataset_root: Path
    ser_root: Path | None
    output_root: Path
    flamegraph_dir: Path | None


class PathResolutionError(RuntimeError):
    pass


def resolve_project_paths(
    config: dict,
    cli_overrides: dict[str, str | None],
    start: Path | None = None,
) -> ResolvedPaths:
    repo_root = find_repo_root(start or Path.cwd())
    scan_roots = [repo_root, repo_root.parent]

    astrometry_install = resolve_dir(
        name="astrometry-install",
        cli_value=cli_overrides.get("astrometry_install"),
        env_name="ASTROBENCH_ASTROMETRY_INSTALL",
        config_value=config.get("paths", {}).get("astrometry_install"),
        scan_roots=scan_roots,
        required_markers=[Path("bin/solve-field"), Path("etc/astrometry.cfg")],
    )

    profiling_root = resolve_dir(
        name="profiling",
        cli_value=cli_overrides.get("profiling_root"),
        env_name="ASTROBENCH_PROFILING_ROOT",
        config_value=config.get("paths", {}).get("profiling_root"),
        scan_roots=scan_roots,
        required_markers=[Path("data/categorized_5img_set"), Path("runs")],
    )

    flamegraph_dir = resolve_optional_dir(
        name="FlameGraph",
        cli_value=cli_overrides.get("flamegraph_dir"),
        env_name="ASTROBENCH_FLAMEGRAPH_DIR",
        config_value=config.get("paths", {}).get("flamegraph_dir"),
        scan_roots=[
            repo_root / "tools",
            repo_root.parent / "tools",
            repo_root,
            repo_root.parent,
            Path.home() / "tools",
        ],
        required_markers=[Path("flamegraph.pl"), Path("stackcollapse-perf.pl")],
    )

    dataset_rel = Path(config.get("dataset", {}).get("categorized_relative", "data/categorized_5img_set"))
    ser_rel = Path(config.get("dataset", {}).get("ser_relative", "data/ser_data"))
    output_rel = Path(config.get("benchmark", {}).get("output_relative", "runs/astrobench"))

    solve_field = astrometry_install / "bin" / "solve-field"
    astrometry_cfg = astrometry_install / "etc" / "astrometry.cfg"
    dataset_root = profiling_root / dataset_rel
    ser_root = profiling_root / ser_rel
    output_root = profiling_root / output_rel

    require_file(solve_field, "solve-field")
    require_file(astrometry_cfg, "astrometry.cfg")
    require_dir(dataset_root, "categorized dataset root")

    output_root.mkdir(parents=True, exist_ok=True)

    return ResolvedPaths(
        repo_root=repo_root,
        astrometry_install=astrometry_install,
        solve_field=solve_field,
        astrometry_cfg=astrometry_cfg,
        profiling_root=profiling_root,
        dataset_root=dataset_root,
        ser_root=ser_root if ser_root.is_dir() else None,
        output_root=output_root,
        flamegraph_dir=flamegraph_dir,
    )


def find_repo_root(start: Path) -> Path:
    current = start.resolve()
    if current.is_file():
        current = current.parent

    for candidate in [current, *current.parents]:
        if is_astrometry_repo(candidate):
            return candidate

    raise PathResolutionError(
        "Could not find astrometry.net repository root. "
        "Run astrobench from inside the repository."
    )


def is_astrometry_repo(path: Path) -> bool:
    return (
        (path / "bin").is_dir()
        and (path / "lib").is_dir()
        and (path / "doc").is_dir()
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
    candidates = candidate_dirs(name, cli_value, env_name, config_value, scan_roots)

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


def resolve_optional_dir(
    name: str,
    cli_value: str | None,
    env_name: str,
    config_value: str | None,
    scan_roots: list[Path],
    required_markers: list[Path],
) -> Path | None:
    candidates = candidate_dirs(name, cli_value, env_name, config_value, scan_roots)

    for candidate in candidates:
        if directory_matches(candidate, required_markers):
            return candidate.resolve()

    return None


def candidate_dirs(
    name: str,
    cli_value: str | None,
    env_name: str,
    config_value: str | None,
    scan_roots: list[Path],
) -> list[Path]:
    raw_candidates: list[Path] = []

    for value in [cli_value, os.environ.get(env_name), config_value]:
        if value:
            raw_candidates.append(Path(value).expanduser())

    for root in scan_roots:
        raw_candidates.extend([
            root / name,
            root / name.lower(),
            root / name.replace("-", "_"),
        ])

    if name == "astrometry-install":
        found = shutil.which("solve-field")
        if found:
            raw_candidates.append(Path(found).resolve().parents[1])

    return deduplicate_paths(raw_candidates)


def directory_matches(path: Path, required_markers: list[Path]) -> bool:
    return path.is_dir() and all((path / marker).exists() for marker in required_markers)


def require_file(path: Path, label: str) -> None:
    if not path.is_file():
        raise PathResolutionError(f"Missing required file for {label}: {path}")


def require_dir(path: Path, label: str) -> None:
    if not path.is_dir():
        raise PathResolutionError(f"Missing required directory for {label}: {path}")


def deduplicate_paths(paths: list[Path]) -> list[Path]:
    seen: set[Path] = set()
    result: list[Path] = []

    for path in paths:
        try:
            normalized = path.expanduser().resolve()
        except FileNotFoundError:
            normalized = path.expanduser().absolute()

        if normalized not in seen:
            seen.add(normalized)
            result.append(normalized)

    return result
