#!/usr/bin/env python3

"""Unit tests for TinyDB benchmark pairing and inference."""

import math
import random
import unittest

import runner as target


def sample(variant: str, value: float = 1.0, **changes: str) -> dict[str, str]:
    row = {
        "variant": variant,
        "scenario": "read.engine_hot",
        "family": "reads",
        "trial_seed": "42",
        "fixture_id": "fixture-a",
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
    def test_balanced_orders(self) -> None:
        even = target.balanced_orders(5, 0, random.Random(1))
        odd = target.balanced_orders(5, 1, random.Random(1))
        self.assertEqual(sum(order[0] == "baseline" for order in even), 3)
        self.assertEqual(sum(order[0] == "candidate" for order in even), 2)
        self.assertEqual(sum(order[0] == "baseline" for order in odd), 2)
        self.assertEqual(sum(order[0] == "candidate" for order in odd), 3)

    def test_trial_seeds_are_stable_and_distinct(self) -> None:
        first = target.derive_trial_seed(7, "read.engine_hot", 0)
        self.assertEqual(first, target.derive_trial_seed(7, "read.engine_hot", 0))
        self.assertNotEqual(first, target.derive_trial_seed(7, "read.engine_hot", 1))
        self.assertNotEqual(first, target.derive_trial_seed(7, "read.eviction.uniform", 0))

    def test_exact_pairing(self) -> None:
        pairs = target.pair_trial_samples([sample("candidate", 2.0), sample("baseline", 1.0)])
        self.assertEqual(len(pairs), 1)
        self.assertEqual(pairs[0][1:], (1.0, 2.0))

    def test_missing_partner_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "missing candidate"):
            target.pair_trial_samples([sample("baseline")])

    def test_duplicate_partner_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "duplicate baseline"):
            target.pair_trial_samples([sample("baseline"), sample("baseline")])

    def test_pair_identity_includes_fixture(self) -> None:
        with self.assertRaisesRegex(ValueError, "missing"):
            target.pair_trial_samples(
                [sample("baseline"), sample("candidate", fixture_id="fixture-b")]
            )

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

    def test_practical_assessment(self) -> None:
        self.assertEqual(target.assessment("higher", 0.03, 1.04, 1.08), "improved")
        self.assertEqual(target.assessment("higher", 0.03, 0.90, 0.96), "regressed")
        self.assertEqual(target.assessment("lower", 0.03, 0.90, 0.96), "improved")
        self.assertEqual(target.assessment("lower", 0.03, 1.04, 1.08), "regressed")
        self.assertEqual(target.assessment("higher", 0.03, 0.99, 1.01), "equivalent")
        self.assertEqual(target.assessment("higher", 0.03, 0.98, 1.04), "inconclusive")

    def test_noisy_interval_is_finite(self) -> None:
        interval = target.paired_log_interval([10, 11, 9, 10, 10], [10, 10, 10, 11, 9])
        self.assertIsNotNone(interval)
        self.assertTrue(all(math.isfinite(value) for value in interval))


if __name__ == "__main__":
    unittest.main()
