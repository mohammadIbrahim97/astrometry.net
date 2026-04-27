from __future__ import annotations

import logging
import sys
from pathlib import Path


LOG_FORMAT = "[%(asctime)s] %(levelname)-5s %(name)s %(message)s"
DATE_FORMAT = "%H:%M:%S"


def configure_logging(
    *,
    verbose: bool = False,
    quiet: bool = False,
    log_file: Path | None = None,
) -> None:
    """
    Configure Astrobench logging.

    Terminal output is the primary CLI interface.
    File output is used for reproducible run diagnostics.
    """
    root = logging.getLogger()
    root.handlers.clear()

    if quiet:
        level = logging.WARNING
    elif verbose:
        level = logging.DEBUG
    else:
        level = logging.INFO

    root.setLevel(level)

    stream_handler = logging.StreamHandler(sys.stderr)
    stream_handler.setLevel(level)
    stream_handler.setFormatter(logging.Formatter(LOG_FORMAT, DATE_FORMAT))
    root.addHandler(stream_handler)

    if log_file is not None:
        log_file.parent.mkdir(parents=True, exist_ok=True)

        file_handler = logging.FileHandler(log_file, mode="a", encoding="utf-8")
        file_handler.setLevel(logging.DEBUG)
        file_handler.setFormatter(logging.Formatter(LOG_FORMAT, DATE_FORMAT))
        root.addHandler(file_handler)


def get_logger(name: str) -> logging.Logger:
    return logging.getLogger(name)


def attach_run_log(log_file: Path) -> None:
    """
    Add a run-specific file handler after the run directory is known.

    This is used because CLI logging starts before the final run directory exists.
    """
    root = logging.getLogger()

    for handler in root.handlers:
        if isinstance(handler, logging.FileHandler):
            if Path(handler.baseFilename) == log_file:
                return

    log_file.parent.mkdir(parents=True, exist_ok=True)
    file_handler = logging.FileHandler(log_file, mode="a", encoding="utf-8")
    file_handler.setLevel(logging.DEBUG)
    file_handler.setFormatter(logging.Formatter(LOG_FORMAT, DATE_FORMAT))
    root.addHandler(file_handler)
