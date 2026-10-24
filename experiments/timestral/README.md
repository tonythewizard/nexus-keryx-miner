# Timestral-209M — experimental Nexus shadow sidecar

This is a read-only forecasting prototype. It never predicts cryptographic
winning nonces, suppresses candidate hashes, changes a miner parameter, or
affects AI-service preemption. Its purpose is to forecast operational
throughput and stale-work fraction.

## Architecture

The collector calls the existing authenticated MCP tool "nexus_telemetry"
through "scripts/nexus_mcp_proxy.py". It records selected **non-secret**
fields in a local SQLite database: worker elapsed time, eligible attempts,
completed GPU work units, discarded stale work, mining/inference state.

The forecaster derives per-second rates from **counter differences**, never
from cumulative counters. Stale snapshots, gaps, counter resets, worker
restarts and missing telemetry invalidate the contiguous historical window.
Timestral forecasts three target variates jointly: eligible attempts/s,
completed work units/s, and fraction of completed work discarded as stale.
It outputs P10, P50, P90 paths. This is NOT a block-found prediction or
a validated improvement in block discovery.

The sidecar never calls any MCP mutation tool. There is no GPU access or
worker control in this code. The data and the checkpoint stay outside Git.

## Model

Official model: https://huggingface.co/mistralai/Timestral-209M

- 209M parameters, approximately 837 MB of FP32 weights **plus runtime RAM**
- CPU PyTorch inference, 2 threads by default
- Maximum 32,768 history steps and 1,024 future steps
- Apache-2.0 Python code; separately licensed Modified-MIT checkpoint
- Default admission: at least 3 GiB MemAvailable and 2 GiB free disk
- No deployment or inference against the live miner has been performed

Check the model's real license, including commercial-use restrictions:
https://huggingface.co/mistralai/Timestral-209M/blob/main/LICENSE

## Setup on CachyOS / fish

Use the existing local checkout of the experimental branch. Run from a
**normal KDE terminal, never a TTY**. All caches and temp directories are
persistent. The MCP service must already be running with a valid diagnostics
token through the existing Nexus authenticated proxy.

```fish
cd ~/Scaricati/KERYX-NEXUS
mkdir -p "$HOME/Scaricati/KERYX-NEXUS/.persistent-tmp"
set -gx TMPDIR "$HOME/Scaricati/KERYX-NEXUS/.persistent-tmp"
set -gx TMP "$TMPDIR"
set -gx TEMP "$TMPDIR"
set -l STATE "$HOME/.local/share/nexus-keryx-miner/predictive"
mkdir -p "$STATE"
python3 -m venv "$STATE/venv"
"$STATE/venv/bin/python" -m pip install --upgrade pip
"$STATE/venv/bin/python" -m pip install -r scripts/requirements-mcp.txt
"$STATE/venv/bin/python" -m pip install --index-url https://download.pytorch.org/whl/cpu 'torch>=2.4'
"$STATE/venv/bin/python" -m pip install 'numpy>=1.24' git+https://github.com/mistralai/timestral.git
```

Collect every 10 seconds (terminate with Ctrl+C):

```fish
set -l STATE "$HOME/.local/share/nexus-keryx-miner/predictive"
"$STATE/venv/bin/python" experiments/timestral/nexus_timestral.py collect --interval 10
```

After at least **65 uninterrupted fresh snapshots**, about 11 minutes,
run a 2-minute prediction in a second KDE terminal:

```fish
set -l STATE "$HOME/.local/share/nexus-keryx-miner/predictive"
"$STATE/venv/bin/python" experiments/timestral/nexus_timestral.py forecast --interval 10 --horizon 12 --threads 2
cat "$STATE/latest-forecast.json"
```

The first prediction downloads the checkpoint once. After download,
add "--offline" to prevent network access. If memory/disk admission fails,
the model will not load. The JSON is only a forecast artifact, **not** a
configuration consumed by a mining worker. Do not run two concurrent
collectors on one SQLite database.

## Tests (no model download, no GPU, no MCP service)

```fish
python3 -m unittest discover -s experiments/timestral -p 'test_*.py' -v
```

These tests validate schema, data gaps and counter deltas, not real
prediction accuracy. Before any tuning integration, add chronological
backtests, baseline comparisons, confidence calibration, and a whitelisted
opt-in policy that preserves proof verification and all AI deadlines.
