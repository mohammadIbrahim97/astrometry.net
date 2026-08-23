from __future__ import annotations

import json
from pathlib import Path

# read-only run inspection, no mutation
def resolve_run_dir(output_root: Path, run: str) -> Path:
    # latest keyword or explicit fs path/name
    if run == "latest":
        return latest_run_dir(output_root)

    path = Path(run).expanduser()
    if path.is_dir():
        return path.resolve()

    candidate = output_root / run
    if candidate.is_dir():
        return candidate.resolve()

    raise FileNotFoundError(f"Run directory not found: {run}")


def latest_run_dir(output_root: Path) -> Path:
    if not output_root.is_dir():
        raise FileNotFoundError(f"Astrobench output root not found: {output_root}")

    # ignore helper dirs like _manifests
    candidates = [
        path
        for path in output_root.iterdir()
        if path.is_dir()
        and not path.name.startswith("_")
        and ((path / "run.json").is_file() or (path / "comparison.md").is_file())
    ]

    if not candidates:
        raise FileNotFoundError(f"No completed Astrobench runs found under {output_root}")

    return sorted(candidates, key=lambda path: path.name)[-1]


def print_status(run_dir: Path) -> None:
    print(f"Run directory: {run_dir}")

    # run.json = normal run, comparison.md = compare root
    run_json = run_dir / "run.json"
    comparison_md = run_dir / "comparison.md"

    if run_json.is_file():
        print_single_run_status(read_json(run_json), run_json.parent)
        return

    if comparison_md.is_file():
        print_comparison_status(run_dir)
        return

    print("Type: unknown/incomplete")
    print("No run.json or comparison.md found.")


def print_inspect(run_dir: Path) -> None:
    print_status(run_dir)
    print()
    print("Failures")
    print("========")

    # compare roots contain child run.json files
    run_json_files = collect_run_json_files(run_dir)
    any_failure = False

    for run_json in run_json_files:
        payload = read_json(run_json)

        # solved=false drives inspect, no stderr parsing
        failed = [
            item
            for item in payload.get("results", [])
            if not item.get("solved")
        ]

        if not failed:
            continue

        any_failure = True
        print()
        print(f"{run_json.parent.name}: {len(failed)} failure(s)")

        for item in failed:
            print(
                f"  {item.get('job_id')} "
                f"rc={item.get('return_code')} "
                f"error={item.get('error_kind') or '-'} "
                f"stderr={item.get('stderr_log')}"
            )

    if not any_failure:
        print("No failed jobs recorded.")


def print_comparison_status(run_dir: Path) -> None:
    print("Type: comparison")
    print(f"Comparison report: {run_dir / 'comparison.md'}")

    # child summaries, serial + parallel
    comparison_csv = run_dir / "comparison.csv"
    if comparison_csv.is_file():
        print(f"Comparison CSV: {comparison_csv}")

    for child_run_json in sorted(run_dir.glob("*/run.json")):
        print()
        print(f"[{child_run_json.parent.name}]")
        print_single_run_status(read_json(child_run_json), child_run_json.parent)


def print_single_run_status(payload: dict, run_dir: Path) -> None:
    print(f"Type: {payload.get('mode', 'run')}")
    print(f"Run name: {payload.get('run_name')}")
    print(f"Workers: {payload.get('workers')}")
    print(f"Jobs: {payload.get('job_count', len(payload.get('results', [])))}")
    print(f"Success: {payload.get('success_count')}")
    print(f"Failure: {payload.get('failure_count')}")
    print(f"Elapsed [s]: {float(payload.get('elapsed_seconds', 0.0)):.3f}")

    results_csv = run_dir / "results.csv"
    if results_csv.is_file():
        print(f"Results CSV: {results_csv}")


def collect_run_json_files(run_dir: Path) -> list[Path]:
    # direct run first
    if (run_dir / "run.json").is_file():
        return [run_dir / "run.json"]

    return sorted(run_dir.glob("*/run.json"))


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))
