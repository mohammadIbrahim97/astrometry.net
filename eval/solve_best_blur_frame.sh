#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

PYTHON_BIN="${PYTHON:-python3}"
SOLVE_FIELD_BIN="${SOLVE_FIELD:-}"
IMAGE2XY_PATH="${IMAGE2XY_PATH:-}"

IMAGES_DIR="/src/Astrometry-testing-data/data"
OUT_DIR="$REPO_ROOT/eval/best_blur_solve"
THRESHOLD="0.8"
BLUR_BACKEND="source-extractor"
SOURCE_BACKEND=""
BLUR_JOBS="4"
SOLVE_WORKERS="4"
SHARD_MODE="threads"
MAX_IMAGES=""
DOWNSAMPLE="0"
DOWNSAMPLE_AS_REQUIRED="3"
TRACE_SHARDS="${ASTROMETRY_INDEX_SHARD_TRACE:-1}"
PREPARE_AXY="1"
WRITE_NEW_FITS="0"
BASE_NAME=""
ALLOW_BEST_BELOW_THRESHOLD="0"

usage() {
    cat <<'EOF'
Usage:
  eval/solve_best_blur_frame.sh --images-dir DIR [options] -- [solve-field args]

Pipeline:
  1. Run blur-score detection over DIR.
  2. Keep images with score > threshold.
  3. Select the highest-score image.
  4. Prepare .xyls/.axy source lists for that image.
  5. Run solve-field with threaded index shards on the selected input.

Options:
  --images-dir DIR             Frame/image directory (default: /src/Astrometry-testing-data/data)
  --out-dir DIR                Output directory (default: eval/best_blur_solve)
  --threshold FLOAT            Strict blur-score threshold (default: 0.8)
  --blur-backend NAME          source-extractor|simplexy|sep (default: source-extractor)
  --source-backend NAME        solve-field source backend for .axy preparation (default: blur backend)
  --blur-jobs N                Parallel blur-score jobs (default: 4)
  --solve-workers N            ASTROMETRY_INDEX_SHARD_WORKERS (default: 4)
  --shard-mode MODE            ASTROMETRY_INDEX_SHARDS mode (default: threads)
  --max-images N               Limit image scan for quick tests
  --downsample N               Downsample for blur scoring and .axy preparation
  --downsample-as-required N   blur_score_benchmark downsample fallback (default: 3)
  --base NAME                  Output base name (default: derived from selected image)
  --allow-best-below-threshold Use the best valid image if none exceeds threshold
  --no-axy                     Solve the selected image directly instead of prepared .xyls/.axy
  --write-new-fits             Allow solve-field to write a new FITS output
  --no-trace-shards            Do not enable ASTROMETRY_INDEX_SHARD_TRACE
  --solve-field PATH           solve-field binary (default: repo build, then PATH)
  --image2xy-path PATH         image2xy binary for blur_score_benchmark
  -h, --help                   Show this help

Everything after "--" is passed to solve-field. Put index config, scale,
depth, RA/Dec/radius, and other solver-specific options there, for example:

  eval/solve_best_blur_frame.sh \
    --images-dir /src/Astrometry-testing-data/data/20260303200006652 \
    --out-dir /tmp/best-frame-solve \
    -- \
    --config /tmp/indexes.cfg --depth 10-20 -L 2.5 -H 3.2 -u arcsecperpix
EOF
}

die() {
    echo "error: $*" >&2
    exit 2
}

