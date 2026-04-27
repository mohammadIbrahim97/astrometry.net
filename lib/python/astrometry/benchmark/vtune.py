from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path


SUPPORTED_ANALYSES = {"hotspots", "threading", "hpc-performance"}


def run_vtune_collect(
    analysis: str,
    target_command: list[str],
    result_dir: Path,
    vtune_env: str | None = None,
) -> None:
    if analysis not in SUPPORTED_ANALYSES:
        raise ValueError(f"Unsupported VTune analysis: {analysis}")

    if shutil.which("vtune") is None and vtune_env:
        command = f'source "{vtune_env}" && vtune -collect "{analysis}" -result-dir "{result_dir}" -- ' \
                  + " ".join(_quote(item) for item in target_command)
        subprocess.run(["bash", "-lc", command], check=False)
        return

    if shutil.which("vtune") is None:
        raise RuntimeError("vtune not found in PATH and no vtune_env was usable")

    result_dir.mkdir(parents=True, exist_ok=True)

    subprocess.run(
        ["vtune", "-collect", analysis, "-result-dir", str(result_dir), "--", *target_command],
        check=False,
    )


def export_vtune_reports(result_dir: Path, report_dir: Path) -> None:
    if shutil.which("vtune") is None:
        raise RuntimeError("vtune not found in PATH")

    report_dir.mkdir(parents=True, exist_ok=True)

    for report_name in ["summary", "hotspots", "callstacks"]:
        text_out = report_dir / f"{report_name}.txt"
        html_out = report_dir / f"{report_name}.html"

        with text_out.open("w", encoding="utf-8") as file:
            subprocess.run(
                ["vtune", "-report", report_name, "-result-dir", str(result_dir), "-format", "text"],
                stdout=file,
                stderr=subprocess.STDOUT,
                check=False,
            )

        subprocess.run(
            [
                "vtune",
                "-report", report_name,
                "-result-dir", str(result_dir),
                "-format", "html",
                "-report-output", str(html_out),
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )


def _quote(value: str) -> str:
    return "'" + value.replace("'", "'\"'\"'") + "'"
