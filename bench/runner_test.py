#!/usr/bin/env python3

"""Unit tests for benchmark orchestration and paired inference."""

import math
import pathlib
import random
import sys
import tempfile
import unittest
from unittest import mock

import runner as target


def sample(variant: str, value: float = 1.0, **changes: str) -> dict[str, str]:
    row = {
        "variant": variant,
        "scenario": "read.engine_hot",
        "family": "reads",
        "trial_seed": "42",
        "dataset_id": "dataset-a",
        "metric": "throughput",
        "unit": "reads/second",
        "scope": "trial",
        "trial": "0",
        "observation": "0",
        "value": str(value),
    }
    row.update(changes)
    return row


class RunnerTest(unittest.TestCase):
    def test_positive_integer(self) -> None:
        self.assertEqual(target.positive_integer("16"), 16)
        with self.assertRaises(target.argparse.ArgumentTypeError):
            target.positive_integer("0")

    def test_compare_accepts_independent_cache_overrides(self) -> None:
        with mock.patch.object(
            sys,
            "argv",
            [
                "runner.py",
                "compare",
                "buffered",
                "direct",
                "--baseline-cache-mib",
                "16",
                "--candidate-cache-mib",
                "32",
            ],
        ):
            args = target.parse_args()
        self.assertEqual(args.baseline_cache_mib, 16)
        self.assertEqual(args.candidate_cache_mib, 32)

    def test_common_cache_override_applies_to_standalone_run(self) -> None:
        with mock.patch.object(
            sys,
            "argv",
            ["runner.py", "run", "buffered", "--cache-mib", "8"],
        ):
            args = target.parse_args()
        self.assertEqual(target.resolve_page_cache_overrides(args), {"current": 8 << 20})

    def test_standalone_run_uses_buffered_io_by_default(self) -> None:
        with mock.patch.object(
            sys,
            "argv",
            ["runner.py", "run", "tinydb"],
        ):
            args = target.parse_args()
        self.assertEqual(target.resolve_io_modes(args), {"current": "buffered"})

    def test_standalone_run_accepts_direct_io(self) -> None:
        with mock.patch.object(
            sys,
            "argv",
            ["runner.py", "run", "tinydb", "--io-mode", "direct"],
        ):
            args = target.parse_args()
        self.assertEqual(target.resolve_io_modes(args), {"current": "direct"})

    def test_compare_accepts_independent_io_modes(self) -> None:
        with mock.patch.object(
            sys,
            "argv",
            [
                "runner.py",
                "compare",
                "tinydb",
                "tinydb",
                "--baseline-io-mode",
                "buffered",
                "--candidate-io-mode",
                "direct",
            ],
        ):
            args = target.parse_args()
        self.assertEqual(
            target.resolve_io_modes(args),
            {"baseline": "buffered", "candidate": "direct"},
        )

    def test_variant_cache_override_takes_precedence_over_common_value(self) -> None:
        with mock.patch.object(
            sys,
            "argv",
            [
                "runner.py",
                "compare",
                "buffered",
                "direct",
                "--cache-mib",
                "8",
                "--candidate-cache-mib",
                "32",
            ],
        ):
            args = target.parse_args()
        self.assertEqual(
            target.resolve_page_cache_overrides(args),
            {"baseline": 8 << 20, "candidate": 32 << 20},
        )

    def test_managed_output_replaces_only_a_completed_result(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            destination = pathlib.Path(directory) / "latest"
            destination.mkdir()
            (destination / "report.md").write_text("old report")
            (destination / "metadata.json").write_text("{}")

            output = target.ManagedOutput(destination)
            self.assertEqual((destination / "report.md").read_text(), "old report")
            (output.path / "report.md").write_text("new report")
            (output.path / "results.csv").write_text("value\n1\n")
            (output.path / "metadata.json").write_text("{}")

            self.assertEqual(output.publish(), destination)
            self.assertEqual((destination / "report.md").read_text(), "new report")
            self.assertEqual(
                {path.name for path in destination.iterdir()},
                {"report.md", "results.csv", "metadata.json"},
            )

    def test_managed_output_refuses_an_unrecognized_destination(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            destination = pathlib.Path(directory) / "latest"
            destination.mkdir()
            (destination / "unrelated.txt").write_text("keep me")

            output = target.ManagedOutput(destination)
            with self.assertRaisesRegex(SystemExit, "non-benchmark output"):
                output.publish()
            self.assertEqual((destination / "unrelated.txt").read_text(), "keep me")
            output.cleanup()

    def test_balanced_orders(self) -> None:
        even = target.balanced_orders(
            ["baseline", "candidate"], 5, 0, random.Random(1)
        )
        odd = target.balanced_orders(
            ["baseline", "candidate"], 5, 1, random.Random(1)
        )
        even_baseline_first = sum(order[0] == "baseline" for order in even)
        odd_baseline_first = sum(order[0] == "baseline" for order in odd)
        self.assertIn(even_baseline_first, (2, 3))
        self.assertIn(odd_baseline_first, (2, 3))
        self.assertEqual(even_baseline_first + odd_baseline_first, 5)

    def test_multi_engine_orders_rotate_every_engine(self) -> None:
        variants = ["tinydb", "sqlite", "leveldb", "rocksdb"]
        orders = target.balanced_orders(variants, 4, 0, random.Random(3))
        self.assertEqual({order[0] for order in orders}, set(variants))
        self.assertTrue(all(sorted(order) == sorted(variants) for order in orders))

    def test_trial_seeds_are_stable_and_distinct(self) -> None:
        first = target.derive_trial_seed(7, "read.engine_hot", 0)
        self.assertEqual(first, target.derive_trial_seed(7, "read.engine_hot", 0))
        self.assertNotEqual(first, target.derive_trial_seed(7, "read.engine_hot", 1))
        self.assertNotEqual(first, target.derive_trial_seed(7, "read.eviction.uniform", 0))

    def test_dataset_identity_is_logical_and_stable(self) -> None:
        scenario = {"scenario": "read.cold.large-values.native.64MiB", "rows": "1023"}
        self.assertEqual(target.dataset_id(scenario, 7), target.dataset_id(scenario, 7))
        self.assertNotEqual(target.dataset_id(scenario, 7), target.dataset_id(scenario, 8))

    def test_native_comparison_accounts_for_both_canonical_fixtures(self) -> None:
        shared = {
            "rows": "1",
            "key_bytes": "16",
            "value_bytes": "16",
            "target_bytes": "0",
            "fixture_policy": "shared",
        }
        native = {**shared, "fixture_policy": "native"}
        overhead = 512 << 20
        identities = {
            "baseline": {"format_family": "tinydb"},
            "candidate": {"format_family": "tinydb"},
        }
        self.assertEqual(
            target.estimate_fixture_bytes([shared], identities), 2 * 64 + overhead
        )
        self.assertEqual(
            target.estimate_fixture_bytes([native], identities), 3 * 64 + overhead
        )

    def test_cross_engine_fixture_estimate_counts_formats(self) -> None:
        scenario = {
            "rows": "1",
            "key_bytes": "16",
            "value_bytes": "16",
            "target_bytes": "0",
            "fixture_policy": "shared",
        }
        identities = {
            "buffered": {"format_family": "tinydb"},
            "direct": {"format_family": "tinydb"},
            "sqlite": {"format_family": "sqlite"},
        }
        overhead = 512 << 20
        self.assertEqual(
            target.estimate_fixture_bytes([scenario], identities),
            3 * 64 + overhead,
        )

    def test_exact_pairing(self) -> None:
        pairs = target.pair_trial_samples(
            [sample("candidate", 2.0), sample("baseline", 1.0)],
            "baseline",
            "candidate",
        )
        self.assertEqual(len(pairs), 1)
        self.assertEqual(pairs[0][1:], (1.0, 2.0))

    def test_missing_partner_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "missing candidate"):
            target.pair_trial_samples(
                [sample("baseline")], "baseline", "candidate"
            )

    def test_duplicate_partner_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "duplicate baseline"):
            target.pair_trial_samples(
                [sample("baseline"), sample("baseline")],
                "baseline",
                "candidate",
            )

    def test_pair_identity_includes_dataset(self) -> None:
        with self.assertRaisesRegex(ValueError, "missing"):
            target.pair_trial_samples(
                [sample("baseline"), sample("candidate", dataset_id="dataset-b")],
                "baseline",
                "candidate",
            )

    def test_cross_engine_selection(self) -> None:
        with mock.patch.object(
            sys,
            "argv",
            [
                "runner.py",
                "cross",
                "--engine",
                "tinydb=buffered",
                "--engine",
                "sqlite=sqlite-worker",
                "--baseline",
                "tinydb",
                "--family",
                "db_bench",
                "--family",
                "ycsb",
            ],
        ):
            args = target.parse_args()
        binaries, baseline = target.selected_binaries(args)
        self.assertEqual(
            binaries,
            {
                "tinydb": pathlib.Path("buffered"),
                "sqlite": pathlib.Path("sqlite-worker"),
            },
        )
        self.assertEqual(baseline, "tinydb")
        self.assertEqual(args.family, ["db_bench", "ycsb"])

    def test_primary_sample_count_is_enforced(self) -> None:
        matrix = [{"scenario": "read.engine_hot", "primary_metric": "throughput", "trials": "1"}]
        target.validate_primary_samples([sample("current")], matrix, ["current"])
        with self.assertRaisesRegex(ValueError, "expected 1"):
            target.validate_primary_samples([], matrix, ["current"])

    def test_paired_log_interval(self) -> None:
        ratio, low, high = target.paired_log_interval([10.0] * 5, [11.0] * 5)
        self.assertAlmostEqual(ratio, 1.1)
        self.assertAlmostEqual(low, 1.1)
        self.assertAlmostEqual(high, 1.1)

    def test_nonpositive_diagnostic_has_no_ratio_interval(self) -> None:
        self.assertIsNone(target.paired_log_interval([0.0, 0.0], [0.0, 1.0]))

    def test_comparison_ignores_variant_specific_diagnostics(self) -> None:
        rows = [
            sample("baseline", 10.0),
            sample("candidate", 11.0),
            sample("candidate", 7.0, metric="candidate_only", unit="pages"),
        ]
        matrix = [
            {
                "scenario": "read.engine_hot",
                "primary_metric": "throughput",
                "primary_direction": "higher",
                "meaningful_difference": "0.05",
            }
        ]

        comparison = target.compare_trials(rows, matrix, "baseline", ["candidate"])

        self.assertEqual(len(comparison), 1)
        self.assertEqual(comparison[0]["metric"], "throughput")

    def test_report_uses_current_read_ahead_counters(self) -> None:
        scenario = {
            "scenario": "read.cold",
            "workload": "io_read",
            "primary_metric": "throughput",
            "page_cache_bytes": str(16 << 20),
        }
        summary = [
            {
                "scenario": "read.cold",
                "metric": "throughput",
                "unit": "reads/second",
                "p50": 100.0,
                "p95": 110.0,
                "samples": 3,
            },
            {
                "scenario": "read.cold",
                "metric": "readahead_plans",
                "unit": "plans",
                "p50": 2.0,
            },
            {
                "scenario": "read.cold",
                "metric": "readahead_pages_scheduled",
                "unit": "pages",
                "p50": 40.0,
            },
            {
                "scenario": "read.cold",
                "metric": "readahead_pages_consumed",
                "unit": "pages",
                "p50": 30.0,
            },
        ]
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            target.write_report(
                root,
                "run",
                [scenario],
                summary,
                [],
                [
                    {
                        "scenario": "read.cold",
                        "fixture_seconds": 1.0,
                        "trial_seconds": 2.0,
                        "total_seconds": 3.0,
                    }
                ],
                3.0,
                {"current": None},
                None,
                "quick",
                "native",
                {"current": "direct"},
            )
            report = (root / "report.md").read_text()

        self.assertIn(
            "| `read.cold` | 2 | 40 | 30 | 75.0% |",
            report,
        )
        self.assertNotIn("read_streams_started", report)

    def test_practical_assessment(self) -> None:
        self.assertEqual(target.assessment("higher", 0.03, 1.04, 1.08), "improved")
        self.assertEqual(target.assessment("higher", 0.03, 0.90, 0.96), "regressed")
        self.assertEqual(target.assessment("lower", 0.03, 0.90, 0.96), "improved")
        self.assertEqual(target.assessment("lower", 0.03, 1.04, 1.08), "regressed")
        self.assertEqual(target.assessment("higher", 0.03, 0.99, 1.01), "equivalent")
        self.assertEqual(target.assessment("higher", 0.03, 0.98, 1.04), "inconclusive")

    def test_improvement_interval_uses_metric_direction(self) -> None:
        effect, low, high = target.improvement_interval("higher", 1.2, 1.1, 1.3)
        self.assertAlmostEqual(effect, 20.0)
        self.assertAlmostEqual(low, 10.0)
        self.assertAlmostEqual(high, 30.0)
        effect, low, high = target.improvement_interval("lower", 0.7, 0.6, 0.8)
        self.assertAlmostEqual(effect, 30.0)
        self.assertAlmostEqual(low, 20.0)
        self.assertAlmostEqual(high, 40.0)

    def test_noisy_interval_is_finite(self) -> None:
        interval = target.paired_log_interval([10, 11, 9, 10, 10], [10, 10, 10, 11, 9])
        self.assertIsNotNone(interval)
        self.assertTrue(all(math.isfinite(value) for value in interval))


if __name__ == "__main__":
    unittest.main()
