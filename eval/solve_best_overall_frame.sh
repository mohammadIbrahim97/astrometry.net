#!/usr/bin/env bash
# Script to find and solve the best frame across all directories based on blur score.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

PYTHON_BIN="${PYTHON:-python3}"
IMAGES_DIR="${1:-/src/Astrometry-testing-data/data}"
CSV_OUTPUT="$REPO_ROOT/eval/blur_scores_all.csv"
OUT_DIR="$REPO_ROOT/eval/best_overall_solve"
# 1. Prioritize binaries in PATH (e.g. inside Docker), fallback to repo binaries
if command -v solve-field >/dev/null 2>&1; then
    SOLVE_FIELD_BIN="solve-field"
elif [[ -f "$REPO_ROOT/solver/solve-field" ]]; then
    SOLVE_FIELD_BIN="$REPO_ROOT/solver/solve-field"
else
    SOLVE_FIELD_BIN="solve-field"
fi

if command -v image2xy >/dev/null 2>&1; then
    IMAGE2XY_BIN="image2xy"
elif [[ -f "$REPO_ROOT/solver/image2xy" ]]; then
    IMAGE2XY_BIN="$REPO_ROOT/solver/image2xy"
else
    IMAGE2XY_BIN="image2xy"
fi


echo "========================================================================="
echo "[overall-solve] Step 1: Running blur-score over all folders in $IMAGES_DIR"
echo "========================================================================="

mkdir -p "$OUT_DIR"

# Run the python benchmark and write directly to the target CSV file
"$PYTHON_BIN" -m astrometry.util.blur_score_benchmark \
    --images-dir "$IMAGES_DIR" \
    --backend source-extractor \
    --jobs 4 \
    --image2xy-path "$IMAGE2XY_BIN" \
    > "$CSV_OUTPUT"

echo "[overall-solve] CSV file updated/overwritten at $CSV_OUTPUT"
echo

echo "========================================================================="
echo "[overall-solve] Step 2: Selecting the image with the highest blur score"
echo "========================================================================="

# Find the best image with the highest score and status='ok' using python
BEST_INFO=$("$PYTHON_BIN" - "$CSV_OUTPUT" <<'PY'
import csv
import sys
import math

csv_path = sys.argv[1]
best_score = -1.0
best_image = None
best_n_sources = 0
best_fwhm = 0.0
best_ellipticity = 0.0

with open(csv_path, newline='') as f:
    reader = csv.DictReader(f)
    for row in reader:
        if row.get("status") != "ok":
            continue
        try:
            score = float(row.get("score", -1.0))
            n_sources = int(row.get("n_sources", 0))
            fwhm = float(row.get("fwhm", 0.0))
            ellipticity = float(row.get("ellipticity", 0.0))
        except ValueError:
            continue
        if not math.isfinite(score):
            continue
        
        # Select highest score; break ties with n_sources
        if score > best_score:
            best_score = score
            best_image = row.get("image")
            best_n_sources = n_sources
            best_fwhm = fwhm
            best_ellipticity = ellipticity
        elif score == best_score and n_sources > best_n_sources:
            best_image = row.get("image")
            best_n_sources = n_sources
            best_fwhm = fwhm
            best_ellipticity = ellipticity

if best_image:
    print(f"{best_image}\t{best_score}\t{best_fwhm}\t{best_ellipticity}\t{best_n_sources}")
else:
    sys.exit("Error: No valid images found in CSV.")
PY
)

BEST_IMAGE=$(echo "$BEST_INFO" | cut -f1)
BEST_SCORE=$(echo "$BEST_INFO" | cut -f2)
BEST_FWHM=$(echo "$BEST_INFO" | cut -f3)
BEST_ELLIPTICITY=$(echo "$BEST_INFO" | cut -f4)
BEST_N_SOURCES=$(echo "$BEST_INFO" | cut -f5)

echo "Selected Best Frame:"
echo "  Image:       $BEST_IMAGE"
echo "  Blur Score:  $BEST_SCORE"
echo "  FWHM:        $BEST_FWHM"
echo "  Ellipticity: $BEST_ELLIPTICITY"
echo "  N Sources:   $BEST_N_SOURCES"
echo

