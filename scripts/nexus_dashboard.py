#!/usr/bin/env python3
"""Read-only terminal supervisor for NEXUS; all child output is durably logged."""
from __future__ import annotations

from nexus_gpu_identity import display_devices
import argparse
import curses
import copy
import json
import math
import os
import queue
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from collections import deque
from pathlib import Path
from decimal import Decimal
from urllib import request as http_request, parse as url_parse, error as http_error


def release_version():
    try:
        value = (Path(__file__).resolve().parents[1] / "VERSION").read_text().strip()
        if re.fullmatch(r"\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?", value): return value
    except OSError: pass
    return "0.0.3"


RELEASE_VERSION = release_version()
EXPLORER_BASE = "https://keryx-labs.com"
EXPLORER_TTL = 90

AUTHORIZED_GPU = os.environ.get("NEXUS_PRIMARY_GPU_UUID", "GPU-00000000-0000-0000-0000-000000000000")
KV = re.compile(r"([A-Za-z][A-Za-z0-9_]*)=([^\s]+)")
NATIVE_SCHEMA_VERSION = 1
TELEMETRY_STALE_NS = 3_000_000_000
MATRIX_BRIGHT = 2
MATRIX_DARK = 3
MATRIX_TEXT = 4
MATRIX_WARN = 5
MATRIX_ERROR = 6


