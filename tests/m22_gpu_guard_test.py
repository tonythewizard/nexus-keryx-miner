#!/usr/bin/env python3
import importlib.util
import contextlib
import io
import os
import pathlib
import subprocess
import sys
import unittest
from unittest import mock

SCRIPT = pathlib.Path(__file__).parents[1] / "scripts" / "m22_gpu_guard.py"
spec = importlib.util.spec_from_file_location("m22_gpu_guard", SCRIPT)
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)
budget_path = pathlib.Path(__file__).parents[1] / "scripts" / "m22_memory_budget.py"
budget_spec = importlib.util.spec_from_file_location("m22_memory_budget", budget_path)
budget = importlib.util.module_from_spec(budget_spec)
budget_spec.loader.exec_module(budget)
env_path = pathlib.Path(__file__).parents[1] / "scripts" / "check_persistent_env.py"
env_spec = importlib.util.spec_from_file_location("check_persistent_env", env_path)
persistent_env = importlib.util.module_from_spec(env_spec)
env_spec.loader.exec_module(persistent_env)
sys.path.insert(0, str(SCRIPT.parent))
import m22_build_guard as build_guard


def app(pid, exe, comm, cmdline, memory=32, uid=None, cgroup=None, ppid=10, method="proc-exe"):
    uid = guard.os.geteuid() if uid is None else uid
    cgroup = "/user.slice/user-1000.slice/user@1000.service/session.slice/plasma-kwin_wayland.service" if cgroup is None else cgroup
    ident = (pid, "12345", uid, comm, exe, cmdline, ppid, cgroup, method)
    return {"pid": pid, "pname": exe, "memory_mib": memory, "identity": ident}