# 3. Create WCS indexes configuration if running in docker
INDEX_CFG="/tmp/indexes_overall.cfg"
if [[ ! -f "$INDEX_CFG" ]]; then
    echo "[overall-solve] Generating solver indexes config at $INDEX_CFG..."
    printf "%s\n" \
      "add_path /usr/local/data" \
      "index index-4119.fits" \
      "index index-4118.fits" \
      "index index-4117.fits" \
      "index index-4116.fits" \
      "index index-4115.fits" \
      "index index-4114.fits" \
      "index index-4113.fits" \
      "index index-4112.fits" \
      "index index-4111.fits" \
      "index index-4110.fits" \
      "index index-4109.fits" \
      "index index-4108.fits" \
      "index index-4107.fits" \
      > "$INDEX_CFG"
fi

# Determine image dimensions for solve-field flags
IMAGE_WIDTH=""
IMAGE_HEIGHT=""
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

image_name="$(basename "$BEST_IMAGE")"
image_stem="${image_name%.*}"
BASE_NAME="$(printf '%s' "$image_stem" | tr -c 'A-Za-z0-9._-' '_')"
AXY_FILE="$OUT_DIR/axy/$BASE_NAME.axy"
XYLS_FILE="$OUT_DIR/axy/$BASE_NAME.xyls"
mkdir -p "$OUT_DIR/axy" "$OUT_DIR/solve"

echo "========================================================================="
echo "[overall-solve] Step 3: Preparing .xyls/.axy source list"
echo "========================================================================="

"$SOLVE_FIELD_BIN" \
    --overwrite \
    --no-plots \
    --just-augment \
    --use-source-extractor \
    -w "$IMAGE_WIDTH" \
    -e "$IMAGE_HEIGHT" \
    -X X_IMAGE -Y Y_IMAGE -s MAG_AUTO -a \
    --axy "$AXY_FILE" \
    --keep-xylist "$XYLS_FILE" \
    --dir "$OUT_DIR/axy" \
    --out "$BASE_NAME" \
    "$BEST_IMAGE"

echo "[overall-solve] Sources extracted and saved to $XYLS_FILE"
echo

echo "========================================================================="
echo "[overall-solve] Step 4: Solving the best image with parallelized index shards"
echo "========================================================================="

# Set environment variables for the solver to run parallel index sharding
export ASTROMETRY_INDEX_SHARDS="threads"
export ASTROMETRY_INDEX_SHARD_WORKERS=4
export ASTROMETRY_INDEX_SHARD_TRACE=1
export ASTROMETRY_INDEX_SHARD_METRICS="$OUT_DIR/$BASE_NAME.index-shard-metrics.csv"

# Run the solver, outputting directly to the terminal
"$SOLVE_FIELD_BIN" \
    --config "$INDEX_CFG" \
    --depth 10-20 \
    -L 2.5 -H 3.2 -u arcsecperpix \
    --overwrite \
    --no-plots \
    -N none \
    -w "$IMAGE_WIDTH" \
    -e "$IMAGE_HEIGHT" \
    -X X_IMAGE -Y Y_IMAGE -s MAG_AUTO -a \
    --dir "$OUT_DIR/solve" \
    --out "$BASE_NAME" \
    "$XYLS_FILE"

echo
echo "========================================================================="
echo "[overall-solve] Step 5: Final Result Summary"
echo "========================================================================="
SOLVED_FILE="$OUT_DIR/solve/$BASE_NAME.solved"
if [[ -f "$SOLVED_FILE" ]] && [[ "$(od -An -tu1 -N1 "$SOLVED_FILE" | tr -d '[:space:]')" == "1" ]]; then
    echo "SOLVE SUCCESSFUL!"
    echo "Best Image: $BEST_IMAGE"
    echo "Score:      $BEST_SCORE"
    echo "Output dir: $OUT_DIR/solve"
else
    echo "SOLVE FAILED!"
    echo "Best Image: $BEST_IMAGE"
    echo "Score:      $BEST_SCORE"
fi
echo "========================================================================="
