from __future__ import annotations

import time
from concurrent.futures import ThreadPoolExecutor, as_completed

from .model import RunSummary, SolveJob, SolveOptions, SolveResult
from .runner import run_one_job
from .logging_utils import get_logger

log = get_logger(__name__)

class SerialExecutor:
    def __init__(self, options: SolveOptions):
        self.options = options

    def run(self, run_name: str, jobs: list[SolveJob]) -> RunSummary:
        log.info("Starting %s: %d job(s), workers=1", run_name, len(jobs))

        started = time.perf_counter()
        results = []

        for job in jobs:
            log.info("[%s] queued: %s", job.job_id, job.image_path.name)
            results.append(run_one_job(self.options, job))

        elapsed = time.perf_counter() - started
        failed = sum(1 for result in results if not result.solved)

        log.info("Finished %s in %.2fs; failed=%d", run_name, elapsed, failed)

        return RunSummary(
            run_name=run_name,
            workers=1,
            elapsed_seconds=elapsed,
            results=sorted(results, key=lambda item: item.job_index),
        )


class OuterParallelExecutor:
    def __init__(self, options: SolveOptions, workers: int):
        if workers < 1:
            raise ValueError("workers must be >= 1")
        self.options = options
        self.workers = workers

    def run(self, run_name: str, jobs: list[SolveJob]) -> RunSummary:
        log.info(
            "Starting %s: %d job(s), workers=%d",
            run_name,
            len(jobs),
            self.workers,
        )

        started = time.perf_counter()
        results: list[SolveResult] = []

        with ThreadPoolExecutor(max_workers=self.workers) as pool:
            future_to_job = {}

            for job in jobs:
                log.info("[%s] submitted: %s", job.job_id, job.image_path.name)
                future = pool.submit(run_one_job, self.options, job)
                future_to_job[future] = job

            completed_count = 0

            for future in as_completed(future_to_job):
                job = future_to_job[future]
                try:
                    result = future.result()
                except Exception:
                    log.exception("[%s] crashed", job.job_id)
                    raise

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

        return RunSummary(
            run_name=run_name,
            workers=self.workers,
            elapsed_seconds=elapsed,
            results=sorted(results, key=lambda item: item.job_index),
        )
