[toc]

# Astrobench readme

## Purpose
Astrobench is a repository-integrated CLI controller for running multiple independent Astrometry.net `solve-field` jobs in a structured and reproducible way.

Astrobench focuses on three practical goals:

1. execute many `solve-field` processes concurrently across CPU cores;
2. organize outputs, logs, manifests, and result files per run;
3. compare serial execution against concurrent execution when proof or validation is needed.

Astrobench does **not** modify the internal Astrometry.net solver. Each image is still processed by a normal `solve-field` process.
Profiling is no longer part of the Astrobench CLI.

## Required prerequisites
Astrobench expects an Astrometry.net source repository and an installed Astrometry.net runtime.

Recommended workspace shape:

```text
workspace/
├── astrometry.net/
│   ├── bin/
│   │   └── astrobench
│   ├── lib/python/astrometry/benchmark/
│   └── examples/benchmark/
│       └── astrobench.toml
├── astrometry-install/
│   ├── bin/
│   │   └── solve-field
│   └── etc/
│       └── astrometry.cfg
└── profiling/
    ├── data/
    │   ├── categorized_5img_set/      # optional demo/test data
    │   └── ser_data/                  # optional extracted video frame data
    └── runs/
        └── astrobench/
```

Required:

| Requirement                             | Purpose                             |
| --------------------------------------- | ----------------------------------- |
| `astrometry-install/bin/solve-field`    | solver executable                   |
| `astrometry-install/etc/astrometry.cfg` | solver backend config               |
| workspace root with `runs/`             | output location for Astrobench runs |
| Python 3.11+                            | required for `tomllib`              |

Optional:

| Optional item                          | Purpose                                                       |
| -------------------------------------- | ------------------------------------------------------------- |
| `profiling/data/categorized_5img_set/` | demo workload commands such as `mixed5`, `mixed10`, `mixed20` |
| `profiling/data/ser_data/`             | extracted scientific video frame data                         |
| external profilers                     | direct analysis outside Astrobench                            |

## Repository layout
```
astrometry.net/
├── bin/
│   └── astrobench
├── examples/benchmark/
│   └── astrobench.toml
├── lib/python/astrometry/
│   ├── __init__.py
│   └── benchmark/
│       ├── __init__.py
│       ├── cli.py
│       ├── command.py
│       ├── compare.py
│       ├── config.py
│       ├── executor.py
│       ├── logging_utils.py
│       ├── manifest.py
│       ├── model.py
│       ├── pathscan.py
│       ├── recorder.py
│       ├── runner.py
│       ├── settings.py
│       ├── sources.py
│       ├── status.py
│       └── worker_suggest.py
└── doc/astrobench/
    └── Astrobench_readme.md
```

## Concept diagram

```plantUML
@startuml
title Astrobench Component Architecture

skinparam componentStyle rectangle
skinparam shadowing false
skinparam packageStyle rectangle
skinparam linetype ortho

actor "User" as User

package "Astrobench CLI Layer" {
  component "bin/astrobench" as Entry
  component "cli.py\ncommand dispatch" as CLI
}

package "Configuration + Path Layer" {
  component "settings.py\nlocal config" as Settings
  component "config.py\nconfig merge" as Config
  component "pathscan.py\npath discovery" as Pathscan
}

package "Input Layer" {
  component "sources.py\nimage discovery" as Sources
  component "manifest.py\nmanifest compatibility" as Manifest
}

package "Execution Layer" {
  component "executor.py\nserial / concurrent scheduling" as Executor
  component "runner.py\nsingle job lifecycle" as Runner
  component "command.py\nsolve-field command builder" as Command
}

package "Result Layer" {
  component "recorder.py\nCSV / JSON writer" as Recorder
  component "compare.py\nserial-vs-concurrent report" as Compare
  component "status.py\nstatus / inspection" as Status
}

package "External Astrometry.net Runtime" {
  component "solve-field process 1" as Solve1
  component "solve-field process 2" as Solve2
  component "solve-field process N" as SolveN
  artifact "astrometry.cfg" as AstrometryCfg
}

database "Astrobench Run Directory" as RunDir {
}

User --> Entry
Entry --> CLI

CLI --> Settings
CLI --> Config
CLI --> Pathscan

CLI --> Sources
Sources --> Manifest

CLI --> Executor
Executor --> Runner
Runner --> Command

Command --> Solve1
Command --> Solve2
Command --> SolveN
Command --> AstrometryCfg

Runner --> Recorder
Executor --> Recorder
CLI --> Compare
CLI --> Status

Recorder --> RunDir
Compare --> RunDir
Status --> RunDir

note right of Executor
outer parallelism only
multiple independent solve-field processes
no internal solver modification
end note

note bottom of RunDir
astrobench.log
manifest.txt
run.json
results.csv
jobs/
comparison.md
comparison.csv
end note

@enduml
```

## Command overview
Run all commands from the Astrometry.net repository root unless using the absolute path to `bin/astrobench`.

```bash
cd /path/to/astrometry.net
chmod +x ./bin/astrobench
```

Check available commands:

```bash
./bin/astrobench --help
```

Command set:

```text
init
config
doctor
suggest-workers
manifest
run
compare
benchmark
status
inspect
```


