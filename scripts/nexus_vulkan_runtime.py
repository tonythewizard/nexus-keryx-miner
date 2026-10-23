"""Preparatory worker-side Vulkan module interface; offline workloads only.

Compilation remains JIT from CUDA source. This module does not load a model,
create a Vulkan device, execute kernels or submit network data. A future
validated dispatch backend can consume the prepared module within its lease.
"""
from contextlib import contextmanager
import importlib.util
import json
import os
from pathlib import Path

from nexus_gpu_devices import ROOT, offline_lease

SOURCE = Path(__file__).resolve().parent.parent


class OfflineVulkanSession:
    def __init__(self, device):
        self.device = device
        self.active = True

    def prepare_module(self, source, entry, local_size=128, *, bounded_indices_proven=False, banked_model_tiles=False):
        if not self.active:
            raise ValueError('GPU session has ended')
        if not bounded_indices_proven:
            raise ValueError('The host adapter must prove buffer indexing bounds')
        maximum = self.device.get('capabilities', {}).get('max_workgroup_invocations')
        if type(maximum) is not int or type(local_size) is not int or not 1 <= local_size <= maximum:
            raise ValueError('Local size exceeds the selected device capability')
        path = Path(source).resolve()
        if not path.is_relative_to(SOURCE / 'consensus') or path.suffix != '.cu':
            raise ValueError('JIT input must be trusted CUDA source from this checkout')
        directory = SOURCE / 'experiments/vulkan_jit'
        # Load this checkout's compiler, independent of the caller's working
        # directory and without process-global import-path mutation.
        spec = importlib.util.spec_from_file_location('nexus_cuda_vulkan_translator', directory / 'translate.py')
        if spec is None or spec.loader is None:
            raise ValueError('Runtime CUDA translator is unavailable')
        translator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(translator)
        origin = None
        if banked_model_tiles:
            if path != SOURCE / 'consensus/cuda_portable_resident.cu' or entry != 'pom_v4_resident_walk_kernel':
                raise ValueError('Banked storage requires the canonical resident CUDA source')
            adapter_spec = importlib.util.spec_from_file_location('nexus_banked_source', directory / 'banked_source.py')
            adapter = importlib.util.module_from_spec(adapter_spec)
            adapter_spec.loader.exec_module(adapter)
            path, origin = adapter.generate(path, ROOT / 'vulkan-jit/cache')
            entry = 'pom_v4_banked_resident_walk_kernel'
        result = translator.compile_kernel(path, entry, ROOT / 'vulkan-jit/cache', local_size, True)
        if origin: result['origin'] = origin
        reflection = json.loads(Path(result['manifest']).read_text()).get('reflection', {})
        shared = reflection.get('shared_memory_bytes', 0)
        limit = self.device.get('capabilities', {}).get('shared_memory_bytes')
        if type(limit) is not int or type(shared) is not int or not 0 <= shared <= limit:
            raise ValueError('Generated module exceeds the selected device shared-memory limit')
        return {**result, 'device_id': self.device['id'], 'backend': 'vulkan-jit',
                'production_ready': False, 'dispatch': 'NOT_STARTED'}

    def environment(self):
        if not self.active:
            raise ValueError('GPU session has ended')
        scratch = ROOT / 'work/tmp'
        return dict(os.environ, VK_DRIVER_FILES='/usr/share/vulkan/icd.d/radeon_icd.json',
                    VK_ICD_FILENAMES='/usr/share/vulkan/icd.d/radeon_icd.json',
                    VK_LOADER_LAYERS_DISABLE='~implicit~', NEXUS_VULKAN_DEVICE_ID=self.device['id'],
                    TMPDIR=str(scratch), TMP=str(scratch), TEMP=str(scratch))


@contextmanager
def session(device_id, workload):
    with offline_lease(device_id, workload) as device:
        value = OfflineVulkanSession(device)
        try:
            yield value
        finally:
            value.active = False
