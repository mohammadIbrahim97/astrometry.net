# Index shard parallel metrics

Measured in Docker image `astrometrynet/solver:test` built from branch
`phase-10-threaded-index-shards`.

Input image:

- `/src/Astrometry-testing-data/data/20260303200006652/frame0003.png`

Index set:

- `/usr/local/data/index-4119.fits` through `/usr/local/data/index-4107.fits`
- 13 candidate indexes

Common solve options:

```sh
solve-field \
  --config /tmp/indexes.cfg \
  --no-plots \
  --overwrite \
  -N none \
  --depth 10-20 \
  -L 0.18 -H 340 -u arcsecperpix \
  frame0003.png
```

## Wall time

| Mode | Workers | Wall time | Output relationship |
| --- | ---: | ---: | --- |
| default | 1 | 79.392 s | Original early-exit behavior |
| serial-compat | 1 | 89.213 s | Matches default outputs |
| serial-reduce | 1 | 90.225 s | Reduction-mode reference |
| threads | 4 | 60.409 s | Matches serial-reduce outputs |

## Speedup

The apples-to-apples parallel comparison is `serial-reduce` vs `threads`,
because both modes use reduction semantics.

| Comparison | Speedup | Time saved | Wall-time reduction |
| --- | ---: | ---: | ---: |
| threads vs serial-reduce | 1.49x | 29.816 s | 33.0% |
| threads vs default | 1.31x | 18.983 s | 23.9% |

`threads vs default` is useful as a rough performance signal, but it is not an
output-equivalent comparison. Default stops after the first accepted solution
at `index-4108`; threaded reduction also evaluates later candidates and merges
the `index-4107` solution.

## Output checks

```text
default vs serial-compat: OK
serial-reduce vs threads: OK
default vs threads: DIFFERENT
```

The `default vs threads` difference is expected for the current implementation:
threaded reduction does not emulate original early exit. It merged two
solutions (`index-4108` and `index-4107`), while default stopped at
`index-4108`.

## Metrics CSV

Both reduction modes wrote one CSV header and one data row per candidate index:

| Mode | Header lines | Total CSV lines | Candidate metric rows |
| --- | ---: | ---: | ---: |
| serial-reduce | 1 | 14 | 13 |
| threads | 1 | 14 | 13 |

This confirms that the metrics writer is active only when requested and that
the threaded CSV header race is fixed for this run.

## Full-depth smoke result

A full-depth PNG run was also measured without `--depth 10-20`:

| Mode | Workers | Wall time | Notes |
| --- | ---: | ---: | --- |
| default | 1 | 88.976 s | Solved with `index-4108` |
| threads | 4 | 66.042 s | Solved, reduction output differs from default |

This gives a rough full-image signal of 1.35x speedup and 25.8% lower wall
time, but the output-equivalent benchmark remains `serial-reduce` vs `threads`.

## 45-second video estimate

A 45-second video at 60 fps contains:

```text
45 * 60 = 2700 frames
```

If every frame were solved independently, the solve-field stage alone would
take:

| Mode | Per-frame time | 2700-frame total |
| --- | ---: | ---: |
| threads reduction | 60.409 s | 45.3 hours |
| serial-reduce | 90.225 s | 67.7 hours |
| default early-exit | 79.392 s | 59.5 hours |
| full-depth threads smoke | 66.042 s | 49.5 hours |
| full-depth default smoke | 88.976 s | 66.7 hours |

The existing blur-score preprocessing benchmark is much cheaper than solving:
SEP with 4 parallel jobs processed 2647 images in 120.626 s. At the same
throughput, 2700 frames would take about 123 s, or 2.05 minutes.

That makes the rough full pipeline estimate for solving every frame:

```text
preprocess all frames + threaded solve all frames
~= 2.05 minutes + 45.3 hours
~= 45.34 hours
```

This is not a practical operating mode. The intended video workflow should be:

1. Extract frames.
2. Downsample and blur-score all frames in parallel.
3. Pick a small number of sharp candidate frames.
4. Run solve-field only on those selected frames.

Using the measured numbers, solving only `K` selected frames is roughly:

```text
total ~= 123 s + K * 60.409 s
```

| Selected frames | Estimated total |
| ---: | ---: |
| 1 | 3.1 minutes |
| 3 | 5.1 minutes |
| 5 | 7.1 minutes |
| 10 | 12.1 minutes |

If the camera is stable, one solved best frame may be enough. If the camera
moves, solve selected keyframes instead of every video frame, then use tracking
or interpolation between keyframes.

## Latency isolation

The 60-90 second measurements include a broad 13-index candidate search. A
single-image latency test with only the known matching index (`index-4108`) is
much faster:

| Input | Indexes | Wall time | Notes |
| --- | ---: | ---: | --- |
| PNG | 1 | 4.700 s | Includes image-to-source extraction |
| `.axy` from same PNG | 1 | 0.464 s | Reuses extracted source list |

This means the high latency is not inherent to solving one already prepared
frame. It is dominated by candidate-index search and repeated source
extraction. A video pipeline should keep the source list generated during
preprocessing and pass `.axy`/`.xyls` into the solver for selected keyframes.
