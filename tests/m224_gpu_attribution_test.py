#!/usr/bin/env python3
import importlib.util
import errno
import pathlib
import subprocess
import sys
import unittest
from unittest import mock

SCRIPTS = pathlib.Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location("m224_gpu_monitor", SCRIPTS / "m224_gpu_monitor.py")
monitor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(monitor)
import m224_gpu_guard as guard


def sample(util=12, sm=12, category="kde-kwin-mixed-graphics", pid=13800,
           exe="/usr/bin/kwin_wayland", process_type="C+G", memory=26,
           throttle=1, temperature=42, power=30, nvml_sm=None):
    process = {
        "pid": pid, "starttime": "9876", "uid": 1000, "comm": "kwin_wayland",
        "exe": exe, "cmdline": [exe, "--wayland"], "ppid": 13739,
        "cgroup": "/user.slice/user-1000.slice/user@1000.service/session.slice/plasma-kwin_wayland.service",
        "identity_method": "verified", "type": process_type, "sm": sm,
        "memory_util": 2, "fb_mib": memory, "vram_mib": memory,
        "nvml_sm": nvml_sm, "nvml_memory_util": 2 if nvml_sm is not None else None,
        "nvml_sample_time_us": 1000 if nvml_sm is not None else None,
        "category": category, "nvml_compute_context": "C" in process_type,
        "nvml_graphics_context": "G" in process_type,
    }
    return {"time_ns": 123, "uuid": monitor.GPU_UUID, "name": monitor.GPU_NAME,
            "gpu_util": util, "memory_util": 10, "vram_total_mib": 8192,
            "vram_free_mib": 7000, "vram_used_mib": 1192,
            "temperature_c": temperature, "power_w": power,
            "power_limit_w": 230, "graphics_clock_mhz": 300,
            "throttle_reasons": throttle, "compute_pids": {}, "graphics_pids": {},
            "processes": [process]}


