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


def assert_only_display_contexts(process_lines, proc_root=Path('/proc')):
    """Admit only the verified system KDE display context from an NVML PID/name query.

    KWin may be non-dumpable, denying /proc/exe even to its owner. In that case
    require NVML's exact executable path plus matching proc owner and comm.
    Missing/raced identities and every other compute client remain refusals.
    """
    for value in process_lines.splitlines():
        if not value.strip():
            continue
        fields = [part.strip() for part in value.split(',')]
        if len(fields) != 2:
            raise RuntimeError('unrecognized CUDA compute process identity')
        pid, reported_path = fields
        if not pid.isascii() or not pid.isdigit() or int(pid) <= 0:
            raise RuntimeError('unrecognized CUDA compute process identity')
        if reported_path != '/usr/bin/kwin_wayland':
            raise RuntimeError('another CUDA compute process is using the allocated GPU')
        process = proc_root / pid
        try:
            if process.stat().st_uid != os.getuid() or (process / 'comm').read_text().strip() != 'kwin_wayland':
                raise RuntimeError('CUDA display process owner/name mismatch')
            try:
                executable = os.readlink(process / 'exe')
            except PermissionError:
                executable = reported_path  # trusted NVML query, corroborated above
        except OSError as error:
            raise RuntimeError('CUDA compute process identity cannot be verified') from error
        if executable != reported_path:
            raise RuntimeError('CUDA display executable differs from NVML identity')
