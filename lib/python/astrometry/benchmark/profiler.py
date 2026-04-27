from __future__ import annotations

import shutil
import subprocess
from pathlib import Path
from .logging_utils import get_logger

log = get_logger(__name__)

SOFT_EVENTS = "task-clock,context-switches,cpu-migrations,page-faults,minor-faults,major-faults"
HW_EVENTS = "cycles,instructions,branches,branch-misses,cache-references,cache-misses"


def require_tool(name: str) -> None:
    if shutil.which(name) is None:
        raise RuntimeError(f"Required tool not found in PATH: {name}")


def run_perf_stat(command: list[str], output_dir: Path) -> None:
    require_tool("perf")

    output_dir.mkdir(parents=True, exist_ok=True)

    stdout_log = output_dir / "stdout.log"
    stderr_log = output_dir / "stderr.log"
    perf_csv = output_dir / "perf_stat.csv"

    log.info("Starting perf stat")
    log.info("Output: %s", perf_csv)
    log.debug("Target command: %s", " ".join(command))

    full_command = [
        "perf", "stat",
        "-x", ";",
        "-o", str(perf_csv),
        "-e", f"{SOFT_EVENTS},{HW_EVENTS}",
        "--",
        *command,
    ]

    with stdout_log.open("w", encoding="utf-8") as out, stderr_log.open("w", encoding="utf-8") as err:
        completed = subprocess.run(full_command, stdout=out, stderr=err, check=False)

    if completed.returncode == 0:
        log.info("perf stat complete")
        return

    log.warning("Hardware counters failed; falling back to software events")

    fallback_command = [
        "perf", "stat",
        "-x", ";",
        "-o", str(perf_csv),
        "-e", SOFT_EVENTS,
        "--",
        *command,
    ]

    with stdout_log.open("a", encoding="utf-8") as out, stderr_log.open("a", encoding="utf-8") as err:
        err.write("\n[astrobench] Hardware counters failed. Falling back to software events.\n")
        subprocess.run(fallback_command, stdout=out, stderr=err, check=False)

    log.info("perf stat fallback complete")
