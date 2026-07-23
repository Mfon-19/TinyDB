#!/usr/bin/env python3

"""Run and compare TinyDB's unified benchmark workloads."""

from __future__ import annotations

import argparse
import atexit
import csv
import datetime
import errno
import hashlib
import json
import math
import os
import pathlib
import platform
import random
import shutil
import statistics
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


def engine_argument(value: str) -> tuple[str, pathlib.Path]:
    label, separator, binary = value.partition("=")
    if not separator or not label or not binary:
        raise argparse.ArgumentTypeError("must be LABEL=/path/to/binary")
    if any(character not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_" for character in label):
        raise argparse.ArgumentTypeError("engine label contains unsupported characters")
    return label, pathlib.Path(binary)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="mode", required=True)

    run = subparsers.add_parser("run", help="measure one engine revision")
    run.add_argument("binary", type=pathlib.Path)

    compare = subparsers.add_parser("compare", help="run a paired A/B comparison")
    compare.add_argument("baseline", type=pathlib.Path)
    compare.add_argument("candidate", type=pathlib.Path)
    compare.add_argument("--baseline-cache-mib", type=positive_integer)
    compare.add_argument("--candidate-cache-mib", type=positive_integer)

    cross = subparsers.add_parser("cross", help="compare two or more backend workers")
    cross.add_argument(
        "--engine",
        action="append",
        type=engine_argument,
        required=True,
        help="repeat LABEL=/path/to/binary for each engine",
    )
    cross.add_argument("--baseline", required=True, help="engine label used as the report baseline")

    for subparser in (run, compare, cross):
        subparser.add_argument("--output", type=pathlib.Path)
        subparser.add_argument("--family", action="append")
        subparser.add_argument("--filter")
        subparser.add_argument(
            "--cache-mib",
            type=positive_integer,
            help="override the page cache for every measured engine",
        )
        subparser.add_argument(
            "--profile",
            choices=("smoke", "standard", "soak"),
            default="standard",
        )
        subparser.add_argument(
            "--semantics",
            choices=("durable", "native"),
            default="durable",
        )
        subparser.add_argument("--trials", type=positive_integer)
        subparser.add_argument("--seed", type=int, default=0x54494E594442)
    return parser.parse_args()


def selected_binaries(args: argparse.Namespace) -> tuple[dict[str, pathlib.Path], str | None]:
    if args.mode == "run":
        return {"current": args.binary}, None
    if args.mode == "compare":
        return {"baseline": args.baseline, "candidate": args.candidate}, "baseline"
    binaries = dict(args.engine)
    if len(binaries) != len(args.engine):
        raise ValueError("engine labels must be unique")
    if len(binaries) < 2:
        raise ValueError("cross comparison requires at least two engines")
    if args.baseline not in binaries:
        raise ValueError(f"unknown baseline engine {args.baseline!r}")
    return binaries, args.baseline


def resolve_page_cache_overrides(args: argparse.Namespace) -> dict[str, int | None]:
    common = args.cache_mib << 20 if args.cache_mib is not None else None
    if args.mode == "run":
        return {"current": common}
    if args.mode == "cross":
        return {label: common for label, _ in args.engine}
    return {
        "baseline": args.baseline_cache_mib << 20 if args.baseline_cache_mib is not None else common,
        "candidate": args.candidate_cache_mib << 20 if args.candidate_cache_mib is not None else common,
    }


def scenario_matrix(binary: pathlib.Path, args: argparse.Namespace) -> list[dict[str, str]]:
    command = [str(binary), "--list", "--profile", args.profile]
    if args.family:
        for family in args.family:
            command += ["--family", family]
    if args.filter:
        command += ["--filter", args.filter]
    result = subprocess.run(command, check=True, text=True, capture_output=True)
    rows = list(csv.DictReader(result.stdout.splitlines()))
    if args.trials is not None:
        for row in rows:
            row["trials"] = str(args.trials)
    if not rows:
        raise ValueError("benchmark binary exposed no selected scenarios")
    return rows


