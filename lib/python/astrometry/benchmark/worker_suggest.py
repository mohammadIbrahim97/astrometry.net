from __future__ import annotations

import os
from dataclasses import dataclass
from .model import WorkerSuggestion

def detect_usable_cpus() -> int:
    """
    Return the number of CPUs usable by the current process.

    On Linux, sched_getaffinity is preferred because it respects CPU affinity.
    On newer Python versions, process_cpu_count may also reflect usable CPUs.
    cpu_count is the final portable fallback.
    """
    if hasattr(os, "sched_getaffinity"):
        try:
            return max(1, len(os.sched_getaffinity(0)))
        except OSError:
            pass

    process_cpu_count = getattr(os, "process_cpu_count", None)
    if callable(process_cpu_count):
        count = process_cpu_count()
        if count:
            return max(1, int(count))

    count = os.cpu_count()
    if count:
        return max(1, int(count))

    return 1


def suggest_workers() -> WorkerSuggestion:
    usable = detect_usable_cpus()
    installed = os.cpu_count()

    official = max(1, usable // 2)

    if usable >= 4:
        official = max(2, official)

    aggressive = max(1, usable - 1)
    stress = usable

    return WorkerSuggestion(
        usable_cpus=usable,
        installed_cpus=installed,
        official_workers=official,
        aggressive_workers=aggressive,
        stress_workers=stress,
    )
    
def resolve_worker_value(value: str | int | None) -> int:
    suggestion = suggest_workers()

    if value is None:
        return suggestion.official_workers

    text = str(value).strip().lower()

    if text in {"official", "safe"}:
        return suggestion.official_workers

    if text in {"aggressive", "fast"}:
        return suggestion.aggressive_workers

    if text in {"stress", "max", "all"}:
        return suggestion.stress_workers

    if text == "auto":
        return suggestion.official_workers

    try:
        workers = int(text)
    except ValueError as error:
        raise ValueError(
            "workers must be an integer or one of: "
            "official, aggressive, stress, max, all, auto"
        ) from error

    if workers < 1:
        raise ValueError("workers must be >= 1")

    return workers

def format_worker_suggestion(suggestion: WorkerSuggestion) -> str:
    installed = (
        str(suggestion.installed_cpus)
        if suggestion.installed_cpus is not None
        else "unknown"
    )

    return f"""Astrobench worker suggestion
============================

Detected CPU information:
  Usable logical CPUs:     {suggestion.usable_cpus}
  Installed logical CPUs:  {installed}

Recommended worker counts:
  Official proof workers:  {suggestion.official_workers}
  Aggressive workers:      {suggestion.aggressive_workers}
  Stress-test workers:     {suggestion.stress_workers}

Meaning:
  official   -> use for the documented clean serial-vs-parallel proof
  aggressive -> use when official workers succeeds and more speed is desired
  stress     -> use all usable CPUs; may cause timeouts or contention

Recommended official benchmark command:
  ./bin/astrobench --verbose benchmark --workload mixed20 --workers {suggestion.official_workers}

Recommended profiling commands:
  ./bin/astrobench --verbose profile --tool perf-stat --workload mixed20 --workers 1
  ./bin/astrobench --verbose profile --tool perf-stat --workload mixed20 --workers {suggestion.official_workers}
"""
