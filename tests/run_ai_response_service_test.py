#!/usr/bin/env python3
import os
import pathlib
import subprocess
import sys
import hashlib
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs

binary, cargo, manifest, cargo_home, target_dir, root, *extra = sys.argv[1:]
fixture_tool = None
if '--fixture-tool' in extra:
    index = extra.index('--fixture-tool')
    if index + 1 >= len(extra):
        raise SystemExit('missing --fixture-tool executable')
    fixture_tool = pathlib.Path(extra[index + 1]).resolve(strict=True)
    del extra[index:index + 2]
root = pathlib.Path(root).resolve()
if str(root).startswith('/tmp/') or root == pathlib.Path('/tmp'):
    raise SystemExit('test root must be persistent and outside /tmp')
root.mkdir(parents=True, exist_ok=True)
# Keep previous proof/journal artifacts as durable acceptance evidence. Each
# invocation owns a fresh child directory rather than deleting the shared root.
root = root / f'run-{time.time_ns()}-{os.getpid()}'
root.mkdir(parents=True)
tmp = root / 'tmp'
tmp.mkdir()
env = os.environ.copy()
env.update(CARGO_HOME=cargo_home, CARGO_TARGET_DIR=target_dir, CARGO_BUILD_JOBS='1',
           TMPDIR=str(tmp), TMP=str(tmp), TEMP=str(tmp))
if '--pom-cuda' in extra:
    authorized = 'GPU-00000000-0000-0000-0000-000000000000'
    if env.get('CUDA_VISIBLE_DEVICES') != authorized:
        raise SystemExit('CUDA integration requires only the authorized primary NVIDIA GPU UUID')
    from cuda_test_isolation import assert_no_gpu_miner
    try:
        assert_no_gpu_miner()
    except RuntimeError as error:
        raise SystemExit(str(error)) from error
    pom_index = extra.index('--pom-cuda')
    if len(extra) < pom_index + 7:
        raise SystemExit('incomplete PoM CUDA fixture arguments')
    proof_dir = root / 'proofs'
    proof_dir.mkdir()
    extra[pom_index + 6] = str(proof_dir)
model = 'bd34568cd89f5f19c6c3a6e1a61b929bc868709409eaad8e672d85f3c1eb5710'
for kind, name in [('public', 'public.req'), ('private', 'private.req')]:
    generator = ([str(fixture_tool)] if fixture_tool else
                 [cargo, 'run', '--quiet', '--release', '--locked', '--manifest-path', manifest,
                  '--bin', 'fixture', '--'])
    subprocess.run([*generator, kind, model, 'official prompt: answer briefly', str(root / name)],
                   env=env, check=True, timeout=120)
class IpfsMock(BaseHTTPRequestHandler):
    blobs = {}
    root_blocks = {}
    protocol_version = 'HTTP/1.1'

    @staticmethod
    def varint(n):
        out = bytearray()
        while n >= 128:
            out.append((n & 127) | 128)
            n >>= 7
        return bytes(out) + bytes([n])
    alphabet = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'

    @classmethod
    def b58(cls, raw):
        zeroes = len(raw) - len(raw.lstrip(b'\0'))
        value = int.from_bytes(raw, 'big')
        out = ''
        while value:
            value, rem = divmod(value, 58)
            out = cls.alphabet[rem] + out
        return '1' * zeroes + (out or ('1' if not zeroes else ''))

    def do_POST(self):
        parsed = urlparse(self.path)
        body = self.rfile.read(int(self.headers.get('Content-Length', '0')))
        if parsed.path.endswith('/add'):
            boundary = self.headers.get('Content-Type', '').split('boundary=', 1)[-1].strip('"')
            marker = b'--' + boundary.encode()
            part = next((p for p in body.split(marker) if b'name="file"' in p), None)
            if part is None or b'\r\n\r\n' not in part:
                self.send_error(400); return
            payload = part.split(b'\r\n\r\n', 1)[1].rsplit(b'\r\n', 1)[0]
            unixfs = b'\x08\x02\x12' + self.varint(len(payload)) + payload + b'\x18' + self.varint(len(payload))
            root = b'\x0a' + self.varint(len(unixfs)) + unixfs
            cid = self.b58(b'\x12\x20' + hashlib.sha256(root).digest())
            self.blobs[cid] = payload
            self.root_blocks[cid] = root
            response = json.dumps({'Hash': cid}).encode()
        elif parsed.path.endswith('/block/get'):
            cid = parse_qs(parsed.query).get('arg', [''])[0]
            if cid not in self.root_blocks:
                self.send_error(404); return
            response = self.root_blocks[cid]
        elif parsed.path.endswith('/cat'):
            cid = parse_qs(parsed.query).get('arg', [''])[0]
            if cid not in self.blobs:
                self.send_error(404); return
            response = self.blobs[cid]
        else:
            self.send_error(404); return
        self.send_response(200)
        self.send_header('Content-Length', str(len(response)))
        self.end_headers()
        self.wfile.write(response)

    def log_message(self, *_args):
        pass