def engine_identity(binary: pathlib.Path) -> dict[str, object]:
    result = subprocess.run(
        [str(binary), "--describe"],
        check=True,
        text=True,
        capture_output=True,
    )
    identity = json.loads(result.stdout)
    if (
        set(identity)
        != {
            "backend",
            "format_family",
            "tinydb_qualification",
            "always_durable",
            "engine_revision",
            "engine_dirty",
            "harness_revision",
            "harness_dirty",
            "build_type",
            "compiler",
        }
        or not isinstance(identity["backend"], str)
        or not isinstance(identity["format_family"], str)
        or not isinstance(identity["tinydb_qualification"], bool)
        or not isinstance(identity["always_durable"], bool)
        or not isinstance(identity["engine_revision"], str)
        or not isinstance(identity["engine_dirty"], (bool, type(None)))
        or not isinstance(identity["harness_revision"], str)
        or not isinstance(identity["harness_dirty"], (bool, type(None)))
        or not isinstance(identity["build_type"], str)
        or not isinstance(identity["compiler"], str)
    ):
        raise ValueError(f"invalid worker identity from {binary}")
    return identity


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


def binary_record(path: pathlib.Path, identity: dict[str, object]) -> dict[str, object]:
    path = path.resolve(strict=True)
    status = path.stat()
    return {
        "path": str(path),
        "sha256": sha256(path),
        "size": status.st_size,
        **identity,
    }


def family_members(root: pathlib.Path) -> list[pathlib.Path]:
    members = [
        entry
        for entry in root.rglob("*")
        if entry.is_file() and not entry.is_symlink()
    ]
    if not members:
        raise ValueError(f"fixture has no database files: {root}")
    return sorted(members, key=lambda path: path.relative_to(root).as_posix())


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
        name = member.relative_to(database).as_posix()
        record = {
            "name": name,
            "size": status.st_size,
            "sha256": content_hash,
        }
        files.append(record)
        identity.update(name.encode())
        identity.update(b"\0")
        identity.update(str(status.st_size).encode())
        identity.update(b"\0")
        identity.update(content_hash.encode())
        identity.update(b"\n")
    return {"family_id": identity.hexdigest(), "files": files}


def freeze_fixture(database: pathlib.Path, manifest: dict[str, object]) -> None:
    for record in manifest["files"]:
        os.chmod(database / str(record["name"]), 0o400)
    fsync_directory(database)


def copy_fixture(
    source: pathlib.Path, destination: pathlib.Path, manifest: dict[str, object]
) -> None:
    destination.mkdir(parents=True, exist_ok=False)
    for record in manifest["files"]:
        name = str(record["name"])
        source_member = source / name
        destination_member = destination / name
        destination_member.parent.mkdir(parents=True, exist_ok=True)
        digest = stream_copy(source_member, destination_member, 0o600)
        if digest != record["sha256"]:
            raise ValueError(f"canonical fixture changed while copying {source_member}")
        status = destination_member.stat(follow_symlinks=False)
        if status.st_size != record["size"] or status.st_nlink != 1:
            raise ValueError(f"working fixture metadata differs for {destination_member}")
    fsync_directory(destination)


def build_fixture(
    binary: pathlib.Path,
    database: pathlib.Path,
    scenario: str,
    seed: int,
    profile: str,
    semantics: str,
) -> dict[str, object]:
    database.parent.mkdir(parents=True, exist_ok=False)
    subprocess.run(
        [
            str(binary),
            "--scenario",
            scenario,
            "--build-fixture",
            str(database),
            "--seed",
            str(seed),
            "--profile",
            profile,
            "--semantics",
            semantics,
        ],
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


def balanced_orders(
    variants: list[str],
    trials: int,
    scenario_index: int,
    generator: random.Random,
) -> list[list[str]]:
    if len(variants) == 1:
        return [variants.copy() for _ in range(trials)]
    base = variants.copy()
    generator.shuffle(base)
    orders: list[list[str]] = []
    for trial in range(trials):
        offset = (scenario_index + trial) % len(base)
        order = base[offset:] + base[:offset]
        if (trial // len(base)) % 2:
            order.reverse()
        orders.append(order)
    return orders


def read_child_rows(output: str) -> list[dict[str, str]]:
    rows = list(csv.DictReader(output.splitlines()))
    if not rows:
        raise ValueError("trial emitted no samples")
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
    workspace: pathlib.Path,
    page_cache_bytes: int | None,
    profile: str,
    semantics: str,
) -> list[dict[str, str]]:
    working_directory = workspace / "trials" / variant / scenario / f"trial-{trial:02d}"
    working_database = working_directory / "database"
    copy_fixture(fixture, working_database, manifest)
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
        "--profile",
        profile,
        "--semantics",
        semantics,
    ]
    if page_cache_bytes is not None:
        command += ["--page-cache-bytes", str(page_cache_bytes)]
    completed = subprocess.run(
        command, check=True, text=True, stdout=subprocess.PIPE
    )
    rows = read_child_rows(completed.stdout)
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
    shutil.rmtree(working_directory)
    return rows