class DashboardModel:
    def __init__(self):
        self.events = deque(maxlen=400)
        self._response_lifecycle_seen = set()
        self._assigned_request_ids = set()
        self._received_request_ids = set()
        self._block_seen = {key: set() for key in ("submit_sent", "rpc_ack", "rpc_rejected", "chain_observed", "chain_confirmed")}
        self._block_device = {}
        self.unresolved_block_submissions = set()
        self.last_submission_hash = None
        self.last_submission_state = None
        self._winner_seen = set()
        self.gpu_winners = {'rtx': 0, 'rx': 0}
        self.gpu_block_stats = {lane: {key: 0 for key in self._block_seen} for lane in ('rtx', 'rx')}
        self.gpu_block_unknown = {key: 0 for key in self._block_seen}
        self.milestones = deque(maxlen=80)
        self.network = "UNKNOWN"
        self.session = "CURRENT"
        self.log_scroll = 0
        self.history_complete = False
        self.states = {}
        self.stats = {
            "nonce_verified": 0, "rust_valid": 0, "rust_failed": 0,
            "kernels": 0, "stale": 0, "submit_sent": 0, "rpc_ack": 0, "rpc_rejected": 0,
            "chain_observed": 0, "chain_confirmed": 0,
        }
        self.last_job = "UNKNOWN"
        self.daa = "UNKNOWN"
        self.synced = "UNKNOWN"
        self.started = time.monotonic()
        self.gpu = "UNKNOWN"
        self.status = "STARTING"
        self.inference = "UNKNOWN"
        self.escrow = "UNKNOWN"
        self.ai_service = "UNKNOWN"
        self.ai_metrics = {"requests_received": None, "requests_assigned": None,
                           "responses_sent": None, "responses_confirmed": None,
                           "pending": None, "service_ms": None, "last_error": "UNKNOWN"}
        self.escrow_metrics = {"delegation": "UNKNOWN", "identity": "UNKNOWN",
                              "mature_claims": None, "pending_claims": None,
                              "confirmed_claims": None, "consecutive_strikes": None,
                              "total_strikes": None, "pending_burns": None,
                              "suspended_until_daa": None, "strike_status": "UNKNOWN",
                              "strike_observed_unix_ms": None, "strike_error": "UNKNOWN",
                              "escrow_pending_outpoints": None}
        self.native = None
        self.mining_payout = None
        self.last_measured_ms = {}
        self._inference_start_ns = None
        self._native_inference_start = None
        self.explorer = {}
        self.wallet = {"authorization": "UNKNOWN", "payout_address": None,
                       "escrow_public_key": None, "verification": "UNKNOWN",
                       "certificate_source": None, "error": None, "notice": None}
        self.native_health = "N/A"
        self.native_error = "worker snapshot unavailable"
        self.fixture_mode = False
        self.claims_disabled = None
        self.development = {}
        self._development_poll = 0
        self.development_notice = None
        self.updates = {}
        self.setup = {}
        self.gpu_devices = {}
        self.ai_backend = {}
        self.pom_backend = {}
        self.gpu_selection_runtime = {}
        self.amd_lane = {}
        self.amd_samples = deque(maxlen=65)
        self.gpu_samples = deque(maxlen=66)
        self._pending_amd = None
        self.setup_notice = None
        self._setup_check_thread = None
        self._setup_poll = 0
        self.escrow_available = {"outputs": None, "amount_sompi": None, "status": "N/A"}
        self.claimed = {"outputs": 0, "payout_sompi": 0, "transactions": 0, "status": "SESSION · ACK"}
        self._claim_acks = {}
        self._claim_error = False
        self._render_cache = None

    def presentation_copy(self):
        """Independent view snapshot without ingestion-only dedup indexes or threads."""
        snapshot = DashboardModel()
        excluded = {'_render_cache', '_setup_check_thread', '_claim_acks', '_claim_error',
                    '_block_seen', '_block_device', '_winner_seen', '_received_request_ids', '_assigned_request_ids',
                    '_response_lifecycle_seen'}
        for name, value in vars(self).items():
            if name not in excluded:
                setattr(snapshot, name, copy.deepcopy(value))
        return snapshot

    def _ingest_claim_submission(self, obj):
        """Count only unique ACKs in this worker log session; never replay all-run totals."""
        if obj.get("state") != "RPC_ACKNOWLEDGED": return
        try:
            txid, outputs, payout = obj.get("txid"), obj.get("inputs"), obj.get("payout_sompi")
            if not isinstance(txid, str) or not re.fullmatch(r"[0-9a-f]{64}", txid): raise ValueError("invalid claim ID")
            if type(outputs) is not int or not 1 <= outputs <= 87: raise ValueError("invalid claim count")
            if type(payout) is not int or not 0 < payout <= 2**64 - 1: raise ValueError("invalid claim payout")
            values = (outputs, payout)
            if txid in self._claim_acks and self._claim_acks[txid] != values: raise ValueError("conflicting claim ACK")
            if len(self._claim_acks) >= 10000 and txid not in self._claim_acks: raise ValueError("claim session limit")
            self._claim_acks[txid] = values
        except (ValueError, TypeError, AttributeError):
            self._claim_error = True
        if self._claim_error:
            self.claimed = {"outputs": None, "payout_sompi": None, "transactions": None, "status": "INVALID · session claim records"}
        else:
            self.claimed = {"outputs": sum(v[0] for v in self._claim_acks.values()),
                            "payout_sompi": sum(v[1] for v in self._claim_acks.values()),
                            "transactions": len(self._claim_acks), "status": "SESSION · ACK"}

    def ingest(self, line: str, observed_ns=None) -> dict:
        obj = None
        stripped = line.strip()
        if stripped.startswith("{"):
            try:
                obj = json.loads(stripped)
            except json.JSONDecodeError:
                pass
        if obj is None:
            obj = {"event": stripped.split(" ", 1)[0] if stripped else "OUTPUT"}
            obj.update({k: v.rstrip(",") for k, v in KV.findall(stripped)})
        event = str(obj.get("event", "OUTPUT"))
        if event == "V002_UI_FIXTURE":
            self.fixture_mode = True
        if event not in ("NEXUS_TELEMETRY", "GPU_POM_SELECTION"):
            timestamp = time.time()
            if isinstance(observed_ns, int) and 0 < observed_ns <= time.monotonic_ns():
                timestamp -= (time.monotonic_ns() - observed_ns) / 1e9
            item = (time.strftime("%H:%M:%S", time.localtime(timestamp)), event, event_summary(obj))
            self.events.append(item)
            if important_event(event): self.milestones.append(item)
        if event == "NEXUS_TELEMETRY":
            self._ingest_native_snapshot(obj)
        if event == "GPU_POM_SELECTION":
            flags = ['live_control'] + [lane + '_' + key for lane in ('nvidia', 'amd')
                                      for key in ('requested', 'available', 'enabled', 'applied')]
            if (type(obj.get('schema_version')) is int and obj['schema_version'] == 1 and
                    all(type(obj.get(key)) is bool for key in flags) and
                    obj.get('error') in ('', 'SELECTION_INVALID_POM_PAUSED')):
                self.gpu_selection_runtime = dict(obj)
        if event == "AI_BACKEND_SELECTED":
            expected = {"cuda": {AUTHORIZED_GPU}, "vulkan": {AUTHORIZED_GPU, "pci:0000:00:00.0"}}
            backend = obj.get("backend")
            if (type(obj.get("schema_version")) is int and obj["schema_version"] == 1 and
                    type(backend) is str and backend in expected and
                    obj.get("device_id") in expected[backend] and obj.get("trace_capture") is False):
                self.ai_backend = dict(obj)
        if event == "POM_BACKEND_SELECTED":
            if (type(obj.get('schema_version')) is int and obj['schema_version'] == 1 and
                    obj.get('backend') in ('cuda-driver','vulkan-jit','vulkan-bda') and obj.get('device_id') == AUTHORIZED_GPU):
                self.pom_backend = dict(obj)
        if event == "AMD_LANE_STATE":
            keys = ('attempts', 'completed', 'verified', 'discarded')
            if all(type(obj.get(k)) is int and 0 <= obj[k] <= 2**64 - 1 for k in keys) and all(
                    type(obj.get(k)) is bool for k in ('busy', 'preparing', 'paused', 'failed')):
                # The worker emits AMD counters immediately before its native
                # snapshot. Pair them with that producer clock, never replay speed.
                if obj['completed'] + obj['discarded'] <= obj['attempts']:
                    self._pending_amd = dict(obj)
        if event == "ESCROW_CLAIM_SUBMISSION":
            self._ingest_claim_submission(obj)
        if event == "LIVE_PHASE_PROFILE" and obj.get("phase") == "official_rust_verify":
            value = obj.get("duration_ms")
            if type(value) in (int, float) and math.isfinite(value) and value >= 0:
                self.last_measured_ms['rust_verify'] = value
        if event == "STARTUP_STATE":
            state, result = str(obj.get("state", "")), str(obj.get("result", "UNKNOWN"))
            if state == "BOOT":
                self.native = None; self.native_health = 'N/A'
                self.amd_lane = {}; self.amd_samples.clear(); self.ai_backend = {}; self.gpu_selection_runtime = {}
                self.pom_backend = {}
                self.gpu_samples.clear(); self._pending_amd = None
                self._block_device.clear(); self._winner_seen.clear()
                self.gpu_winners = {'rtx': 0, 'rx': 0}
                for key in self._block_seen:
                    self._block_seen[key].clear(); self.stats[key] = 0
                    self.gpu_block_unknown[key] = 0
                    for lane in self.gpu_block_stats: self.gpu_block_stats[lane][key] = 0
                self._claim_acks.clear(); self._claim_error = False
                self.claimed = {"outputs": 0, "payout_sompi": 0, "transactions": 0, "status": "SESSION · ACK"}
            self.states[state] = result
            if state == "ESCROW_IDENTITY_READY":
                self.escrow = result
                self.escrow_metrics["identity"] = result
            if state in ("AI_REQUEST_SERVICE_READY", "AI_RESPONSE_TRANSPORT_READY", "PENDING_REQUEST_RECOVERY", "SERVICE_READY"):
                self.ai_service = result
            if state == "ESCROW_CLAIM_ENGINE_READY":
                reason = str(obj.get("reason", "")).lower()
                if 'disabled' in reason: self.claims_disabled = True
                elif 'explicitly authorized' in reason: self.claims_disabled = False
            if state.startswith("INFERENCE_") or state == "CUDA_ENGINE_READY": self.inference = result
            if result in ("FAIL", "BLOCKED"): self.status = "DEGRADED"
            elif state == "NODE_SYNC" and result == "PASS": self.status = "WAITING"
        if event == "NODE_INFO": self.synced = str(obj.get("synced", "UNKNOWN")).lower()
        if event == "NODE_NETWORK": self.network = str(obj.get("name", "UNKNOWN"))
        if event in ("CONNECT_FAILED", "DISCONNECTED"): self.synced = "UNKNOWN"
        if event == "AI_SERVICE_RPC_SNAPSHOT":
            self.escrow_metrics["consecutive_strikes"] = "UNKNOWN"
            self.escrow_metrics["total_strikes"] = "UNKNOWN"
            self.ai_service = "WAITING" if obj.get("providers_available") else "UNKNOWN"
        if (event == "AI_STARTUP_BACKFILL=COMPLETE" and
                str(obj.get("unavailable")) == "0" and self.ai_metrics["pending"] is None):
            pending = str(obj.get("known_pending", ""))
            if pending.isascii() and pending.isdecimal():
                self.ai_metrics["pending"] = int(pending)
        if event == "AI_SERVICE_STATUS": self.ai_service = str(obj.get("status", "UNKNOWN"))
        if event in ("AI_REQUEST_DISCOVERED", "AI_REQUEST_QUEUED"):
            request_id = str(obj.get("request_id", ""))
            if not request_id or request_id not in self._received_request_ids:
                if request_id: self._received_request_ids.add(request_id)
                self.ai_metrics["requests_received"] = int(self.ai_metrics["requests_received"] or 0) + 1
            if "pending" in obj: self.ai_metrics["pending"] = obj["pending"]
        if event == "AI_REQUEST_ASSIGNED":
            request_id = str(obj.get("request_id", ""))
            if not request_id or request_id not in self._assigned_request_ids:
                if request_id: self._assigned_request_ids.add(request_id)
                self.ai_metrics["requests_assigned"] = int(self.ai_metrics["requests_assigned"] or 0) + 1
        if event == "AI_RESPONSE_SUBMITTED":
            self.ai_metrics["responses_sent"] = int(self.ai_metrics["responses_sent"] or 0) + 1
        if event == "AI_RESPONSE_CONFIRMED":
            self.ai_metrics["responses_confirmed"] = int(self.ai_metrics["responses_confirmed"] or 0) + 1
        if event == "AI_RESPONSE_CHAIN_OBSERVED":
            self.ai_metrics["responses_confirmed"] = int(self.ai_metrics["responses_confirmed"] or 0) + int(obj.get("count", 1))
        if event == "AI_SERVICE_ERROR": self.ai_metrics["last_error"] = str(obj.get("reason", "UNKNOWN"))
        if event == "AI_SERVICE_METRICS":
            for key in ("requests_received", "requests_assigned", "responses_sent", "responses_confirmed", "pending", "service_ms"):
                if key in obj: self.ai_metrics[key] = obj[key]
        if event == "ESCROW_SERVICE_SNAPSHOT":
            for key in self.escrow_metrics:
                if key in obj: self.escrow_metrics[key] = obj[key]
            self.escrow = str(obj.get("status", self.escrow))
        if event == "SERVICE_STRIKES_SNAPSHOT":
            self.escrow_metrics["consecutive_strikes"] = obj.get("consecutive", "UNKNOWN")
            self.escrow_metrics["total_strikes"] = obj.get("lifetime", "UNKNOWN")
            self.escrow_metrics["pending_burns"] = obj.get("pending_burns", "UNKNOWN")
            self.escrow_metrics["suspended_until_daa"] = obj.get("suspended_until_daa", "UNKNOWN")
        if event == "SERVICE_STRIKE_POLL":
            available = obj.get("availability") == "AVAILABLE" and obj.get("stale") is False
            self.escrow_metrics["strike_status"] = "AVAILABLE" if available else ("STALE" if obj.get("stale") else "UNKNOWN")
            for key, source in (("consecutive_strikes", "consecutive"),
                                ("total_strikes", "lifetime"),
                                ("pending_burns", "pending_burns"),
                                ("suspended_until_daa", "suspended_until_daa")):
                self.escrow_metrics[key] = obj.get(source) if available else None
            self.escrow_metrics["strike_observed_unix_ms"] = obj.get("observed_unix_ms")
            self.escrow_metrics["strike_error"] = obj.get("error", "UNKNOWN")
        if event == "ESCROW_WATCHER_BLOCK":
            if obj.get("claim_submission") == "DISABLED": self.claims_disabled = True
            elif obj.get("claim_submission") == "ENABLED": self.claims_disabled = False
            # This is the stock watcher's unresolved outpoint count, not a claim count.
            self.escrow_metrics["escrow_pending_outpoints"] = obj.get("pending_count")
        if event == "AI_RESPONSE_LIFECYCLE":
            state = str(obj.get("state", "UNKNOWN"))
            request_id = str(obj.get("request_id", ""))
            if "pending" in obj: self.ai_metrics["pending"] = obj["pending"]
            if request_id and state not in ("NOT_ASSIGNED", "FAILED", "EXPIRED", "UNKNOWN") and request_id not in self._assigned_request_ids:
                self._assigned_request_ids.add(request_id)
                self.ai_metrics["requests_assigned"] = int(self.ai_metrics["requests_assigned"] or 0) + 1
            for group, states in (("responses_sent", ("RPC_ACKNOWLEDGED", "UNKNOWN_AFTER_TIMEOUT", "SUBMITTING")),
                                  ("responses_confirmed", ("CHAIN_OBSERVED", "FINALIZED"))):
                token = (request_id, group)
                if request_id and state in states and token not in self._response_lifecycle_seen:
                    self._response_lifecycle_seen.add(token)
                    self.ai_metrics[group] = int(self.ai_metrics[group] or 0) + 1
            if state in ("FAILED", "EXPIRED", "RPC_REJECTED"):
                self.ai_metrics["last_error"] = state
        if event == "LIVE_TEMPLATE":
            if "synced" in obj: self.synced = str(obj["synced"]).lower()
            if "network" in obj: self.network = str(obj["network"])
            self.last_job = str(obj.get("generation", "UNKNOWN"))
            self.daa = str(obj.get("daa", "UNKNOWN"))
        if event == "LIVE_CUDA_NONCE=PASS" or obj.get("LIVE_CUDA_NONCE") == "PASS":
            self.stats["nonce_verified"] += 1
            self.stats["rust_valid"] += 1
            self.stats["kernels"] += 1
        if "LIVE_CUDA_STALE_DISCARD" in event or obj.get("event") == "LIVE_CUDA_STALE_DISCARD":
            self.stats["stale"] += 1
        if "RUST_VERIFY_FAIL" in stripped: self.stats["rust_failed"] += 1
        winner_lane = ('rx' if event == 'AMD_CANDIDATE_VERIFIED' else
                       'rtx' if event in ('LIVE_CUDA_NONCE=PASS','NVIDIA_VULKAN_CANDIDATE_VERIFIED') else None)
        if (winner_lane and obj.get('target_satisfied') in (True, 'true') and
                obj.get('rust') == 'PASS' and str(obj.get('nonce', '')).isascii() and
                str(obj.get('nonce', '')).isdecimal()):
            token = (winner_lane, str(obj['nonce']))
            if token not in self._winner_seen:
                self._winner_seen.add(token); self.gpu_winners[winner_lane] += 1
        if event in ("LIVE_SUBMIT_RESULT", "LIVE_CHAIN_STATUS"):
            state = str(obj.get("state", ""))
            identity = str(obj.get("hash", "")).lower()
            if re.fullmatch(r'[0-9a-f]{64}', identity):
                if event == 'LIVE_SUBMIT_RESULT':
                    self.last_submission_hash, self.last_submission_state = identity, state
                    if state in ('UNKNOWN_OUTCOME', 'UNKNOWN_AFTER_TIMEOUT', 'SUBMIT_SENT'):
                        if identity not in self._block_seen['chain_observed']:
                            self.unresolved_block_submissions.add(identity)
                    elif state in ('RPC_ACKNOWLEDGED', 'RPC_REJECTED'):
                        self.unresolved_block_submissions.discard(identity)
                elif state in ('CHAIN_OBSERVED', 'CHAIN_CONFIRMED'):
                    self.unresolved_block_submissions.discard(identity)
            if event == 'LIVE_SUBMIT_RESULT' and re.fullmatch(r'[0-9a-f]{64}', identity):
                lane = {'AMD_VULKAN': 'rx', 'CUDA': 'rtx', 'NVIDIA_VULKAN':'rtx', None: 'rtx'}.get(obj.get('source'))
                previous = self._block_device.get(identity)
                if lane and previous is None:
                    self._block_device[identity] = lane
                    for key, seen in self._block_seen.items():
                        if identity in seen: self.gpu_block_stats[lane][key] += 1
                elif lane and previous in ('rtx', 'rx') and previous != lane:
                    for key, seen in self._block_seen.items():
                        if identity in seen: self.gpu_block_stats[previous][key] -= 1
                    self._block_device[identity] = 'UNKNOWN'
            def count_once(key):
                if not re.fullmatch(r"[0-9a-f]{64}", identity): return
                if identity not in self._block_seen[key]:
                    self._block_seen[key].add(identity); self.stats[key] += 1
                    lane = self._block_device.get(identity)
                    if lane in self.gpu_block_stats: self.gpu_block_stats[lane][key] += 1
            if event == "LIVE_SUBMIT_RESULT":
                if state in ("SUBMIT_SENT", "RPC_ACKNOWLEDGED", "RPC_REJECTED", "UNKNOWN_AFTER_TIMEOUT"):
                    count_once("submit_sent")
                if state == "RPC_ACKNOWLEDGED": count_once("rpc_ack")
                if state == "RPC_REJECTED": count_once("rpc_rejected")
            elif state in ("CHAIN_OBSERVED", "CHAIN_CONFIRMED"):
                count_once("chain_observed")
                selected = obj.get("selected_chain") is True or obj.get("selected_chain") == "true"
                blue, tip = (str(obj.get(key, "")) for key in ("blue_score", "selected_tip_blue_score"))
                depth_verified = all(value.isascii() and value.isdecimal() and len(value) <= 20
                                     for value in (blue, tip))
                if depth_verified:
                    blue, tip = int(blue), int(tip)
                    depth_verified = 0 <= blue <= tip < 2**64 and tip - blue >= 6
                if state == "CHAIN_CONFIRMED" and selected and depth_verified:
                    count_once("chain_confirmed")
            self.gpu_block_unknown = {key: self.stats[key] - sum(v[key] for v in self.gpu_block_stats.values())
                                      for key in self._block_seen}
        if event == "LIVE_CUDA_WORKER=START": self.status = "MINING"
        if event == "LIVE_CUDA_WORKER=STOP": self.status = "WAITING"
        if event == "INFERENCE_SELF_TEST=PASS": self.inference = "READY"
        if event == "OPOI_RUNTIME=START":
            self.inference = "BUSY"
            self._inference_start_ns = observed_ns if type(observed_ns) is int and observed_ns > 0 else None
        if event == "OPOI_RUNTIME=PASS":
            self.inference = "READY"
            if self._inference_start_ns and type(observed_ns) is int and observed_ns >= self._inference_start_ns:
                self.ai_metrics['service_ms'] = (observed_ns - self._inference_start_ns) / 1e6
                self.ai_metrics['service_source'] = 'EVENT_INTERVAL'
            self._inference_start_ns = None
        return obj

    def _ingest_native_snapshot(self, obj):
        try:
            if obj.get("schema_version") != NATIVE_SCHEMA_VERSION:
                raise ValueError("unsupported schema version")
            stamp = obj.get("monotonic_ns")
            elapsed = obj.get("elapsed_ns")
            counters = obj.get("counters")
            rates = obj.get("rates")
            state = obj.get("state")
            if type(stamp) is not int or stamp <= 0 or type(elapsed) is not int or elapsed < 0:
                raise ValueError("invalid monotonic timestamp or elapsed time")
            required = ("gpu_work_units_launched", "gpu_work_units_completed", "pom_steps_completed",
                        "pom_kernels_completed", "nonce_attempts", "proofs_completed",
                        "proofs_rust_verified", "proofs_stale",
                        "stale_work_discarded", "targets_satisfied")
            if not isinstance(counters, dict) or any(type(counters.get(key)) is not int or counters[key] < 0 for key in required):
                raise ValueError("invalid cumulative counter")
            if "proofs_fresh" in counters and (type(counters["proofs_fresh"]) is not int or
                                                 counters["proofs_fresh"] < 0):
                raise ValueError("invalid fresh proof counter")
            if (not isinstance(rates, dict) or rates.get("stock_comparable_mhs") is not None or
                    not isinstance(state, dict) or not isinstance(state.get("mining"), str) or
                    not isinstance(state.get("inference"), str)):
                raise ValueError("invalid rates or state object")
            if state.get('pom_backend', 'UNKNOWN') not in ('UNKNOWN', 'cuda-driver', 'vulkan-jit', 'vulkan-bda'):
                raise ValueError("invalid PoM execution backend")
            for name in ("pom_msteps_s_1s", "pom_msteps_s_10s", "pom_msteps_s_60s",
                         "kernels_s_1s", "kernels_s_10s", "kernels_s_60s",
                         "proofs_s_1s", "proofs_s_10s", "proofs_s_60s", "stale_work_fraction"):
                value = rates.get(name)
                if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
                    raise ValueError("invalid rolling rate")
            if rates["stale_work_fraction"] > 1:
                raise ValueError("stale fraction exceeds completed work")
            if any(type(rates.get(key)) is not bool for key in ("valid_1s", "valid_10s", "valid_60s", "valid_stale_work_fraction")):
                raise ValueError("invalid rolling rate validity")
            latency = obj.get("latency_ms")
            if not isinstance(latency, dict) or any(
                    value is not None and (isinstance(value, bool) or not isinstance(value, (int, float))
                                           or not math.isfinite(value) or value < 0)
                    for value in latency.values()):
                raise ValueError("invalid latency metrics")
            wallet = obj.get("wallet")
            if wallet is not None:
                if (not isinstance(wallet, dict) or
                        wallet.get("authorization") not in ("AUTHORIZED", "INVALID", "UNKNOWN") or
                        wallet.get("verification") not in ("VALID", "FAILED", "UNKNOWN")):
                    raise ValueError("invalid wallet authorization state")
                pubkey = wallet.get("escrow_public_key")
                if pubkey is not None and (not isinstance(pubkey, str) or
                                           not re.fullmatch(r"[0-9a-f]{64}", pubkey)):
                    raise ValueError("invalid escrow public key")
                payout = wallet.get('payout_address')
                if payout is not None and (not isinstance(payout, str) or not re.fullmatch(r"keryx:[a-z0-9]{40,120}", payout)):
                    raise ValueError("invalid worker payout address")
            if self.native and stamp <= self.native.get("monotonic_ns", 0):
                raise ValueError("duplicate or out-of-order monotonic timestamp")
            if self.native:
                if elapsed < self.native.get("elapsed_ns", 0):
                    raise ValueError("elapsed time moved backwards")
                previous = self.native.get("counters", {})
                if any(counters[key] < previous.get(key, 0) for key in required):
                    raise ValueError("cumulative counter moved backwards")
                if ("proofs_fresh" in counters and "proofs_fresh" in previous and
                        counters["proofs_fresh"] < previous["proofs_fresh"]):
                    raise ValueError("fresh proof counter moved backwards")
            previous_stamp = self.native.get('monotonic_ns') if self.native else None
            previous_inference = self.native.get('state', {}).get('inference') if self.native else None
            if state['inference'] == 'RUNNING' and previous_inference == 'IDLE' and previous_stamp:
                self._native_inference_start = (stamp, stamp - previous_stamp)
            elif state['inference'] == 'IDLE' and previous_inference == 'RUNNING' and self._native_inference_start and previous_stamp:
                start, start_gap = self._native_inference_start
                self.ai_metrics['service_ms'] = (stamp - start) / 1e6
                self.ai_metrics['service_uncertainty_ms'] = (start_gap + stamp - previous_stamp) / 1e6
                self.ai_metrics['service_source'] = 'NATIVE_SAMPLED'
                self._native_inference_start = None
            self.native = obj
            if state.get('pom_backend') in ('cuda-driver', 'vulkan-jit', 'vulkan-bda'):
                self.pom_backend = {'schema_version': 1, 'device_id': AUTHORIZED_GPU,
                                    'backend': state['pom_backend']}
            from nexus_gpu_metrics import paired_sample
            paired_sample(self, obj)
            next_payout = wallet.get('payout_address') if wallet else None
            if self.mining_payout != next_payout: self.explorer.clear()
            self.mining_payout = next_payout
            for key, value in latency.items():
                if value is not None: self.last_measured_ms[key] = value
            if wallet is not None:
                notices = {k: self.wallet[k] for k in ("notice", "ui_error") if self.wallet.get(k)}
                self.wallet = {**wallet, **notices}
                if wallet.get("authorization") == "AUTHORIZED": self.escrow_metrics["identity"] = "VALID"
                if wallet.get("verification") == "VALID": self.escrow_metrics["delegation"] = "VALID"
            self.native_health = "LIVE"
            self.native_error = ""
        except (ValueError, TypeError, KeyError) as exc:
            self._pending_amd = None
            self.native_health = "INVALID"
            self.native_error = str(exc)

    def native_status(self):
        if self.native is None:
            return "N/A"
        age = time.monotonic_ns() - self.native["monotonic_ns"]
        if age < 0 or age > TELEMETRY_STALE_NS:
            return "STALE"
        return self.native_health

    def snapshot(self):
        if self.native is None:
            return {"uptime": None, "pom_msteps_s": None, "proofs_s": None, "kernels_s": None}
        native = self.native
        rates = native["rates"]
        fresh = self.native_status() == "LIVE"
        return {
            "uptime": native["elapsed_ns"] / 1.0e9,
            "pom_msteps_s": rates["pom_msteps_s_1s"] if fresh and rates["valid_1s"] else None,
            "proofs_s": rates["proofs_s_1s"] if fresh and rates["valid_1s"] else None,
            "kernels_s": rates["kernels_s_1s"] if fresh and rates["valid_1s"] else None,
        }


