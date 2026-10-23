#!/usr/bin/env python3
"""Offline journal/resume regression tests; all scratch stays in workspace/work."""
import importlib.util
import pathlib
import tempfile
import sys
import os
from unittest import mock
import unittest

SCRIPT = pathlib.Path(__file__).parents[1] / "scripts" / "m2212_resumable_cuda.py"
sys.path.insert(0, str(SCRIPT.parent))
spec = importlib.util.spec_from_file_location("m2212_resumable_cuda", SCRIPT)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def persistent_test_directory():
    root = pathlib.Path(__file__).resolve().parents[1] / ".local/tmp"
    root.mkdir(parents=True, exist_ok=True)
    return root


class ResumableJournalTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=persistent_test_directory())
        self.old_root = runner.ROOT
        runner.ROOT = pathlib.Path(self.temp.name)
        self.path = runner.ROOT / "checkpoints/journal.jsonl"
        self.journal = runner.Journal(self.path)
        self.journal.append("identity", {"fingerprint": {"git_commit": "a" * 40,
                                                              "template_sha256": "b" * 64}})

    def tearDown(self):
        runner.ROOT = self.old_root
        self.temp.cleanup()

    def checkpoint(self, logical=8, start=1, nonces=None, artifact_data=b"proof", count=None,
                   planned=4, physical_limit=4, safety="PASS"):
        range_start = dict(runner.LOGICAL_RANGES)[logical]
        count = count if count is not None else min(runner.MICROBATCH, logical - (start - range_start))
        nonce_start = runner.NONCE_BASE + start
        if nonces is None:
            nonces = list(range(nonce_start, nonce_start + count))
        artifact_paths = []
        segment_path = runner.ROOT / "artifacts" / f"batch-{logical}-{start}.nxcb"
        segment_path.parent.mkdir(parents=True, exist_ok=True)
        segment_path.write_bytes(artifact_data)
        artifact_paths.append(runner.rel_artifact(segment_path))
        for nonce in nonces:
            for suffix in (".bin", ".context", ".trace"):
                proof_path = runner.ROOT / "artifacts" / f"proof-nonce-{nonce}{suffix}"
                proof_path.write_bytes(artifact_data)
                artifact_paths.append(runner.rel_artifact(proof_path))
        log_specs = {
            f"m2212-guard-{logical}-{start}.log": "M224_CUDA_CHILD=PASS",
            f"m2212-memory-{logical}-{start}.log": f"M22_MICROBATCH={planned}",
            f"m2212-rust-batch-{logical}-{start}.log": "OFFICIAL_RUST_CUDA_SEGMENT=PASS",
        }
        for nonce in nonces:
            log_specs[f"m2212-rust-proof-{nonce}.log"] = "OFFICIAL_RUST_REAL_MODEL=PASS"
        logs = []
        for name, contents in log_specs.items():
            log_path = runner.ROOT / "logs" / name
            log_path.parent.mkdir(parents=True, exist_ok=True)
            log_path.write_text(contents + "\n")
            logs.append(runner.rel_artifact(log_path))
        self.journal.append("checkpoint", {
            "logical_batch": logical, "start_offset": start, "count": count,
            "nonce_start": nonce_start, "nonces": nonces,
            "fingerprint": self.journal.identity(),
            "safety": safety, "cpu_cuda": "PASS", "rust_batch": "PASS", "rust_proofs": "PASS",
            "planner_authorized": planned, "physical_limit": physical_limit,
            "artifacts": artifact_paths, "logs": logs,
        })

    def test_hash_chain_survives_reopen_and_resume_finds_first_missing(self):
        self.checkpoint(logical=1, start=0, nonces=[runner.NONCE_BASE])
        self.checkpoint(logical=8, start=1)
        reopened = runner.Journal(self.path)
        done = reopened.verified_segments()
        self.assertEqual(runner.next_segment(done), (8, 5, 4))

    def test_hash_corruption_is_rejected(self):
        self.journal.append("session_start", {"run": "test"})
        raw = self.path.read_text()
        self.path.write_text(raw.replace('"run":"test"', '"run":"tampered"'))
        with self.assertRaisesRegex(RuntimeError, "SHA-256 mismatch"):
            runner.Journal(self.path)

    def test_incomplete_checkpoint_cannot_be_reused(self):
        self.journal.append("checkpoint", {"logical_batch": 8, "start_offset": 1,
            "count": 4, "nonce_start": runner.NONCE_BASE + 1,
            "nonces": [runner.NONCE_BASE + 1], "safety": "PASS", "cpu_cuda": "PASS",
            "rust_batch": "PASS", "rust_proofs": "PASS", "artifacts": [], "logs": [],
            "fingerprint": self.journal.identity()})
        with self.assertRaisesRegex(RuntimeError, "missing, reordered or duplicate"):
            self.journal.verified_segments()

    def test_corrupt_or_missing_artifact_blocks_resume(self):
        self.checkpoint()
        (runner.ROOT / "artifacts" / f"proof-nonce-{runner.NONCE_BASE+1}.bin").write_bytes(b"changed")
        with self.assertRaisesRegex(RuntimeError, "artifact missing/corrupt"):
            self.journal.verified_segments()

    def test_duplicate_nonce_across_checkpoints_is_rejected(self):
        self.checkpoint(logical=8, start=1)
        self.checkpoint(logical=8, start=5, nonces=[runner.NONCE_BASE + 1] * 3)
        with self.assertRaisesRegex(RuntimeError, "missing, reordered or duplicate"):
            self.journal.verified_segments()

    def test_changed_commit_or_template_fingerprint_is_not_compatible(self):
        fingerprint = {"git_commit": "a" * 40, "template_sha256": "b" * 64}
        path = runner.ROOT / "other-journal.jsonl"
        other = runner.Journal(path)
        other.append("identity", {"fingerprint": fingerprint})
        reopened = runner.Journal(path)
        self.assertEqual(reopened.identity(), fingerprint)
        with self.assertRaisesRegex(RuntimeError, "git_commit"):
            runner.require_fingerprint({"git_commit": "c" * 40, "template_sha256": "b" * 64},
                                       reopened.identity())
        with self.assertRaisesRegex(RuntimeError, "template_sha256"):
            runner.require_fingerprint({"git_commit": "a" * 40, "template_sha256": "d" * 64},
                                       reopened.identity())

    def test_missing_binary_attestation_fails_closed(self):
        old = runner.BUILD_PROVENANCE
        runner.BUILD_PROVENANCE = runner.ROOT / "missing-build-provenance.json"
        try:
            with self.assertRaisesRegex(RuntimeError, "provenance is missing"):
                runner.verify_build_provenance("a" * 40)
        finally:
            runner.BUILD_PROVENANCE = old

    def test_guard_block_event_does_not_mark_segment_complete(self):
        self.journal.append("segment_attempt", {"logical_batch": 1, "start_offset": 0})
        self.journal.append("segment_blocked", {"logical_batch": 1, "start_offset": 0,
                                                "reason": "safety guard blocked"})
        self.assertEqual(self.journal.verified_segments(), {})
        self.assertEqual(runner.next_segment({}), (1, 0, 1))

    def test_corrupt_proof_artifact_hash_is_detected(self):
        self.checkpoint(artifact_data=b"original proof")
        (runner.ROOT / "artifacts" / f"proof-nonce-{runner.NONCE_BASE+1}.bin").write_bytes(b"corrupt proof")
        with self.assertRaisesRegex(RuntimeError, "artifact missing/corrupt"):
            self.journal.verified_segments()

    def test_next_segment_keeps_job_despite_different_count_and_nonce(self):
        header = dict(version=2, count=1, ntiles=123, nonce_start=runner.NONCE_BASE,
                      timestamp=987, daa=456, pre_pow_hash="a" * 64, target_le="b" * 64)
        anchor = dict(header=header, capture_digest="c" * 64, job_generation=9, daa=456)
        following = dict(header, count=4, nonce_start=runner.NONCE_BASE + 1)
        self.assertNotEqual(header, following)  # Reproduces the old full-header comparison.
        self.assertTrue(runner.same_job(anchor, following, "c" * 64, 9, 456))
        for field in ("version", "ntiles", "timestamp", "daa", "pre_pow_hash", "target_le"):
            with self.subTest(field=field):
                changed = dict(following, **{field: "different"})
                self.assertFalse(runner.same_job(anchor, changed, "c" * 64, 9, 456))
        self.assertFalse(runner.same_job(anchor, following, "d" * 64, 9, 456))
        self.assertFalse(runner.same_job(anchor, following, "c" * 64, 10, 456))
        self.assertFalse(runner.same_job(anchor, following, "c" * 64, 9, 457))



