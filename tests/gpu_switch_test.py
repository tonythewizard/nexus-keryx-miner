"""CPU-only guarded GPU configuration transactions; no real signals or services."""
import json
import fcntl
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import nexus_gpu_switch as switch
import nexus_gpu_devices as devices
import nexus_gpu_selection as selection
import nexus_ui_preferences as preferences


class Handoff(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=os.environ['TMPDIR'])
        self.root = Path(self.tmp.name)
        self.patches = [patch.object(switch, 'ROOT', self.root),
                        patch.object(devices, 'ROOT', self.root),
                        patch.object(devices, 'STATE', self.root / 'state/gpu-devices'),
                        patch.object(preferences, 'preferences_path', return_value=self.root/'preferences.json'),
                        patch.object(switch, 'run', return_value=''),
                        patch.object(switch.subprocess, 'run', return_value=SimpleNamespace(returncode=1)),
                        patch.object(switch, 'idle_worker', return_value=None),
                        patch.object(switch, 'worker_identity', return_value=[]),
                        patch.object(switch, 'validate_route'),
                        patch.object(switch, 'request_switch')]
        for p in self.patches: p.start()
    def tearDown(self):
        for p in reversed(self.patches): p.stop()
        self.tmp.cleanup()
    def stage(self, enabled):
        switch.request_change('gpu_select', dict(device=selection.CUDA_UUID, enabled=enabled))
        return devices.private_json(devices.STATE/'handoff-request.json', {})['request_id']
    def test_secondary_workload_is_staged_independently(self):
        devices.prepare_state()
        preferences.save(dict(preferences.DEFAULTS, workloads={selection.CUDA_UUID:'nvidia'}))
        with patch.object(devices,'amd_devices',return_value=[{'id':selection.AMD_ID}]):
            switch.request_change('workload',dict(device=selection.AMD_ID,workload='nexus_bda'))
        pending=devices.private_json(devices.STATE/'handoff-request.json',{})
        self.assertEqual(pending['workloads'],{selection.CUDA_UUID:'nvidia',selection.AMD_ID:'nexus_bda'})
        self.assertEqual(preferences.load()['workloads'],{selection.CUDA_UUID:'nvidia'})
        self.assertEqual(pending['selection'],pending['previous_selection'])
    def test_secondary_cuda_route_is_rejected(self):
        with self.assertRaises(ValueError):
            switch.request_change('workload',dict(device=selection.AMD_ID,workload='nvidia'))
        switch.request_switch.assert_not_called()
    def test_unsupported_route_never_queues_a_stop(self):
        switch.validate_route.side_effect=ValueError('BDA_UNSUPPORTED')
        with patch.object(devices,'amd_devices',return_value=[{'id':selection.AMD_ID}]):
            with self.assertRaisesRegex(ValueError,'BDA_UNSUPPORTED'):
                switch.request_change('workload',dict(device=selection.AMD_ID,workload='nexus_bda'))
        switch.request_switch.assert_not_called()
        self.assertEqual(preferences.load()['workloads'],{})
    def test_primary_change_keeps_secondary_preference(self):
        preferences.save(dict(preferences.DEFAULTS,workloads={selection.AMD_ID:'nexus_bda'}))
        switch.request_change('workload',dict(device=selection.CUDA_UUID,workload='nexus'))
        pending=devices.private_json(devices.STATE/'handoff-request.json',{})
        self.assertEqual(pending['workloads'][selection.AMD_ID],'nexus_bda')

    def test_secondary_readiness_requires_rust_parity_acknowledgement(self):
        devices.prepare_state()
        worker=dict(pid=123,starttime='456',kind='nexus-live-cuda-worker')
        snap=dict(status='LIVE',elapsed_ns=1000,age_s=0,state=dict(mining='ACTIVE',pom_backend='cuda-driver'),
                  gpu_backends={selection.AMD_ID:'vulkan-jit'},gpu_ready={selection.AMD_ID:False})
        ready=dict(snap,gpu_ready={selection.AMD_ID:True})
        with patch.object(switch,'worker_identity',return_value=[worker]), \
             patch.object(switch,'recent_snapshot',side_effect=[snap,ready]) as read, \
             patch.object(switch.time,'sleep'), patch.object(switch.time,'monotonic',return_value=100):
            result=switch.start_runtime(dict(nvidia_pom=True,amd_pom=True),None,'fixture',switch.time.time())
        self.assertEqual(result,worker);self.assertEqual(read.call_count,2)

    def test_completed_ai_not_permanent_block(self):
        model=SimpleNamespace(history_complete=True, native_status=lambda:'LIVE',
            ai_metrics=dict(pending=0, requests_assigned=1, responses_confirmed=1))
        switch.require_idle_ai(model)
        for key,value in [('pending',None),('pending',1),('responses_confirmed',0)]:
            original=model.ai_metrics.copy(); model.ai_metrics[key]=value
            with self.assertRaises(ValueError): switch.require_idle_ai(model)
            model.ai_metrics=original
    def test_wrapper_preserves_vulkan_only_and_exclusive_cuda(self):
        from nexus_dashboard import worker_environment
        original=dict(NEXUS_POM_BACKEND='vulkan-jit', CUDA_VISIBLE_DEVICES=selection.CUDA_UUID,
                      NEXUS_CUDA_DEVICE_UUID=selection.CUDA_UUID)
        env=worker_environment(original)
        self.assertEqual(env['CUDA_VISIBLE_DEVICES'],'')
        self.assertNotIn('NEXUS_CUDA_DEVICE_UUID',env)
        self.assertEqual(original['CUDA_VISIBLE_DEVICES'],selection.CUDA_UUID)
        env=worker_environment(dict(NEXUS_POM_BACKEND='cuda-driver',CUDA_VISIBLE_DEVICES='all'))
        self.assertEqual(env['CUDA_VISIBLE_DEVICES'],selection.CUDA_UUID)
        with self.assertRaises(ValueError): worker_environment(dict(NEXUS_POM_BACKEND='unknown'))
    def test_startup_pending_requires_complete_available_scan(self):
        from nexus_dashboard import DashboardModel
        model=DashboardModel()
        model.ingest('AI_STARTUP_BACKFILL=INCOMPLETE unavailable=0 known_pending=0')
        self.assertIsNone(model.ai_metrics['pending'])
        model.ingest('AI_STARTUP_BACKFILL=COMPLETE unavailable=1 known_pending=0')
        self.assertIsNone(model.ai_metrics['pending'])
        model.ingest('AI_STARTUP_BACKFILL=COMPLETE unavailable=0 known_pending=0')
        self.assertEqual(model.ai_metrics['pending'],0)
        model.ai_metrics['pending']=2
        model.ingest('AI_STARTUP_BACKFILL=COMPLETE unavailable=0 known_pending=0')
        self.assertEqual(model.ai_metrics['pending'],2)
    def test_request_does_not_publish_before_handoff(self):
        self.stage(False)
        self.assertTrue(selection.configuration()['nvidia_pom'])
    def test_last_off_stops_without_starting(self):
        request=self.stage(False)
        switch.apply_runtime(request)
        self.assertFalse(selection.configuration()['nvidia_pom'])
        calls=[args.args[0] for args in switch.run.call_args_list]
        self.assertIn(['systemctl','--user','stop',switch.UNIT],calls)
        self.assertNotIn(['systemctl','--user','start',switch.UNIT],calls)
        self.assertEqual(devices.private_json(devices.STATE/'handoff.json',{})['status'],'STOPPED')
    def test_on_after_all_off_starts_and_checks_fresh_worker(self):
        devices.prepare_state()
        devices.atomic(devices.STATE/'selection.json',dict(schema_version=1,nvidia_pom=False,amd_pom=False))
        request=self.stage(True)
        worker=dict(pid=123,starttime='456',kind='nexus-live-cuda-worker')
        def start_check(args, **kwargs):
            if args == ['systemctl','--user','start',switch.UNIT]:
                # The real launcher takes this lock via ensure_runtime().
                with (devices.STATE/'selection.lock').open('a') as probe:
                    fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)
        with patch.object(switch,'worker_identity',side_effect=[[],[worker]]), patch.object(switch,'recent_snapshot',return_value=dict(status='LIVE',elapsed_ns=1000,age_s=0,state=dict(mining='ACTIVE',pom_backend='cuda-driver'))), patch.object(switch,'run',side_effect=start_check):
            switch.apply_runtime(request)
        self.assertTrue(selection.configuration()['nvidia_pom'])
        self.assertEqual(devices.private_json(devices.STATE/'handoff.json',{})['status'],'APPLIED')
    def test_failed_start_restores_previous_workflow_once(self):
        switch.request_change('workload',dict(device=selection.CUDA_UUID,workload='nexus'))
        request=devices.private_json(devices.STATE/'handoff-request.json',{})['request_id']
        with patch.object(switch,'start_runtime',side_effect=[ValueError('startup failed'),None]) as start, patch.object(switch,'run',return_value='failed'):
            switch.apply_runtime(request)
        self.assertEqual(start.call_count,2)
        self.assertEqual(preferences.load()['workloads'],{})
        self.assertEqual(devices.private_json(devices.STATE/'handoff.json',{})['status'],'ROLLED_BACK')
    def test_starting_unit_without_worker_is_not_restarted_again(self):
        switch.request_change('workload',dict(device=selection.CUDA_UUID,workload='nexus'))
        request=devices.private_json(devices.STATE/'handoff-request.json',{})['request_id']
        with patch.object(switch,'start_runtime',side_effect=ValueError('not ready')) as start, \
             patch.object(switch,'run',return_value='activating'):
            with self.assertRaisesRegex(ValueError,'not ready'): switch.apply_runtime(request)
        self.assertEqual(start.call_count,1)
    def test_occupied_worker_is_never_killed_for_rollback(self):
        switch.request_change('workload',dict(device=selection.CUDA_UUID,workload='nexus'))
        request=devices.private_json(devices.STATE/'handoff-request.json',{})['request_id']
        with patch.object(switch,'worker_identity',side_effect=[[],[dict(pid=3)]]), \
             patch.object(switch,'start_runtime',side_effect=ValueError('not ready')) as start:
            with self.assertRaisesRegex(ValueError,'not ready'): switch.apply_runtime(request)
        self.assertEqual(start.call_count,1)
        self.assertEqual(preferences.load()['workloads'][selection.CUDA_UUID],'nexus')
    def test_wrong_backend_never_acknowledged(self):
        devices.prepare_state()
        with patch.object(switch,'worker_identity',return_value=[dict(pid=123,kind='nexus-live-cuda-worker')]), \
             patch.object(switch,'recent_snapshot',return_value=dict(status='LIVE',elapsed_ns=1000,age_s=0,
                state=dict(mining='ACTIVE',pom_backend='vulkan-jit'))), \
             patch.object(switch.subprocess,'run',return_value=SimpleNamespace(returncode=0)):
            with self.assertRaisesRegex(ValueError,'startup failed'):
                switch.start_runtime(selection.configuration(),None,'a'*32,switch.time.time())
        self.assertEqual(devices.private_json(devices.STATE/'handoff.json',{})['status'],'START_REQUESTED')
    def test_newer_manual_stop_wins(self):
        request=self.stage(False)
        marker=self.root/'state/updates/user-stop.json';marker.parent.mkdir(parents=True)
        marker.write_text(json.dumps(dict(requested_unix=switch.time.time()+1)))
        with self.assertRaisesRegex(ValueError,'Manual shutdown'): switch.apply_runtime(request)
        self.assertTrue(selection.configuration()['nvidia_pom'])
        switch.run.assert_not_called()
    def test_changed_worker_refused(self):
        request=self.stage(False)
        with patch.object(switch,'idle_worker',return_value=dict(pid=2)):
            with self.assertRaisesRegex(ValueError,'identity changed'): switch.apply_runtime(request)
        self.assertTrue(selection.configuration()['nvidia_pom'])
    def test_unknown_request_refused(self):
        self.stage(False)
        with self.assertRaisesRegex(ValueError,'changed before restart'): switch.apply_runtime('0'*32)
    def test_duplicate_switch_refused(self):
        with patch.object(switch.subprocess,'run',return_value=SimpleNamespace(returncode=0)):
            with self.assertRaisesRegex(ValueError,'already in progress'): self.stage(False)
    def test_workload_only_staged(self):
        switch.request_change('workload',dict(device=selection.CUDA_UUID,workload='nexus'))
        self.assertEqual(preferences.load()['workloads'],{})
        request=devices.private_json(devices.STATE/'handoff-request.json',{})
        self.assertEqual(request['workloads'][selection.CUDA_UUID],'nexus')
    def test_missing_capability_does_not_save_workflow(self):
        switch.validate_route.side_effect=ValueError('refusing CUDA fallback')
        with self.assertRaisesRegex(ValueError,'refusing CUDA fallback'):
            switch.request_change('workload',dict(device=selection.CUDA_UUID,workload='nexus'))
        self.assertFalse((devices.STATE/'handoff-request.json').exists())

