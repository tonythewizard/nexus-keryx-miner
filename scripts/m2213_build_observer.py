#!/usr/bin/env python3
"""Read-only process/VM sampling around the unchanged sequential build guard.

No resource policy or signal handling is implemented here: the existing guard
owns the build process group and all shutdown decisions.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from check_persistent_env import validate
from m22_memory_budget import read_snapshot

ROOT = Path(os.environ.get("NEXUS_DATA_DIR", str(Path.home() / ".local/share/nexus-keryx-miner")))
WATCH = {'keryxd', 'ipfs', 'kwin_wayland', 'plasmashell', 'chromium', 'codex',
         'cargo', 'rustc', 'nvcc', 'cmake', 'ninja', 'cc1plus', 'cc1', 'c++',
         'g++', 'ld', 'ld.bfd', 'ld.lld', 'collect2', 'ptxas', 'cicc'}


def snapshot(phase):
    processes = []
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        try:
            name = (proc / 'comm').read_text().strip()
            if name not in WATCH:
                continue
            stat = (proc / 'stat').read_text().rsplit(')', 1)[1].split()
            item = {'pid': int(proc.name), 'name': name, 'ppid': int(stat[1]),
                    'pgid': int(stat[2]), 'starttime': int(stat[19]),
                    'major_faults': int(stat[9]), 'minor_faults': int(stat[7])}
            for line in (proc / 'status').read_text().splitlines():
                key, _, value = line.partition(':')
                if key in ('VmRSS', 'VmHWM', 'VmSwap', 'RssAnon', 'RssFile', 'RssShmem'):
                    item[key + '_kib'] = int(value.split()[0])
            try:
                for line in (proc / 'smaps_rollup').read_text().splitlines():
                    key, _, value = line.partition(':')
                    if key in ('Pss', 'Pss_Anon', 'Pss_File', 'Pss_Shmem', 'Swap', 'SwapPss'):
                        item[key + '_kib'] = int(value.split()[0])
            except OSError as exc:
                item['pss_unavailable'] = str(exc)
            try:
                item['io'] = (proc / 'io').read_text()
            except OSError as exc:
                item['io_unavailable'] = str(exc)
            item['cgroup'] = (proc / 'cgroup').read_text().strip()
            processes.append(item)
        except (OSError, ValueError, IndexError):
            continue  # A read-only observation of a process that exited is not a safety decision.
    groups = {}
    for item in processes:
        line = next((s for s in item['cgroup'].splitlines() if s.startswith('0::')), None)
        if line:
            group = line[3:]
            if group not in groups:
                values = {}
                for name in ('memory.current', 'memory.swap.current', 'memory.pressure', 'memory.stat'):
                    try:
                        values[name] = (Path('/sys/fs/cgroup') / group.lstrip('/') / name).read_text()
                    except OSError as exc:
                        values[name] = {'unavailable': str(exc)}
                groups[group] = values
    return {'phase': phase, 'monotonic': time.monotonic(), 'time_ns': time.time_ns(),
            'page_size': os.sysconf('SC_PAGE_SIZE'), 'host': read_snapshot(),
            'psi': Path('/proc/pressure/memory').read_text(),
            'processes': processes, 'cgroups': groups}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('command', nargs=argparse.REMAINDER)
    args = ap.parse_args()
    errors = validate(ROOT, os.environ)
    if errors:
        raise RuntimeError('; '.join(errors))
    if ROOT not in args.output.resolve().parents:
        raise RuntimeError('observer output must be in the persistent workspace')
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        raise RuntimeError('missing target build command')
    args.output.mkdir(parents=True, exist_ok=False)
    with (args.output / 'observations.jsonl').open('x') as data, (args.output / 'guard.log').open('x') as log:
        def sample(phase):
            row = snapshot(phase)
            data.write(json.dumps(row, sort_keys=True) + '\n')
            data.flush()
            return row
        for _ in range(3):
            sample('before')
            time.sleep(1)
        guarded = [sys.executable, str(ROOT / 'source/scripts/m22_build_guard.py'),
                   '--workspace', str(ROOT), '--', *command]
        (args.output / 'command.json').write_text(json.dumps(guarded, indent=2) + '\n')
        child = subprocess.Popen(guarded, stdout=log, stderr=subprocess.STDOUT)
        while child.poll() is None:
            sample('during')
            time.sleep(0.5)
        for _ in range(3):
            sample('after')
            time.sleep(1)
        rc = child.wait()
        (args.output / 'exit.json').write_text(json.dumps({'exit_code': rc}) + '\n')
    print((args.output / 'guard.log').read_text())
    print(f'M2213_OBSERVED_BUILD_EXIT={rc} evidence={args.output}')
    return rc


if __name__ == '__main__':
    sys.exit(main())