## Setup commands

Create local config:

```bash
./bin/astrobench init
```

Check setup:

```bash
./bin/astrobench doctor
```

Suggest worker counts:

```bash
./bin/astrobench suggest-workers
```

Set a default input folder:

```bash
./bin/astrobench config set input.default_dir /path/to/images
```

Run using the configured default input folder:

```bash
./bin/astrobench run --workers aggressive
```


## Running image batches

### Run from an image folder

```bash
./bin/astrobench run \
  --input-dir /path/to/images \
  --workers aggressive \
  --run-label my_batch
```

With limit:

```bash
./bin/astrobench run \
  --input-dir /path/to/images \
  --limit 20 \
  --workers 4 \
  --run-label sample20
```

Recursive scan:

```bash
./bin/astrobench run \
  --input-dir /path/to/images \
  --recursive \
  --workers aggressive \
  --run-label recursive_batch
```

### Run from a manifest

A manifest is a text file with one image path per line:

```text
/path/to/image_001.png
/path/to/image_002.png
/path/to/image_003.fits
```

Run manifest:

```bash
./bin/astrobench run \
  --manifest /path/to/manifest.txt \
  --workers 4 \
  --run-label manifest_batch
```

### Demo workload mode

Demo workloads are still supported for controlled local tests:

```bash
./bin/astrobench manifest --workload mixed5
./bin/astrobench manifest --workload mixed10
./bin/astrobench manifest --workload mixed20
./bin/astrobench manifest --workload category:clean:2
```

This mode depends on the optional demo dataset under:

```text
workspace/data/categorized_5img_set/
```

### Comparing serial and concurrent execution

Use `compare` when you need a serial-vs-concurrent proof report.

```bash
./bin/astrobench compare \
  --input-dir /path/to/images \
  --limit 20 \
  --workers official \
  --run-label my_comparison
```

The legacy alias still works:

```bash
./bin/astrobench benchmark \
  --input-dir /path/to/images \
  --limit 20 \
  --workers official \
  --run-label my_comparison
```

Comparison output includes:

```text
comparison.md
comparison.csv
serial_w1/run.json
serial_w1/results.csv
parallel_wN/run.json
parallel_wN/results.csv
```

A clean comparison proof requires:

1. same job count;
2. same success count;
3. lower elapsed time for the concurrent run.


## Status and inspection

Show latest run:

```bash
./bin/astrobench status --run latest
```

Inspect latest run:

```bash
./bin/astrobench inspect --run latest
```

Show a specific run directory:

```bash
./bin/astrobench status --run /path/to/run-directory
./bin/astrobench inspect --run /path/to/run-directory
```

`status` reports:

```text
run directory
run type
worker count
job count
success count
failure count
elapsed time
result file paths
```

`inspect` reports:

```text
failed jobs
return codes
error kinds
stderr log paths
```

## Output directory

Default output root:

```text
workspace/runs/astrobench/
```

For the current project workspace this may resolve to:

```text
/home/dsongt/HAW/26/PROINF/profiling/runs/astrobench/
```

### Single run output

```text
runs/astrobench/<timestamp>_<label>_wN/
├── astrobench.log
├── manifest.txt
├── run.json
├── results.csv
└── jobs/
    ├── img_0001/
    │   ├── command.txt
    │   ├── stdout.log
    │   ├── stderr.log
    │   └── solver_output/
    └── img_0002/
        ├── command.txt
        ├── stdout.log
        ├── stderr.log
        └── solver_output/
```

### Comparison output

```text
runs/astrobench/<timestamp>_compare_<label>_wN/
├── astrobench.log
├── config_snapshot.toml
├── manifest.txt
├── system_snapshot.json
├── serial_w1/
│   ├── run.json
│   ├── results.csv
│   └── jobs/
├── parallel_wN/
│   ├── run.json
│   ├── results.csv
│   └── jobs/
├── comparison.csv
└── comparison.md
```

## Worker-count guidance

Use:

```bash
./bin/astrobench suggest-workers
```

Worker modes:

| Mode         | Meaning                                             |
| ------------ | --------------------------------------------------- |
| `official`   | conservative setting for clean comparison runs      |
| `aggressive` | high utilization while leaving one logical CPU free |
| `stress`     | all usable CPUs; may cause contention or timeouts   |

Examples:

```bash
./bin/astrobench run --input-dir /path/to/images --workers official
./bin/astrobench run --input-dir /path/to/images --workers aggressive
./bin/astrobench run --input-dir /path/to/images --workers stress
```

For formal comparison, start with:

```bash
./bin/astrobench compare \
  --input-dir /path/to/images \
  --workers official
```

## Tips and cautions

Use `suggest-workers` before choosing worker counts.

For clean proof runs:

```text
use official workers first
avoid changing input set between serial and concurrent runs
check success count before interpreting speedup
```

If concurrent runs produce more failures than serial runs:

```text
reduce workers
increase solver.timeout_seconds
inspect stderr logs
check memory and I/O pressure
```

Do not commit generated artifacts:

```text
.astrobench.toml
runs/
solver_output/
results.csv
run.json
comparison.csv
comparison.md
```
