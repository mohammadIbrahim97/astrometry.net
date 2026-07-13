#!/usr/bin/env python3
"""Compare stable solve-field outputs from two run directories.

The comparison focuses on scientific solver outputs and ignores volatile
metadata such as FITS creation dates, temporary paths, and timing-only fields.
"""
from __future__ import annotations

import argparse
import difflib
import math
import re
import sys
from dataclasses import dataclass
from numbers import Real
from pathlib import Path
from typing import Iterable

import numpy as np
from astropy.io import fits


DEFAULT_EXTENSIONS = (".solved", ".wcs", ".match", ".corr")
RDLS_EXTENSION = ".rdls"
DEFAULT_IGNORE_HEADER_KEYS = {
    "",
    "HISTORY",
    "DATE",
    "CHECKSUM",
    "DATASUM",
}
DEFAULT_IGNORE_COLUMNS = {
    "TIMEUSED",
}
TIMESTAMP_REPLACEMENTS = (
    re.compile(r"\b\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:?\d{2})?\b"),
    re.compile(r"\b\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}(?:\.\d+)?\b"),
    re.compile(r"\b(?:Mon|Tue|Wed|Thu|Fri|Sat|Sun)_[A-Za-z]{3}_\d{1,2}_\d{2}:\d{2}:\d{2}_\d{4}_[+-]\d{4}\b"),
    re.compile(r"\b(?:Mon|Tue|Wed|Thu|Fri|Sat|Sun)\s+[A-Za-z]{3}\s+\d{1,2}\s+\d{2}:\d{2}:\d{2}\s+\d{4}\b"),
)
TEMP_PATH_RE = re.compile(r"(?<![\w.-])(?:/tmp|/var/tmp|/private/tmp)(?:/[^\s\"')\],;]+)*")
TIMING_LOG_RE = re.compile(
    r"\b(cpu time|wall time|elapsed|runtime|timeused|time used|total time limit|time limit)\b",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class Difference:
    filename: str
    where: str
    detail: str

    def format(self) -> str:
        return f"{self.filename}: {self.where}: {self.detail}"


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Compare .solved, .wcs, .match, .corr, and optionally .rdls "
            "outputs from two solve-field runs."
        )
    )
    parser.add_argument("baseline_dir", type=Path, help="Directory containing baseline outputs.")
    parser.add_argument("candidate_dir", type=Path, help="Directory containing candidate outputs.")
    parser.add_argument(
        "--base",
        help="Output base name, for example 'apod5' for apod5.wcs. "
        "If omitted, a unique base is inferred from the baseline directory.",
    )
    parser.add_argument(
        "--include-rdls",
        action="store_true",
        help="Also compare the optional .rdls file.",
    )
    parser.add_argument("--log-a", type=Path, help="Optional baseline solve-field log.")
    parser.add_argument("--log-b", type=Path, help="Optional candidate solve-field log.")
    parser.add_argument(
        "--rtol",
        type=float,
        default=1e-10,
        help="Relative tolerance for floating point FITS values (default: %(default)s).",
    )
    parser.add_argument(
        "--atol",
        type=float,
        default=1e-12,
        help="Absolute tolerance for floating point FITS values (default: %(default)s).",
    )
    parser.add_argument(
        "--ignore-header",
        action="append",
        default=[],
        metavar="KEY",
        help="Additional FITS header keyword to ignore. May be repeated.",
    )
    parser.add_argument(
        "--ignore-column",
        action="append",
        default=[],
        metavar="NAME",
        help="Additional FITS table column to ignore. May be repeated.",
    )
    parser.add_argument(
        "--strict-logs",
        action="store_true",
        help="Compare normalized logs without dropping timing-only log lines.",
    )
    parser.add_argument(
        "--max-diffs",
        type=int,
        default=50,
        help="Maximum number of differences to print (default: %(default)s).",
    )
    return parser.parse_args(argv)


def infer_base(run_dir: Path) -> str:
    base_sets: list[set[str]] = []
    for ext in DEFAULT_EXTENSIONS:
        base_sets.append({path.name[: -len(ext)] for path in run_dir.glob(f"*{ext}")})
    common = set.intersection(*base_sets) if base_sets else set()
    if len(common) == 1:
        return next(iter(common))
    if common:
        choices = ", ".join(sorted(common))
        raise SystemExit(f"Multiple output bases found; pass --base. Choices: {choices}")
    raise SystemExit(f"Could not infer output base from required files in {run_dir}")