def nearest_rank(values: Iterable[float], percentile: float) -> float:
    ordered = sorted(values)
    if not ordered:
        raise ValueError("cannot summarize an empty population")
    return ordered[math.ceil(percentile * len(ordered)) - 1]


def summarize_values(values: list[float]) -> dict[str, float | int]:
    return {
        "samples": len(values),
        "p50": statistics.median(values),
        "p95": nearest_rank(values, 0.95),
    }


def write_results(
    root: pathlib.Path, rows: list[dict[str, str]]
) -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    with (root / "results.csv").open("w", newline="") as destination:
        writer = csv.DictWriter(destination, fieldnames=RAW_FIELDS)
        writer.writeheader()
        writer.writerows({field: row[field] for field in RAW_FIELDS} for row in rows)
    return (
        [row for row in rows if row["scope"] == "trial"],
        [row for row in rows if row["scope"] == "observation"],
    )


def summarize_trials(rows: list[dict[str, str]]) -> list[dict[str, object]]:
    groups: dict[tuple[str, str, str, str, str], list[float]] = defaultdict(list)
    for row in rows:
        groups[
            (
                row["variant"],
                row["family"],
                row["scenario"],
                row["metric"],
                row["unit"],
            )
        ].append(float(row["value"]))
    output: list[dict[str, object]] = []
    for (variant, family, scenario, metric, unit), values in sorted(
        groups.items()
    ):
        output.append(
            {
                "variant": variant,
                "family": family,
                "scenario": scenario,
                "metric": metric,
                "unit": unit,
                **summarize_values(values),
            }
        )
    return output


def pair_trial_samples(
    rows: list[dict[str, str]], baseline: str, candidate: str
) -> list[tuple[tuple[str, ...], float, float]]:
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
        selected = {name: value for name, value in variants.items() if name in (baseline, candidate)}
        if selected.keys() != {baseline, candidate}:
            missing = {baseline, candidate} - selected.keys()
            raise ValueError(f"missing {', '.join(sorted(missing))} trial sample for {key}")
        pairs.append((key, selected[baseline], selected[candidate]))
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


