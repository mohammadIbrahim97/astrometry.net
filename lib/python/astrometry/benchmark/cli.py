from __future__ import annotations

import argparse
import os
import platform
import sys
from datetime import datetime
from pathlib import Path

from .compare import compare_runs
from .config import load_config, local_config_path
from .executor import OuterParallelExecutor, SerialExecutor
from .logging_utils import attach_run_log, configure_logging, get_logger
from .manifest import (
    build_input_dir_manifest,
    build_workload_manifest,
    make_jobs,
    read_manifest,
    write_manifest,
)
from .model import SolveOptions
from .pathscan import PathResolutionError, resolve_project_paths, find_repo_root
from .recorder import copy_text_snapshot, write_run_summary
from .settings import (
    get_config_value,
    init_local_config,
    parse_config_value,
    read_local_config,
    set_config_value,
    write_local_config,
)
from .status import print_inspect, print_status, resolve_run_dir

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

    # fastpath: no project paths req
    if args.command == "suggest-workers":
        return cmd_suggest_workers()

    # root context: entrypoint env -> imported module path -> cwd fallback
    try:
        repo_root = resolve_cli_repo_root()
    except PathResolutionError as error:
        print(f"[ERROR] {error}", file=sys.stderr)
        return 2

    # local env init routes
    if args.command == "init":
        return cmd_init(args, repo_root)

    if args.command == "config":
        return cmd_config(args, repo_root)

    # load cfg + strict path res for execution cmds
    try:
        config = load_config(args.config, repo_root)
        paths = resolve_project_paths(config, cli_overrides_from_args(args), start=repo_root)
    except (OSError, PathResolutionError, ValueError) as error:
        print(f"[ERROR] {error}", file=sys.stderr)
        return 2

    # dispatch exe modes
    if args.command == "doctor":
        return cmd_doctor(paths)

    if args.command == "manifest":
        return cmd_manifest(args, config, paths)

    if args.command == "run":
        return cmd_run(args, config, paths)

    # legacy compat routing
    if args.command in {"compare", "benchmark"}:
        return cmd_compare(args, config, paths)

    if args.command == "status":
        return cmd_status(args, paths)

    if args.command == "inspect":
        return cmd_inspect(args, paths)

    parser.print_help()
    return 1


def resolve_cli_repo_root() -> Path:
    env_root = os.environ.get("ASTROBENCH_REPO_ROOT")
    if env_root:
        return find_repo_root(Path(env_root))

    try:
        return find_repo_root(Path(__file__).resolve())
    except PathResolutionError:
        return find_repo_root(Path.cwd())


def build_parser() -> argparse.ArgumentParser:
    # init main argparse tree
    parser = argparse.ArgumentParser(
        prog="astrobench",
        description="Astrometry.net concurrent solve-field batch controller.",
    )

    # global opts
    parser.add_argument("--config", type=Path, default=None)
    parser.add_argument("--astrometry-install", default=None)
    parser.add_argument("--workspace-root", default=None)

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

    # subcmds gen
    sub = parser.add_subparsers(
        dest="command",
        metavar="COMMAND",
        required=True,
    )

    # aux commands tree
    p_init = sub.add_parser(
        "init",
        help="Create local .astrobench.toml configuration.",
    )
    p_init.add_argument("--force", action="store_true")

    p_config = sub.add_parser(
        "config",
        help="Read or update local Astrobench configuration.",
    )
    config_sub = p_config.add_subparsers(dest="config_action", required=True)

    config_sub.add_parser(
        "show",
        help="Show local .astrobench.toml configuration.",
    )

    p_config_get = config_sub.add_parser(
        "get",
        help="Get one local config value.",
    )
    p_config_get.add_argument("key")

    p_config_set = config_sub.add_parser(
        "set",
        help="Set one local config value.",
    )
    p_config_set.add_argument("key")
    p_config_set.add_argument("value")

    sub.add_parser(
        "suggest-workers",
        help="Suggest worker counts for official, aggressive, and stress runs.",
    )

    sub.add_parser(
        "doctor",
        help="Check path discovery and required external files.",
    )

    # wkld manifest routes
    p_manifest = sub.add_parser(
        "manifest",
        help="Generate a controlled workload manifest.",
    )
    p_manifest.add_argument("--workload", default="mixed20")

    # core execution block w shared arg sigs
    add_run_like_arguments(
        sub.add_parser(
            "run",
            help="Run one serial or concurrent solve-field batch.",
        ),
        default_workers="1",
    )

    add_run_like_arguments(
        sub.add_parser(
            "compare",
            help="Compare serial execution against concurrent execution.",
        ),
        default_workers="official",
    )

    add_run_like_arguments(
        sub.add_parser(
            "benchmark",
            help="Alias for compare.",
        ),
        default_workers="official",
    )

    p_status = sub.add_parser(
        "status",
        help="Show summary for a run directory.",
    )
    p_status.add_argument("--run", default="latest")

    p_inspect = sub.add_parser(
        "inspect",
        help="Show failures and log paths for a run directory.",
    )
    p_inspect.add_argument("--run", default="latest")

    return parser