def gpu_sample() -> str:
    # OFF excludes NVIDIA from periodic UI/API/MCP diagnostics too.
    from nexus_gpu_selection import configuration
    try:
        if not configuration()['nvidia_pom']:
            return 'DISABLED'
    except (OSError, ValueError):
        return 'UNKNOWN'

    # Explicit UUID selection is mandatory; never enumerate/query other devices.
    cmd = ["nvidia-smi", "-i", AUTHORIZED_GPU, "--query-gpu=utilization.gpu,memory.used,memory.total,temperature.gpu,power.draw,clocks.gr", "--format=csv,noheader,nounits"]
    try:
        result = subprocess.run(cmd, check=True, text=True, capture_output=True, timeout=2)
        return result.stdout.strip().replace("\n", " ") or "UNKNOWN"
    except (OSError, subprocess.SubprocessError):
        return "UNKNOWN"


def load_wallet_status(model: DashboardModel) -> None:
    """Read current local wallet authorization without exposing cert/key material."""
    helper = Path(__file__).with_name("nexus_wallet_authorization.py")
    try:
        result = subprocess.run([sys.executable, "-B", str(helper), "status"],
                                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True, check=False, timeout=8)
        payload = json.loads(result.stdout) if result.stdout else {}
        if isinstance(payload, dict) and payload.get("authorization") in ("AUTHORIZED", "INVALID"):
            if model.mining_payout:
                if payload.get('payout_address') != model.mining_payout or payload.get('escrow_public_key') != model.wallet.get('escrow_public_key'):
                    model.wallet['ui_error'] = 'Local certificate config differs from active worker; active payout retained'
            else: model.wallet = {**model.wallet, **payload}
    except (OSError, ValueError, subprocess.SubprocessError):
        if model.mining_payout: model.wallet['ui_error'] = 'Local certificate status unavailable; worker identity retained'
        else: model.wallet = {**model.wallet, "authorization": "UNKNOWN",
                             "verification": "UNKNOWN", "error": "local status unavailable"}


def import_wallet_certificate(stdscr, model: DashboardModel) -> None:
    """Ask for a local cert path and verify/store it; never accepts a key or password."""
    try:
        height, width = stdscr.getmaxyx()
        prompt = "Wallet escrow.cert path (wallet-signed 128 hex file), Enter to cancel: "
        stdscr.addnstr(max(0, height - 2), 0, prompt, max(1, width - 1))
        stdscr.refresh()
        stdscr.nodelay(False)
        curses.echo()
        curses.curs_set(1)
        raw = stdscr.getstr(max(0, height - 2), min(len(prompt), max(0, width - 2)), 1024)
        path = raw.decode("utf-8", errors="strict").strip()
    except (curses.error, UnicodeError):
        model.wallet = {**model.wallet, "error": "certificate path input unavailable"}
        return
    finally:
        try: stdscr.nodelay(True)
        except (curses.error, AttributeError): pass
        model._render_cache = None
        try: curses.noecho()
        except curses.error: pass
        try: curses.curs_set(0)
        except curses.error: pass
    if not path:
        model.wallet = {**model.wallet, "error": None}
        return
    helper = Path(__file__).with_name("nexus_wallet_authorization.py")
    try:
        result = subprocess.run([sys.executable, "-B", str(helper), "import", "--certificate", path],
                                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True, check=False, timeout=10)
        output = result.stdout.strip()
        model.wallet["ui_error"] = None if result.returncode == 0 else "Certificate rejected; current worker identity unchanged"
        model.wallet["notice"] = "Certificate verified locally; no worker restart or transaction" if result.returncode == 0 else None
        load_wallet_status(model)
    except (OSError, subprocess.SubprocessError):
        model.wallet = {**model.wallet, "error": "wallet certificate import could not be completed"}


def copy_escrow_pubkey(model: DashboardModel) -> str:
    pubkey = model.wallet.get("escrow_public_key")
    if not isinstance(pubkey, str) or not re.fullmatch(r"[0-9a-f]{64}", pubkey):
        return "escrow public key unavailable"
    return copy_public_value(pubkey, 'escrow public key')


def copy_public_value(value, label):
    # KDE Wayland already owns the clipboard through Klipper. D-Bus carries the
    # value in memory, never as a shell argument or a temporary file.
    try:
        import dbus
        bus = dbus.SessionBus()
        klipper = dbus.Interface(bus.get_object('org.kde.klipper', '/klipper'), 'org.kde.klipper.klipper')
        klipper.setClipboardContents(value, timeout=2)
        return label + ' copied to clipboard'
    except Exception:
        pass
    commands = []
    if shutil.which("wl-copy"):
        commands.append(["wl-copy"])
    if shutil.which("xclip"):
        commands.append(["xclip", "-selection", "clipboard"])
    for command in commands:
        try:
            result = subprocess.run(command, input=value, text=True,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                    check=False, timeout=2)
            if result.returncode == 0:
                return label + " copied to clipboard"
        except (OSError, subprocess.SubprocessError):
            continue
    return "Clipboard unavailable: check KDE Klipper"


def host_memory(model):
    now = time.monotonic()
    cached = getattr(model, '_host_memory', (0, {}))
    if now - cached[0] < 2: return cached[1]
    values = {}
    try:
        for line in Path('/proc/meminfo').read_text().splitlines():
            key, _, tail = line.partition(':')
            if key in ('MemTotal', 'MemAvailable'):
                values[key] = int(tail.split()[0]) / 1048576
        if len(values) != 2: values = {}
    except (OSError, ValueError, IndexError): values = {}
    model._host_memory = (now, values)
    return values


def set_matrix_background(enabled: bool) -> None:
    # OSC 11 changes only this terminal session; OSC 111 restores its configured default.
    if not sys.stdout.isatty():
        return
    sys.stdout.write((f"\x1b]0;Nexus - {RELEASE_VERSION}\x07\x1b]30;Nexus - {RELEASE_VERSION}\x07\x1b]11;#000000\x07") if enabled else "\x1b]111\x07")
    sys.stdout.flush()


def explorer_view(model, name):
    item = model.explorer.get(name, {})
    age = time.monotonic() - item.get("observed", 0)
    live = item.get("status") == "AVAILABLE" and 0 <= age <= EXPLORER_TTL
    return (item.get("data", {}) if live else {}), (f"LIVE · {age:.0f}s" if live else "STALE" if item.get("data") else item.get("status", "WAITING"))


def krx(value, italian=False):
    if isinstance(value, bool) or not isinstance(value, (int, str)) or not re.fullmatch(r"\d{1,30}", str(value)): return "N/A"
    amount = f"{Decimal(str(value)) / Decimal(100_000_000):,.8f}"
    if italian: amount = amount.translate(str.maketrans({',': '.', '.': ','}))
    return amount + " KRX"