def compare_trials(
    rows: list[dict[str, str]],
    matrix: list[dict[str, str]],
    baseline_variant: str,
    candidate_variants: list[str],
) -> list[dict[str, object]]:
    scenario_specs = {row["scenario"]: row for row in matrix}
    groups: dict[
        tuple[str, str, str, str], tuple[list[float], list[float]]
    ] = {}
    for candidate_variant in candidate_variants:
        for key, baseline, candidate in pair_trial_samples(
            rows, baseline_variant, candidate_variant
        ):
            identity = (candidate_variant, key[0], key[3], key[4])
            if identity not in groups:
                groups[identity] = ([], [])
            groups[identity][0].append(baseline)
            groups[identity][1].append(candidate)

    output: list[dict[str, object]] = []
    for (candidate_variant, scenario, metric, unit), (
        baseline,
        candidate,
    ) in sorted(groups.items()):
        spec = scenario_specs[scenario]
        primary = metric == spec["primary_metric"]
        interval = paired_log_interval(baseline, candidate)
        ratio, low, high = (
            interval
            if interval
            else (
                math.exp(
                    statistics.fmean(
                        math.log(candidate_value / baseline_value)
                        for baseline_value, candidate_value in zip(
                            baseline, candidate
                        )
                    )
                )
                if all(value > 0 for value in baseline + candidate)
                else math.nan,
                math.nan,
                math.nan,
            )
        )
        direction = spec["primary_direction"] if primary else "neutral"
        threshold = float(spec["meaningful_difference"]) if primary else math.nan
        result = (
            assessment(direction, threshold, low, high)
            if primary and interval
            else "insufficient"
            if primary
            else "diagnostic"
        )
        if primary and interval:
            improvement, improvement_low, improvement_high = improvement_interval(
                direction, ratio, low, high
            )
        elif primary and math.isfinite(ratio):
            improvement = (
                (1.0 - ratio) * 100.0
                if direction == "lower"
                else (ratio - 1.0) * 100.0
            )
            improvement_low = improvement_high = math.nan
        else:
            improvement = improvement_low = improvement_high = math.nan
        output.append(
            {
                "baseline": baseline_variant,
                "candidate": candidate_variant,
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
        )
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
    variant_summary: list[dict[str, object]],
    comparisons: list[dict[str, object]],
    scenario_timings: list[dict[str, object]],
    elapsed: float,
    page_cache_overrides: dict[str, int | None],
    comparison_baseline: str | None,
    profile: str,
    semantics: str,
) -> None:
    title = (
        "# TinyDB cross-engine benchmark report"
        if mode == "cross"
        else "# TinyDB benchmark report"
    )
    lines = [
        title,
        "",
        f"Mode: `{mode}`  ",
        f"Profile: `{profile}`  ",
        f"Semantics: `{semantics}`  ",
        f"Wall time: {elapsed / 60.0:.1f} minutes",
        "",
    ]
    specs = {row["scenario"]: row for row in matrix}
    declared_cache_bytes = sorted({int(row["page_cache_bytes"]) for row in matrix})
    declared_cache = ", ".join(format_value(value, "bytes") for value in declared_cache_bytes)

    def cache_description(variant: str) -> str:
        override = page_cache_overrides[variant]
        return (
            f"{format_value(override, 'bytes')} (override)"
            if override is not None
            else f"{declared_cache} (standard matrix)"
        )

    if mode == "compare":
        lines += [
            f"Page cache: baseline {cache_description('baseline')}; "
            f"candidate {cache_description('candidate')}",
            "",
        ]
    elif mode == "cross":
        lines += [
            "Engine configuration: backend factory defaults. A common cache "
            "override, when supplied, applies only to backends that expose one.",
            "",
        ]
    else:
        lines += [f"Page cache: {cache_description('current')}", ""]
    if mode != "run" and any(row["fixture_policy"] == "native" for row in matrix):
        lines += [
            "Fixture layouts: `shared` uses one canonical database family per "
            "compatible format; `native` uses a logically identical family "
            "built by each variant.",
            "",
        ]
    if mode == "run":
        lookup = {
            (str(row["scenario"]), str(row["metric"])): row
            for row in variant_summary
        }
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
            f"Effects are paired against `{comparison_baseline}`.",
            "",
            "| Scenario | Candidate | Baseline | Candidate result | Effect (95% CI) | Result |",
            "|---|---|---:|---:|---:|---|",
        ]
        for row in primary:
            effect = float(row["improvement_percent"])
            low = float(row["improvement_ci95_low"])
            high = float(row["improvement_ci95_high"])
            effect_text = (
                f"{effect:+.1f}% ({low:+.1f}% to {high:+.1f}%)"
                if math.isfinite(low) and math.isfinite(high)
                else f"{effect:+.1f}% (one trial; no CI)"
            )
            lines.append(
                f"| `{row['scenario']}` | `{row['candidate']}` | "
                f"{format_value(float(row['baseline_median']), str(row['unit']))} | "
                f"{format_value(float(row['candidate_median']), str(row['unit']))} | "
                f"{effect_text} | **{row['assessment']}** |"
            )

    portable_scenarios = [
        scenario
        for scenario, spec in specs.items()
        if spec["workload"] == "portable"
    ]
    if mode == "cross" and portable_scenarios:
        values = {
            (str(row["variant"]), str(row["scenario"]), str(row["metric"])): float(
                row["p50"]
            )
            for row in variant_summary
        }
        ratio_lookup = {
            (str(row["candidate"]), str(row["scenario"])): float(
                row["paired_ratio_geomean"]
            )
            for row in comparisons
            if row["role"] == "primary"
        }
        variants = list(dict.fromkeys(str(row["variant"]) for row in variant_summary))
        lines += [
            "",
            "## Portable workload scorecard",
            "",
            "| Scenario | Engine | ops/s | vs baseline | p50 call µs | p99 call µs | "
            "CPU | engine PSS | file cache | total observed | DB size | storage read/write |",
            "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
        ]
        for scenario in portable_scenarios:
            for variant in variants:
                def metric(name: str) -> float:
                    return values[(variant, scenario, name)]

                ratio = (
                    1.0
                    if variant == comparison_baseline
                    else ratio_lookup[(variant, scenario)]
                )
                lines.append(
                    f"| `{scenario}` | `{variant}` | "
                    f"{metric('throughput'):,.0f} | {ratio:.2f}× | "
                    f"{metric('call_latency_p50'):,.2f} | "
                    f"{metric('call_latency_p99'):,.2f} | "
                    f"{metric('cpu_utilization') * 100.0:.0f}% | "
                    f"{format_value(metric('engine_pss_bytes'), 'bytes')} | "
                    f"{format_value(metric('database_file_resident_bytes'), 'bytes')} | "
                    f"{format_value(metric('combined_observed_memory'), 'bytes')} | "
                    f"{format_value(metric('database_size'), 'bytes')} | "
                    f"{format_value(metric('storage_read_bytes'), 'bytes')}/"
                    f"{format_value(metric('storage_write_bytes'), 'bytes')} |"
                )

    diagnostic_names = {
        "call_latency_p99",
        "commit_latency_p95",
        "engine_pss_bytes",
        "wal_amplification",
        "combined_cache_resident_bytes",
        "combined_observed_memory",
        "database_file_resident_bytes",
        "database_size",
        "file_amplification",
        "growth",
        "storage_read_bytes",
        "storage_write_bytes",
        "workload_storage_read_amplification",
        "process_rss_growth",
    }
    if mode == "run":
        diagnostic = [
            row for row in variant_summary if row["metric"] in diagnostic_names
        ]
        if diagnostic:
            lines += [
                "",
                "## Memory, I/O, and latency diagnostics",
                "",
                "| Scenario | Metric | Median |",
                "|---|---|---:|",
            ]
            for row in diagnostic:
                lines.append(
                    f"| `{row['scenario']}` | `{row['metric']}` | "
                    f"{format_value(float(row['p50']), str(row['unit']))} |"
                )
    else:
        diagnostic = [
            row
            for row in comparisons
            if row["metric"] in diagnostic_names
            and not (
                mode == "cross"
                and specs[str(row["scenario"])]["workload"] == "portable"
            )
        ]
        if diagnostic:
            lines += [
                "",
                "## Memory, I/O, and latency diagnostics",
                "",
                "| Scenario | Candidate | Metric | Baseline | Candidate result |",
                "|---|---|---|---:|---:|",
            ]
            for row in diagnostic:
                lines.append(
                    f"| `{row['scenario']}` | `{row['candidate']}` | "
                    f"`{row['metric']}` | "
                    f"{format_value(float(row['baseline_median']), str(row['unit']))} | "
                    f"{format_value(float(row['candidate_median']), str(row['unit']))} |"
                )
    if diagnostic:
        lines.append("")
    if mode != "run":
        lines.append("Primary confidence intervals use paired trial log-ratios.")
    lines += ["Commit and churn observations remain nested diagnostics and are not treated as independent trials.", ""]
    read_scenarios = [
        scenario for scenario, spec in specs.items() if spec["workload"] == "io_read"
    ]
    if read_scenarios:
        if mode == "run":
            readahead_lookup = {
                (str(row["scenario"]), str(row["metric"])): float(row["p50"])
                for row in variant_summary
            }
            readahead_variant = "current"
        else:
            readahead_candidate = next(
                str(row["candidate"])
                for row in comparisons
                if row["role"] == "primary"
            )
            readahead_lookup = {
                (str(row["scenario"]), str(row["metric"])): float(row["candidate_median"])
                for row in comparisons
                if row["candidate"] == readahead_candidate
            }
            readahead_variant = readahead_candidate
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


