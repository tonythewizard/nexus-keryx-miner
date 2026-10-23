import os
#!/usr/bin/env python3
import importlib.util
import pathlib
import tempfile
import sys
import unittest

SCRIPT = pathlib.Path(__file__).parents[1] / "scripts" / "nexus_force_shadow.py"
sys.path.insert(0, str(SCRIPT.parent))
spec = importlib.util.spec_from_file_location("nexus_force_shadow", SCRIPT)
force = importlib.util.module_from_spec(spec)
spec.loader.exec_module(force)


class ForceShadowTests(unittest.TestCase):
    def test_high_pressure_and_load_values_are_telemetry_not_policy(self):
        row = force.host_telemetry_tuple((1, 2, 4.16, 900, 1200, 0, 3))
        self.assertEqual(row["psi_status"], "WARNING")
        self.assertEqual(row["memory_available_kib"], 1)
        self.assertEqual(row["swap_free_kib"], 2)
        self.assertEqual(row["swap_out_pages"], 1200)

    def test_unavailable_pressure_stays_unavailable_not_zero(self):
        row = force.host_telemetry_tuple((None, None, None, None, None, None, None))
        self.assertIsNone(row["psi_memory_full_avg10"])
        self.assertEqual(row["psi_status"], "UNAVAILABLE")

    def test_authorized_gpu_identity_is_mandatory(self):
        force.validate_identity(force.GPU_UUID, force.GPU_UUID, force.GPU_UUID, force.GPU_NAME)
        with self.assertRaisesRegex(RuntimeError, "device selection"):
            force.validate_identity("0", force.GPU_UUID, force.GPU_UUID, force.GPU_NAME)
        with self.assertRaisesRegex(RuntimeError, "NVML identity"):
            force.validate_identity(force.GPU_UUID, force.GPU_UUID,
                                    "GPU-other", force.GPU_NAME)

    def test_force_mode_is_finite(self):
        force.validate_limits(8, 120)
        force.validate_limits(32, 120)
        for count in (0, 33):
            with self.assertRaises(ValueError):
                force.validate_limits(count, 120)
        with self.assertRaises(ValueError):
            force.validate_limits(8, 121)

    def test_only_local_tls_node_endpoint_is_allowed(self):
        force.validate_endpoint("grpcs://127.0.0.1:22110")
        force.validate_endpoint("grpc://127.0.0.1:22110")
        for endpoint in ("grpc://example.org:22110", "grpcs://example.org:22110"):
            with self.assertRaises(ValueError):
                force.validate_endpoint(endpoint)

    def test_stock_launcher_fields_are_parsed_without_logging_secrets(self):
        cert = "ab" * 64
        text = f"set -l CERT {cert}\nset -l argv --mining-address keryx:test-address\n"
        self.assertEqual(force.parse_stock_mining_fields(text), ("keryx:test-address", cert))
        with self.assertRaises(RuntimeError):
            force.parse_stock_mining_fields("set -l argv --mining-address keryx:test-address\n")

    def test_escrow_pubkey_derivation_uses_valid_key_without_persisting_private_material(self):
        work = pathlib.Path(os.environ.get("NEXUS_DATA_DIR", str(pathlib.Path.home() / ".local/share/nexus-keryx-miner"))) / "work"
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as tmp:
            key_path = pathlib.Path(tmp) / "test-key.hex"
            key_path.write_text("01".zfill(64), encoding="ascii")
            pubkey = force.derive_escrow_pubkey(key_path)
            self.assertRegex(pubkey, r"^[0-9a-f]{64}$")
            self.assertNotEqual(pubkey, "01".zfill(64))


if __name__ == "__main__":
    unittest.main(verbosity=2)
