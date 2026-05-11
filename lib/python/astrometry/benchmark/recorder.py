from __future__ import annotations

import csv
import json
from dataclasses import asdict
from pathlib import Path
from typing import Any

from .model import RunSummary


def write_run_summary(summary: RunSummary, run_dir: Path) -> None:
    # run artifact root
    run_dir.mkdir(parents=True, exist_ok=True)

    # tabular + structured exports
    write_results_csv(summary, run_dir / "results.csv")
    write_run_json(summary, run_dir / "run.json")


def write_results_csv(summary: RunSummary, path: Path) -> None:
    # semicolon CSV for spreadsheet-safe export
    with path.open("w", encoding="utf-8", newline="") as file:
        writer = csv.writer(file, delimiter=";")

        # stable column contract
        writer.writerow([
            "job_index",
            "job_id",
            "image_path",
            "return_code",
            "runtime_seconds",
            "queue_wait_seconds",
            "solved",
            "error_kind",
            "error_message",
            "job_dir",
            "solver_output_dir",
            "stdout_log",
            "stderr_log",
            "command_log",
        ])

        # 1 row per solver job
        for result in summary.results:
            writer.writerow([
                result.job_index,
                result.job_id,
                result.image_path,
                result.return_code,
                f"{result.runtime_seconds:.6f}",
                format_optional_float(result.queue_wait_seconds),
                int(result.solved),
                result.error_kind or "",
                result.error_message or "",
                result.job_dir,
                result.solver_output_dir,
                result.stdout_log,
                result.stderr_log,
                result.command_log,
            ])


def write_run_json(summary: RunSummary, path: Path) -> None:
    # run-level metadata + expanded results
    payload = {
        "run_name": summary.run_name,
        "mode": summary.mode,
        "workers": summary.workers,
        "elapsed_seconds": summary.elapsed_seconds,
        "job_count": len(summary.results),
        "success_count": sum(1 for result in summary.results if result.solved),
        "failure_count": sum(1 for result in summary.results if not result.solved),
        "results": [serialize_result(result) for result in summary.results],
    }

    # human-readable JSON snapshot
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")


def serialize_result(result) -> dict[str, Any]:
    # dataclass -> JSON-ready payload
    payload = asdict(result)

    # Path normalization
    for key in [
        "image_path",
        "job_dir",
        "solver_output_dir",
        "stdout_log",
        "stderr_log",
        "command_log",
    ]:
        payload[key] = str(payload[key])

    return payload


def format_optional_float(value: float | None) -> str:
    # CSV blank for missing metric
    if value is None:
        return ""

    return f"{value:.6f}"


def copy_text_snapshot(source: Path, destination: Path) -> None:
    # copy only existing text files
    if source.is_file():
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(source.read_text(encoding="utf-8"), encoding="utf-8")
