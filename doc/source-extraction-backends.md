# Source Extraction Backends

Astrometry.net can now run source extraction through three paths:

- `simplexy`: the built-in C extractor.  This is the default and has no extra
  runtime dependency.
- `sep`: the in-process SEP library backend.  This avoids spawning the external
  Source Extractor executable, but Astrometry.net must be built with SEP support.
- `source-extractor`: the existing external executable backend.

Use the explicit backend option where available:

```bash
solver/augment-xylist --source-backend simplexy -i image.fits -o out.axy
solver/augment-xylist --source-backend sep -i image.fits -o out.axy
solver/augment-xylist --source-backend source-extractor -i image.fits -o out.axy
```

The older `--use-sep` and `--use-source-extractor` flags are still accepted as
aliases.

`image2xy` supports the in-process backends directly:

```bash
solver/image2xy --source-backend simplexy -o out.xyls image.fits
solver/image2xy --source-backend sep -o out.xyls image.fits
```

To build with SEP from a local SEP install:

```bash
make -C solver HAVE_SEP=yes \
    SEP_INC="-I/path/to/sep/include" \
    SEP_LIB="-L/path/to/sep/lib -lsep"
```

To avoid a runtime `LD_LIBRARY_PATH` requirement, link the static SEP archive:

```bash
make -C solver HAVE_SEP=yes \
    SEP_INC="-I/path/to/sep/include" \
    SEP_LIB="/path/to/sep/lib/libsep.a"
```

If SEP was installed into a non-system prefix, set the runtime library path when
running dynamically linked tools:

```bash
LD_LIBRARY_PATH=/path/to/sep/lib solver/image2xy --source-backend sep -o out.xyls image.fits
```

For blur-score benchmarking:

```bash
LD_LIBRARY_PATH=/path/to/sep/lib \
python3 -m astrometry.util.blur_score_benchmark \
    --images-dir /path/to/images \
    --backend source-extractor \
    --backend simplexy \
    --backend sep \
    --jobs 4
```

The benchmark CSV includes backend, elapsed time, status, FWHM, ellipticity,
bright source count, score, and error text.

`--jobs` processes multiple images in parallel for each backend.  The default is
4 because it was the fastest setting on the local full-dataset SEP benchmark.
Use `--jobs 1` for serial timing, and keep the value modest for the external
Source Extractor backend because every job starts its own extractor process.
