from __future__ import annotations

import csv
import json
from dataclasses import asdict
from pathlib import Path

from .model import RunSummary


def write_run_summary(summary: RunSummary, run_dir: Path) -> None:
    run_dir.mkdir(parents=True, exist_ok=True)
    write_results_csv(summary, run_dir / "results.csv")
    write_run_json(summary, run_dir / "run.json")


def write_results_csv(summary: RunSummary, path: Path) -> None:
    with path.open("w", encoding="utf-8", newline="") as file:
        writer = csv.writer(file, delimiter=";")
        writer.writerow([
            "job_index",
            "job_id",
            "image_path",
            "return_code",
            "runtime_seconds",
            "solved",
            "job_dir",
            "solver_output_dir",
        ])

        for result in summary.results:
            writer.writerow([
                result.job_index,
                result.job_id,
                result.image_path,
                result.return_code,
                f"{result.runtime_seconds:.6f}",
                int(result.solved),
                result.job_dir,
                result.solver_output_dir,
            ])


def write_run_json(summary: RunSummary, path: Path) -> None:
    payload = {
        "run_name": summary.run_name,
        "workers": summary.workers,
        "elapsed_seconds": summary.elapsed_seconds,
        "success_count": sum(1 for result in summary.results if result.solved),
        "failure_count": sum(1 for result in summary.results if not result.solved),
        "results": [
            {
                **asdict(result),
                "image_path": str(result.image_path),
                "job_dir": str(result.job_dir),
                "solver_output_dir": str(result.solver_output_dir),
                "stdout_log": str(result.stdout_log),
                "stderr_log": str(result.stderr_log),
                "command_log": str(result.command_log),
            }
            for result in summary.results
        ],
    }

    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")


def copy_text_snapshot(source: Path, destination: Path) -> None:
    if source.is_file():
        destination.write_text(source.read_text(encoding="utf-8"), encoding="utf-8")