def normalize_text(value: object, roots: Iterable[Path]) -> str:
    text = str(value).strip()
    for root in roots:
        if not root:
            continue
        root_text = str(root)
        if root_text:
            text = text.replace(root_text, "<RUN>")
        try:
            resolved = str(root.resolve())
        except OSError:
            resolved = ""
        if resolved and resolved != root_text:
            text = text.replace(resolved, "<RUN>")
    text = TEMP_PATH_RE.sub("<TMPPATH>", text)
    for regex in TIMESTAMP_REPLACEMENTS:
        text = regex.sub("<TIMESTAMP>", text)
    text = re.sub(r"\s+", " ", text)
    return text


def compare_scalar(
    filename: str,
    where: str,
    left: object,
    right: object,
    roots: Iterable[Path],
    rtol: float,
    atol: float,
) -> list[Difference]:
    if isinstance(left, str) or isinstance(right, str):
        left_text = normalize_text(left, roots)
        right_text = normalize_text(right, roots)
        if left_text != right_text:
            return [Difference(filename, where, f"{left_text!r} != {right_text!r}")]
        return []

    if isinstance(left, Real) and isinstance(right, Real) and not isinstance(left, bool) and not isinstance(right, bool):
        if math.isclose(float(left), float(right), rel_tol=rtol, abs_tol=atol) or (
            math.isnan(float(left)) and math.isnan(float(right))
        ):
            return []
        return [Difference(filename, where, f"{left!r} != {right!r}")]

    if left != right:
        return [Difference(filename, where, f"{left!r} != {right!r}")]
    return []


def normalized_header_cards(
    header: fits.Header,
    ignore_header_keys: set[str],
    roots: Iterable[Path],
) -> list[tuple[str, object]]:
    cards: list[tuple[str, object]] = []
    comment_parts: list[str] = []

    def flush_comments() -> None:
        if comment_parts:
            cards.append(("COMMENT", " ".join(comment_parts)))
            comment_parts.clear()

    for card in header.cards:
        key = card.keyword.upper()
        if key in ignore_header_keys:
            continue
        if key == "COMMENT":
            text = normalize_text(card.value, roots)
            if TIMING_LOG_RE.search(text):
                continue
            comment_parts.append(text)
            continue
        flush_comments()
        cards.append((card.keyword, card.value))
    flush_comments()
    return cards


def compare_headers(
    filename: str,
    hdu_label: str,
    left: fits.Header,
    right: fits.Header,
    ignore_header_keys: set[str],
    roots: Iterable[Path],
    rtol: float,
    atol: float,
) -> list[Difference]:
    diffs: list[Difference] = []
    left_cards = normalized_header_cards(left, ignore_header_keys, roots)
    right_cards = normalized_header_cards(right, ignore_header_keys, roots)
    left_keys = [key for key, _ in left_cards]
    right_keys = [key for key, _ in right_cards]
    if left_keys != right_keys:
        for line in difflib.unified_diff(left_keys, right_keys, fromfile="baseline", tofile="candidate", lineterm=""):
            diffs.append(Difference(filename, f"{hdu_label} header keys", line))
            if len(diffs) >= 10:
                break
        return diffs

    for idx, ((key, left_value), (_, right_value)) in enumerate(zip(left_cards, right_cards)):
        where = f"{hdu_label} header {key} card {idx}"
        diffs.extend(compare_scalar(filename, where, left_value, right_value, roots, rtol, atol))
    return diffs


def first_mismatch(mask: np.ndarray) -> tuple[int, ...]:
    positions = np.argwhere(mask)
    if positions.size == 0:
        return ()
    return tuple(int(value) for value in positions[0])


def compare_float_arrays(
    filename: str,
    where: str,
    left: np.ndarray,
    right: np.ndarray,
    rtol: float,
    atol: float,
) -> list[Difference]:
    close = np.isclose(left, right, rtol=rtol, atol=atol, equal_nan=True)
    if np.all(close):
        return []
    mismatch = ~close
    idx = first_mismatch(mismatch)
    absdiff = np.abs(left - right)
    finite = absdiff[np.isfinite(absdiff)]
    max_abs = float(np.max(finite)) if finite.size else float("nan")
    return [
        Difference(
            filename,
            where,
            f"float values differ at {idx}: {left[idx]!r} != {right[idx]!r}; max abs diff {max_abs:.6g}",
        )
    ]


def normalize_string_array(values: np.ndarray, roots: Iterable[Path]) -> np.ndarray:
    normalized = [normalize_text(item.decode("utf-8", "replace") if isinstance(item, bytes) else item, roots) for item in values.flat]
    return np.array(normalized, dtype=object).reshape(values.shape)


