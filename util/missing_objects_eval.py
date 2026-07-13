"""Evaluate solver tolerance to missing detected objects.

This utility has three subcommands:

  score   - compute blur scores for an image folder
  select  - choose a stratified sample by blur-score band
  run     - solve selected images and rerun with rows removed from .axy files

It injects the missing-object fault after source extraction by deleting rows
from the generated .axy source table. Original images are never modified.
"""
from __future__ import annotations

import argparse
import csv
import math
import os
import random
import re
import signal
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from astropy.io import fits

try:
    from .blur_score import (
        DEFAULT_TARGET_FWHM,
        InsufficientSources,
        measure_psf,
        score_from_psf,
    )
except ImportError:
    from astrometry.util.blur_score import (
        DEFAULT_TARGET_FWHM,
        InsufficientSources,
        measure_psf,
        score_from_psf,
    )


IMG_EXTS = (".fits", ".fit", ".fts", ".png", ".jpg", ".jpeg", ".tif", ".tiff")
DEFAULT_BANDS = (
    ("0.50-0.70", 0.50, 0.70, 10),
    ("0.70-0.85", 0.70, 0.85, 10),
    ("0.85-1.00", 0.85, 1.000000001, 10),
)
SCORE_FIELDS = ("image", "status", "fwhm", "ellipticity", "n_sources", "score", "error")
SELECTION_FIELDS = ("image", "band", "score", "fwhm", "ellipticity", "n_sources")
RESULT_FIELDS = (
    "image",
    "band",
    "score",
    "baseline_solved",
    "baseline_time_sec",
    "baseline_ra",
    "baseline_dec",
    "baseline_n_sources",
    "missing_fraction",
    "seed",
    "missing_count",
    "kept_count",
    "solved",
    "correct",
    "solve_time_sec",
    "center_ra",
    "center_dec",
    "center_sep_deg",
    "returncode",
    "output_dir",
    "error",
)


@dataclass(frozen=True)
class SolveResult:
    solved: bool
    elapsed_sec: float
    returncode: int
    ra: float
    dec: float
    output: str
    error: str


def iter_images(root: Path) -> list[Path]:
    paths: list[Path] = []
    for dirpath, _, filenames in os.walk(root):
        for filename in filenames:
            if filename.lower().endswith(IMG_EXTS):
                paths.append(Path(dirpath) / filename)
    return sorted(paths)


def safe_float(value: str) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return math.nan


def _score_one(payload: tuple[str, float]) -> dict[str, str]:
    image, target_fwhm = payload
    try:
        fwhm, ecc, n_sources = measure_psf(image)
        score = score_from_psf(fwhm, ecc, n_sources, target_fwhm)
        return {
            "image": image,
            "status": "ok",
            "fwhm": f"{fwhm:.6f}",
            "ellipticity": f"{ecc:.6f}",
            "n_sources": str(n_sources),
            "score": f"{score:.6f}",
            "error": "",
        }
    except InsufficientSources as exc:
        return {
            "image": image,
            "status": "insufficient_sources",
            "fwhm": "nan",
            "ellipticity": "nan",
            "n_sources": "0",
            "score": "nan",
            "error": str(exc),
        }
    except Exception as exc:
        return {
            "image": image,
            "status": "error",
            "fwhm": "nan",
            "ellipticity": "nan",
            "n_sources": "0",
            "score": "nan",
            "error": str(exc),
        }


def command_score(args: argparse.Namespace) -> int:
    images = iter_images(Path(args.images_dir))
    if args.limit:
        images = images[: args.limit]
    if not images:
        print(f"No images found under {args.images_dir}", file=sys.stderr)
        return 2

    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    payloads = [(str(path), args.target_fwhm) for path in images]
    with open(args.output, "w", newline="") as out:
        writer = csv.DictWriter(out, fieldnames=SCORE_FIELDS)
        writer.writeheader()
        if args.workers == 1:
            iterator = map(_score_one, payloads)
        else:
            pool = ProcessPoolExecutor(max_workers=args.workers)
            iterator = pool.map(_score_one, payloads)
        try:
            for i, row in enumerate(iterator, 1):
                writer.writerow(row)
                if i % args.progress_every == 0:
                    print(f"scored {i}/{len(images)} images", file=sys.stderr, flush=True)
        finally:
            if args.workers != 1:
                pool.shutdown(wait=True)
    print(f"Wrote blur scores: {args.output}")
    return 0


