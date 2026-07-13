# Blur Backend Benchmark Summary

Date: 2026-05-31

Dataset: `/home/mohammadibrahim/Astrometry-testing-data/data`

This benchmark only compares source extraction and blur-score backends. It does
not run the missing-objects experiment.

## Setup

- External Source Extractor: `/usr/bin/source-extractor`
- SEP source: `https://github.com/sep-developers/sep`
- SEP install used for the run: `/tmp/sep-install`
- Solver binaries linked with SEP static archive:
  `/tmp/sep-install/lib/libsep.a`

Static-link verification:

- `ldd solver/image2xy` does not list `libsep.so`
- `ldd solver/augment-xylist` does not list `libsep.so`
- `solver/image2xy --source-backend sep` writes `SRCBACK=sep`
- `solver/augment-xylist --source-backend sep` writes `SRCBACK=sep`

## CSV Outputs

- `eval/blur_backends_sample_3.csv`
- `eval/blur_backends_10.csv`
- `eval/blur_backends_source_sep_100.csv`

## First 10 Entries, All Backends

Command:

```bash
LD_LIBRARY_PATH=/tmp/sep-install/lib /usr/bin/time -v \
python3 -m astrometry.util.blur_score_benchmark \
    --images-dir /home/mohammadibrahim/Astrometry-testing-data/data \
    --backend all \
    --max-images 10 \
    > /tmp/blur_backends_10.csv
```

Results:

| Backend | OK | Error | Median elapsed (s) | Total elapsed (s) | Median score | Median FWHM | Score corr vs Source Extractor | FWHM corr vs Source Extractor |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| source-extractor | 9 | 1 | 0.7809 | 8.056 | 0.1343 | 15.1700 | 1.0000 | 1.0000 |
| simplexy | 9 | 1 | 24.5523 | 226.462 | 0.3549 | 6.5766 | -0.8844 | -0.9411 |
| sep | 9 | 1 | 0.7200 | 7.086 | 0.1580 | 10.2327 | 0.9716 | 0.9746 |

The one error is an invalid image file:
`frame0001-objs.png`.

## First 100 Entries, Source Extractor vs SEP

Command:

```bash
LD_LIBRARY_PATH=/tmp/sep-install/lib /usr/bin/time -v \
python3 -m astrometry.util.blur_score_benchmark \
    --images-dir /home/mohammadibrahim/Astrometry-testing-data/data \
    --backend source-extractor \
    --backend sep \
    --max-images 100 \
    > /tmp/blur_backends_source_sep_100.csv
```

Results:

| Backend | OK | Error | Median elapsed (s) | Mean elapsed (s) | Total elapsed (s) | Median score | Median FWHM | Median ellipticity | Median bright sources |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| source-extractor | 99 | 1 | 0.5149 | 0.6259 | 62.391 | 0.2271 | 7.7050 | 0.2770 | 41 |
| sep | 99 | 1 | 0.4081 | 0.6090 | 60.290 | 0.3633 | 5.5831 | 0.3341 | 47 |

Correlation across the 99 common successful images:

| Metric | Correlation |
| --- | ---: |
| blur score | 0.9726 |
| FWHM | 0.9711 |
| ellipticity | 0.9570 |
| bright source count | 0.9898 |

## Full Dataset, Source Extractor vs SEP

Command:

```bash
/usr/bin/time -v python3 -m astrometry.util.blur_score_benchmark \
    --images-dir /home/mohammadibrahim/Astrometry-testing-data/data \
    --backend source-extractor \
    --backend sep \
    > eval/blur_backends_source_sep_full.csv \
    2> eval/blur_backends_source_sep_full.log
```

Results:

| Backend | OK | Insufficient | Error | Median elapsed (s) | Mean elapsed (s) | Total elapsed (s) | Median score | Median FWHM | Median ellipticity | Median bright sources |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| source-extractor | 2515 | 131 | 1 | 0.1020 | 0.1049 | 277.572 | 0.5648 | 4.2600 | 0.1505 | 20 |
| sep | 2601 | 45 | 1 | 0.0944 | 0.0983 | 260.208 | 0.5080 | 4.6449 | 0.1747 | 25 |

Pair status counts:

| Source Extractor status | SEP status | Images |
| --- | --- | ---: |
| ok | ok | 2515 |
| insufficient_sources | ok | 86 |
| insufficient_sources | insufficient_sources | 45 |
| error | error | 1 |

Correlation across the 2515 common successful images:

| Metric | Pearson | Spearman |
| --- | ---: | ---: |
| blur score | 0.8809 | 0.8482 |
| FWHM | 0.7336 | 0.7039 |
| ellipticity | 0.9360 | 0.9424 |
| bright source count | 0.9637 | 0.9715 |

Median absolute differences on common successful images:

| Metric | Median absolute difference | 95th percentile absolute difference |
| --- | ---: | ---: |
| blur score | 0.0839 | 0.2831 |
| FWHM | 0.7608 | 5.7309 |
| ellipticity | 0.0231 | 0.1370 |
| bright source count | 4.0000 | 13.0000 |

The one error is the same invalid image file for both backends:
`frame0001-objs.png`.

## Full Dataset, SEP With 4 Parallel Jobs

Command:

```bash
/usr/bin/time -v python3 -m astrometry.util.blur_score_benchmark \
    --images-dir /home/mohammadibrahim/Astrometry-testing-data/data \
    --backend sep \
    --jobs 4 \
    > eval/blur_sep_parallel.csv \
    2> eval/blur_sep_parallel.log
```

Results:

| Backend | Jobs | OK | Insufficient | Error | Wall time (s) | CPU usage | Median elapsed per image (s) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| sep | 4 | 2601 | 45 | 1 | 120.626 | 350% | 0.1810 |

The parallel run produced the same statuses and score values as the serial SEP
run from `eval/blur_backends_source_sep_full.csv`.  The higher per-image
elapsed values are expected because multiple extractor jobs compete for CPU at
the same time; wall-clock time is the important number for the full-folder run.

## Full Dataset, SEP With 8 Parallel Jobs

Command:

```bash
/usr/bin/time -v python3 -m astrometry.util.blur_score_benchmark \
    --images-dir /home/mohammadibrahim/Astrometry-testing-data/data \
    --backend sep \
    --jobs 8 \
    > eval/blur_sep_parallel_jobs8.csv \
    2> eval/blur_sep_parallel_jobs8.log
```

Results:

| Backend | Jobs | OK | Insufficient | Error | Wall time (s) | CPU usage | Median elapsed per image (s) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| sep | 8 | 2601 | 45 | 1 | 167.569 | 502% | 0.5064 |

The 8-job run produced the same statuses and score values as the 4-job run, but
it was slower.  On this machine and dataset, `--jobs 4` is currently the better
parallel setting.

## Interpretation

SEP is now the strongest candidate for the blur-score backend, but the full
dataset shows it is not a drop-in identical replacement for Source Extractor.
It is faster, succeeds on more images, and tracks ellipticity/source-count well.
The blur-score correlation is good but not perfect, and the FWHM correlation is
only moderate on the full dataset. Before making SEP the default, calibrate the
SEP score thresholds against known good and bad images.

The built-in `simplexy` backend is not a good blur-score replacement yet. On the
first 10 entries it is much slower and its score/FWHM correlations against
Source Extractor are negative. It may still be useful for solving, but it should
not be used as the blur scoring baseline without more work on the FWHM model.
