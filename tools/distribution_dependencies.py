"""Inspect library imports without loading a miner or inference DLL."""
import re
import subprocess

WINDOWS_SYSTEM = {
    'kernel32.dll', 'kernelbase.dll', 'ntdll.dll', 'advapi32.dll', 'user32.dll',
    'gdi32.dll', 'shell32.dll', 'ole32.dll', 'oleaut32.dll', 'ws2_32.dll',
    'wsock32.dll', 'mswsock.dll', 'crypt32.dll', 'secur32.dll', 'bcrypt.dll',
    'bcryptprimitives.dll', 'userenv.dll', 'rpcrt4.dll', 'iphlpapi.dll',
    'netapi32.dll', 'version.dll', 'winmm.dll', 'dbghelp.dll', 'setupapi.dll',
    'cfgmgr32.dll', 'comdlg32.dll', 'msvcrt.dll', 'ucrtbase.dll', 'nvcuda.dll',
    'wintrust.dll', 'normaliz.dll', 'psapi.dll', 'shlwapi.dll', 'powrprof.dll',
}
LINUX_SYSTEM = {
    'libc.so.6', 'libm.so.6', 'libpthread.so.0', 'libdl.so.2', 'librt.so.1',
    'libstdc++.so.6', 'libgcc_s.so.1', 'libatomic.so.1', 'libresolv.so.2',
    'libutil.so.1', 'ld-linux-x86-64.so.2', 'libcuda.so.1',
}


def imports(output, platform):
    if platform == 'windows':
        return {line.strip().lower() for line in output.splitlines()
                if re.fullmatch(r'[A-Za-z0-9_.-]+\.dll', line.strip(), re.I)}
    return set(re.findall(r'Shared library: \[([^\]]+)\]', output))


def verify(directory, platform, inspector):
    available = {file.name.lower() if platform == 'windows' else file.name
                 for file in directory.iterdir() if file.is_file()}
    count = 0
    for binary in sorted(directory.iterdir()):
        if not binary.is_file():
            continue
        if platform == 'windows':
            if binary.suffix.lower() not in ('.dll', '.exe'):
                continue
            args = [str(inspector), '/nologo', '/dependents', str(binary)]
            system = WINDOWS_SYSTEM
        else:
            if '.so' not in binary.name and binary.name not in ('nexus-live-cuda-worker', 'nexus-m21-oracle'):
                continue
            args = [str(inspector), '-d', str(binary)]
            system = LINUX_SYSTEM
        output = subprocess.check_output(args, text=True, encoding='utf-8', errors='replace')
        needed = imports(output, platform)
        if not needed:
            raise ValueError(f'No library imports obtained from {binary.name}')
        missing = {name for name in needed - available - system
                   if not (platform == 'windows' and name.startswith(('api-ms-win-', 'ext-ms-win-')))}
        if missing:
            raise ValueError(f'Unbundled dependencies of {binary.name}: {", ".join(sorted(missing))}')
        count += 1
    if not count:
        raise ValueError('No native binaries inspected')
    return count
