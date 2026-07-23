#!/usr/bin/env python3

"""Run TinyDB's representative suite with one or two engine revisions."""

from __future__ import annotations

import argparse
import atexit
import csv
import errno
import fcntl
import hashlib
import json
import math
import os
import pathlib
import random
import shutil
import statistics
import struct
import subprocess
import tempfile
import time
from collections import defaultdict
from typing import Iterable


CHILD_FIELDS = (
    "scenario",
    "family",
    "trial_seed",
    "dataset_id",
    "metric",
    "unit",
    "scope",
    "trial",
    "observation",
    "value",
)
RAW_FIELDS = ("variant", *CHILD_FIELDS)

FS_IOC_FIEMAP = 0xC020660B
FIEMAP_FLAG_SYNC = 0x00000001
FIEMAP_EXTENT_LAST = 0x00000001
FIEMAP_EXTENT_SHARED = 0x00002000
FIEMAP_HEADER = struct.Struct("=QQIIII")
FIEMAP_EXTENT = struct.Struct("=QQQQQIIII")

T_CRITICAL_95 = {
    1: 12.706,
    2: 4.303,
    3: 3.182,
    4: 2.776,
    5: 2.571,
    6: 2.447,
    7: 2.365,
    8: 2.306,
    9: 2.262,
    10: 2.228,
    11: 2.201,
    12: 2.179,
    13: 2.160,
    14: 2.145,
    15: 2.131,
    16: 2.120,
    17: 2.110,
    18: 2.101,
    19: 2.093,
    20: 2.086,
    21: 2.080,
    22: 2.074,
    23: 2.069,
    24: 2.064,
    25: 2.060,
    26: 2.056,
    27: 2.052,
    28: 2.048,
    29: 2.045,
    30: 2.042,
}


def positive_integer(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="mode", required=True)

    run = subparsers.add_parser("run", help="measure one engine revision")
    run.add_argument("binary", type=pathlib.Path)

    compare = subparsers.add_parser("compare", help="run a paired A/B comparison")
    compare.add_argument("baseline", type=pathlib.Path)
    compare.add_argument("candidate", type=pathlib.Path)
    compare.add_argument("--candidate-cache-mib", type=positive_integer)

    for subparser in (run, compare):
        subparser.add_argument("--output", type=pathlib.Path)
        subparser.add_argument("--family")
        subparser.add_argument("--filter")
        subparser.add_argument("--seed", type=int, default=0x54494E594442)
    return parser.parse_args()


