"""Content identities for immutable validation inputs; paths/stats are observations."""
import hashlib
import os
import pathlib
import stat
import time

# Official H6 tier 0: consensus/core/src/config/params.rs (v1.6.4).
MODEL_ID = "bd34568cd89f5f19c6c3a6e1a61b929bc868709409eaad8e672d85f3c1eb5710"
ROOT = "2c497164eaf20078add20e82ae4e1b0fdb27d3fdd5eaefc1c48f204111e14e88"


def stat_token(s):
    return (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)


def observe(path):
    p = pathlib.Path(path)
    return {"absolute": str(p.absolute()), "canonical": str(p.resolve(strict=True)),
            "symlink_target": os.readlink(p) if p.is_symlink() else None,
            "stat_token": list(stat_token(p.stat()))}


def content_identity(path, *, expected=None, guard=None, observations=None, streaming=False):
    """Always read the bytes, even when size, inode and timestamps are unchanged.

    Metadata only detects races within this read/session. It is never a durable
    identity across reboots, copies, remounts or symlink/path changes.
    """
    path = pathlib.Path(path)
    before = observe(path)
    if guard:
        guard()
    digest = hashlib.sha256()
    count = 0
    with path.open("rb", buffering=0) as stream:
        initial = os.fstat(stream.fileno())
        if not stat.S_ISREG(initial.st_mode) or list(stat_token(initial)) != before["stat_token"]:
            raise RuntimeError(f"input changed before content read: {path}")
        started = time.monotonic()
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
            count += len(chunk)
            if streaming:
                # Same per-file consumed-page advice as ReadOnlyGguf::recompute_root.
                # No global cache operation; no file contents or metadata writes.
                os.posix_fadvise(stream.fileno(), count - len(chunk), len(chunk),
                                 os.POSIX_FADV_DONTNEED)
                delay = count / (64 * 1024 * 1024) - (time.monotonic() - started)
                if delay > 0:
                    time.sleep(delay)
            if guard and count % (16 * 1024 * 1024) == 0:
                guard()
        if stat_token(os.fstat(stream.fileno())) != stat_token(initial):
            raise RuntimeError(f"input changed during content read: {path}")
    if before != observe(path) or count != initial.st_size:
        raise RuntimeError(f"input path/metadata changed during content read: {path}")
    if guard:
        guard()
    identity = {"size": count, "sha256": digest.hexdigest()}
    if expected is not None:
        old_size = expected.get("size", expected.get("file_identity", {}).get("size"))
        if identity["size"] != old_size or identity["sha256"] != expected.get("sha256"):
            raise RuntimeError(f"GGUF content mismatch: {path}; expected size={old_size} "
                               f"sha256={expected.get('sha256')}, actual={identity}; "
                               "revalidate canonical index, independent root and consensus")
    if observations is not None:
        observations[str(path)] = before
    return identity


def verify_unchanged(observations):
    for path, original in observations.items():
        if observe(path) != original:
            raise RuntimeError(f"validation input changed during session: {path}")


def consensus_binding(metadata, model, tree):
    expected = {"format_version": 1, "layout_version": 1, "chunk_size": 32,
                "checkpoint_interval": 6, "n_chunks": 203469888,
                "gguf_size": model["size"], "tree_size": tree["size"]}
    for key, value in expected.items():
        if metadata.get(key) != value:
            raise RuntimeError(f"sidecar consensus metadata mismatch: {key}")
    for key, value in (("model_id", MODEL_ID), ("root", ROOT), ("tree_sha256", tree["sha256"])):
        if bytes(metadata.get(key, [])).hex() != value:
            raise RuntimeError(f"sidecar consensus/hash mismatch: {key}")
    return {"tier": 0, "model_id": MODEL_ID, "root": ROOT,
            "n_chunks": 203469888, "chunk_size": 32}