class DeliveryCompatibilityTests(unittest.TestCase):
    def test_guard_change_invalidates_results_even_with_same_attested_binary(self):
        a = {"git_commit": "9fdf20204627e1a41c239bde379b24099ca1f085",
             "runner_sources_sha256": "a"*64, "cuda_binary_sha256": "b"*64}
        b = dict(a, runner_sources_sha256="c"*64)
        with self.assertRaisesRegex(RuntimeError, "runner_sources_sha256"):
            runner.require_fingerprint(b, a)

    def test_only_scripts_reports_and_python_tests_are_binary_independent(self):
        for p in ("scripts/m224_gpu_monitor.py", "reports/M2215.md", "tests/m224_gpu_attribution_test.py"):
            self.assertTrue(runner.non_binary_change(p))
        for p in ("consensus/cuda_walk.cu", "src/model/cuda_real_model.cpp",
                  "include/keryx/gguf.hpp", "oracle-rust/src/cuda_batch.rs",
                  "oracle-rust/Cargo.lock", "CMakeLists.txt", "tests/header_test.cpp"):
            self.assertFalse(runner.non_binary_change(p))



class ReinitializationTests(unittest.TestCase):
    setUp = ResumableJournalTests.setUp
    tearDown = ResumableJournalTests.tearDown
    checkpoint = ResumableJournalTests.checkpoint

    def attempt(self, fingerprint):
        destination = runner.ROOT / "checkpoints/new"
        with mock.patch.multiple(runner, JOURNAL_DIR=destination,
                                 JOURNAL=destination/"journal.jsonl",
                                 LOCK=destination/"journal.lock"), \
             mock.patch.object(runner, "ensure_persistent_environment"), \
             mock.patch.object(runner, "current_fingerprint", return_value=fingerprint), \
             mock.patch.object(runner, "run_checked", side_effect=RuntimeError("test resource block")), \
             mock.patch.object(runner.subprocess, "check_output", return_value="fixture-commit\n"):
            return runner.run_session(1, self.path)

    def test_empty_legacy_reinitializes_distinct_journal_without_importing_results(self):
        original = self.path.read_bytes()
        fp = dict(self.journal.identity(), identity_schema=2)
        self.assertEqual(self.attempt(fp), 13)
        self.assertEqual(self.path.read_bytes(), original)
        new = runner.Journal(runner.ROOT/"checkpoints/new/journal.jsonl")
        self.assertEqual(new.verified_segments(), {})
        self.assertEqual(new.entries[0]["payload"]["reinitialized_from"]["sha256"],
                         runner.sha256_file(self.path))
        self.assertEqual(new.identity(), fp)

    def test_completed_segments_forbid_automatic_reinitialization(self):
        self.checkpoint(logical=1, start=0, nonces=[runner.NONCE_BASE])
        original = self.path.read_bytes()
        with self.assertRaisesRegex(RuntimeError, "verified segments"):
            self.attempt(dict(self.journal.identity(), identity_schema=2))
        self.assertEqual(self.path.read_bytes(), original)
        self.assertFalse((runner.ROOT/"checkpoints/new/journal.jsonl").exists())

    def test_changed_template_binding_blocks_new_journal(self):
        original = self.path.read_bytes()
        with self.assertRaisesRegex(RuntimeError, "capture_sha256"):
            self.attempt(dict(self.journal.identity(), identity_schema=2, capture_sha256="a"*64))
        self.assertEqual(self.path.read_bytes(), original)
        self.assertFalse((runner.ROOT/"checkpoints/new/journal.jsonl").exists())


class ContentIdentityTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory(dir=persistent_test_directory())
        self.path = pathlib.Path(self.work.name) / "model.gguf"
        self.path.write_bytes(b"authentic-test-weights")
        self.identity = runner.input_identity.content_identity(self.path)

    def tearDown(self):
        self.work.cleanup()

    def test_identical_content_different_path_and_inode_matches(self):
        other = self.path.with_name("copy.gguf")
        other.write_bytes(self.path.read_bytes())
        self.assertNotEqual(self.path.stat().st_ino, other.stat().st_ino)
        self.assertEqual(runner.input_identity.content_identity(other, expected=self.identity), self.identity)

    def test_symlink_spelling_does_not_change_content_identity(self):
        link = self.path.with_name("alias.gguf")
        link.symlink_to(self.path)
        self.assertEqual(runner.input_identity.content_identity(link, expected=self.identity), self.identity)

    def test_changed_same_name_and_size_is_rejected_even_with_restored_mtime(self):
        original = self.path.stat()
        self.path.write_bytes(b"X" * original.st_size)
        os.utime(self.path, ns=(original.st_atime_ns, original.st_mtime_ns))
        with self.assertRaisesRegex(RuntimeError, "GGUF content mismatch"):
            runner.input_identity.content_identity(self.path, expected=self.identity)

    def test_legacy_device49_to53_is_not_content_identity(self):
        previous = {"path": "/old/mount/model.gguf",
                    "file_identity": {"size": self.path.stat().st_size, "device": 49,
                                      "inode": 9999, "mtime_ns": 123},
                    "sha256": self.identity["sha256"]}
        self.assertEqual(runner.input_identity.content_identity(self.path, expected=previous), self.identity)

    def test_touched_metadata_accepted_only_after_actual_rehash(self):
        original = self.path.stat()
        os.utime(self.path, ns=(original.st_atime_ns, original.st_mtime_ns + 100))
        with mock.patch.object(runner.input_identity.hashlib, "sha256",
                               wraps=runner.input_identity.hashlib.sha256) as hashing:
            self.assertEqual(runner.input_identity.content_identity(self.path, expected=self.identity), self.identity)
            hashing.assert_called_once()

    def test_session_mutation_blocks_even_after_successful_initial_hash(self):
        observations = {}
        runner.input_identity.content_identity(self.path, observations=observations)
        self.path.write_bytes(b"X" * self.path.stat().st_size)
        with self.assertRaisesRegex(RuntimeError, "input changed during session"):
            runner.input_identity.verify_unchanged(observations)

    def test_concurrent_replacement_during_hash_is_rejected(self):
        def replace():
            copy = self.path.with_name("replacement.gguf")
            copy.write_bytes(self.path.read_bytes())
            os.replace(copy, self.path)
        with self.assertRaisesRegex(RuntimeError, "input changed before content read"):
            runner.input_identity.content_identity(self.path, guard=replace)

    def test_resource_guard_failure_does_not_return_an_identity(self):
        with self.assertRaisesRegex(RuntimeError, "pressure"):
            runner.input_identity.content_identity(self.path, guard=mock.Mock(side_effect=RuntimeError("pressure")))

    def test_tier_root_and_sidecar_bytes_are_bound(self):
        i = runner.input_identity
        model = {"size": 6522004864}
        tree = {"size": 103349888, "sha256": "aa"*32}
        metadata = {"format_version": 1, "layout_version": 1, "chunk_size": 32,
                    "checkpoint_interval": 6, "n_chunks": 203469888,
                    "gguf_size": model["size"], "tree_size": tree["size"],
                    "root": list(bytes.fromhex(i.ROOT)), "model_id": list(bytes.fromhex(i.MODEL_ID)),
                    "tree_sha256": list(bytes.fromhex(tree["sha256"]))}
        self.assertEqual(i.consensus_binding(metadata, model, tree)["tier"], 0)
        for field in ("root", "model_id", "tree_sha256"):
            bad = dict(metadata, **{field: [0]*32})
            with self.assertRaisesRegex(RuntimeError, field):
                i.consensus_binding(bad, model, tree)


