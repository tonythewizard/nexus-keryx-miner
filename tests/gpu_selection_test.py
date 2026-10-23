"""CPU-only selection and dual presentation regressions; private persistent fixtures."""
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import nexus_gpu_devices as inventory
import nexus_gpu_selection as selection
import nexus_gpu_switch as gpu_switch
from nexus_dashboard import DashboardModel
from nexus_presenter import build_presenter_frame


class Selection(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=os.environ['TMPDIR'])
        self.root = Path(self.tmp.name)
        self.devices = [{'id': selection.AMD_ID, 'driver': 'amdgpu', 'vram_bytes': 8*2**30, 'metrics': {}}]
        self.patches = [patch.object(inventory, 'ROOT', self.root),
                        patch.object(inventory, 'STATE', self.root/'state/gpu-devices'),
                        # Selection fixtures must never inspect or hand off the
                        # real worker running alongside the CPU test suite.
                        patch.object(gpu_switch, 'idle_worker', return_value=None),
                        patch.object(inventory, 'amd_devices', lambda: [dict(d) for d in self.devices])]
        for p in self.patches: p.start()
    def tearDown(self):
        for p in reversed(self.patches): p.stop()
        self.tmp.cleanup()
    def test_defaults_and_no_implicit_gpu_work(self):
        self.assertEqual(selection.runtime_lines(), ['1', '0'])
        self.assertFalse((inventory.STATE/'selection.json').exists())
    def test_each_gpu_toggle_is_persistent_and_independent(self):
        selection.set_enabled(selection.AMD_ID, True)
        selection.set_enabled(selection.CUDA_UUID, False)
        self.assertEqual(selection.runtime_lines(), ['0', '1'])
        selection.set_enabled(selection.CUDA_UUID, True)
        self.assertEqual(selection.runtime_lines(), ['1', '1'])
        self.assertEqual((inventory.STATE/'selection.json').stat().st_mode & 0o777, 0o600)
    def test_both_off_pauses_pom_preferences_not_ai(self):
        selection.set_enabled(selection.CUDA_UUID, False)
        self.assertEqual(selection.runtime_lines(), ['0', '0'])
        self.assertNotIn('inference', selection.configuration())
    def test_unknown_or_non_boolean_rejected(self):
        for ident, value in [('GPU-other', True), (selection.CUDA_UUID, 1), (selection.AMD_ID, 'on')]:
            with self.assertRaises(ValueError): selection.set_enabled(ident, value)
    def test_missing_or_unbound_amd_is_fail_closed(self):
        selection.set_enabled(selection.AMD_ID, True)
        self.devices[0]['driver'] = 'UNBOUND'
        with self.assertRaises(ValueError): selection.runtime_lines()
        self.devices.clear()
        with self.assertRaises(ValueError): selection.set_enabled(selection.AMD_ID, True)
    def test_corrupt_config_and_symlink_rejected(self):
        inventory.atomic(inventory.STATE/'selection.json', {'schema_version': 1, 'nvidia_pom': '1', 'amd_pom': False})
        with self.assertRaises(ValueError): selection.configuration()
        (inventory.STATE/'selection.json').unlink()
        target=self.root/'other';target.write_text('{}')
        (inventory.STATE/'selection.json').symlink_to(target)
        with self.assertRaises(OSError): selection.configuration()


