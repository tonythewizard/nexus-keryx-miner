#!/usr/bin/env python3
import os
import pathlib
import subprocess
import sys

probe, mock, root = sys.argv[1:]
root = pathlib.Path(root).resolve()
root.mkdir(parents=True, exist_ok=True)
(root / "tmp").mkdir(exist_ok=True)
cert, key = root / "service-cert.pem", root / "service-key.pem"
subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-keyout", str(key), "-out", str(cert), "-days", "1", "-subj",
                "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1"],
               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
log = (root / "service-mock.log").open("a")
server = subprocess.Popen([mock, str(cert), str(key), "--service-fixture", "--chain-status-fixture", "--slow-strikes"],
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=log, text=True, bufsize=1)
try:
    line = server.stdout.readline().strip()
    if not line.startswith("PORT="):
        raise RuntimeError(f"mock gRPC server failed to start: {line}")
    env = os.environ.copy()
    env["GRPC_DEFAULT_SSL_ROOTS_FILE_PATH"] = str(cert)
    result = subprocess.run([probe, "grpcs://127.0.0.1:" + line.split("=", 1)[1]],
                            capture_output=True, text=True, timeout=10, env=env)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    print(result.stdout.strip())
finally:
    if server.poll() is None:
        server.stdin.write("\n")
        server.stdin.flush()
        server.wait(timeout=5)
    log.close()
