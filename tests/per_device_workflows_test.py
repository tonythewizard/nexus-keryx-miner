"""Per-identity routing and admission. Fixtures only: no GPU, services or node calls."""
import sys
from pathlib import Path
import unittest
import tempfile
import os
from unittest.mock import patch
from types import SimpleNamespace
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from nexus_gpu_registry import workload_options, resolve_workload
from nexus_gpu_selection import CUDA_UUID, AMD_ID
import nexus_workload_admission as admission

class DeviceWorkflows(unittest.TestCase):
    caps=dict(schema_version=1,vulkan_helper=True,primary_vulkan=True,primary_vulkan_bda=True,
              secondary_vulkan_bda=True,independent_vulkan_lanes=True)
    def test_vendor_matrix_not_display_order(self):
        for vendor,expected in [('AMD',['auto','nexus','nexus_bda']),('Intel',['auto','nexus','nexus_bda']),
                                ('NVIDIA',['auto','nvidia','nexus','nexus_bda'])]:
            self.assertEqual([v for _,v in workload_options({'vendor':vendor})],expected)
        for vendor in ('AMD','Intel'):
            with self.assertRaises(ValueError):resolve_workload({'vendor':vendor},'nvidia')
    def test_inventory_excludes_hdmi_audio_functions(self):
        import nexus_gpu_devices as devices
        with tempfile.TemporaryDirectory(dir=os.environ['TMPDIR']) as directory:
            root=Path(directory)
            for name,vendor,code in [('0000:00:00.0','0x1002','0x030000'),
                                     ('0000:00:02.0','0x1002','0x040300'),
                                     ('0000:00:01.0','0x10de','0x030000')]:
                p=root/name;p.mkdir();(p/'vendor').write_text(vendor);(p/'class').write_text(code)
            with patch('nexus_gpu_identity.amd_name',return_value='Fixture AMD'):
                found=devices.amd_devices(root)
        self.assertEqual([d['id'] for d in found],['pci:0000:00:00.0'])

    def test_routes_are_independent(self):
        for primary in ('auto','nvidia','nexus','nexus_bda'):
            for secondary in ('auto','nexus','nexus_bda'):
                prefs={'workloads':{CUDA_UUID:primary,AMD_ID:secondary}}
                self.assertEqual(admission.device_backend({'id':AMD_ID,'vendor':'AMD'},prefs),
                                 'vulkan-bda' if secondary=='nexus_bda' else 'vulkan-jit')
                self.assertEqual(admission.device_backend({'id':CUDA_UUID,'vendor':'NVIDIA'},prefs),
                                 {'auto':'cuda-driver','nvidia':'cuda-driver','nexus':'vulkan-jit','nexus_bda':'vulkan-bda'}[primary])
    def test_mixed_vulkan_needs_explicit_worker_capability(self):
        with patch.object(admission,'worker_capabilities',return_value=dict(self.caps,independent_vulkan_lanes=False)):
            with self.assertRaisesRegex(ValueError,'independent Vulkan lanes'):
                admission.admit_routes(dict(nvidia_pom=True,amd_pom=True),{CUDA_UUID:'nexus'},'worker','sha')
    def test_both_vulkan_routes_can_be_admitted_without_cuda_fallback(self):
        with patch.object(admission,'worker_capabilities',return_value=self.caps):
            routes=admission.admit_routes(dict(nvidia_pom=True,amd_pom=True),{CUDA_UUID:'nexus'},'worker','sha')
        self.assertEqual(routes,{CUDA_UUID:'vulkan-jit',AMD_ID:'vulkan-jit'})
    def test_bda_hardware_rejection_is_preserved(self):
        with patch.object(admission,'worker_capabilities',return_value=self.caps), \
             patch.object(admission.subprocess,'run',return_value=SimpleNamespace(returncode=13,stdout=b'',stderr=b'BDA_UNSUPPORTED')):
            with self.assertRaisesRegex(ValueError,'BDA_UNSUPPORTED'):
                admission.admit_routes(dict(nvidia_pom=True,amd_pom=True),{AMD_ID:'nexus_bda'},'worker','sha')
    def test_bda_probe_is_device_scoped(self):
        with patch.object(admission,'worker_capabilities',return_value=self.caps), \
             patch.object(admission.subprocess,'run',return_value=SimpleNamespace(returncode=0,stdout=b'BDA_ROUTE_CAPABILITY=PASS\n',stderr=b'')) as run:
            routes=admission.admit_routes(dict(nvidia_pom=True,amd_pom=True),{AMD_ID:'nexus_bda'},'worker','sha')
        self.assertEqual(routes[AMD_ID],'vulkan-bda')
        self.assertEqual(run.call_args.args[0],['worker','--probe-bda-route',AMD_ID])
        self.assertTrue(run.call_args.kwargs['env']['VK_DRIVER_FILES'].endswith('/radeon_icd.json'))
    def test_off_primary_does_not_override_secondary_ai_route(self):
        with patch.object(admission,'worker_capabilities',return_value=self.caps),patch.object(admission.subprocess,'run') as run:
            routes=admission.admit_routes(dict(nvidia_pom=False,amd_pom=True),{CUDA_UUID:'nexus_bda'},'worker','sha')
        self.assertEqual(routes[CUDA_UUID],'cuda-driver');run.assert_not_called()
    def test_bda_cannot_be_selected_on_old_secondary_worker(self):
        with patch.object(admission,'worker_capabilities',return_value=dict(self.caps,secondary_vulkan_bda=False)):
            with self.assertRaisesRegex(ValueError,'selected BDA route'):
                admission.admit_routes(dict(nvidia_pom=True,amd_pom=True),{AMD_ID:'nexus_bda'},'worker','sha')

if __name__=='__main__':unittest.main(verbosity=2)
