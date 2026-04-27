from __future__ import annotations

import argparse
import platform
import sys
from datetime import datetime
from pathlib import Path

from .compare import compare_runs
from .config import load_config
from .executor import OuterParallelExecutor, SerialExecutor
from .model import SolveOptions
from .pathscan import PathResolutionError, resolve_project_paths
from .profiler import run_perf_stat
from .recorder import copy_text_snapshot, write_run_summary
from .vtune import run_vtune_collect, export_vtune_reports
from .logging_utils import attach_run_log, configure_logging, get_logger
from .manifest import (
    build_input_dir_manifest,
    build_workload_manifest,
    make_jobs,
    read_manifest,
    write_manifest,
)
from .worker_suggest import (
    format_worker_suggestion,
    resolve_worker_value,
    suggest_workers,
)

log = get_logger(__name__)

def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    configure_logging(
        verbose=getattr(args, "verbose", False),
        quiet=getattr(args, "quiet", False),
    )

    try:
        config = load_config(args.config)
        paths = resolve_project_paths(config, cli_overrides_from_args(args))
    except (OSError, PathResolutionError) as error:
        print(f"[ERROR] {error}", file=sys.stderr)
        return 2

    if args.command == "suggest-workers":
        return cmd_suggest_workers()

    if args.command == "doctor":
        return cmd_doctor(paths)

    if args.command == "manifest":
        return cmd_manifest(args, config, paths)

    if args.command == "run":
        return cmd_run(args, config, paths)

    if args.command == "benchmark":
        return cmd_benchmark(args, config, paths)

    if args.command == "profile":
        return cmd_profile(args, config, paths)

    if args.command == "vtune":
        return cmd_vtune(args, config, paths)

    parser.print_help()
    return 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="astrobench",
        description="Astrometry.net benchmark and profiling controller.",
    )

    # Global options: must appear before the subcommand.
    parser.add_argument("--config", type=Path, default=None)
    parser.add_argument("--astrometry-install", default=None)
    parser.add_argument("--profiling-root", default=None)
    parser.add_argument("--flamegraph-dir", default=None)

    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Enable detailed debug logging.",
    )
    parser.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        help="Show only warnings and errors.",
    )

    sub = parser.add_subparsers(
        dest="command",
        metavar="COMMAND",
        required=True,
    )

    #worker_suggest
    sub.add_parser(
    "suggest-workers",
    help="Suggest worker counts for official, aggressive, and stress benchmark runs.",
)
    # doctor
    sub.add_parser(
        "doctor",
        help="Check path discovery and required external tools.",
    )

    # stream
    p_stream = sub.add_parser(
    "stream",
    help="Read image paths from stdin and solve them as a parallel batch.",
)
    p_stream.add_argument("--workers", default="aggressive")
    p_stream.add_argument("--run-label", default="stream")
    p_stream.add_argument("--limit", type=int, default=None)

    # manifest
    p_manifest = sub.add_parser(
        "manifest",
        help="Generate a workload manifest.",
    )
    p_manifest.add_argument("--workload", default="mixed20")

    # run
    p_run = sub.add_parser(
        "run",
        help="Run one serial or parallel batch.",
    )
    p_run.add_argument("--workload", default=None)
    p_run.add_argument("--manifest", default=None)
    p_run.add_argument("--input-dir", type=Path, default=None)
    p_run.add_argument("--recursive", action="store_true")
    p_run.add_argument("--limit", type=int, default=None)
    p_run.add_argument("--workers", default="1")
    p_run.add_argument("--run-label", default=None)

    # benchmark
    p_benchmark = sub.add_parser(
        "benchmark",
        help="Run serial baseline and outer-parallel comparison.",
    )
    p_benchmark.add_argument("--workload", default="mixed20")
    p_benchmark.add_argument("--workers", type=int, required=True)

    # profile
    p_profile = sub.add_parser(
        "profile",
        help="Run profiling directly on a workload.",
    )
    p_profile.add_argument("--tool", choices=["perf-stat"], required=True)
    p_profile.add_argument("--workload", default=None)
    p_profile.add_argument("--manifest", default=None)
    p_profile.add_argument("--input-dir", type=Path, default=None)
    p_profile.add_argument("--recursive", action="store_true")
    p_profile.add_argument("--limit", type=int, default=None)
    p_profile.add_argument("--workers", default="1")
    p_profile.add_argument("--run-label", default=None)


    # vtune
    p_vtune = sub.add_parser(
        "vtune",
        help="Run Intel VTune profiling on a workload.",
    )
    p_vtune.add_argument("--analysis", choices=["hotspots", "threading", "hpc-performance"], required=True)
    p_vtune.add_argument("--workload", default=None)
    p_vtune.add_argument("--manifest", default=None)
    p_vtune.add_argument("--input-dir", type=Path, default=None)
    p_vtune.add_argument("--recursive", action="store_true")
    p_vtune.add_argument("--limit", type=int, default=None)
    p_vtune.add_argument("--workers", default="1")
    return parser


