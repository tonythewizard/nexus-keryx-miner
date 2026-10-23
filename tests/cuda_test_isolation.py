"""Read-only process identity gate for exclusive GPU integration tests."""
import os
from pathlib import Path


def assert_no_gpu_miner(proc_root=Path('/proc')):
    for executable in proc_root.glob('[0-9]*/exe'):
        try:
            name = Path(os.readlink(executable)).name.removesuffix(' (deleted)')
            if name == 'nexus-live-cuda-worker':
                raise RuntimeError('an existing production NEXUS worker is present; refusing a second CUDA worker')
            if name == 'keryx-miner':
                raise RuntimeError('stock miner is active; refusing simultaneous CUDA test')
            if name == 'fish':
                arguments = (executable.parent / 'cmdline').read_bytes().split(b'\0')
                scripts = {'nexusmine.fish', 'nexusmine-next.fish', 'launcher.fish'}
                if b'live' in arguments and any(Path(os.fsdecode(a)).name in scripts for a in arguments[1:]):
                    raise RuntimeError('a live miner launcher is active; refusing simultaneous CUDA test')
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            # Processes can exit during this read-only snapshot. Kernel-owned
            # processes may also restrict their executable links.
            continue
