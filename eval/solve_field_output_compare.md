# solve-field output comparison

This notes the existing solve-field fixtures found in this checkout and shows a
small repeatable baseline run for `util/compare_solve_field_runs.py`.

## Repository fixtures

- `solver/solve-field.c` is the CLI source, with the built binary currently at
  `solver/solve-field`.
- `doc/readme.rst` documents solve-field demo commands for `demo/apod*.jpg`,
  `demo/apod*.xyls`, and `demo/sdss.*`.
- `README.md` contains Docker-oriented solve-field examples using demo images.
- `demo/` contains the most useful local fixtures: APOD JPEGs, matching `.xyls`
  files, `sdss.*`, `m44-*` images, `index-4119.fits`, and `demo/cfg`.
- `solver/test-solver.c`, `solver/test-solver-2.c`,
  `solver/test_multiindex2.c`, and `solver/test_predistort.c` are lower-level
  solver tests or source comments with manual solve-field commands. They are
  not an end-to-end output comparison harness.
- `sdss/testdata/` and `doc/UCAC*_guide/centu1.jpg` provide more sample data,
  but they are less self-contained for a local solve-field output comparison
  than `demo/apod5.xyls` plus `demo/index-4119.fits`.

## Baseline run

Use the xylist fixture to avoid JPEG conversion dependencies and plotting
commands. The default solve-field outputs include `.solved`, `.wcs`, `.match`,
`.corr`, and `.rdls`.

```sh
RUN_ROOT=eval/solve-field-baseline
mkdir -p "$RUN_ROOT/base" "$RUN_ROOT/candidate"

./solver/solve-field \
  --config demo/cfg \
  --no-plots \
  --overwrite \
  --dir "$RUN_ROOT/base" \
  --out apod5 \
  --scale-low 30 \
  demo/apod5.xyls \
  > "$RUN_ROOT/base/solve.log" 2>&1

./solver/solve-field \
  --config demo/cfg \
  --no-plots \
  --overwrite \
  --dir "$RUN_ROOT/candidate" \
  --out apod5 \
  --scale-low 30 \
  demo/apod5.xyls \
  > "$RUN_ROOT/candidate/solve.log" 2>&1

python3 util/compare_solve_field_runs.py \
  "$RUN_ROOT/base" \
  "$RUN_ROOT/candidate" \
  --base apod5 \
  --include-rdls \
  --log-a "$RUN_ROOT/base/solve.log" \
  --log-b "$RUN_ROOT/candidate/solve.log"
```

For comparing two solver builds, run the same command with different
`solve-field` binaries but keep the same `--out` value and separate `--dir`
directories. The comparison script ignores FITS creation dates, history,
checksums, timing-only comments, `TIMEUSED`, temporary paths, timestamps, and
timing-only log lines; differences in WCS values, match/correlation table
contents, marker bytes, domain comments, and domain log lines are still
reported.