def compare_arrays(
    filename: str,
    where: str,
    left: np.ndarray,
    right: np.ndarray,
    roots: Iterable[Path],
    rtol: float,
    atol: float,
) -> list[Difference]:
    left = np.asarray(left)
    right = np.asarray(right)
    if left.shape != right.shape:
        return [Difference(filename, where, f"shape {left.shape} != {right.shape}")]

    if left.dtype.kind in "fc" or right.dtype.kind in "fc":
        return compare_float_arrays(filename, where, left, right, rtol, atol)

    if left.dtype.kind in "SUO" or right.dtype.kind in "SUO":
        left_norm = normalize_string_array(left, roots)
        right_norm = normalize_string_array(right, roots)
        mismatch = left_norm != right_norm
        if np.any(mismatch):
            idx = first_mismatch(mismatch)
            return [Difference(filename, where, f"values differ at {idx}: {left_norm[idx]!r} != {right_norm[idx]!r}")]
        return []

    mismatch = left != right
    if np.any(mismatch):
        idx = first_mismatch(mismatch)
        return [Difference(filename, where, f"values differ at {idx}: {left[idx]!r} != {right[idx]!r}")]
    return []


def compare_table_data(
    filename: str,
    hdu_label: str,
    left_data: fits.FITS_rec,
    right_data: fits.FITS_rec,
    ignore_columns: set[str],
    roots: Iterable[Path],
    rtol: float,
    atol: float,
) -> list[Difference]:
    diffs: list[Difference] = []
    left_names = [name for name in (left_data.names or []) if name.upper() not in ignore_columns]
    right_names = [name for name in (right_data.names or []) if name.upper() not in ignore_columns]
    if left_names != right_names:
        for line in difflib.unified_diff(left_names, right_names, fromfile="baseline", tofile="candidate", lineterm=""):
            diffs.append(Difference(filename, f"{hdu_label} table columns", line))
            if len(diffs) >= 10:
                break
        return diffs

    if len(left_data) != len(right_data):
        diffs.append(Difference(filename, hdu_label, f"row count {len(left_data)} != {len(right_data)}"))
        return diffs

    for name in left_names:
        diffs.extend(
            compare_arrays(
                filename,
                f"{hdu_label} column {name}",
                left_data[name],
                right_data[name],
                roots,
                rtol,
                atol,
            )
        )
    return diffs


def compare_hdu_data(
    filename: str,
    hdu_label: str,
    left_hdu: fits.hdu.base.ExtensionHDU,
    right_hdu: fits.hdu.base.ExtensionHDU,
    ignore_columns: set[str],
    roots: Iterable[Path],
    rtol: float,
    atol: float,
) -> list[Difference]:
    left_data = left_hdu.data
    right_data = right_hdu.data
    if left_data is None and right_data is None:
        return []
    if (left_data is None) != (right_data is None):
        return [Difference(filename, hdu_label, "one HDU has data and the other does not")]
    if getattr(left_data, "names", None) is not None or getattr(right_data, "names", None) is not None:
        return compare_table_data(filename, hdu_label, left_data, right_data, ignore_columns, roots, rtol, atol)
    return compare_arrays(filename, f"{hdu_label} data", left_data, right_data, roots, rtol, atol)


def compare_fits(
    filename: str,
    left_path: Path,
    right_path: Path,
    ignore_header_keys: set[str],
    ignore_columns: set[str],
    roots: Iterable[Path],
    rtol: float,
    atol: float,
) -> list[Difference]:
    diffs: list[Difference] = []
    try:
        left_hdus = fits.open(left_path, memmap=False)
        right_hdus = fits.open(right_path, memmap=False)
    except Exception as exc:
        return [Difference(filename, "open", str(exc))]

    try:
        if len(left_hdus) != len(right_hdus):
            return [Difference(filename, "HDU count", f"{len(left_hdus)} != {len(right_hdus)}")]
        for idx, (left_hdu, right_hdu) in enumerate(zip(left_hdus, right_hdus)):
            hdu_label = f"HDU {idx}"
            if type(left_hdu).__name__ != type(right_hdu).__name__:
                diffs.append(
                    Difference(filename, hdu_label, f"{type(left_hdu).__name__} != {type(right_hdu).__name__}")
                )
                continue
            diffs.extend(
                compare_headers(
                    filename,
                    hdu_label,
                    left_hdu.header,
                    right_hdu.header,
                    ignore_header_keys,
                    roots,
                    rtol,
                    atol,
                )
            )
            diffs.extend(
                compare_hdu_data(
                    filename,
                    hdu_label,
                    left_hdu,
                    right_hdu,
                    ignore_columns,
                    roots,
                    rtol,
                    atol,
                )
            )
    finally:
        left_hdus.close()
        right_hdus.close()
    return diffs


