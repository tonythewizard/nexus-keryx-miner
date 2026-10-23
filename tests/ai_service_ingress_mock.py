#!/usr/bin/env python3
"""TLS mock for NotifyBlockAdded -> bounded GetBlock startup backfill."""
import os
import pathlib
import subprocess
import sys

client, mock, capture, root = sys.argv[1:]
root = pathlib.Path(root)
root.mkdir(parents=True, exist_ok=True)
cert, key = root / "cert.pem", root / "key.pem"
log_path = root / "mock.log"
subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-keyout", str(key), "-out", str(cert), "-days", "1", "-subj",
                "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1"],
               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
log = log_path.open("w")
proc = subprocess.Popen([mock, str(cert), str(key), capture, "--hold", "--ai-block-fixture"],
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log,
                        text=True, bufsize=1)
try:
    line = proc.stdout.readline().strip()
    if not line.startswith("PORT="):
        raise RuntimeError("gRPC AI fixture server failed to start: " + line)
    env = os.environ.copy()
    env["GRPC_DEFAULT_SSL_ROOTS_FILE_PATH"] = str(cert)
    result = subprocess.run([client, "grpcs://127.0.0.1:" + line.split("=", 1)[1]],
                            env=env, capture_output=True, text=True, timeout=20)
    if result.returncode != 0 or "AI_REQUEST_STARTUP_BACKFILL=PASS" not in result.stdout:
        raise RuntimeError("AI request ingress/backfill failed: " + result.stdout + result.stderr + log_path.read_text())
finally:
    if proc.poll() is None:
        proc.stdin.write("\n")
        proc.stdin.flush()
        proc.wait(timeout=5)
    log.close()
print("AI_REQUEST_GRPC_INGRESS_MOCK=PASS notification=official get_block_backfill=bounded submit=DISABLED")