class GuardPolicyTests(unittest.TestCase):
    def test_fish_runner_failure_branch_preserves_original_exit_status(self):
        result = subprocess.run(
            ["fish", "-c", "false; or begin; set -l failure_status $status; true; exit $failure_status; end"],
            check=False,
        )
        self.assertEqual(result.returncode, 1)

    def test_legacy_guard_queries_only_authorized_rtx_uuid(self):
        row = ("NVIDIA GeForce primary NVIDIA GPU," + guard.GPU_UUID +
               ",8.6,7000,8192,2,40,30,230,300")
        with mock.patch.object(guard, "run_nvidia", return_value=row) as query:
            result = guard.target_gpu()
        self.assertEqual(result["uuid"], guard.GPU_UUID)
        self.assertEqual(query.call_args.args[0][0], "--id=" + guard.GPU_UUID)
        self.assertNotIn("--query-gpu", query.call_args.args[0][0])

    def test_legacy_compute_process_query_is_uuid_scoped(self):
        with mock.patch.object(guard, "run_nvidia", return_value="") as query:
            self.assertEqual(guard.compute_apps(), [])
        args = query.call_args.args[0]
        self.assertEqual(args[0], "--id=" + guard.GPU_UUID)
        self.assertTrue(any(item.startswith("--query-compute-apps=") for item in args))

    def test_memory_psi_parser_handles_linux_equals_separator(self):
        self.assertEqual(guard.parse_psi_full_avg10("some avg10=0.20 avg60=0.1 avg300=0\nfull avg10=0.16 avg60=0.1 avg300=0\n"), 0.16)
        with self.assertRaisesRegex(RuntimeError, "invalid memory PSI"):
            guard.parse_psi_full_avg10("full avg10=invalid avg60=0 avg300=0\n")

    def test_missing_psi_is_reported_unavailable_not_fabricated_zero(self):
        meminfo = "MemTotal: 16000000 kB\nMemFree: 4000000 kB\nMemAvailable: 5000000 kB\nSwapTotal: 8000000 kB\nSwapFree: 7000000 kB\n"
        vmstat = "pswpin 10\npswpout 20\n"
        def read_text(path, *args, **kwargs):
            if str(path) == "/proc/meminfo":
                return meminfo
            if str(path) == "/proc/vmstat":
                return vmstat
            raise PermissionError("PSI unavailable")
        with mock.patch.object(pathlib.Path, "read_text", autospec=True, side_effect=read_text):
            snapshot = budget.read_snapshot()
        self.assertIsNone(snapshot["psi_full_avg10"])
        self.assertEqual(budget.psi_status(snapshot["psi_full_avg10"]), "UNAVAILABLE")

    def test_toolchain_environment_paths_must_stay_in_workspace(self):
        root = pathlib.Path(__file__).parents[2].resolve()
        values = {name: str(root / name.lower()) for name in persistent_env.REQUIRED}
        self.assertEqual(persistent_env.validate(root, values), [])
        values["CARGO_TARGET_DIR"] = "/var/cache/cargo"
        self.assertIn("CARGO_TARGET_DIR escapes", persistent_env.validate(root, values)[0])
        del values["TMP"]
        self.assertIn("TMP=unset", persistent_env.validate(root, values))

    def test_microbatch_shrinks_with_available_memory_and_respects_hard_floor(self):
        self.assertEqual(budget.select_microbatch(256, 2200 * 1024), 1)
        self.assertEqual(budget.select_microbatch(64, 3500 * 1024), 2)
        self.assertEqual(budget.select_microbatch(256, 5000 * 1024), 4)
        self.assertEqual(budget.select_microbatch(256, 8000 * 1024), 8)
        self.assertEqual(budget.select_microbatch(1, 8000 * 1024), 1)
        with self.assertRaisesRegex(RuntimeError, "2 GiB"):
            budget.select_microbatch(256, 2047 * 1024)
        sample = self.resource_snapshot(2200 * 1024, 3000 * 1024, 0.05)
        budget.validate_snapshot("cuda", sample)
        sample["SwapFree"] = 1900 * 1024
        with self.assertRaisesRegex(RuntimeError, "SwapFree"):
            budget.validate_snapshot("cuda", sample)
        sample = self.resource_snapshot(2200 * 1024, 3000 * 1024, 4.16)
        budget.validate_snapshot("cuda", sample)
        sample["psi_full_avg10"] = None
        budget.validate_snapshot("cuda", sample)

    @staticmethod
    def resource_snapshot(available, swap_free, psi, swapout=123):
        return {"MemTotal": 16 * 1024 * 1024, "MemFree": available // 2,
                "MemAvailable": available, "Buffers": 0, "Cached": 100000,
                "Shmem": 1000, "Slab": 2000, "SReclaimable": 1000,
                "SUnreclaim": 1000, "AnonPages": 8000000,
                "SwapTotal": 16 * 1024 * 1024, "SwapFree": swap_free,
                "psi_full_avg10": psi, "pswpin": 456, "pswpout": swapout}

    def test_build_cuda_and_benchmark_preflights_are_separate(self):
        self.assertEqual(budget.POLICIES["build"]["available"], 4 * 1024 * 1024)
        self.assertEqual(budget.POLICIES["cuda"]["available"], 2 * 1024 * 1024)
        self.assertEqual(budget.POLICIES["benchmark"]["available"], 6 * 1024 * 1024)
        budget.validate_snapshot("build", self.resource_snapshot(4 * 1024 * 1024,
                                                                   2 * 1024 * 1024, 0.05))
        with self.assertRaisesRegex(RuntimeError, "build preflight BLOCKED"):
            budget.validate_snapshot("build", self.resource_snapshot(3 * 1024 * 1024,
                                                                       2 * 1024 * 1024, 0.05))
        budget.validate_snapshot("cuda", self.resource_snapshot(2 * 1024 * 1024,
                                                                  2 * 1024 * 1024, 0.09))
        budget.validate_snapshot("benchmark", self.resource_snapshot(6 * 1024 * 1024,
                                                                        4 * 1024 * 1024, 4.16))

    def test_stable_build_preflight_blocks_swapout_during_sampling(self):
        snapshots = iter([
            self.resource_snapshot(5 * 1024 * 1024, 3 * 1024 * 1024, 0.02, 100),
            self.resource_snapshot(5 * 1024 * 1024, 3 * 1024 * 1024, 0.02, 101),
        ])
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "swap-out changed"):
                budget.stable_preflight("build", samples=2, interval=0,
                                        reader=lambda: next(snapshots), sleeper=lambda _: None)

    def test_build_guard_measures_only_its_process_group(self):
        rss, pss, count = build_guard.process_group_usage(os.getpgrp())
        self.assertGreaterEqual(count, 1)
        self.assertGreaterEqual(rss, 0)
        self.assertGreaterEqual(pss, 0)

    def test_exact_kwin_process_is_scoped_and_allowed_under_memory_cap(self):
        kwin = app(22, guard.KWN_EXE, "kwin_wayland", [guard.KWN_EXE, "--wayland-fd", "7"])
        with mock.patch.object(guard, "verify_kwin_service") as verify:
            self.assertIn(22, guard.classify_apps([kwin]))
            verify.assert_called_once_with(22, 10, guard.os.geteuid(), "kwin_wayland",
                                           [guard.KWN_EXE, "--wayland-fd", "7"],
                                           kwin["identity"][7], "12345")

    def test_kwin_name_lookalike_is_not_whitelisted(self):
        other = app(23, "/usr/bin/kwin_wayland_helper", "kwin_wayland_hel", ["/usr/bin/kwin_wayland_helper"])
        with self.assertRaisesRegex(RuntimeError, "unapproved CUDA process"):
            guard.classify_apps([other])

    def test_kwin_above_memory_cap_blocks(self):
        kwin = app(22, guard.KWN_EXE, "kwin_wayland", [guard.KWN_EXE], memory=129)
        with mock.patch.object(guard, "verify_kwin_service"):
            with self.assertRaisesRegex(RuntimeError, "exceeds 128 MiB"):
                guard.classify_apps([kwin])

    def test_wrong_uid_cmdline_or_cgroup_blocks(self):
        cases = [
            app(22, guard.KWN_EXE, "kwin_wayland", [guard.KWN_EXE], uid=guard.os.geteuid()+1),
            app(22, guard.KWN_EXE, "kwin_wayland", ["/opt/untrusted-kwin"]),
            app(22, guard.KWN_EXE, "kwin_wayland", [guard.KWN_EXE], cgroup="/user.slice/other.service"),
        ]
        for candidate in cases:
            with self.subTest(identity=candidate["identity"]):
                with self.assertRaises(RuntimeError):
                    guard.classify_apps([candidate])

    def test_only_exact_child_executable_may_be_attributed_to_runner(self):
        child = app(44, "/workspace/nexus-cuda-real-model", "nexus-cuda-real", ["nexus-cuda-real-model"])
        self.assertIn(44, guard.classify_apps([child], 44, "/workspace/nexus-cuda-real-model"))
        with self.assertRaisesRegex(RuntimeError, "identity mismatch"):
            guard.classify_apps([child], 44, "/workspace/other")

    def test_unrecognized_compute_process_blocks(self):
        app_unknown = app(55, "/usr/bin/unknown-cuda-worker", "cuda-worker", ["unknown-cuda-worker"])
        with self.assertRaisesRegex(RuntimeError, "unapproved CUDA process"):
            guard.classify_apps([app_unknown])


if __name__ == "__main__":
    unittest.main(verbosity=2)