def cli_overrides_from_args(args: argparse.Namespace) -> dict[str, str | None]:
    return {
        "astrometry_install": getattr(args, "astrometry_install", None),
        "profiling_root": getattr(args, "profiling_root", None),
        "flamegraph_dir": getattr(args, "flamegraph_dir", None),
    }


def cmd_doctor(paths) -> int:
    print("Astrobench path discovery")
    print("=========================")
    print(f"repo_root:          {paths.repo_root}")
    print(f"astrometry_install: {paths.astrometry_install}")
    print(f"solve_field:        {paths.solve_field}")
    print(f"astrometry_cfg:     {paths.astrometry_cfg}")
    print(f"profiling_root:     {paths.profiling_root}")
    print(f"dataset_root:       {paths.dataset_root}")
    print(f"ser_root:           {paths.ser_root}")
    print(f"output_root:        {paths.output_root}")
    print(f"flamegraph_dir:     {paths.flamegraph_dir}")
    print("status:             OK")
    return 0


def cmd_manifest(args, config, paths) -> int:
    manifest_dir = paths.output_root / "_manifests"
    manifest_path = manifest_dir / f"{args.workload}.txt"

    categories = list(config.get("dataset", {}).get("categories", []))
    images = build_workload_manifest(paths.dataset_root, categories, args.workload, manifest_path)

    print(f"[OK] Wrote manifest: {manifest_path}")
    print(f"[OK] Image count: {len(images)}")
    return 0


def cmd_run(args, config, paths) -> int:
    workers = resolve_worker_value(args.workers)
    args.workers = str(workers)

    label = derive_run_label(args, "run")
    run_dir = new_run_dir(paths.output_root, f"{label}_w{workers}")
    run_dir.mkdir(parents=True, exist_ok=True)
    attach_run_log(run_dir / "astrobench.log")

    log.info("Starting run")
    log.info("Label: %s", label)
    log.info("Run directory: %s", run_dir)
    log.info("Workers: %s", workers)

    summary = execute_run(args, config, paths, run_dir)

    write_run_summary(summary, run_dir)
    log.info("Run complete: %s", run_dir)

    print(f"[OK] Run complete: {run_dir}")
    print(f"[OK] Results CSV: {run_dir / 'results.csv'}")
    print(f"[OK] Manifest: {run_dir / 'manifest.txt'}")
    return 0