die_images_dir_not_found() {
    echo "error: images directory not found: $IMAGES_DIR" >&2
    if [[ "$IMAGES_DIR" == /src/Astrometry-testing-data/* ]]; then
        cat >&2 <<'EOF'
hint: /src/Astrometry-testing-data exists inside the astrometrynet/solver:test Docker image.
      Run this script inside that container, or pass the real host path to --images-dir.
EOF
    fi
    exit 2
}

quote_command() {
    printf '%q ' "$@"
}

preflight_solve_args() {
    local arg
    local next
    local i=0

    while [[ "$i" -lt "${#SOLVE_ARGS[@]}" ]]; do
        arg="${SOLVE_ARGS[$i]}"
        case "$arg" in
            --config|-b|--backend-config)
                next=$((i + 1))
                [[ "$next" -lt "${#SOLVE_ARGS[@]}" ]] || die "$arg needs a value"
                [[ -f "${SOLVE_ARGS[$next]}" ]] ||
                    die "solve-field config not found in current environment: ${SOLVE_ARGS[$next]}"
                i=$((i + 2))
                ;;
            --config=*)
                [[ -f "${arg#--config=}" ]] ||
                    die "solve-field config not found in current environment: ${arg#--config=}"
                i=$((i + 1))
                ;;
            --backend-config=*)
                [[ -f "${arg#--backend-config=}" ]] ||
                    die "solve-field config not found in current environment: ${arg#--backend-config=}"
                i=$((i + 1))
                ;;
            *)
                i=$((i + 1))
                ;;
        esac
    done
}

SOLVE_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --images-dir)
            [[ $# -ge 2 ]] || die "--images-dir needs a value"
            IMAGES_DIR="$2"
            shift 2
            ;;
        --out-dir)
            [[ $# -ge 2 ]] || die "--out-dir needs a value"
            OUT_DIR="$2"
            shift 2
            ;;
        --threshold)
            [[ $# -ge 2 ]] || die "--threshold needs a value"
            THRESHOLD="$2"
            shift 2
            ;;
        --blur-backend)
            [[ $# -ge 2 ]] || die "--blur-backend needs a value"
            BLUR_BACKEND="$2"
            shift 2
            ;;
        --source-backend)
            [[ $# -ge 2 ]] || die "--source-backend needs a value"
            SOURCE_BACKEND="$2"
            shift 2
            ;;
        --blur-jobs)
            [[ $# -ge 2 ]] || die "--blur-jobs needs a value"
            BLUR_JOBS="$2"
            shift 2
            ;;
        --solve-workers)
            [[ $# -ge 2 ]] || die "--solve-workers needs a value"
            SOLVE_WORKERS="$2"
            shift 2
            ;;
        --shard-mode)
            [[ $# -ge 2 ]] || die "--shard-mode needs a value"
            SHARD_MODE="$2"
            shift 2
            ;;
        --max-images)
            [[ $# -ge 2 ]] || die "--max-images needs a value"
            MAX_IMAGES="$2"
            shift 2
            ;;
        --downsample)
            [[ $# -ge 2 ]] || die "--downsample needs a value"
            DOWNSAMPLE="$2"
            shift 2
            ;;
        --downsample-as-required)
            [[ $# -ge 2 ]] || die "--downsample-as-required needs a value"
            DOWNSAMPLE_AS_REQUIRED="$2"
            shift 2
            ;;
        --base)
            [[ $# -ge 2 ]] || die "--base needs a value"
            BASE_NAME="$2"
            shift 2
            ;;
        --allow-best-below-threshold)
            ALLOW_BEST_BELOW_THRESHOLD="1"
            shift
            ;;
        --no-axy)
            PREPARE_AXY="0"
            shift
            ;;
        --write-new-fits)
            WRITE_NEW_FITS="1"
            shift
            ;;
        --no-trace-shards)
            TRACE_SHARDS="0"
            shift
            ;;
        --solve-field)
            [[ $# -ge 2 ]] || die "--solve-field needs a value"
            SOLVE_FIELD_BIN="$2"
            shift 2
            ;;
        --image2xy-path)
            [[ $# -ge 2 ]] || die "--image2xy-path needs a value"
            IMAGE2XY_PATH="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            SOLVE_ARGS=("$@")
            break
            ;;
        *)
            die "unknown argument: $1"
            ;;
    esac
done

[[ -d "$IMAGES_DIR" ]] || die_images_dir_not_found
[[ "$BLUR_JOBS" =~ ^[0-9]+$ ]] && [[ "$BLUR_JOBS" -ge 1 ]] || die "--blur-jobs must be >= 1"
[[ "$SOLVE_WORKERS" =~ ^[0-9]+$ ]] && [[ "$SOLVE_WORKERS" -ge 1 ]] || die "--solve-workers must be >= 1"
[[ "$DOWNSAMPLE" =~ ^[0-9]+$ ]] || die "--downsample must be an integer"
[[ "$DOWNSAMPLE_AS_REQUIRED" =~ ^[0-9]+$ ]] || die "--downsample-as-required must be an integer"

if [[ -z "$SOLVE_FIELD_BIN" ]]; then
    solve_field_candidate="$REPO_ROOT/solver/solve-field"
    if [[ -x "$solve_field_candidate" ]] && "$solve_field_candidate" --help >/dev/null 2>&1; then
        SOLVE_FIELD_BIN="$solve_field_candidate"
    elif command -v solve-field >/dev/null 2>&1; then
        SOLVE_FIELD_BIN="$(command -v solve-field)"
    elif [[ -x "$solve_field_candidate" ]]; then
        SOLVE_FIELD_BIN="$solve_field_candidate"
    else
        SOLVE_FIELD_BIN="solve-field"
    fi
fi

if [[ -z "$IMAGE2XY_PATH" && -x "$REPO_ROOT/solver/image2xy" ]]; then
    image2xy_candidate="$REPO_ROOT/solver/image2xy"
    if "$image2xy_candidate" --help >/dev/null 2>&1; then
        IMAGE2XY_PATH="$image2xy_candidate"
    elif command -v image2xy >/dev/null 2>&1; then
        IMAGE2XY_PATH="$(command -v image2xy)"
    else
        IMAGE2XY_PATH="$image2xy_candidate"
    fi
fi

if [[ -z "$SOURCE_BACKEND" ]]; then
    SOURCE_BACKEND="$BLUR_BACKEND"
fi

case "$BLUR_BACKEND" in
    source-extractor|simplexy|sep) ;;
    *) die "--blur-backend must be source-extractor, simplexy, or sep" ;;
esac

case "$SOURCE_BACKEND" in
    source-extractor|simplexy|sep) ;;
    *) die "--source-backend must be source-extractor, simplexy, or sep" ;;
esac

SOLVE_FIELD_HELP="$("$SOLVE_FIELD_BIN" --help 2>&1 || true)"
SOLVE_SOURCE_FLAGS=()
if [[ "$SOURCE_BACKEND" != "simplexy" ]]; then
    if [[ "$SOLVE_FIELD_HELP" == *"--source-backend"* ]]; then
        SOLVE_SOURCE_FLAGS=(--source-backend "$SOURCE_BACKEND")
    elif [[ "$SOURCE_BACKEND" == "source-extractor" ]]; then
        SOLVE_SOURCE_FLAGS=(--use-source-extractor)
    elif [[ "$SOURCE_BACKEND" == "sep" && "$SOLVE_FIELD_HELP" == *"--use-sep"* ]]; then
        SOLVE_SOURCE_FLAGS=(--use-sep)
    else
        die "$SOLVE_FIELD_BIN does not support source backend '$SOURCE_BACKEND'"
    fi
fi

preflight_solve_args

mkdir -p "$OUT_DIR"

export PYTHONPATH="$REPO_ROOT${PYTHONPATH:+:$PYTHONPATH}"

BLUR_CSV="$OUT_DIR/blur_scores.csv"
BLUR_LOG="$OUT_DIR/blur_scores.log"
SELECTED_CSV="$OUT_DIR/selected_above_threshold.csv"
BEST_TSV="$OUT_DIR/best_image.tsv"

BLUR_CMD=(
    "$PYTHON_BIN" -m astrometry.util.blur_score_benchmark
    --images-dir "$IMAGES_DIR"
    --backend "$BLUR_BACKEND"
    --jobs "$BLUR_JOBS"
    --downsample "$DOWNSAMPLE"
    --downsample-as-required "$DOWNSAMPLE_AS_REQUIRED"
)
if [[ -n "$MAX_IMAGES" ]]; then
    BLUR_CMD+=(--max-images "$MAX_IMAGES")
fi
if [[ -n "$IMAGE2XY_PATH" ]]; then
    BLUR_CMD+=(--image2xy-path "$IMAGE2XY_PATH")
fi

{
    echo "Command:"
    quote_command "${BLUR_CMD[@]}"
    echo
    echo
} > "$BLUR_LOG"

echo "[best-blur-solve] scoring images in $IMAGES_DIR"
"${BLUR_CMD[@]}" > "$BLUR_CSV" 2>> "$BLUR_LOG"

"$PYTHON_BIN" - "$BLUR_CSV" "$THRESHOLD" "$SELECTED_CSV" "$BEST_TSV" "$ALLOW_BEST_BELOW_THRESHOLD" <<'PY'
import csv
import math
import sys

csv_path, threshold_text, selected_path, best_path, allow_best_text = sys.argv[1:6]
threshold = float(threshold_text)
allow_best_below_threshold = allow_best_text == "1"

ok_rows = []
selected = []
used_fallback = False
with open(csv_path, newline="") as f:
    reader = csv.DictReader(f)
    for row in reader:
        if row.get("status") != "ok":
            continue
        try:
            score = float(row.get("score", "nan"))
            n_sources = int(row.get("n_sources", "0"))
        except ValueError:
            continue
        if not math.isfinite(score):
            continue
        row["_score"] = score
        row["_n_sources"] = n_sources
        ok_rows.append(row)
        if score > threshold:
            selected.append(row)

def sort_key(row):
    return (-row["_score"], -row["_n_sources"], row.get("image", ""))

selected.sort(key=sort_key)
if not selected:
    if not ok_rows:
        print("no usable blur-score rows found", file=sys.stderr)
        sys.exit(3)
    best = sorted(ok_rows, key=sort_key)[0]
    message = (
        "no image exceeded threshold "
        f"{threshold}; best available score={best.get('score')} "
        f"image={best.get('image')}"
    )
    if not allow_best_below_threshold:
        print(message, file=sys.stderr)
        sys.exit(3)
    print(f"{message}; continuing because --allow-best-below-threshold is set", file=sys.stderr)
    selected = [best]
    used_fallback = True

fieldnames = [
    "rank", "threshold_passed", "backend", "image", "elapsed_s", "status",
    "fwhm", "ellipticity", "n_sources", "score", "error",
]
with open(selected_path, "w", newline="") as f:
    writer = csv.DictWriter(f, fieldnames=fieldnames)
    writer.writeheader()
    for rank, row in enumerate(selected, start=1):
        out = {name: row.get(name, "") for name in fieldnames}
        out["rank"] = rank
        out["threshold_passed"] = "0" if used_fallback else "1"
        writer.writerow(out)

best = selected[0]
with open(best_path, "w", newline="") as f:
    f.write("\t".join([
        best.get("image", ""),
        best.get("score", ""),
        best.get("fwhm", ""),
        best.get("ellipticity", ""),
        best.get("n_sources", ""),
    ]))
    f.write("\n")
PY

IFS=$'\t' read -r BEST_IMAGE BEST_SCORE BEST_FWHM BEST_ELLIPTICITY BEST_N_SOURCES < "$BEST_TSV"
[[ -n "$BEST_IMAGE" ]] || die "best image selection failed"

IFS=$'\t' read -r IMAGE_WIDTH IMAGE_HEIGHT < <("$PYTHON_BIN" - "$BEST_IMAGE" <<'PY'
import os
import sys

path = sys.argv[1]
ext = os.path.splitext(path)[1].lower()
if ext in (".fits", ".fit", ".fts"):
    from astropy.io import fits
    with fits.open(path) as hdul:
        hdu = hdul[0]
        width = hdu.header.get("NAXIS1")
        height = hdu.header.get("NAXIS2")
        if not width or not height:
            data = hdu.data
            if data is None or data.ndim < 2:
                raise SystemExit(f"cannot determine image size: {path}")
            height, width = data.shape[-2], data.shape[-1]
else:
    from PIL import Image
    with Image.open(path) as img:
        width, height = img.size
print(f"{int(width)}\t{int(height)}")
PY
)
[[ -n "$IMAGE_WIDTH" && -n "$IMAGE_HEIGHT" ]] || die "could not determine image dimensions for $BEST_IMAGE"

if [[ -z "$BASE_NAME" ]]; then
    image_name="$(basename "$BEST_IMAGE")"
    image_stem="${image_name%.*}"
    BASE_NAME="$(printf '%s' "$image_stem" | tr -c 'A-Za-z0-9._-' '_')"
fi

AXY_DIR="$OUT_DIR/axy"
SOLVE_DIR="$OUT_DIR/solve"
mkdir -p "$AXY_DIR" "$SOLVE_DIR"

PREPARED_AXY="$AXY_DIR/$BASE_NAME.axy"
PREPARED_XYLS="$AXY_DIR/$BASE_NAME.xyls"
PREPARE_LOG="$OUT_DIR/$BASE_NAME.prepare-axy.log"
SOLVE_LOG="$OUT_DIR/$BASE_NAME.solve.log"
METRICS_CSV="$OUT_DIR/$BASE_NAME.index-shard-metrics.csv"
SUMMARY="$OUT_DIR/$BASE_NAME.summary.txt"

SOLVE_INPUT="$BEST_IMAGE"

if [[ "$PREPARE_AXY" == "1" ]]; then
    PREPARE_CMD=(
        "$SOLVE_FIELD_BIN"
        --overwrite
        --no-plots
        --just-augment
        "${SOLVE_SOURCE_FLAGS[@]}"
        --axy "$PREPARED_AXY"
        --keep-xylist "$PREPARED_XYLS"
        --dir "$AXY_DIR"
        --out "$BASE_NAME"
    )
    if [[ "$DOWNSAMPLE" -gt 0 ]]; then
        PREPARE_CMD+=(--downsample "$DOWNSAMPLE")
    fi
    PREPARE_CMD+=("${SOLVE_ARGS[@]}" "$BEST_IMAGE")

    {
        echo "Command:"
        quote_command "${PREPARE_CMD[@]}"
        echo
        echo
    } > "$PREPARE_LOG"

    echo "[best-blur-solve] preparing .xyls/.axy for $BEST_IMAGE"
    "${PREPARE_CMD[@]}" >> "$PREPARE_LOG" 2>&1
    SOLVE_INPUT="$PREPARED_XYLS"
fi

SOLVE_CMD=(
    "$SOLVE_FIELD_BIN"
    --overwrite
    --no-plots
)
if [[ "$PREPARE_AXY" == "0" ]]; then
    SOLVE_CMD+=("${SOLVE_SOURCE_FLAGS[@]}")
else
    case "$SOURCE_BACKEND" in
        source-extractor)
            SOLVE_CMD+=(-w "$IMAGE_WIDTH" -e "$IMAGE_HEIGHT" -X X_IMAGE -Y Y_IMAGE -s MAG_AUTO -a)
            ;;
        simplexy|sep)
            SOLVE_CMD+=(-w "$IMAGE_WIDTH" -e "$IMAGE_HEIGHT" -X X -Y Y -s FLUX)
            ;;
    esac
fi
if [[ "$WRITE_NEW_FITS" == "0" ]]; then
    SOLVE_CMD+=(-N none)
fi
SOLVE_CMD+=("${SOLVE_ARGS[@]}")
SOLVE_CMD+=(
    --dir "$SOLVE_DIR"
    --out "$BASE_NAME"
    "$SOLVE_INPUT"
)

{
    echo "Command:"
    quote_command "${SOLVE_CMD[@]}"
    echo
    echo
} > "$SOLVE_LOG"

echo "[best-blur-solve] solving best image with score=$BEST_SCORE image=$BEST_IMAGE"
SOLVE_START=$SECONDS
ASTROMETRY_INDEX_SHARDS="$SHARD_MODE" \
ASTROMETRY_INDEX_SHARD_WORKERS="$SOLVE_WORKERS" \
ASTROMETRY_INDEX_SHARD_METRICS="$METRICS_CSV" \
ASTROMETRY_INDEX_SHARD_TRACE="$TRACE_SHARDS" \
    "${SOLVE_CMD[@]}" >> "$SOLVE_LOG" 2>&1
SOLVE_SECONDS=$((SECONDS - SOLVE_START))

SOLVED_FILE="$SOLVE_DIR/$BASE_NAME.solved"
SOLVED_VALUE="missing"
if [[ -f "$SOLVED_FILE" ]]; then
    SOLVED_VALUE="$(od -An -tu1 -N1 "$SOLVED_FILE" | tr -d '[:space:]')"
    [[ -n "$SOLVED_VALUE" ]] || SOLVED_VALUE="empty"
fi

{
    echo "images_dir=$IMAGES_DIR"
    echo "threshold=$THRESHOLD"
    echo "blur_backend=$BLUR_BACKEND"
    echo "blur_jobs=$BLUR_JOBS"
    echo "selected_csv=$SELECTED_CSV"
    echo "best_image=$BEST_IMAGE"
    echo "best_score=$BEST_SCORE"
    echo "best_fwhm=$BEST_FWHM"
    echo "best_ellipticity=$BEST_ELLIPTICITY"
    echo "best_n_sources=$BEST_N_SOURCES"
    echo "image_width=$IMAGE_WIDTH"
    echo "image_height=$IMAGE_HEIGHT"
    echo "prepare_axy=$PREPARE_AXY"
    echo "prepared_axy=$PREPARED_AXY"
    echo "prepared_xyls=$PREPARED_XYLS"
    echo "solve_input=$SOLVE_INPUT"
    echo "shard_mode=$SHARD_MODE"
    echo "solve_workers=$SOLVE_WORKERS"
    echo "solve_seconds=$SOLVE_SECONDS"
    echo "solved_file=$SOLVED_FILE"
    echo "solved_value=$SOLVED_VALUE"
    echo "solve_dir=$SOLVE_DIR"
    echo "metrics_csv=$METRICS_CSV"
    echo "solve_log=$SOLVE_LOG"
} > "$SUMMARY"

echo "[best-blur-solve] done"
echo "[best-blur-solve] summary: $SUMMARY"
echo "[best-blur-solve] selected list: $SELECTED_CSV"
echo "[best-blur-solve] solve outputs: $SOLVE_DIR"
