"""Blur score for astronomical images via stellar PSF measurement.

Runs source-extractor to detect stars, takes the median FWHM and
ellipticity of the brightest detections, and maps to a [0, 1] score
(1 = sharp pinpoint stars, 0 = severely blurred).

The score is FWHM-driven (penalizing wide PSFs from defocus / poor
seeing), down-weighted by ellipticity (penalizing tracking-error
streaks), and down-weighted by source count (severely blurred frames
lose faint stars below the detection threshold). Satellite trails do
not affect the score: the metric is local to detected stars, so a
single bright trail on an otherwise sharp frame still scores high.
"""
import os
import shutil
import subprocess
import tempfile
import warnings
import numpy as np

DEFAULT_TARGET_FWHM = 3.0
DETECT_THRESH = 5.0
DETECT_MINAREA = 5
MIN_SOURCES = 10
MIN_BRIGHT = 4
BRIGHT_QUANTILE = 0.7
COUNT_SATURATION = 10
FWHM_RANGE = (1.0, 30.0)
SOURCE_EXTRACTOR = "source-extractor"
FILTER_NAME = "/usr/share/source-extractor/gauss_3.0_5x5.conv"
FITS_EXTS = (".fits", ".fit", ".fts")
BACKEND_SOURCE_EXTRACTOR = "source-extractor"
BACKEND_SIMPLEXY = "simplexy"
BACKEND_SEP = "sep"
SOURCE_BACKENDS = (BACKEND_SOURCE_EXTRACTOR, BACKEND_SIMPLEXY, BACKEND_SEP)


class InsufficientSources(RuntimeError):
    pass


def to_grayscale(image):
    """Accept a path or ndarray. Return float32 2D array (no resampling)."""
    if isinstance(image, str):
        if os.path.splitext(image)[1].lower() in FITS_EXTS:
            from astropy.io import fits
            with fits.open(image) as hdul:
                arr = hdul[0].data
        else:
            from PIL import Image
            arr = np.asarray(Image.open(image).convert("F"))
    else:
        arr = np.asarray(image)
    arr = arr.astype(np.float32, copy=False)
    if arr.ndim == 3:
        arr = (0.2989 * arr[..., 0] + 0.5870 * arr[..., 1]
               + 0.1140 * arr[..., 2]) if arr.shape[2] >= 3 else arr[..., 0]
    elif arr.ndim != 2:
        raise ValueError(f"Unsupported image shape: {arr.shape}")
    return arr.astype(np.float32, copy=False)


def _ensure_fits(image, workdir):
    if isinstance(image, str) and os.path.splitext(image)[1].lower() in FITS_EXTS:
        return os.path.abspath(image)
    from astropy.io import fits
    out = os.path.join(workdir, "in.fits")
    fits.PrimaryHDU(to_grayscale(image)).writeto(out, overwrite=True)
    return out


def _image2xy_executable(image2xy_path=None):
    if image2xy_path:
        if os.path.sep in image2xy_path or (os.path.altsep and os.path.altsep in image2xy_path):
            return os.path.abspath(image2xy_path)
        return shutil.which(image2xy_path) or image2xy_path
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir))
    candidate = os.path.join(repo_root, "solver", "image2xy")
    if os.path.exists(candidate):
        return candidate
    found = shutil.which("image2xy")
    if found:
        return found
    raise FileNotFoundError("image2xy executable not found; build solver/image2xy first")


def _read_xyls_sources(cat_file):
    from astropy.io import fits
    fwhm_parts = []
    ecc_parts = []
    flux_parts = []

    with fits.open(cat_file) as hdul:
        for hdu in hdul[1:]:
            data = hdu.data
            if data is None or len(data) == 0:
                continue
            names = set(data.names or [])
            if not {"FWHM_IMAGE", "ELLIPTICITY", "FLUX"}.issubset(names):
                continue
            fwhm_parts.append(np.asarray(data["FWHM_IMAGE"], dtype=np.float64))
            ecc_parts.append(np.asarray(data["ELLIPTICITY"], dtype=np.float64))
            flux_parts.append(np.asarray(data["FLUX"], dtype=np.float64))

    if not fwhm_parts:
        empty = np.empty(0, dtype=np.float64)
        return empty, empty, empty
    return np.concatenate(fwhm_parts), np.concatenate(ecc_parts), np.concatenate(flux_parts)


def _detect_sources_image2xy(image, backend,
                             detect_thresh=DETECT_THRESH,
                             downsample=0,
                             downsample_as_required=3,
                             image2xy_path=None):
    with tempfile.TemporaryDirectory() as td:
        fits_path = _ensure_fits(image, td)
        cat_file = os.path.join(td, "image2xy.xyls")
        cmd = [
            _image2xy_executable(image2xy_path),
            "--source-backend", backend,
            "-O",
            "-o", cat_file,
            "-p", str(detect_thresh),
        ]
        if downsample:
            cmd.extend(["-d", str(downsample)])
        if downsample_as_required:
            cmd.extend(["-D", str(downsample_as_required)])
        cmd.append(fits_path)
        subprocess.run(cmd, check=True, capture_output=True, cwd=td)
        fwhm, ecc, flux = _read_xyls_sources(cat_file)

    keep = (
        np.isfinite(fwhm) & np.isfinite(ecc) & np.isfinite(flux) &
        (fwhm > FWHM_RANGE[0]) & (fwhm < FWHM_RANGE[1])
    )
    return fwhm[keep], ecc[keep], flux[keep]