def estimate_fixture_bytes(
    matrix: list[dict[str, str]], identities: dict[str, dict[str, object]]
) -> int:
    formats = {
        str(identity["format_family"]) for identity in identities.values()
    }
    maximum = 0
    for row in matrix:
        logical = int(row["rows"]) * (int(row["key_bytes"]) + int(row["value_bytes"]) + 32)
        size = max(logical, int(row["target_bytes"]))
        copies = (
            len(identities)
            if row["fixture_policy"] == "native"
            else len(formats)
        )
        maximum = max(maximum, (copies + 1) * size)
    return maximum + (512 << 20)


def default_output(mode: str) -> pathlib.Path:
    name = "tinydb-crossbench-latest" if mode == "cross" else "tinydb-benchmark-latest"
    return pathlib.Path(tempfile.gettempdir()) / name


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


def text_value(path: pathlib.Path, prefix: str | None = None) -> str:
    try:
        lines = path.read_text().splitlines()
    except OSError:
        return "unknown"
    for line in lines:
        if prefix is None:
            return line.strip() or "unknown"
        if line.startswith(prefix):
            return line.partition(":")[2].strip() or "unknown"
    return "unknown"


def host_record(root: pathlib.Path) -> dict[str, object]:
    system = platform.uname()
    storage = os.statvfs(root)
    return {
        "hostname": system.node,
        "kernel": system.release,
        "architecture": system.machine,
        "cpu": text_value(pathlib.Path("/proc/cpuinfo"), "model name"),
        "cpu_governor": text_value(
            pathlib.Path(
                "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
            )
        ),
        "memory": text_value(pathlib.Path("/proc/meminfo"), "MemTotal"),
        "hardware_threads": os.cpu_count(),
        "storage_block_bytes": storage.f_frsize,
        "storage_available_bytes": storage.f_bavail * storage.f_frsize,
    }


