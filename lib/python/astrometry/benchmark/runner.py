from __future__ import annotations

import os
import shlex
import signal
import subprocess
import time

from .command import build_solve_field_command
from .logging_utils import get_logger
from .model import SolveJob, SolveOptions, SolveResult

log = get_logger(__name__)

# astrobench rc space: timeout / orchestration / input contract
TIMEOUT_RETURN_CODE = 124
ORCHESTRATOR_EXCEPTION_RETURN_CODE = 125
MISSING_INPUT_RETURN_CODE = 126


def run_one_job(
    options: SolveOptions,
    job: SolveJob,
    *,
    submitted_at: float | None = None,
) -> SolveResult:
    # timing split: queue wait vs solve-field wall time
    started_at = time.perf_counter()
    queue_wait = (
        max(0.0, started_at - submitted_at)
        if submitted_at is not None
        else None
    )
    # per-image isolation: logs + solver artifacts
    job.job_dir.mkdir(parents=True, exist_ok=True)
    job.solver_output_dir.mkdir(parents=True, exist_ok=True)

    stdout_log = job.job_dir / "stdout.log"
    stderr_log = job.job_dir / "stderr.log"
    command_log = job.job_dir / "command.txt"

    # preflight fail: bad manifest entry, no subprocess
    if not job.image_path.is_file():
        message = f"Input image not found: {job.image_path}"
        stdout_log.write_text("", encoding="utf-8")
        stderr_log.write_text(message + "\n", encoding="utf-8")
        command_log.write_text("", encoding="utf-8")

        finished_at = time.perf_counter()
        log.warning("[%s] missing input: %s", job.job_id, job.image_path)

        return SolveResult(
            job_index=job.job_index,
            job_id=job.job_id,
            image_path=job.image_path,
            job_dir=job.job_dir,
            solver_output_dir=job.solver_output_dir,
            return_code=MISSING_INPUT_RETURN_CODE,
            runtime_seconds=finished_at - started_at,
            solved=False,
            stdout_log=stdout_log,
            stderr_log=stderr_log,
            command_log=command_log,
            submitted_at=submitted_at,
            started_at=started_at,
            finished_at=finished_at,
            queue_wait_seconds=queue_wait,
            error_kind="missing_input",
            error_message=message,
        )

    # replay trail: exact solve-field invocation
    command = build_solve_field_command(options, job)
    command_log.write_text(shlex.join(command) + "\n", encoding="utf-8")

    log.info("[%s] start: %s", job.job_id, job.image_path.name)
    log.debug("[%s] command: %s", job.job_id, shlex.join(command))
    log.debug("[%s] job dir: %s", job.job_id, job.job_dir)

    return_code = 0
    error_kind: str | None = None
    error_message: str | None = None

    try:
        with stdout_log.open("w", encoding="utf-8") as out, stderr_log.open("w", encoding="utf-8") as err:
            # proc group: kill child tree on timeout
            process = subprocess.Popen(
                command,
                stdout=out,
                stderr=err,
                start_new_session=True,
            )

            try:
                process.wait(timeout=options.timeout_seconds)
                return_code = int(process.returncode or 0)
            except subprocess.TimeoutExpired:
                # controlled failure: job fails, batch continues
                return_code = TIMEOUT_RETURN_CODE
                error_kind = "timeout"
                error_message = f"timeout after {options.timeout_seconds}s"

                log.warning(
                    "[%s] timeout after %ss; terminating process group",
                    job.job_id,
                    options.timeout_seconds,
                )
                _terminate_process_group(process)

    except OSError as error:
        # launch fault: binary/env/permission layer
        return_code = ORCHESTRATOR_EXCEPTION_RETURN_CODE
        error_kind = "popen_exception"
        error_message = str(error)
        stderr_log.write_text(f"[astrobench] {error_kind}: {error}\n", encoding="utf-8")
        stdout_log.write_text("", encoding="utf-8")
        log.exception("[%s] failed to start solve-field", job.job_id)

    finished_at = time.perf_counter()
    elapsed = finished_at - started_at

     # astrometry success contract: rc=0 + .solved marker
    solved_marker = job.solver_output_dir / f"{job.image_path.stem}.solved"
    solved = solved_marker.exists() and return_code == 0

    if solved:
        log.info("[%s] solved rc=%d time=%.2fs", job.job_id, return_code, elapsed)
    else:
        if error_kind is None and return_code != 0:
            error_kind = "solver_failed"
            error_message = f"solve-field returned {return_code}"

        if error_kind is None:
            error_kind = "not_solved"
            error_message = "solve-field finished but no solved marker was produced"

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
        submitted_at=submitted_at,
        started_at=started_at,
        finished_at=finished_at,
        queue_wait_seconds=queue_wait,
        error_kind=error_kind,
        error_message=error_message,
    )


def failed_result_from_exception(
    job: SolveJob,
    error: BaseException,
    *,
    submitted_at: float | None = None,
    started_at: float | None = None,
) -> SolveResult:
    # last-resort guard: python fault -> failed SolveResult
    now = time.perf_counter()
    actual_started = started_at if started_at is not None else now
    queue_wait = (
        max(0.0, actual_started - submitted_at)
        if submitted_at is not None
        else None
    )

    job.job_dir.mkdir(parents=True, exist_ok=True)
    job.solver_output_dir.mkdir(parents=True, exist_ok=True)

    stdout_log = job.job_dir / "stdout.log"
    stderr_log = job.job_dir / "stderr.log"
    command_log = job.job_dir / "command.txt"

    if not stdout_log.exists():
        stdout_log.write_text("", encoding="utf-8")
    if not command_log.exists():
        command_log.write_text("", encoding="utf-8")

    stderr_log.write_text(
        f"[astrobench] orchestrator_exception: {type(error).__name__}: {error}\n",
        encoding="utf-8",
    )

    return SolveResult(
        job_index=job.job_index,
        job_id=job.job_id,
        image_path=job.image_path,
        job_dir=job.job_dir,
        solver_output_dir=job.solver_output_dir,
        return_code=ORCHESTRATOR_EXCEPTION_RETURN_CODE,
        runtime_seconds=now - actual_started,
        solved=False,
        stdout_log=stdout_log,
        stderr_log=stderr_log,
        command_log=command_log,
        submitted_at=submitted_at,
        started_at=actual_started,
        finished_at=now,
        queue_wait_seconds=queue_wait,
        error_kind="orchestrator_exception",
        error_message=f"{type(error).__name__}: {error}",
    )


def _terminate_process_group(process: subprocess.Popen) -> None:
    # soft kill, then hard kill
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=5)
    except Exception:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except Exception:
            process.kill()