def detect_sources(image,
                   detect_thresh=DETECT_THRESH,
                   detect_minarea=DETECT_MINAREA,
                   backend=BACKEND_SOURCE_EXTRACTOR,
                   **kwargs):
    """Return (fwhm, ellipticity, flux) arrays for FLAGS==0 sources whose
    FWHM falls inside FWHM_RANGE."""
    if backend not in SOURCE_BACKENDS:
        raise ValueError(f"Unknown source extraction backend: {backend}")
    if backend in (BACKEND_SIMPLEXY, BACKEND_SEP):
        return _detect_sources_image2xy(
            image,
            backend,
            detect_thresh=detect_thresh,
            **kwargs
        )

    with tempfile.TemporaryDirectory() as td:
        fits_path = _ensure_fits(image, td)
        param_file = os.path.join(td, "sex.param")
        with open(param_file, "w") as f:
            f.write("FWHM_IMAGE\nELLIPTICITY\nFLAGS\nFLUX_AUTO\n")
        cat_file = os.path.join(td, "sex.cat")
        subprocess.run([
            SOURCE_EXTRACTOR, fits_path,
            "-CATALOG_NAME", cat_file,
            "-CATALOG_TYPE", "ASCII_HEAD",
            "-PARAMETERS_NAME", param_file,
            "-DETECT_THRESH", str(detect_thresh),
            "-ANALYSIS_THRESH", str(detect_thresh),
            "-DETECT_MINAREA", str(detect_minarea),
            "-FILTER", "Y",
            "-FILTER_NAME", FILTER_NAME,
            "-VERBOSE_TYPE", "QUIET",
        ], check=True, capture_output=True, cwd=td)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            data = np.loadtxt(cat_file, ndmin=2)
    if data.size == 0:
        empty = np.empty(0, dtype=np.float64)
        return empty, empty, empty
    fwhm, ecc, flags, flux = data[:, 0], data[:, 1], data[:, 2], data[:, 3]
    keep = (flags == 0) & (fwhm > FWHM_RANGE[0]) & (fwhm < FWHM_RANGE[1])
    return fwhm[keep], ecc[keep], flux[keep]


def measure_psf(image, bright_quantile=BRIGHT_QUANTILE, **kwargs):
    """Return (median_fwhm, median_ellipticity, n_bright_sources).

    Restricts the median to the brightest `bright_quantile` of detected
    sources (top 30% by FLUX_AUTO by default), so faint detections and
    leftover noise blobs don't wash out the PSF signal coming from real
    reference stars. Raises InsufficientSources when fewer than
    MIN_SOURCES total or MIN_BRIGHT bright sources are detected."""
    fwhm, ecc, flux = detect_sources(image, **kwargs)
    if len(fwhm) < MIN_SOURCES:
        raise InsufficientSources(
            f"only {len(fwhm)} clean sources detected (need >= {MIN_SOURCES})"
        )
    bright = flux >= np.quantile(flux, bright_quantile)
    if bright.sum() < MIN_BRIGHT:
        raise InsufficientSources(
            f"only {int(bright.sum())} bright sources above quantile "
            f"{bright_quantile} (need >= {MIN_BRIGHT})"
        )
    return float(np.median(fwhm[bright])), float(np.median(ecc[bright])), int(bright.sum())


def compute_blur_score(image, target_fwhm=DEFAULT_TARGET_FWHM, **kwargs):
    """Return blur score in [0, 1]. 1 = sharp, 0 = severely blurred.

    score = fwhm_score * shape_weight * count_weight, where
        fwhm_score   = clip(target_fwhm / median_bright_fwhm, 0, 1)
        shape_weight = clip(1 - median_bright_ellipticity, 0, 1)
        count_weight = clip(n_bright / COUNT_SATURATION, 0, 1)
    """
    return score_from_psf(*measure_psf(image, **kwargs), target_fwhm=target_fwhm)


def score_from_psf(fwhm, ecc, n_bright, target_fwhm):
    fwhm_score = min(1.0, target_fwhm / fwhm) if fwhm > 0 else 0.0
    shape_weight = max(0.0, 1.0 - ecc)
    count_weight = min(1.0, n_bright / COUNT_SATURATION)
    return float(fwhm_score * shape_weight * count_weight)


def _main(argv=None):
    import argparse, sys
    p = argparse.ArgumentParser(
        prog="blur_score",
        description="Astro blur score (1 = sharp, 0 = blurred) via stellar PSF.",
    )
    p.add_argument("image")
    p.add_argument("--backend", choices=SOURCE_BACKENDS, default=BACKEND_SOURCE_EXTRACTOR)
    p.add_argument("--image2xy-path",
                   help="Path to image2xy for simplexy/sep backends.")
    p.add_argument("--target-fwhm", type=float, default=DEFAULT_TARGET_FWHM)
    p.add_argument("--detect-thresh", type=float, default=DETECT_THRESH)
    p.add_argument("--raw", action="store_true",
                   help="Print 'fwhm=... ellipticity=... n_sources=...' instead of the score.")
    args = p.parse_args(argv)
    try:
        f, e, n = measure_psf(args.image, detect_thresh=args.detect_thresh,
                              backend=args.backend,
                              image2xy_path=args.image2xy_path)
    except InsufficientSources as ex:
        print(f"insufficient_sources: {ex}", file=sys.stderr)
        return 2
    if args.raw:
        print(f"fwhm={f:.3f} ellipticity={e:.3f} n_sources={n}")
    else:
        print(score_from_psf(f, e, n, args.target_fwhm))
    return 0


if __name__ == "__main__":
    raise SystemExit(_main())