class OfficialExplorerPoller:
    """Advisory GETs only. Network I/O runs outside the terminal/worker threads."""
    routes = {"balance": "balance", "inferences": "inferences-served", "strikes": "service-strikes", "reward": "holder-reward"}

    class OfficialRedirect(http_request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            if url_parse.urlsplit(newurl).netloc != "keryx-labs.com" or url_parse.urlsplit(newurl).scheme != "https":
                raise ValueError("non-official redirect rejected")
            return super().redirect_request(req, fp, code, msg, headers, newurl)

    def __init__(self):
        self.results = queue.Queue(maxsize=1); self.thread = None; self.next_poll = 0

    @staticmethod
    def get(path):
        opener = http_request.build_opener(OfficialExplorerPoller.OfficialRedirect())
        req = http_request.Request(EXPLORER_BASE + path, headers={"Accept": "application/json", "User-Agent": f"Nexus/{RELEASE_VERSION}", "Cache-Control": "no-cache"}, method="GET")
        with opener.open(req, timeout=4) as response:
            if response.headers.get_content_type() != "application/json": raise ValueError("non-JSON API response")
            raw = response.read(262145)
        if len(raw) > 262144: raise ValueError("API response exceeds bounded buffer")
        payload = json.loads(raw)
        if not isinstance(payload, dict): raise ValueError("API response is not an object")
        return payload

    @staticmethod
    def normalize(name, payload):
        fields = {"balance": ("balance_sompi",), "inferences": ("served", "answered", "last_daa_score"),
                  "strikes": ("consecutive_misses", "lifetime_strikes", "suspended_until_daa_score", "virtual_daa_score"),
                  "reward": ("bracket_bps", "tip_daa", "window_daa")}
        required = {"balance": ("balance_sompi",), "inferences": ("served",),
                    "strikes": ("consecutive_misses", "lifetime_strikes", "suspended_until_daa_score"), "reward": ("bracket_bps",)}
        if any(key not in payload for key in required[name]): raise ValueError("incomplete API schema")
        def integer(value):
            if value is None: return None
            if type(value) is int and 0 <= value < 10**30: return value
            if isinstance(value, str) and re.fullmatch(r"\d{1,30}", value): return int(value)
            raise ValueError("invalid API counter")
        out = {key: integer(payload[key]) for key in fields[name] if key in payload}
        if any(out.get(key) is None for key in required[name] if key != "suspended_until_daa_score"):
            raise ValueError("missing API counter")
        if name == "strikes" and "pending_burns" in payload:
            if not isinstance(payload['pending_burns'], list): raise ValueError("invalid pending burns")
            out['pending_burns_count'] = len(payload['pending_burns'])
        if name == "reward" and payload.get('income') is not None:
            if not isinstance(payload['income'], dict): raise ValueError("invalid income schema")
            keys = ("paid_sompi", "escrow_sompi", "inference_sompi", "chain_blocks", "income_window_daa")
            out['income'] = {key: integer(payload['income'][key]) for key in keys if key in payload['income']}
        return out

    def pump(self, model):
        address = model.mining_payout
        try:
            identity, items = self.results.get_nowait()
            if identity == address and model.network == "mainnet": model.explorer.update(items)
        except queue.Empty: pass
        if model.network != "mainnet":
            model.explorer.clear(); return
        if model.native_status() != 'LIVE': return
        if not isinstance(address, str) or not re.fullmatch(r"keryx:[a-z0-9]{40,120}", address): return
        if self.thread and self.thread.is_alive(): return
        if time.monotonic() < self.next_poll: return
        self.next_poll = time.monotonic() + 30
        def fetch():
            items = {}
            for name, route in self.routes.items():
                try:
                    data = self.normalize(name, self.get("/api/v1/addresses/" + url_parse.quote(address, safe="") + "/" + route))
                    items[name] = {"status": "AVAILABLE", "observed": time.monotonic(), "data": data}
                except (OSError, ValueError, http_error.HTTPError) as exc:
                    items[name] = {"status": f"HTTP {exc.code}" if isinstance(exc, http_error.HTTPError) else "UNAVAILABLE"}
            self.results.put((address, items))
        self.thread = threading.Thread(target=fetch, name="nexus-explorer-readonly", daemon=True); self.thread.start()


def _largest_reported_phase(latency, fresh):
    if not fresh:
        return "STALE"
    phases = (("template wait", "template_wait"), ("GGUF gather", "gguf_gather_wall"),
              ("Merkle paths", "merkle_wall"), ("CUDA engine", "cuda_engine_total"))
    measured = [(label, latency.get(key)) for label, key in phases
                if isinstance(latency.get(key), (int, float)) and not isinstance(latency.get(key), bool)]
    if not measured:
        return "N/A"
    label, value = max(measured, key=lambda item: item[1])
    return f"{label} ({value:.3f} ms)"


def important_event(event):
    return any(key in event for key in ("TARGET", "NONCE=PASS", "SUBMIT_RESULT", "CHAIN_STATUS", "STALE_DISCARD", "STALE_WINNER", "AI_REQUEST_ASSIGNED", "AI_RESPONSE", "CONNECT_FAILED", "WORKER=STOP"))


def event_summary(obj):
    """Readable event details; prompts, payloads, certificates and secrets excluded."""
    event = str(obj.get("event", "OUTPUT"))
    fields = []
    for key in ("state", "status", "generation", "daa", "nonce", "stage", "reason", "reject_reason", "hash", "selected_chain", "availability", "pending", "duration_ms"):
        value = obj.get(key)
        if value is None: continue
        text = str(value).replace("\n", " ").replace("\r", " ")
        if key == "hash": text = text[:12] + "…" + text[-6:]
        if key == "duration_ms":
            try: text = f"{float(value):.2f} ms"
            except (ValueError, TypeError): continue
        fields.append(f"{key.replace('_', ' ')} {text[:100]}")
    if event == "LIVE_TEMPLATE_PROFILE":
        fields = [f"RPC {obj.get('grpc_request_ms', 0):.2f} ms", f"validation {obj.get('validation_ms', 0):.2f} ms"]
    if event == "STARTUP_STATE": fields = [str(obj.get("state", "STARTUP")), str(obj.get("result", "UNKNOWN"))]
    return " · ".join(fields) or event.replace("_", " ")


def event_matches(event, selected):
    if selected == "ALL": return True
    groups = {"AI": ("AI_", "OPOI", "INFERENCE"), "SUBMIT": ("SUBMIT", "CHAIN", "RPC_ACKNOWLEDGED", "RPC_REJECTED", "TARGET"),
              "JOB": ("JOB", "TEMPLATE", "STALE"), "ERROR": ("ERROR", "FAIL", "BLOCKED", "REJECTED")}
    return any(token in event.upper() for token in groups.get(selected, (selected,)))


def number(value, precision=2):
    if value is None or isinstance(value, bool): return "N/A"
    try:
        v = float(value)
        if not math.isfinite(v): return "N/A"
        if abs(v) >= 1_000_000_000: return f"{v / 1_000_000_000:.3f} B"
        if abs(v) >= 1_000_000: return f"{v / 1_000_000:.3f} M"
        if abs(v) >= 1000: return f"{v:,.0f}"
        return f"{v:.{precision}f}"
    except (TypeError, ValueError): return str(value)


def format_hashrate(value):
    """Final PoM digests per second, with decimal SI prefixes."""
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
        return "N/A"
    for scale, unit in ((1e12, "TH/s"), (1e9, "GH/s"), (1e6, "MH/s"), (1e3, "kH/s")):
        if value >= scale: return f"{value / scale:.3f} {unit}"
    return f"{value:.3f} H/s"


def pom_hashrate(model, window):
    # Resident search completes all 256 walk steps, one final pom_pow_fold and
    # one target comparison per completed work unit. Includes stale work.
    # Source: consensus/cuda_resident_kernels.cu and cuda_resident.cpp.
    if model.native_status() != "LIVE": return None
    snap = model.native; counters = snap.get("counters", {}); rates = snap.get("rates", {})
    completed = counters.get("gpu_work_units_completed", 0)
    if completed <= 0 or counters.get("pom_steps_completed") != completed * 256: return None
    steps = rates.get("pom_msteps_s_" + window)
    if not rates.get("valid_" + window) or isinstance(steps, bool) or not isinstance(steps, (int, float)) or not math.isfinite(steps) or steps < 0:
        return None
    return steps * 1e6 / 256


def uptime_text(seconds):
    if seconds is None: return "N/A"
    seconds = max(0, int(seconds)); days, seconds = divmod(seconds, 86400)
    hours, seconds = divmod(seconds, 3600); minutes, seconds = divmod(seconds, 60)
    return (f"{days}d " if days else "") + f"{hours:02}:{minutes:02}:{seconds:02}"


def gpu_fields(raw):
    try:
        values = [float(x.strip()) for x in raw.split(",")]
        if len(values) != 6 or any(not math.isfinite(x) or x < 0 for x in values): return {}
        return dict(zip(("util", "used", "total", "temp", "power", "clock"), values))
    except (ValueError, TypeError, AttributeError): return {}


def select_page(key, tab):
    if ord("1") <= key <= ord("8"): return key - ord("1")
    keys = (curses.KEY_F1, curses.KEY_F2, curses.KEY_F3, curses.KEY_F4, curses.KEY_F5, curses.KEY_F6, curses.KEY_F7, curses.KEY_F8)
    if key in keys: return keys.index(key)
    if key in (9, curses.KEY_RIGHT): return (tab + 1) % 8
    if key in (curses.KEY_BTAB, curses.KEY_LEFT): return (tab - 1) % 8
    return tab


class MatrixFrame:
    """A fixed cell canvas. No scrolling terminal output, animation or worker action."""
    def __init__(self, height, width):
        self.height, self.width = height, max(1, width - 1)
        self.chars = [[" "] * self.width for _ in range(height)]
        self.roles = [["text"] * self.width for _ in range(height)]

    def text(self, y, x, text, role="text", limit=None):
        if y < 0 or y >= self.height or x >= self.width: return
        text = str(text).replace("\n", " ").replace("\r", " ")
        room = self.width - max(0, x)
        if limit is not None: room = min(room, max(0, limit))
        if len(text) > room and room > 1: text = text[:room - 1] + "…"
        for i, char in enumerate(text[:room]):
            col = x + i
            if 0 <= col < self.width: self.chars[y][col], self.roles[y][col] = char, role

    def box(self, y, x, height, width, title):
        if height < 3 or width < 8: return
        self.text(y, x, "┌" + "─" * (width - 2) + "┐", "border")
        self.text(y, x + 2, " " + title + " ", "label", width - 4)
        for row in range(y + 1, min(self.height, y + height - 1)):
            self.text(row, x, "│", "border"); self.text(row, x + width - 1, "│", "border")
        self.text(y + height - 1, x, "└" + "─" * (width - 2) + "┘", "border")

    def kv(self, y, x, width, label, value, role="value"):
        value = "N/A" if value is None else str(value)
        if value in ("N/A", "UNKNOWN", "NOT REPORTED"): role = "muted"
        max_label = max(8, int(width * .48))
        self.text(y, x, label, "muted", max_label)
        start = x + max_label + 1
        self.text(y, start, value, role, max(0, width - max_label - 1))

    def rows(self, y, x, height, width, title, rows):
        self.box(y, x, height, width, title)
        for i, row in enumerate(rows[:max(0, height - 2)]):
            label, value, *role = row
            self.kv(y + i + 1, x + 2, width - 4, label, value, role[0] if role else "value")

    def meter(self, y, x, width, label, value, maximum, suffix):
        self.text(y, x, label, "muted", 12)
        if value is None or not maximum:
            self.text(y, x + 13, "N/A", "muted"); return
        cells = max(4, min(20, width - 27)); ratio = min(1, max(0, value / maximum))
        filled = int(round(cells * ratio))
        self.text(y, x + 13, "█" * filled, "value")
        self.text(y, x + 13 + filled, "░" * (cells - filled), "border")
        self.text(y, x + 14 + cells, suffix, "text", width - 14 - cells)

    def lines(self): return ["".join(row) for row in self.chars]


def build_matrix_frame(height, width, model, gpu, child_status, tab, log_filter="ALL", frame_class=MatrixFrame):
    frame = frame_class(height, width)
    if height < 8 or width < 30:
        frame.text(0, 0, "NEXUS | " + model.native_status(), "value")
        frame.text(2, 0, "Resize terminal · Q closes UI", "muted")
        return frame
    if height < 18 or width < 52:
        frame.text(0, 1, "NEXUS / " + model.native_status(), "value")
        frame.text(1, 1, "Page " + str(tab + 1) + " / 8 · compact view", "muted")
        snap = model.snapshot()
        small_gpu = gpu_fields(gpu)
        rows = [("GPU power", f"{small_gpu['power']:.1f} W" if small_gpu else "N/A"), ("PoM hashrate", format_hashrate(pom_hashrate(model, "10s"))), ("Node synced", model.synced), ("PoM Mstep/s", number(snap['pom_msteps_s'])), ("Uptime", uptime_text(snap['uptime'])),
                ("DAG / confirmed seen", f"{model.stats['chain_observed']} / {model.stats['chain_confirmed']}")]
        for i, (key, value) in enumerate(rows[:max(0, height - 5)]): frame.kv(3 + i, 1, width - 3, key, value)
        frame.text(height - 1, 1, "1–8 pages · Q close · resize for detail", "muted", width - 3)
        return frame
    fresh = model.native_status() == "LIVE"
    native = model.native or {}; counters = native.get("counters", {}); state = native.get("state", {})
    rates = native.get("rates", {}); latency = native.get("latency_ms", {}); g = gpu_fields(gpu)
    def rate(key, window="60s"):
        return rates.get(key + "_" + window) if fresh and rates.get("valid_" + window) else None
    def eligible(window):
        v = native.get("eligible_rates", {}).get(window)
        return v if fresh and rates.get("valid_" + window) and isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v) and v >= 0 else None
    chosen = "60s" if rates.get("valid_60s") else "10s" if rates.get("valid_10s") else "1s"
    hashrate = format_hashrate(pom_hashrate(model, chosen))
    def ms(key):
        value = latency.get(key) if latency.get(key) is not None else model.last_measured_ms.get(key)
        return number(value, 2) + " ms" if fresh and value is not None else "N/A"
    def seen(value): return "N/A" if value is None else str(value)
    work_stale = rates.get("stale_work_fraction") if fresh and rates.get("valid_stale_work_fraction") else None
    sync = "SYNCED" if model.synced == "true" and fresh else "NOT SYNCED" if model.synced == "false" else "N/A"
    mining = state.get("mining", model.status) if fresh else "STALE / LAST " + state.get("mining", model.status)
    age = max(0, (time.monotonic_ns() - native.get("monotonic_ns", time.monotonic_ns())) / 1e9) if native else None
    frame.text(0, 2, "KERYX  /  N E X U S", "title")
    version_x = max(28, width - 30)
    frame.text(0, version_x, f"v{RELEASE_VERSION}  ·  MATRIX CONSOLE", "label", 27)
    memory = host_memory(model)
    if width >= 100:
        info = (f"RAM {memory['MemTotal']-memory['MemAvailable']:.1f}/{memory['MemTotal']:.1f}G" if memory else 'RAM N/A') + '  ·  '
        frame.text(0, max(24, version_x - len(info)), info, 'label', version_x - 24)
    subtitle = "SYNTHETIC FIXTURE / NOT LIVE MINING" if model.fixture_mode else f"{model.network.upper()}  /  {display_devices(model.gpu_devices)[0]['name']}  /  READ ONLY  /  SESSION {model.session}"
    frame.text(1, 2, subtitle, "warning" if model.fixture_mode else "muted", width - 5)
    pages = ["OVERVIEW", "GPU", "AI SERVICE", "ESCROW", "LOG", "WALLET", "DEVELOPMENT", "SETUP"]
    x = 2
    for index, page in enumerate(pages):
        name = f" {index + 1} {page} " if width >= 120 else f" {index + 1} "
        frame.text(2, x, name, "selected" if index == tab else "nav", len(name)); x += len(name) + 1
    if width < 120: frame.text(2, x + 1, pages[tab], "value", width - x - 2)
    frame.text(3, 1, "─" * (width - 3), "border")
    feed = model.native_status()
    frame.text(4, 2, "● " + feed, "value" if fresh else "warning")
    frame.text(4, 17, f"NODE {sync}", "value" if sync == "SYNCED" else "warning" if sync == "NOT SYNCED" else "muted", 23)
    frame.text(4, 42, "AGE " + (f"{age:.1f}s" if age is not None else "N/A"), "muted", 18)
    frame.text(4, max(60, width - 26), "UP " + uptime_text(model.snapshot()["uptime"]), "text", 23)
    mining_rows = [("Mining", mining), ("Network / node", model.network.upper() + " / " + sync),
                   ("DAA / job", model.daa + " / " + model.last_job),
                   ("GPU activity", state.get("gpu_activity", "N/A")),
                   ("Completed / eligible", number(counters.get("gpu_work_units_completed"), 0) + " / " + number(counters.get("eligible_attempts"), 0)),
                   ("Kernels / s (" + chosen + ")", number(rate("kernels_s", chosen))),
                   ("Full proofs / Rust", f"{seen(counters.get('proofs_completed'))} / {seen(counters.get('proofs_rust_verified'))}"),
                   ("Job invalidations", number(native.get("scheduler", {}).get("job_invalidations"), 0)),
                   ("Gather · last witness", ms("gguf_gather_wall")), ("Merkle · last witness", ms("merkle_wall")),
                   ("Rust · last measured", ms("rust_verify")), ("Template wait", ms("template_wait"))]
    escrow_home_rows = [
        ("Escrow claimed · ACK", krx(model.claimed.get('payout_sompi'), italian=True)),
        ("Mature escrow · gross", krx(model.escrow_available.get('amount_sompi'), italian=True)),
        ("Mature outputs", seen(model.escrow_available.get('outputs')) + " · " + str(model.escrow_available.get('status', 'N/A')))]
    block_rows = [("Validated targets", number(counters.get("targets_satisfied"), 0)),
                  ("Submit attempts", str(model.stats["submit_sent"])), ("RPC acknowledged", str(model.stats["rpc_ack"])),
                  ("DAG observed", str(model.stats["chain_observed"])), ("Chain confirmed · observed", str(model.stats["chain_confirmed"])),
                  ("RPC rejected", str(model.stats["rpc_rejected"]), "error" if model.stats["rpc_rejected"] else "value")]
    def activity(y, height, selected, title="LATEST ACTIVITY"):
        frame.box(y, 1, height, width - 3, title)
        if not selected:
            frame.text(y + 1, 3, "Waiting for reported events", "muted", width - 7); return
        for i, (stamp, event, detail) in enumerate(selected[-max(1, height - 2):]):
            row = y + 1 + i
            frame.text(row, 3, stamp, "muted", 8)
            category = "BLOCK" if any(v in event for v in ("SUBMIT", "CHAIN")) else "STALE" if "STALE" in event else "AI" if "AI_" in event else "PoM"
            frame.text(row, 13, category, "warning" if category == "STALE" else "label", 7)
            frame.text(row, 21, detail, "error" if any(v in event for v in ("REJECTED", "FAIL", "ERROR")) else "text", width - 25)
    if tab == 0 and width >= 100 and height >= 30:
        cards = [
            ("ELIGIBLE NONCES / s", number(eligible(chosen)), f"eligible work · {chosen}"),
            ("PoM Mstep / s", number(rate("pom_msteps_s", chosen)), "completed PoM steps"),
            ("STALE COMPLETED WORK", f"{work_stale:.1%}" if work_stale is not None else "N/A", "discarded completed work"),
        ]
        if width >= 120: cards.insert(0, ("PoM HASHRATE", hashrate, f"final PoM hashes · {chosen}"))
        power_card = ("GPU POWER", f"{g['power']:.1f} W" if g else "N/A", display_devices(model.gpu_devices)[0]['name']+' · live')
        cards.insert(1 if width >= 120 else 2, power_card)
        card_y, card_h = 6, 6; cw = (width - 2 - len(cards)) // len(cards)
        for i, (title, value, note) in enumerate(cards):
            x = 1 + i * (cw + 1); w = cw if i < len(cards) - 1 else width - x - 2
            if w < 27:
                title = {"ELIGIBLE NONCES / s": "ELIGIBLE / s", "STALE COMPLETED WORK": "STALE WORK"}.get(title, title)
            frame.box(card_y, x, card_h, w, title); frame.text(card_y + 2, x + 3, value, "hero", w - 6)
            frame.text(card_y + 4, x + 2, note, "muted", w - 4)
        y = 13; available = height - y - 3; event_h = min(9, max(4, available // 3)); body_h = available - event_h - 1
        left_w = (width - 4) * 58 // 100; right_x = left_w + 2; right_w = width - right_x - 2
        frame.rows(y, 1, body_h, left_w, "NETWORK / MINING ENGINE", ([("PoM hashrate", hashrate)] if width < 120 else []) + mining_rows[:8] + escrow_home_rows)
        block_h = min(9, max(8, body_h - 6)); frame.rows(y, right_x, block_h, right_w, "BLOCK PIPELINE · THIS SESSION", block_rows)
        gy = y + block_h; gh = body_h - block_h
        if gh >= 4:
            frame.box(gy, right_x, gh, right_w, "GPU / AI SERVICE")
            frame.meter(gy + 1, right_x + 2, right_w - 4, "GPU load", g.get("util"), 100, f"{g.get('util', 0):.0f}%")
            if gh >= 5: frame.meter(gy + 2, right_x + 2, right_w - 4, "VRAM", g.get("used"), g.get("total"), f"{g.get('used', 0)/1024:.2f}G")
            if gh >= 5: frame.kv(gy + 3, right_x + 2, right_w - 4, "Temp / power", f"{g['temp']:.0f}°C / {g['power']:.0f}W" if g else "N/A")
            if gh >= 6: frame.kv(gy + 4, right_x + 2, right_w - 4, "AI / wallet", f"{model.ai_service} / {model.wallet.get('authorization', 'UNKNOWN')}")
        activity(y + body_h + 1, event_h, list(model.milestones))
    elif tab == 0:
        rows = [("PoM hashrate", hashrate), ("GPU power", f"{g['power']:.1f} W" if g else "N/A"), ("Eligible nonces/s", number(eligible(chosen))), ("PoM Mstep/s", number(rate("pom_msteps_s", chosen))),
                ("Stale work", f"{work_stale:.1%}" if work_stale is not None else "N/A")] + escrow_home_rows + mining_rows[:3] + block_rows
        frame.rows(6, 1, height - 9, width - 3, "OVERVIEW / COMPACT", rows)
    elif tab == 1:
        backend = model.pom_backend.get('backend') if fresh else None
        backend_label = {'cuda-driver': 'CUDA DRIVER API', 'vulkan-jit': 'JIT / VULKAN', 'vulkan-bda': 'NATIVE VULKAN (BDA)'}.get(backend, 'BACKEND NOT REPORTED')
        panel_h = height - 9; frame.box(6, 1, panel_h, width - 3, display_devices(model.gpu_devices)[0]['name']+' / '+backend_label)
        frame.meter(8, 3, width - 7, "GPU load", g.get("util"), 100, f"{g.get('util', 0):.0f}%")
        frame.meter(10, 3, width - 7, "VRAM", g.get("used"), g.get("total"), f"{g.get('used', 0):.0f} / {g.get('total', 0):.0f} MiB")
        rows = [("Temperature", f"{g['temp']:.0f} °C" if g else "N/A"), ("Power", f"{g['power']:.1f} W" if g else "N/A"),
                ("Graphics clock", f"{g['clock']:.0f} MHz" if g else "N/A"), ("Kernel/s · " + chosen, number(rate("kernels_s", chosen))),
                ("Kernel · latest", ms("cuda_kernel")), ("CUDA H2D / D2H", ms("cuda_h2d") + " / " + ms("cuda_d2h") if latency.get('cuda_h2d') is not None or latency.get('cuda_d2h') is not None else "Resident search · copy times not reported"),
                ("GGUF gather wall / CPU", ms("gguf_gather_wall") + " / " + ms("gguf_gather_cpu")),
                ("Merkle wall / CPU", ms("merkle_wall") + " / " + ms("merkle_cpu")), ("Proof build · latest", ms("proof_build")), ("Rust · last measured", ms("rust_verify")),
                ("Last measured phase", _largest_reported_phase(latency, fresh)), ("PoM pause", state.get("pause_reason") or "NONE"),
                ("PoM hashrate · " + chosen, hashrate), ("Hashrate unit", "Final PoM hashes · including stale")]
        if backend in ('vulkan-jit', 'vulkan-bda'):
            unsupported = {'Kernel · latest', 'CUDA H2D / D2H', 'GGUF gather wall / CPU',
                           'Merkle wall / CPU', 'Proof build · latest', 'Rust · last measured', 'Last measured phase'}
            rows = [(name if name != 'CUDA H2D / D2H' else 'Transfers',
                     'N/A · Vulkan phase timing not published' if name in unsupported else value)
                    for name, value in rows]
        from nexus_gpu_devices import rows as device_rows
        rows += device_rows(model.gpu_devices)
        for i, row in enumerate(rows[:max(0, panel_h - 7)]):
            frame.kv(12 + i, 3, width - 7, row[0], row[1], row[2] if len(row) > 2 else 'value')
    elif tab == 2:
        m = model.ai_metrics
        external, api_status = explorer_view(model, "inferences")
        service_span = number(m['service_ms']) + ' ms' if m['service_ms'] is not None else 'No measured interval reported'
        if m.get('service_source') == 'NATIVE_SAMPLED':
            service_span = f"~{m['service_ms']/1000:.2f} s · ±{m['service_uncertainty_ms']/1000:.2f} s sampling"
        rows = [("Model", "Qwen3.5-9B · official GGUF"), ("Service readiness", model.ai_service), ("Inference", state.get("inference", model.inference)),
                ("Session · received / assigned", f"{seen(m['requests_received'])} / {seen(m['requests_assigned'])}"),
                ("Session · sent / confirmed", f"{seen(m['responses_sent'])} / {seen(m['responses_confirmed'])}"),
                ("Explorer · payout served", seen(external.get('served'))), ("Explorer · payout answered", seen(external.get('answered'))),
                ("Explorer API · refresh 30s", api_status),
                ("Pending · reported", seen(m['pending'])), ("Last inference · observed span", service_span),
                ("Deadline", "No pending request reported" if m['pending'] == 0 and not model._inference_start_ns else "Not published by worker"), ("PoM pause reason", state.get("pause_reason") or "NONE"), ("Last error", "No error recorded in session" if m['last_error'] == 'UNKNOWN' and model.history_complete else m['last_error'])]
        frame.rows(6, 1, height - 9, width - 3, "AI SERVICE / PRIORITY OVER PoM", rows)
    elif tab == 3:
        m = model.escrow_metrics
        external, api_status = explorer_view(model, "strikes")
        reward, reward_status = explorer_view(model, "reward")
        income = reward.get("income") if isinstance(reward.get("income"), dict) else {}
        worker_age = time.time() - (m.get('strike_observed_unix_ms') or 0) / 1000
        worker_live = m['strike_status'] == 'AVAILABLE' and 0 <= worker_age <= EXPLORER_TTL
        consecutive = m['consecutive_strikes'] if worker_live else external.get('consecutive_misses')
        total = m['total_strikes'] if worker_live else external.get('lifetime_strikes')
        suspended = m['suspended_until_daa'] if worker_live else external.get('suspended_until_daa_score')
        pending_burns = m['pending_burns'] if worker_live else external.get('pending_burns_count')
        rows = [("Escrow identity", m['identity']), ("Wallet delegation", m['delegation']),
                ("Mature escrow · gross", krx(model.escrow_available.get('amount_sompi'), italian=True)), ("Mature outputs · verified", seen(model.escrow_available.get('outputs'))), ("UTXO availability", str(model.escrow_available.get('status', 'N/A'))), ("Tracked outpoints", seen(m['escrow_pending_outpoints'])), ("Nominal pool · verified", str(model.escrow_available.get("nominal_outputs", "N/A")) + " / 87 · current CSV"), ("Inputs missing · nominal", str(max(0, 87 - model.escrow_available["nominal_outputs"])) if type(model.escrow_available.get("nominal_outputs")) is int else "N/A"), ("Nominal claim batch", "87 inputs · same CSV window"), ("Claim selection", "Stock rules · maturity alone is insufficient"),
                ("Strikes consecutive / total", f"{seen(consecutive)} / {seen(total)}"),
                ("Strike source", "LOCAL NODE RPC" if worker_live else "OFFICIAL EXPLORER · " + api_status),
                ("Explorer strike coverage · DAA", seen(external.get('virtual_daa_score'))),
                ("Suspended until DAA", "NONE" if (worker_live or api_status.startswith('LIVE')) and suspended is None else seen(suspended)),
                ("Pending burns · count", seen(pending_burns)), ("Claim transactions", "DISABLED" if model.claims_disabled is True else "ENABLED · stock rules · check 30 min" if model.claims_disabled is False else "N/A"),
                ("Escrow claimed · NEXUS ACK", seen(model.claimed['outputs']) + " outputs"),
                ("Claimed payout · net", krx(model.claimed['payout_sompi'], italian=True)),
                ("Claim ACK transactions", seen(model.claimed['transactions'])),
                ("Claim session", model.claimed['status']),
                ("Claim finality", "RPC ACK · not chain confirmation"),
                ("Escrow earned · income window", krx(income.get('escrow_sompi'))),
                ("Income coverage · DAA", seen(income.get('income_window_daa'))), ("Income source", "EXPLORER · " + reward_status),
                ("Node strike poll", m['strike_status'])]
        frame.rows(6, 1, height - 9, width - 3, "ESCROW / STRIKE MONITOR", rows)
    elif tab == 4:
        selected = [item for item in model.events if event_matches(item[1], log_filter)]
        count = max(1, height - 12); model.log_scroll = min(model.log_scroll, max(0, len(selected) - count))
        end = len(selected) - model.log_scroll; activity(6, height - 9, selected[max(0, end - count):end], "EVENT LOG / " + log_filter + (" / PAUSED SCROLL" if model.log_scroll else " / LIVE"))
    elif tab == 6:
        api = model.development
        age = time.time() - api.get('unix', 0)
        active = api.get('server_unit') == 'active' and api.get('status') == 'ACTIVE' and 0 <= age <= 6
        key_states = {k['name']: k for k in api.get('keys', []) if isinstance(k, dict) and 'name' in k}
        def key_text(name):
            k = key_states.get(name)
            if not k: return 'N/A'
            return ('ACTIVE' if k.get('active') else 'DISABLED') + ' · ' + str(k.get('fingerprint', 'N/A'))
        job = api.get('latest_job') or {}
        rows = [
                ("MCP server · M toggle", 'ACTIVE · AUTHENTICATED CONTROL' if api.get('mcp', {}).get('server_unit') == 'active' else 'OFF'),
                ("MCP endpoint · E copy", 'http://127.0.0.1:3341/mcp'),
                ("MCP persistence", 'ON · restored with Nexus' if api.get('mcp', {}).get('enabled') else 'OFF'),
                ("MCP requests / denied", f"{seen(api.get('mcp', {}).get('requests'))} / {seen(api.get('mcp', {}).get('denied'))}"),
                ("MCP in-flight requests", seen(api.get('mcp', {}).get('in_flight'))),
                ("Agent API", 'ACTIVE · LOCAL ONLY' if active else 'OFF / UNAVAILABLE'),
                ("Endpoint", api.get('endpoint', 'http://127.0.0.1:3340')),
                ("Authentication", 'Bearer · single key'),
                ("API/MSP key · C copy", key_text('agent')),
                ("Permissions", 'DIAGNOSTICS · DEVELOPMENT · LIVE'),
                ("Server control · T", 'ON' if active else 'OFF' if api.get('server_unit') in ('inactive', 'failed') else 'STALE'), ("Requests / denied", f"{seen(api.get('requests'))} / {seen(api.get('denied'))}"),
                ("Diagnostic feed", model.native_status()),
                ("Development proposals", seen(api.get('proposals'))),
                ("Development mode", 'ISOLATED WORKTREE · no automatic deployment'),
                ("Live commands", 'BUSY · safety gate' if api.get('command_busy') else 'READY' if active else 'N/A'),
                ("Latest command", job.get('command', 'NONE')), ("Command result", job.get('status', 'N/A')),
                ("Result detail", job.get('detail', 'N/A')),
                ("Mining / inference", str(state.get('mining', 'N/A')) + ' / ' + str(state.get('inference', 'N/A'))),
                ("Guardrails", 'AI audit · pinned worker · no duplicate miner'),
                ("Allowed actions", 'diagnostics · dashboard · worker start/stop/restart'),
]
        frame.rows(6, 1, height - 12, width - 3, "DEVELOPMENT / AI AGENT API", rows)
        frame.text(height - 6, 3, model.development_notice or 'Copy the key and paste it into the agent API page', 'value', width - 7)
        frame.text(height - 4, 3, 'M MCP SERVER · E COPY ENDPOINT · G GENERATE KEY · C COPY KEY · T API SERVER', 'title', width - 7)
    elif tab == 7:
        setup = model.setup; update = model.updates
        status = update.get('status', 'NOT_CHECKED')
        if status in ('UP_TO_DATE', 'LOCAL_AHEAD'):
            version_status, role = 'UP TO DATE', 'value'
        elif status in ('UPDATE_AVAILABLE', 'STAGED', 'WAITING'):
            version_status, role = 'UPDATE AVAILABLE', 'warning'
        elif status == 'CHECKING': version_status, role = 'CHECKING', 'muted'
        else: version_status, role = 'CHECK REQUIRED' if status != 'ERROR' else 'CHECK FAILED', 'warning'
        rows = [('Version', version_status, role),
                ('Installed version', 'NEXUS v' + RELEASE_VERSION),
                ('Automatic updates · U', 'ON' if update.get('enabled', True) else 'OFF'),
                ('Latest GitHub commit', str(update.get('remote_commit') or 'N/A')[:12]),
                ('Update detail', update.get('detail', 'Press R to check the latest private release')),
                ('1 · Mining payout · P', setup.get('payout') or model.mining_payout or 'NOT CONFIGURED'),
                ('2 · Escrow key · K import', setup.get('key_path') or 'NOT CONFIGURED · N creates a first identity'),
                ('Escrow public key · C copy', setup.get('pubkey') or model.wallet.get('escrow_public_key') or 'N/A'),
                ('3 · Wallet certificate · I', setup.get('cert_path') or ('EXISTING DELEGATION' if setup.get('legacy_certificate') else 'NOT CONFIGURED')),
                ('Wallet authorization', model.wallet.get('authorization', 'UNKNOWN')),
                ('Configuration', 'SAVED PROFILE · checked at launch' if setup.get('profile_active') else 'EXISTING IDENTITY / DRAFT'),
                ('Activation · V', 'Verify and save · no automatic restart'),
                ('Wallet signing', 'Keryx Wallet → Authorise a miner → import signed certificate'),
                ('Model / H6 anchor', 'VERIFIED · active worker' if fresh and state.get('mining') == 'ACTIVE' else 'Checked at miner startup · not a wallet key'),
                ('Node endpoint', 'grpc://127.0.0.1:22110 · mainnet'),
                ('GPU', display_devices(model.gpu_devices)[0]['name']+' · configured UUID'),
                ('AMD secondary AMD GPU', 'OFFLINE DIAGNOSTICS ONLY · no production admission', 'warning'),
                ('AMD device mode', next((d['mode'].upper() for d in model.gpu_devices.get('amd_devices', [])), 'NOT DETECTED')),
                ('AMD capabilities', next((d['capabilities_status'] for d in model.gpu_devices.get('amd_devices', [])), 'UNAVAILABLE')),
                ('Private storage', 'state/setup · outside Git · existing keys preserved')]
        frame.rows(6, 1, height - 12, width - 3, 'SETUP / FIRST RUN & UPDATES', rows)
        frame.text(height - 6, 3, model.setup_notice or 'P payout → K import key / N first key → C copy public key → I certificate → V verify', 'value', width - 7)
        frame.text(height - 4, 3, 'P PAYOUT · K KEY FILE · N FIRST KEY · C COPY PUBLIC KEY · I CERTIFICATE · V VERIFY', 'title', width - 7)
        frame.text(height - 3, 3, 'R CHECK UPDATES · U AUTO UPDATES · A AMD REFRESH · D AMD DIAGNOSTICS', 'title', width - 7)
    else:
        w = model.wallet; payout = model.mining_payout or 'Waiting for active worker payout'
        frame.box(6, 1, height - 9, width - 3, "WALLET / AUTOMATIC MINING PAYOUT")
        rows = [("Authorization · local", w.get('authorization', 'UNKNOWN')), ("Certificate verification", w.get('verification', 'UNKNOWN')),
                ("Payout source", "ACTIVE WORKER · AUTOMATIC" if model.mining_payout and fresh else "Worker telemetry unavailable"), ("Certificate source", w.get('certificate_source') or 'N/A')]
        for i, (key, value) in enumerate(rows): frame.kv(8 + i, 3, width - 7, key, value)
        frame.text(13, 3, "MINING PAYOUT · WALLET API ADDRESS", "label", width - 7)
        payout_room = max(16, width - 9)
        for i in range(0, len(payout), payout_room): frame.text(14 + i // payout_room, 3, payout[i:i+payout_room], "value", payout_room)
        base = 16 + (len(payout) - 1) // payout_room
        balance, balance_status = explorer_view(model, "balance")
        reward, reward_status = explorer_view(model, "reward")
        income = reward.get("income") if isinstance(reward.get("income"), dict) else {}
        bracket = reward.get('bracket_bps')
        bracket_text = f"{bracket / 100:.2f}%" if type(bracket) is int and 0 <= bracket <= 10000 else "N/A"
        paid_label = 'Mining paid · ~24h indexed' if income.get('income_window_daa') == 864000 else 'Mining paid · covered window'
        public_rows = [("Wallet balance", krx(balance.get('balance_sompi'), italian=True)), ("Wallet API · refresh 30s", balance_status),
                       ("Holder bracket", bracket_text), (paid_label, krx(income.get('paid_sompi'), italian=True)),
                       ("Inference income · window", krx(income.get('inference_sompi'))),
                       ("Selected blocks · income window", seen(income.get('chain_blocks'))),
                       ("Income coverage · DAA", seen(income.get('income_window_daa'))),
                       ("Reward snapshot · DAA", seen(reward.get('tip_daa'))),
                       ("Income API · HTTP freshness", reward_status)]
        for i, (key, value) in enumerate(public_rows):
            if base + i < height - 5: frame.kv(base + i, 3, width - 7, key, value)
        message = w.get('ui_error') or w.get('notice') or w.get('error')
        if message: frame.text(height - 5, 3, message, "error" if w.get('ui_error') or w.get('error') else "value", width - 7)
    controls = "1–8 / Tab / ← → pages · 0 all  A ai  S submit  J job  E errors" if width >= 80 else "1–8 / Tab pages · 0/A/S/J/E filters"
    frame.text(height - 2, 2, controls, "nav", width - 5)
    tail = "Q closes dashboard · miner stays active" if child_status == "READ ONLY" else "Q hides dashboard · worker continues"
    if tab == 4: tail += "  |  ↑ ↓ / PgUp PgDn scroll · Home live"
    if tab == 7: tail += "  |  R check updates · V verify setup"

    frame.text(height - 1, 2, tail, "muted", width - 5)
    return frame


def render(stdscr, model: DashboardModel, tab: int, gpu: str, child_status: str, log_filter="ALL"):
    try:
        curses.curs_set(0)
        if not getattr(model, "_palette_ready", False) and curses.has_colors():
            curses.start_color(); curses.use_default_colors()
            palette = [(MATRIX_BRIGHT, 46, curses.COLOR_GREEN), (MATRIX_DARK, 28, curses.COLOR_GREEN),
                       (MATRIX_TEXT, 151, curses.COLOR_WHITE), (MATRIX_WARN, 220, curses.COLOR_YELLOW),
                       (MATRIX_ERROR, 196, curses.COLOR_RED), (7, 244, curses.COLOR_WHITE)]
            for pair, rich, fallback in palette: curses.init_pair(pair, rich if curses.COLORS >= 256 else fallback, 16 if curses.COLORS >= 256 else 0)
            curses.init_pair(8, 16 if curses.COLORS >= 256 else 0, 46 if curses.COLORS >= 256 else curses.COLOR_GREEN)
            stdscr.bkgd(" ", curses.color_pair(MATRIX_TEXT)); model._palette_ready = True
    except (curses.error, AttributeError): pass
    def color(pair):
        try: return curses.color_pair(pair)
        except curses.error: return 0
    attrs = {"text": color(MATRIX_TEXT), "muted": color(7), "border": color(MATRIX_DARK), "label": color(MATRIX_DARK) | curses.A_BOLD,
             "value": color(MATRIX_BRIGHT) | curses.A_BOLD, "hero": color(MATRIX_BRIGHT) | curses.A_BOLD,
             "title": color(MATRIX_BRIGHT) | curses.A_BOLD, "warning": color(MATRIX_WARN), "error": color(MATRIX_ERROR),
             "nav": color(MATRIX_DARK), "selected": color(8) | curses.A_BOLD}
    height, width = stdscr.getmaxyx(); canvas = build_matrix_frame(height, width, model, gpu, child_status, tab, log_filter)
    lines = canvas.lines(); old = model._render_cache
    changed_size = old is None or old[0] != (height, width) or getattr(model, '_render_tab', None) != tab
    model._render_tab = tab
    if changed_size:
        try: stdscr.erase()
        except curses.error: pass
    for y, line in enumerate(lines):
        roles = canvas.roles[y]
        if not changed_size and old[1][y] == line and old[2][y] == roles: continue
        if changed_size:
            try: stdscr.addnstr(y, 0, line, canvas.width, attrs["text"])
            except curses.error: pass
        x = 0
        while x < canvas.width:
            if not changed_size and old[1][y][x] == line[x] and old[2][y][x] == roles[x]: x += 1; continue
            start, role = x, roles[x]; x += 1
            while x < canvas.width and roles[x] == role and (changed_size or old[1][y][x] != line[x] or old[2][y][x] != roles[x]): x += 1
            try: stdscr.addnstr(y, start, line[start:x], x - start, attrs[role])
            except curses.error: pass
    model._render_cache = ((height, width), lines, canvas.roles)
    try: stdscr.noutrefresh(); curses.doupdate()
    except (curses.error, AttributeError):
        try: stdscr.refresh()
        except curses.error: pass

def worker_environment(environment):
    """Preserve the admitted route; never re-enable CUDA in a Vulkan-only worker."""
    env = dict(environment)
    backend = env.get("NEXUS_POM_BACKEND", "cuda-driver")
    if backend not in ("cuda-driver", "vulkan-jit", "vulkan-bda"):
        raise ValueError("Unknown worker backend; refusing implicit CUDA fallback")
    env["CUDA_DEVICE_ORDER"] = "PCI_BUS_ID"
    if backend in ("vulkan-jit", "vulkan-bda"):
        env["CUDA_VISIBLE_DEVICES"] = ""
        env.pop("NEXUS_CUDA_DEVICE_UUID", None)
    else:
        env["CUDA_VISIBLE_DEVICES"] = AUTHORIZED_GPU
        env["NEXUS_CUDA_DEVICE_UUID"] = AUTHORIZED_GPU
    return env


def run(args) -> int:
    root = Path(args.root).resolve()
    root.mkdir(parents=True, exist_ok=True)
    log_path = Path(args.log).resolve()
    telemetry_path = Path(args.telemetry).resolve()
    for path in (log_path, telemetry_path):
        path.parent.mkdir(parents=True, exist_ok=True)
        if str(path).startswith("/tmp/") or path == Path("/tmp"):
            raise SystemExit("temporary files must stay in persistent workspace")
    env = worker_environment(os.environ)
    p = subprocess.Popen(args.command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, bufsize=1, env=env)
    lines: queue.Queue[str | None] = queue.Queue()
    reader_done = threading.Event()
    model = DashboardModel()
    from nexus_escrow_availability import EscrowAvailabilityPoller
    availability_poller = EscrowAvailabilityPoller()

    from nexus_updates import EmbeddedUpdater
    updater = EmbeddedUpdater()
    production_runtime = (getattr(args, "no_tui", False) and args.command and Path(args.command[0]).name == "nexus-live-cuda-worker"
                          and "nexus-v002-production.service" in Path("/proc/self/cgroup").read_text())
    if production_runtime:
        updater.start()

    def reader():
        assert p.stdout is not None
        try:
            for line in p.stdout:
                lines.put(line)
        finally:
            p.stdout.close()
            reader_done.set()

    threading.Thread(target=reader, name="nexus-log-reader", daemon=True).start()
    gpu_value = "UNKNOWN"
    gpu_stop = threading.Event()

    def poll_gpu():
        nonlocal gpu_value
        while not gpu_stop.wait(2.0): gpu_value = gpu_sample()

    threading.Thread(target=poll_gpu, name="nexus-gpu-telemetry", daemon=True).start()
    interactive = not args.no_tui and sys.stdout.isatty() and sys.stdin.isatty() and os.environ.get("TERM", "") not in ("", "dumb")
    child_status = "RUNNING"
    shutdown_requested = False

    def handle_signal(_signum, _frame):
        nonlocal shutdown_requested
        shutdown_requested = True
        if p.poll() is None:
            try: p.send_signal(signal.SIGINT)
            except ProcessLookupError: pass

    old_sigint = signal.signal(signal.SIGINT, handle_signal)
    old_sigterm = signal.signal(signal.SIGTERM, handle_signal)
    try:
        with open(log_path, "a", encoding="utf-8", buffering=1) as raw, open(telemetry_path, "a", encoding="utf-8", buffering=1) as telemetry:
            def pump_many(limit=1000):
                model.escrow_available = availability_poller.pump()
                refresh_development_status(model, root)
                processed = 0
                while processed < limit:
                    try: line = lines.get(timeout=0.02 if processed == 0 else 0)
                    except queue.Empty: break
                    if line is None: break
                    raw.write(line)
                    if not interactive:
                        sys.stdout.write(line)
                        sys.stdout.flush()
                    event = model.ingest(line)
                    telemetry.write(json.dumps({"observed_monotonic_ns": time.monotonic_ns(), "worker": event}, separators=(",", ":")) + "\n")
                    processed += 1

            if interactive:
                def ui(stdscr):
                    nonlocal child_status, interactive
                    stdscr.nodelay(True)
                    tab = 0
                    log_filter = "ALL"
                    while p.poll() is None or not reader_done.is_set() or not lines.empty():
                        pump_many()
                        key = stdscr.getch()
                        if key in (ord("q"), ord("Q")):
                            interactive = False
                            break
                        tab = select_page(key, tab)
                        if key in (curses.KEY_F1, curses.KEY_F2, curses.KEY_F3, curses.KEY_F4, curses.KEY_F5, curses.KEY_F6, curses.KEY_F7, curses.KEY_F8): tab = key - curses.KEY_F1
                        if key in (ord("0"), ord("a"), ord("s"), ord("j"), ord("e")):
                            log_filter = {ord("0"): "ALL", ord("a"): "AI", ord("s"): "SUBMIT", ord("j"): "JOB", ord("e"): "ERROR"}[key]
                        if tab == 6: development_keypress(model, root, key)
                        if tab == 7: setup_keypress(stdscr, model, root, key)
                        render(stdscr, model, tab, gpu_value, child_status, log_filter)
                        if shutdown_requested: break
                        time.sleep(0.15)
                try:
                    set_matrix_background(True)
                    curses.wrapper(ui)
                except curses.error:
                    # Terminal resize/teardown must not take down the miner.
                    interactive = False
                finally:
                    set_matrix_background(False)
            while p.poll() is None or not reader_done.is_set() or not lines.empty():
                pump_many()
                if interactive:
                    # q leaves the read-only dashboard but keeps draining/logging the worker.
                    time.sleep(0.02)
            child_status = f"EXIT_{p.returncode}"
    finally:
        updater.close()
        gpu_stop.set()
        signal.signal(signal.SIGINT, old_sigint)
        signal.signal(signal.SIGTERM, old_sigterm)
    if shutdown_requested and p.poll() is None:
        p.send_signal(signal.SIGINT)
        p.wait()
    if production_runtime:
        subprocess.run(['systemctl','--user','stop','--no-block','nexus-tray.service',
                        'nexus-matrix-dashboard.service','nexus-matrix-explorer-readonly.service',
                        'nexus-agent-api.service','nexus-mcp.service','nexus-first-block-alerts.service'],
                       stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=3)
    return int(p.returncode or 0)


def setup_keypress(stdscr, model, root, key):
    if key in (ord('a'), ord('A'), ord('d'), ord('D')):
        if model._setup_check_thread and model._setup_check_thread.is_alive():
            model.setup_notice = 'A setup operation is already running'
            return
        def configure_gpu():
            try:
                from nexus_gpu_devices import refresh_capabilities, configure_mode, status
                if key in (ord('a'), ord('A')):
                    model.setup_notice = refresh_capabilities()
                else:
                    devices = status()['amd_devices']
                    if len(devices) != 1: raise ValueError('Select exactly one secondary AMD GPU by PCI id using the GPU configuration CLI')
                    model.setup_notice = configure_mode('disabled' if devices[0]['mode'] == 'diagnostics' else 'diagnostics', devices[0]['id'])
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                model.setup_notice = str(error) if isinstance(error, ValueError) else 'AMD configuration unavailable; production unchanged'
            model._setup_poll = 0
        model._setup_check_thread = threading.Thread(target=configure_gpu, daemon=True, name='nexus-amd-setup')
        model.setup_notice = 'Updating AMD offline configuration · mining continues'
        model._setup_check_thread.start()
        return
    if key in (ord('r'), ord('R')):
        if model._setup_check_thread and model._setup_check_thread.is_alive():
            model.setup_notice = 'Update check already running'
            return
        def check_version():
            from nexus_updates import check
            try:
                check(activate=False, inspect_only=True)
                if model.setup_notice == 'Checking latest private GitHub version · no download or restart':
                    model.setup_notice = 'Version check complete · see status above'
            except (OSError, ValueError):
                model.setup_notice = 'Version check unavailable'
        model._setup_check_thread = threading.Thread(target=check_version, daemon=True, name='nexus-manual-version-check')
        model.setup_notice = 'Checking latest private GitHub version · no download or restart'
        model._setup_check_thread.start()
    elif key in (ord('u'), ord('U')):
        try:
            from nexus_updates import toggle
            model.setup_notice = 'Automatic updates ON' if toggle()['enabled'] else 'Automatic updates OFF'
        except (OSError, ValueError, subprocess.SubprocessError): model.setup_notice = 'Update setting unavailable'
    elif key in (ord('c'), ord('C')):
        from nexus_setup import public_view
        try:
            pubkey = public_view().get('pubkey') or model.wallet.get('escrow_public_key')
            if not isinstance(pubkey, str) or not re.fullmatch('[0-9a-f]{64}', pubkey): raise ValueError('Configure an escrow key first')
            model.setup_notice = copy_public_value(pubkey, 'escrow public key')
        except (OSError, ValueError): model.setup_notice = 'Escrow public key unavailable'
    elif key in tuple(map(ord, 'pPkKnNiIvV')):
        from nexus_setup import configure
        action = {'p':'payout','k':'key','n':'new_key','i':'certificate','v':'verify'}[chr(key).lower()]
        value = ''
        if action in ('payout', 'key', 'certificate'):
            prompt = {'payout':'Mining payout address: ', 'key':'Private escrow KEY FILE path (never paste a key): ',
                      'certificate':'Wallet-signed CERTIFICATE FILE path: '}[action]
            try:
                height, width = stdscr.getmaxyx(); stdscr.nodelay(False)
                curses.echo() if action == 'payout' else curses.noecho()
                curses.curs_set(1)
                stdscr.move(height - 2, 0); stdscr.clrtoeol()
                stdscr.addnstr(height - 2, 0, prompt, width - 2); stdscr.refresh()
                value = stdscr.getstr(height - 2, min(len(prompt), width - 2), 1024).decode('utf-8').strip()
            except (curses.error, UnicodeError): model.setup_notice = 'Input unavailable'; return
            finally:
                stdscr.nodelay(True); model._render_cache = None
                try: curses.noecho(); curses.curs_set(0)
                except curses.error: pass
            if not value: return
        try:
            model.setup_notice = configure(action, value)
            load_wallet_status(model)
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
            model.setup_notice = str(error) if isinstance(error, ValueError) else 'Setup operation rejected; existing identity preserved'
    else: return
    model._development_poll = 0
    model._setup_poll = 0
    refresh_development_status(model, root)


def development_keypress(model, root, key):
    if key in (ord('m'), ord('M'), ord('e'), ord('E')):
        try:
            from nexus_mcp_control import toggle, ENDPOINT
            if key in (ord('e'), ord('E')):
                model.development_notice = copy_public_value(ENDPOINT, 'MCP endpoint')
            else:
                model.development_notice = 'MCP server ON · shared authenticated controls' if toggle() else 'MCP server OFF · miner continues'
        except (OSError, ValueError, subprocess.SubprocessError):
            model.development_notice = 'MCP unavailable · generate the key with G or check the service'
        model._development_poll = 0
        refresh_development_status(model, root)
        return
    if key not in (ord('g'), ord('G'), ord('c'), ord('C'), ord('t'), ord('T')): return
    from nexus_agent_api import ensure_agent_key, atomic_json
    import fcntl
    path = Path(root) / 'agent-api'
    try:
        if key in (ord('g'), ord('G')):
            model.development_notice = ensure_agent_key(rotate=True)
        elif key in (ord('c'), ord('C')):
            p = path / 'client-keys.json'
            if p.is_symlink() or p.stat().st_mode & 0o077: raise ValueError('unsafe key file')
            token = json.loads(p.read_text()).get('agent')
            if not isinstance(token, str) or not re.fullmatch(r'nx_[A-Za-z0-9_-]{43}', token):
                raise ValueError('generate the API key with G first')
            verifier = json.loads((path / 'keys-hashed.json').read_text())
            import hashlib
            if len(verifier) != 1 or not verifier[0].get('active') or verifier[0].get('sha256') != hashlib.sha256(token.encode()).hexdigest():
                raise ValueError('API key inactive or mismatched; press G')
            model.development_notice = copy_public_value(token, 'API/MSP key')
            if 'unavailable' in model.development_notice: model.development_notice = 'Clipboard unavailable: check KDE Klipper'
        else:
            path.mkdir(parents=True, exist_ok=True, mode=0o700)
            fd = os.open(path / 'api-operation.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                unit = subprocess.run(['systemctl', '--user', 'is-active', 'nexus-agent-api.service'], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, timeout=2).stdout.strip()
                enable = unit != 'active'
                if enable and not (path / 'keys-hashed.json').exists(): raise ValueError('generate the API key with G first')
                subprocess.run(['systemctl', '--user', 'start' if enable else 'stop', 'nexus-agent-api.service'], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
                atomic_json(path / 'enabled.json', {'enabled': enable})
                model.development_notice = 'API server ON' if enable else 'API server OFF · miner continues'
            finally: os.close(fd)
    except BlockingIOError: model.development_notice = 'API operation in progress · try again after completion'
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        model.development_notice = str(exc) if isinstance(exc, ValueError) else 'API control unavailable'
    model._development_poll = 0
    refresh_development_status(model, root)


def refresh_development_status(model, root):
    now = time.monotonic()
    if now < model._development_poll: return
    model._development_poll = now + 2
    update_path = Path(root) / 'state/updates'
    model.updates = {'status': 'NOT_CHECKED'}
    try:
        status_file = update_path / 'status.json'
        if status_file.stat().st_size > 65536: raise ValueError('oversized update status')
        value = json.loads(status_file.read_text())
        if isinstance(value, dict): model.updates = value
    except (OSError, ValueError): pass
    try:
        enabled = json.loads((update_path / 'config.json').read_text())['enabled']
        if type(enabled) is not bool: raise ValueError('invalid update setting')
        model.updates['enabled'] = enabled
    except FileNotFoundError: model.updates['enabled'] = True
    except (OSError, ValueError, KeyError, TypeError):
        model.updates.update(enabled=False, status='CONFIG_ERROR', detail='Invalid setting; updates blocked')
    path = Path(root) / 'agent-api'
    obj = {}
    try:
        p = path / 'status.json'
        if p.stat().st_size > 65536: raise ValueError('oversized API status')
        obj = json.loads(p.read_text())
        if not isinstance(obj, dict) or obj.get('schema_version') != 1: obj = {}
    except (OSError, ValueError): pass
    try:
        obj['server_unit'] = subprocess.run(['systemctl', '--user', 'is-active', 'nexus-agent-api.service'], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, timeout=1).stdout.strip()
        keys = json.loads((path / 'keys-hashed.json').read_text())
        obj['keys'] = [{'name': k['name'], 'scopes': k['scopes'], 'fingerprint': k['sha256'][:12], 'active': k.get('active', True)} for k in keys]
    except (OSError, ValueError, KeyError, subprocess.SubprocessError): obj['server_unit'] = 'UNKNOWN'
    try:
        from nexus_mcp_control import status as mcp_status
        obj['mcp'] = mcp_status()
    except (OSError, ValueError, subprocess.SubprocessError):
        obj['mcp'] = {'server_unit': 'UNKNOWN'}
    model.development = obj
    if now >= model._setup_poll:
        model._setup_poll = now + 30
        try:
            from nexus_gpu_devices import status as device_status
            if not model.fixture_mode: model.gpu_devices = device_status()
        except (OSError, ValueError, RuntimeError):
            model.gpu_devices = {'error': 'GPU inventory unavailable; current worker unchanged'}
        try:
            from nexus_setup import draft, PROFILE
            value = draft()
            model.setup = {k: value.get(k) for k in ('payout', 'key_path', 'cert_path', 'pubkey', 'legacy_certificate')}
            model.setup['profile_active'] = PROFILE.exists()
        except (OSError, ValueError, RuntimeError):
            model.setup = {'error': 'Setup configuration unavailable; current worker unchanged'}


class TelemetryFollower:
    """Read a complete log session with bounded buffers, then drain bursts in batches."""
    history_markers = (b"NEXUS_TELEMETRY", b"STARTUP_STATE", b"NODE_INFO", b"NODE_NETWORK", b"SERVICE_STRIKE_POLL", b"AI_SERVICE_STATUS", b"AI_SERVICE_ERROR", b"AI_STARTUP_BACKFILL=", b"OPOI_RUNTIME=", b'"official_rust_verify"',
                       b"AI_REQUEST_", b"AI_RESPONSE_", b"LIVE_SUBMIT_RESULT", b"LIVE_CHAIN_STATUS", b"ESCROW_CLAIM_SUBMISSION",
                       b"AMD_LANE_STATE", b"AMD_CANDIDATE_VERIFIED", b"LIVE_CUDA_NONCE=PASS", b"NVIDIA_VULKAN_CANDIDATE_VERIFIED", b"POM_BACKEND_SELECTED")
    def __init__(self, path):
        self.path = Path(path).resolve()
        if self.path == Path("/tmp") or str(self.path).startswith("/tmp/"):
            raise ValueError("telemetry must be stored in the persistent workspace")
        self.source = self.path.open("rb")
        st = os.fstat(self.source.fileno()); self.identity = (st.st_dev, st.st_ino)
        self.pending = b""

    def close(self): self.source.close()

    @staticmethod
    def ingest_record(model, raw):
        try:
            record = json.loads(raw)
            if not isinstance(record, dict): return
            worker = record.get("worker", record)
            if isinstance(worker, dict):
                model.ingest(json.dumps(worker, separators=(",", ":")), record.get("observed_monotonic_ns"))
        except (ValueError, TypeError, AttributeError): pass

    def bootstrap(self, model):
        # All block and AI lifecycle records are counted, including those outside
        # the last 2 MiB. Only recent noisy events are decoded/rendered.
        end = os.fstat(self.source.fileno()).st_size
        tail_start = max(0, end - 2 * 1024 * 1024); position = 0
        while self.source.tell() < end:
            data = self.pending + self.source.read(min(512 * 1024, end - self.source.tell()))
            lines = data.split(b"\n"); self.pending = lines.pop()
            for raw in lines:
                if position < 256 * 1024 or position >= tail_start or any(marker in raw for marker in self.history_markers):
                    self.ingest_record(model, raw)
                position += len(raw) + 1
            if len(self.pending) > 1024 * 1024:
                position += len(self.pending); self.pending = b""
        model.history_complete = True; model.session = self.path.stem.replace("nexus-ui-inference-", "")
        for key in ("requests_received", "requests_assigned", "responses_sent", "responses_confirmed"):
            if model.ai_metrics[key] is None: model.ai_metrics[key] = 0

    def drain(self, model, budget=2 * 1024 * 1024):
        data = self.pending + self.source.read(budget)
        lines = data.split(b"\n"); self.pending = lines.pop()
        for raw in lines:
            if len(raw) <= 1024 * 1024: self.ingest_record(model, raw)
        if len(self.pending) > 1024 * 1024:
            self.pending = b""; model.native_health = "INVALID"; model.native_error = "oversized telemetry record"
        return len(lines)

    def replaced(self):
        try:
            st = self.path.stat()
            return (st.st_dev, st.st_ino) != self.identity or st.st_size < self.source.tell()
        except OSError: return False


class TelemetrySessionReader:
    """Shared read-only session rollover for desktop and terminal attachments."""
    def __init__(self, root, path=None, follow_latest=True):
        self.root = Path(root)
        self.path = Path(path).resolve() if path else None
        self.follow_latest = follow_latest
        self.follower = None
        self.model = DashboardModel()
        self.next_scan = 0

    def poll(self):
        now = time.monotonic()
        if now >= self.next_scan:
            candidate = self.path
            if self.follow_latest:
                paths = list((self.root / 'logs').glob('nexus-ui-inference-*.jsonl'))
                if paths:
                    candidate = max(paths, key=lambda path: path.stat().st_mtime_ns)
            if candidate and (self.follower is None or candidate != self.follower.path or self.follower.replaced()):
                follower = TelemetryFollower(candidate)
                model = DashboardModel()
                try:
                    load_wallet_status(model)
                    follower.bootstrap(model)
                except Exception:
                    follower.close()
                    raise
                if self.follower:
                    self.follower.close()
                self.follower, self.model = follower, model
            self.next_scan = now + 2
        if self.follower:
            self.follower.drain(self.model)
        return self.model

    def close(self):
        if self.follower:
            self.follower.close()
            self.follower = None


def run_read_only(args) -> int:
    """Follow saved snapshots without starting, stopping or signalling a worker."""
    telemetry_path = Path(args.telemetry).resolve() if args.telemetry else None
    if not getattr(args, 'setup', False) and (telemetry_path is None or not telemetry_path.is_file()):
        raise SystemExit('read-only telemetry file does not exist')
    reader = TelemetrySessionReader(args.root, telemetry_path, getattr(args, 'follow_latest', False))
    model = reader.model
    load_wallet_status(model)
    explorer_poller = OfficialExplorerPoller()
    from nexus_escrow_availability import EscrowAvailabilityPoller
    availability_poller = EscrowAvailabilityPoller()
    gpu_value, next_gpu_poll = "UNKNOWN", 0.0
    tab, log_filter = (7 if getattr(args, "setup", False) else 0), "ALL"
    def pump():
        nonlocal model, gpu_value, next_gpu_poll
        now = time.monotonic()
        model = reader.poll()
        model.escrow_available = availability_poller.pump()
        refresh_development_status(model, args.root)
        explorer_poller.pump(model)
        if now >= next_gpu_poll:
            gpu_value = gpu_sample(); next_gpu_poll = time.monotonic() + 2
    def keypress(stdscr, key):
        nonlocal tab, log_filter
        if key in (ord("q"), ord("Q")): return False
        previous = tab; tab = select_page(key, tab)
        if previous != tab: model.log_scroll = 0
        if key in (ord("0"), ord("a"), ord("s"), ord("j"), ord("e")):
            log_filter = {ord("0"): "ALL", ord("a"): "AI", ord("s"): "SUBMIT", ord("j"): "JOB", ord("e"): "ERROR"}[key]; model.log_scroll = 0
        if tab == 4:
            if key == curses.KEY_UP: model.log_scroll += 1
            elif key == curses.KEY_DOWN: model.log_scroll = max(0, model.log_scroll - 1)
            elif key == curses.KEY_PPAGE: model.log_scroll += max(1, stdscr.getmaxyx()[0] - 12)
            elif key == curses.KEY_NPAGE: model.log_scroll = max(0, model.log_scroll - max(1, stdscr.getmaxyx()[0] - 12))
            elif key in (curses.KEY_HOME, curses.KEY_END): model.log_scroll = 0
        if tab == 6: development_keypress(model, args.root, key)
        if tab == 7: setup_keypress(stdscr, model, args.root, key)
        return True
    try:
        interactive = (not args.no_tui and sys.stdout.isatty() and sys.stdin.isatty() and os.environ.get("TERM", "") not in ("", "dumb"))
        pump()
        if interactive:
            def ui(stdscr):
                stdscr.nodelay(True)
                try: stdscr.keypad(True)
                except AttributeError: pass
                while True:
                    pump()
                    if not keypress(stdscr, stdscr.getch()): return
                    render(stdscr, model, tab, gpu_value, "READ ONLY", log_filter)
                    time.sleep(.2)
            try:
                set_matrix_background(True); curses.wrapper(ui)
            finally: set_matrix_background(False)
        else:
            # Headless attachment is also bounded; no replay of thousands of old
            # per-template lines through stdout that would recreate the backlog.
            while True:
                pump(); snap = model.snapshot()
                sys.stdout.write("\rNEXUS " + model.native_status() + " | PoM Mstep/s " + number(snap['pom_msteps_s']) + " | read-only, Ctrl+C closes UI")
                sys.stdout.flush(); time.sleep(.5)
    except (KeyboardInterrupt, curses.error): return 0
    finally:
        reader.close()
    return 0

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--log")
    parser.add_argument("--telemetry")
    parser.add_argument("--setup", action="store_true", help="open first-run SETUP without starting a worker")
    parser.add_argument("--no-tui", action="store_true")
    parser.add_argument("--follow-latest", action="store_true", help="read-only: follow new production log sessions automatically")
    parser.add_argument("--read-only", action="store_true",
                        help="follow the saved event stream without starting or signalling a worker")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.command and args.command[0] == "--": args.command = args.command[1:]
    if args.setup: args.read_only = True
    if args.read_only:
        if args.command: parser.error("--read-only does not accept a worker command")
        return run_read_only(args)
    if not args.telemetry: parser.error("--telemetry is required when launching a worker")
    if not args.log: parser.error("--log is required when launching a worker")
    if not args.command: parser.error("a worker command is required after --")
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