def scenario_matrix(binary: pathlib.Path, args: argparse.Namespace) -> list[dict[str, str]]:
    command = [str(binary), "--list"]
    if args.family:
        command += ["--family", args.family]
    if args.filter:
        command += ["--filter", args.filter]
    result = subprocess.run(command, check=True, text=True, capture_output=True)
    rows = list(csv.DictReader(result.stdout.splitlines()))
    if not rows:
        raise ValueError("benchmark binary exposed no selected scenarios")
    return rows


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while block := source.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def fsync_directory(path: pathlib.Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def stream_copy(source: pathlib.Path, destination: pathlib.Path, mode: int) -> str:
    digest = hashlib.sha256()
    source_fd = os.open(source, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    destination_fd = os.open(
        destination,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
        mode,
    )
    try:
        while True:
            block = os.read(source_fd, 1024 * 1024)
            if not block:
                break
            digest.update(block)
            view = memoryview(block)
            while view:
                written = os.write(destination_fd, view)
                if written == 0:
                    raise OSError(errno.EIO, "zero progress copying file")
                view = view[written:]
        os.fsync(destination_fd)
    finally:
        os.close(destination_fd)
        os.close(source_fd)
    return digest.hexdigest()


def archive_binary(source: pathlib.Path, variant: str, root: pathlib.Path) -> tuple[pathlib.Path, dict[str, object]]:
    source = source.resolve(strict=True)
    status = source.stat()
    before = sha256(source)
    destination = (root / "binaries" / variant / "TinyDB_bench").resolve()
    destination.parent.mkdir(parents=True, exist_ok=False)
    copied = stream_copy(source, destination, status.st_mode & 0o777)
    after = sha256(source)
    if copied != before or after != before:
        raise ValueError(f"benchmark binary changed while archiving {source}")
    destination_status = destination.stat()
    if destination_status.st_nlink != 1:
        raise ValueError("archived benchmark binary must have one link")
    fsync_directory(destination.parent)
    return destination, {
        "original_path": str(source),
        "archived_path": str(destination),
        "sha256": copied,
        "size": destination_status.st_size,
    }


def fiemap(path: pathlib.Path) -> dict[str, object]:
    extent_capacity = 512
    start = 0
    extent_count = 0
    shared_extents = 0
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
        try:
            while True:
                buffer = bytearray(FIEMAP_HEADER.size + extent_capacity * FIEMAP_EXTENT.size)
                FIEMAP_HEADER.pack_into(
                    buffer, 0, start, (1 << 64) - 1 - start, FIEMAP_FLAG_SYNC, 0, extent_capacity, 0
                )
                fcntl.ioctl(descriptor, FS_IOC_FIEMAP, buffer, True)
                _, _, _, mapped, _, _ = FIEMAP_HEADER.unpack_from(buffer)
                if mapped == 0:
                    break
                final_logical = start
                final_flags = 0
                for index in range(mapped):
                    extent = FIEMAP_EXTENT.unpack_from(
                        buffer, FIEMAP_HEADER.size + index * FIEMAP_EXTENT.size
                    )
                    logical, _, length, _, _, flags, _, _, _ = extent
                    extent_count += 1
                    shared_extents += int(bool(flags & FIEMAP_EXTENT_SHARED))
                    final_logical = logical + length
                    final_flags = flags
                if final_flags & FIEMAP_EXTENT_LAST:
                    break
                if final_logical <= start:
                    raise OSError(errno.EIO, "FIEMAP did not advance")
                start = final_logical
        finally:
            os.close(descriptor)
    except OSError as error:
        if error.errno not in (errno.EINVAL, errno.ENOTTY, errno.EOPNOTSUPP, errno.ENOSYS):
            raise
        return {"available": False, "reason": str(error), "extent_count": None, "shared_extents": None}
    return {
        "available": True,
        "reason": None,
        "extent_count": extent_count,
        "shared_extents": shared_extents,
    }


def family_members(database: pathlib.Path) -> list[pathlib.Path]:
    members: list[pathlib.Path] = []
    wal_name = database.name + "-wal"
    archive_prefix = wal_name + "."
    for entry in database.parent.iterdir():
        if entry.name == database.name or entry.name == wal_name:
            members.append(entry)
        elif entry.name.startswith(archive_prefix) and entry.name.endswith(".segment"):
            generation = entry.name[len(archive_prefix) : -len(".segment")]
            if generation.isdecimal():
                members.append(entry)
    if database not in members:
        raise ValueError(f"fixture has no database file: {database}")
    return sorted(members, key=lambda path: path.name)


def family_manifest(database: pathlib.Path) -> dict[str, object]:
    files: list[dict[str, object]] = []
    identity = hashlib.sha256()
    for member in family_members(database):
        status = member.stat(follow_symlinks=False)
        if not member.is_file() or member.is_symlink():
            raise ValueError(f"fixture member is not a regular file: {member}")
        if status.st_nlink != 1:
            raise ValueError(f"fixture member has {status.st_nlink} links: {member}")
        content_hash = sha256(member)
        record = {
            "name": member.name,
            "size": status.st_size,
            "sha256": content_hash,
            "allocated_bytes": status.st_blocks * 512,
            "fiemap": fiemap(member),
        }
        files.append(record)
        identity.update(member.name.encode())
        identity.update(b"\0")
        identity.update(str(status.st_size).encode())
        identity.update(b"\0")
        identity.update(content_hash.encode())
        identity.update(b"\n")
    return {"family_id": identity.hexdigest(), "database": database.name, "files": files}


def freeze_fixture(database: pathlib.Path, manifest: dict[str, object]) -> None:
    for record in manifest["files"]:
        os.chmod(database.parent / str(record["name"]), 0o400)
    fsync_directory(database.parent)


def copy_fixture(source: pathlib.Path, destination: pathlib.Path, manifest: dict[str, object]) -> dict[str, object]:
    destination.parent.mkdir(parents=True, exist_ok=False)
    copied_files = []
    for record in manifest["files"]:
        name = str(record["name"])
        source_member = source.parent / name
        destination_member = destination.parent / name
        digest = stream_copy(source_member, destination_member, 0o600)
        if digest != record["sha256"]:
            raise ValueError(f"canonical fixture changed while copying {source_member}")
        status = destination_member.stat(follow_symlinks=False)
        if status.st_size != record["size"] or status.st_nlink != 1:
            raise ValueError(f"working fixture metadata differs for {destination_member}")
        extent = fiemap(destination_member)
        if extent["available"] and extent["shared_extents"]:
            raise ValueError(f"working fixture contains shared extents: {destination_member}")
        copied_files.append(
            {
                "name": name,
                "size": status.st_size,
                "sha256": digest,
                "allocated_bytes": status.st_blocks * 512,
                "fiemap": extent,
            }
        )
    fsync_directory(destination.parent)
    return {"family_id": manifest["family_id"], "files": copied_files}


def remove_working_directory(directory: pathlib.Path) -> None:
    for path in sorted(directory.rglob("*"), reverse=True):
        if path.is_dir():
            path.rmdir()
        else:
            path.unlink()
    directory.rmdir()


def build_fixture(
    binary: pathlib.Path, database: pathlib.Path, scenario: str, seed: int
) -> dict[str, object]:
    database.parent.mkdir(parents=True, exist_ok=False)
    subprocess.run(
        [str(binary), "--scenario", scenario, "--build-fixture", str(database), "--seed", str(seed)],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    manifest = family_manifest(database)
    freeze_fixture(database, manifest)
    return manifest


def derive_trial_seed(base_seed: int, scenario: str, trial: int) -> int:
    identity = hashlib.sha256(f"{scenario}\0{trial}".encode()).digest()
    return (base_seed ^ int.from_bytes(identity[:8], "little")) & ((1 << 64) - 1)


def dataset_id(scenario: dict[str, str], seed: int) -> str:
    identity = hashlib.sha256()
    identity.update(b"tinydb-benchmark-dataset-v1\0")
    identity.update(json.dumps(scenario, sort_keys=True, separators=(",", ":")).encode())
    identity.update(b"\0")
    identity.update(str(seed).encode())
    return identity.hexdigest()


def balanced_orders(trials: int, scenario_index: int, generator: random.Random) -> list[list[str]]:
    baseline_first = (trials + (1 if scenario_index % 2 == 0 else 0)) // 2
    orders = [["baseline", "candidate"]] * baseline_first
    orders += [["candidate", "baseline"]] * (trials - baseline_first)
    generator.shuffle(orders)
    return orders


def read_child_rows(destination: pathlib.Path) -> list[dict[str, str]]:
    with (destination / "samples.csv").open(newline="") as source:
        rows = list(csv.DictReader(source))
    if not rows:
        raise ValueError(f"trial emitted no samples: {destination}")
    return rows


def run_trial(
    binary: pathlib.Path,
    variant: str,
    scenario: str,
    trial: int,
    trial_seed: int,
    fixture: pathlib.Path,
    manifest: dict[str, object],
    logical_dataset_id: str,
    root: pathlib.Path,
    page_cache_bytes: int | None,
) -> tuple[list[dict[str, str]], dict[str, object], dict[str, object]]:
    destination = root / "runs" / variant / scenario / f"trial-{trial:02d}"
    working_directory = root / "working" / variant / scenario / f"trial-{trial:02d}"
    working_database = working_directory / fixture.name
    audit = copy_fixture(fixture, working_database, manifest)
    print(f"[{variant} {trial + 1}] {scenario}", flush=True)
    command = [
        str(binary),
        "--scenario",
        scenario,
        "--run-trial",
        str(working_database),
        "--dataset-id",
        logical_dataset_id,
        "--seed",
        str(trial_seed),
        "--trial-index",
        str(trial),
        "--output",
        str(destination),
    ]
    if page_cache_bytes is not None:
        command += ["--page-cache-bytes", str(page_cache_bytes)]
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
    rows = read_child_rows(destination)
    for row in rows:
        if set(row) != set(CHILD_FIELDS):
            raise ValueError(f"unexpected child sample schema: {sorted(row)}")
        expected = {
            "scenario": scenario,
            "trial_seed": str(trial_seed),
            "dataset_id": logical_dataset_id,
            "trial": str(trial),
        }
        for field, value in expected.items():
            if row[field] != value:
                raise ValueError(f"benchmark emitted {field}={row[field]!r}; expected {value!r}")
        if row["scope"] not in ("trial", "observation"):
            raise ValueError(f"unknown sample scope {row['scope']!r}")
        value = float(row["value"])
        if not math.isfinite(value):
            raise ValueError(f"non-finite sample for {scenario}.{row['metric']}")
        row["variant"] = variant
    (destination / "fixture-audit.json").write_text(json.dumps(audit, indent=2) + "\n")
    child_metadata = json.loads((destination / "metadata.json").read_text())
    remove_working_directory(working_directory)
    return rows, audit, child_metadata


def nearest_rank(values: Iterable[float], percentile: float) -> float:
    ordered = sorted(values)
    if not ordered:
        raise ValueError("cannot summarize an empty population")
    return ordered[math.ceil(percentile * len(ordered)) - 1]


def summarize_values(values: list[float]) -> dict[str, float | int]:
    return {
        "samples": len(values),
        "mean": statistics.fmean(values),
        "stddev": statistics.stdev(values) if len(values) > 1 else 0.0,
        "minimum": min(values),
        "p50": statistics.median(values),
        "p95": nearest_rank(values, 0.95),
        "p99": nearest_rank(values, 0.99),
        "maximum": max(values),
    }


def write_raw(root: pathlib.Path, rows: list[dict[str, str]]) -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    with (root / "samples.csv").open("w", newline="") as destination:
        writer = csv.DictWriter(destination, fieldnames=RAW_FIELDS)
        writer.writeheader()
        writer.writerows({field: row[field] for field in RAW_FIELDS} for row in rows)
    trial_rows = [row for row in rows if row["scope"] == "trial"]
    observation_rows = [row for row in rows if row["scope"] == "observation"]
    with (root / "observations.csv").open("w", newline="") as destination:
        writer = csv.DictWriter(destination, fieldnames=RAW_FIELDS)
        writer.writeheader()
        writer.writerows({field: row[field] for field in RAW_FIELDS} for row in observation_rows)
    return trial_rows, observation_rows


def write_observation_summary(root: pathlib.Path, rows: list[dict[str, str]]) -> None:
    groups: dict[tuple[str, str, str, str], list[float]] = defaultdict(list)
    for row in rows:
        groups[(row["variant"], row["scenario"], row["metric"], row["unit"])].append(float(row["value"]))
    fields = ("variant", "scenario", "metric", "unit", "samples", "mean", "p50", "p95", "p99")
    with (root / "observation-summary.csv").open("w", newline="") as destination:
        writer = csv.DictWriter(destination, fieldnames=fields)
        writer.writeheader()
        for (variant, scenario, metric, unit), values in sorted(groups.items()):
            summary = summarize_values(values)
            writer.writerow(
                {
                    "variant": variant,
                    "scenario": scenario,
                    "metric": metric,
                    "unit": unit,
                    "samples": summary["samples"],
                    "mean": summary["mean"],
                    "p50": summary["p50"],
                    "p95": summary["p95"],
                    "p99": summary["p99"],
                }
            )


def write_standalone_summary(root: pathlib.Path, rows: list[dict[str, str]]) -> list[dict[str, object]]:
    groups: dict[tuple[str, str, str, str], list[float]] = defaultdict(list)
    for row in rows:
        groups[(row["family"], row["scenario"], row["metric"], row["unit"])].append(float(row["value"]))
    fields = (
        "family",
        "scenario",
        "metric",
        "unit",
        "samples",
        "mean",
        "stddev",
        "minimum",
        "p50",
        "p95",
        "p99",
        "maximum",
    )
    output: list[dict[str, object]] = []
    with (root / "summary.csv").open("w", newline="") as destination:
        writer = csv.DictWriter(destination, fieldnames=fields)
        writer.writeheader()
        for (family, scenario, metric, unit), values in sorted(groups.items()):
            row: dict[str, object] = {
                "family": family,
                "scenario": scenario,
                "metric": metric,
                "unit": unit,
                **summarize_values(values),
            }
            writer.writerow(row)
            output.append(row)
    return output


def pair_trial_samples(rows: list[dict[str, str]]) -> list[tuple[tuple[str, ...], float, float]]:
    fields = ("scenario", "trial_seed", "dataset_id", "metric", "unit", "trial")
    pending: dict[tuple[str, ...], dict[str, float]] = defaultdict(dict)
    for row in rows:
        key = tuple(row[field] for field in fields)
        variant = row["variant"]
        if variant in pending[key]:
            raise ValueError(f"duplicate {variant} trial sample for {key}")
        pending[key][variant] = float(row["value"])
    pairs = []
    for key, variants in pending.items():
        if variants.keys() != {"baseline", "candidate"}:
            missing = {"baseline", "candidate"} - variants.keys()
            raise ValueError(f"missing {', '.join(sorted(missing))} trial sample for {key}")
        pairs.append((key, variants["baseline"], variants["candidate"]))
    return pairs


def validate_primary_samples(
    rows: list[dict[str, str]], matrix: list[dict[str, str]], variants: Iterable[str]
) -> None:
    counts: dict[tuple[str, str], int] = defaultdict(int)
    primary_metrics = {row["scenario"]: row["primary_metric"] for row in matrix}
    for row in rows:
        if row["metric"] == primary_metrics.get(row["scenario"]):
            counts[(row["variant"], row["scenario"])] += 1
    for variant in variants:
        for scenario in matrix:
            expected = int(scenario["trials"])
            actual = counts[(variant, scenario["scenario"])]
            if actual != expected:
                raise ValueError(
                    f"{variant} emitted {actual} primary samples for {scenario['scenario']}; "
                    f"expected {expected}"
                )


def paired_log_interval(baseline: list[float], candidate: list[float]) -> tuple[float, float, float] | None:
    if len(baseline) != len(candidate) or len(baseline) < 2:
        return None
    if any(value <= 0.0 for value in baseline + candidate):
        return None
    logs = [math.log(candidate_value / baseline_value) for baseline_value, candidate_value in zip(baseline, candidate)]
    mean = statistics.fmean(logs)
    deviation = statistics.stdev(logs)
    critical = T_CRITICAL_95.get(len(logs) - 1, 1.96)
    half_width = critical * deviation / math.sqrt(len(logs))
    return math.exp(mean), math.exp(mean - half_width), math.exp(mean + half_width)


def assessment(direction: str, threshold: float, low: float, high: float) -> str:
    if low >= 1.0 - threshold and high <= 1.0 + threshold:
        return "equivalent"
    if direction == "higher":
        if low > 1.0 + threshold:
            return "improved"
        if high < 1.0 - threshold:
            return "regressed"
    else:
        if high < 1.0 - threshold:
            return "improved"
        if low > 1.0 + threshold:
            return "regressed"
    return "inconclusive"


def improvement_interval(direction: str, ratio: float, low: float, high: float) -> tuple[float, float, float]:
    if direction == "lower":
        return (1.0 - ratio) * 100.0, (1.0 - high) * 100.0, (1.0 - low) * 100.0
    return (ratio - 1.0) * 100.0, (low - 1.0) * 100.0, (high - 1.0) * 100.0


def write_comparison(
    root: pathlib.Path, rows: list[dict[str, str]], matrix: list[dict[str, str]]
) -> list[dict[str, object]]:
    scenario_specs = {row["scenario"]: row for row in matrix}
    groups: dict[tuple[str, str, str], tuple[list[float], list[float]]] = {}
    for key, baseline, candidate in pair_trial_samples(rows):
        identity = (key[0], key[3], key[4])
        if identity not in groups:
            groups[identity] = ([], [])
        groups[identity][0].append(baseline)
        groups[identity][1].append(candidate)

    fields = (
        "scenario",
        "metric",
        "unit",
        "role",
        "direction",
        "meaningful_difference",
        "assessment",
        "paired_trials",
        "baseline_median",
        "candidate_median",
        "paired_ratio_geomean",
        "ratio_ci95_low",
        "ratio_ci95_high",
        "improvement_percent",
        "improvement_ci95_low",
        "improvement_ci95_high",
    )
    output: list[dict[str, object]] = []
    with (root / "comparison.csv").open("w", newline="") as destination:
        writer = csv.DictWriter(destination, fieldnames=fields)
        writer.writeheader()
        for (scenario, metric, unit), (baseline, candidate) in sorted(groups.items()):
            spec = scenario_specs[scenario]
            primary = metric == spec["primary_metric"]
            interval = paired_log_interval(baseline, candidate)
            ratio, low, high = interval if interval else (math.nan, math.nan, math.nan)
            direction = spec["primary_direction"] if primary else "neutral"
            threshold = float(spec["meaningful_difference"]) if primary else math.nan
            result = assessment(direction, threshold, low, high) if primary and interval else "diagnostic"
            if primary and interval:
                improvement, improvement_low, improvement_high = improvement_interval(direction, ratio, low, high)
            else:
                improvement = improvement_low = improvement_high = math.nan
            row = {
                "scenario": scenario,
                "metric": metric,
                "unit": unit,
                "role": "primary" if primary else "diagnostic",
                "direction": direction,
                "meaningful_difference": threshold,
                "assessment": result,
                "paired_trials": len(baseline),
                "baseline_median": statistics.median(baseline),
                "candidate_median": statistics.median(candidate),
                "paired_ratio_geomean": ratio,
                "ratio_ci95_low": low,
                "ratio_ci95_high": high,
                "improvement_percent": improvement,
                "improvement_ci95_low": improvement_low,
                "improvement_ci95_high": improvement_high,
            }
            writer.writerow(row)
            output.append(row)
    return output


def format_value(value: float, unit: str) -> str:
    if unit == "bytes":
        magnitude = abs(value)
        if magnitude >= 1 << 30:
            return f"{value / (1 << 30):.2f} GiB"
        if magnitude >= 1 << 20:
            return f"{value / (1 << 20):.2f} MiB"
        if magnitude >= 1 << 10:
            return f"{value / (1 << 10):.2f} KiB"
        return f"{value:.0f} B"
    if unit == "ratio":
        return f"{value:.3f}"
    if "second" in unit and "/" in unit:
        return f"{value:,.0f}"
    if unit in ("milliseconds", "microseconds"):
        return f"{value:,.2f}"
    return f"{value:,.3g}"


def write_report(
    root: pathlib.Path,
    mode: str,
    matrix: list[dict[str, str]],
    standalone: list[dict[str, object]],
    comparisons: list[dict[str, object]],
    scenario_timings: list[dict[str, object]],
    elapsed: float,
    candidate_cache_bytes: int | None,
) -> None:
    lines = ["# TinyDB benchmark report", "", f"Mode: `{mode}`  ", f"Wall time: {elapsed / 60.0:.1f} minutes", ""]
    specs = {row["scenario"]: row for row in matrix}
    declared_cache_bytes = sorted({int(row["cache_bytes"]) for row in matrix})
    declared_cache = ", ".join(format_value(value, "bytes") for value in declared_cache_bytes)
    if mode == "compare" and candidate_cache_bytes is not None:
        lines += [
            f"Page cache: baseline {declared_cache} (scenario setting); "
            f"candidate {format_value(candidate_cache_bytes, 'bytes')} (override)",
            "",
        ]
    else:
        lines += [f"Page cache: {declared_cache} (scenario setting)", ""]
    if mode == "compare" and any(row["fixture_policy"] == "native" for row in matrix):
        lines += [
            "Fixture layouts: `shared` uses one baseline-built database family; "
            "`native` uses a logically identical family built by each variant.",
            "",
        ]
    if mode == "run":
        lookup = {(str(row["scenario"]), str(row["metric"])): row for row in standalone}
        lines += ["## Primary results", "", "| Scenario | Median | p95 | Trials |", "|---|---:|---:|---:|"]
        for scenario, spec in specs.items():
            row = lookup[(scenario, spec["primary_metric"])]
            lines.append(
                f"| `{scenario}` | {format_value(float(row['p50']), str(row['unit']))} | "
                f"{format_value(float(row['p95']), str(row['unit']))} | {row['samples']} |"
            )
    else:
        primary = [row for row in comparisons if row["role"] == "primary"]
        lines += [
            "## Primary comparison",
            "",
            "| Scenario | Baseline | Candidate | Effect (95% CI) | Result |",
            "|---|---:|---:|---:|---|",
        ]
        for row in primary:
            effect = float(row["improvement_percent"])
            low = float(row["improvement_ci95_low"])
            high = float(row["improvement_ci95_high"])
            lines.append(
                f"| `{row['scenario']}` | {format_value(float(row['baseline_median']), str(row['unit']))} | "
                f"{format_value(float(row['candidate_median']), str(row['unit']))} | "
                f"{effect:+.1f}% ({low:+.1f}% to {high:+.1f}%) | **{row['assessment']}** |"
            )

    diagnostic_names = {
        "commit_latency_p95",
        "wal_amplification",
        "combined_cache_resident_bytes",
        "storage_read_bytes",
        "storage_write_bytes",
        "workload_storage_read_amplification",
        "process_rss_growth",
    }
    lines += ["", "## Memory, I/O, and latency diagnostics", ""]
    if mode == "run":
        diagnostic = [row for row in standalone if row["metric"] in diagnostic_names]
        lines += ["| Scenario | Metric | Median |", "|---|---|---:|"]
        for row in diagnostic:
            lines.append(
                f"| `{row['scenario']}` | `{row['metric']}` | "
                f"{format_value(float(row['p50']), str(row['unit']))} |"
            )
    else:
        diagnostic = [row for row in comparisons if row["metric"] in diagnostic_names]
        lines += ["| Scenario | Metric | Baseline | Candidate |", "|---|---|---:|---:|"]
        for row in diagnostic:
            lines.append(
                f"| `{row['scenario']}` | `{row['metric']}` | "
                f"{format_value(float(row['baseline_median']), str(row['unit']))} | "
                f"{format_value(float(row['candidate_median']), str(row['unit']))} |"
            )
    lines.append("")
    if mode == "compare":
        lines.append("Primary confidence intervals use paired trial log-ratios.")
    lines += ["Commit and churn observations remain nested diagnostics and are not treated as independent trials.", ""]
    read_scenarios = [
        scenario for scenario, spec in specs.items() if spec["workload"] in ("point_read", "scan", "io_read")
    ]
    if read_scenarios:
        if mode == "run":
            readahead_lookup = {
                (str(row["scenario"]), str(row["metric"])): float(row["p50"]) for row in standalone
            }
            readahead_variant = "current"
        else:
            readahead_lookup = {
                (str(row["scenario"]), str(row["metric"])): float(row["candidate_median"])
                for row in comparisons
            }
            readahead_variant = "candidate"
        lines += [
            "## Read-ahead behavior",
            "",
            f"Values below describe the `{readahead_variant}` engine.",
            "",
            "| Scenario | Streams active/started | Pages submitted | Ready | Waited | Unused | Drops queue/budget | I/O failures | Peak staging |",
            "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
        ]
    for scenario in read_scenarios:

        def read_metric(metric: str) -> float:
            return readahead_lookup[(scenario, metric)]

        lines.append(
            f"| `{scenario}` | {read_metric('read_streams_activated'):,.0f}/"
            f"{read_metric('read_streams_started'):,.0f} | "
            f"{read_metric('readahead_pages_submitted'):,.0f} | "
            f"{read_metric('readahead_pages_ready'):,.0f} | "
            f"{read_metric('readahead_pages_waited'):,.0f} | "
            f"{read_metric('readahead_pages_unused'):,.0f} | "
            f"{read_metric('readahead_queue_drops'):,.0f}/"
            f"{read_metric('readahead_budget_drops'):,.0f} | "
            f"{read_metric('readahead_io_failures'):,.0f} | "
            f"{format_value(read_metric('readahead_maximum_staging_bytes'), 'bytes')} |"
        )
    if read_scenarios:
        lines.append("")
    lines += [
        "## Runtime by scenario",
        "",
        "| Scenario | Fixture build | Trial execution | Total |",
        "|---|---:|---:|---:|",
    ]
    for timing in sorted(scenario_timings, key=lambda row: float(row["total_seconds"]), reverse=True):
        lines.append(
            f"| `{timing['scenario']}` | {float(timing['fixture_seconds']):.1f}s | "
            f"{float(timing['trial_seconds']):.1f}s | {float(timing['total_seconds']):.1f}s |"
        )
    lines.append("")
    (root / "report.md").write_text("\n".join(lines))


def estimate_fixture_bytes(matrix: list[dict[str, str]], mode: str) -> int:
    total = 0
    maximum = 0
    for row in matrix:
        logical = int(row["rows"]) * (int(row["key_bytes"]) + int(row["value_bytes"]) + 32)
        size = max(logical, int(row["target_bytes"]))
        copies = 2 if mode == "compare" and row["fixture_policy"] == "native" else 1
        total += copies * size
        maximum = max(maximum, size)
    return total + 2 * maximum + (512 << 20)


def default_output() -> pathlib.Path:
    return pathlib.Path(tempfile.gettempdir()) / "tinydb-benchmark-latest"


def rebase_output_paths(value: object, source: pathlib.Path, destination: pathlib.Path) -> object:
    """Rewrite paths recorded while a managed result lived in its staging directory."""

    source_text = os.fspath(source)
    if isinstance(value, str):
        if value == source_text or value.startswith(source_text + os.sep):
            return os.fspath(destination) + value[len(source_text) :]
        return value
    if isinstance(value, list):
        return [rebase_output_paths(item, source, destination) for item in value]
    if isinstance(value, dict):
        return {
            key: rebase_output_paths(item, source, destination) for key, item in value.items()
        }
    return value


class ManagedOutput:
    """Build a default result beside its predecessor, then replace it on success."""

    def __init__(self, destination: pathlib.Path):
        self.destination = destination
        destination.parent.mkdir(parents=True, exist_ok=True)
        self.path = pathlib.Path(tempfile.mkdtemp(prefix=f".{destination.name}.", dir=destination.parent))
        self.active = True
        atexit.register(self.cleanup)

    def cleanup(self) -> None:
        if self.active:
            shutil.rmtree(self.path, ignore_errors=True)
            self.active = False

    def publish(self) -> pathlib.Path:
        for metadata_path in self.path.rglob("*.json"):
            metadata = json.loads(metadata_path.read_text())
            metadata = rebase_output_paths(metadata, self.path, self.destination)
            metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
        if self.destination.exists():
            if (
                self.destination.is_symlink()
                or not self.destination.is_dir()
                or not (self.destination / "report.md").is_file()
                or not (self.destination / "metadata.json").is_file()
            ):
                raise SystemExit(f"refusing to replace non-benchmark output: {self.destination}")
            shutil.rmtree(self.destination)
        self.path.rename(self.destination)
        fsync_directory(self.destination.parent)
        self.active = False
        return self.destination


def main() -> None:
    args = parse_args()
    started = time.monotonic()
    managed_output = ManagedOutput(default_output()) if args.output is None else None
    root = managed_output.path if managed_output is not None else args.output
    candidate_cache_bytes = (
        args.candidate_cache_mib << 20
        if args.mode == "compare" and args.candidate_cache_mib is not None
        else None
    )
    if managed_output is None:
        root.mkdir(parents=True, exist_ok=False)

    binaries: dict[str, pathlib.Path] = {}
    artifacts: dict[str, dict[str, object]] = {}
    if args.mode == "run":
        binaries["current"], artifacts["current"] = archive_binary(args.binary, "current", root)
    else:
        binaries["baseline"], artifacts["baseline"] = archive_binary(args.baseline, "baseline", root)
        binaries["candidate"], artifacts["candidate"] = archive_binary(args.candidate, "candidate", root)

    matrices = {variant: scenario_matrix(binary, args) for variant, binary in binaries.items()}
    matrix = next(iter(matrices.values()))
    if any(candidate != matrix for candidate in matrices.values()):
        raise SystemExit("engine revisions expose different benchmark matrices")
    required_free = estimate_fixture_bytes(matrix, args.mode)
    available_free = shutil.disk_usage(root).free
    if available_free < required_free:
        raise SystemExit(
            f"benchmark needs {required_free / (1 << 30):.2f} GiB free; "
            f"{available_free / (1 << 30):.2f} GiB is available"
        )

    generator = random.Random(args.seed)
    scenarios = matrix.copy()
    generator.shuffle(scenarios)
    rows: list[dict[str, str]] = []
    fixtures: list[dict[str, object]] = []
    execution: list[dict[str, object]] = []
    scenario_timings: list[dict[str, object]] = []
    child_metadata: dict[str, dict[str, object]] = {}

    for scenario_index, scenario_row in enumerate(scenarios):
        scenario_started = time.monotonic()
        scenario = scenario_row["scenario"]
        fixture_policy = scenario_row["fixture_policy"]
        if fixture_policy not in ("shared", "native"):
            raise ValueError(f"unknown fixture policy {fixture_policy!r} for {scenario}")
        logical_dataset_id = dataset_id(scenario_row, args.seed)
        fixture_started = time.monotonic()
        if args.mode == "run":
            fixture_builders = {"current": "current"}
        elif fixture_policy == "native":
            fixture_builders = {variant: variant for variant in binaries}
        else:
            fixture_builders = {"shared": "baseline"}

        physical_fixtures: dict[str, tuple[pathlib.Path, dict[str, object]]] = {}
        for layout, builder_variant in fixture_builders.items():
            fixture = root / "fixtures" / scenario / layout / "database.db"
            manifest = build_fixture(binaries[builder_variant], fixture, scenario, args.seed)
            physical_fixtures[layout] = (fixture, manifest)
            fixtures.append(
                {
                    "scenario": scenario,
                    "fixture_policy": fixture_policy,
                    "dataset_id": logical_dataset_id,
                    "layout": layout,
                    "builder_variant": builder_variant,
                    "path": str(fixture),
                    "manifest": manifest,
                }
            )
        if args.mode == "run":
            fixtures_by_variant = {"current": physical_fixtures["current"]}
        elif fixture_policy == "native":
            fixtures_by_variant = {variant: physical_fixtures[variant] for variant in binaries}
        else:
            fixtures_by_variant = {variant: physical_fixtures["shared"] for variant in binaries}
        fixture_seconds = time.monotonic() - fixture_started
        trials = int(scenario_row["trials"])
        if args.mode == "run":
            orders = [["current"] for _ in range(trials)]
        else:
            orders = balanced_orders(trials, scenario_index, generator)
        for trial, order in enumerate(orders):
            seed = derive_trial_seed(args.seed, scenario, trial)
            trial_record: dict[str, object] = {
                "scenario": scenario,
                "fixture_policy": fixture_policy,
                "dataset_id": logical_dataset_id,
                "trial": trial,
                "trial_seed": seed,
                "order": order,
                "runs": [],
            }
            execution.append(trial_record)
            for variant in order:
                fixture, manifest = fixtures_by_variant[variant]
                run_started = time.monotonic()
                emitted, audit, metadata = run_trial(
                    binaries[variant],
                    variant,
                    scenario,
                    trial,
                    seed,
                    fixture,
                    manifest,
                    logical_dataset_id,
                    root,
                    candidate_cache_bytes if variant == "candidate" else None,
                )
                trial_record["runs"].append(
                    {
                        "variant": variant,
                        "family_id": audit["family_id"],
                        "runner_seconds": time.monotonic() - run_started,
                        "benchmark_seconds": metadata.get("elapsed_seconds"),
                    }
                )
                rows.extend(emitted)
                child_metadata.setdefault(variant, metadata)
        total_seconds = time.monotonic() - scenario_started
        scenario_timings.append(
            {
                "scenario": scenario,
                "fixture_seconds": fixture_seconds,
                "trial_seconds": total_seconds - fixture_seconds,
                "total_seconds": total_seconds,
            }
        )

    trial_rows, observation_rows = write_raw(root, rows)
    validate_primary_samples(trial_rows, matrix, binaries)
    write_observation_summary(root, observation_rows)
    standalone: list[dict[str, object]] = []
    comparisons: list[dict[str, object]] = []
    if args.mode == "run":
        standalone = write_standalone_summary(root, trial_rows)
    else:
        comparisons = write_comparison(root, trial_rows, matrix)

    elapsed = time.monotonic() - started
    write_report(
        root,
        args.mode,
        matrix,
        standalone,
        comparisons,
        scenario_timings,
        elapsed,
        candidate_cache_bytes,
    )
    for variant, metadata in child_metadata.items():
        artifacts[variant]["reported_build"] = {
            field: metadata[field]
            for field in (
                "engine_git_commit",
                "engine_git_dirty",
                "harness_git_commit",
                "harness_git_dirty",
                "build_type",
                "compiler",
            )
        }
    metadata = {
        "suite_version": 8,
        "mode": args.mode,
        "seed": args.seed,
        "candidate_page_cache_override_bytes": candidate_cache_bytes,
        "elapsed_seconds": elapsed,
        "binaries": artifacts,
        "system": next(iter(child_metadata.values()))["system"],
        "fixture_storage": next(iter(child_metadata.values()))["fixture_storage"],
        "scenario_matrix": matrix,
        "execution": execution,
        "scenario_timings": scenario_timings,
        "fixtures": fixtures,
        "trial_samples": len(trial_rows),
        "nested_observations": len(observation_rows),
        "note": (
            "Every trial used a private userspace copy of an immutable canonical fixture. Shared-layout "
            "scenarios use one baseline-built family for both variants; native-layout scenarios use each "
            "variant's own physical family under one validated logical dataset identity. "
            "A/B confidence intervals use paired trial log-ratios; nested commit and churn observations "
            "are descriptive only."
        ),
    }
    (root / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    if managed_output is not None:
        root = managed_output.publish()
    print(f"Benchmark report: {root / 'report.md'}")


if __name__ == "__main__":
    main()