def compare_marker(filename: str, left_path: Path, right_path: Path) -> list[Difference]:
    left = left_path.read_bytes()
    right = right_path.read_bytes()
    if left != right:
        return [Difference(filename, "marker bytes", f"{left!r} != {right!r}")]
    return []


def compare_output_file(
    filename: str,
    left_path: Path,
    right_path: Path,
    ignore_header_keys: set[str],
    ignore_columns: set[str],
    roots: Iterable[Path],
    rtol: float,
    atol: float,
) -> list[Difference]:
    if not left_path.exists() or not right_path.exists():
        diffs = []
        if not left_path.exists():
            diffs.append(Difference(filename, "file", f"missing baseline file {left_path}"))
        if not right_path.exists():
            diffs.append(Difference(filename, "file", f"missing candidate file {right_path}"))
        return diffs
    if filename.endswith(".solved"):
        return compare_marker(filename, left_path, right_path)
    return compare_fits(filename, left_path, right_path, ignore_header_keys, ignore_columns, roots, rtol, atol)


def normalize_log(path: Path, roots: Iterable[Path], strict_logs: bool) -> list[str]:
    lines = path.read_text(errors="replace").splitlines()
    normalized: list[str] = []
    for line in lines:
        text = normalize_text(line, roots)
        text = re.sub(r"^\[?<TIMESTAMP>\]?\s*", "", text)
        if not strict_logs and TIMING_LOG_RE.search(text):
            continue
        if text:
            normalized.append(text)
    return normalized


def compare_logs(
    left_path: Path,
    right_path: Path,
    roots: Iterable[Path],
    strict_logs: bool,
) -> list[Difference]:
    if not left_path.exists() or not right_path.exists():
        diffs = []
        if not left_path.exists():
            diffs.append(Difference("logs", "file", f"missing baseline log {left_path}"))
        if not right_path.exists():
            diffs.append(Difference("logs", "file", f"missing candidate log {right_path}"))
        return diffs

    left_lines = normalize_log(left_path, roots, strict_logs)
    right_lines = normalize_log(right_path, roots, strict_logs)
    if left_lines == right_lines:
        return []
    diff_lines = list(
        difflib.unified_diff(left_lines, right_lines, fromfile=str(left_path), tofile=str(right_path), lineterm="")
    )
    return [Difference("logs", "normalized diff", line) for line in diff_lines[:40]]


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    baseline_dir = args.baseline_dir
    candidate_dir = args.candidate_dir
    base = args.base or infer_base(baseline_dir)
    extensions = list(DEFAULT_EXTENSIONS)
    if args.include_rdls:
        extensions.append(RDLS_EXTENSION)

    ignore_header_keys = {key.upper() for key in DEFAULT_IGNORE_HEADER_KEYS}
    ignore_header_keys.update(key.upper() for key in args.ignore_header)
    ignore_columns = {name.upper() for name in DEFAULT_IGNORE_COLUMNS}
    ignore_columns.update(name.upper() for name in args.ignore_column)
    roots = (baseline_dir, candidate_dir)

    diffs: list[Difference] = []
    for ext in extensions:
        filename = f"{base}{ext}"
        diffs.extend(
            compare_output_file(
                filename,
                baseline_dir / filename,
                candidate_dir / filename,
                ignore_header_keys,
                ignore_columns,
                roots,
                args.rtol,
                args.atol,
            )
        )

    if bool(args.log_a) != bool(args.log_b):
        raise SystemExit("--log-a and --log-b must be provided together")
    if args.log_a and args.log_b:
        diffs.extend(compare_logs(args.log_a, args.log_b, roots, args.strict_logs))

    if diffs:
        print(f"DIFFERENT: found {len(diffs)} difference(s)")
        for diff in diffs[: args.max_diffs]:
            print(diff.format())
        if len(diffs) > args.max_diffs:
            print(f"... {len(diffs) - args.max_diffs} more difference(s) omitted")
        return 1

    compared = " ".join(extensions)
    print(f"OK: {base} outputs match for {compared}")
    print(
        "Ignored volatile data: FITS DATE/HISTORY/CHECKSUM/DATASUM, timing-only "
        "COMMENT cards, table columns TIMEUSED, temp paths, timestamps, and "
        "timing-only log lines."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
