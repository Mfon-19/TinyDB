#!/usr/bin/env python3

"""Run two TinyDB benchmark binaries in alternating A/B order.

Each binary owns its linked TinyDB revision. The runner uses identical scenario
geometry and seeds for a pair, preserves every child artifact, and emits one
raw sample table plus a direction-aware comparison table.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import pathlib
import random
import statistics
import subprocess
from collections import defaultdict


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline", type=pathlib.Path)
    parser.add_argument("candidate", type=pathlib.Path)
    parser.add_argument("--profile", choices=("smoke", "standard", "soak"), default="standard")
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--family")
    parser.add_argument("--filter")
    parser.add_argument("--repetitions", type=int, default=2)
    parser.add_argument("--seed", type=int, default=0x54494E594442)
    return parser.parse_args()


def scenario_matrix(binary: pathlib.Path, args: argparse.Namespace) -> list[dict[str, str]]:
    command = [str(binary), "--profile", args.profile, "--list"]
    if args.family:
        command += ["--family", args.family]
    if args.filter:
        command += ["--filter", args.filter]
    result = subprocess.run(command, check=True, text=True, capture_output=True)
    return list(csv.DictReader(result.stdout.splitlines()))


def direction(metric: str) -> str:
    if "throughput" in metric or metric.endswith("_rate") or metric == "cache_hit_rate":
        return "higher"
    if any(word in metric for word in ("latency", "amplification", "size", "growth")):
        return "lower"
    return "neutral"


def bootstrap_ratio(baseline: list[float], candidate: list[float], seed: str) -> tuple[float, float]:
    generator = random.Random(seed)
    ratios = []
    for _ in range(5000):
        baseline_sample = [generator.choice(baseline) for _ in baseline]
        candidate_sample = [generator.choice(candidate) for _ in candidate]
        baseline_median = statistics.median(baseline_sample)
        if baseline_median:
            ratios.append(statistics.median(candidate_sample) / baseline_median)
    ratios.sort()
    if not ratios:
        return float("nan"), float("nan")
    return ratios[int(len(ratios) * 0.025)], ratios[int(len(ratios) * 0.975)]


def run_one(binary: pathlib.Path, variant: str, scenario: str, repetition: int,
            pair_seed: int, root: pathlib.Path, profile: str) -> list[dict[str, str]]:
    destination = root / "runs" / variant / f"repeat-{repetition:02d}" / scenario
    command = [
        str(binary),
        "--profile", profile,
        "--filter", scenario,
        "--ordered",
        "--seed", str(pair_seed),
        "--output", str(destination),
    ]
    print(f"[{variant} repeat {repetition}] {scenario}", flush=True)
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
    with (destination / "samples.csv").open(newline="") as source:
        rows = list(csv.DictReader(source))
    for row in rows:
        row["variant"] = variant
        row["repetition"] = str(repetition)
    return rows


def write_comparison(root: pathlib.Path, rows: list[dict[str, str]]) -> None:
    raw_fields = ["variant", "repetition", "scenario", "family", "metric", "unit",
                  "trial", "observation", "value"]
    with (root / "samples.csv").open("w", newline="") as destination:
        writer = csv.DictWriter(destination, fieldnames=raw_fields)
        writer.writeheader()
        writer.writerows({field: row[field] for field in raw_fields} for row in rows)

    raw_groups: dict[tuple[str, str, str, str], list[float]] = defaultdict(list)
    trial_groups: dict[tuple[str, str, str, str, str, str], list[float]] = defaultdict(list)
    for row in rows:
        key = (row["variant"], row["scenario"], row["metric"], row["unit"])
        raw_groups[key].append(float(row["value"]))
        trial_key = key + (row["repetition"], row["trial"])
        trial_groups[trial_key].append(float(row["value"]))

    groups: dict[tuple[str, str, str, str], list[float]] = defaultdict(list)
    for trial_key, values in trial_groups.items():
        groups[trial_key[:4]].append(statistics.median(values))

    fields = ["scenario", "metric", "unit", "direction", "assessment",
              "baseline_raw_samples", "candidate_raw_samples", "baseline_trial_samples",
              "candidate_trial_samples", "baseline_mean", "candidate_mean",
              "baseline_median", "candidate_median", "candidate_over_baseline",
              "ratio_ci95_low", "ratio_ci95_high", "improvement_percent"]
    with (root / "comparison.csv").open("w", newline="") as destination:
        writer = csv.DictWriter(destination, fieldnames=fields)
        writer.writeheader()
        identities = sorted({(row["scenario"], row["metric"], row["unit"]) for row in rows})
        for scenario, metric, unit in identities:
            baseline = groups[("baseline", scenario, metric, unit)]
            candidate = groups[("candidate", scenario, metric, unit)]
            baseline_median = statistics.median(baseline)
            candidate_median = statistics.median(candidate)
            ratio = candidate_median / baseline_median if baseline_median else float("nan")
            preferred = direction(metric)
            ci_low, ci_high = bootstrap_ratio(baseline, candidate, scenario + "\0" + metric)
            if preferred == "higher":
                improvement = (ratio - 1.0) * 100.0
                assessment = "improved" if ci_low > 1.0 else "regressed" if ci_high < 1.0 else "inconclusive"
            elif preferred == "lower" and candidate_median:
                improvement = (baseline_median / candidate_median - 1.0) * 100.0
                assessment = "improved" if ci_high < 1.0 else "regressed" if ci_low > 1.0 else "inconclusive"
            else:
                improvement = float("nan")
                assessment = "neutral"
            raw_key = ("baseline", scenario, metric, unit)
            candidate_raw_key = ("candidate", scenario, metric, unit)
            writer.writerow({
                "scenario": scenario,
                "metric": metric,
                "unit": unit,
                "direction": preferred,
                "assessment": assessment,
                "baseline_raw_samples": len(raw_groups[raw_key]),
                "candidate_raw_samples": len(raw_groups[candidate_raw_key]),
                "baseline_trial_samples": len(baseline),
                "candidate_trial_samples": len(candidate),
                "baseline_mean": statistics.fmean(baseline),
                "candidate_mean": statistics.fmean(candidate),
                "baseline_median": baseline_median,
                "candidate_median": candidate_median,
                "candidate_over_baseline": ratio,
                "ratio_ci95_low": ci_low,
                "ratio_ci95_high": ci_high,
                "improvement_percent": improvement,
            })


def main() -> None:
    args = parse_args()
    if args.repetitions < 1:
        raise SystemExit("--repetitions must be positive")
    timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    root = args.output or pathlib.Path("benchmark-results") / f"compare-{timestamp}"
    root.mkdir(parents=True, exist_ok=False)

    baseline_matrix = scenario_matrix(args.baseline, args)
    candidate_matrix = scenario_matrix(args.candidate, args)
    if baseline_matrix != candidate_matrix:
        raise SystemExit("baseline and candidate expose different scenario matrices")
    baseline_scenarios = [row["scenario"] for row in baseline_matrix]

    rows: list[dict[str, str]] = []
    generator = random.Random(args.seed)
    for repetition in range(args.repetitions):
        scenarios = baseline_scenarios.copy()
        generator.shuffle(scenarios)
        for index, scenario in enumerate(scenarios):
            variants = [("baseline", args.baseline), ("candidate", args.candidate)]
            if (repetition + index) % 2:
                variants.reverse()
            pair_seed = args.seed ^ repetition
            for variant, binary in variants:
                rows.extend(run_one(binary, variant, scenario, repetition, pair_seed, root, args.profile))

    write_comparison(root, rows)
    metadata = {
        "profile": args.profile,
        "seed": args.seed,
        "repetitions": args.repetitions,
        "baseline": str(args.baseline.resolve()),
        "candidate": str(args.candidate.resolve()),
        "scenarios": baseline_scenarios,
        "note": ("Positive improvement_percent means the candidate moved in the preferred direction. "
                 "Confidence intervals bootstrap per-trial medians; inconclusive is not proof of equality."),
    }
    (root / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"Comparison artifacts: {root}")


if __name__ == "__main__":
    main()
