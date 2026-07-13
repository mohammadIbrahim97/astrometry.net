"""Runnable check: `python3 -m astrometry.util.test_blur_score_benchmark`.

Verifies _run_all pools every (backend x image) task together and runs
backends concurrently, without needing source-extractor/image2xy installed.
"""
import csv
import io
import threading
import time

from . import blur_score_benchmark as bench


def _run(backends, files, jobs):
    live = [0]
    peak = [0]
    lock = threading.Lock()

    def fake_measure_one(task):
        backend, path, _opts = task
        with lock:
            live[0] += 1
            peak[0] = max(peak[0], live[0])
        time.sleep(0.05)  # force overlap so concurrency is observable
        with lock:
            live[0] -= 1
        return {"backend": backend, "path": path, "elapsed": 0.05,
                "status": "ok", "fwhm": "3.0", "ellipticity": "0.1",
                "n_sources": 12, "score": "0.9", "error": ""}

    orig = bench._measure_one
    bench._measure_one = fake_measure_one
    try:
        buf = io.StringIO()
        summary, wall = bench._run_all(backends, files, {}, jobs, csv.writer(buf))
    finally:
        bench._measure_one = orig
    rows = list(csv.reader(io.StringIO(buf.getvalue())))
    return summary, wall, peak[0], rows


def main():
    backends = ["source-extractor", "simplexy", "sep"]

    # 3 backends x 1 image, pool of 4 -> all 3 run at once.
    summary, _wall, peak, rows = _run(backends, ["/img.png"], jobs=4)
    assert len(rows) == 3, rows
    assert peak == 3, f"expected all 3 backends concurrent, peak={peak}"
    assert all(summary[b]["ok"] == 1 for b in backends), summary

    # jobs=2 caps concurrency even with 3 tasks.
    _s, _w, peak2, _r = _run(backends, ["/img.png"], jobs=2)
    assert peak2 == 2, f"expected pool cap of 2, peak={peak2}"

    # jobs=1 stays sequential.
    _s, _w, peak1, rows1 = _run(backends, ["/img.png"], jobs=1)
    assert peak1 == 1, f"expected sequential, peak={peak1}"
    assert len(rows1) == 3

    # per-backend aggregation over multiple images.
    summary, _w, _p, rows = _run(["sep"], ["/a.png", "/b.png"], jobs=2)
    assert summary["sep"]["ok"] == 2, summary
    assert len(rows) == 2

    print("ok: _run_all pools backends concurrently and aggregates per backend")


if __name__ == "__main__":
    main()