def in_band(score: float, low: float, high: float) -> bool:
    return low <= score < high


def command_select(args: argparse.Namespace) -> int:
    rng = random.Random(args.seed)
    rows_by_band: dict[str, list[dict[str, str]]] = {name: [] for name, _, _, _ in DEFAULT_BANDS}
    with open(args.scores, newline="") as f:
        for row in csv.DictReader(f):
            if row.get("status") != "ok":
                continue
            score = safe_float(row.get("score", "nan"))
            if not math.isfinite(score):
                continue
            for name, low, high, _ in DEFAULT_BANDS:
                if in_band(score, low, high):
                    rows_by_band[name].append(row)
                    break

    selected: list[dict[str, str]] = []
    for name, _, _, count in DEFAULT_BANDS:
        rows = rows_by_band[name]
        rng.shuffle(rows)
        chosen = rows[:count]
        print(f"{name}: selected {len(chosen)} of {len(rows)} candidates")
        for row in chosen:
            selected.append({
                "image": row["image"],
                "band": name,
                "score": row["score"],
                "fwhm": row["fwhm"],
                "ellipticity": row["ellipticity"],
                "n_sources": row["n_sources"],
            })

    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    with open(args.output, "w", newline="") as out:
        writer = csv.DictWriter(out, fieldnames=SELECTION_FIELDS)
        writer.writeheader()
        writer.writerows(selected)
    print(f"Wrote selected sample: {args.output}")
    return 0


def slugify_image(path: str, index: int) -> str:
    stem = Path(path).stem
    slug = re.sub(r"[^A-Za-z0-9._-]+", "-", stem).strip("-")
    if not slug:
        slug = "image"
    return f"{index:03d}-{slug}"


def parse_center(output: str) -> tuple[float, float]:
    match = re.search(r"Field center:\s+\(RA,Dec\)\s+=\s+\(([-+0-9.eE]+),\s*([-+0-9.eE]+)\)", output)
    if not match:
        return math.nan, math.nan
    return float(match.group(1)), float(match.group(2))


def angular_sep_deg(ra1: float, dec1: float, ra2: float, dec2: float) -> float:
    if not all(math.isfinite(v) for v in (ra1, dec1, ra2, dec2)):
        return math.nan
    r1 = math.radians(ra1)
    d1 = math.radians(dec1)
    r2 = math.radians(ra2)
    d2 = math.radians(dec2)
    cos_sep = math.sin(d1) * math.sin(d2) + math.cos(d1) * math.cos(d2) * math.cos(r1 - r2)
    cos_sep = max(-1.0, min(1.0, cos_sep))
    return math.degrees(math.acos(cos_sep))


def run_solve(
    solve_field: str,
    config: str,
    input_path: Path,
    outdir: Path,
    cpulimit: int,
    wall_timeout: float,
    outbase: str = "solution",
) -> SolveResult:
    outdir.mkdir(parents=True, exist_ok=True)
    cmd = [
        solve_field,
        "--config",
        config,
        "--dir",
        str(outdir),
        "--out",
        outbase,
        "--overwrite",
        "--no-plots",
        "--new-fits",
        "none",
        "--corr",
        "none",
        "--rdls",
        "none",
        "--match",
        "none",
        "--index-xyls",
        "none",
        "--cpulimit",
        str(cpulimit),
        str(input_path),
    ]
    start = time.perf_counter()
    proc = subprocess.Popen(
        cmd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        preexec_fn=os.setsid,
    )
    timed_out = False
    try:
        stdout, stderr = proc.communicate(timeout=wall_timeout if wall_timeout > 0 else None)
    except subprocess.TimeoutExpired:
        timed_out = True
        os.killpg(proc.pid, signal.SIGTERM)
        try:
            stdout, stderr = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            stdout, stderr = proc.communicate()
    elapsed = time.perf_counter() - start
    combined = (stdout or "") + (stderr or "")
    solved = (outdir / f"{outbase}.solved").exists() and (outdir / f"{outbase}.wcs").exists()
    ra, dec = parse_center(combined)
    err = ""
    if timed_out:
        err = f"wall timeout after {wall_timeout:.1f}s"
    elif proc.returncode != 0:
        err = combined.strip().splitlines()[-1] if combined.strip() else f"returncode={proc.returncode}"
    elif not solved:
        err = "not solved"
    return SolveResult(solved, elapsed, proc.returncode, ra, dec, combined, err)