server = HTTPServer(('127.0.0.1', 0), IpfsMock)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    api = f'http://127.0.0.1:{server.server_port}'
    # The stock wrapper is built against CUDA 12 and carries RUNPATH=$ORIGIN.
    # PoM must remain Driver-API-only so the process contains one CUDA Runtime
    # major after the wrapper is loaded.
    if '--real-inference' in extra:
        inference_index = extra.index('--real-inference')
        if len(extra) < inference_index + 3:
            raise SystemExit('incomplete real-inference arguments')
        llama_path = pathlib.Path(extra[inference_index + 2]).resolve(strict=True)
        runtime_dir = llama_path.parent
        required_cuda12 = ('libcudart.so.12', 'libcublas.so.12', 'libcublasLt.so.12')
        missing = [name for name in required_cuda12 if not (runtime_dir / name).is_file()]
        if missing:
            raise SystemExit('stock CUDA 12 runtime closure missing beside libkeryx-llama: ' + ', '.join(missing))
        extra[inference_index + 2] = str(llama_path)
        # The wrapper carries a $ORIGIN RUNPATH to its adjacent CUDA 12
        # runtime. Keep resolution in the ELF's own verified closure; do not
        # mutate LD_LIBRARY_PATH or use a dynamic-linker workaround.
        # Trace the loader's ELF closure before any CUDA test work. `ldd -r`
        # resolves dependencies and relocations but does not call CUDA APIs.
        for elf in (binary, str(llama_path)):
            closure = subprocess.run(['ldd', '-r', elf], env=env, text=True,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     check=False)
            if closure.returncode != 0 or 'not found' in closure.stdout or 'undefined symbol' in closure.stdout:
                raise SystemExit(f'ELF dependency closure is incomplete for {elf}:\n{closure.stdout}')
        llama_closure = subprocess.run(['ldd', str(llama_path)], env=env, text=True,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       check=False).stdout
        if f'libcudart.so.12 => {runtime_dir}/libcudart.so.12' not in llama_closure:
            raise SystemExit('CUDA 12 runtime did not resolve from the adjacent stock runtime directory')
        worker_closure = subprocess.run(['ldd', binary], env=env, text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        check=False).stdout
        if 'libcuda.so.1 =>' not in worker_closure:
            raise SystemExit('PoM Driver API dependency libcuda.so.1 is not resolved')
        if 'libcudart.so.12 =>' in worker_closure or 'libcudart.so.13 =>' in worker_closure:
            raise SystemExit('PoM executable still has a dynamic CUDA Runtime dependency; refusing mixed or unverified architecture')
        if 'libcudart.so.13 =>' in llama_closure:
            raise SystemExit('stock llama closure unexpectedly contains CUDA Runtime 13')
    # Keep native output even when an external safety monitor stops CTest.
    # CTest's captured pipe otherwise loses the current test's diagnostics.
    native_log = root / 'native.log'
    print(f'NATIVE_TEST_LOG={native_log}', flush=True)
    try:
        with native_log.open('wb') as output:
            subprocess.run([binary, str(root / 'public.req'), str(root / 'private.req'), str(root / 'journal'),
                            '--ipfs-api', api, *extra], env=env, check=True,
                           stdout=output, stderr=subprocess.STDOUT,
                           timeout=600 if extra else 30)
    finally:
        if native_log.exists():
            with native_log.open(errors='replace') as output:
                for line in output:
                    print(line, end='', flush=True)
finally:
    server.shutdown()
    server.server_close()
    thread.join(timeout=2)
