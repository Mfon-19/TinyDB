#!/usr/bin/env python3
"""Summarize one --engine both CSV per configuration; emit Markdown."""

import argparse
import csv
from collections import defaultdict
from pathlib import Path
from statistics import median


def summarize(path):
    lines = path.read_text().splitlines()
    metadata = next((line[2:] for line in lines if line.startswith("# directory=")), "")
    config = dict(field.split("=", 1) for field in metadata.split(", ") if "=" in field)
    rows = list(csv.DictReader(line for line in lines if not line.startswith("#")))
    groups = defaultdict(lambda: defaultdict(list))
    for row in rows:
        groups[row["workload"]][row["engine"]].append(row)
    if not groups:
        raise ValueError(f"{path}: no measurements")

    for name, engines in groups.items():
        if set(engines) != {"tinydb", "sqlite"}:
            raise ValueError(f"{path}: {name} needs both engines; use --engine both")
        signatures = []
        for engine in ("tinydb", "sqlite"):
            samples = engines[engine]
            if len({r["run"] for r in samples}) != len(samples):
                raise ValueError(f"{path}: duplicate runs; do not concatenate CSV files")
            if "runs" in config and {int(r["run"]) for r in samples} != set(
                range(1, int(config["runs"]) + 1)
            ):
                raise ValueError(f"{path}: {name}/{engine} is missing requested runs")
            signatures.append(sorted(
                (r["run"], r["operations"], r["transactions"]) for r in samples
            ))
        if signatures[0] != signatures[1]:
            raise ValueError(f"{path}: {name} has unmatched runs or operation counts")

    def values(name, engine, column):
        return [float(row[column]) for row in groups[name][engine]]

    def middle(name, engine, column):
        return median(values(name, engine, column))

    def rate(name, engine, column):
        rates = values(name, engine, column)
        return f"{median(rates):,.0f} ({min(rates):,.0f}–{max(rates):,.0f})"

    print(f"## {path.name}\n")
    for line in lines:
        if line.startswith("#"):
            print(line.removeprefix("# ") + "\n")
    print("Throughput: median (min–max). Ratios are TinyDB / SQLite; "
          "above 1 favors TinyDB. The last column also includes the final "
          "checkpoint in elapsed time. Automatic checkpoints are always timed.\n")
    print("| Workload | Unit | TinyDB | SQLite | Ratio | Ratio incl. checkpoint |")
    print("| --- | --- | ---: | ---: | ---: | ---: |")
    for name in groups:
        column = "txn_per_sec" if name == "scan_100" else "ops_per_sec"
        unit = "scans/s" if name == "scan_100" else "entries/s" if name == "scan_full" else "ops/s"
        ratio = middle(name, "tinydb", column) / middle(name, "sqlite", column)
        total_ratio = middle(name, "tinydb", "ops_with_checkpoint_per_sec") / middle(
            name, "sqlite", "ops_with_checkpoint_per_sec"
        )
        print(f"| {name} | {unit} | {rate(name, 'tinydb', column)} | "
              f"{rate(name, 'sqlite', column)} | {ratio:.2f}× | {total_ratio:.2f}× |")

    print("\nLatency and storage: medians across runs. Each p99 is computed "
          "within a run, including transaction teardown. DB size is measured "
          "after checkpointing; it excludes WAL and shared-memory sidecars.\n")
    print("| Workload | Engine | Samples/run (min) | p99 txn (ms) | "
          "Elapsed (s) | Final checkpoint (ms) | DB bytes |")
    print("| --- | --- | ---: | ---: | ---: | ---: | ---: |")
    for name in groups:
        for engine in ("tinydb", "sqlite"):
            print(f"| {name} | {engine} | "
                  f"{min(values(name, engine, 'transactions')):,.0f} | "
                  f"{middle(name, engine, 'p99_us') / 1000:.4f} | "
                  f"{middle(name, engine, 'seconds'):.3f} | "
                  f"{middle(name, engine, 'checkpoint_ms'):.3f} | "
                  f"{middle(name, engine, 'database_bytes'):,.0f} |")

    short = [name for name in groups if any(
        float(row["seconds"]) < 1 for samples in groups[name].values() for row in samples
    )]
    sparse = [name for name in groups if any(
        int(row["transactions"]) < 1000 for samples in groups[name].values() for row in samples
    )]
    if short:
        print("\nSome runs lasted under one second: " + ", ".join(short) +
              ". Increase --read-passes / --scan-passes for reads and scans, "
              "or --keys for writes before drawing performance conclusions.")
    if sparse:
        print("\np99 has fewer than 1,000 samples in some runs: " + ", ".join(sparse) + ".")
    print()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path, nargs="+", help="CSV files, reported separately")
    args = parser.parse_args()
    try:
        for path in args.csv:
            summarize(path)
    except (OSError, ValueError, KeyError, ZeroDivisionError) as error:
        parser.exit(1, f"comparison failed: {error}\n")
