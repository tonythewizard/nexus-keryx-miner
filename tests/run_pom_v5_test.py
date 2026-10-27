#!/usr/bin/env python3
"""Explicit CPU/oracle or allocated-GPU v5 acceptance; never starts a node/miner."""
import argparse
import contextlib
import fcntl
import json
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('binary', type=Path)
parser.add_argument('oracle', type=Path)
parser.add_argument('root', type=Path)
mode = parser.add_mutually_exclusive_group()
mode.add_argument('--gpu', action='store_true')
mode.add_argument('--cpu-real', nargs=3, metavar=('MODEL', 'TREE', 'META'))
mode.add_argument('--gpu-real', nargs=3, metavar=('MODEL', 'TREE', 'META'))
args = parser.parse_args()
args.binary = args.binary.resolve(strict=True)
args.oracle = args.oracle.resolve(strict=True)
root = args.root.resolve()
if root == Path('/tmp') or Path('/tmp') in root.parents:
    raise SystemExit('persistent test root required')
root.mkdir(parents=True, exist_ok=True)
root = root / f'run-{time.time_ns()}'
root.mkdir(mode=0o700)
(root / 'tmp').mkdir()
env = dict(os.environ, TMPDIR=str(root / 'tmp'), TMP=str(root / 'tmp'), TEMP=str(root / 'tmp'))

def run(command, name, success=True, timeout=300):
    result = subprocess.run([str(x) for x in command], env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=timeout, check=False)
    (root / (name + '.log')).write_bytes(result.stdout)
    if (result.returncode == 0) != success:
        raise RuntimeError(f'{name}: unexpected status {result.returncode}; see {root / (name + ".log")}')
    print(f'{name}=PASS', flush=True)

