"""Offline unit tests: standard library only, no mining, MCP or model loading."""
import importlib.util
from pathlib import Path
import sqlite3
import unittest

MODULE = Path(__file__).with_name("nexus_timestral.py")
spec = importlib.util.spec_from_file_location("nexus_timestral_under_test", MODULE)
nexus = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nexus)


def database():
    db = sqlite3.connect(":memory:")
    db.execute("""
        CREATE TABLE samples (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            wall_ns INTEGER NOT NULL, sampled_ns INTEGER NOT NULL,
            elapsed_ns INTEGER, eligible_attempts INTEGER,
            gpu_work_units_completed INTEGER, stale_work_discarded INTEGER,
            mining TEXT, inference TEXT, status TEXT NOT NULL
        )
    """)
    return db


def add(db, index, elapsed=None, eligible=None, completed=None, stale=None,
        status="LIVE"):
    elapsed = index * 10_000_000_000 if elapsed is None else elapsed
    eligible = index * 1000 if eligible is None else eligible
    completed = index * 2000 if completed is None else completed
    stale = index * 200 if stale is None else stale
    db.execute("""
        INSERT INTO samples (wall_ns, sampled_ns, elapsed_ns,
          eligible_attempts, gpu_work_units_completed, stale_work_discarded,
          mining, inference, status)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (index * 10_000_000_000, index * 10_000_000_000,
          elapsed, eligible, completed, stale, "ACTIVE", "IDLE", status))


class TimestralSidecarTests(unittest.TestCase):
    def test_accept_fresh_snapshot(self):
        snapshot = {
            "schema_version": 1, "status": "LIVE", "age_s": 0.5,
            "elapsed_ns": 20_000_000_000,
            "counters": {"eligible_attempts": 200,
                         "gpu_work_units_completed": 400,
                         "stale_work_discarded": 40},
            "state": {"mining": "ACTIVE", "inference": "IDLE"},
        }
        self.assertEqual(nexus.normalized(snapshot)[-1], "LIVE")
        snapshot["status"] = "STALE"
        self.assertEqual(nexus.normalized(snapshot)[-1], "INVALID")
        snapshot["status"] = "LIVE"
        snapshot["counters"]["eligible_attempts"] = True
        self.assertEqual(nexus.normalized(snapshot)[-1], "INVALID")

    def test_counter_differences_not_cumulative_rates(self):
        with database() as db:
            for i in range(80):
                add(db, i)
            rates = nexus.recent_features(db, 10)
            self.assertEqual(len(rates), 79)
            self.assertAlmostEqual(rates[-1][0], 100)
            self.assertAlmostEqual(rates[-1][1], 200)
            self.assertAlmostEqual(rates[-1][2], 0.1)

    def test_stale_sample_breaks_history(self):
        with database() as db:
            for i in range(80):
                add(db, i)
            add(db, 80, status="INVALID")
            add(db, 81)
            self.assertEqual(nexus.recent_features(db, 10), [])

    def test_worker_counter_reset_breaks_history(self):
        with database() as db:
            for i in range(80):
                add(db, i)
            for i in range(80, 84):
                idx = i - 80
                add(db, i, elapsed=idx * 10_000_000_000,
                    eligible=idx * 1000, completed=idx * 2000,
                    stale=idx * 200)
            self.assertEqual(len(nexus.recent_features(db, 10)), 3)

    def test_missing_work_is_not_zero_stale_fraction(self):
        with database() as db:
            add(db, 0)
            add(db, 1, eligible=0, completed=0, stale=0)
            rate = nexus.recent_features(db, 10)[0]
            self.assertEqual(rate[0], 0)
            self.assertNotEqual(rate[2], rate[2])  # NaN means unknown.

    def test_no_model_import_at_startup(self):
        import sys
        self.assertNotIn("timestral", sys.modules)


if __name__ == "__main__":
    unittest.main()
