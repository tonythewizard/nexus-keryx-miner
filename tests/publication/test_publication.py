"""Exercise publication boundaries with isolated, synthetic Git repositories."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("gate", ROOT / "tools/security_gate.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class PrivacyBoundaries(unittest.TestCase):
    def test_source_text_is_allowed(self):
        self.assertEqual(gate.violations("src/example.cpp", b"int main() {}\n"), [])

    def test_binary_symlink_submodule_and_private_paths_are_rejected(self):
        for name, data, mode in (("src/a.cpp", b"\0", "100644"),
                                 ("src/a.cpp", b"", "120000"),
                                 ("src/a.cpp", b"", "160000"),
                                 ("state/session.json", b"{}", "100644"),
                                 ("src/.env", b"", "100644"),
                                 ("src/a.exe", b"MZ", "100644")):
            with self.subTest(name=name, mode=mode):
                self.assertTrue(gate.violations(name, data, mode))

    def test_personal_paths_device_ids_and_delegations_are_rejected(self):
        samples = [b"/" + b"home/private-user/source", b"GPU-" + b"12345678-1234-1234-1234-123456789abc",
                   b"pci:" + b"abcd:" + b"12:34.0", b"/esig:" + b"a" * 128,
                   b"keryx:" + b"a" * 61]
        for data in samples:
            with self.subTest(length=len(data)):
                self.assertTrue(gate.violations("src/example.cpp", data))

    def test_inert_identity_does_not_authorize_a_real_gpu(self):
        self.assertEqual(gate.violations("src/example.cpp", gate.SYNTHETIC_UUID.encode()), [])

    def test_only_sha_pinned_gallery_images_are_allowed(self):
        import json
        hashes = json.loads((ROOT / "tools/publication-policy.json").read_text())["public_screenshot_sha256"]
        old_hashes = gate.PUBLIC_SCREENSHOT_HASHES
        gate.PUBLIC_SCREENSHOT_HASHES = hashes
        self.addCleanup(setattr, gate, "PUBLIC_SCREENSHOT_HASHES", old_hashes)
        path = "docs/screenshots/01-overview.png"
        data = (ROOT / path).read_bytes()
        self.assertEqual(gate.violations(path, data), [])
        self.assertTrue(gate.violations(path, data + b"x"))
        self.assertTrue(gate.violations(path, data, "120000"))
        self.assertTrue(gate.violations("docs/screenshots/unreviewed.png", data))
        self.assertTrue(gate.violations(path, b"\x89PNG\r\n\x1a\n" + b"x" * 64))


@unittest.skipUnless(shutil.which("gitleaks"), "Gitleaks required for integration tests")
class GitBoundaries(unittest.TestCase):
    def setUp(self):
        persistent = ROOT / ".local/tmp"
        persistent.mkdir(parents=True, exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(prefix="gate-test-", dir=persistent)
        self.addCleanup(self.directory.cleanup)
        self.repo = Path(self.directory.name)
        for name in ("tools/security_gate.py", "tools/publication-policy.json", ".gitleaks.toml"):
            output = self.repo / name
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, output)
        self.git("init", "-b", "main")
        self.git("config", "user.name", "Fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        (self.repo / "src").mkdir()

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.repo), *args], check=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def check_gate(self, *args):
        return subprocess.run([sys.executable, str(self.repo / "tools/security_gate.py"), *args],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def stage(self):
        self.git("add", "tools", ".gitleaks.toml", "src")

    def test_clean_snapshot_passes(self):
        (self.repo / "src/safe.cpp").write_text("int main() {}\n")
        self.stage()
        self.assertEqual(self.check_gate("--index").returncode, 0)

    def test_unstaged_cleanup_cannot_hide_staged_private_data(self):
        target = self.repo / "src/sample.cpp"
        target.write_text("/" + "home/private-user/source")
        self.stage()
        target.write_text("safe working copy")
        self.assertNotEqual(self.check_gate("--index").returncode, 0)

    def test_deleted_secret_still_blocks_history(self):
        target = self.repo / "src/sample.cpp"
        target.write_text("gh" + "p_" + "Ab9" * 14)
        self.stage()
        self.git("commit", "-m", "Synthetic unsafe history")
        self.git("rm", "src/sample.cpp")
        self.git("commit", "-m", "Remove current file")
        self.assertNotEqual(self.check_gate("--history").returncode, 0)

    def test_gitleaks_detects_staged_secret_outside_builtin_patterns(self):
        target = self.repo / "src/sample.cpp"
        target.write_text('api_key = "' + "aZ9bX7cV5dF3gH1jK8mN6pQ4rS2tU0wY" + '"\n')
        self.stage()
        target.write_text("safe working copy")
        self.assertNotEqual(self.check_gate("--index").returncode, 0)

    def test_missing_scanner_blocks_commit(self):
        (self.repo / "src/safe.cpp").write_text("safe")
        self.stage()
        import os
        env = os.environ.copy()
        # Keep Git available while deliberately excluding the scanner.
        commands = self.repo / "commands"
        commands.mkdir()
        (commands / "git").symlink_to(shutil.which("git"))
        env["PATH"] = str(commands)
        result = subprocess.run([sys.executable, str(self.repo / "tools/security_gate.py"), "--index"],
                                env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"mandatory", result.stderr)


class PushBoundaries(unittest.TestCase):
    def setUp(self):
        persistent = ROOT / ".local/tmp"
        persistent.mkdir(parents=True, exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(prefix="push-test-", dir=persistent)
        self.addCleanup(self.directory.cleanup)
        self.commands = Path(self.directory.name)
        self.scan_log = self.commands / "scan.log"
        fixtures = {
            "gh": '#!/bin/sh\nprintf "%s\\n" "$HOOK_TEST_REPOSITORY"\nexit "$HOOK_TEST_GH_STATUS"\n',
            "python3": '#!/bin/sh\nprintf "%s\\n" "$@" > "$HOOK_TEST_SCAN_LOG"\nexit "$HOOK_TEST_SCAN_STATUS"\n',
        }
        for name, script in fixtures.items():
            command = self.commands / name
            command.write_text(script)
            command.chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.commands),
                        HOOK_TEST_REPOSITORY="tonythewizard/nexus-keryx-miner",
                        HOOK_TEST_GH_STATUS="0", HOOK_TEST_SCAN_STATUS="0",
                        HOOK_TEST_SCAN_LOG=str(self.scan_log))

    def run_hook(self, destination="https://github.com/tonythewizard/nexus-keryx-miner.git"):
        return subprocess.run(["/bin/sh", str(ROOT / ".githooks/pre-push"), "origin", destination],
                              env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def test_authorized_destinations_require_history_scan(self):
        for destination in ("https://github.com/tonythewizard/nexus-keryx-miner.git",
                            "git@github.com:tonythewizard/nexus-keryx-miner.git"):
            with self.subTest(destination=destination):
                self.scan_log.unlink(missing_ok=True)
                self.assertEqual(self.run_hook(destination).returncode, 0)
                self.assertEqual(self.scan_log.read_text().splitlines(),
                                 ["tools/security_gate.py", "--history"])

    def test_unapproved_destination_is_rejected(self):
        self.assertNotEqual(self.run_hook("https://github.com/example/other.git").returncode, 0)
        self.assertFalse(self.scan_log.exists())

    def test_changed_repository_identity_is_rejected(self):
        self.env["HOOK_TEST_REPOSITORY"] = "example/other"
        self.assertNotEqual(self.run_hook().returncode, 0)
        self.assertFalse(self.scan_log.exists())

    def test_github_failure_cannot_pass_even_with_matching_output(self):
        self.env["HOOK_TEST_GH_STATUS"] = "1"
        self.assertNotEqual(self.run_hook().returncode, 0)
        self.assertFalse(self.scan_log.exists())

    def test_history_scan_failure_blocks_push(self):
        self.env["HOOK_TEST_SCAN_STATUS"] = "1"
        self.assertNotEqual(self.run_hook().returncode, 0)
        self.assertTrue(self.scan_log.exists())


if __name__ == "__main__":
    unittest.main()
