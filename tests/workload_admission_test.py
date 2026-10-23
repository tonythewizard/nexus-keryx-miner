"""CPU-only backend admission: a saved preference must not imply activation."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from nexus_gpu_devices import CUDA_UUID
from nexus_ui_preferences import DEFAULTS
from nexus_workload_admission import primary_backend, read_preferences


class WorkloadAdmission(unittest.TestCase):
    def test_default_and_explicit_cuda(self):
        for value in ('auto', 'nvidia'):
            p = dict(DEFAULTS, workloads={CUDA_UUID:value})
            self.assertEqual(primary_backend(CUDA_UUID,p),'cuda-driver')

    def test_vulkan_never_silently_falls_back(self):
        p = dict(DEFAULTS,workloads={CUDA_UUID:'nexus'})
        original=copy.deepcopy(p)
        with self.assertRaisesRegex(ValueError,'refusing CUDA fallback'):
            primary_backend(CUDA_UUID,p)
        self.assertEqual(original,p)

    def test_vulkan_requires_native_capability(self):
        p = dict(DEFAULTS, workloads={CUDA_UUID: 'nexus'})
        self.assertEqual(primary_backend(CUDA_UUID, p, vulkan_worker=True), 'vulkan-jit')

    def test_bda_requires_explicit_route_capability(self):
        from nexus_workload_admission import vulkan_worker_available
        import hashlib
        from types import SimpleNamespace
        p = dict(DEFAULTS, workloads={CUDA_UUID: 'nexus_bda'})
        with self.assertRaisesRegex(ValueError, 'refusing CUDA fallback'):
            primary_backend(CUDA_UUID, p)
        self.assertEqual(primary_backend(CUDA_UUID, p, vulkan_worker=True), 'vulkan-bda')
        with tempfile.TemporaryDirectory() as root:
            worker=Path(root)/'worker';worker.write_bytes(b'fixture')
            sha=hashlib.sha256(b'fixture').hexdigest()
            caps=dict(schema_version=1, primary_vulkan=True, vulkan_helper=True)
            with patch('nexus_workload_admission.subprocess.run', return_value=SimpleNamespace(stdout=json.dumps(caps).encode())):
                self.assertFalse(vulkan_worker_available(worker, sha, require_bda=True))
                self.assertTrue(vulkan_worker_available(worker, sha))
            caps['primary_vulkan_bda']=True
            with patch('nexus_workload_admission.subprocess.run', return_value=SimpleNamespace(stdout=json.dumps(caps).encode())):
                self.assertTrue(vulkan_worker_available(worker, sha, require_bda=True))

    def test_invalid_workload_refused(self):
        with self.assertRaises(ValueError):
            primary_backend(CUDA_UUID,dict(DEFAULTS,workloads={CUDA_UUID:'unknown'}))

    def test_missing_preferences_are_defaults(self):
        with tempfile.TemporaryDirectory() as root:
            self.assertEqual(primary_backend(CUDA_UUID,read_preferences(Path(root)/'absent')),'cuda-driver')

    def test_corrupt_or_public_preferences_fail_closed(self):
        with tempfile.TemporaryDirectory() as root:
            path=Path(root)/'preferences.json';path.touch(mode=0o600)
            path.write_text('{')
            with self.assertRaises(ValueError):read_preferences(path)
            path.write_text(json.dumps(DEFAULTS));path.chmod(0o644)
            with self.assertRaises(ValueError):read_preferences(path)

    def test_symlink_is_refused(self):
        with tempfile.TemporaryDirectory() as root:
            path=Path(root)/'preferences.json';path.touch(mode=0o600)
            path.write_text(json.dumps(DEFAULTS));link=Path(root)/'link';link.symlink_to(path)
            with self.assertRaises(OSError):read_preferences(link)

    def test_both_source_entrypoints_gate_before_worker_start(self):
        root=Path(__file__).resolve().parents[1]
        for file in ('nexus-open.fish','nexusmine-next.fish'):
            text=(root/'scripts'/file).read_text()
            self.assertIn('nexus_workload_admission.py',text)
            boundary='systemctl --user start nexus-v002-production.service' if file=='nexus-open.fish' else 'set -gx NEXUS_POM_BACKEND "$admitted_backend"'
            self.assertLess(text.index('nexus_workload_admission.py'),text.index(boundary))

    def test_ui_exposes_requested_but_unadmitted_vulkan(self):
        import nexus_dashboard
        from nexus_presenter import build_presenter_frame
        model=nexus_dashboard.DashboardModel();model.fixture_mode=True
        model.gpu_devices={'cuda_primary':{'id':CUDA_UUID,'name':'Fixture NVIDIA','vendor':'NVIDIA'},
                           'amd_devices':[{'id':'pci:0000:00:00.0','name':'Fixture AMD','vendor':'AMD',
                                           'driver':'amdgpu','mode':'disabled','capabilities_status':'N/A'}]}
        frame=build_presenter_frame(46,160,model,'UNKNOWN',1,
            preferences=dict(DEFAULTS,workloads={CUDA_UUID:'nexus'}))
        rows=[row for panel in frame.panels for row in panel['rows']]
        self.assertTrue(any(row[0]=='Vulkan admission' and row[1].startswith('BLOCKED') and row[2]=='warning' for row in rows))

    def test_saving_vulkan_does_not_claim_activation(self):
        import nexus_tauri_actions as actions
        import nexus_ui_preferences as prefs
        from types import SimpleNamespace
        with patch.object(prefs,'save') as save, \
             patch.object(actions,'production_source',return_value=actions.SOURCE), \
             patch.object(actions.subprocess,'run',return_value=SimpleNamespace(
                 returncode=0, stdout='Configuration queued; guarded restart requested', stderr='')) as run:
            result=actions.action('workload',{'device':CUDA_UUID,'workload':'nexus'},True)
            self.assertIn('queued',result['message'])
            self.assertIn('request_change',run.call_args.args[0][-1])
            self.assertEqual(json.loads(run.call_args.kwargs['input'])['value']['workload'],'nexus')
            save.assert_not_called()  # The desktop must not change a running worker's files.

    def test_vulkan_backend_survives_dashboard_attach_and_rejects_unknown(self):
        from nexus_dashboard import DashboardModel
        from nexus_dashboard_test import DashboardTests
        model = DashboardModel()
        snapshot = DashboardTests.native_snapshot()
        for backend in ('vulkan-jit', 'vulkan-bda'):
            snapshot['state']['pom_backend'] = backend
            model.ingest(json.dumps(snapshot))
            self.assertEqual(model.native_status(), 'LIVE')
            self.assertEqual(model.pom_backend['backend'], backend)
            snapshot['monotonic_ns'] += 1
        snapshot['monotonic_ns'] += 1
        snapshot['state']['pom_backend'] = 'invented'
        model.ingest(json.dumps(snapshot))
        self.assertEqual(model.native_health, 'INVALID')
        self.assertEqual(model.pom_backend['backend'], 'vulkan-bda')

    def test_vulkan_ai_on_nvidia_is_not_attributed_to_amd(self):
        from nexus_dashboard import DashboardModel
        from nexus_gpu_selection import application_status
        model = DashboardModel()
        event = dict(event='AI_BACKEND_SELECTED', schema_version=1,
                     backend='vulkan', device_id=CUDA_UUID, trace_capture=False)
        model.ingest(json.dumps(event))
        self.assertEqual(model.ai_backend['device_id'], CUDA_UUID)
        # Device identity, not the Vulkan backend label, decides which OFF
        # device is still draining inference.
        runtime = dict(nvidia_enabled=False, nvidia_applied=False,
                       amd_enabled=False, amd_applied=False)
        result = application_status({'nvidia_pom':False, 'amd_pom':False},
                                    'amd_pom', runtime, True, model.ai_backend)
        self.assertNotIn('inference', result.lower())

    def test_single_gpu_vulkan_view_reports_actual_backend(self):
        from nexus_dashboard import DashboardModel
        from nexus_dashboard_test import DashboardTests
        from nexus_presenter import build_presenter_frame
        model = DashboardModel(); model.fixture_mode = True
        model.gpu_devices = {'cuda_primary': {'id': CUDA_UUID, 'name': 'Fixture NVIDIA', 'vendor': 'NVIDIA'}}
        snapshot = DashboardTests.native_snapshot()
        snapshot['state']['pom_backend'] = 'vulkan-jit'
        model.ingest(json.dumps(snapshot))
        frame = build_presenter_frame(46, 160, model, 'UNKNOWN', 1, preferences=dict(DEFAULTS))
        titles = ' '.join(panel['title'] for panel in frame.panels)
        self.assertIn('JIT / VULKAN', titles)
        self.assertNotIn('CUDA DRIVER API', titles)


if __name__=='__main__':unittest.main()