class Presentation(unittest.TestCase):
    def model(self, dual):
        m=DashboardModel();m.fixture_mode=True
        m.gpu_devices={'cuda_primary': {'id': selection.CUDA_UUID, 'vendor': 'NVIDIA', 'name': 'Fixture NVIDIA', 'vram_bytes': 8*2**30},
                       'amd_devices': [{'id': selection.AMD_ID, 'name': 'Fixture AMD', 'mode': 'disabled', 'driver': 'amdgpu', 'capabilities_status': 'N/A', 'metrics': {}, 'vram_bytes': 8*2**30}] if dual else [],
                       'selection': {'nvidia_pom': True, 'amd_pom': False}}
        return m
    def test_single_gpu_view_is_preserved(self):
        m=self.model(False)
        frame=build_presenter_frame(46,160,m,'UNKNOWN',1)
        self.assertEqual(len(frame.panels),1)
        self.assertIn('Fixture NVIDIA',frame.panels[0]['title'])
    def test_dual_view_has_separate_cards_and_controls(self):
        m=self.model(True)
        frame=build_presenter_frame(46,160,m,'UNKNOWN',1)
        self.assertEqual([p['title'] for p in frame.panels],['Fixture NVIDIA / DEVICE VALUES','Fixture AMD / DEVICE VALUES','TOTALS / MATCHED UNITS AND WINDOWS'])
        labels=[r[0] for p in frame.panels for r in p['rows']]
        self.assertEqual(labels.count('PoM execution backend'),1)
        self.assertIn('AI service device',labels)
    def test_overview_has_both_gpu_rows_only_when_dual(self):
        from nexus_ui_preferences import DEFAULTS
        for dual in (False,True):
            frame=build_presenter_frame(46,160,self.model(dual),'UNKNOWN',0,
                preferences=dict(DEFAULTS, gpu_view='dual' if dual else 'single'))
            self.assertEqual(any(p['title']=='GPU FLEET / AI' for p in frame.panels),dual)
    def test_missing_telemetry_never_shows_zero_or_active(self):
        frame=build_presenter_frame(46,160,self.model(True),'UNKNOWN',1)
        rows={r[0]:r[1] for r in frame.panels[1]['rows']}
        self.assertEqual(rows['Mining state'],'N/A')
        self.assertEqual(rows['Fresh rate · 1 / 10 / 60s'],'N/A / N/A / N/A')
    def test_invalid_lane_does_not_enter_counters(self):
        m=self.model(True)
        m.ingest(json.dumps({'event':'AMD_LANE_STATE','attempts':-1}))
        self.assertFalse(m.amd_lane)
    def test_ai_backend_is_authoritative_and_resets(self):
        m=self.model(True);m.native={'rates':{'valid_1s':False},'elapsed_ns':0,'counters':{}}
        event=dict(event='AI_BACKEND_SELECTED',schema_version=1,backend='vulkan',device_id=selection.AMD_ID,trace_capture=False)
        m.ingest(json.dumps(event))
        with patch.object(m,'native_status',return_value='LIVE'):
            frame=build_presenter_frame(46,160,m,'UNKNOWN',1)
        self.assertIn('Fixture AMD',dict((r[0],r[1]) for r in frame.panels[1]['rows'])['AI service device'])
        for invalid in [dict(event,device_id='GPU-unapproved'),dict(event,backend=[]),dict(event,schema_version=True),dict(event,trace_capture=True)]:
            m.ingest(json.dumps(invalid))
        self.assertEqual(m.ai_backend['device_id'],selection.AMD_ID)
        m.ingest(json.dumps({'event':'STARTUP_STATE','state':'BOOT','result':'PASS'}))
        self.assertFalse(m.ai_backend)

    def test_native_counter_windows_and_restart(self):
        m=self.model(True);now=time.monotonic_ns()
        import nexus_dashboard_test
        for elapsed, count in ((11,0),(0,1000)):
            m.ingest(json.dumps(dict(event='AMD_LANE_STATE',attempts=count,completed=count,verified=0,discarded=0,
                                      busy=True,preparing=False,paused=False,failed=False)),observed_ns=now-elapsed*10**9)
            snap=nexus_dashboard_test.DashboardTests.native_snapshot(now-elapsed*10**9)
            snap['elapsed_ns']=(12-elapsed)*10**9
            snap['counters']['eligible_attempts']=count
            m.ingest(json.dumps(snap))
        self.assertEqual(len(m.amd_samples),2)
        with patch.object(m,'native_status',return_value='LIVE'):
            frame=build_presenter_frame(46,160,m,'UNKNOWN',1)
        rows={r[0]:r[1] for r in frame.panels[1]['rows']}
        self.assertIn('90.909 H/s',rows['Fresh rate · 1 / 10 / 60s'])
        m.ingest(json.dumps({'event':'STARTUP_STATE','state':'BOOT','result':'PASS'}))
        self.assertFalse(m.amd_samples);self.assertFalse(m.amd_lane)


if __name__=='__main__':unittest.main(verbosity=2)
