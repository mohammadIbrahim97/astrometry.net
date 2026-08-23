from __future__ import annotations

import sys
from pathlib import Path

from .logging_utils import get_logger
from .model import SolveJob

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

    log.info("Discovered %d image(s) in %s recursive=%s", len(images), directory, recursive)
    return images


def build_input_dir_manifest(
    input_dir: Path,
    output_manifest: Path,
    recursive: bool = False,
    limit: int | None = None,
) -> list[Path]:
    images = discover_images(input_dir, recursive=recursive)
    images = apply_limit(images, limit)

    if not images:
        raise RuntimeError(f"No supported images found in input directory: {input_dir}")

    write_manifest(output_manifest, images)
    return images


def build_workload_manifest(
    dataset_root: Path,
    categories: list[str],
    workload: str,
    output_manifest: Path,
) -> list[Path]:
    if not dataset_root.is_dir():
        raise FileNotFoundError(f"Demo dataset root not found: {dataset_root}")

    images = select_demo_workload(dataset_root, categories, workload)

    if not images:
        raise RuntimeError(f"Workload produced no images: {workload}")

    write_manifest(output_manifest, images)
    return images


def select_demo_workload(dataset_root: Path, categories: list[str], workload: str) -> list[Path]:
    log.info("Building demo workload: %s", workload)

    if workload.startswith("mixed"):
        count_text = workload.removeprefix("mixed")
        count = parse_positive_int(count_text, f"Invalid mixed workload: {workload}")

        # Round-robin keeps the demo set category-balanced.
        buckets = [
            discover_images(dataset_root / category)
            for category in categories
            if (dataset_root / category).is_dir()
        ]

        mixed: list[Path] = []
        max_bucket_size = max((len(bucket) for bucket in buckets), default=0)

        for index in range(max_bucket_size):
            for bucket in buckets:
                if index < len(bucket):
                    mixed.append(bucket[index])
                if len(mixed) >= count:
                    return mixed

        return mixed

    if workload.startswith("category:"):
        parts = workload.split(":")
        if len(parts) != 3:
            raise ValueError(
                f"Invalid category workload '{workload}'. "
                "Use category:<name>:<count>."
            )

        _, category, count_text = parts
        count = parse_positive_int(count_text, f"Invalid category workload: {workload}")
        return discover_images(dataset_root / category)[:count]

    raise ValueError(
        f"Unsupported workload '{workload}'. "
        "Use mixed5, mixed10, mixed20, or category:<name>:<count>."
    )


def read_manifest(path: Path | str) -> list[Path]:
    if str(path) == "-":
        return parse_manifest_lines(sys.stdin.read().splitlines(), source="stdin")

    manifest_path = Path(path).expanduser()

    if not manifest_path.is_file():
        raise FileNotFoundError(f"Manifest not found: {manifest_path}")

    return parse_manifest_lines(
        manifest_path.read_text(encoding="utf-8").splitlines(),
        source=str(manifest_path),
    )


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
    path.write_text("\n".join(str(image) for image in images) + "\n", encoding="utf-8")

    log.info("Manifest written: %s", path)
    log.info("Manifest image count: %d", len(images))


def apply_limit(images: list[Path], limit: int | None) -> list[Path]:
    if limit is None or limit <= 0:
        return images
    return images[:limit]


def make_jobs(images: list[Path], run_dir: Path) -> list[SolveJob]:
    jobs: list[SolveJob] = []

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


def parse_positive_int(text: str, message: str) -> int:
    try:
        value = int(text)
    except ValueError as error:
        raise ValueError(message) from error

    if value < 1:
        raise ValueError(message)

    return value
