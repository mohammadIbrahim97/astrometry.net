from __future__ import annotations

import csv
import statistics
from pathlib import Path

from .logging_utils import get_logger
from .model import RunSummary

log = get_logger(__name__)


def compare_runs(serial: RunSummary, parallel: RunSummary, output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)

    serial_success = count_success(serial)
    parallel_success = count_success(parallel)

    serial_failures = len(serial.results) - serial_success
    parallel_failures = len(parallel.results) - parallel_success

    serial_median = median_runtime(serial)
    parallel_median = median_runtime(parallel)

    speedup = (
        serial.elapsed_seconds / parallel.elapsed_seconds
        if parallel.elapsed_seconds > 0
        else 0.0
    )
    efficiency = speedup / parallel.workers if parallel.workers > 0 else 0.0

    same_job_count = len(serial.results) == len(parallel.results)
    same_success_count = serial_success == parallel_success
    throughput_improved = parallel.elapsed_seconds < serial.elapsed_seconds
    valid_clean_proof = same_job_count and same_success_count and throughput_improved

    comparison_csv = output_dir / "comparison.csv"
    comparison_md = output_dir / "comparison.md"

    rows = [
        ("serial_run_name", serial.run_name),
        ("parallel_run_name", parallel.run_name),
        ("serial_workers", str(serial.workers)),
        ("parallel_workers", str(parallel.workers)),
        ("serial_job_count", str(len(serial.results))),
        ("parallel_job_count", str(len(parallel.results))),
        ("serial_success_count", str(serial_success)),
        ("parallel_success_count", str(parallel_success)),
        ("serial_failure_count", str(serial_failures)),
        ("parallel_failure_count", str(parallel_failures)),
        ("serial_elapsed_s", f"{serial.elapsed_seconds:.6f}"),
        ("parallel_elapsed_s", f"{parallel.elapsed_seconds:.6f}"),
        ("serial_median_image_runtime_s", f"{serial_median:.6f}"),
        ("parallel_median_image_runtime_s", f"{parallel_median:.6f}"),
        ("speedup", f"{speedup:.6f}"),
        ("efficiency", f"{efficiency:.6f}"),
        ("same_job_count", str(int(same_job_count))),
        ("same_success_count", str(int(same_success_count))),
        ("throughput_improved", str(int(throughput_improved))),
        ("valid_clean_proof", str(int(valid_clean_proof))),
    ]

    with comparison_csv.open("w", encoding="utf-8", newline="") as file:
        writer = csv.writer(file, delimiter=";")
        writer.writerow(["metric", "value"])
        writer.writerows(rows)

    write_markdown_report(
        path=comparison_md,
        serial=serial,
        parallel=parallel,
        serial_success=serial_success,
        parallel_success=parallel_success,
        serial_failures=serial_failures,
        parallel_failures=parallel_failures,
        serial_median=serial_median,
        parallel_median=parallel_median,
        speedup=speedup,
        efficiency=efficiency,
        same_job_count=same_job_count,
        same_success_count=same_success_count,
        throughput_improved=throughput_improved,
        valid_clean_proof=valid_clean_proof,
    )

    log.info("Comparison complete")
    log.info("Serial elapsed: %.3fs", serial.elapsed_seconds)
    log.info("Parallel elapsed: %.3fs", parallel.elapsed_seconds)
    log.info("Speedup: %.3fx", speedup)
    log.info("Efficiency: %.3f", efficiency)
    log.info("Comparison written: %s", comparison_md)

    if not same_job_count:
        log.warning(
            "Job count differs: serial=%d parallel=%d",
            len(serial.results),
            len(parallel.results),
        )

    if not same_success_count:
        log.warning(
            "Success count changed: serial=%d parallel=%d",
            serial_success,
            parallel_success,
        )

    if not valid_clean_proof:
        log.warning("This run is not a clean official proof of throughput improvement")


def count_success(summary: RunSummary) -> int:
    return sum(1 for result in summary.results if result.solved)


def median_runtime(summary: RunSummary) -> float:
    if not summary.results:
        return 0.0

    return statistics.median(result.runtime_seconds for result in summary.results)


def write_markdown_report(
    *,
    path: Path,
    serial: RunSummary,
    parallel: RunSummary,
    serial_success: int,
    parallel_success: int,
    serial_failures: int,
    parallel_failures: int,
    serial_median: float,
    parallel_median: float,
    speedup: float,
    efficiency: float,
    same_job_count: bool,
    same_success_count: bool,
    throughput_improved: bool,
    valid_clean_proof: bool,
) -> None:
    if valid_clean_proof:
        verdict = "VALID CLEAN PROOF"
        conclusion = (
            "The outer-layer parallel execution improved batch throughput while "
            "preserving the observed solve success count."
        )
    elif throughput_improved and not same_success_count:
        verdict = "NOT A CLEAN PROOF"
        conclusion = (
            "The parallel execution reduced total elapsed time, but the result is "
            "not a clean proof because the success count changed. Some parallel jobs "
            "failed or timed out."
        )
    elif throughput_improved:
        verdict = "PARTIAL EVIDENCE"
        conclusion = (
            "The parallel execution reduced total elapsed time, but job count or "
            "result consistency must be inspected before using this as final evidence."
        )
    else:
        verdict = "NO THROUGHPUT IMPROVEMENT PROVEN"
        conclusion = (
            "This run does not prove throughput improvement. Possible causes include "
            "too small workload, too many workers, I/O contention, memory pressure, "
            "or solver failures."
        )

    path.write_text(
        f"""# Astrobench serial-vs-parallel comparison

## Verdict

**{verdict}**

## Purpose

This report compares a serial Astrometry.net batch execution against an outer-layer parallel batch execution.

The comparison verifies **batch throughput**, not the internal runtime of one individual image solve.

## Compared runs

| Field | Serial baseline | Outer-parallel run |
|---|---:|---:|
| Run name | `{serial.run_name}` | `{parallel.run_name}` |
| Workers | {serial.workers} | {parallel.workers} |
| Job count | {len(serial.results)} | {len(parallel.results)} |
| Successful solves | {serial_success} | {parallel_success} |
| Failed solves | {serial_failures} | {parallel_failures} |
| Total elapsed time [s] | {serial.elapsed_seconds:.3f} | {parallel.elapsed_seconds:.3f} |
| Median per-image runtime [s] | {serial_median:.3f} | {parallel_median:.3f} |

## Derived metrics

| Metric | Value |
|---|---:|
| Speedup | {speedup:.3f}x |
| Efficiency | {efficiency:.3f} |
| Same job count | {same_job_count} |
| Same success count | {same_success_count} |
| Throughput improved | {throughput_improved} |
| Valid clean proof | {valid_clean_proof} |

## Interpretation

{conclusion}

## Correctness boundary

The internal Astrometry.net solver is not modified by this outer-layer benchmark. Each input image is still processed through a normal `solve-field` execution. The parallel layer only schedules multiple independent `solve-field` jobs at the same time.

Therefore, a successful result means:

```text
The image batch finished faster.
"""
)
