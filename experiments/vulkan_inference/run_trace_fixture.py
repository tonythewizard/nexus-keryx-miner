#!/usr/bin/env python3
"""One isolated offline capture, with node and host-swap protection. Never restarts a node."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time

MIB = 1024 * 1024


def process_memory(text):
    result = {key: int(value.split()[0]) * 1024
              for key, value in (line.split(':', 1) for line in text.splitlines() if ':' in line)
              if key in ('VmRSS', 'VmSwap')}
    return result if set(result) == {'VmRSS', 'VmSwap'} else None


def host_memory(text):
    data = {line.split(':')[0]: int(line.split()[1]) * 1024 for line in text.splitlines()}
    return {'available': data['MemAvailable'], 'swap_used': data['SwapTotal'] - data['SwapFree'],
            'swap_total': data['SwapTotal'], 'oomd_swap_limit': data['SwapTotal'] * 90 // 100}


def stop_reason(node, initial, host, events, current, paging, bad_rpc, elapsed):
    if node is None: return 'node disappeared or process identity/status unavailable'
    if host['oomd_swap_limit'] - host['swap_used'] < 512 * MIB: return 'host swap approaching observed oomd limit'
    if events.get('oom', 0) or events.get('oom_kill', 0): return 'cgroup OOM event'
    if host['available'] < 512 * MIB: return 'host emergency memory reserve'
    if paging >= 4: return 'sustained node paging during fixture'
    if bad_rpc >= 3: return 'node RPC or synchronization degraded'
    if current > 1400 * MIB: return 'harness approaching memory ceiling'
    if elapsed > 180: return 'bounded fixture deadline'
    return None


def run(args):
    for key in ('TMPDIR', 'TMP', 'TEMP'):
        p = Path(os.environ.get(key, ''))
        if not p.is_absolute() or not p.is_dir() or p.resolve() == Path('/tmp') or Path('/tmp') in p.resolve().parents:
            raise ValueError('Persistent temporary directories required')
    root = args.output.resolve()
    for name in ('executable', 'model', 'manifest', 'output', 'node_probe'):
        path = getattr(args, name).resolve()
        if path == Path('/tmp') or Path('/tmp') in path.parents: raise ValueError('Persistent fixture paths required')
    if not args.output.is_absolute() or root.exists(): raise ValueError('New absolute output directory required')
    with args.executable.open('rb') as stream:
        if hashlib.file_digest(stream, 'sha256').hexdigest() != args.sha256: raise ValueError('Fixture binary hash mismatch')
    cg = Path('/sys/fs/cgroup') / next(line.split('::')[1] for line in Path('/proc/self/cgroup').read_text().splitlines() if '::' in line).lstrip('/')
    limit = (cg / 'memory.max').read_text().strip()
    if not cg.name.startswith('nexus-dual-') or not cg.name.endswith('.service') or limit == 'max' or int(limit) > 1536 * MIB or (cg / 'memory.swap.max').read_text().strip() != '0':
        raise ValueError('Dedicated delegated nexus-dual cgroup with bounded RAM and zero swap required')
    nodes = []
    for p in Path('/proc').iterdir():
        if not p.name.isdigit(): continue
        try:
            if p.joinpath('exe').resolve().name.startswith('keryxd-'): nodes.append(p)
        except OSError: pass
    if len(nodes) != 1: raise ValueError('Exactly one existing keryxd required')
    node = nodes[0]
    identity = node.joinpath('stat').read_text().rsplit(')', 1)[1].split()[19]

    def node_state():
        try:
            if node.joinpath('stat').read_text().rsplit(')', 1)[1].split()[19] != identity: return None
            return process_memory(node.joinpath('status').read_text())
        except (OSError, ValueError, IndexError): return None

    def synced():
        try:
            p = subprocess.run([str(args.node_probe), '--node', 'grpc://127.0.0.1:22110'], capture_output=True, text=True, timeout=3)
            return p.returncode == 0 and 'network=mainnet' in p.stdout and 'synced=true' in p.stdout
        except (OSError, subprocess.TimeoutExpired): return False

    initial = node_state()
    if initial is None or not synced(): raise ValueError('Verified synchronized node required')
    root.mkdir(parents=True)
    host = host_memory(Path('/proc/meminfo').read_text())
    required = 1151770624 + 128 * MIB + 512 * MIB
    if min(host['available'], host['oomd_swap_limit'] - host['swap_used']) < required:
        result = {'status': 'BLOCKED', 'gpu_started': False, 'required_bytes': required, 'host': host}
        (root / 'result.json').write_text(json.dumps(result, indent=2) + '\n'); return 6
    env = os.environ.copy()
    env.update(VK_DRIVER_FILES='/usr/share/vulkan/icd.d/radeon_icd.json', VK_ICD_FILENAMES='/usr/share/vulkan/icd.d/radeon_icd.json',
               VK_LOADER_LAYERS_DISABLE='~implicit~', CUDA_VISIBLE_DEVICES='', NEXUS_VK_Q8_PROBE_SOFTWARE_DOT='1',
               GGML_VK_FORCE_MMVQ='1', MESA_SHADER_CACHE_DIR=str(root / 'mesa-cache'), MESA_SHADER_CACHE_MAX_SIZE='128M')
    command = [str(args.executable), str(args.model), str(args.manifest), str(root / 'captures')]
    if args.prefix:
        # Public deterministic request 0x41*32, official tier-0 table maps byte 0x41 to token 32.
        token_file = root / 'prefix.tokens'; token_file.write_bytes((32).to_bytes(4, 'little') * 32); command.append(str(token_file))
    reason = None; child = None; signalled = False; interrupted = False
    def interrupt(_signal, _frame):
        nonlocal interrupted
        interrupted = True
    signal.signal(signal.SIGINT, interrupt); signal.signal(signal.SIGTERM, interrupt)
    start = time.monotonic(); next_rpc = 0; bad_rpc = 0; paging = 0
    with (root / 'probe.log').open('w') as log, (root / 'health.jsonl').open('w') as health:
        try:
            child = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, env=env)
            while child.poll() is None:
                elapsed = time.monotonic() - start; state = node_state(); host = host_memory(Path('/proc/meminfo').read_text())
                events = {k: int(v) for k, v in (line.split() for line in (cg / 'memory.events').read_text().splitlines())}
                current = int((cg / 'memory.current').read_text())
                sample = {'elapsed_s': elapsed, 'node': state, 'host': host, 'memory_current': current, 'memory_events': events,
                          'memory_stat': {k: int(v) for k, v in (line.split() for line in (cg / 'memory.stat').read_text().splitlines()) if k in ('anon', 'file', 'shmem', 'kernel')},
                          'host_psi': Path('/proc/pressure/memory').read_text(), 'cgroup_psi': (cg / 'memory.pressure').read_text()}
                for name in ('vram', 'gtt'):
                    sample['amd_' + name] = int(Path('/sys/bus/pci/devices/0000:00:00.0/mem_info_' + name + '_used').read_text())
                if elapsed >= next_rpc:
                    okay = synced(); sample['rpc_ok'] = okay; bad_rpc = 0 if okay else bad_rpc + 1; next_rpc = elapsed + 2
                paging = paging + 1 if state is not None and state['VmSwap'] - initial['VmSwap'] > 256 * MIB and initial['VmRSS'] - state['VmRSS'] > 256 * MIB else 0
                if reason is None:
                    reason = 'operator interrupted fixture' if interrupted else stop_reason(state, initial, host, events, current, paging, bad_rpc, elapsed)
                if reason and not signalled:
                    try: child.send_signal(signal.SIGINT)
                    except ProcessLookupError: pass
                    signalled = True; sample['cooperative_stop'] = reason
                health.write(json.dumps(sample) + '\n'); health.flush(); time.sleep(.5)
        except Exception as error:
            reason = 'supervisor error: ' + str(error)
        finally:
            if child is not None and child.poll() is None:
                if not signalled:
                    try: child.send_signal(signal.SIGINT)
                    except ProcessLookupError: pass
                child.wait()  # Never SIGKILL, never signal the node or restart a workload.
    rc = child.returncode if child is not None else -1
    node_final = node_state(); node_synced = synced() if node_final is not None else False
    result = {'status': 'PASS' if rc == 0 and reason is None and node_synced else 'FAIL', 'exit_code': rc,
              'stop_reason': reason, 'node_pid': int(node.name), 'node_starttime': identity,
              'node_initial': initial, 'node_final': node_final, 'node_synced_after': node_synced,
              'elapsed_s': time.monotonic() - start, 'memory_peak': int((cg / 'memory.peak').read_text()),
              'memory_events': (cg / 'memory.events').read_text(), 'executable_sha256': args.sha256,
              'opening_verifier_ran': False, 'mainnet_transactions': 0}
    (root / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return 0 if result['status'] == 'PASS' else 5


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('executable', 'model', 'manifest', 'output', 'node_probe'): parser.add_argument(name, type=Path)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--prefix', action='store_true')
    raise SystemExit(run(parser.parse_args()))
