"""CPU/loopback workflow checks; no wallet authorization, mining or GPU use."""
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import nexus_mining_mode as mode


class PoolProbeTests(unittest.TestCase):
    def probe(self, *, hello=True, subscribe=True, tls=None, trust=None, host="127.0.0.1"):
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(2)
        methods = []
        errors = []

        def serve():
            try:
                conn, _ = listener.accept()
                conn.settimeout(2)
                if tls:
                    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                    context.load_cert_chain(tls[1], tls[0])
                    conn = context.wrap_socket(conn, server_side=True)
                with conn:
                    pending = bytearray()
                    for _ in range(2):
                        while b"\n" not in pending:
                            data = conn.recv(8192)
                            if not data:
                                return
                            pending.extend(data)
                        line, _, tail = pending.partition(b"\n")
                        pending[:] = tail
                        q = json.loads(line)
                        methods.append(q["method"])
                        if q["method"] == "mining.subscribe":
                            reply = {"id": q["id"], "result": [[], "aa", 7] if subscribe else False,
                                     "error": None}
                            conn.sendall(b'{"method":"mining.notify","params":[]}\n')
                        else:
                            self.assertEqual(q["method"], "mining.hello")
                            obj = q["params"][0]
                            self.assertEqual(obj["trace"], 0)
                            self.assertFalse(obj["model"]["loaded"])
                            self.assertEqual(obj["miner"]["name"], "nexus-keryx-miner")
                            reply = {"id": q["id"], "result": hello,
                                     "error": None if hello else [20, "unknown method", None]}
                        conn.sendall((json.dumps(reply) + "\n").encode())
            except (OSError, ssl.SSLError):
                pass
            except BaseException as exc:
                errors.append(exc)

        thread = threading.Thread(target=serve)
        thread.start()
        try:
            scheme = "stratum+ssl" if tls else "stratum+tcp"
            with patch.dict(os.environ, {"SSL_CERT_FILE": str(trust)} if trust else {}):
                result = mode.test_connection(f"{scheme}://{host}:{listener.getsockname()[1]}", 1.5)
        finally:
            listener.close()
            thread.join(timeout=3)
        self.assertFalse(thread.is_alive())
        if errors:
            raise errors[0]
        self.assertNotIn("mining.authorize", methods)
        self.assertNotIn("mining.submit", methods)
        return result, methods

    def test_protocol_probe_is_separate_from_authorization_and_escrow(self):
        result, methods = self.probe()
        self.assertTrue(result["ok"])
        self.assertTrue(result["telemetry_supported"])
        self.assertEqual(methods, ["mining.subscribe", "mining.hello"])
        self.assertFalse(result["mining_validated"])
        self.assertFalse(result["authorization_tested"])
        self.assertIsNone(result["escrow_approved"])

    def test_unsupported_optional_extension_and_rejected_subscription(self):
        result, _ = self.probe(hello=False)
        self.assertTrue(result["ok"])
        self.assertFalse(result["telemetry_supported"])
        with patch.object(mode, "pool_requirements", return_value={"telemetry_required": True}):
            result, _ = self.probe(hello=False)
        self.assertFalse(result["ok"])
        result, _ = self.probe(subscribe=False)
        self.assertFalse(result["ok"])

    def test_pool_escrow_approval_never_inferred(self):
        k = mode.pool_requirements("stratum+tcp://s1.keryxpool.com:4401")
        self.assertEqual((k["escrow_miner_percent"], k["escrow_pool_percent"], k["escrow_release_hours"]), (70, 30, 22))
        self.assertEqual(k["escrow_vendor"], "NVIDIA")
        self.assertIsNone(k["escrow_approved"])
        self.assertIsNone(mode.pool_requirements("stratum+ssl://krx.suprnova.cc:4405")["escrow_approval_required"])

    def test_cryo_preset_migrates_without_changing_active_pool(self):
        original = mode.default_config()
        original["mode"] = "pool"
        original["active_pool_id"] = "suprnova"
        original["pools"] = [p for p in original["pools"] if p["id"] != "cryopool"]
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/"mode.json"
            path.write_text(json.dumps(original))
            path.chmod(0o600)
            before = path.read_bytes()
            with patch.object(mode, "config_path", return_value=path):
                cfg = mode.load()
                self.assertEqual(cfg["active_pool_id"], "suprnova")
                cryo = next(p for p in cfg["pools"] if p["id"] == "cryopool")
                self.assertEqual(cryo["url"], "stratum+ssl://krx.cryo-pool.com:5555")
                self.assertTrue(cryo["tls"])
                self.assertEqual(path.read_bytes(), before)
                path.write_text(json.dumps(cfg))
                self.assertEqual(sum(p["id"]=="cryopool" for p in mode.load()["pools"]), 1)
        self.assertTrue(mode.pool_requirements(cryo["url"])["ai_service_required"])
        self.assertIsNone(mode.pool_requirements(cryo["url"])["escrow_approved"])

    def test_nodvexa_preset_migrates_without_changing_active_pool(self):
        original = mode.default_config()
        original["mode"] = "pool"
        original["active_pool_id"] = "suprnova"
        original["pools"] = [p for p in original["pools"] if p["id"] != "nodvexa"]
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/"mode.json"
            path.write_text(json.dumps(original))
            path.chmod(0o600)
            before = path.read_bytes()
            with patch.object(mode, "config_path", return_value=path):
                cfg = mode.load()
                self.assertEqual(cfg["active_pool_id"], "suprnova")
                nodvexa = next(p for p in cfg["pools"] if p["id"] == "nodvexa")
                self.assertEqual(nodvexa["url"], "stratum+ssl://nodvexa.eu:5555")
                self.assertTrue(nodvexa["tls"])
                self.assertEqual(path.read_bytes(), before)
                path.write_text(json.dumps(cfg))
                self.assertEqual(sum(p["id"]=="nodvexa" for p in mode.load()["pools"]), 1)
        reqs = mode.pool_requirements(nodvexa["url"])
        self.assertEqual(reqs["pool"], "nodvexa")
        self.assertFalse(reqs["telemetry_required"])
        self.assertIsNone(reqs["escrow_approved"])

    def test_real_tls_chain_and_name_validation(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.assertFalse(str(root).startswith("/tmp/"))
            pairs = []
            for name, san in (("valid", "DNS:localhost,IP:127.0.0.1"), ("wrong", "DNS:wrong.invalid")):
                key, cert = root / f"{name}.key", root / f"{name}.pem"
                subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                                "-subj", "/CN=synthetic", "-addext", "subjectAltName=" + san,
                                "-keyout", str(key), "-out", str(cert)], check=True, capture_output=True)
                pairs.append((key, cert))
            good, wrong = pairs
            result, _ = self.probe(tls=good, trust=good[1])
            self.assertTrue(result["ok"], result)
            result, _ = self.probe(tls=wrong, trust=wrong[1])
            self.assertFalse(result["ok"])
            result, _ = self.probe(tls=good, trust=wrong[1])
            self.assertFalse(result["ok"])


if __name__ == "__main__":
    unittest.main()
