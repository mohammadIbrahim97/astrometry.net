from __future__ import annotations

import os
import shlex
import signal
import subprocess
import time

from .command import build_solve_field_command
from .model import SolveJob, SolveOptions, SolveResult
from .logging_utils import get_logger

log = get_logger(__name__)

def run_one_job(options: SolveOptions, job: SolveJob) -> SolveResult:
    job.job_dir.mkdir(parents=True, exist_ok=True)
    job.solver_output_dir.mkdir(parents=True, exist_ok=True)

    stdout_log = job.job_dir / "stdout.log"
    stderr_log = job.job_dir / "stderr.log"
    command_log = job.job_dir / "command.txt"

    command = build_solve_field_command(options, job)
    command_log.write_text(shlex.join(command) + "\n", encoding="utf-8")

    log.info("[%s] start: %s", job.job_id, job.image_path.name)
    log.debug("[%s] command: %s", job.job_id, shlex.join(command))
    log.debug("[%s] job dir: %s", job.job_id, job.job_dir)

    started = time.perf_counter()
    return_code = 0

    with stdout_log.open("w", encoding="utf-8") as out, stderr_log.open("w", encoding="utf-8") as err:
        process = subprocess.Popen(
            command,
            stdout=out,
            stderr=err,
            start_new_session=True,
        )

        try:
            process.wait(timeout=options.timeout_seconds)
            return_code = process.returncode
        except subprocess.TimeoutExpired:
            return_code = 124
            log.warning(
                "[%s] timeout after %ss; terminating process group",
                job.job_id,
                options.timeout_seconds,
            )
            _terminate_process_group(process)

    elapsed = time.perf_counter() - started
    solved_marker = job.solver_output_dir / f"{job.image_path.stem}.solved"
    solved = solved_marker.exists()

    if solved and return_code == 0:
        log.info("[%s] solved rc=%d time=%.2fs", job.job_id, return_code, elapsed)
    else:
        log.warning(
            "[%s] not solved rc=%d time=%.2fs stderr=%s",
            job.job_id,
            return_code,
            elapsed,
            stderr_log,
        )

    return SolveResult(
        job_index=job.job_index,
        job_id=job.job_id,
        image_path=job.image_path,
        job_dir=job.job_dir,
        solver_output_dir=job.solver_output_dir,
        return_code=return_code,
        runtime_seconds=elapsed,
        solved=solved,
        stdout_log=stdout_log,
        stderr_log=stderr_log,
        command_log=command_log,
    )


def _terminate_process_group(process: subprocess.Popen) -> None:
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=5)
    except Exception:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except Exception:
            process.kill()
