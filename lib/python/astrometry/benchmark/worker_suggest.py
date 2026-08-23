from __future__ import annotations

import os

from .model import WorkerSuggestion


def detect_usable_cpus() -> int:
    # affinity-aware CPU count
    if hasattr(os, "sched_getaffinity"):
        try:
            return max(1, len(os.sched_getaffinity(0)))
        except OSError:
            pass

    # Python 3.13+ process CPU quota
    process_cpu_count = getattr(os, "process_cpu_count", None)
    if callable(process_cpu_count):
        count = process_cpu_count()
        if count:
            return max(1, int(count))

    # system logical CPU fallback
    count = os.cpu_count()
    if count:
        return max(1, int(count))

    # final safety floor
    return 1


def suggest_workers() -> WorkerSuggestion:
    # effective vs physical availability
    usable = detect_usable_cpus()
    installed = os.cpu_count()

    # conservative baseline
    official = max(1, usable // 2)
    if usable >= 4:
        official = max(2, official)

    # higher-throughput presets
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

    # default preset
    if value is None:
        return suggestion.official_workers

    text = str(value).strip().lower()

    # named presets
    if text in {"official", "safe", "auto"}:
        return suggestion.official_workers

    if text in {"aggressive", "fast"}:
        return suggestion.aggressive_workers

    if text in {"stress", "max", "all"}:
        return suggestion.stress_workers

    # explicit worker count
    try:
        workers = int(text)
    except ValueError as error:
        raise ValueError(
            "workers must be an integer or one of: "
            "official, aggressive, stress, max, all, auto"
        ) from error

    # positive process count only
    if workers < 1:
        raise ValueError("workers must be >= 1")

    return workers


def format_worker_suggestion(suggestion: WorkerSuggestion) -> str:
    # display-safe installed count
    installed = (
        str(suggestion.installed_cpus)
        if suggestion.installed_cpus is not None
        else "unknown"
    )

    # CLI help block
    return f"""Astrobench worker suggestion
============================

Detected CPU information:
  Usable logical CPUs:     {suggestion.usable_cpus}
  Installed logical CPUs:  {installed}

Recommended worker counts:
  official:                {suggestion.official_workers}
  aggressive:              {suggestion.aggressive_workers}
  stress:                  {suggestion.stress_workers}

Meaning:
  official   -> conservative setting for clean comparison runs
  aggressive -> high utilization while leaving one CPU free
  stress     -> all usable CPUs; may cause timeouts or contention

Typical commands:
  ./bin/astrobench run --input-dir /path/to/images --workers aggressive
  ./bin/astrobench compare --input-dir /path/to/images --workers official
"""
