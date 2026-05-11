from __future__ import annotations

import csv
import statistics
from collections import Counter
from pathlib import Path

# report core, no execution side effects
from .logging_utils import get_logger
from .model import RunSummary

log = get_logger(__name__)


def compare_runs(serial: RunSummary, parallel: RunSummary, output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)

    # invariant block, used by csv + md
    serial_success = count_success(serial)
    parallel_success = count_success(parallel)

    serial_failures = len(serial.results) - serial_success
    parallel_failures = len(parallel.results) - parallel_success

    serial_median = median_runtime(serial)
    parallel_median = median_runtime(parallel)

    # queue pressure, not solver runtime
    serial_queue_avg = mean_queue_wait(serial)
    parallel_queue_avg = mean_queue_wait(parallel)

    saved_seconds = serial.elapsed_seconds - parallel.elapsed_seconds
    speedup = (
        serial.elapsed_seconds / parallel.elapsed_seconds
        if parallel.elapsed_seconds > 0
        else 0.0
    ) #through put metrics
    efficiency = speedup / parallel.workers if parallel.workers > 0 else 0.0

    same_job_count = len(serial.results) == len(parallel.results)
    same_success_count = serial_success == parallel_success

    # proof gate, not just speed number
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
        ("saved_seconds", f"{saved_seconds:.6f}"),
        ("serial_median_image_runtime_s", f"{serial_median:.6f}"),
        ("parallel_median_image_runtime_s", f"{parallel_median:.6f}"),
        ("serial_avg_queue_wait_s", f"{serial_queue_avg:.6f}"),
        ("parallel_avg_queue_wait_s", f"{parallel_queue_avg:.6f}"),
        ("speedup", f"{speedup:.6f}"),
        ("efficiency", f"{efficiency:.6f}"),
        ("same_job_count", str(int(same_job_count))),
        ("same_success_count", str(int(same_success_count))),
        ("throughput_improved", str(int(throughput_improved))),
        ("valid_clean_proof", str(int(valid_clean_proof))),
        ("serial_failure_kinds", format_failure_kinds(serial)),
        ("parallel_failure_kinds", format_failure_kinds(parallel)),
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
        serial_queue_avg=serial_queue_avg,
        parallel_queue_avg=parallel_queue_avg,
        saved_seconds=saved_seconds,
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
    log.info("Saved time: %.3fs", saved_seconds)
    log.info("Speedup: %.3fx", speedup)
    log.info("Efficiency: %.3f", efficiency)
    log.info("Comparison written: %s", comparison_md)

    if not valid_clean_proof:
        log.warning("Comparison is not a clean throughput proof; inspect failed jobs and counts")


def count_success(summary: RunSummary) -> int:
    return sum(1 for result in summary.results if result.solved)


def median_runtime(summary: RunSummary) -> float:
    if not summary.results:
        return 0.0
    return statistics.median(result.runtime_seconds for result in summary.results)


def mean_queue_wait(summary: RunSummary) -> float:
    values = [
        result.queue_wait_seconds
        for result in summary.results
        if result.queue_wait_seconds is not None
    ]

    if not values:
        return 0.0

    return statistics.mean(values)


def format_failure_kinds(summary: RunSummary) -> str:
    # compact failure taxonomy for reports
    counter = Counter(
        result.error_kind or "unknown"
        for result in summary.results
        if not result.solved
    )

    if not counter:
        return ""

    return ", ".join(f"{kind}:{count}" for kind, count in sorted(counter.items()))


def write_markdown_report(
    *,
    path: Path,
    # rendered md, user-facing proof artifact
    serial: RunSummary,
    parallel: RunSummary,
    serial_success: int,
    parallel_success: int,
    serial_failures: int,
    parallel_failures: int,
    serial_median: float,
    parallel_median: float,
    serial_queue_avg: float,
    parallel_queue_avg: float,
    saved_seconds: float,
    speedup: float,
    efficiency: float,
    same_job_count: bool,
    same_success_count: bool,
    throughput_improved: bool,
    valid_clean_proof: bool,
) -> None:
    # verdict ladder, strict -> weak evidence -> no proof
    if valid_clean_proof:
        verdict = "VALID CLEAN PROOF"
        conclusion = (
            "Outer parallel execution improved batch throughput while preserving "
            "the observed solve success count."
        )
    elif throughput_improved and not same_success_count:
        verdict = "NOT A CLEAN PROOF"
        conclusion = (
            "Parallel execution reduced elapsed time, but success count changed. "
            "Inspect failures before using this result as evidence."
        )
    elif throughput_improved:
        verdict = "PARTIAL EVIDENCE"
        conclusion = (
            "Parallel execution reduced elapsed time, but job/result consistency "
            "must be inspected before final interpretation."
        )
    else:
        verdict = "NO THROUGHPUT IMPROVEMENT PROVEN"
        conclusion = (
            "This run does not prove throughput improvement. Likely causes include "
            "small workload, worker contention, I/O pressure, memory pressure, or solver failures."
        )

    path.write_text(
        f"""# Astrobench serial-vs-parallel comparison

## Verdict

**{verdict}**

## Purpose

This report compares serial Astrometry.net batch execution against concurrent `solve-field` execution.

The comparison measures **batch throughput**. It does not claim that one individual image was solved faster inside the Astrometry.net solver.

## Compared runs

| Field | Serial baseline | Concurrent run |
|---|---:|---:|
| Run name | `{serial.run_name}` | `{parallel.run_name}` |
| Workers | {serial.workers} | {parallel.workers} |
| Job count | {len(serial.results)} | {len(parallel.results)} |
| Successful solves | {serial_success} | {parallel_success} |
| Failed solves | {serial_failures} | {parallel_failures} |
| Total elapsed time [s] | {serial.elapsed_seconds:.3f} | {parallel.elapsed_seconds:.3f} |
| Median per-image runtime [s] | {serial_median:.3f} | {parallel_median:.3f} |
| Avg queue wait [s] | {serial_queue_avg:.6f} | {parallel_queue_avg:.6f} |

## Derived metrics

| Metric | Value |
|---|---:|
| Saved time [s] | {saved_seconds:.3f} |
| Speedup | {speedup:.3f}x |
| Efficiency | {efficiency:.3f} |
| Same job count | {same_job_count} |
| Same success count | {same_success_count} |
| Throughput improved | {throughput_improved} |
| Valid clean proof | {valid_clean_proof} |

## Failure kinds

| Run | Failure kinds |
|---|---|
| Serial | `{format_failure_kinds(serial) or "-"}` |
| Concurrent | `{format_failure_kinds(parallel) or "-"}` |

## Interpretation

{conclusion}

## Boundary

Astrobench starts multiple independent `solve-field` processes. It does not modify the internal Astrometry.net solver.

```text
valid result:
  image batch finished faster

not implied:
  one image solved faster inside the solver core
```""",
encoding="utf-8",
)
