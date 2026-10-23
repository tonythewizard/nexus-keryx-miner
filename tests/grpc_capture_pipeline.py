#!/usr/bin/env python3
"""Offline TLS gRPC replay of an authentic captured mainnet template."""
import os
import pathlib
import re
import os
import select
import signal
import subprocess
import sys
import time

client, mock, replay, source_capture, root = sys.argv[1:]
root = pathlib.Path(root)
root.mkdir(parents=True, exist_ok=True)
source_capture = pathlib.Path(source_capture)
cert, key = root / "cert.pem", root / "key.pem"
captured, server_log = root / "grpc-captured.capture", root / "mock-server.log"
for path in (captured,):
    path.unlink(missing_ok=True)
subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-keyout", str(key), "-out", str(cert), "-days", "1", "-subj",
                "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1"],
               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def start_mock(hold=False):
    args = [mock, str(cert), str(key), str(source_capture)]
    if hold:
        args.append("--hold")
    log = server_log.open("a")
    proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=log, text=True, bufsize=1)
    line = proc.stdout.readline().strip()
    if not line.startswith("PORT="):
        proc.stdin.write("\n"); proc.stdin.flush()
        proc.wait(timeout=5); log.close()
        raise RuntimeError(f"mock failed to start: {line}")
    return proc, log, "grpcs://127.0.0.1:" + line.split("=", 1)[1]

def stop_mock(proc, log):
    if proc.poll() is None:
        proc.stdin.write("\n"); proc.stdin.flush()
        proc.wait(timeout=5)
    log.close()

def template_fields(path):
    p = subprocess.run([replay, str(path)], check=True, capture_output=True, text=True)
    m = re.search(r"REPLAY_TEMPLATE index=1 network=mainnet daa=(\d+) bits=(\d+) timestamp=(\d+) nonce=(\d+) synced=true template_blake3=([0-9a-f]{64})", p.stdout)
    if not m:
        raise RuntimeError("capture does not contain a synchronized mainnet template")
    return m.groups()

source_fields = template_fields(source_capture)
mock_proc, mock_log, endpoint = start_mock()
try:
    env = os.environ.copy()
    env["GRPC_DEFAULT_SSL_ROOTS_FILE_PATH"] = str(cert)
    result = subprocess.run([client, "--dry-run", "--endpoint", endpoint,
                             "--pay-address", "kaspa:offline-shadow-mock",
                             "--extra-data", "NEXUS-M24-OFFLINE-MOCK",
                             "--capture", str(captured), "--max-templates", "1"],
                            capture_output=True, text=True, timeout=20, env=env)
    if result.returncode != 0 or "LIVE_TEMPLATE generation=1" not in result.stdout:
        raise RuntimeError("gRPC template capture failed: " + result.stdout + result.stderr)
    if template_fields(captured) != source_fields:
        raise RuntimeError("mock gRPC template fields differ from the authenticated source capture")
finally:
    stop_mock(mock_proc, mock_log)

# Exercise cancellation while the server keeps the stream open after its template.
mock_proc, mock_log, endpoint = start_mock(hold=True)
client_proc = None
try:
    env = os.environ.copy()
    env["GRPC_DEFAULT_SSL_ROOTS_FILE_PATH"] = str(cert)
    client_proc = subprocess.Popen([client, "--dry-run", "--endpoint", endpoint,
                                    "--pay-address", "kaspa:offline-shadow-mock",
                                    "--extra-data", "NEXUS-M24-CANCEL",
                                    "--capture", str(root / "cancel.capture")],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=False, bufsize=0, env=env)
    deadline = time.monotonic() + 15
    seen_bytes = bytearray()
    marker = b"LIVE_TEMPLATE generation="
    while time.monotonic() < deadline and marker not in seen_bytes:
        ready, _, _ = select.select([client_proc.stdout], [], [], 0.25)
        if not ready:
            if client_proc.poll() is not None:
                break
            continue
        chunk = os.read(client_proc.stdout.fileno(), 4096)
        if not chunk and client_proc.poll() is not None:
            break
        seen_bytes.extend(chunk)
    if marker not in seen_bytes:
        tail = client_proc.stdout.read().decode(errors="replace") if client_proc.poll() is not None else ""
        raise RuntimeError(
            f"client never received the held mock template rc={client_proc.poll()}: "
            f"{seen_bytes.decode(errors='replace') + tail}"
        )
    client_proc.send_signal(signal.SIGINT)
    client_proc.wait(timeout=5)
    tail = client_proc.stdout.read().decode(errors="replace")
    seen = seen_bytes.decode(errors="replace")
    if client_proc.returncode != 0 or "CLIENT_STREAM_CLOSED" not in seen + tail:
        raise RuntimeError(f"cooperative gRPC cancellation failed rc={client_proc.returncode}: {seen + tail}")
finally:
    if client_proc is not None and client_proc.poll() is None:
        client_proc.send_signal(signal.SIGINT)
        client_proc.wait(timeout=5)
    stop_mock(mock_proc, mock_log)

print("M24_AUTHENTIC_CAPTURE_GRPC_MOCK=PASS template_fields=IDENTICAL submit=DISABLED")
print("M24_GRPC_COOPERATIVE_CANCEL=PASS client_exit=0 forced_kill=NONE")
