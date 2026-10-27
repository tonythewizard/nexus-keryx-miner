#!/usr/bin/env python3
import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
SPEC = importlib.util.spec_from_file_location("nexus_dashboard", ROOT / "scripts/nexus_dashboard.py")
dashboard = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(dashboard)


class DashboardTests(unittest.TestCase):
    def test_submit_contains_only_block_and_ai_outcomes(self):
        included = [
            ('LIVE_SUBMIT_RESULT', {'state': 'RPC_ACKNOWLEDGED'}),
            ('LIVE_H15_SUBMIT_RESULT', {'state': 'RPC_REJECTED'}),
            ('LIVE_CHAIN_STATUS', {'state': 'CHAIN_CONFIRMED'}),
            ('LIVE_CHAIN_STATUS', {'state': 'CHAIN_OBSERVED', 'selected_chain': False}),
            ('AI_RESPONSE_LIFECYCLE', {'state': 'FAILED'}),
            ('AI_RESPONSE_LIFECYCLE', {'state': 'RPC_ACKNOWLEDGED'}),
            ('AI_RESPONSE_LIFECYCLE', {'state': 'UNKNOWN_AFTER_TIMEOUT'}),
            ('AI_RESPONSE_SUBMISSION', {'reason': 'RPC_REJECTED_PERMANENT_OR_UNCLASSIFIED'}),
            ('AI_RESPONSE_SUBMISSION', {'reason': 'RPC_ACKNOWLEDGED'}),
        ]
        excluded = [
            ('ESCROW_CHAIN_GETBLOCK_SENT', {}), ('ESCROW_CLAIM_SUBMISSION', {}),
            ('LIVE_CUDA_NONCE=PASS', {'target_satisfied': True}),
            ('LIVE_TEMPLATE', {}), ('LIVE_SUBMIT_RESULT', {'state': 'SUBMIT_SENT'}),
            ('AI_RESPONSE_LIFECYCLE', {'state': 'SUBMITTING'}),
            ('AI_RESPONSE_LIFECYCLE', {'state': 'NOT_ASSIGNED'}),
            ('AI_RESPONSE_SUBMISSION', {'reason': 'PRIVATE_REQUEST_CONTEXT_PENDING'}),
            ('INFERENCE_SELF_TEST=PASS', {}),
        ]
        for event, obj in included:
            self.assertTrue(dashboard.event_matches(event, 'SUBMIT', obj), (event, obj))
        for event, obj in excluded:
            self.assertFalse(dashboard.event_matches(event, 'SUBMIT', obj), (event, obj))
        self.assertTrue(dashboard.event_matches('STARTUP_STATE', 'ERROR', {'result': 'BLOCKED'}))
        self.assertTrue(dashboard.event_matches('AI_RESPONSE_LIFECYCLE', 'ERROR', {'state': 'FAILED'}))

    def test_session_history_survives_noisy_tail_and_snapshot_is_fixed(self):
        model = dashboard.DashboardModel()
        for index in range(240):
            model.ingest(json.dumps({'event': 'LIVE_SUBMIT_RESULT', 'state': 'RPC_ACKNOWLEDGED',
                                     'hash': f'{index:064x}'}))
        model.ingest('{"event":"TRANSPORT_ERROR","reason":"fixture"}')
        before = model.presentation_copy()
        for _ in range(700): model.ingest('ESCROW_CHAIN_GETBLOCK_SENT id=1')
        self.assertEqual(len(model.log_history['SUBMIT']), 240)
        self.assertEqual(len(model.log_history['ERROR']), 1)
        rows, live = before.log_history['SUBMIT'].page()
        self.assertEqual(len(rows), 100)
        self.assertEqual((live['start'], live['end']), (140, 240))
        older, page = before.log_history['SUBMIT'].page(live['older_before'])
        self.assertEqual((page['start'], page['end']), (40, 140))
        model.ingest('{"event":"AI_RESPONSE_LIFECYCLE","state":"FAILED","request_id":"fixture"}')
        self.assertEqual(len(model.log_history['SUBMIT']), 241)
        self.assertEqual(len(before.log_history['SUBMIT']), 240)
        self.assertEqual(model.log_history['SUBMIT'].page(140)[0], older)
        self.assertEqual(before.log_history['SUBMIT'].page()[0], rows)
        with self.assertRaises(TypeError): before.log_history['SUBMIT'].append(('x', 'x', 'x'))
        with self.assertRaises(ValueError): model.log_history['SUBMIT'].page(True)

    def test_error_and_submit_recover_from_entire_session_then_append_live(self):
        path = pathlib.Path(sys.argv[1]) / f'history-{os.getpid()}.jsonl'
        noise = json.dumps({'worker': {'event': 'NOISY_FIXTURE', 'detail': 'x' * 512}}) + '\n'
        with path.open('w') as stream:
            stream.write('{"worker":{"event":"LIVE_SUBMIT_RESULT","state":"RPC_ACKNOWLEDGED"}}\n')
            stream.write(noise * 1000)
            stream.write('{"worker":{"event":"LIVE_H15_SUBMIT_RESULT","state":"RPC_ACKNOWLEDGED"}}\n')
            stream.write('{"worker":{"event":"TRANSPORT_ERROR","reason":"old middle error"}}\n')
            stream.write(noise * 6000)
        model = dashboard.DashboardModel()
        follower = dashboard.TelemetryFollower(path); self.addCleanup(follower.close)
        follower.bootstrap(model)
        self.assertTrue(model.history_complete)
        self.assertEqual(len(model.log_history['SUBMIT']), 2)
        self.assertEqual(len(model.log_history['ERROR']), 1)
        self.assertIn('old middle error', model.log_history['ERROR'][0][2])
        with path.open('a') as stream:
            stream.write('{"worker":{"event":"AI_RESPONSE_LIFECYCLE","state":"FAILED"}}\n')
        follower.drain(model)
        self.assertEqual(len(model.log_history['SUBMIT']), 3)
        self.assertEqual(len(model.log_history['ERROR']), 2)
        reopened = dashboard.DashboardModel()
        again = dashboard.TelemetryFollower(path); self.addCleanup(again.close)
        again.bootstrap(reopened)
        self.assertEqual([row[1:] for row in reopened.log_history['SUBMIT'][:]],
                         [row[1:] for row in model.log_history['SUBMIT'][:]])

    def test_log_presenter_pages_full_history_in_bounded_frames(self):
        from nexus_presenter import build_presenter_frame
        from nexus_ui_preferences import DEFAULTS
        model = dashboard.DashboardModel()
        for _ in range(230): model.ingest('{"event":"AI_RESPONSE_LIFECYCLE","state":"RPC_ACKNOWLEDGED"}')
        frame = build_presenter_frame(46, 160, model, 'UNKNOWN', 4, 'SUBMIT', preferences=DEFAULTS)
        panel = frame.panels[0]
        self.assertEqual(len(panel['table_rows']), 100)
        self.assertEqual(panel['log_history']['total'], 230)
        first = build_presenter_frame(46, 160, model, 'UNKNOWN', 4, 'SUBMIT',
                                      preferences=DEFAULTS, log_before=30).panels[0]
        self.assertEqual(len(first['table_rows']), 30)
        self.assertIsNone(first['log_history']['older_before'])
        self.assertTrue(first['log_history']['has_newer'])

    def test_native_inference_span_is_labelled_with_sampling_uncertainty(self):
        m = dashboard.DashboardModel(); first = self.native_snapshot()
        m.ingest(json.dumps(first))
        running = self.native_snapshot(first['monotonic_ns'] + 1000000000); running['state']['inference'] = 'RUNNING'
        m.ingest(json.dumps(running))
        ending = self.native_snapshot(first['monotonic_ns'] + 8000000000)
        m.ingest(json.dumps(ending))
        self.assertEqual(m.ai_metrics['service_ms'], 7000)
        self.assertEqual(m.ai_metrics['service_source'], 'NATIVE_SAMPLED')
        self.assertEqual(m.ai_metrics['service_uncertainty_ms'], 8000)

    def test_wallet_follows_worker_payout_and_clears_other_address_balance(self):
        m = dashboard.DashboardModel()
        m.wallet['payout_address'] = 'keryx:' + 'q' * 61
        native = self.native_snapshot()
        native['wallet'] = {'authorization': 'AUTHORIZED', 'verification': 'VALID',
                            'payout_address': 'keryx:' + 'p' * 61, 'escrow_public_key': 'a' * 64}
        m.explorer['balance'] = {'data': {'balance_sompi': 1}}
        m.ingest(json.dumps(native))
        self.assertEqual(m.mining_payout, 'keryx:' + 'p' * 61)
        self.assertEqual(m.wallet['payout_address'], m.mining_payout)
        self.assertEqual(m.explorer, {})
        frame = '\n'.join(dashboard.build_matrix_frame(42, 140, m, '', 'READ ONLY', 5).lines())
        self.assertIn(m.mining_payout, frame)
        self.assertIn('ACTIVE WORKER · AUTOMATIC', frame)
        p = dashboard.OfficialExplorerPoller()
        m2 = dashboard.DashboardModel(); m2.network = 'mainnet'; m2.wallet['payout_address'] = m.wallet['payout_address']
        with mock.patch.object(p, 'get') as get:
            p.pump(m2); get.assert_not_called()

    def test_last_rust_timing_and_observed_inference_interval_survive_bootstrap(self):
        m = dashboard.DashboardModel(); m.ingest(json.dumps(self.native_snapshot()))
        m.ingest('{"event":"LIVE_PHASE_PROFILE","phase":"official_rust_verify","duration_ms":7.25}')
        native = self.native_snapshot(m.native['monotonic_ns'] + 1); native['latency_ms']['rust_verify'] = None
        m.ingest(json.dumps(native))
        self.assertEqual(m.last_measured_ms['rust_verify'], 7.25)
        view = '\n'.join(dashboard.build_matrix_frame(42, 140, m, '', 'READ ONLY', 1).lines())
        self.assertIn('7.25 ms', view)
        m.ingest('OPOI_RUNTIME=START', 1000000); m.ingest('OPOI_RUNTIME=PASS', 4000000)
        self.assertEqual(m.ai_metrics['service_ms'], 3)

    def test_official_explorer_is_advisory_and_expires(self):
        m = dashboard.DashboardModel(); snapshot = self.native_snapshot(); m.ingest(json.dumps(snapshot))
        now = __import__('time').monotonic()
        m.explorer = {
            'balance': {'status': 'AVAILABLE', 'observed': now, 'data': {'balance_sompi': 123456789}},
            'inferences': {'status': 'AVAILABLE', 'observed': now, 'data': {'served': 5, 'answered': 13}},
            'strikes': {'status': 'AVAILABLE', 'observed': now, 'data': {'consecutive_misses': 0, 'lifetime_strikes': 1, 'suspended_until_daa_score': None}},
        }
        self.assertEqual(dashboard.krx(123456789), '1.23456789 KRX')
        self.assertEqual(dashboard.krx(True), 'N/A')
        for tab, expected in ((2, 'payout served'), (3, '0 / 1'), (5, '1,23456789 KRX')):
            view = '\n'.join(dashboard.build_matrix_frame(42, 140, m, '', 'READ ONLY', tab).lines())
            self.assertIn(expected, view)
        self.assertEqual(m.wallet['authorization'], 'UNKNOWN')
        self.assertEqual(m.ai_metrics['responses_confirmed'], None)
        m.explorer['balance']['observed'] -= dashboard.EXPLORER_TTL + 1
        self.assertEqual(dashboard.explorer_view(m, 'balance'), ({}, 'STALE'))

    def test_official_explorer_schema_is_strict_and_strips_unused_data(self):
        p = dashboard.OfficialExplorerPoller
        self.assertEqual(p.normalize('balance', {'balance_sompi': '123', 'secret': 'ignored'}), {'balance_sompi': 123})
        self.assertEqual(p.normalize('strikes', {'consecutive_misses': 0, 'lifetime_strikes': 1, 'suspended_until_daa_score': None, 'pending_burns': [{}, {}]})['pending_burns_count'], 2)
        for bad in ({}, {'balance_sompi': True}, {'balance_sompi': -1}, {'balance_sompi': 'invalid'}):
            with self.assertRaises(ValueError): p.normalize('balance', bad)
        m = dashboard.DashboardModel(); m.network = 'testnet'; m.explorer['balance'] = {'data': {}}
        poller = p()
        with mock.patch.object(p, 'get') as get:
            poller.pump(m); get.assert_not_called()
        self.assertEqual(m.explorer, {})

    def test_terminal_title_uses_release_version(self):
        import io
        output = io.StringIO()
        with mock.patch.object(dashboard.sys, 'stdout', output), mock.patch.object(output, 'isatty', return_value=True):
            dashboard.set_matrix_background(True)
        self.assertIn('Nexus - ' + dashboard.RELEASE_VERSION, output.getvalue())

    def test_hashrate_si_units_and_invalid_values(self):
        for value, expected in ((0, "0.000 H/s"), (500, "500.000 H/s"), (1000, "1.000 kH/s"),
                                (1e6, "1.000 MH/s"), (1e9, "1.000 GH/s"), (1e12, "1.000 TH/s")):
            self.assertEqual(dashboard.format_hashrate(value), expected)
        for value in (None, True, -1, float("nan"), float("inf")):
            self.assertEqual(dashboard.format_hashrate(value), "N/A")

    def test_hashrate_is_completed_final_pom_not_eligible_or_steps(self):
        m = dashboard.DashboardModel(); snapshot = self.native_snapshot()
        snapshot["eligible_rates"] = {"60s": 17}
        m.ingest(json.dumps(snapshot))
        self.assertEqual(dashboard.pom_hashrate(m, "60s"), 1.5e6 / 256)
        view = "\n".join(dashboard.build_matrix_frame(42, 140, m, "", "READ ONLY", 0).lines())
        self.assertIn("PoM HASHRATE", view); self.assertIn("ELIGIBLE / s", view)
        self.assertIn("5.859 kH/s", view)
        m.native["counters"]["pom_steps_completed"] += 1
        self.assertIsNone(dashboard.pom_hashrate(m, "60s"))
        m.native["counters"]["pom_steps_completed"] -= 1
        m.native["rates"]["valid_60s"] = False
        self.assertIsNone(dashboard.pom_hashrate(m, "60s"))
        m.native["monotonic_ns"] -= dashboard.TELEMETRY_STALE_NS + 1
        self.assertIsNone(dashboard.pom_hashrate(m, "10s"))

    @staticmethod
    def native_snapshot(stamp=None):
        return {
            "event": "NEXUS_TELEMETRY", "schema_version": 1,
            "clock": "steady_monotonic_ns", "monotonic_ns": stamp or __import__("time").monotonic_ns(),
            "elapsed_ns": 10_000_000_000,
            "counters": {"gpu_work_units_launched": 1000, "gpu_work_units_completed": 999,
                         "pom_steps_completed": 999 * 256, "pom_kernels_completed": 999,
                         "nonce_attempts": 1000, "proofs_completed": 998,
                         "proofs_rust_verified": 997, "proofs_fresh": 996, "proofs_stale": 1,
                         "stale_work_discarded": 1, "targets_satisfied": 0},
            "rates": {"unit": "million_PoM_model_tile_steps_per_second",
                      "pom_msteps_s_1s": 2.5, "pom_msteps_s_10s": 2.0, "pom_msteps_s_60s": 1.5,
                      "kernels_s_1s": 9.0, "kernels_s_10s": 8.0, "kernels_s_60s": 7.0,
                      "proofs_s_1s": 3.0, "proofs_s_10s": 2.0, "proofs_s_60s": 1.0,
                      "stale_work_fraction": 0.001, "valid_stale_work_fraction": True,
                      "valid_1s": True, "valid_10s": True, "valid_60s": True,
                      "stock_comparable_mhs": None,
                      "stock_comparable_reason": "v0.5.7 packaged miner is binary-only here; exact unit not source-verifiable"},
            "latency_ms": {"cuda_kernel": 1.0, "cuda_h2d": 0.1, "cuda_d2h": 0.1,
                           "cuda_engine_total": 1.5, "proof_build": 2.0, "rust_verify": 3.0},
            "state": {"mining": "ACTIVE", "inference": "IDLE", "gpu_activity": "CUDA_ACTIVE",
                      "pause_reason": "", "submission": "DISABLED"},
            "scheduler": {"job_invalidations": 2},
        }

    def test_readiness_does_not_turn_hash_only_into_inference_ready(self):
        model = dashboard.DashboardModel()
        model.ingest('{"event":"STARTUP_STATE","state":"MODEL_VERIFIED","result":"PASS"}')
        self.assertEqual(model.inference, "UNKNOWN")
        model.ingest('{"event":"STARTUP_STATE","state":"INFERENCE_ENGINE_PROBE","result":"PASS"}')
        self.assertEqual(model.inference, "PASS")
        model.ingest('{"event":"STARTUP_STATE","state":"SERVICE_READY","result":"BLOCKED"}')
        self.assertEqual(model.status, "DEGRADED")

    def test_only_authorized_gpu_uuid_is_queried(self):
        with mock.patch.object(dashboard.subprocess, "run") as runner:
            runner.return_value = subprocess.CompletedProcess([], 0, "3, 800, 8192, 45, 90, 1800\n", "")
            self.assertIn("3, 800", dashboard.gpu_sample())
            command = runner.call_args.args[0]
            self.assertEqual(command[command.index("-i") + 1], dashboard.AUTHORIZED_GPU)
            self.assertNotIn("--query-gpu=all", command)

    def test_all_dashboard_pages_render_without_control_side_effects(self):
        class Screen:
            def __init__(self): self.rows = []
            def getmaxyx(self): return (30, 160)
            def erase(self): self.rows.clear()
            def addnstr(self, row, col, text, limit, *_): self.rows.append((row, text[:limit]))
            def refresh(self): pass
            def getch(self): return -1
        model = dashboard.DashboardModel()
        model.ingest('{"event":"STARTUP_STATE","state":"SERVICE_READY","result":"BLOCKED"}')
        screen = Screen()
        with mock.patch.object(dashboard.curses, "curs_set"):
            for page in range(6):
                dashboard.render(screen, model, page, "GPU telemetry", "RUNNING")
                self.assertTrue(screen.rows)
        self.assertEqual(model.status, "DEGRADED")

    def test_native_snapshot_is_authoritative_for_rate_and_survives_dashboard_restart(self):
        model = dashboard.DashboardModel()
        model.ingest('{"event":"LIVE_CUDA_NONCE=PASS","nonce":1}')
        self.assertIsNone(model.snapshot()["pom_msteps_s"])
        snapshot = self.native_snapshot()
        model.ingest(json.dumps(snapshot))
        self.assertEqual(model.native_status(), "LIVE")
        self.assertEqual(model.snapshot()["pom_msteps_s"], 2.5)
        self.assertEqual(model.native["counters"]["gpu_work_units_completed"], 999)
        restarted = dashboard.DashboardModel()
        restarted.ingest(json.dumps(snapshot))
        self.assertEqual(restarted.native["counters"], model.native["counters"])
        self.assertEqual(restarted.native["rates"], model.native["rates"])

    def test_native_snapshot_rejects_invalid_deltas_and_marks_old_data_stale(self):
        model = dashboard.DashboardModel()
        stamp = __import__("time").monotonic_ns()
        valid = self.native_snapshot(stamp)
        model.ingest(json.dumps(valid))
        older = self.native_snapshot(stamp - 1)
        model.ingest(json.dumps(older))
        self.assertEqual(model.native_status(), "INVALID")
        model3 = dashboard.DashboardModel()
        model3.ingest(json.dumps(valid))
        lowered = self.native_snapshot(stamp + 1)
        lowered["counters"]["pom_steps_completed"] -= 1
        model3.ingest(json.dumps(lowered))
        self.assertEqual(model3.native_status(), "INVALID")
        model2 = dashboard.DashboardModel()
        model2.ingest(json.dumps(valid))
        with mock.patch.object(dashboard.time, "monotonic_ns", return_value=stamp + dashboard.TELEMETRY_STALE_NS + 1):
            self.assertEqual(model2.native_status(), "STALE")
            self.assertIsNone(model2.snapshot()["pom_msteps_s"])

    def test_render_updates_only_changed_rows_and_handles_small_terminal(self):
        class Screen:
            def __init__(self, size=(30, 160)): self.rows, self.size, self.erase_calls = [], size, 0
            def getmaxyx(self): return self.size
            def erase(self): self.erase_calls += 1; self.rows.clear()
            def addnstr(self, row, col, text, limit, *_): self.rows.append((row, text[:limit]))
            def refresh(self): pass
        model = dashboard.DashboardModel()
        model.ingest(json.dumps(self.native_snapshot()))
        screen = Screen()
        with mock.patch.object(dashboard.curses, "curs_set"):
            dashboard.render(screen, model, 0, "GPU sample", "RUNNING")
            first_writes = len(screen.rows)
            screen.rows.clear()
            dashboard.render(screen, model, 0, "GPU sample", "RUNNING")
            self.assertEqual(len(screen.rows), 0)
            self.assertEqual(screen.erase_calls, 1)
            screen.size = (10, 50)
            dashboard.render(screen, model, 0, "GPU sample", "RUNNING")
            self.assertGreater(first_writes, 0)
            self.assertEqual(screen.erase_calls, 2)

    def test_missing_ai_and_escrow_values_render_as_na(self):
        class Screen:
            def __init__(self): self.rows = []
            def getmaxyx(self): return (30, 120)
            def erase(self): pass
            def addnstr(self, row, col, text, limit, *_): self.rows.append(text[:limit])
            def refresh(self): pass
        model, screen = dashboard.DashboardModel(), Screen()
        with mock.patch.object(dashboard.curses, "curs_set"):
            dashboard.render(screen, model, 2, "N/A", "READ ONLY")
        text = "\n".join(screen.rows)
        self.assertIn("N/A / N/A", text)
        self.assertNotIn("None", text)

    def test_read_only_dashboard_closes_without_worker_or_gpu_side_effects(self):
        root = pathlib.Path(sys.argv[1]).resolve() / f"readonly-{os.getpid()}"
        root.mkdir(parents=True, exist_ok=True)
        telemetry_path = root / "events.jsonl"
        snapshot = self.native_snapshot()
        telemetry_path.write_text(json.dumps({"worker": snapshot}) + "\n", encoding="utf-8")
        args = type("Args", (), {"telemetry": str(telemetry_path), "root": str(root), "no_tui": False})()
        class Screen:
            def nodelay(self, _): pass
            def getch(self): return ord("q")
            def getmaxyx(self): return (25, 100)
            def erase(self): pass
            def addnstr(self, *args): pass
            def refresh(self): pass
        with mock.patch.object(dashboard.sys.stdout, "isatty", return_value=True), \
             mock.patch.object(dashboard.sys.stdin, "isatty", return_value=True), \
             mock.patch.object(dashboard.curses, "wrapper", side_effect=lambda fn: fn(Screen())), \
             mock.patch.object(dashboard, "gpu_sample", return_value="AUTHORIZED UUID ONLY") as gpu, \
             mock.patch.object(dashboard, "load_wallet_status"), \
             mock.patch('nexus_escrow_availability.EscrowAvailabilityPoller.pump', return_value={'status': 'N/A'}), \
             mock.patch.object(dashboard, 'refresh_development_status'), \
             mock.patch.object(dashboard.subprocess, "Popen") as popen, \
             mock.patch.dict(os.environ, {"TERM": "xterm"}):
            self.assertEqual(dashboard.run_read_only(args), 0)
            popen.assert_not_called()
            gpu.assert_called_once()

    def test_ai_and_escrow_pages_only_show_reported_values(self):
        model = dashboard.DashboardModel()
        model.ingest('{"event":"STARTUP_STATE","state":"AI_REQUEST_SERVICE_READY","result":"UNKNOWN"}')
        self.assertEqual(model.ai_service, "UNKNOWN")
        self.assertIsNone(model.ai_metrics["requests_received"])
        model.ingest('{"event":"AI_REQUEST_ASSIGNED"}')
        model.ingest('{"event":"SERVICE_STRIKES_SNAPSHOT","consecutive":2,"lifetime":7,"pending_burns":1}')
        self.assertEqual(model.ai_metrics["requests_assigned"], 1)
        self.assertEqual(model.escrow_metrics["consecutive_strikes"], 2)
        self.assertEqual(model.escrow_metrics["suspended_until_daa"], "UNKNOWN")

    def test_worker_strike_poll_and_escrow_events_feed_dashboard_without_inventing_zero(self):
        model = dashboard.DashboardModel()
        model.ingest('{"event":"SERVICE_STRIKE_POLL","identity":"' + "a" * 64 +
                     '","availability":"UNKNOWN","stale":true,"consecutive":null,'
                     '"lifetime":null,"pending_burns":null,"suspended_until_daa":null,'
                     '"observed_unix_ms":1234,"error":"GetServiceStrikes unavailable"}')
        self.assertEqual(model.escrow_metrics["strike_status"], "STALE")
        self.assertIsNone(model.escrow_metrics["consecutive_strikes"])
        self.assertEqual(model.escrow_metrics["strike_error"], "GetServiceStrikes unavailable")
        model.ingest('{"event":"SERVICE_STRIKE_POLL","identity":"' + "a" * 64 +
                     '","availability":"AVAILABLE","stale":false,"consecutive":0,'
                     '"lifetime":0,"pending_burns":0,"suspended_until_daa":null,'
                     '"observed_unix_ms":5678,"error":""}')
        self.assertEqual(model.escrow_metrics["strike_status"], "AVAILABLE")
        self.assertEqual(model.escrow_metrics["consecutive_strikes"], 0)
        model.ingest('{"event":"ESCROW_WATCHER_BLOCK","daa":12,"pending_count":3,'
                     '"pending_amount_sompi":45,"claim_submission":"DISABLED"}')
        self.assertEqual(model.escrow_metrics["escrow_pending_outpoints"], 3)
        self.assertIsNone(model.escrow_metrics["pending_claims"])

    def test_worker_ai_lifecycle_uses_named_states_and_deduplicates_metrics(self):
        model = dashboard.DashboardModel()
        model.ingest('{"event":"AI_REQUEST_QUEUED","request_id":"' + "b" * 64 +
                     '","daa":7,"pending":1}')
        assigned = '{"event":"AI_REQUEST_ASSIGNED","request_id":"' + "b" * 64 + '"}'
        model.ingest(assigned)
        model.ingest(assigned)
        lifecycle = ('{"event":"AI_RESPONSE_LIFECYCLE","request_id":"' + "b" * 64 +
                     '","state":"RPC_ACKNOWLEDGED","serve_result":0,"daa":7,"pending":0}')
        model.ingest(lifecycle)
        model.ingest(lifecycle)
        self.assertEqual(model.ai_metrics["requests_received"], 1)
        self.assertEqual(model.ai_metrics["requests_assigned"], 1)
        self.assertEqual(model.ai_metrics["responses_sent"], 1)
        self.assertIsNone(model.ai_metrics["responses_confirmed"])

    def test_live_template_sync_and_confirmed_block_are_deduplicated(self):
        m = dashboard.DashboardModel()
        m.ingest(json.dumps({"event": "LIVE_TEMPLATE", "network": "mainnet", "synced": True, "daa": 99, "generation": 4}))
        event = {"event": "LIVE_SUBMIT_RESULT", "state": "RPC_ACKNOWLEDGED", "hash": "a" * 64}
        m.ingest(json.dumps(event)); m.ingest(json.dumps(event))
        event = {"event": "LIVE_CHAIN_STATUS", "state": "CHAIN_CONFIRMED", "hash": "a" * 64, "selected_chain": True, "blue_score": 100, "selected_tip_blue_score": 106}
        m.ingest(json.dumps(event)); m.ingest(json.dumps(event))
        self.assertEqual(m.synced, "true")
        self.assertEqual((m.stats["submit_sent"], m.stats["rpc_ack"], m.stats["chain_observed"], m.stats["chain_confirmed"]), (1, 1, 1, 1))
        m.ingest(json.dumps({"event": "LIVE_SUBMIT_RESULT", "state": "RPC_REJECTED", "hash": "b" * 64}))
        self.assertEqual(m.stats["submit_sent"], 2)

    def test_ai_lifecycle_can_advance_from_ack_to_confirmation(self):
        m = dashboard.DashboardModel()
        for state in ["RPC_ACKNOWLEDGED", "RPC_ACKNOWLEDGED", "CHAIN_OBSERVED", "FINALIZED"]:
            m.ingest(json.dumps({"event": "AI_RESPONSE_LIFECYCLE", "state": state, "request_id": "b" * 64}))
        self.assertEqual(m.ai_metrics["responses_sent"], 1)
        self.assertEqual(m.ai_metrics["responses_confirmed"], 1)
        self.assertEqual(m.ai_metrics["requests_assigned"], 1)

    def test_follower_drains_bursts_partial_records_and_old_block_history(self):
        root = pathlib.Path(sys.argv[1]).resolve() / f"burst-{os.getpid()}"
        root.mkdir(parents=True, exist_ok=True); path = root / "events.jsonl"
        snapshot = self.native_snapshot()
        with path.open("w") as f:
            f.write(json.dumps({"worker": {"event": "LIVE_CHAIN_STATUS", "state": "CHAIN_CONFIRMED", "hash": "c" * 64, "selected_chain": True, "blue_score": 100, "selected_tip_blue_score": 106}}) + "\n")
            filler = json.dumps({"worker": {"event": "AI_BLOCK_OBSERVED", "padding": "x" * 2048}}) + "\n"
            for _ in range(1200): f.write(filler)
            f.write(json.dumps({"worker": snapshot}) + "\n")
        follower = dashboard.TelemetryFollower(path); m = dashboard.DashboardModel()
        follower.bootstrap(m)
        self.assertEqual(m.stats["chain_observed"], 1)
        self.assertEqual(m.stats["chain_confirmed"], 1)
        self.assertEqual(m.native["monotonic_ns"], snapshot["monotonic_ns"])
        fresh = self.native_snapshot(snapshot["monotonic_ns"] + 1)
        with path.open("a") as f:
            for _ in range(1000): f.write('{"worker":{"event":"JOB_REFRESH_PENDING"}}\n')
            f.write(json.dumps({"worker": fresh}) + "\n")
        self.assertGreaterEqual(follower.drain(m), 1001)
        self.assertEqual(m.native["monotonic_ns"], fresh["monotonic_ns"])
        raw = json.dumps({"worker": {"event": "LIVE_TEMPLATE", "daa": 777, "synced": True}})
        with path.open("a") as f: f.write(raw[:20])
        follower.drain(m)
        with path.open("a") as f: f.write(raw[20:] + "\n")
        follower.drain(m); self.assertEqual(m.daa, "777")
        path.write_text("", encoding="utf-8"); self.assertTrue(follower.replaced()); follower.close()

    def test_wallet_copy_notice_survives_native_refresh(self):
        m = dashboard.DashboardModel(); m.wallet["notice"] = "copied"
        snapshot = self.native_snapshot(); snapshot["wallet"] = {"authorization": "AUTHORIZED", "verification": "VALID", "escrow_public_key": "a" * 64}
        m.ingest(json.dumps(snapshot)); self.assertEqual(m.wallet["notice"], "copied")
        self.assertEqual(m.escrow_metrics["delegation"], "VALID")

    def test_arrow_navigation_event_filters_and_responsive_frame_bounds(self):
        self.assertEqual(dashboard.select_page(9, 7), 0)
        self.assertEqual(dashboard.select_page(dashboard.curses.KEY_LEFT, 0), 7)
        self.assertTrue(dashboard.event_matches("LIVE_CHAIN_STATUS", "SUBMIT", {"state": "CHAIN_CONFIRMED"}))
        self.assertTrue(dashboard.event_matches("LIVE_TEMPLATE", "JOB"))
        self.assertTrue(dashboard.event_matches("LIVE_SUBMIT_RPC_REJECTED", "ERROR"))
        m = dashboard.DashboardModel(); m.ingest(json.dumps(self.native_snapshot()))
        for height, width in [(10, 50), (25, 80), (42, 140)]:
            for tab in range(7):
                frame = dashboard.build_matrix_frame(height, width, m, "99, 7635, 8192, 66, 229, 1560", "READ ONLY", tab)
                self.assertEqual(len(frame.lines()), height)
                self.assertTrue(all(len(line) == width - 1 for line in frame.lines()))

    def test_headless_mode_preserves_complete_stdout_and_structured_events(self):
        root = pathlib.Path(sys.argv[1]).resolve() / f"run-{os.getpid()}"
        root.mkdir(parents=True, exist_ok=True)
        log = root / "worker.log"
        telemetry = root / "worker.jsonl"
        args = type("Args", (), {
            "root": str(root), "log": str(log), "telemetry": str(telemetry), "no_tui": True,
            "command": [sys.executable, "-c", "print('{\\\"event\\\":\\\"STARTUP_STATE\\\",\\\"state\\\":\\\"BOOT\\\",\\\"result\\\":\\\"PASS\\\"}'); print('second-line')"],
        })()
        with mock.patch.object(dashboard, "gpu_sample", return_value="MOCK"), \
             mock.patch('nexus_escrow_availability.EscrowAvailabilityPoller.pump', return_value={'status': 'N/A'}):
            self.assertEqual(dashboard.run(args), 0)
        contents = log.read_text(encoding="utf-8")
        self.assertIn('"state":"BOOT"', contents)
        self.assertIn("second-line", contents)
        records = [json.loads(line) for line in telemetry.read_text(encoding="utf-8").splitlines()]
        self.assertEqual(len(records), 2)
        self.assertEqual(records[0]["worker"]["event"], "STARTUP_STATE")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: nexus_dashboard_test.py PERSISTENT_TEST_DIR")
    unittest.main(argv=[sys.argv[0]], verbosity=2)
