#!/usr/bin/env python3
"""Static ELF gate; it does not initialize a GPU or attest inference parity."""
import hashlib
from pathlib import Path
import re
import subprocess
import sys

from nexus_cuda_runtime_gate import dynamic_needed, resolved
from nexus_workload_admission import vulkan_worker_available


def validate(worker, helper, helper_sha, library, library_sha):
    with worker.open('rb') as f:
        worker_sha = hashlib.file_digest(f, 'sha256').hexdigest()
    if not vulkan_worker_available(worker, worker_sha):
        raise ValueError('Worker lacks the explicit primary Vulkan route')
    for path, pin in ((helper, helper_sha), (library, library_sha)):
        if not re.fullmatch('[0-9a-f]{64}', pin):
            raise ValueError('Invalid Vulkan artifact SHA-256 pin')
        with path.open('rb') as f:
            if hashlib.file_digest(f, 'sha256').hexdigest() != pin:
                raise ValueError('Vulkan artifact SHA-256 mismatch')
    for path in (worker, helper, library):
        _, needed = dynamic_needed(path)
        closure = resolved(path)
        # A hybrid worker can link the driver for its separately selectable
        # native route. The Vulkan helper/library must have no CUDA closure.
        banned = r'libcudart\.so|libcublas|libnvrtc'
        if path != worker:
            banned += r'|libcuda\.so'
        if re.search(banned, needed + '\n' + closure):
            raise ValueError('CUDA dependency in the Vulkan inference route')
    if 'libvulkan.so' not in resolved(library):
        raise ValueError('Vulkan inference library does not resolve Vulkan')


def main():
    if len(sys.argv) != 6:
        raise SystemExit('usage: nexus_vulkan_runtime_gate.py WORKER HELPER HELPER_SHA LIBRARY LIBRARY_SHA')
    try:
        validate(Path(sys.argv[1]).resolve(strict=True), Path(sys.argv[2]).resolve(strict=True),
                 sys.argv[3], Path(sys.argv[4]).resolve(strict=True), sys.argv[5])
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        print('VULKAN_RUNTIME_GATE=BLOCKED detail=' + str(exc), file=sys.stderr)
        return 13
    print('VULKAN_RUNTIME_GATE=PASS inference=VULKAN cuda_runtime=ABSENT scope=STATIC_ONLY')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
