from __future__ import annotations

from .model import SolveJob, SolveOptions


def build_solve_field_command(options: SolveOptions, job: SolveJob) -> list[str]:
    out_base = job.image_path.stem

    command = [
        str(options.solve_field_bin),
        "--config", str(options.astrometry_cfg),
        "--dir", str(job.solver_output_dir),
        "--out", out_base,
        "--cpulimit", str(options.cpulimit_seconds),
    ]

    if options.overwrite:
        command.append("--overwrite")

    if options.no_plots:
        command.append("--no-plots")

    if options.keep_temp:
        command.append("--no-delete-temp")

    command.extend(options.optional_args)
    command.append(str(job.image_path))

    return command