def add_run_like_arguments(parser: argparse.ArgumentParser, *, default_workers: str) -> None:
    parser.add_argument("--workload", default=None)
    parser.add_argument("--manifest", default=None)
    parser.add_argument("--input-dir", type=Path, default=None)
    parser.add_argument("--recursive", action="store_true")
    parser.add_argument("--limit", type=int, default=None)
    parser.add_argument("--workers", default=default_workers)
    parser.add_argument("--run-label", default=None)


def cli_overrides_from_args(args: argparse.Namespace) -> dict[str, str | None]:
    return {
        "astrometry_install": getattr(args, "astrometry_install", None),
        "workspace_root": getattr(args, "workspace_root", None),
    }


def cmd_init(args: argparse.Namespace, repo_root: Path) -> int:
    path = init_local_config(repo_root, force=args.force)
    print(f"[OK] Local config: {path}")
    return 0


def cmd_config(args: argparse.Namespace, repo_root: Path) -> int:
    # check fs for local toml overriding
    path = local_config_path(repo_root)

    if args.config_action == "show":
        if not path.is_file():
            print(f"[INFO] No local config found: {path}")
            print("[HINT] Run: ./bin/astrobench init")
            return 0
        print(path.read_text(encoding="utf-8"), end="")
        return 0

    config = read_local_config(repo_root)

    # kv io ops
    if args.config_action == "get":
        try:
            value = get_config_value(config, args.key)
        except KeyError:
            print(f"[ERROR] Config key not found: {args.key}", file=sys.stderr)
            return 2
        print(value)
        return 0

    if args.config_action == "set":
        value = parse_config_value(args.value)
        set_config_value(config, args.key, value)
        write_local_config(repo_root, config)
        print(f"[OK] {args.key} = {value}")
        print(f"[OK] Updated: {path}")
        return 0

    return 1


def cmd_suggest_workers() -> int:
    suggestion = suggest_workers()

    log.info("Usable logical CPUs: %d", suggestion.usable_cpus)
    log.info("Official workers: %d", suggestion.official_workers)
    log.info("Aggressive workers: %d", suggestion.aggressive_workers)
    log.info("Stress workers: %d", suggestion.stress_workers)

    print(format_worker_suggestion(suggestion))
    return 0


def cmd_doctor(paths) -> int:
    # debug tree dump
    print("Astrobench path discovery")
    print("=========================")
    print(f"repo_root:          {paths.repo_root}")
    print(f"astrometry_install: {paths.astrometry_install}")
    print(f"solve_field:        {paths.solve_field}")
    print(f"astrometry_cfg:     {paths.astrometry_cfg}")
    print(f"workspace_root:     {paths.workspace_root}")
    print(f"dataset_root:       {paths.dataset_root}")
    print(f"ser_root:           {paths.ser_root}")
    print(f"output_root:        {paths.output_root}")
    print("status:             OK")
    return 0


def cmd_manifest(args, config, paths) -> int:
    # precompute subset n freeze
    manifest_dir = paths.output_root / "_manifests"
    manifest_path = manifest_dir / f"{args.workload}.txt"

    categories = list(config.get("dataset", {}).get("categories", []))
    images = build_workload_manifest(paths.dataset_root, categories, args.workload, manifest_path)

    print(f"[OK] Wrote manifest: {manifest_path}")
    print(f"[OK] Image count: {len(images)}")
    return 0


def cmd_run(args, config, paths) -> int:
    # exec lifecycle sync
    workers = resolve_worker_value(args.workers)
    args.workers = str(workers)

    label = derive_run_label(args, config, "run")
    run_dir = new_run_dir(paths.output_root, f"{label}_w{workers}")

    # boot telemetry
    run_dir.mkdir(parents=True, exist_ok=True)
    attach_run_log(run_dir / "astrobench.log")

    log.info("Starting run")
    log.info("Label: %s", label)
    log.info("Run directory: %s", run_dir)
    log.info("Workers: %s", workers)

    # delegate payload n sync metrics
    summary = execute_run(args, config, paths, run_dir)
    write_run_summary(summary, run_dir)

    log.info("Run complete: %s", run_dir)

    print(f"[OK] Run complete: {run_dir}")
    print(f"[OK] Results CSV: {run_dir / 'results.csv'}")
    print(f"[OK] Manifest: {run_dir / 'manifest.txt'}")
    return 0


