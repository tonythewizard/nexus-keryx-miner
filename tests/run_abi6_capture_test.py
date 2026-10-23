#!/usr/bin/env python3
"""Run one offline real-GPU ABI6 capture and Rust-selected opening verification.

Requires exclusive GPU admission and either a fresh complete operational audit
or explicit offline-only admission, which never authorizes node maintenance. This
runner never stops/starts production, changes node state or submits transactions.
Run under an explicitly budgeted memory cgroup. Logs/captures stay private.
"""
import argparse
import json
import os
from pathlib import Path
import queue
import signal
import subprocess
import threading
import time

UUID = 'GPU-00000000-0000-0000-0000-000000000000'


def admit(audit):
    admit_audit(audit)
    admit_exclusive()


def admit_audit(audit):
    if time.time() - audit.stat().st_mtime > 60:
        raise ValueError('Operational audit must be less than 60 seconds old')
    summary = None
    with audit.open() as f:
        for line in f:
            event = json.loads(line)
            if event.get('event') == 'AUDIT_SUMMARY':
                summary = event
    if not summary or summary.get('complete') is not True or summary.get('live_watermark_complete') is not True:
        raise ValueError('Operational audit coverage is incomplete')
    if summary.get('assigned_open') != 0 or summary.get('journal_ambiguous') != 0:
        raise ValueError('Unresolved AI obligations prevent exclusive GPU admission')
    if summary.get('pending_class') != 'AI_PENDING_NONE_VERIFIED':
        raise ValueError('Operational audit is indeterminate')


def admit_exclusive():
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            executable = (entry / 'exe').resolve(strict=True).name.lower()
        except (OSError, RuntimeError):
            continue
        if executable.startswith(('nexus-live-cuda', 'nexus-abi6-capture', 'abi6-capture-probe', 'keryx-miner', 'keryxmine')):
            raise ValueError(f'Exclusive GPU admission refused: existing miner/test PID {entry.name}')
    if os.environ.get('CUDA_VISIBLE_DEVICES') != UUID:
        raise ValueError('Select only the authorized GPU UUID')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('probe', 'oracle', 'library', 'model', 'tree', 'manifest', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    admission = parser.add_mutually_exclusive_group(required=True)
    admission.add_argument('--audit', type=Path)
    admission.add_argument('--offline-no-node-maintenance', action='store_true',
                           help='Isolated fixture only; historical receipts remain unresolved; no node/service control')
    a = parser.parse_args()
    for key in ('TMPDIR', 'TMP', 'TEMP'):
        path = Path(os.environ.get(key, ''))
        if not path.is_absolute() or not path.is_dir() or path.resolve() == Path('/tmp') or Path('/tmp') in path.resolve().parents:
            raise ValueError('Explicit persistent temporary paths required')
    if not a.output.is_absolute() or a.output.exists() or Path('/tmp') in a.output.resolve().parents:
        raise ValueError('New persistent output directory required')
    if a.offline_no_node_maintenance:
        admit_exclusive()
    else:
        admit(a.audit)
    os.umask(0o077)
    a.output.mkdir(mode=0o700)
    # The official automatic context floor; no smaller replacement model/runtime.
    env = dict(os.environ, KERYX_LLAMA_CTX='8192')
    events = queue.Queue(maxsize=128)
    process = None
    result = {'gpu': UUID, 'submissions': 'NONE', 'result': 'FAIL', 'test': 'real_abi6_capture_replay',
              'admission': 'OFFLINE_NO_NODE_MAINTENANCE' if a.offline_no_node_maintenance else 'COMPLETE_AUDIT',
              'historical_receipts': 'UNCHANGED_NOT_VALIDATED', 'node_control': 'NONE'}
    try:
        with (a.output / 'native.stderr.log').open('w') as err, (a.output / 'native.stdout.log').open('w') as log:
            process = subprocess.Popen([str(a.probe), str(a.library), str(a.model), str(a.manifest),
                                        str(a.output / 'capture')], stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=err, text=True, env=env)

            def drain():
                try:
                    for line in iter(process.stdout.readline, ''):
                        if len(line) > 8192:
                            events.put('INVALID_NATIVE_OUTPUT'); break
                        log.write(line); log.flush(); events.put(line)
                finally:
                    events.put(None)

            reader = threading.Thread(target=drain, daemon=True); reader.start()
            deadline = time.monotonic() + 180
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError('ABI6 generation did not complete within 180 seconds')
                try:
                    line = events.get(timeout=min(1, remaining))
                except queue.Empty:
                    continue
                if line is None or line == 'INVALID_NATIVE_OUTPUT':
                    raise RuntimeError('Native capture failed before READY; inspect native.stderr.log')
                if line.startswith('ABI6_CAPTURE_READY '):
                    break
            with (a.output / 'rust.log').open('w') as rust:
                subprocess.run([str(a.oracle), 'draw', str(a.output / 'capture')], check=True,
                               stdout=rust, stderr=subprocess.STDOUT, timeout=30, env=env)
                draws = (a.output / 'capture/draws.txt').read_text()
                if len(draws.splitlines()) != 4:
                    raise ValueError('Expected exactly four official draw points')
                process.stdin.write(draws); process.stdin.close()
                if process.wait(timeout=180) != 0:
                    raise RuntimeError('Native replay failed; inspect native.stderr.log')
                reader.join(timeout=5)
                subprocess.run([str(a.oracle), 'verify', str(a.output / 'capture'), str(a.model), str(a.tree)],
                               check=True, stdout=rust, stderr=subprocess.STDOUT, timeout=180, env=env)
                # Negative check: a changed real replay must not verify.
                x = a.output / 'capture/0.x'; original = x.read_bytes()
                if not original:
                    raise RuntimeError('Empty replay')
                try:
                    x.write_bytes(bytes([original[0] ^ 1]) + original[1:])
                    bad = subprocess.run([str(a.oracle), 'verify', str(a.output / 'capture'), str(a.model), str(a.tree)],
                                         stdout=rust, stderr=subprocess.STDOUT, timeout=180, env=env)
                    if bad.returncode == 0:
                        raise RuntimeError('Tampered native replay was accepted')
                finally:
                    x.write_bytes(original)
        result['result'] = 'PASS'
        result['tampered_replay'] = 'REJECTED'
    except BaseException as exc:
        result['error'] = str(exc) or type(exc).__name__
        raise
    finally:
        if process is not None and process.poll() is None:
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                # Do not escalate or leave an automatic retry loop.
                result['cleanup'] = 'BLOCKED native child still alive after SIGINT'
        (a.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
