#!/usr/bin/env python3
"""Bounded, hash-chained M2.2.12 CUDA microbatch runner.

Each CUDA launch is independently guarded and independently checked by the Rust
oracle before its nonce range is committed to the journal. The journal never
turns an interrupted or partially checked artifact into a verified checkpoint.
"""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import time
import uuid

import gguf_journal_identity as input_identity

ROOT = pathlib.Path(os.environ.get("NEXUS_DATA_DIR", str(pathlib.Path.home() / ".local/share/nexus-keryx-miner")))
SOURCE = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/m22"
CUDA_BIN = BUILD / "bin/nexus-cuda-real-model"
ORACLE = ROOT / "build/cargo/release/nexus-m21-oracle"
CAPTURE = ROOT / "artifacts/live/live-mainnet.capture"
MODEL = pathlib.Path(os.environ.get("NEXUS_DATA_DIR", str(pathlib.Path.home() / ".local/share/nexus-keryx-miner"))) / "legacy/models/Qwen3.5-9B-abliterated/model.gguf"
TREE = pathlib.Path(os.environ.get("NEXUS_DATA_DIR", str(pathlib.Path.home() / ".local/share/nexus-keryx-miner"))) / "legacy/models/Qwen3.5-9B-abliterated/pom-tree.bin"
TREE_META = pathlib.Path(os.environ.get("NEXUS_DATA_DIR", str(pathlib.Path.home() / ".local/share/nexus-keryx-miner"))) / "legacy/models/Qwen3.5-9B-abliterated/pom-tree.json"
INDEX = ROOT / "artifacts/proofs/m22/20261009T002432/canonical-index-cpp.bin"
BUILD_PROVENANCE = ROOT / "artifacts/build/m2212-cuda-build-provenance.json"
GPU_UUID = os.environ.get("NEXUS_PRIMARY_GPU_UUID", "GPU-00000000-0000-0000-0000-000000000000")
NONCE_BASE = 0x4B455259584D3231
MICROBATCH = 4
MAX_SEGMENTS_PER_RUN = 3
MAX_RUNTIME_SECONDS = 600
GPU_GUARD_SECONDS = 120
HEADER_BYTES = 108
RECORD_BYTES = 16 + 256 * 1024 + 256 * 8 + 257 * 1024
LOGICAL_RANGES = ((1, 0), (8, 1), (64, 9), (256, 73))
ADAPTIVE_SCHEMA = 3
LEGACY_COMMIT = "aac6e6fec023544270a5d47a8d25ddce95ab9eb5"
JOURNAL_DIR = ROOT / "checkpoints/m2218/resumable"
JOURNAL = JOURNAL_DIR / "journal.jsonl"
LOCK = JOURNAL_DIR / "journal.lock"


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_identity(path: pathlib.Path) -> dict:
    stat = path.stat()
    if not path.is_file():
        raise RuntimeError(f"required regular file missing: {path}")
    return {"size": stat.st_size, "mtime_ns": stat.st_mtime_ns, "inode": stat.st_ino,
            "device": stat.st_dev}


def run_checked(argv: list[str], log: pathlib.Path, *, timeout: int = 300,
                cwd: pathlib.Path | None = None) -> str:
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("wb") as output:
        process = subprocess.run(argv, cwd=cwd, env=os.environ.copy(), stdout=output,
                                 stderr=subprocess.STDOUT, timeout=timeout, check=False)
    text = log.read_text(errors="replace")
    if process.returncode:
        raise RuntimeError(f"command exit={process.returncode}: {' '.join(argv)}; log={log}\n{text[-3000:]}")
    return text


def ensure_persistent_environment() -> None:
    required = ("TMPDIR", "TMP", "TEMP", "CUDA_CACHE_PATH", "CARGO_TARGET_DIR", "CARGO_HOME",
                "XDG_CACHE_HOME", "PYTHONPYCACHEPREFIX", "CCACHE_DIR", "PIP_CACHE_DIR")
    root = ROOT.resolve(strict=True)
    bad = []
    for name in required:
        raw = os.environ.get(name)
        if not raw:
            bad.append(f"{name}=unset")
            continue
        resolved = pathlib.Path(raw).resolve(strict=False)
        if resolved != root and root not in resolved.parents:
            bad.append(f"{name} escapes persistent workspace: {resolved}")
    if bad:
        raise RuntimeError("persistent paths BLOCKED: " + "; ".join(bad))
    if os.environ.get("CUDA_VISIBLE_DEVICES") != GPU_UUID or os.environ.get("NEXUS_CUDA_DEVICE_UUID") != GPU_UUID:
        raise RuntimeError("GPU environment must expose only the authorized primary NVIDIA GPU UUID")


def gpu_identity() -> dict:
    text = run_checked([sys.executable, str(SOURCE / "scripts/m223_gpu_identity.py")],
                       ROOT / "logs/m2212-gpu-identity.log", timeout=20)
    if "name=NVIDIA GeForce primary NVIDIA GPU" not in text or f"uuid={GPU_UUID}" not in text or "compute_capability=8.6" not in text:
        raise RuntimeError("authorized primary NVIDIA GPU identity output did not match expected values")
    return {"name": "NVIDIA GeForce primary NVIDIA GPU", "uuid": GPU_UUID, "sm": "8.6"}


def non_binary_change(path: str) -> bool:
    # Deliberately narrow: CMake, Rust, C++, CUDA, headers, schemas and lockfiles
    # cannot be waived by a different delivery commit.
    return (path.startswith(("reports/", "scripts/")) or
            (path.startswith("tests/") and path.endswith(".py")))