def main() -> None:
    args = parse_args()
    started = time.monotonic()
    started_utc = datetime.datetime.now(datetime.timezone.utc).isoformat()
    source_binaries, comparison_baseline = selected_binaries(args)
    managed_output = (
        ManagedOutput(default_output(args.mode)) if args.output is None else None
    )
    root = managed_output.path if managed_output is not None else args.output
    page_cache_overrides = resolve_page_cache_overrides(args)
    if managed_output is None:
        root.mkdir(parents=True, exist_ok=False)
    workspace = root / ".work"
    workspace.mkdir()

    binaries = {
        variant: source.resolve(strict=True)
        for variant, source in source_binaries.items()
    }
    identities = {
        variant: engine_identity(binary) for variant, binary in binaries.items()
    }
    engines = {
        variant: binary_record(binary, identities[variant])
        for variant, binary in binaries.items()
    }
    for variant, record in engines.items():
        record["effective_durable"] = bool(
            record["always_durable"] or args.semantics == "durable"
        )

    matrices = {variant: scenario_matrix(binary, args) for variant, binary in binaries.items()}
    matrix = next(iter(matrices.values()))
    if any(candidate != matrix for candidate in matrices.values()):
        raise SystemExit("engine revisions expose different benchmark matrices")
    required_free = estimate_fixture_bytes(matrix, identities)
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
    fixture_families: list[dict[str, object]] = []
    execution: list[dict[str, object]] = []
    scenario_timings: list[dict[str, object]] = []

    for scenario_index, scenario_row in enumerate(scenarios):
        scenario_started = time.monotonic()
        scenario = scenario_row["scenario"]
        fixture_policy = scenario_row["fixture_policy"]
        if fixture_policy not in ("shared", "native"):
            raise ValueError(f"unknown fixture policy {fixture_policy!r} for {scenario}")
        logical_dataset_id = dataset_id(scenario_row, args.seed)
        fixture_started = time.monotonic()
        if fixture_policy == "native":
            fixture_builders = {variant: variant for variant in binaries}
        else:
            format_groups: dict[str, list[str]] = defaultdict(list)
            for variant, identity in identities.items():
                format_groups[str(identity["format_family"])].append(variant)
            fixture_builders = {}
            for format_family, variants in sorted(format_groups.items()):
                layout = "shared-" + hashlib.sha256(
                    format_family.encode()
                ).hexdigest()[:12]
                builder = (
                    comparison_baseline
                    if comparison_baseline in variants
                    else variants[0]
                )
                fixture_builders[layout] = builder

        physical_fixtures: dict[str, tuple[pathlib.Path, dict[str, object]]] = {}
        for layout, builder_variant in fixture_builders.items():
            fixture = workspace / "fixtures" / scenario / layout / "database"
            manifest = build_fixture(
                binaries[builder_variant],
                fixture,
                scenario,
                args.seed,
                args.profile,
                args.semantics,
            )
            physical_fixtures[layout] = (fixture, manifest)
            fixture_families.append(
                {
                    "scenario": scenario,
                    "fixture_policy": fixture_policy,
                    "dataset_id": logical_dataset_id,
                    "layout": layout,
                    "builder_variant": builder_variant,
                    "family_id": manifest["family_id"],
                    "files": len(manifest["files"]),
                    "bytes": sum(
                        int(file["size"]) for file in manifest["files"]
                    ),
                }
            )
        if fixture_policy == "native":
            fixtures_by_variant = {variant: physical_fixtures[variant] for variant in binaries}
        else:
            fixtures_by_variant = {}
            for variant, identity in identities.items():
                layout = "shared-" + hashlib.sha256(
                    str(identity["format_family"]).encode()
                ).hexdigest()[:12]
                fixtures_by_variant[variant] = physical_fixtures[layout]
        fixture_seconds = time.monotonic() - fixture_started
        trials = int(scenario_row["trials"])
        orders = balanced_orders(
            list(binaries), trials, scenario_index, generator
        )
        for trial, order in enumerate(orders):
            seed = derive_trial_seed(args.seed, scenario, trial)
            trial_record: dict[str, object] = {
                "scenario": scenario,
                "fixture_policy": fixture_policy,
                "dataset_id": logical_dataset_id,
                "trial": trial,
                "trial_seed": seed,
                "order": order,
                "seconds": {},
            }
            execution.append(trial_record)
            for variant in order:
                fixture, manifest = fixtures_by_variant[variant]
                run_started = time.monotonic()
                emitted = run_trial(
                    binaries[variant],
                    variant,
                    scenario,
                    trial,
                    seed,
                    fixture,
                    manifest,
                    logical_dataset_id,
                    workspace,
                    page_cache_overrides[variant],
                    args.profile,
                    args.semantics,
                )
                trial_record["seconds"][variant] = (
                    time.monotonic() - run_started
                )
                rows.extend(emitted)
        total_seconds = time.monotonic() - scenario_started
        scenario_timings.append(
            {
                "scenario": scenario,
                "fixture_seconds": fixture_seconds,
                "trial_seconds": total_seconds - fixture_seconds,
                "total_seconds": total_seconds,
            }
        )
        shutil.rmtree(workspace / "fixtures" / scenario)

    trial_rows, observation_rows = write_results(root, rows)
    validate_primary_samples(trial_rows, matrix, binaries)
    variant_summary = summarize_trials(trial_rows)
    comparisons: list[dict[str, object]] = []
    if args.mode != "run":
        assert comparison_baseline is not None
        comparisons = compare_trials(
            trial_rows,
            matrix,
            comparison_baseline,
            [
                variant
                for variant in binaries
                if variant != comparison_baseline
            ],
        )

    elapsed = time.monotonic() - started
    write_report(
        root,
        args.mode,
        matrix,
        variant_summary,
        comparisons,
        scenario_timings,
        elapsed,
        page_cache_overrides,
        comparison_baseline,
        args.profile,
        args.semantics,
    )
    for variant, binary in binaries.items():
        if sha256(binary) != engines[variant]["sha256"]:
            raise ValueError(f"benchmark binary changed during the run: {binary}")
    metadata = {
        "suite_version": 12,
        "mode": args.mode,
        "profile": args.profile,
        "semantics": args.semantics,
        "comparison_baseline": comparison_baseline,
        "seed": args.seed,
        "started_utc": started_utc,
        "page_cache_overrides_bytes": page_cache_overrides,
        "elapsed_seconds": elapsed,
        "engines": engines,
        "host": host_record(root),
        "scenario_matrix": matrix,
        "execution": execution,
        "scenario_timings": scenario_timings,
        "fixture_families": fixture_families,
        "trial_samples": len(trial_rows),
        "nested_observations": len(observation_rows),
    }
    (root / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    shutil.rmtree(workspace)
    if managed_output is not None:
        root = managed_output.publish()
    print(f"Benchmark report: {root / 'report.md'}")


if __name__ == "__main__":
    main()
