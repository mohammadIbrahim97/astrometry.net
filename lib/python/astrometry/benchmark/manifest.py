from __future__ import annotations

import sys
from pathlib import Path

from .logging_utils import get_logger

log = get_logger(__name__)

IMAGE_EXTENSIONS = {
    ".jpg", ".jpeg", ".png",
    ".fits", ".fit", ".fts",
    ".tif", ".tiff",
}


def discover_images(directory: Path, recursive: bool = False) -> list[Path]:
    if not directory.is_dir():
        raise FileNotFoundError(f"Image directory not found: {directory}")

    iterator = directory.rglob("*") if recursive else directory.iterdir()

    images = sorted(
        path.resolve()
        for path in iterator
        if path.is_file() and path.suffix.lower() in IMAGE_EXTENSIONS
    )

    log.info(
        "Discovered %d image(s) in %s recursive=%s",
        len(images),
        directory,
        recursive,
    )

    return images


def build_input_dir_manifest(
    input_dir: Path,
    output_manifest: Path,
    recursive: bool = False,
    limit: int | None = None,
) -> list[Path]:
    images = discover_images(input_dir, recursive=recursive)

    if limit is not None:
        images = images[:limit]

    if not images:
        raise RuntimeError(f"No supported images found in input directory: {input_dir}")

    write_manifest(output_manifest, images)
    return images


def read_manifest(path: Path | str) -> list[Path]:
    if str(path) == "-":
        return read_manifest_stdin()

    manifest_path = Path(path)

    if not manifest_path.is_file():
        raise FileNotFoundError(f"Manifest not found: {manifest_path}")

    return parse_manifest_lines(
        manifest_path.read_text(encoding="utf-8").splitlines(),
        source=str(manifest_path),
    )


def read_manifest_stdin() -> list[Path]:
    log.info("Reading image path stream from stdin")
    return parse_manifest_lines(sys.stdin.read().splitlines(), source="stdin")


def parse_manifest_lines(lines: list[str], source: str) -> list[Path]:
    images: list[Path] = []

    for line in lines:
        stripped = line.strip()

        if not stripped or stripped.startswith("#"):
            continue

        images.append(Path(stripped).expanduser().resolve())

    if not images:
        raise RuntimeError(f"No image paths received from {source}")

    log.info("Read %d image path(s) from %s", len(images), source)
    return images


def write_manifest(path: Path, images: list[Path]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "\n".join(str(image) for image in images) + "\n",
        encoding="utf-8",
    )

    log.info("Manifest written: %s", path)
    log.info("Manifest image count: %d", len(images))


def build_workload_manifest(
    dataset_root: Path,
    categories: list[str],
    workload: str,
    output_manifest: Path,
) -> list[Path]:
    log.info("Building workload manifest: %s", workload)

    images: list[Path] = []

    if workload.startswith("mixed"):
        count = int(workload.removeprefix("mixed"))
        for category in categories:
            category_images = discover_images(dataset_root / category)
            log.info("Category %-10s -> %d image(s)", category, len(category_images))
            images.extend(category_images)
        images = images[:count]

    elif workload.startswith("category:"):
        _, category, count_text = workload.split(":")
        images = discover_images(dataset_root / category)[:int(count_text)]
        log.info("Category %-10s -> %d selected image(s)", category, len(images))

    else:
        raise ValueError(
            f"Unsupported workload '{workload}'. "
            "Use mixed5, mixed10, mixed20, or category:<name>:<count>."
        )

    write_manifest(output_manifest, images)
    return images





def make_jobs(images: list[Path], run_dir: Path) -> list:
    from .model import SolveJob

    jobs = []

    for index, image_path in enumerate(images, start=1):
        job_id = f"img_{index:04d}"
        job_dir = run_dir / "jobs" / job_id

        jobs.append(
            SolveJob(
                job_index=index,
                job_id=job_id,
                image_path=image_path,
                job_dir=job_dir,
                solver_output_dir=job_dir / "solver_output",
            )
        )

    return jobs