def verify_compatible_build_sources(build_commit: str, runner_commit: str) -> dict:
    receipt = ROOT / "logs/m2214-20261009T134922Z/build-input-output-hashes.json"
    expected_sha = "231ded6b446b0bee1985cd7ae91f95136b836a106b5f1bca291342d29be2091f"
    if sha256_file(receipt) != expected_sha:
        raise RuntimeError("M2214 dependency receipt SHA-256 mismatch")
    data = json.loads(receipt.read_text())
    if data["build_source_commit"] != build_commit:
        raise RuntimeError("M2214 dependency receipt source commit mismatch")
    changed = subprocess.run(["git", "-C", str(SOURCE), "diff", "--name-only",
                              build_commit, runner_commit], check=True,
                             text=True, capture_output=True).stdout.splitlines()
    incompatible = [p for p in changed if not non_binary_change(p)]
    if incompatible:
        raise RuntimeError("compiled dependency changed since attestation: " + ",".join(incompatible))
    checked = 0
    for raw, identity in data["files"].items():
        path = pathlib.Path(raw)
        if path.is_relative_to(SOURCE) and non_binary_change(str(path.relative_to(SOURCE))):
            continue
        if not path.is_file() or sha256_file(path) != identity["sha256"]:
            raise RuntimeError(f"M2214 build dependency/artifact mismatch: {path}")
        checked += 1
    return {"receipt_sha256": expected_sha, "verified_files": checked,
            "build_source_commit": build_commit, "runner_commit": runner_commit}


def verify_build_provenance(commit: str) -> dict:
    if not BUILD_PROVENANCE.is_file():
        raise RuntimeError("exact current CUDA build provenance is missing; refusing stale binary")
    data = json.loads(BUILD_PROVENANCE.read_text())
    build_commit = data.get("git_commit")
    compatibility = verify_compatible_build_sources(build_commit, commit)
    checks = {
        "git_commit": build_commit,
        "gpu_uuid": GPU_UUID,
        "cuda_arch": "sm_86",
        "cuda_binary_sha256": sha256_file(CUDA_BIN),
        "rust_oracle_sha256": sha256_file(ORACLE),
        "cuda_host_source_sha256": sha256_file(SOURCE / "src/model/cuda_real_model.cpp"),
        "cuda_kernel_source_sha256": sha256_file(SOURCE / "consensus/cuda_walk.cu"),
        "rust_batch_oracle_source_sha256": sha256_file(SOURCE / "oracle-rust/src/cuda_batch.rs"),
        "host_object_sha256": sha256_file(BUILD / "CMakeFiles/nexus-cuda-real-model.dir/src/model/cuda_real_model.cpp.o"),
        "cuda_object_sha256": sha256_file(BUILD / "CMakeFiles/nexus_cuda.dir/consensus/cuda_walk.cu.o"),
        "cuda_library_sha256": sha256_file(BUILD / "libnexus_cuda.a"),
    }
    for key, expected in checks.items():
        if data.get(key) != expected:
            raise RuntimeError(f"CUDA build provenance mismatch for {key}")
    if data.get("sm_86_elf_verified") is not True or data.get("ninja_noop_verified") is not True:
        raise RuntimeError("CUDA build provenance lacks sm_86/no-op attestations")
    return {"sha256": sha256_file(BUILD_PROVENANCE), "commit": build_commit,
            "cuda_binary_sha256": checks["cuda_binary_sha256"],
            "compatibility": compatibility}


def current_fingerprint(existing_model: dict | None = None, observations=None) -> dict:
    for path in (CUDA_BIN, ORACLE, CAPTURE, MODEL, TREE, TREE_META, INDEX):
        if not path.is_file():
            raise RuntimeError(f"required offline input is missing: {path}")
    commit = subprocess.run(["git", "-C", str(SOURCE), "rev-parse", "HEAD"], text=True,
                            capture_output=True, check=True).stdout.strip()
    if subprocess.run(["git", "-C", str(SOURCE), "status", "--porcelain"], text=True,
                      capture_output=True, check=True).stdout.strip():
        raise RuntimeError("source worktree is dirty; commit the exact implementation before running")
    build = verify_build_provenance(commit)
    from m224_gpu_guard import verify_host_memory
    def read_guard():
        verify_host_memory(mode="functional")
    observations = observations if observations is not None else {}
    model_content = input_identity.content_identity(MODEL, expected=existing_model,
                            guard=read_guard, observations=observations, streaming=True)
    tree_content = input_identity.content_identity(TREE, guard=read_guard, observations=observations, streaming=True)
    metadata_content = input_identity.content_identity(TREE_META, observations=observations)
    index_content = input_identity.content_identity(INDEX, observations=observations)
    capture_content = input_identity.content_identity(CAPTURE, observations=observations)
    consensus = input_identity.consensus_binding(json.loads(TREE_META.read_text()),
                                                 model_content, tree_content)
    input_identity.verify_unchanged(observations)
    # Delivery/report commits are not compiler inputs. Bind reusable results
    # to the build source and exact runner contents; retain execution commit
    # separately in session_start instead of rewriting historical attestation.
    build["compatibility"].pop("runner_commit")
    runner_sources = {str(p.relative_to(SOURCE)): sha256_file(p)
                      for p in sorted((SOURCE / "scripts").rglob("*"))
                      if p.is_file() and p.suffix in (".py", ".fish")}
    return {
        "git_commit": build["commit"],
        "runner_sources_sha256": hashlib.sha256(canonical(runner_sources)).hexdigest(),
        "cuda_binary_sha256": sha256_file(CUDA_BIN),
        "oracle_sha256": sha256_file(ORACLE),
        "gpu": gpu_identity(),
        "identity_schema": 2,
        "journal_schema": ADAPTIVE_SCHEMA,
        "physical_sizes": [1, 2, 4],
        "gguf": model_content,
        "tree": tree_content,
        "model_consensus": consensus,
        "index_sha256": index_content["sha256"],
        "capture_sha256": capture_content["sha256"],
        "tree_metadata_sha256": metadata_content["sha256"],
        "build_provenance": build,
        "nonce_base": NONCE_BASE,
        "ranges": {str(size): start for size, start in LOGICAL_RANGES},
        "microbatch": MICROBATCH,
        "submit": "DISABLED",
    }