def write_reduced_axy(source_axy: Path, dest_axy: Path, missing_fraction: float, seed: int) -> tuple[int, int, int]:
    with fits.open(source_axy) as hdul:
        table = hdul[1].data
        n_rows = len(table)
        missing_count = int(round(n_rows * missing_fraction))
        missing_count = max(0, min(missing_count, n_rows))
        if missing_count:
            rng = np.random.default_rng(seed)
            remove = set(int(i) for i in rng.choice(n_rows, size=missing_count, replace=False))
            keep = np.array([i not in remove for i in range(n_rows)], dtype=bool)
            reduced = table[keep]
        else:
            reduced = table

        new_hdus = fits.HDUList([hdul[0].copy()])
        new_hdus.append(fits.BinTableHDU(data=reduced, header=hdul[1].header.copy(), name=hdul[1].name))
        dest_axy.parent.mkdir(parents=True, exist_ok=True)
        new_hdus.writeto(dest_axy, overwrite=True)
    return n_rows, missing_count, n_rows - missing_count


def command_run(args: argparse.Namespace) -> int:
    fractions = [float(x) for x in args.fractions.split(",") if x.strip()]
    root = Path(args.output_dir)
    root.mkdir(parents=True, exist_ok=True)

    with open(args.selection, newline="") as f:
        selected = list(csv.DictReader(f))
    if args.limit:
        selected = selected[: args.limit]

    results_path = root / "missing_objects_results.csv"
    with open(results_path, "w", newline="") as out:
        writer = csv.DictWriter(out, fieldnames=RESULT_FIELDS)
        writer.writeheader()
        for image_index, row in enumerate(selected, 1):
            image = row["image"]
            slug = slugify_image(image, image_index)
            image_root = root / slug

            baseline = run_solve(
                args.solve_field,
                args.config,
                Path(image),
                image_root / "baseline",
                args.cpulimit,
                args.wall_timeout,
            )
            baseline_axy = image_root / "baseline" / "solution.axy"
            baseline_n_sources = 0
            if baseline_axy.exists():
                with fits.open(baseline_axy) as hdul:
                    baseline_n_sources = len(hdul[1].data)

            if not baseline.solved or not baseline_axy.exists():
                writer.writerow({
                    "image": image,
                    "band": row["band"],
                    "score": row["score"],
                    "baseline_solved": str(baseline.solved),
                    "baseline_time_sec": f"{baseline.elapsed_sec:.3f}",
                    "baseline_ra": f"{baseline.ra:.8f}" if math.isfinite(baseline.ra) else "nan",
                    "baseline_dec": f"{baseline.dec:.8f}" if math.isfinite(baseline.dec) else "nan",
                    "baseline_n_sources": str(baseline_n_sources),
                    "missing_fraction": "",
                    "seed": "",
                    "missing_count": "",
                    "kept_count": "",
                    "solved": "False",
                    "correct": "False",
                    "solve_time_sec": "",
                    "center_ra": "nan",
                    "center_dec": "nan",
                    "center_sep_deg": "nan",
                    "returncode": str(baseline.returncode),
                    "output_dir": str(image_root / "baseline"),
                    "error": baseline.error,
                })
                out.flush()
                print(f"[{image_index}/{len(selected)}] baseline failed: {image}")
                continue

            trial_specs: list[tuple[float, int]] = []
            for fraction in fractions:
                if fraction == 0:
                    trial_specs.append((fraction, 0))
                else:
                    for seed_offset in range(args.seeds):
                        trial_specs.append((fraction, args.seed + seed_offset))

            for missing_fraction, seed in trial_specs:
                trial_name = f"missing-{int(round(missing_fraction * 100)):03d}-seed-{seed:04d}"
                trial_dir = image_root / trial_name
                trial_axy = trial_dir / "input.axy"
                n_rows, missing_count, kept_count = write_reduced_axy(
                    baseline_axy,
                    trial_axy,
                    missing_fraction,
                    seed,
                )
                result = run_solve(
                    args.solve_field,
                    args.config,
                    trial_axy,
                    trial_dir,
                    args.cpulimit,
                    args.wall_timeout,
                )
                sep = angular_sep_deg(baseline.ra, baseline.dec, result.ra, result.dec)
                correct = result.solved and math.isfinite(sep) and sep <= args.max_center_sep_deg
                writer.writerow({
                    "image": image,
                    "band": row["band"],
                    "score": row["score"],
                    "baseline_solved": "True",
                    "baseline_time_sec": f"{baseline.elapsed_sec:.3f}",
                    "baseline_ra": f"{baseline.ra:.8f}",
                    "baseline_dec": f"{baseline.dec:.8f}",
                    "baseline_n_sources": str(n_rows),
                    "missing_fraction": f"{missing_fraction:.3f}",
                    "seed": str(seed),
                    "missing_count": str(missing_count),
                    "kept_count": str(kept_count),
                    "solved": str(result.solved),
                    "correct": str(correct),
                    "solve_time_sec": f"{result.elapsed_sec:.3f}",
                    "center_ra": f"{result.ra:.8f}" if math.isfinite(result.ra) else "nan",
                    "center_dec": f"{result.dec:.8f}" if math.isfinite(result.dec) else "nan",
                    "center_sep_deg": f"{sep:.8f}" if math.isfinite(sep) else "nan",
                    "returncode": str(result.returncode),
                    "output_dir": str(trial_dir),
                    "error": result.error,
                })
                out.flush()
            print(f"[{image_index}/{len(selected)}] completed: {image}")
    print(f"Wrote missing-object results: {results_path}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="missing_objects_eval")
    subparsers = parser.add_subparsers(dest="command", required=True)

    score = subparsers.add_parser("score", help="compute blur scores for an image folder")
    score.add_argument("--images-dir", required=True)
    score.add_argument("--output", required=True)
    score.add_argument("--target-fwhm", type=float, default=DEFAULT_TARGET_FWHM)
    score.add_argument("--workers", type=int, default=max(1, min(4, os.cpu_count() or 1)))
    score.add_argument("--limit", type=int, default=0)
    score.add_argument("--progress-every", type=int, default=100)
    score.set_defaults(func=command_score)

    select = subparsers.add_parser("select", help="choose 10 images from each blur-score band")
    select.add_argument("--scores", required=True)
    select.add_argument("--output", required=True)
    select.add_argument("--seed", type=int, default=42)
    select.set_defaults(func=command_select)

    run = subparsers.add_parser("run", help="run baseline and missing-object solve trials")
    run.add_argument("--selection", required=True)
    run.add_argument("--output-dir", required=True)
    run.add_argument("--solve-field", default="/usr/local/astrometry/bin/solve-field")
    run.add_argument("--config", default="/usr/local/astrometry/etc/astrometry.cfg")
    run.add_argument("--cpulimit", type=int, default=60)
    run.add_argument("--wall-timeout", type=float, default=0.0)
    run.add_argument("--fractions", default="0,0.1,0.2,0.3,0.4,0.5,0.6,0.7,0.8")
    run.add_argument("--seeds", type=int, default=5)
    run.add_argument("--seed", type=int, default=42)
    run.add_argument("--max-center-sep-deg", type=float, default=0.1)
    run.add_argument("--limit", type=int, default=0)
    run.set_defaults(func=command_run)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