@contextlib.contextmanager
def gpu_lease():
    from cuda_test_isolation import assert_no_gpu_miner, assert_only_display_contexts
    uuid = env.get('NEXUS_PRIMARY_GPU_UUID', '')
    if not re.fullmatch(r'GPU-[a-fA-F0-9]{8}(-[a-fA-F0-9]{4}){3}-[a-fA-F0-9]{12}', uuid):
        raise RuntimeError('explicit authorized NEXUS_PRIMARY_GPU_UUID required')
    if env.get('CUDA_VISIBLE_DEVICES') != uuid or uuid == 'GPU-' + '0' * 8 + ('-' + '0' * 4) * 3 + '-' + '0' * 12:
        raise RuntimeError('CUDA_VISIBLE_DEVICES must be exactly the authorized UUID')
    data = Path(env.get('NEXUS_DATA_DIR', ''))
    if not data.is_absolute() or not data.is_dir():
        raise RuntimeError('existing private NEXUS_DATA_DIR required for device lease')
    directory = data / 'state/gpu-devices'
    directory.mkdir(parents=True, exist_ok=True)
    fd = os.open(directory / f'lease-{uuid}.lock', os.O_CREAT | os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
            raise RuntimeError('unsafe device lease file')
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        assert_no_gpu_miner()
        apps = subprocess.run(['nvidia-smi', '--id=' + uuid, '--query-compute-apps=pid,process_name', '--format=csv,noheader,nounits'],
                              check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert_only_display_contexts(apps.stdout)
        yield
    finally:
        os.close(fd)

run([args.binary, '--cpu', root], 'cpp-cpu')
if args.cpu_real:
    run([args.binary, '--cpu-real', root, *[Path(p).resolve(strict=True) for p in args.cpu_real]], 'cpp-cpu-real')
if args.gpu or args.gpu_real:
    with gpu_lease():
        command = [args.binary, '--gpu-real' if args.gpu_real else '--gpu', root]
        if args.gpu_real:
            command += [Path(p).resolve(strict=True) for p in args.gpu_real]
        run(command, 'cpp-gpu-real' if args.gpu_real else 'cpp-gpu', timeout=600)

def verify_native_stream(candidate):
    raw = candidate.read_bytes()
    wire_len_pos = 189 + 257 * 1024 + 256 * 8
    wire = raw[wire_len_pos + 4:]
    assert len(wire) == struct.unpack_from('<I', raw, wire_len_pos)[0]
    context = (b'NXRF' + raw[85:93] + raw[69:77] + raw[4:5] + raw[37:69] +
               raw[125:157] + raw[93:125] + raw[157:181] + struct.pack('<Q', 1) +
               bytes(32) + wire[65:73] + wire[33:65] + struct.pack('<I', len(wire)))
    trace = b'NXST' + raw[85:93] + raw[165:173] + raw[181:wire_len_pos]
    assert len(context) == 225 and len(trace) == 28 + 257 * 1024 + 256 * 8
    def check(name, ctx, tr, success):
        frame = b'NXVR' + struct.pack('<IQIII', 1, 1, len(ctx), len(tr), len(wire)) + ctx + tr + wire
        result = subprocess.run([str(args.oracle), 'verify-real-stream'], input=frame,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, timeout=60)
        (root / (candidate.stem + '-stream-' + name + '.log')).write_bytes(result.stdout + result.stderr)
        if (result.returncode == 0) != success:
            raise RuntimeError('unexpected native verifier stream result: ' + name)
        if success and (not result.stdout.startswith(b'NXVA') or b'era=H15' not in result.stdout):
            raise RuntimeError('native verifier did not attest the H15 stream')
    check('honest', context, trace, True)
    for name, at in [('seed', 4), ('root', 21), ('tier', 20), ('timestamp', 117),
                     ('nonce', 125), ('final', 181), ('pow', 189)]:
        changed = bytearray(context); changed[at] ^= 1
        check('reject-' + name, changed, trace, False)
    changed = bytearray(context); struct.pack_into('<Q', changed, 133, 140114999)
    check('reject-era-downgrade', changed, trace, False)
    for name, at in [('initial-state', 28), ('final-state', 28 + 256 * 1024),
                     ('offset', 28 + 257 * 1024)]:
        changed = bytearray(trace); changed[at] ^= 1
        check('reject-' + name, context, changed, False)
    print(candidate.stem + '-native-stream=PASS mutations=11', flush=True)

accepted = 0
for candidate in sorted(root.glob('*.bin')):
    real = candidate.name.startswith('real-')
    run([args.oracle, 'verify-v5-real' if real else 'verify-v5-fixture', candidate], 'oracle-' + candidate.stem)
    if real:
        verify_native_stream(candidate)
    accepted += 1

source = bytearray((root / 'cpu-61.bin').read_bytes())
state_start = 189
offset_start = state_start + 257 * 1024
wire_len_pos = offset_start + 256 * 8
wire_start = wire_len_pos + 4
negatives = {}
for name, position in [('nonce', 165), ('root', 37), ('seed', 85), ('pad-size', 77),
                       ('initial-state', state_start), ('final-state', state_start + 256 * 1024),
                       ('offset', offset_start), ('weight-tile', wire_start + 97), ('pow-pin', wire_start + 33)]:
    changed = bytearray(source)
    changed[position] ^= 1
    negatives[name] = changed
changed = bytearray(source)
changed[125:157] = bytes(32)
negatives['target'] = changed
negatives['truncated'] = source[:-1]
negatives['trailing'] = source + b'\0'
# Add one actual payload byte at a pad position and adjust lengths. It must be
# rejected as a nonempty v5 pad witness, rather than just failing truncated Borsh.
pos = wire_start + 93
for i in range(256):
    size = struct.unpack_from('<I', source, pos)[0]
    if size == 0:
        changed = bytearray(source)
        struct.pack_into('<I', changed, pos, 1)
        changed[pos + 4:pos + 4] = b'\x01'
        old_len = struct.unpack_from('<I', source, wire_len_pos)[0]
        struct.pack_into('<I', changed, wire_len_pos, old_len + 1)
        negatives['nonempty-pad'] = changed
        break
    pos += 4 + size
else:
    raise RuntimeError('negative fixture contains no pad positions')

# Mutate a Merkle sibling while keeping every vector length and tile byte intact.
pos = wire_start + 93
for _ in range(256):
    size = struct.unpack_from('<I', source, pos)[0]
    pos += 4 + size
assert struct.unpack_from('<I', source, pos)[0] == 256
pos += 4
for _ in range(256):
    count = struct.unpack_from('<I', source, pos)[0]
    pos += 4
    if count:
        changed = bytearray(source)
        changed[pos] ^= 1
        negatives['merkle-sibling'] = changed
        break
    pos += 32 * count
else:
    raise RuntimeError('negative fixture contains no Merkle siblings')
changed = bytearray(source)
changed[4] ^= 1
negatives['tier-binding'] = changed

for name, data in negatives.items():
    candidate = root / ('negative-' + name + '.bin')
    candidate.write_bytes(data)
    run([args.oracle, 'verify-v5-fixture', candidate], 'reject-' + name, success=False)

result = {'accepted_artifacts': accepted, 'rejected_mutations': len(negatives),
          'gpu_execution': bool(args.gpu or args.gpu_real), 'real_model_gpu': bool(args.gpu_real), 'real_model_cpu': bool(args.cpu_real),
          'ai_handoff': 'GPU memory mock' if args.gpu or args.gpu_real else 'not executed',
          'live_submission': False, 'root': str(root)}
(root / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result), flush=True)