class Journal:
    def __init__(self, path: pathlib.Path):
        self.path = path
        self.entries: list[dict] = []
        self.last_hash = "0" * 64
        if path.exists():
            self._read()

    def _read(self) -> None:
        previous = "0" * 64
        entries = []
        for line_no, raw in enumerate(self.path.read_bytes().splitlines(), 1):
            try:
                item = json.loads(raw)
            except Exception as exc:
                raise RuntimeError(f"journal line {line_no} is invalid JSON: {exc}") from exc
            claimed = item.pop("sha256", None)
            if item.get("seq") != line_no or item.get("previous_sha256") != previous:
                raise RuntimeError(f"journal sequence/hash-chain mismatch at line {line_no}")
            actual = hashlib.sha256(canonical(item)).hexdigest()
            if claimed != actual:
                raise RuntimeError(f"journal SHA-256 mismatch at line {line_no}")
            item["sha256"] = claimed
            entries.append(item)
            previous = claimed
        self.entries, self.last_hash = entries, previous

    def append(self, kind: str, payload: dict) -> dict:
        body = {"seq": len(self.entries) + 1, "previous_sha256": self.last_hash,
                "kind": kind, "payload": payload}
        item = dict(body, sha256=hashlib.sha256(canonical(body)).hexdigest())
        self.path.parent.mkdir(parents=True, exist_ok=True)
        content = b"".join(canonical(x) + b"\n" for x in [*self.entries, item])
        pending = self.path.with_name(self.path.name + ".pending-" + uuid.uuid4().hex)
        fd = os.open(pending, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            with os.fdopen(fd, "wb") as stream:
                stream.write(content)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(pending, self.path)
            dir_fd = os.open(self.path.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(dir_fd)
            finally:
                os.close(dir_fd)
        except Exception:
            raise
        self.entries.append(item)
        self.last_hash = item["sha256"]
        return item

    def identity(self) -> dict | None:
        for item in self.entries:
            if item["kind"] == "identity":
                return item["payload"]["fingerprint"]
        return None

    def verified_segments(self) -> dict[tuple[int, int], dict]:
        identity = self.identity()
        adaptive = identity is not None and identity.get("journal_schema") == ADAPTIVE_SCHEMA
        result = self.imported_segments() if adaptive else {}
        seen_nonces = {nonce for payload in result.values() for nonce in payload["nonces"]}
        expected_segments = {}
        for logical, range_start in LOGICAL_RANGES:
            local = 0
            while local < logical:
                n = min(MICROBATCH, logical - local)
                expected_segments[(logical, range_start + local)] = n
                local += n
        for item in self.entries:
            if item["kind"] != "checkpoint":
                continue
            p = item["payload"]
            if identity is None or p.get("fingerprint") != identity:
                raise RuntimeError("checkpoint fingerprint differs from journal identity")
            key = (int(p["logical_batch"]), int(p["start_offset"]))
            if key in result:
                raise RuntimeError(f"duplicate checkpoint for logical batch/range {key}")
            count = int(p.get("count", -1))
            if adaptive:
                validate_segment_shape(key[0], key[1], count)
                if count != adaptive_count(key[0], key[1], p.get("planner_authorized", 0),
                                           p.get("physical_limit", 0)):
                    raise RuntimeError("checkpoint physical size differs from planner authorization")
            elif key not in expected_segments or count != expected_segments[key]:
                raise RuntimeError(f"checkpoint has unexpected logical/physical segment shape: {key}")
            if p.get("safety") != "PASS" or p.get("cpu_cuda") != "PASS" or p.get("rust_batch") != "PASS" or p.get("rust_proofs") != "PASS":
                raise RuntimeError(f"checkpoint {key} lacks complete safety/consensus verification")
            nonces = p.get("nonces", [])
            expected = list(range(int(p["nonce_start"]), int(p["nonce_start"]) + int(p["count"])))
            if nonces != expected or any(n in seen_nonces for n in nonces):
                raise RuntimeError(f"checkpoint {key} has missing, reordered or duplicate nonces")
            if int(p.get("nonce_start", -1)) != NONCE_BASE + key[1]:
                raise RuntimeError(f"checkpoint {key} nonce base does not match deterministic range")
            artifact_paths = {artifact.get("path") for artifact in p.get("artifacts", [])}
            if not any(path and path.endswith(".nxcb") for path in artifact_paths):
                raise RuntimeError(f"checkpoint {key} lacks the verified NXCB segment artifact")
            for nonce in nonces:
                for suffix in (".bin", ".context", ".trace"):
                    if not any(path and path.endswith(f"proof-nonce-{nonce}{suffix}") for path in artifact_paths):
                        raise RuntimeError(f"checkpoint {key} lacks proof artifact nonce={nonce} suffix={suffix}")
            log_paths = {entry.get("path") for entry in p.get("logs", [])}
            required_log_tokens = ("m2212-guard-", "m2212-memory-", "m2212-rust-batch-")
            if any(not any(token in (path or "") for path in log_paths) for token in required_log_tokens):
                raise RuntimeError(f"checkpoint {key} lacks a safety, resource or Rust batch log")
            if sum("m2212-rust-proof-" in (path or "") for path in log_paths) != len(nonces):
                raise RuntimeError(f"checkpoint {key} lacks one Rust proof log per nonce")
            for artifact in p.get("artifacts", []):
                path = ROOT / artifact["path"]
                if not path.is_file() or sha256_file(path) != artifact["sha256"]:
                    raise RuntimeError(f"checkpoint artifact missing/corrupt: {path}")
            for artifact in p.get("logs", []):
                path = ROOT / artifact["path"]
                if not path.is_file() or sha256_file(path) != artifact["sha256"]:
                    raise RuntimeError(f"checkpoint log missing/corrupt: {path}")
                content = path.read_text(errors="replace")
                name = path.name
                if "m2212-guard-" in name and "M224_CUDA_CHILD=PASS" not in content:
                    raise RuntimeError(f"checkpoint safety log does not contain child PASS: {path}")
                if "m2212-memory-" in name:
                    budget_match = re.search(r"M22_MICROBATCH=(\d+)", content)
                    if not budget_match:
                        raise RuntimeError(f"checkpoint memory preflight log is incomplete: {path}")
                    if adaptive and int(budget_match.group(1)) != p["planner_authorized"]:
                        raise RuntimeError("checkpoint planner authorization differs from memory log")
                if "m2212-rust-batch-" in name and "OFFICIAL_RUST_CUDA_SEGMENT=PASS" not in content:
                    raise RuntimeError(f"checkpoint Rust batch log is incomplete: {path}")
                if "m2212-rust-proof-" in name and "OFFICIAL_RUST_REAL_MODEL=PASS" not in content:
                    raise RuntimeError(f"checkpoint Rust proof log is incomplete: {path}")
            seen_nonces.update(nonces)
            result[key] = p
        if adaptive:
            validate_coverage(result)
        return result

    def imported_segments(self):
        entry = next((e for e in self.entries if e["kind"] == "identity"), None)
        migration = entry["payload"].get("migration") if entry else None
        if migration is None:
            return {}
        if migration.get("source_commit") != LEGACY_COMMIT:
            raise RuntimeError("unapproved legacy migration commit")
        snapshot = self.path.parent / "legacy-journal.jsonl"
        if sha256_file(snapshot) != migration["source_sha256"]:
            raise RuntimeError("legacy snapshot hash mismatch")
        original = pathlib.Path(migration["original_journal"])
        if not original.resolve().is_relative_to((ROOT / "checkpoints").resolve()):
            raise RuntimeError("legacy original escapes persistent checkpoints")
        if sha256_file(original) != migration["source_sha256"]:
            raise RuntimeError("preserved original journal changed")
        old = Journal(snapshot)
        if old.identity() is None or old.identity().get("journal_schema") == ADAPTIVE_SCHEMA:
            raise RuntimeError("only fixed-schema legacy journals may be imported")
        verify_migration_binding(old.identity(), self.identity())
        imported = old.verified_segments()
        validate_coverage(imported)
        if len(imported) != migration["checkpoint_count"] or sum(p["count"] for p in imported.values()) != migration["nonce_count"]:
            raise RuntimeError("legacy migration coverage mismatch")
        return imported


def parse_batch_artifact(path: pathlib.Path, expected_count: int, nonce_start: int) -> tuple[dict, list[dict]]:
    expected_size = HEADER_BYTES + RECORD_BYTES * expected_count
    if path.stat().st_size != expected_size:
        raise RuntimeError(f"NXCB size mismatch: actual={path.stat().st_size} expected={expected_size}")
    records = []
    with path.open("rb") as stream:
        header = stream.read(HEADER_BYTES)
        if len(header) != HEADER_BYTES or header[:4] != b"NXCB":
            raise RuntimeError("NXCB header missing or truncated")
        version, count, ntiles = struct.unpack_from("<IIQ", header, 4)
        base, timestamp, daa = struct.unpack_from("<QQQ", header, 20)
        pph, target = header[44:76].hex(), header[76:108].hex()
        if version != 2 or count != expected_count or base != nonce_start:
            raise RuntimeError("NXCB version/count/nonce-base mismatch")
        for i in range(expected_count):
            raw = stream.read(RECORD_BYTES)
            if len(raw) != RECORD_BYTES:
                raise RuntimeError(f"NXCB record {i} truncated")
            seed, nonce = struct.unpack_from("<QQ", raw, 0)
            if nonce != nonce_start + i:
                raise RuntimeError(f"NXCB nonce order mismatch record={i} actual={nonce}")
            offsets_start = 16 + 256 * 1024
            states_start = offsets_start + 256 * 8
            offsets = [struct.unpack_from("<Q", raw, offsets_start + j * 8)[0] for j in range(256)]
            states = raw[states_start:]
            if len(states) != 257 * 1024:
                raise RuntimeError(f"NXCB state payload size mismatch nonce={nonce}")
            records.append({"nonce": nonce, "seed": seed, "offset_count": len(offsets),
                            "offsets_sha256": hashlib.sha256(raw[offsets_start:states_start]).hexdigest(),
                            "state_count": len(states) // 1024,
                            "final_state_sha256": hashlib.sha256(states[-1024:]).hexdigest()})
    return {"version": version, "count": count, "ntiles": ntiles, "nonce_start": base,
            "timestamp": timestamp, "daa": daa, "pre_pow_hash": pph, "target_le": target}, records


def same_job(anchor: dict, header: dict, capture_digest: str, generation: int, daa: int) -> bool:
    # Segment size and nonce base must differ across a resumed logical batch.
    # parse_batch_artifact validates those against the requested segment separately.
    invariant_fields = ("version", "ntiles", "timestamp", "daa", "pre_pow_hash", "target_le")
    return (capture_digest == anchor["capture_digest"] and
            generation == int(anchor["job_generation"]) and daa == int(anchor["daa"]) and
            all(header[key] == anchor["header"][key] for key in invariant_fields))


def rel_artifact(path: pathlib.Path) -> dict:
    resolved = path.resolve(strict=True)
    if ROOT not in resolved.parents:
        raise RuntimeError(f"artifact escapes persistent workspace: {resolved}")
    return {"path": str(resolved.relative_to(ROOT)), "sha256": sha256_file(resolved),
            "size": resolved.stat().st_size}


def validate_segment_shape(logical: int, offset: int, count: int) -> None:
    starts = dict(LOGICAL_RANGES)
    if logical not in starts or count not in (1, 2, 4):
        raise RuntimeError("invalid adaptive physical segment size or logical batch")
    local = offset - starts[logical]
    if local < 0 or local + count > logical or local % 4 + count > 4:
        raise RuntimeError("adaptive segment exceeds logical range or four-nonce cell")


def validate_coverage(completed: dict) -> None:
    """A partial prefix is resumable; internal holes/overlaps are corruption."""
    expected_keys = set()
    previous_incomplete = False
    for logical, start in LOGICAL_RANGES:
        cursor = start
        rows = sorted((key, p) for key, p in completed.items() if key[0] == logical)
        if previous_incomplete and rows:
            raise RuntimeError("missing nonce coverage in preceding logical batch")
        for key, payload in rows:
            count = int(payload["count"])
            validate_segment_shape(logical, key[1], count)
            if key[1] != cursor:
                raise RuntimeError("missing or overlapping nonce coverage")
            expected_keys.add(key)
            cursor += count
        if cursor != start + logical:
            previous_incomplete = True
    if expected_keys != set(completed):
        raise RuntimeError("unknown logical batch in coverage")


def adaptive_count(logical: int, offset: int, planned: int, physical_limit: int = 4) -> int:
    if planned not in (1, 2, 4, 8) or physical_limit not in (1, 2, 4):
        raise RuntimeError("planner authorizes no supported nonce count")
    local = offset - dict(LOGICAL_RANGES)[logical]
    remaining = min(4 - local % 4, logical - local)
    choices = [n for n in (1, 2, 4) if n <= min(planned, physical_limit, remaining)]
    if not choices:
        raise RuntimeError("planner authorizes no nonce in the remaining segment")
    return max(choices)


def next_segment(completed: dict[tuple[int, int], dict]) -> tuple[int, int, int] | None:
    validate_coverage(completed)
    for logical, start in LOGICAL_RANGES:
        count = sum(p["count"] for key, p in completed.items() if key[0] == logical)
        if count < logical:
            offset = start + count
            return logical, offset, adaptive_count(logical, offset, MICROBATCH)
    return None


def legacy_runner_sources():
    names = subprocess.check_output(
        ["git", "-C", str(SOURCE), "ls-tree", "-r", "--name-only", LEGACY_COMMIT, "--", "scripts"],
        text=True).splitlines()
    return {name: hashlib.sha256(subprocess.check_output(
        ["git", "-C", str(SOURCE), "show", f"{LEGACY_COMMIT}:{name}"])).hexdigest()
            for name in names if pathlib.Path(name).suffix in (".py", ".fish")}


def verify_migration_binding(old: dict, new: dict) -> None:
    if old.get("identity_schema") != 2 or old.get("journal_schema") is not None:
        raise RuntimeError("migration requires verified content-based fixed schema")
    expected = dict(old, journal_schema=ADAPTIVE_SCHEMA, physical_sizes=[1, 2, 4],
                    runner_sources_sha256=new.get("runner_sources_sha256"))
    require_fingerprint(new, expected)
    legacy_sources = legacy_runner_sources()
    if hashlib.sha256(canonical(legacy_sources)).hexdigest() != old["runner_sources_sha256"]:
        raise RuntimeError("legacy runner fingerprint does not match pinned source commit")
    current = {str(p.relative_to(SOURCE)): sha256_file(p)
               for p in sorted((SOURCE / "scripts").rglob("*"))
               if p.is_file() and p.suffix in (".py", ".fish")}
    if hashlib.sha256(canonical(current)).hexdigest() != new["runner_sources_sha256"]:
        raise RuntimeError("current runner fingerprint differs from migration")
    changed = {name for name in set(current) | set(legacy_sources)
               if current.get(name) != legacy_sources.get(name)}
    if changed != {"scripts/m2212_resumable_cuda.py"}:
        raise RuntimeError("migration permits only the audited orchestration change; "
                           "guard/consensus/input changes are incompatible: " + repr(sorted(changed)))


def prepare_migration(journal: Journal, source_path: pathlib.Path, fingerprint: dict, observations: dict):
    """Single atomic identity record refers to an immutable, verbatim legacy snapshot."""
    old = Journal(source_path)
    verify_migration_binding(old.identity(), fingerprint)
    imported = old.verified_segments()
    validate_coverage(imported)
    source_hash = sha256_file(source_path)
    snapshot = journal.path.parent / "legacy-journal.jsonl"
    if snapshot.exists():
        if sha256_file(snapshot) != source_hash:
            raise RuntimeError("existing legacy snapshot differs; refusing overwrite")
    else:
        data = source_path.read_bytes()
        if hashlib.sha256(data).hexdigest() != source_hash:
            raise RuntimeError("legacy source changed during snapshot")
        with snapshot.open("xb") as f:
            f.write(data); f.flush(); os.fsync(f.fileno())
    if sha256_file(source_path) != source_hash or sha256_file(snapshot) != source_hash:
        raise RuntimeError("legacy source/snapshot changed")
    journal.append("identity", {"fingerprint": fingerprint, "input_observations": observations,
        "migration": {"source_commit": LEGACY_COMMIT, "original_journal": str(source_path),
                      "source_sha256": source_hash, "checkpoint_count": len(imported),
                      "nonce_count": sum(p["count"] for p in imported.values()),
                      "semantics": "retain original payloads/fingerprints; no new GPU execution"}})
    journal.verified_segments()


def require_fingerprint(actual: dict, expected: dict) -> None:
    if actual != expected:
        differing = sorted(key for key in set(actual) | set(expected) if actual.get(key) != expected.get(key))
        raise RuntimeError("configuration fingerprint mismatch: " + ",".join(differing))


def run_session(max_segments: int, reinitialize_from: pathlib.Path | None = None,
                migrate_from: pathlib.Path | None = None, physical_limit: int = 4) -> int:
    ensure_persistent_environment()
    if physical_limit not in (1, 2, 4):
        raise RuntimeError("physical limit must be 1, 2 or 4")
    if migrate_from is not None and reinitialize_from is not None:
        raise RuntimeError("migration and empty reinitialization are mutually exclusive")
    if not 1 <= max_segments <= MAX_SEGMENTS_PER_RUN:
        raise RuntimeError(f"max segments per invocation must be 1..{MAX_SEGMENTS_PER_RUN}")
    JOURNAL_DIR.mkdir(parents=True, exist_ok=True)
    with LOCK.open("a+b") as lock_stream:
        fcntl.flock(lock_stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        journal = Journal(JOURNAL)
        previous_fp = journal.identity()
        if previous_fp and previous_fp.get("journal_schema") != ADAPTIVE_SCHEMA:
            raise RuntimeError("fixed-schema journal preserved; explicitly --migrate-from it "
                               "into a distinct --journal-dir")
        if previous_fp and previous_fp.get("identity_schema") != 2:
            raise RuntimeError("legacy metadata-based journal preserved; use a distinct --journal-dir "
                               "and --reinitialize-from ORIGINAL_JOURNAL after content verification")
        origin = None
        legacy_fp = None
        source_lock = None
        try:
            if migrate_from:
                if previous_fp is not None:
                    raise RuntimeError("--migrate-from requires an empty destination journal")
                path = migrate_from.resolve(strict=True)
                if not path.is_relative_to((ROOT / "checkpoints").resolve()) or path == JOURNAL.resolve():
                    raise RuntimeError("migration source must be a distinct persistent checkpoint")
                # Existing source lock is opened read-only; the legacy directory is not written.
                source_lock = path.with_name("journal.lock").open("rb")
                fcntl.flock(source_lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
                legacy_fp = Journal(path).identity()
                if legacy_fp is None:
                    raise RuntimeError("legacy journal identity missing")
                migrate_from = path
            if reinitialize_from:
                if previous_fp is not None:
                    raise RuntimeError("--reinitialize-from requires an empty destination journal")
                path = reinitialize_from.resolve(strict=True)
                if not path.is_relative_to((ROOT / "checkpoints").resolve()) or path == JOURNAL.resolve():
                    raise RuntimeError("source journal must be a distinct persistent checkpoint")
                source_lock = path.with_name("journal.lock").open("a+b")
                fcntl.flock(source_lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
                old = Journal(path)
                completed = old.verified_segments()
                if completed:
                    raise RuntimeError("source journal has verified segments; automatic migration forbidden; "
                                       "preserve and audit consensus/runner provenance explicitly")
                legacy_fp = old.identity()
                if legacy_fp is None:
                    raise RuntimeError("source journal has no model identity")
                origin = {"path": str(path), "sha256": sha256_file(path),
                          "verified_nonces": 0, "segments_imported": 0}
            observations = {}
            expected_model = (previous_fp or legacy_fp or {}).get("gguf")
            fingerprint = current_fingerprint(expected_model, observations)
            if legacy_fp and not migrate_from:
                # Model contents were fully reread against the old GGUF digest.
                # The old metadata hash also binds the sidecar tree's digest.
                for key in ("index_sha256", "capture_sha256", "tree_metadata_sha256",
                            "cuda_binary_sha256", "oracle_sha256", "git_commit",
                            "gpu", "nonce_base", "ranges", "microbatch", "submit"):
                    if fingerprint.get(key) != legacy_fp.get(key):
                        raise RuntimeError(f"journal reinitialization binding mismatch: {key}")
                if sha256_file(reinitialize_from) != origin["sha256"]:
                    raise RuntimeError("source journal changed during reinitialization")
            if migrate_from:
                prepare_migration(journal, migrate_from, fingerprint, observations)
            elif previous_fp is None:
                journal.append("identity", {"fingerprint": fingerprint,
                                             "input_observations": observations,
                                             "reinitialized_from": origin})
            else:
                require_fingerprint(fingerprint, previous_fp)
        finally:
            if source_lock is not None:
                source_lock.close()
        run_id = time.strftime("%Y%m%dT%H%M%S", time.gmtime()) + "-" + uuid.uuid4().hex[:8]
        run_dir = ROOT / "artifacts/proofs/m2212" / run_id
        run_dir.mkdir(parents=True, exist_ok=False)
        baseline = {"runner_git_commit": subprocess.check_output(
                        ["git", "-C", str(SOURCE), "rev-parse", "HEAD"], text=True).strip(),
                    "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                    "input_observations": observations,
                    "run_id": run_id, "attempt_limit": max_segments,
                    "max_runtime_seconds": MAX_RUNTIME_SECONDS}
        journal.append("session_start", baseline)
        started = time.monotonic()
        attempts = 0
        while attempts < max_segments:
            remaining = MAX_RUNTIME_SECONDS - (time.monotonic() - started)
            if remaining < GPU_GUARD_SECONDS + 60:
                journal.append("session_blocked", {"run_id": run_id, "reason": "session time budget exhausted"})
                print(f"M2212_SESSION=BLOCKED reason=time_budget run={run_id}")
                return 13
            input_identity.verify_unchanged(observations)
            completed = journal.verified_segments()
            segment = next_segment(completed)
            if segment is None:
                journal.append("session_complete", {"run_id": run_id, "all_logical_batches_complete": True})
                print(f"M2212_ALL_LOGICAL_BATCHES=PASS run={run_id} journal={JOURNAL}")
                return 0
            logical, offset, count = segment
            attempts += 1
            nonce_start = NONCE_BASE + offset
            memory_log = ROOT / f"logs/m2212-memory-{run_id}-b{logical}-o{offset}.log"
            guard_log = ROOT / f"logs/m2212-guard-{run_id}-b{logical}-o{offset}.log"
            try:
                preflight_log = run_checked([sys.executable, str(SOURCE / "scripts/m22_memory_budget.py"),
                                             "--phase", "cuda", "--logical-batch", str(max(8, count))],
                                            memory_log, timeout=20)
                planned_match = re.search(r"M22_MICROBATCH=(\d+)", preflight_log)
                planned = int(planned_match.group(1)) if planned_match else 0
                count = adaptive_count(logical, offset, planned, physical_limit)
            except Exception as exc:
                journal.append("segment_blocked", {"run_id": run_id, "logical_batch": logical,
                    "start_offset": offset, "count": count, "reason": str(exc),
                    "memory_log": rel_artifact(memory_log) if memory_log.exists() else None})
                print(f"M2212_SEGMENT=BLOCKED batch={logical} offset={offset} memory_preflight={exc}")
                return 13
            attempt_dir = run_dir / f"logical-{logical}-offset-{offset}-count-{count}"
            attempt_dir.mkdir(parents=True, exist_ok=False)
            journal.append("segment_attempt", {"run_id": run_id, "attempt": attempts,
                "logical_batch": logical, "start_offset": offset, "count": count,
                "nonce_start": nonce_start, "planner_authorized": planned,
                "physical_limit": physical_limit, "memory_log_sha256": sha256_file(memory_log)})
            guard_cmd = [sys.executable, str(SOURCE / "scripts/m224_gpu_guard.py"),
                         "--mode", "functional", "--allow-exe", str(CUDA_BIN),
                         "--samples", "6", "--max-runtime-seconds", str(GPU_GUARD_SECONDS), "--",
                         str(CUDA_BIN), str(CAPTURE), str(MODEL), str(TREE), str(TREE_META),
                         str(count), str(attempt_dir), str(count), str(offset)]
            try:
                guard_text = run_checked(guard_cmd, guard_log, timeout=GPU_GUARD_SECONDS + 60, cwd=ROOT)
            except Exception as exc:
                journal.append("segment_blocked", {"run_id": run_id, "logical_batch": logical,
                    "start_offset": offset, "count": count, "reason": str(exc),
                    "memory_log": rel_artifact(memory_log), "guard_log": rel_artifact(guard_log)})
                print(f"M2212_SEGMENT=BLOCKED batch={logical} offset={offset} count={count} guard_log={guard_log}")
                return 13
            if "M224_CUDA_CHILD=PASS" not in guard_text:
                journal.append("segment_blocked", {"run_id": run_id, "logical_batch": logical,
                    "start_offset": offset, "count": count, "reason": "guard did not emit child PASS",
                    "memory_log": rel_artifact(memory_log), "guard_log": rel_artifact(guard_log)})
                return 13
            batch_file = attempt_dir / f"batch-{count}.nxcb"
            rust_batch_log = ROOT / f"logs/m2212-rust-batch-{run_id}-b{logical}-o{offset}.log"
            try:
                rust_batch_text = run_checked([str(ORACLE), "verify-cuda-batch", str(MODEL),
                                               str(INDEX), str(batch_file)], rust_batch_log,
                                              timeout=GPU_GUARD_SECONDS)
                if "OFFICIAL_RUST_CUDA_SEGMENT=PASS" not in rust_batch_text:
                    raise RuntimeError("Rust batch verifier did not report PASS")
                header, records = parse_batch_artifact(batch_file, count, nonce_start)
                proof_logs = []
                for record in records:
                    nonce = record["nonce"]
                    prefix = attempt_dir / f"proof-nonce-{nonce}"
                    proof_log = ROOT / f"logs/m2212-rust-proof-{run_id}-n{nonce}.log"
                    proof_text = run_checked([str(ORACLE), "verify-real", str(prefix) + ".context",
                                              str(prefix) + ".trace", str(prefix) + ".bin"],
                                             proof_log, timeout=60)
                    if "OFFICIAL_RUST_REAL_MODEL=PASS" not in proof_text:
                        raise RuntimeError(f"Rust real-proof verifier did not report PASS for nonce={nonce}")
                    proof_logs.append(rel_artifact(proof_log))
            except Exception as exc:
                journal.append("segment_failed", {"run_id": run_id, "logical_batch": logical,
                    "start_offset": offset, "count": count, "reason": str(exc),
                    "guard_log": rel_artifact(guard_log), "memory_log": rel_artifact(memory_log),
                    "rust_batch_log": rel_artifact(rust_batch_log) if rust_batch_log.exists() else None})
                print(f"M2212_SEGMENT=FAIL batch={logical} offset={offset} reason={exc}")
                return 1
            artifacts = [rel_artifact(p) for p in sorted(attempt_dir.iterdir()) if p.is_file()]
            logs = [rel_artifact(memory_log), rel_artifact(guard_log), rel_artifact(rust_batch_log)] + proof_logs
            output_match = re.search(r"capture_digest=([0-9a-f]+).*generation=(\d+).*daa=(\d+).*nonce_base=(\d+).*seed0=(\d+)",
                                     guard_text.replace("\n", " "))
            if not output_match:
                journal.append("segment_failed", {"run_id": run_id, "logical_batch": logical,
                    "start_offset": offset, "count": count, "reason": "missing job/seed metadata in runner output",
                    "guard_log": rel_artifact(guard_log)})
                return 1
            existing = list(journal.verified_segments().values())
            if existing:
                anchor = existing[0]
                if not same_job(anchor, header, output_match.group(1),
                                int(output_match.group(2)), int(output_match.group(3))):
                    journal.append("segment_failed", {"run_id": run_id, "logical_batch": logical,
                        "start_offset": offset, "count": count,
                        "reason": "selected template/header/DAA differs from prior verified checkpoints",
                        "guard_log": rel_artifact(guard_log)})
                    return 1
            payload = {"run_id": run_id, "logical_batch": logical, "start_offset": offset,
                "fingerprint": fingerprint,
                "count": count, "planner_authorized": planned, "physical_limit": physical_limit,
                "nonce_start": nonce_start, "nonces": [x["nonce"] for x in records],
                "capture_digest": output_match.group(1), "job_generation": int(output_match.group(2)),
                "daa": int(output_match.group(3)), "seed0": int(output_match.group(5)),
                "header": header, "records": records, "safety": "PASS", "cpu_cuda": "PASS",
                "rust_batch": "PASS", "rust_proofs": "PASS", "target_check": "PASS",
                "target_candidates": int(re.search(r"target_candidates=(\d+)", rust_batch_text).group(1))
                    if re.search(r"target_candidates=(\d+)", rust_batch_text) else None,
                "artifacts": artifacts, "logs": logs}
            input_identity.verify_unchanged(observations)
            journal.append("checkpoint", payload)
            print(f"M2212_MICROBATCH=PASS logical_batch={logical} range={offset}:{offset+count} "
                  f"verified={count}/{count} gpu={GPU_UUID} rust_batch=PASS rust_proofs=PASS "
                  f"journal_sha256={journal.last_hash}")
        journal.append("session_yield", {"run_id": run_id, "reason": "per-invocation microbatch cap",
            "attempts": attempts, "next": next_segment(journal.verified_segments())})
        print(f"M2212_SESSION=INCOMPLETE run={run_id} completed_microbatches={attempts} "
              f"resume_command='fish {SOURCE}/scripts/run-m2212.fish --journal-dir {JOURNAL_DIR}'")
        return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--max-microbatches", type=int, default=MAX_SEGMENTS_PER_RUN)
    parser.add_argument("--journal-dir", type=pathlib.Path,
                        help="new persistent journal for a changed runner; old journals remain immutable")
    parser.add_argument("--reinitialize-from", type=pathlib.Path,
                        help="preserve an empty legacy journal and verify all input bindings in a new one")
    parser.add_argument("--migrate-from", type=pathlib.Path,
                        help="explicitly import verified fixed-schema attestations into a new adaptive journal")
    parser.add_argument("--physical-limit", type=int, choices=(1, 2, 4), default=4,
                        help="cap only; every launch still requires the unchanged planner and safety guard")
    args = parser.parse_args()
    if args.journal_dir:
        global JOURNAL_DIR, JOURNAL, LOCK
        selected = args.journal_dir.resolve()
        if not selected.is_relative_to((ROOT / "checkpoints").resolve()):
            raise RuntimeError("journal must remain under persistent workspace checkpoints")
        JOURNAL_DIR = selected
        JOURNAL = selected / "journal.jsonl"
        LOCK = selected / "journal.lock"
    try:
        return run_session(args.max_microbatches, args.reinitialize_from,
                           args.migrate_from, args.physical_limit)
    except Exception as exc:
        print(f"M2212_ORCHESTRATOR=BLOCKED reason={exc}", file=sys.stderr)
        return 13


if __name__ == "__main__":
    raise SystemExit(main())
