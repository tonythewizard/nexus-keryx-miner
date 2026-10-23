"""Bound test file cache using the test's delegated cgroup, never host reclaim."""
from __future__ import annotations

import errno
import json
from pathlib import Path
import threading


def file_cache_bytes(text: str) -> int:
    values = dict(line.split() for line in text.splitlines())
    # Shared memory is swap-backed; do not count it as disposable file cache.
    return max(0, int(values["file"]) - int(values["shmem"]))


class ScopedFileCache:
    """Harness guard; requires a dedicated, delegated test service."""

    def __init__(self) -> None:
        relative = next(line[3:] for line in Path("/proc/self/cgroup").read_text().splitlines()
                        if line.startswith("0::"))
        self.root = Path("/sys/fs/cgroup") / relative.lstrip("/")
        name = self.root.name
        if not (name.startswith("nexus-dual-") and name.endswith(".service")):
            raise ValueError("File-cache reclaim requires a dedicated nexus-dual-*.service")
        # A successful zero-byte request verifies permissions and kernel support
        # before any GPU/model work. Never fall back to host reclaim/swappiness.
        (self.root / "memory.reclaim").write_text("0 swappiness=0")
        self.stop = threading.Event()
        self.error: str | None = None
        self.attempts = 0
        self.partial = 0
        self.peak_file = 0
        self.thread = threading.Thread(target=self._run, name="test-file-cache", daemon=True)

    def _run(self) -> None:
        try:
            while not self.stop.is_set():
                cached = file_cache_bytes((self.root / "memory.stat").read_text())
                self.peak_file = max(self.peak_file, cached)
                if cached > 256 * 1024**2:
                    self.attempts += 1
                    try:
                        (self.root / "memory.reclaim").write_text("134217728 swappiness=0")
                    except OSError as exc:
                        if exc.errno != errno.EAGAIN:
                            raise
                        # Reclaim may free less than requested. This is not OOM.
                        self.partial += 1
                self.stop.wait(0.02)
        except (OSError, ValueError, KeyError) as exc:
            self.error = str(exc)

    def __enter__(self) -> ScopedFileCache:
        self.thread.start()
        return self

    def __exit__(self, *_: object) -> None:
        self.stop.set()
        self.thread.join(timeout=5)
        if self.thread.is_alive():
            self.error = "Scoped file reclaim did not finish within cleanup deadline"
        print("TEST_FILE_CACHE=" + json.dumps({
            "attempts": self.attempts, "partial": self.partial,
            "observed_file_peak_bytes": self.peak_file,
            "error": self.error, "swappiness": 0,
        }), flush=True)
        if self.error:
            raise RuntimeError(self.error)