def cmd_benchmark(args, config, paths) -> int:
    workers = resolve_worker_value(args.workers)
    args.workers = str(workers)

    label = derive_run_label(args, "benchmark")
    run_dir = new_run_dir(paths.output_root, f"{label}_w{workers}")

    run_dir.mkdir(parents=True, exist_ok=True)
    attach_run_log(run_dir / "astrobench.log")

    log.info("Starting benchmark")
    log.info("Workload label: %s", label)
    log.info("Parallel workers: %s", workers)
    log.info("Run directory: %s", run_dir)

    images = select_images_for_run(args, config, paths, run_dir)

    copy_text_snapshot(
        paths.repo_root / "examples" / "benchmark" / "astrobench.toml",
        run_dir / "config_snapshot.toml",
    )

    import platform
    (run_dir / "system_snapshot.json").write_text(
        f'{{"platform": "{platform.platform()}", "python": "{platform.python_version()}"}}\n',
        encoding="utf-8",
    )

    options = make_options(config, paths)

    serial_jobs = make_jobs(images, run_dir / "serial_w1")
    serial = SerialExecutor(options).run("serial_w1", serial_jobs)
    write_run_summary(serial, run_dir / "serial_w1")

    parallel_jobs = make_jobs(images, run_dir / f"parallel_w{workers}")
    parallel = OuterParallelExecutor(options, workers).run(f"parallel_w{workers}", parallel_jobs)
    write_run_summary(parallel, run_dir / f"parallel_w{workers}")

    compare_runs(serial, parallel, run_dir)

    print(f"[OK] Benchmark complete: {run_dir}")
    print(f"[OK] Comparison report: {run_dir / 'comparison.md'}")
    print(f"[OK] Manifest: {run_dir / 'manifest.txt'}")
    return 0


def cmd_profile(args, config, paths) -> int:
    run_dir = new_run_dir(paths.output_root, f"profile_{args.workload}_w{args.workers}_{args.tool}")
    run_dir.mkdir(parents=True, exist_ok=True)
    attach_run_log(run_dir / "astrobench.log")

    log.info("Starting profiling run")
    log.info("Tool: %s", args.tool)
    log.info("Workload: %s", args.workload)
    log.info("Workers: %s", args.workers)
    log.info("Run directory: %s", run_dir)

    command = [
        str(paths.repo_root / "bin" / "astrobench"),
        "run",
        "--workload", args.workload,
        "--workers", str(args.workers),
    ]

    if args.tool == "perf-stat":
        run_perf_stat(command, run_dir / "profiling")

    log.info("Profile complete: %s", run_dir)
    return 0


def cmd_vtune(args, config, paths) -> int:
    run_dir = new_run_dir(paths.output_root, f"vtune_{args.analysis}_{args.workload}_w{args.workers}")
    run_dir.mkdir(parents=True, exist_ok=True)
    attach_run_log(run_dir / "astrobench.log")

    result_dir = run_dir / "vtune_result"
    report_dir = run_dir / "reports"

    log.info("Starting VTune run")
    log.info("Analysis: %s", args.analysis)
    log.info("Workload: %s", args.workload)
    log.info("Workers: %s", args.workers)
    log.info("Result directory: %s", result_dir)

    command = [
        str(paths.repo_root / "bin" / "astrobench"),
        "run",
        "--workload", args.workload,
        "--workers", str(args.workers),
    ]

    vtune_env = config.get("vtune", {}).get("vtune_env")
    run_vtune_collect(args.analysis, command, result_dir, vtune_env)
    export_vtune_reports(result_dir, report_dir)

    log.info("VTune run complete: %s", run_dir)
    return 0


def execute_run(args, config, paths, run_dir: Path):
    images = select_images_for_run(args, config, paths, run_dir)

    workers = resolve_worker_value(getattr(args, "workers", 1))

    options = make_options(config, paths)
    jobs = make_jobs(images, run_dir)

    if workers == 1:
        return SerialExecutor(options).run("serial_w1", jobs)

    return OuterParallelExecutor(options, workers).run(f"parallel_w{workers}", jobs)