class AdaptiveJournalTests(unittest.TestCase):
    checkpoint = ResumableJournalTests.checkpoint
    tearDown = ResumableJournalTests.tearDown

    def setUp(self):
        ResumableJournalTests.setUp(self)
        self.path = runner.ROOT / "checkpoints/adaptive/journal.jsonl"
        self.journal = runner.Journal(self.path)
        self.journal.append("identity", {"fingerprint": {
            "identity_schema": 2, "journal_schema": 3, "physical_sizes": [1,2,4],
            "git_commit": "a"*40, "capture_sha256": "b"*64, "gguf": {"sha256":"c"*64},
            "cuda_binary_sha256": "d"*64, "oracle_sha256": "e"*64}})
        self.checkpoint(logical=1, start=0)
        self.checkpoint(logical=8, start=1)
        self.checkpoint(logical=8, start=5)

    def test_four_becomes_two_plus_two(self):
        for offset in (9,11):
            self.assertEqual(runner.adaptive_count(64, offset, 2),2)
            self.checkpoint(64,offset,count=2,planned=2)
        done=runner.Journal(self.path).verified_segments()
        self.assertEqual(sum(p["count"] for k,p in done.items() if k[0]==64),4)
        self.assertEqual(runner.next_segment(done),(64,13,4))

    def test_four_becomes_four_singletons(self):
        for offset in range(9,13):
            self.checkpoint(64,offset,count=1,planned=1)
        self.assertEqual(runner.next_segment(self.journal.verified_segments()),(64,13,4))

    def test_changing_budget_preserves_four_nonce_boundaries(self):
        for offset,plan,expected in ((9,2,2),(11,1,1),(12,4,1),(13,4,4)):
            count=runner.adaptive_count(64,offset,plan)
            self.assertEqual(count,expected)
            self.checkpoint(64,offset,count=count,planned=plan)
        self.assertEqual(runner.next_segment(self.journal.verified_segments()),(64,17,4))

    def test_interrupt_and_exact_resume_without_repeating_verified_nonces(self):
        self.checkpoint(64,9,count=2,planned=2)
        self.journal.append("segment_blocked",{"logical_batch":64,"start_offset":11,"count":2})
        done=runner.Journal(self.path).verified_segments()
        self.assertEqual(runner.next_segment(done),(64,11,2))
        self.assertEqual(len(done),4)
        self.checkpoint(64,11,count=2,planned=2)
        self.assertEqual(runner.next_segment(self.journal.verified_segments()),(64,13,4))

    def test_gap_is_not_promoted_to_coverage(self):
        self.checkpoint(64,11,count=2,planned=2)
        with self.assertRaisesRegex(RuntimeError,"missing or overlapping"):
            self.journal.verified_segments()

    def test_duplicate_and_overlapping_ranges_rejected(self):
        self.checkpoint(64,9,count=2,planned=2)
        self.checkpoint(64,9,count=2,planned=2)
        with self.assertRaisesRegex(RuntimeError,"duplicate"):
            self.journal.verified_segments()

    def test_corrupt_native_proof_rejected(self):
        self.checkpoint(64,9,count=2,planned=2)
        (runner.ROOT/"artifacts"/f"proof-nonce-{runner.NONCE_BASE+9}.bin").write_bytes(b"bad")
        with self.assertRaisesRegex(RuntimeError,"artifact missing/corrupt"):
            self.journal.verified_segments()

    def test_failed_post_safety_is_not_a_checkpoint(self):
        self.checkpoint(64,9,count=2,planned=2,safety="BLOCKED")
        with self.assertRaisesRegex(RuntimeError,"complete safety"):
            self.journal.verified_segments()

    def test_zero_invalid_budget_and_boundaries_fail_closed(self):
        for budget in (0,-1,3,5):
            with self.assertRaisesRegex(RuntimeError,"planner authorizes"):
                runner.adaptive_count(64,9,budget)
        with self.assertRaisesRegex(RuntimeError,"cell"):
            runner.validate_segment_shape(64,12,2)
        self.assertEqual(runner.adaptive_count(64,9,8,2),2)

    def test_checkpoint_cannot_exceed_planner(self):
        self.checkpoint(64,9,count=4,planned=2)
        with self.assertRaisesRegex(RuntimeError,"planner authorization"):
            self.journal.verified_segments()

    def test_template_binary_model_verifier_mismatch(self):
        fp=self.journal.identity()
        for key in ("capture_sha256","cuda_binary_sha256","oracle_sha256","gguf","git_commit"):
            with self.subTest(key=key):
                with self.assertRaisesRegex(RuntimeError,key):
                    runner.require_fingerprint(dict(fp,**{key:"changed"}),fp)

    def test_runner_reruns_planner_and_does_not_commit_post_run_failure(self):
        fp=self.journal.identity()
        def command(argv,log,**kw):
            if "m22_memory_budget.py" in str(argv):
                text="M22_MICROBATCH=2 logical_batch=8 phase=cuda"
                log.parent.mkdir(parents=True,exist_ok=True);log.write_text(text)
                return text
            log.write_text("post-test safety BLOCKED")
            raise RuntimeError("post-test safety BLOCKED")
        with mock.patch.multiple(runner,JOURNAL_DIR=self.path.parent,JOURNAL=self.path,
                                  LOCK=self.path.parent/"journal.lock"),              mock.patch.object(runner,"ensure_persistent_environment"),              mock.patch.object(runner,"current_fingerprint",return_value=fp),              mock.patch.object(runner,"run_checked",side_effect=command) as invoked, mock.patch.object(runner.subprocess,"check_output",return_value="fixture-commit\n"):
            self.assertEqual(runner.run_session(3),13)
        fresh=runner.Journal(self.path)
        self.assertEqual(len(fresh.verified_segments()),3)
        attempt=next(e for e in fresh.entries if e["kind"]=="segment_attempt")
        self.assertEqual(attempt["payload"]["count"],2)
        self.assertEqual(attempt["payload"]["planner_authorized"],2)
        self.assertEqual(invoked.call_count,2)


