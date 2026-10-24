#!/usr/bin/env python3
"""Read-only Timestral-209M forecasting sidecar for Nexus.

No miner commands, GPU imports, wallet reads, or block/nonce prediction.
Run outside the mining worker. Timestral and NumPy are imported only by forecast.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import math
import os
from pathlib import Path
import shutil
import sqlite3
import sys
import time

MODEL = "mistralai/Timestral-209M"
DEFAULT_ROOT = Path.home() / ".local/share/nexus-keryx-miner/predictive"
FIELDS = ("eligible_attempts", "gpu_work_units_completed", "stale_work_discarded")


def private_root(path: Path) -> Path:
    path = path.expanduser()
    if path.is_symlink():
        raise ValueError("Refusing a symlinked state directory")
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    path.chmod(0o700)
    return path.resolve()


def connect(root: Path) -> sqlite3.Connection:
    path = root / "samples.sqlite3"
    if path.is_symlink():
        raise ValueError("Refusing symlinked SQLite database")
    db = sqlite3.connect(path, timeout=10)
    db.execute("""
        CREATE TABLE IF NOT EXISTS samples (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            wall_ns INTEGER NOT NULL,
            sampled_ns INTEGER NOT NULL,
            elapsed_ns INTEGER,
            eligible_attempts INTEGER,
            gpu_work_units_completed INTEGER,
            stale_work_discarded INTEGER,
            mining TEXT,
            inference TEXT,
            status TEXT NOT NULL
        )
    """)
    db.commit()
    return db


def valid_counter(value: object) -> bool:
    return type(value) is int and 0 <= value <= (2**63 - 1)


def normalized(snapshot: object) -> tuple:
    """Do not treat absent, stale or invalid telemetry as zero."""
    if not isinstance(snapshot, dict):
        return (None, None, None, None, None, None, "INVALID")
    age = snapshot.get("age_s")
    counters = snapshot.get("counters")
    state = snapshot.get("state")
    elapsed = snapshot.get("elapsed_ns")
    if (snapshot.get("schema_version") != 1
            or snapshot.get("status") != "LIVE"
            or type(age) not in (float, int)
            or not math.isfinite(age) or not 0 <= age <= 5
            or not valid_counter(elapsed)
            or not isinstance(counters, dict)
            or not all(valid_counter(counters.get(k)) for k in FIELDS)
            or not isinstance(state, dict)
            or not isinstance(state.get("mining"), str)
            or not isinstance(state.get("inference"), str)):
        return (None, None, None, None, None, None, "INVALID")
    return (
        elapsed, *(counters[k] for k in FIELDS),
        state["mining"][:64], state["inference"][:64], "LIVE",
    )


def record(db: sqlite3.Connection, snapshot: object) -> str:
    row = normalized(snapshot)
    db.execute("""
        INSERT INTO samples (wall_ns, sampled_ns, elapsed_ns, eligible_attempts,
          gpu_work_units_completed, stale_work_discarded, mining, inference, status)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (time.time_ns(), time.monotonic_ns(), *row))
    db.commit()
    return row[-1]


async def read_mcp() -> object:
    # Use the repository's existing authenticated, read-only MCP proxy.
    # It never exposes the token in this process's output or data files.
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
    from nexus_mcp_proxy import call
    return await call("nexus_telemetry")


def collect(db: sqlite3.Connection, interval: float, once: bool) -> None:
    if not 5 <= interval <= 60:
        raise ValueError("Interval must be 5..60 seconds")
    while True:
        started = time.monotonic()
        try:
            status = record(db, asyncio.run(read_mcp()))
        except Exception as exc:
            # Persist a gap rather than fabricating a successful observation.
            status = record(db, None)
            print("MCP sample failed:", type(exc).__name__, file=sys.stderr)
        print(json.dumps({"event": "TIMESTRAL_SAMPLE", "status": status}))
        if once:
            return
        time.sleep(max(0, interval - (time.monotonic() - started)))


def recent_features(db: sqlite3.Connection, interval: float, limit: int = 1024) -> list[tuple[float, float, float]]:
    """Return only the latest contiguous window, without spanning resets/gaps."""
    rows = list(db.execute("""
        SELECT sampled_ns, elapsed_ns, eligible_attempts,
               gpu_work_units_completed, stale_work_discarded, status
        FROM samples ORDER BY id DESC LIMIT ?
    """, (limit + 1,)))
    rows.reverse()
    segment: list[tuple[float, float, float]] = []
    previous = None
    for row in rows:
        tick, elapsed, eligible, completed, stale, status = row
        if status != "LIVE" or any(not valid_counter(x) for x in (tick, elapsed, eligible, completed, stale)):
            segment, previous = [], None
            continue
        if previous is not None:
            ptick, pelapsed, peligible, pcompleted, pstale, _ = previous
            elapsed_s = (elapsed - pelapsed) / 1e9
            observed_s = (tick - ptick) / 1e9
            de, dc, ds = eligible - peligible, completed - pcompleted, stale - pstale
            if (not interval * 0.5 <= observed_s <= interval * 2.5
                    or not interval * 0.5 <= elapsed_s <= interval * 2.5
                    or min(de, dc, ds) < 0 or ds > dc):
                segment = []
            else:
                rate = de / elapsed_s
                completed_rate = dc / elapsed_s
                stale_fraction = ds / dc if dc else float("nan")
                if rate > 1e12 or completed_rate > 1e12:
                    segment = []
                else:
                    segment.append((rate, completed_rate, stale_fraction))
        previous = row
    return segment[-limit:]


