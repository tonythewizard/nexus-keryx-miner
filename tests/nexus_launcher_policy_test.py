#!/usr/bin/env python3
import pathlib
import os
import hashlib
import re
import unittest

ROOT = pathlib.Path(__file__).parents[1]
RECOVERY = pathlib.Path(os.environ.get("NEXUS_TEST_RECOVERY_ROOT", str(ROOT.parent)))


class LauncherPolicyTests(unittest.TestCase):
    def test_shadow_live_forces_environment_and_worker_mode_off(self):
        fish = (ROOT / "scripts/nexusmine.fish").read_text(encoding="utf-8")
        branch = fish.split("    case shadow-live", 1)[1].split("    case live", 1)[0]
        self.assertIn("set -gx NEXUS_SUBMIT_ENABLED 0", branch)
        self.assertIn("set -e NEXUS_SUBMIT_ENDPOINT", branch)
        self.assertRegex(branch, r"set -l expected_hash [0-9a-f]{64}")
        self.assertIn("shadow --no-submit", branch)

    def test_force_shadow_worker_env_is_forced_off(self):
        helper = (ROOT / "scripts/nexus_force_shadow.py").read_text(encoding="utf-8")
        self.assertIn('worker_env["NEXUS_SUBMIT_ENABLED"] = "0"', helper)
        self.assertIn('worker_env.pop("NEXUS_SUBMIT_ENDPOINT", None)', helper)
        self.assertIn('"shadow", "--no-submit"]', helper)

    def test_live_requires_exact_cli_opt_in_and_uses_attested_worker(self):
        fish = (ROOT / "scripts/nexusmine.fish").read_text(encoding="utf-8")
        branch = fish.split("    case live", 1)[1].split("    case shadow-replay", 1)[0]
        self.assertIn("test \"$argv[2]\" != --enable-submit", branch)
        self.assertIn("set -l submit_flag --no-submit", branch)
        self.assertIn("set submit_flag --enable-submit", branch)
        self.assertRegex(branch, r"set -l expected_hash [0-9a-f]{64}")
        pinned_hashes = re.findall(r"set -l expected_hash ([0-9a-f]{64})", fish)
        self.assertEqual(len(pinned_hashes), 3)
        self.assertEqual(len(set(pinned_hashes)), 1)
        self.assertIn('"$out" 0 4 live "$submit_flag"', branch)

    def test_candidate_launcher_is_separate_fail_closed_and_tui_read_only(self):
        candidate = (ROOT / "scripts/nexusmine-next.fish").read_text(encoding="utf-8")
        self.assertIn("--enable-submit", candidate)
        self.assertIn("--enable-ai-response", candidate)
        self.assertIn("audit-ai", candidate)
        audit_branch = candidate.split('if test "$argv[1]" = audit-ai', 1)[1].split(
            'if test "$argv[1]" != live', 1)[0]
        self.assertIn("set -gx NEXUS_SUBMIT_ENABLED 0", audit_branch)
        self.assertIn("set -e CUDA_VISIBLE_DEVICES", audit_branch)
        self.assertIn("transactions=DISABLED", audit_branch)
        self.assertIn('"$out" 0 4 live "$submit_arg" "$ai_response_arg"', candidate)
        self.assertIn("if not $enable_ai_response", candidate)
        self.assertIn("set -gx NEXUS_SUBMIT_ENABLED 0", candidate)
        self.assertIn("nexus_no_duplicate_worker.py", candidate)
        self.assertIn("nexus_dashboard.py", candidate)
        self.assertIn("--no-tui", candidate)
        candidate_hashes = re.findall(r"set -l expected_sha256 ([0-9a-f]{64})", candidate)
        self.assertEqual(len(candidate_hashes), 1)
        # Verify the actual path selected by the launcher, including an isolated candidate build.
        match = re.search(r'set -l worker "\$recovery/([^"\n]+)"', candidate)
        self.assertIsNotNone(match, "launcher must select an explicit persistent worker")
        recovery_match = re.search(r'set -l recovery (/[^\n]+)', candidate)
        self.assertIsNotNone(recovery_match)
        worker = pathlib.Path(recovery_match.group(1)) / match.group(1)
        self.assertTrue(worker.is_file(), "attested candidate worker is missing")
        if candidate_hashes[0] == "0" * 64:
            self.assertIn("candidate binary SHA-256 is not pinned", candidate)
        else:
            self.assertEqual(candidate_hashes[0], hashlib.sha256(worker.read_bytes()).hexdigest())
        self.assertIn('"state"/"ai-service"/"ai-response.journal"',
                      (ROOT / "src/model/live_cuda_worker.cpp").read_text(encoding="utf-8"))
        self.assertNotIn("exec \"$worker\"", candidate)

    def test_vulkan_ai_is_explicit_pinned_and_checked_after_worker_hash(self):
        candidate=(ROOT / "scripts/nexusmine-next.fish").read_text()
        self.assertIn('set -l ai_backend auto',candidate)
        self.assertIn('case --ai-backend=auto --ai-backend=cuda --ai-backend=vulkan',candidate)
        self.assertIn('if test "$ai_backend" = auto',candidate)
        self.assertIn('if test "$gpu_selection[1]" = 1\n        set ai_backend cuda',candidate)
        self.assertIn('else if test "$gpu_selection[2]" = 1\n        set ai_backend vulkan',candidate)
        self.assertIn('selected CUDA GPU is OFF',candidate)
        self.assertIn('NEXUS_AI=BLOCKED Vulkan forbids CUDA inference fallback',candidate)
        self.assertIn('NEXUS_VULKAN_AI_HELPER_SHA256',candidate)
        self.assertIn('NEXUS_VULKAN_AI_LIBRARY_SHA256',candidate)
        self.assertLess(candidate.index('set -l actual_hash'),candidate.index('"$worker" --inference-capabilities'))

    def test_escrow_import_pin_matches_immutable_backup(self):
        source = (ROOT / "src/model/live_cuda_worker.cpp").read_text(encoding="utf-8")
        expected = "c890191b98ad5f0bf6e63fd974ad2e0c2d0eb1ee4347f49d8e8bb43447cd5f3e"
        self.assertIn(expected, source)
        backup = RECOVERY / "artifacts/escrow-backups/20261011/stock-state.json"
        self.assertEqual(hashlib.sha256(backup.read_bytes()).hexdigest(), expected)


if __name__ == "__main__":
    unittest.main(verbosity=2)