def cmd_stream(args, config, paths) -> int:
    workers = resolve_worker_value(args.workers)
    args.workers = str(workers)

    label = derive_run_label(args, "stream")
    run_dir = new_run_dir(paths.output_root, f"{label}_w{workers}")
    run_dir.mkdir(parents=True, exist_ok=True)
    attach_run_log(run_dir / "astrobench.log")

    log.info("Starting stream run")
    log.info("Workers: %s", workers)
    log.info("Run directory: %s", run_dir)

    images = read_manifest("-")

    if args.limit is not None:
        images = images[: args.limit]

    if not images:
        raise RuntimeError("Stream produced no images")

    manifest_path = run_dir / "manifest.txt"
    write_manifest(manifest_path, images)

    options = make_options(config, paths)
    jobs = make_jobs(images, run_dir)

    if workers == 1:
        summary = SerialExecutor(options).run("serial_w1", jobs)
    else:
        summary = OuterParallelExecutor(options, workers).run(f"parallel_w{workers}", jobs)

    write_run_summary(summary, run_dir)

    log.info("Stream run complete: %s", run_dir)
    print(f"[OK] Stream run complete: {run_dir}")
    print(f"[OK] Results CSV: {run_dir / 'results.csv'}")
    print(f"[OK] Manifest: {run_dir / 'manifest.txt'}")
    return 0

def make_options(config, paths) -> SolveOptions:
    solver = config.get("solver", {})

    return SolveOptions(
        solve_field_bin=paths.solve_field,
        astrometry_cfg=paths.astrometry_cfg,
        cpulimit_seconds=int(solver.get("cpulimit_seconds", 30)),
        timeout_seconds=int(solver.get("timeout_seconds", 45)),
        no_plots=bool(solver.get("no_plots", True)),
        overwrite=bool(solver.get("overwrite", True)),
        keep_temp=bool(solver.get("keep_temp", True)),
        optional_args=tuple(str(item) for item in solver.get("optional_args", [])),
    )

def new_run_dir(output_root: Path, label: str) -> Path:
    stamp = datetime.now().strftime("%Y-%m-%d_%H%M%S")
    return output_root / f"{stamp}_{label}"

def cmd_suggest_workers() -> int:
    suggestion = suggest_workers()
    log.info("Usable logical CPUs: %d", suggestion.usable_cpus)
    log.info("Official workers: %d", suggestion.official_workers)
    log.info("Aggressive workers: %d", suggestion.aggressive_workers)
    log.info("Stress workers: %d", suggestion.stress_workers)
    print(format_worker_suggestion(suggestion))
    return 0

def select_images_for_run(args, config, paths, run_dir: Path) -> list[Path]:
    manifest_path = run_dir / "manifest.txt"

    if getattr(args, "manifest", None):
        images = read_manifest(args.manifest)
        write_manifest(manifest_path, images)
        return images

    if getattr(args, "input_dir", None):
        return build_input_dir_manifest(
            input_dir=args.input_dir,
            output_manifest=manifest_path,
            recursive=getattr(args, "recursive", False),
            limit=getattr(args, "limit", None),
        )

    workload = (
        getattr(args, "workload", None)
        or config.get("benchmark", {}).get("default_workload", "mixed20")
    )

    categories = list(config.get("dataset", {}).get("categories", []))

    return build_workload_manifest(
        paths.dataset_root,
        categories,
        workload,
        manifest_path,
    )

def safe_label(text: str) -> str:
    cleaned = []

    for char in text:
        if char.isalnum() or char in {"-", "_"}:
            cleaned.append(char)
        else:
            cleaned.append("_")

    result = "".join(cleaned).strip("_")
    return result or "run"


def derive_run_label(args, default: str) -> str:
    if getattr(args, "run_label", None):
        return safe_label(args.run_label)

    if getattr(args, "workload", None):
        return safe_label(args.workload)

    if getattr(args, "input_dir", None):
        return safe_label(f"input_{args.input_dir.name}")

    if getattr(args, "manifest", None):
        if str(args.manifest) == "-":
            return "stream"
        return safe_label(f"manifest_{Path(args.manifest).stem}")

    return safe_label(default)


if __name__ == "__main__":
    raise SystemExit(main())