class AdaptiveMigrationTests(unittest.TestCase):
    setUp = ResumableJournalTests.setUp
    tearDown = ResumableJournalTests.tearDown
    checkpoint = ResumableJournalTests.checkpoint

    def migrated(self):
        self.checkpoint(logical=1,start=0)
        self.checkpoint(logical=8,start=1)
        self.checkpoint(logical=8,start=5)
        destination=self.path.parent/"new/journal.jsonl"
        destination.parent.mkdir()
        new=runner.Journal(destination)
        fp=dict(self.journal.identity(),journal_schema=3,physical_sizes=[1,2,4])
        runner.prepare_migration(new,self.path,fp,{})
        return new

    def test_legacy_nine_results_imported_without_rewriting_attestations(self):
        with mock.patch.object(runner,"verify_migration_binding"):
            new=self.migrated()
            before=self.path.read_bytes()
            original=self.journal.verified_segments()
            self.assertEqual(new.verified_segments(),original)
            self.assertEqual(sum(p["count"] for p in original.values()),9)
            self.assertEqual((new.path.parent/"legacy-journal.jsonl").read_bytes(),before)
            self.assertEqual(self.path.read_bytes(),before)
            self.assertEqual(runner.next_segment(new.verified_segments()),(64,9,4))
            self.assertEqual(sum(e["kind"]=="checkpoint" for e in new.entries),0)

    def test_snapshot_corruption_blocks_migration_resume(self):
        with mock.patch.object(runner,"verify_migration_binding"):
            new=self.migrated()
            (new.path.parent/"legacy-journal.jsonl").write_bytes(b"corrupt")
            with self.assertRaisesRegex(RuntimeError,"snapshot hash"):
                new.verified_segments()

    def test_original_mutation_is_not_silently_accepted(self):
        with mock.patch.object(runner,"verify_migration_binding"):
            new=self.migrated()
            self.path.write_bytes(self.path.read_bytes()+b"\n")
            with self.assertRaisesRegex(RuntimeError,"original journal changed"):
                new.verified_segments()

    def test_migration_binding_checks_all_consensus_fields_and_guard_sources(self):
        import hashlib
        current={str(p.relative_to(runner.SOURCE)):runner.sha256_file(p)
                 for p in sorted((runner.SOURCE/"scripts").rglob("*"))
                 if p.is_file() and p.suffix in (".py",".fish")}
        old_sources=dict(current,**{"scripts/m2212_resumable_cuda.py":"f"*64})
        old=dict(identity_schema=2,git_commit="a"*40,capture_sha256="b"*64,
                 gguf={"sha256":"c"*64},cuda_binary_sha256="d"*64,oracle_sha256="e"*64,
                 runner_sources_sha256=hashlib.sha256(runner.canonical(old_sources)).hexdigest())
        new=dict(old,journal_schema=3,physical_sizes=[1,2,4],
                 runner_sources_sha256=hashlib.sha256(runner.canonical(current)).hexdigest())
        with mock.patch.object(runner,"legacy_runner_sources",return_value=old_sources):
            runner.verify_migration_binding(old,new)
            for key in ("git_commit","capture_sha256","gguf","cuda_binary_sha256","oracle_sha256"):
                with self.assertRaisesRegex(RuntimeError,key):
                    runner.verify_migration_binding(old,dict(new,**{key:"wrong"}))
        unsafe=dict(old_sources,**{"scripts/m224_gpu_guard.py":"f"*64})
        old=dict(old,runner_sources_sha256=hashlib.sha256(runner.canonical(unsafe)).hexdigest())
        with mock.patch.object(runner,"legacy_runner_sources",return_value=unsafe):
            with self.assertRaisesRegex(RuntimeError,"guard/consensus"):
                runner.verify_migration_binding(old,new)

if __name__ == "__main__":
    unittest.main(verbosity=2)
