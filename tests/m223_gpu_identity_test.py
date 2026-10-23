#!/usr/bin/env python3
import importlib.util
import pathlib
import subprocess
import unittest
from unittest import mock

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "scripts" / "m223_gpu_identity.py"
spec = importlib.util.spec_from_file_location("m223_gpu_identity", SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class GpuIdentityTests(unittest.TestCase):
    def test_identity_cli_query_is_scoped_to_authorized_uuid(self):
        result = subprocess.CompletedProcess([], 0,
            "NVIDIA GeForce primary NVIDIA GPU, GPU-00000000-0000-0000-0000-000000000000, 8.6\n", "")
        with mock.patch.object(module.subprocess, "run", return_value=result) as run:
            module.query("nvidia-smi")
        args = run.call_args.args[0]
        self.assertIn("--id=" + module.GPU_UUID, args)
        self.assertNotIn("--list-gpus", args)
        self.assertNotIn("--query-gpu=index,name,uuid", args)

    def test_authorized_gpu_only(self):
        row = "NVIDIA GeForce primary NVIDIA GPU, GPU-00000000-0000-0000-0000-000000000000, 8.6\n"
        self.assertEqual(module.validate_csv(row)["uuid"], module.GPU_UUID)

    def test_non_target_uuid_is_rejected(self):
        row = "Unrelated GPU, GPU-unqueried-fixture, 9.9\n"
        with self.assertRaisesRegex(ValueError, "not uniquely present"):
            module.validate_csv(row)

    def test_missing_authorized_device_fails_closed(self):
        with self.assertRaisesRegex(ValueError, "not uniquely present"):
            module.validate_csv("Unselected NVIDIA GPU, GPU-unqueried-fixture, 7.5\n")

    def test_wrong_name_or_capability_fails_closed(self):
        with self.assertRaisesRegex(ValueError, "identity mismatch"):
            module.validate_csv("NVIDIA GeForce primary NVIDIA GPU, GPU-00000000-0000-0000-0000-000000000000, 7.5\n")

    def test_duplicate_uuid_fails_closed(self):
        row = "NVIDIA GeForce primary NVIDIA GPU, GPU-00000000-0000-0000-0000-000000000000, 8.6\n"
        with self.assertRaisesRegex(ValueError, "duplicate"):
            module.validate_csv(row + row)


if __name__ == "__main__":
    unittest.main()
