from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class SolveOptions:
    solve_field_bin: Path
    astrometry_cfg: Path
    cpulimit_seconds: int
    timeout_seconds: int
    no_plots: bool
    overwrite: bool
    keep_temp: bool
    optional_args: tuple[str, ...]


@dataclass(frozen=True)
class SolveJob:
    job_index: int
    job_id: str
    image_path: Path
    job_dir: Path
    solver_output_dir: Path


@dataclass(frozen=True)
class SolveResult:
    job_index: int
    job_id: str
    image_path: Path
    job_dir: Path
    solver_output_dir: Path
    return_code: int
    runtime_seconds: float
    solved: bool
    stdout_log: Path
    stderr_log: Path
    command_log: Path

@dataclass
class WorkerSuggestion:
    usable_cpus: int
    installed_cpus: int | None
    official_workers: int
    aggressive_workers: int
    stress_workers: int


@dataclass(frozen=True)
class RunSummary:
    run_name: str
    workers: int
    elapsed_seconds: float
    results: list[SolveResult]