def available_memory_mib() -> int:
    for line in Path("/proc/meminfo").read_text().splitlines():
        if line.startswith("MemAvailable:"):
            return int(line.split()[1]) // 1024
    return 0


def atomic_json(path: Path, result: dict) -> None:
    temporary = path.with_name(path.name + ".new")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "w") as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def forecast(db: sqlite3.Connection, root: Path, interval: float,
             horizon: int, threads: int, minimum_memory_mib: int,
             offline: bool) -> dict:
    if not 1 <= horizon <= 1024:
        raise ValueError("Horizon must be 1..1024 samples")
    if not 1 <= threads <= 4:
        raise ValueError("CPU thread count must be 1..4")
    features = recent_features(db, interval)
    if len(features) < 64:
        raise RuntimeError("Need 64 contiguous, fresh intervals before forecasting")
    free_mib = available_memory_mib()
    if free_mib < minimum_memory_mib:
        raise RuntimeError("Insufficient MemAvailable; model was not loaded")
    if shutil.disk_usage(root).free < 2 * 1024**3:
        raise RuntimeError("Insufficient persistent disk space for model/cache")

    # Override temp/cache roots BEFORE importing Torch, Timestral or HF libraries.
    for key, location in {
        "TMPDIR": root / "tmp", "TMP": root / "tmp", "TEMP": root / "tmp",
        "HF_HOME": root / "hf", "TORCH_HOME": root / "torch",
        "XDG_CACHE_HOME": root / "cache",
    }.items():
        location.mkdir(mode=0o700, exist_ok=True)
        os.environ[key] = str(location)
    os.environ["OMP_NUM_THREADS"] = str(threads)
    os.environ["MKL_NUM_THREADS"] = str(threads)
    os.environ["CUDA_VISIBLE_DEVICES"] = ""
    os.environ["HIP_VISIBLE_DEVICES"] = ""
    import numpy as np
    import torch
    from timestral import TimestralForecaster
    torch.set_num_threads(threads)

    series = np.asarray(features, dtype=np.float32).T[None, :, :]
    model = TimestralForecaster.from_pretrained(
        MODEL, device="cpu", cache_dir=str(root / "hf"),
        local_files_only=offline,
    )
    with torch.inference_mode():
        predicted = model.forecast(series, prediction_length=horizon)
    result = {
        "schema_version": 1,
        "model": MODEL,
        "mode": "SHADOW",
        "device": "cpu",
        "sample_interval_seconds": interval,
        "history_intervals": len(features),
        "forecast_intervals": horizon,
        "forecast_generated_unix": time.time(),
        "units": ["eligible_nonces_per_second", "gpu_work_units_per_second", "stale_fraction"],
        "p10": np.asarray(predicted.quantile(0.1))[0].tolist(),
        "p50": np.asarray(predicted.median)[0].tolist(),
        "p90": np.asarray(predicted.quantile(0.9))[0].tolist(),
        "note": "Operational forecast, not a prediction of winning nonces or blocks. No miner control.",
    }
    # Reject nonfinite model outputs (rather than write invalid JSON).
    for field in ("p10", "p50", "p90"):
        if not all(math.isfinite(v) for row in result[field] for v in row):
            raise RuntimeError("Nonfinite forecast; not published")
    atomic_json(root / "latest-forecast.json", result)
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("collect", "forecast"))
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--interval", type=float, default=10)
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--horizon", type=int, default=12)
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--min-memory-mib", type=int, default=3072)
    parser.add_argument("--offline", action="store_true")
    args = parser.parse_args()
    os.umask(0o077)
    root = private_root(args.root)
    with connect(root) as db:
        if args.command == "collect":
            collect(db, args.interval, args.once)
        else:
            result = forecast(db, root, args.interval, args.horizon,
                              args.threads, args.min_memory_mib, args.offline)
            print(json.dumps({
                "status": "FORECAST_WRITTEN",
                "model": result["model"],
                "path": str(root / "latest-forecast.json"),
                "history_intervals": result["history_intervals"],
            }))


if __name__ == "__main__":
    main()