class AttributionTests(unittest.TestCase):
    def test_post_exit_proc_exe_race_retries_with_saved_identity(self):
        child = mock.Mock(pid=4242)
        child.poll.side_effect = [None, 0]
        snapshot = {"process_lists_complete": True}
        error = FileNotFoundError(errno.ENOENT, "gone", "/proc/4242/exe")
        with mock.patch.object(guard.monitor, "collect", side_effect=[error, snapshot]) as collect:
            got = guard.collect_child_gpu_sample(
                object(), child, "/workspace/nexus-cuda-real-model",
                {"pid": 4242, "exe": "/workspace/nexus-cuda-real-model"},
                retries=2, delay_seconds=0,
            )
        self.assertIs(got, snapshot)
        self.assertEqual(collect.call_count, 2)
        self.assertIsNone(collect.call_args_list[0].kwargs["exited_nexus_identity"])
        self.assertEqual(collect.call_args_list[1].kwargs["exited_nexus_identity"]["pid"], 4242)

    def test_post_exit_proc_exe_race_fails_closed_while_child_alive(self):
        child = mock.Mock(pid=4242)
        child.poll.return_value = None
        error = FileNotFoundError(errno.ENOENT, "gone", "/proc/4242/exe")
        with mock.patch.object(guard.monitor, "collect", side_effect=error):
            with self.assertRaisesRegex(RuntimeError, "remains alive"):
                guard.collect_child_gpu_sample(
                    object(), child, "/workspace/nexus-cuda-real-model",
                    {"pid": 4242, "exe": "/workspace/nexus-cuda-real-model"},
                    retries=2, delay_seconds=0,
                )

    def test_post_exit_wrong_proc_path_is_not_tolerated(self):
        child = mock.Mock(pid=4242)
        child.poll.return_value = 0
        error = FileNotFoundError(errno.ENOENT, "gone", "/proc/9999/exe")
        with mock.patch.object(guard.monitor, "collect", side_effect=error):
            with self.assertRaises(FileNotFoundError):
                guard.collect_child_gpu_sample(
                    object(), child, "/workspace/nexus-cuda-real-model",
                    {"pid": 4242, "exe": "/workspace/nexus-cuda-real-model"},
                    retries=2, delay_seconds=0,
                )

    def test_only_confirmed_exited_nexus_pid_enoent_or_esrch_is_tolerated(self):
        identity = {"pid": 4242, "exe": "/workspace/nexus-cuda-real-model", "starttime": "100"}
        self.assertTrue(monitor.confirmed_exited_nexus_procfs_race(
            4242, FileNotFoundError(errno.ENOENT, "gone"), 4242,
            identity["exe"], identity))
        self.assertTrue(monitor.confirmed_exited_nexus_procfs_race(
            4242, ProcessLookupError(errno.ESRCH, "gone"), 4242,
            identity["exe"], identity))
        self.assertFalse(monitor.confirmed_exited_nexus_procfs_race(
            4242, PermissionError(errno.EACCES, "denied"), 4242,
            identity["exe"], identity))
        self.assertFalse(monitor.confirmed_exited_nexus_procfs_race(
            9000, FileNotFoundError(errno.ENOENT, "gone"), 4242,
            identity["exe"], identity))
        self.assertFalse(monitor.confirmed_exited_nexus_procfs_race(
            4242, FileNotFoundError(errno.ENOENT, "gone"), 4242,
            "/wrong/executable", identity))
        self.assertFalse(monitor.confirmed_exited_nexus_procfs_race(
            4242, FileNotFoundError(errno.ENOENT, "gone"), 4242,
            identity["exe"], None))

    def test_collect_omits_only_confirmed_exited_nexus_nvml_record(self):
        class FakeNvml:
            def snapshot(self):
                return {"uuid": monitor.GPU_UUID, "name": monitor.GPU_NAME,
                        "gpu_util": 10,
                        "process_util_samples": {}, "compute_pids": {"4242": {"memory_mib": 20}},
                        "graphics_pids": {}}
            def process_pids(self, kind):
                return {"4242": {"memory_mib": 20}} if kind == "compute" else {}

        identity = {"pid": 4242, "exe": "/workspace/nexus-cuda-real-model", "starttime": "100"}
        pmon = {"4242": {"type": "C", "sm": 0, "memory_util": 0, "fb_mib": 20}}
        with mock.patch.object(monitor, "pmon_sample", return_value=pmon), \
             mock.patch.object(monitor, "proc_details",
                               side_effect=FileNotFoundError(errno.ENOENT, "reaped")):
            snapshot = monitor.collect(FakeNvml(), nexus_pid=4242,
                                      nexus_exe=identity["exe"],
                                      exited_nexus_identity=identity)
        self.assertEqual(snapshot["processes"], [])
        self.assertEqual(snapshot["terminated_nexus_pids"], [4242])

    def test_collect_does_not_tolerate_reused_or_unrelated_gpu_pid(self):
        class FakeNvml:
            def snapshot(self):
                return {"uuid": monitor.GPU_UUID, "name": monitor.GPU_NAME,
                        "gpu_util": 10,
                        "process_util_samples": {}, "compute_pids": {"4242": {"memory_mib": 20}},
                        "graphics_pids": {}}
            def process_pids(self, kind):
                return {"4242": {"memory_mib": 20}} if kind == "compute" else {}

        identity = {"pid": 4242, "exe": "/workspace/nexus-cuda-real-model", "starttime": "100"}
        pmon = {"4242": {"type": "C", "sm": 0, "memory_util": 0, "fb_mib": 20}}
        replacement = {"pid": 4242, "starttime": "101", "uid": 1000, "comm": "other",
                       "exe": "/usr/bin/other", "cmdline": ["/usr/bin/other"], "ppid": 1,
                       "cgroup": "/user.slice", "identity_method": "proc-exe"}
        with mock.patch.object(monitor, "pmon_sample", return_value=pmon), \
             mock.patch.object(monitor, "proc_details", return_value=replacement):
            with self.assertRaisesRegex(RuntimeError, "exited NEXUS PID=.*possible PID reuse"):
                monitor.collect(FakeNvml(), nexus_pid=4242,
                                nexus_exe=identity["exe"], exited_nexus_identity=identity)

    def test_pmon_cli_is_always_scoped_to_authorized_uuid(self):
        stdout = ("# gpu pid type sm mem enc dec jpg ofa fb ccpm command\n"
                  "# Idx # C/G % % % % % % MB MB name\n"
                  "0 13800 C+G 8 6 0 0 - - 26 0 kwin_wayland\n")
        result = subprocess.CompletedProcess([], 0, stdout, "")
        with mock.patch.object(monitor.subprocess, "run", return_value=result) as run:
            monitor.pmon_sample()
        args = run.call_args.args[0]
        self.assertEqual(args[:4], ["nvidia-smi", "pmon", "-i", monitor.GPU_UUID])
        self.assertIn("-c", args)

    def test_nvml_device_handle_is_obtained_only_by_authorized_uuid(self):
        fake_lib = mock.MagicMock()
        fake_lib.nvmlInit_v2.return_value = 0
        fake_lib.nvmlShutdown.return_value = 0
        captured = []

        def set_handle(uuid_bytes, handle_ptr):
            captured.append(uuid_bytes.decode())
            handle_ptr._obj.value = 123
            return 0

        fake_lib.nvmlDeviceGetHandleByUUID_v2.side_effect = set_handle
        with mock.patch.object(monitor.ctypes, "CDLL", return_value=fake_lib):
            device = monitor.Nvml()
            device.close()
        self.assertEqual(captured, [monitor.GPU_UUID])

    def test_verified_nexus_nvml_process_survives_pmon_sample_lag(self):
        pmon = {"13800": {"type": "C+G", "sm": 4, "memory_util": 2, "fb_mib": 25}}
        nvml_compute = {"13800": {"memory_mib": 25}, "4242": {"memory_mib": 10}}
        nvml_graphics = {"13800": {"memory_mib": 25}}
        aligned = monitor.reconcile_process_sets(pmon, nvml_compute, nvml_graphics, 4242)
        self.assertEqual(aligned["4242"]["type"], "C")
        self.assertIsNone(aligned["4242"]["sm"])

    def test_external_nvml_pmon_mismatch_still_blocks(self):
        pmon = {"13800": {"type": "C+G", "sm": 4, "memory_util": 2, "fb_mib": 25}}
        nvml_compute = {"13800": {"memory_mib": 25}, "9090": {"memory_mib": 32}}
        nvml_graphics = {"13800": {"memory_mib": 25}}
        with self.assertRaisesRegex(RuntimeError, "external NVML/pmon process sets disagree"):
            monitor.reconcile_process_sets(pmon, nvml_compute, nvml_graphics, 4242)

    def test_nexus_graphics_or_mixed_process_context_blocks(self):
        pmon = {"4242": {"type": "C+G", "sm": 4, "memory_util": 2, "fb_mib": 10}}
        with self.assertRaisesRegex(RuntimeError, "exclusively to a compute context"):
            monitor.reconcile_process_sets(pmon, {"4242": {"memory_mib": 10}},
                                           {"4242": {"memory_mib": 10}}, 4242)

    def test_procfs_disappearing_after_child_exit_is_not_a_gpu_failure(self):
        child = mock.Mock(pid=4242)
        child.poll.side_effect = [None, 0]
        with mock.patch.object(guard.hostguard, "process_memory", side_effect=FileNotFoundError("smaps_rollup")):
            metrics, exited = guard.read_child_memory_or_exit(child, retries=3, delay_seconds=0)
        self.assertIsNone(metrics)
        self.assertTrue(exited)

    def test_procfs_missing_while_cuda_child_remains_alive_blocks(self):
        child = mock.Mock(pid=4242)
        child.poll.return_value = None
        with mock.patch.object(guard.hostguard, "process_memory", side_effect=FileNotFoundError("smaps_rollup")):
            with self.assertRaisesRegex(RuntimeError, "still alive but procfs memory metrics disappeared"):
                guard.read_child_memory_or_exit(child, retries=2, delay_seconds=0)

    def test_procfs_esrch_race_after_cuda_child_exit_is_tolerated(self):
        child = mock.Mock(pid=4242)
        child.poll.side_effect = [None, 0]
        error = ProcessLookupError(errno.ESRCH, "No such process", "/proc/4242/smaps_rollup")
        with mock.patch.object(guard.hostguard, "process_memory", side_effect=error):
            metrics, exited = guard.read_child_memory_or_exit(child, retries=3, delay_seconds=0)
        self.assertIsNone(metrics)
        self.assertTrue(exited)

    def test_procfs_esrch_while_cuda_child_alive_blocks(self):
        child = mock.Mock(pid=4242)
        child.poll.return_value = None
        error = ProcessLookupError(errno.ESRCH, "No such process", "/proc/4242/smaps_rollup")
        with mock.patch.object(guard.hostguard, "process_memory", side_effect=error):
            with self.assertRaisesRegex(RuntimeError, "still alive but procfs memory metrics disappeared"):
                guard.read_child_memory_or_exit(child, retries=2, delay_seconds=0)

    def test_pmon_parses_gpu_uuid_scoped_process_metrics(self):
        text = ("# gpu pid type sm mem enc dec jpg ofa fb ccpm command\n"
                "# Idx # C/G % % % % % % MB MB name\n"
                "0 13800 C+G 8 6 0 0 - - 26 0 kwin_wayland\n")
        row = monitor.parse_pmon_text(text)["13800"]
        self.assertEqual((row["type"], row["sm"], row["memory_util"], row["fb_mib"]), ("C+G", 8, 6, 26))

    def test_attributed_stable_kde_graphics_can_pass_without_idle_gpu(self):
        self.assertEqual(monitor.policy_failures([sample(util=18)] * 5), [])

    def test_attributed_stable_graphics_at_32_percent_can_pass_with_headroom(self):
        snap = sample(util=32, sm=29, category="verified-user-graphics", pid=14252,
                      exe="/usr/lib/chromium/chromium", process_type="G", memory=430,
                      nvml_sm=28)
        self.assertEqual(monitor.policy_failures([snap] * 5), [])

    def test_graphics_burst_38_percent_is_not_ignored(self):
        snapshots = [sample(util=value, sm=min(value, 20)) for value in (8, 10, 38, 11, 9)]
        failures = monitor.policy_failures(snapshots)
        self.assertTrue(any("burst range" in reason for reason in failures))

    def test_functional_mode_allows_attributed_16_to_37_percent_graphics_burst(self):
        snapshots = [
            sample(util=value, sm=value - 3, nvml_sm=value - 3)
            for value in (16, 24, 37, 22, 18, 20)
        ]
        self.assertEqual(monitor.policy_failures(snapshots, mode="functional"), [])
        benchmark_failures = monitor.policy_failures(snapshots, mode="benchmark")
        self.assertTrue(any("aggregate GPU burst range" in failure
                            for failure in benchmark_failures))

    def test_nvml_process_utilization_burst_is_attributed_and_blocks(self):
        snapshots = [sample(util=14, sm=None, nvml_sm=value) for value in (2, 4, 18, 3, 2)]
        failures = monitor.policy_failures(snapshots)
        self.assertTrue(any("per-process SM burst range" in reason for reason in failures))

    def test_functional_mode_allows_verified_kwin_2_to_14_burst_but_benchmark_blocks(self):
        snapshots = [
            sample(util=13 + (index % 2), sm=2, nvml_sm=value)
            for index, value in enumerate((2, 4, 14, 6, 3, 5))
        ]
        self.assertEqual(monitor.policy_failures(snapshots, mode="functional"), [])
        failures = monitor.policy_failures(snapshots, mode="benchmark")
        self.assertTrue(any("per-process SM burst range" in reason for reason in failures))

    def test_functional_mode_still_enforces_absolute_kwin_sm_cap(self):
        snapshots = [sample(util=38, sm=36, nvml_sm=36) for _ in range(6)]
        failures = monitor.policy_failures(snapshots, mode="functional")
        self.assertTrue(any("KWin attributed SM" in reason for reason in failures))

    def test_functional_mode_still_enforces_total_gpu_cap(self):
        high_total = [sample(util=41, sm=20) for _ in range(6)]
        failures = monitor.policy_failures(high_total, mode="functional")
        self.assertTrue(any("aggregate GPU utilization" in reason for reason in failures))

    def test_functional_mode_blocks_insufficient_vram_temperature_power_and_throttle(self):
        cases = (
            (sample(), "RTX free VRAM below"),
            (sample(temperature=76), "GPU temperature exceeds"),
            (sample(power=185), "GPU power draw fraction exceeds"),
            (sample(throttle=4), "GPU slowdown/throttle active"),
        )
        # Keep fixture creation independent of the selected VRAM budget.
        cases = list(cases)
        low_vram = sample()
        low_vram["vram_free_mib"] = monitor.MIN_FREE_MIB - 1
        cases[0] = (low_vram, "RTX free VRAM below")
        for snap, reason in cases:
            with self.subTest(reason=reason):
                self.assertTrue(any(reason in failure for failure in
                                    monitor.policy_failures([snap] * 6, mode="functional")))

    def test_functional_mode_fails_closed_on_wrong_gpu_or_missing_telemetry(self):
        wrong_gpu = sample()
        wrong_gpu["uuid"] = "GPU-unauthorized"
        self.assertTrue(any("GPU identity mismatch" in failure for failure in
                            monitor.policy_failures([wrong_gpu] * 6, mode="functional")))

        missing = sample()
        del missing["temperature_c"]
        self.assertTrue(any("GPU telemetry incomplete" in failure for failure in
                            monitor.policy_failures([missing] * 6, mode="functional")))

        bad_process = sample()
        bad_process["processes"][0]["cgroup"] = None
        self.assertTrue(any("GPU process identity incomplete" in failure for failure in
                            monitor.policy_failures([bad_process] * 6, mode="functional")))

    def test_functional_mode_does_not_allow_kwin_identity_change(self):
        snapshots = [sample(util=13 + (i % 2), sm=2, nvml_sm=v)
                     for i, v in enumerate((2, 4, 14, 6, 3, 5))]
        snapshots[-1] = sample(util=14, sm=5, nvml_sm=5, pid=13801)
        failures = monitor.policy_failures(snapshots, mode="functional")
        self.assertTrue(any("identity set changed" in reason for reason in failures))

    def test_functional_mode_still_blocks_external_compute_unknown_and_graphics_burst(self):
        external = sample(category="external-compute", process_type="C", exe="/opt/worker")
        self.assertTrue(any("external compute" in reason
                            for reason in monitor.policy_failures([external] * 6, mode="functional")))
        unknown = sample(category="unknown", process_type="G", exe="/opt/unknown", pid=9090)
        self.assertTrue(any("unknown GPU process" in reason
                            for reason in monitor.policy_failures([unknown] * 6, mode="functional")))
        graphics = [sample(util=14, sm=2, nvml_sm=value, category="verified-user-graphics",
                           pid=14252, exe="/usr/lib/chromium/chromium", process_type="G", memory=100)
                    for value in (2, 4, 14, 6, 3, 5)]
        self.assertTrue(any("per-process SM burst" in reason
                            for reason in monitor.policy_failures(graphics, mode="functional")))

    def test_running_functional_mode_caps_external_attributed_activity(self):
        snap = sample(util=40, sm=25)
        kwin = snap["processes"][0]
        chromium = {**kwin, "pid": 14252, "starttime": "9877", "comm": "chromium",
                    "exe": "/usr/lib/chromium/chromium", "category": "verified-user-graphics",
                    "type": "G", "sm": 25, "nvml_sm": 25, "vram_mib": 100, "fb_mib": 100}
        own = {**kwin, "pid": 4242, "exe": "/opt/nexus", "category": "nexus-cuda",
               "type": "C", "sm": 0, "nvml_sm": 0, "vram_mib": 10, "fb_mib": 10}
        snap["processes"] = [kwin, chromium, own]
        failures = monitor.policy_failures([snap] * 3, running=True, own_pid=4242, mode="functional")
        self.assertTrue(any("external attributed SM activity" in reason for reason in failures))

    def test_cpu_psi_is_telemetry_and_never_an_execution_gate(self):
        self.assertEqual(guard.cpu_pressure_some_avg10("some avg10=0.00 avg60=0.00 avg300=0.00 total=0\nfull avg10=0.00 avg60=0.00 avg300=0.00 total=0"), 0.0)
        self.assertEqual(guard.cpu_pressure_some_avg10("some avg10=4.16 avg60=0.00 avg300=0.00 total=0"), 4.16)
        self.assertIsNone(guard.cpu_pressure_some_avg10("full avg10=0.00 avg60=0.00 avg300=0.00 total=0"))
        self.assertIsNone(guard.cpu_pressure_some_avg10("some avg10=nan avg60=0.00 avg300=0.00 total=0"))
        self.assertEqual(guard.psi_telemetry(0.43), "0.430:WARNING")
        self.assertEqual(guard.psi_telemetry(None), "UNAVAILABLE")

    def test_functional_mode_warns_on_psi_but_blocks_actual_swapout(self):
        host = (3 * 1024 * 1024, 3 * 1024 * 1024, 4.16, 100, 1000, 0, 8 * 1024 * 1024)
        with mock.patch.object(guard.hostguard, "host_memory", return_value=host), \
             mock.patch.object(guard, "cpu_pressure_some_avg10", return_value=4.16):
            result = guard.verify_host_memory(baseline_swapout=1000, mode="functional")
        self.assertEqual(result[8], 0)
        self.assertEqual(result[2], 4.16)
        self.assertEqual(result[7], 4.16)

        paging = (*host[:4], 1200, *host[5:])
        with mock.patch.object(guard.hostguard, "host_memory", return_value=paging), \
             mock.patch.object(guard, "cpu_pressure_some_avg10", return_value=4.16):
            with self.assertRaisesRegex(RuntimeError, "swap-out advanced during CUDA session"):
                guard.verify_host_memory(baseline_swapout=1000, mode="functional")

        low_ram = (2 * 1024 * 1024 - 1, 3 * 1024 * 1024, 0.0, 100, 1200, 0, 8 * 1024 * 1024)
        with mock.patch.object(guard.hostguard, "host_memory", return_value=low_ram), \
             mock.patch.object(guard, "cpu_pressure_some_avg10", return_value=0.0):
            with self.assertRaisesRegex(RuntimeError, "host preflight BLOCKED"):
                guard.verify_host_memory(baseline_swapout=1000, mode="functional")

        with mock.patch.object(guard.hostguard, "host_memory", return_value=paging), \
             mock.patch.object(guard, "cpu_pressure_some_avg10", return_value=4.16):
            with self.assertRaisesRegex(RuntimeError, "benchmark requires stable swap-out"):
                guard.verify_host_memory(baseline_swapout=1000, mode="benchmark")

    def test_functional_mode_still_blocks_low_swap_reserve_with_high_psi(self):
        host = (3 * 1024 * 1024, 3 * 1024 * 1024, 0.0, 100, 1000, 0, 8 * 1024 * 1024)
        high_psi_low_swap = (3 * 1024 * 1024, 2 * 1024 * 1024 - 1, 4.16, 100, 1000, 0, 8 * 1024 * 1024)
        with mock.patch.object(guard.hostguard, "host_memory", return_value=high_psi_low_swap), \
             mock.patch.object(guard, "cpu_pressure_some_avg10", return_value=4.16):
            with self.assertRaisesRegex(RuntimeError, "host preflight BLOCKED"):
                guard.verify_host_memory(baseline_swapout=1000, mode="functional")

    def test_sustained_38_percent_kwin_sm_exceeds_attribution_cap(self):
        failures = monitor.policy_failures([sample(util=38, sm=38)] * 5)
        self.assertTrue(any("KWin attributed SM" in reason for reason in failures))

    def test_external_compute_context_blocks(self):
        snap = sample(category="external-compute", process_type="C", exe="/opt/worker")
        self.assertTrue(any("external compute" in reason for reason in monitor.policy_failures([snap] * 5)))

    def test_known_non_kde_graphics_high_sm_blocks(self):
        snap = sample(util=32, sm=31, category="verified-user-graphics", pid=14252,
                      exe="/usr/lib/chromium/chromium", process_type="G", memory=430)
        self.assertTrue(any("non-KDE graphics process SM" in reason
                            for reason in monitor.policy_failures([snap] * 5)))

    def test_known_graphics_memory_is_budgeted_as_a_total(self):
        snap = sample(util=3, sm=None, category="verified-user-graphics", pid=14252,
                      exe="/usr/lib/chromium/chromium", process_type="G", memory=770)
        self.assertTrue(any("exceeds 768 MiB" in reason
                            for reason in monitor.policy_failures([snap] * 5)))

    def test_unknown_process_blocks(self):
        snap = sample(category="unknown", process_type="G", exe="/opt/unknown", pid=9090)
        self.assertTrue(any("unknown GPU process" in reason for reason in monitor.policy_failures([snap] * 5)))

    def test_process_replacement_blocks(self):
        snapshots = [sample() for _ in range(5)]
        snapshots[-1] = sample(pid=13801)
        self.assertTrue(any("identity set changed" in reason for reason in monitor.policy_failures(snapshots)))

    def test_telemetry_or_throttle_limits_block(self):
        self.assertTrue(monitor.policy_failures([sample()] * 2))
        self.assertTrue(any("throttle" in reason for reason in monitor.policy_failures([sample(throttle=4)] * 5)))

    def test_cmdline_values_are_not_written_to_process_log(self):
        encoded = monitor.json_line(sample())
        self.assertNotIn("--wayland", encoded)
        self.assertIn("cmdline_sha256", encoded)