class IdleStates(unittest.TestCase):
    def test_unknown_block_outcome_reconciled_only_by_matching_hash(self):
        from nexus_dashboard import DashboardModel
        model=DashboardModel();h='a'*64
        model.ingest(json.dumps(dict(event='LIVE_SUBMIT_RESULT',state='UNKNOWN_OUTCOME',hash=h)))
        self.assertEqual(model.unresolved_block_submissions,{h})
        model.ingest(json.dumps(dict(event='LIVE_CHAIN_STATUS',state='CHAIN_OBSERVED',hash='b'*64)))
        self.assertEqual(model.unresolved_block_submissions,{h})
        model.ingest(json.dumps(dict(event='LIVE_CHAIN_STATUS',state='CHAIN_OBSERVED',hash=h)))
        self.assertEqual(model.unresolved_block_submissions,set())
        model.ingest(json.dumps(dict(event='LIVE_SUBMIT_RESULT',state='UNKNOWN_OUTCOME',hash=h)))
        self.assertEqual(model.unresolved_block_submissions,set())
    def test_unknown_outcome_handoff_requires_observed_hash_and_no_unresolved_submit(self):
        worker=dict(pid=123, starttime='456', kind='nexus-live-cuda-worker')
        h='a'*64
        model=SimpleNamespace(history_complete=True,native_status=lambda:'LIVE',
            ai_metrics=dict(pending=0,requests_assigned=0,responses_confirmed=0),
            last_submission_hash=h,last_submission_state='UNKNOWN_OUTCOME',
            _block_seen={'chain_observed':{h}},unresolved_block_submissions=set())
        with patch.object(switch,'worker_identity',return_value=[worker]), \
             patch.object(Path,'read_text',return_value='0::/user.slice/'+switch.UNIT), \
             patch('nexus_dashboard.TelemetrySessionReader') as reader, \
             patch.object(switch,'recent_snapshot',return_value=dict(status='LIVE',
                 state=dict(inference='IDLE',pause_reason='',submission='UNKNOWN_OUTCOME'))):
            reader.return_value.poll.return_value=model
            self.assertEqual(switch.idle_worker(),worker)
            for changes in ({'_block_seen':{'chain_observed':{'b'*64}}},
                            {'unresolved_block_submissions':{'b'*64}},
                            {'last_submission_state':'RPC_ACKNOWLEDGED'}):
                with patch.multiple(model,**changes):
                    with self.assertRaises(ValueError):switch.idle_worker()
            model.ai_metrics['pending']=1
            with self.assertRaises(ValueError):switch.idle_worker()

    def test_submission_states_and_resolved_ai(self):
        worker=dict(pid=123, starttime='456', kind='nexus-live-cuda-worker')
        model=SimpleNamespace(history_complete=True, native_status=lambda:'LIVE',
            ai_metrics=dict(pending=0, requests_assigned=1, responses_confirmed=1))
        with patch.object(switch,'worker_identity',return_value=[worker]), \
             patch.object(Path,'read_text',return_value='0::/user.slice/'+switch.UNIT), \
             patch('nexus_dashboard.TelemetrySessionReader') as reader:
            reader.return_value.poll.return_value=model
            for state in ('ARMED','DISABLED','RPC_ACKNOWLEDGED','RPC_REJECTED','CHAIN_OBSERVED','CHAIN_CONFIRMED'):
                with self.subTest(state=state), patch.object(switch,'recent_snapshot',return_value=dict(
                    status='LIVE',state=dict(inference='IDLE',pause_reason='',submission=state))):
                    self.assertEqual(switch.idle_worker(),worker)
            for state in ('SUBMIT_SENT','CANDIDATE_VALID','UNKNOWN_OUTCOME','UNKNOWN_AFTER_TIMEOUT'):
                with self.subTest(state=state), patch.object(switch,'recent_snapshot',return_value=dict(
                    status='LIVE',state=dict(inference='IDLE',pause_reason='',submission=state))):
                    with self.assertRaises(ValueError): switch.idle_worker()


if __name__ == '__main__': unittest.main()