def cmd_compare(args, config, paths) -> int:
    # diff exe bounds check
    workers = resolve_worker_value(args.workers)

    if workers < 2:
        print("[ERROR] compare requires at least 2 workers for the parallel run.", file=sys.stderr)
        print("[HINT] Use --workers official, --workers aggressive, or --workers 2.", file=sys.stderr)
        return 2

    args.workers = str(workers)

    label = derive_run_label(args, config, "compare")
    run_dir = new_run_dir(paths.output_root, f"compare_{label}_w{workers}")

    run_dir.mkdir(parents=True, exist_ok=True)
    attach_run_log(run_dir / "astrobench.log")

    log.info("Starting comparison")
    log.info("Label: %s", label)
    log.info("Parallel workers: %s", workers)
    log.info("Run directory: %s", run_dir)

    # lock inputs for strict ser vs par comparison
    images = select_images_for_run(args, config, paths, run_dir)

    copy_text_snapshot(
        paths.repo_root / "examples" / "benchmark" / "astrobench.toml",
        run_dir / "config_snapshot.toml",
    )

    (run_dir / "system_snapshot.json").write_text(
        f'{{"platform": "{platform.platform()}", "python": "{platform.python_version()}"}}\n',
        encoding="utf-8",
    )

    options = make_options(config, paths)

    # ser baseline pass w=1
    serial_jobs = make_jobs(images, run_dir / "serial_w1")
    serial = SerialExecutor(options).run("serial_w1", serial_jobs)
    write_run_summary(serial, run_dir / "serial_w1")

    # par test pass w=N
    parallel_jobs = make_jobs(images, run_dir / f"parallel_w{workers}")
    parallel = OuterParallelExecutor(options, workers).run(f"parallel_w{workers}", parallel_jobs)
    write_run_summary(parallel, run_dir / f"parallel_w{workers}")

    # calc overhead n emit md diff
    compare_runs(serial, parallel, run_dir)

    print(f"[OK] Comparison complete: {run_dir}")
    print(f"[OK] Comparison report: {run_dir / 'comparison.md'}")
    print(f"[OK] Manifest: {run_dir / 'manifest.txt'}")
    return 0


def cmd_status(args, paths) -> int:
    run_dir = resolve_run_dir(paths.output_root, args.run)
    print_status(run_dir)
    return 0


def cmd_inspect(args, paths) -> int:
    run_dir = resolve_run_dir(paths.output_root, args.run)
    print_inspect(run_dir)
    return 0


def execute_run(args, config, paths, run_dir: Path):
    # dynamic dispatcher mapping payload pool to exe strat
    images = select_images_for_run(args, config, paths, run_dir)
    workers = resolve_worker_value(getattr(args, "workers", 1))

    options = make_options(config, paths)
    jobs = make_jobs(images, run_dir)

    if workers == 1:
        return SerialExecutor(options).run("serial_w1", jobs)

    return OuterParallelExecutor(options, workers).run(f"parallel_w{workers}", jobs)


def select_images_for_run(args, config, paths, run_dir: Path) -> list[Path]:
    manifest_path = run_dir / "manifest.txt"

    # explicit manifest override highest prio
    if getattr(args, "manifest", None):
        images = read_manifest(args.manifest)

        if getattr(args, "limit", None) is not None:
            images = images[: args.limit]

        write_manifest(manifest_path, images)
        return images

    # dynamic dir scan next prio
    if getattr(args, "input_dir", None):
        return build_input_dir_manifest(
            input_dir=args.input_dir,
            output_manifest=manifest_path,
            recursive=getattr(args, "recursive", False),
            limit=getattr(args, "limit", None),
        )

    # fallback map to config defaults
    default_input = config.get("input", {}).get("default_dir")
    if default_input:
        return build_input_dir_manifest(
            input_dir=Path(str(default_input)).expanduser(),
            output_manifest=manifest_path,
            recursive=bool(config.get("input", {}).get("recursive", False)),
            limit=int(config.get("input", {}).get("limit", 0) or 0) or None,
        )

    # legacy compat route
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


def make_options(config, paths) -> SolveOptions:
    # cast cfg dict to strict pydantic model constraints
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
    # alloc isolated fs block
    stamp = datetime.now().strftime("%Y-%m-%d_%H%M%S")
    return output_root / f"{stamp}_{label}"


def safe_label(text: str) -> str:
    # fs sanitization iter
    cleaned = []

    for char in text:
        if char.isalnum() or char in {"-", "_"}:
            cleaned.append(char)
        else:
            cleaned.append("_")

    result = "".join(cleaned).strip("_")
    return result or "run"


def derive_run_label(args, config: dict, default: str) -> str:
    # heuristic trace for semantic dir names
    if getattr(args, "run_label", None):
        return safe_label(args.run_label)

    if getattr(args, "workload", None):
        return safe_label(args.workload)

    if getattr(args, "input_dir", None):
        return safe_label(f"input_{args.input_dir.name}")

    if getattr(args, "manifest", None):
        if str(args.manifest) == "-":
            return "stdin_manifest"
        return safe_label(f"manifest_{Path(args.manifest).stem}")

    default_input = config.get("input", {}).get("default_dir")
    if default_input:
        return safe_label(f"input_{Path(str(default_input)).name}")

    return safe_label(default)


if __name__ == "__main__":
    raise SystemExit(main())