class TelemetryCompatibilityTests(unittest.TestCase):
    def fallback_sample(self, status="NOT_SUPPORTED", **kwargs):
        s = sample(sm=None, util=20, **kwargs)
        s.update(process_util_capability={"status": status},
                 process_lists_complete=True, identities_rechecked=True)
        s["processes"][0]["identity_method"] = "proc-exe"
        return s

    def failures(self, s, **kw):
        return monitor.policy_failures([s, s, s], mode="functional",
                                       bounded_functional=True, **kw)

    def test_missing_sm_supported_and_not_supported_functional_only(self):
        for status in ("SUCCESS", "NOT_SUPPORTED", "NOT_FOUND"):
            with self.subTest(status=status):
                s = self.fallback_sample(status)
                self.assertEqual(self.failures(s), [])
                self.assertTrue(monitor.policy_failures([s]*3, mode="benchmark",
                                                        bounded_functional=True))
                self.assertTrue(monitor.policy_failures([s]*3, mode="functional"))

    def test_fallback_does_not_turn_missing_sm_into_zero(self):
        s = self.fallback_sample()
        self.assertIsNone(s["processes"][0]["sm"])
        self.assertIsNone(s["processes"][0]["nvml_sm"])
        self.assertEqual(self.failures(s), [])

    def test_fallback_ambiguous_metadata_blocks(self):
        for key, value in (("process_lists_complete", False), ("identities_rechecked", False),
                           ("process_util_capability", {"status": "ERROR"}),
                           ("processes", [])):
            s = self.fallback_sample(); s[key] = value
            self.assertTrue(self.failures(s), (key,value))

    def test_fallback_requires_real_executable_not_cmdline_surrogate(self):
        s = self.fallback_sample()
        s["processes"][0]["identity_method"] = "systemd-cgroup-parent-cmdline-fallback"
        self.assertTrue(self.failures(s))

    def test_fallback_external_unknown_context_and_missing_memory_block(self):
        for key, value in (("category", "external-compute"), ("category", "unknown"),
                           ("nvml_compute_context", False), ("vram_mib", None), ("fb_mib", None)):
            s = self.fallback_sample(); s["processes"][0][key] = value
            self.assertTrue(self.failures(s), (key,value))

    def test_fallback_resource_bounds_unchanged_even_with_own_work(self):
        for key, value in (("gpu_util", 41), ("vram_free_mib", 100),
                           ("temperature_c", 90), ("power_w", 240), ("throttle_reasons", 4),
                           ("uuid", "GPU-wrong")):
            s = self.fallback_sample(); s[key] = value
            self.assertTrue(self.failures(s), (key,value))
            self.assertTrue(self.failures(s, running=True, own_pid=9999), (key,value))

    def test_fallback_pid_reuse_or_process_disappearance_blocks(self):
        import copy
        a = self.fallback_sample()
        for field, value in (("starttime", "new"), ("pid", 999), ("exe", "/unknown")):
            b = copy.deepcopy(a); b["processes"][0][field] = value
            self.assertTrue(monitor.policy_failures([a,b,a], mode="functional", bounded_functional=True))
        b = copy.deepcopy(a); b["processes"] = []
        self.assertTrue(monitor.policy_failures([a,b,a], mode="functional", bounded_functional=True))

    def test_nvml_codes_are_distinct_and_errors_do_not_fallback(self):
        for code, name in ((0, "SUCCESS"), (3, "NOT_SUPPORTED"), (6, "NOT_FOUND")):
            d = object.__new__(monitor.Nvml)
            d.lib = mock.Mock(); d.handle = None
            d.process_util_last_seen_us = 0; d.process_util_cache = {}
            def query(handle, entries, count, stamp):
                count._obj.value = 0
                return code
            d.lib.nvmlDeviceGetProcessUtilization.side_effect = query
            self.assertEqual(d.process_utilization(), {})
            self.assertEqual(d.process_util_status["status"], name)
            self.assertEqual(d.process_util_status["code"], code)
        for code in (4, 9, 15, 999):
            d.lib.nvmlDeviceGetProcessUtilization.side_effect = None
            d.lib.nvmlDeviceGetProcessUtilization.return_value = code
            d.lib.nvmlErrorString.return_value = b"driver or permission failure"
            with self.assertRaisesRegex(RuntimeError, f"code={code}"):
                d.process_utilization()

    def test_collection_rechecks_identity_and_complete_lists(self):
        import copy
        s = self.fallback_sample()
        row = s["processes"][0]
        identity = {k: row[k] for k in ("pid", "starttime", "uid", "comm", "exe",
                                       "cmdline", "ppid", "cgroup", "identity_method")}
        pid = str(row["pid"])
        s["compute_pids"] = s["graphics_pids"] = {pid: {"memory_mib": 26}}
        s["process_util_samples"] = {}
        pmon = {pid: {"type": "C+G", "sm": None, "memory_util": None, "fb_mib": 26}}
        for fault in ("exited", "reused", "list-changed"):
            dev = mock.Mock()
            dev.snapshot.return_value = copy.deepcopy(s)
            dev.process_pids.return_value = {} if fault == "list-changed" else {pid: {}}
            second = dict(identity, starttime="new") if fault == "reused" else identity
            if fault == "exited": second = FileNotFoundError("process exited")
            with mock.patch.object(monitor, "pmon_sample", return_value=pmon), \
                 mock.patch.object(monitor, "proc_details", side_effect=[identity,second]), \
                 mock.patch.object(monitor, "classify_process", return_value=row["category"]):
                with self.assertRaises((RuntimeError, FileNotFoundError)):
                    monitor.collect(dev)

    def test_fallback_allows_high_psi_but_blocks_real_memory_constraints(self):
        with mock.patch.object(guard.hostguard, "host_memory",
                               return_value=(3*1024*1024,3*1024*1024,0.11,0,0,0,0)), \
             mock.patch.object(guard, "cpu_pressure_some_avg10", return_value=4.16):
            sample = guard.verify_host_memory(mode="functional")
            self.assertEqual(sample[2], 0.11)
            self.assertEqual(sample[7], 4.16)
        for available, swap_free in ((2*1024*1024-1,3*1024*1024),
                                     (3*1024*1024,2*1024*1024-1)):
            with mock.patch.object(guard.hostguard, "host_memory",
                                   return_value=(available,swap_free,4.16,0,0,0,0)), \
                 mock.patch.object(guard, "cpu_pressure_some_avg10", return_value=4.16):
                with self.assertRaisesRegex(RuntimeError, "host preflight BLOCKED"):
                    guard.verify_host_memory(mode="functional")


