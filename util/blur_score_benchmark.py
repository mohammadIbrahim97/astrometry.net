"""Benchmark blur_score over a directory or list of astronomical images.

Outputs CSV: backend,image,elapsed_s,status,fwhm,ellipticity,n_sources,score,error.

Usage:
    python -m astrometry.util.blur_score_benchmark \
        --images-dir /src/Astrometry-testing-data/data
    python -m astrometry.util.blur_score_benchmark \
        --paths img1.png img2.fits ...
    python -m astrometry.util.blur_score_benchmark \
        --images-dir /src/Astrometry-testing-data/data --backend sep --jobs 4
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import csv
import os
import sys
import time

from .blur_score import (
    DEFAULT_TARGET_FWHM,
    InsufficientSources,
    SOURCE_BACKENDS,
    measure_psf,
    score_from_psf,
)

DEFAULT_IMAGES_DIR = "/src/Astrometry-testing-data/data"
IMG_EXTS = (".fits", ".fit", ".fts", ".png", ".jpg", ".jpeg", ".tif", ".tiff")


def _walk_images(root):
    out = []
    for dirpath, _, files in os.walk(root):
        for fn in files:
            if fn.lower().endswith(IMG_EXTS):
                out.append(os.path.join(dirpath, fn))
    out.sort()
    return out


def _measure_one(task):
    backend, path, opts = task
    start = time.perf_counter()
    try:
        f, e, n = measure_psf(
            path,
            backend=backend,
            detect_thresh=opts["detect_thresh"],
            downsample=opts["downsample"],
            downsample_as_required=opts["downsample_as_required"],
            image2xy_path=opts["image2xy_path"],
        )
    except InsufficientSources as ex:
        elapsed = time.perf_counter() - start
        return {
            "backend": backend,
            "path": path,
            "elapsed": elapsed,
            "status": "insufficient_sources",
            "fwhm": "nan",
            "ellipticity": "nan",
            "n_sources": 0,
            "score": "nan",
            "error": str(ex),
        }
    except Exception as ex:
        elapsed = time.perf_counter() - start
        return {
            "backend": backend,
            "path": path,
            "elapsed": elapsed,
            "status": "error",
            "fwhm": "nan",
            "ellipticity": "nan",
            "n_sources": 0,
            "score": "nan",
            "error": str(ex),
        }

    elapsed = time.perf_counter() - start
    return {
        "backend": backend,
        "path": path,
        "elapsed": elapsed,
        "status": "ok",
        "fwhm": f"{f:.4f}",
        "ellipticity": f"{e:.4f}",
        "n_sources": n,
        "score": f"{score_from_psf(f, e, n, opts['target_fwhm']):.4f}",
        "error": "",
    }


def _write_result(writer, result):
    writer.writerow([
        result["backend"],
        result["path"],
        f"{result['elapsed']:.6f}",
        result["status"],
        result["fwhm"],
        result["ellipticity"],
        result["n_sources"],
        result["score"],
        result["error"],
    ])


def _run_backend(backend, files, opts, jobs, writer):
    summary = {"ok": 0, "insufficient": 0, "error": 0, "elapsed": 0.0}
    tasks = [(backend, path, opts) for path in files]
    start = time.perf_counter()

    if jobs == 1:
        results = map(_measure_one, tasks)
    else:
        pool = ThreadPoolExecutor(max_workers=jobs)
        results = pool.map(_measure_one, tasks)

    try:
        for result in results:
            if result["status"] == "ok":
                summary["ok"] += 1
            elif result["status"] == "insufficient_sources":
                summary["insufficient"] += 1
            else:
                summary["error"] += 1
            summary["elapsed"] += result["elapsed"]
            _write_result(writer, result)
    finally:
        if jobs != 1:
            pool.shutdown()

    summary["wall"] = time.perf_counter() - start
    return summary


def main(argv=None):
    p = argparse.ArgumentParser(prog="blur_score_benchmark")
    p.add_argument("--images-dir", default=DEFAULT_IMAGES_DIR)
    p.add_argument("--paths", nargs="*",
                   help="Specific image paths (overrides --images-dir walk)")
    p.add_argument("--backend", action="append",
                   choices=SOURCE_BACKENDS + ("all",),
                   help="Source extraction backend. Repeat to compare backends, or use 'all'.")
    p.add_argument("--detect-thresh", type=float, default=5.0)
    p.add_argument("--downsample", type=int, default=0)
    p.add_argument("--downsample-as-required", type=int, default=3)
    p.add_argument("--image2xy-path",
                   help="Path to image2xy for simplexy/sep backends.")
    p.add_argument("--max-images", type=int,
                   help="Limit the number of images for quick benchmark runs.")
    p.add_argument("--jobs", type=int, default=4,
                   help="Number of images to process in parallel per backend.")
    p.add_argument("--target-fwhm", type=float, default=DEFAULT_TARGET_FWHM)
    args = p.parse_args(argv)
    if args.jobs < 1:
        p.error("--jobs must be >= 1")

    backends = args.backend or ["source-extractor"]
    if "all" in backends:
        backends = list(SOURCE_BACKENDS)

    if args.paths:
        files = args.paths
    elif os.path.isdir(args.images_dir):
        files = _walk_images(args.images_dir)
    else:
        print(f"images-dir not found: {args.images_dir}", file=sys.stderr)
        return 2
    if args.max_images:
        files = files[:args.max_images]

    writer = csv.writer(sys.stdout)
    writer.writerow([
        "backend", "image", "elapsed_s", "status",
        "fwhm", "ellipticity", "n_sources", "score", "error"
    ])

    opts = {
        "detect_thresh": args.detect_thresh,
        "downsample": args.downsample,
        "downsample_as_required": args.downsample_as_required,
        "image2xy_path": args.image2xy_path,
        "target_fwhm": args.target_fwhm,
    }
    summary = {}
    for backend in backends:
        summary[backend] = _run_backend(backend, files, opts, args.jobs, writer)
        item = summary[backend]
        total = item["ok"] + item["insufficient"] + item["error"]
        print(
            "# summary "
            f"backend={backend} total={total} ok={item['ok']} "
            f"insufficient={item['insufficient']} error={item['error']} "
            f"elapsed_s={item['elapsed']:.3f} wall_s={item['wall']:.3f} "
            f"jobs={args.jobs}",
            file=sys.stderr
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
