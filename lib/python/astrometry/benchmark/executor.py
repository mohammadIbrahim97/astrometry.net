from __future__ import annotations

import time
from concurrent.futures import ThreadPoolExecutor, as_completed

from .logging_utils import get_logger
from .model import RunSummary, SolveJob, SolveOptions, SolveResult
from .runner import failed_result_from_exception, run_one_job

log = get_logger(__name__)


class SerialExecutor:
    def __init__(self, options: SolveOptions):
        self.options = options

    def run(self, run_name: str, jobs: list[SolveJob]) -> RunSummary:
        # serial baseline: same result contract as parallel
        log.info("Starting %s: %d job(s), workers=1", run_name, len(jobs))

        started = time.perf_counter()
        results: list[SolveResult] = []

        for completed_count, job in enumerate(jobs, start=1):
            submitted_at = time.perf_counter()
            log.info("[%s] queued: %s", job.job_id, job.image_path.name)

            try:
                result = run_one_job(self.options, job, submitted_at=submitted_at)
            except Exception as error:
                # symmetry: one job -> one failed row
                log.exception("[%s] crashed in serial executor", job.job_id)
                result = failed_result_from_exception(job, error, submitted_at=submitted_at)

            results.append(result)

            log.info(
                "[%s] completed %d/%d solved=%s rc=%d time=%.2fs",
                result.job_id,
                completed_count,
                len(jobs),
                result.solved,
                result.return_code,
                result.runtime_seconds,
            )

        elapsed = time.perf_counter() - started
        failed = sum(1 for result in results if not result.solved)

        log.info("Finished %s in %.2fs; failed=%d", run_name, elapsed, failed)

        return RunSummary(
            run_name=run_name,
            workers=1,
            elapsed_seconds=elapsed,
            results=sorted(results, key=lambda item: item.job_index),
            mode="serial",
        )


class OuterParallelExecutor:
    def __init__(self, options: SolveOptions, workers: int):
        # outer parallelism: many solve-field procs, no solver threads
        if workers < 1:
            raise ValueError("workers must be >= 1")
        self.options = options
        self.workers = workers

    def run(self, run_name: str, jobs: list[SolveJob]) -> RunSummary:
         # scheduler layer: submit + collect, no shared solver state
        log.info(
            "Starting %s: %d job(s), workers=%d",
            run_name,
            len(jobs),
            self.workers,
        )

        started = time.perf_counter()
        results: list[SolveResult] = []

        with ThreadPoolExecutor(max_workers=self.workers) as pool:
             # future metadata: needed when future.result() explodes
            future_to_job = {}
            submitted_at_by_job_id: dict[str, float] = {}

            for job in jobs:
                # queue timestamp: later -> queue_wait_seconds
                submitted_at = time.perf_counter()
                submitted_at_by_job_id[job.job_id] = submitted_at

                log.info("[%s] submitted: %s", job.job_id, job.image_path.name)
                future = pool.submit(
                    run_one_job,
                    self.options,
                    job,
                    submitted_at=submitted_at,
                )
                future_to_job[future] = job

            completed_count = 0

            for future in as_completed(future_to_job):
                 # completion order != manifest order
                job = future_to_job[future]
                submitted_at = submitted_at_by_job_id.get(job.job_id)

                try:
                    result = future.result()
                except Exception as error:
                    # hard rule: failed future != failed batch
                    log.exception("[%s] crashed; recording failed result", job.job_id)
                    result = failed_result_from_exception(job, error, submitted_at=submitted_at)

                completed_count += 1
                results.append(result)

                log.info(
                    "[%s] completed %d/%d solved=%s rc=%d time=%.2fs",
                    result.job_id,
                    completed_count,
                    len(jobs),
                    result.solved,
                    result.return_code,
                    result.runtime_seconds,
                )

        elapsed = time.perf_counter() - started
        failed = sum(1 for result in results if not result.solved)

        log.info(
            "Finished %s in %.2fs; workers=%d; failed=%d",
            run_name,
            elapsed,
            self.workers,
            failed,
        )

        # stable output: manifest/job order, not finish order
        return RunSummary(
            run_name=run_name,
            workers=self.workers,
            elapsed_seconds=elapsed,
            results=sorted(results, key=lambda item: item.job_index),
            mode="parallel",
        )