class KwinCorrelationTests(TelemetryCompatibilityTests):
    def correlated_sample(self, readable=False):
        import copy, os
        s = self.fallback_sample()
        p = s["processes"][0]
        uid = os.geteuid()
        p["uid"] = uid
        p["cgroup"] = f"/user.slice/user-{uid}.slice/user@{uid}.service/session.slice/plasma-kwin_wayland.service"
        p["identity_method"] = "proc-exe" if readable else monitor.kwin_identity.METHOD
        child = dict(pid=p["pid"], starttime=p["starttime"], state="S", ppid=p["ppid"],
                     uids=[uid]*4, gids=[os.getegid()]*4, comm=p["comm"],
                     cmdline=list(p["cmdline"]), cgroup="0::"+p["cgroup"],
                     exe=p["exe"] if readable else None,
                     exe_errno=None if readable else errno.EACCES)
        parent = dict(child, pid=p["ppid"], starttime="9000", ppid=10,
                      comm="kwin_wayland_wr", cmdline=["/usr/bin/kwin_wayland_wrapper", "--xwayland"],
                      exe="/usr/bin/kwin_wayland_wrapper", exe_errno=None)
        service = dict(ActiveState="active", MainPID=str(parent["pid"]),
                       ControlGroup=p["cgroup"],
                       ExecStart="{ path=/usr/bin/kwin_wayland_wrapper ; argv[]=/usr/bin/kwin_wayland_wrapper --xwayland ; ignore_errors=no ; pid=13739 ; }")
        evidence = dict(child_before=child, child_after=copy.deepcopy(child),
                        parent_before=parent, parent_after=copy.deepcopy(parent),
                        service_before=service, service_after=copy.deepcopy(service), opt_in=True)
        p["kwin_evidence"] = evidence
        p["kwin_identity_token"] = monitor.kwin_identity.validate(evidence, p)
        return s

    def test_legitimate_readable_and_eacces_complete_correlation(self):
        for readable in (False, True):
            s = self.correlated_sample(readable)
            self.assertEqual(self.failures(s), [])
        s = self.correlated_sample()
        self.assertTrue(monitor.policy_failures([s]*3, mode="benchmark", bounded_functional=True))

    def test_forged_or_incomplete_identity_blocks(self):
        edits = [
            ("child_before", "cmdline", ["/usr/bin/fake"]),
            ("child_before", "cgroup", "0::/other"),
            ("child_before", "uids", [999]*4),
            ("child_before", "gids", [999]*4),
            ("child_after", "starttime", "9999"),
            ("child_after", "state", "Z"),
            ("child_before", "ppid", 123),
            ("child_before", "exe", "/usr/bin/other"),
            ("child_before", "exe_errno", errno.EPERM),
            ("parent_before", "exe", "/usr/bin/fake"),
            ("parent_before", "exe_errno", errno.EACCES),
            ("parent_after", "starttime", "1"),
            ("service_before", "ActiveState", "inactive"),
            ("service_before", "MainPID", "1"),
            ("service_before", "ControlGroup", "/wrong"),
            ("service_before", "ExecStart", "{ path=/usr/bin/fake ; }"),
        ]
        for role, field, value in edits:
            with self.subTest(role=role, field=field):
                s = self.correlated_sample()
                s["processes"][0]["kwin_evidence"][role][field] = value
                self.assertTrue(self.failures(s))
        s = self.correlated_sample()
        s["processes"][0]["kwin_evidence"]["opt_in"] = False
        self.assertTrue(self.failures(s))

    def test_correlated_identity_requires_nvml_graphics_complete_telemetry(self):
        for field,value in (("category","unknown"), ("category","external-compute"),
                            ("nvml_graphics_context",False), ("kwin_identity_token","wrong")):
            s = self.correlated_sample(); s["processes"][0][field] = value
            self.assertTrue(self.failures(s), field)
        for field in ("process_lists_complete","identities_rechecked"):
            s = self.correlated_sample(); s[field] = False
            self.assertTrue(self.failures(s))

    def test_wrapper_replacement_between_windows_is_not_inherited(self):
        import copy
        s = self.correlated_sample(); b = copy.deepcopy(s)
        p = b["processes"][0]
        for role in ("parent_before","parent_after"):
            p["kwin_evidence"][role]["starttime"] = "9001"
        p["kwin_identity_token"] = monitor.kwin_identity.validate(p["kwin_evidence"], p)
        self.assertTrue(monitor.policy_failures([s,b,s],mode="functional",bounded_functional=True))

    def test_process_exit_and_service_failure_propagate(self):
        s = self.correlated_sample(); p = s["processes"][0]
        with mock.patch.object(monitor.kwin_identity, "read_process",
                               side_effect=ProcessLookupError(errno.ESRCH, "exited")):
            with self.assertRaises(ProcessLookupError):
                monitor.kwin_identity.collect(p)
        with mock.patch.object(monitor.kwin_identity, "read_process",
                               side_effect=[p["kwin_evidence"]["child_before"],
                                            p["kwin_evidence"]["parent_before"]]), \
             mock.patch.object(monitor.kwin_identity, "read_service",
                               side_effect=RuntimeError("systemd unavailable")):
            with self.assertRaisesRegex(RuntimeError, "systemd unavailable"):
                monitor.kwin_identity.collect(p)

if __name__ == "__main__":
    unittest.main(verbosity=2)
