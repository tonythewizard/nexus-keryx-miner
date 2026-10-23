#!/usr/bin/env python3
"""Create a content-addressed M2.7 build provenance manifest (no temp files)."""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import subprocess


def sha(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def version(argv: list[str]) -> str:
    result = subprocess.run(argv, check=True, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    return result.stdout.strip().splitlines()[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("workspace", type=pathlib.Path)
    parser.add_argument("--build", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--build-log", action="append", required=True, type=pathlib.Path)
    parser.add_argument("--ctest-log", required=True, type=pathlib.Path)
    args = parser.parse_args()
    root = args.workspace.resolve(strict=True)
    build = args.build.resolve(strict=True)
    output = args.output.resolve(strict=False)
    if root not in build.parents or root not in output.parents:
        raise SystemExit("build and manifest output must stay inside persistent workspace")
    cache = build / "CMakeCache.txt"
    cache_text = cache.read_text()
    if not re.search(r"^CMAKE_CUDA_ARCHITECTURES(?::[^=]+)?=86$", cache_text, re.M):
        raise SystemExit("build cache is not configured exclusively for SM 8.6")
    if "NEXUS_ENABLE_CUDA:BOOL=ON" not in cache_text:
        raise SystemExit("CUDA backend is not enabled in the attested build")
    ctest_text = args.ctest_log.read_text(errors="replace")
    if "100% tests passed out of 18" not in ctest_text or "Not Run" in ctest_text:
        raise SystemExit("CTest log does not attest all 18 tests passing")

    def record(path: pathlib.Path) -> dict:
        return {"path": path.relative_to(root).as_posix(), "bytes": path.stat().st_size,
                "sha256": sha(path)}

    bins = sorted(path for path in (build / "bin").iterdir() if path.is_file() and path.stat().st_mode & 0o111)
    if not any(path.name == "nexus-cuda-real-model" for path in bins):
        raise SystemExit("compiled CUDA worker is missing")
    artifact_paths = list(bins)
    for directory in (build,):
        artifact_paths.extend(sorted(directory.glob("libnexus_*.a")))
    for directory in (build / "CMakeFiles",):
        artifact_paths.extend(sorted(directory.rglob("*.o")))
    artifacts = [record(path) for path in sorted(set(artifact_paths)) if path.is_file()]

    source_files = []
    source_root = root / "source"
    candidates = [source_root / "CMakeLists.txt"]
    for dirname in ("consensus", "include", "src", "tests", "proto"):
        candidates.extend((source_root / dirname).rglob("*"))
    for path in sorted(set(candidates)):
        if path.is_file() and (path.name == "CMakeLists.txt" or
                               path.suffix in {".cpp", ".cu", ".cuh", ".hpp", ".h", ".proto"}):
            source_files.append(record(path))

    linked: dict[str, dict] = {}
    for binary in bins:
        result = subprocess.run(["ldd", str(binary)], check=True, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for line in result.stdout.splitlines():
            match = re.search(r"=>\s+(/[^ ]+)|^\s*(/[^ ]+)\s+\(", line)
            if not match:
                if "not found" in line:
                    raise SystemExit(f"unresolved runtime dependency: {line.strip()}")
                continue
            lib = pathlib.Path(match.group(1) or match.group(2)).resolve(strict=True)
            linked[str(lib)] = {"bytes": lib.stat().st_size, "sha256": sha(lib)}

    git_head = subprocess.run(["git", "-C", str(root / "source"), "rev-parse", "HEAD"],
                              check=True, text=True, stdout=subprocess.PIPE).stdout.strip()
    status = subprocess.run(["git", "-C", str(root / "source"), "status", "--porcelain=v1"],
                            check=True, text=True, stdout=subprocess.PIPE).stdout
    manifest_rel = output.relative_to(root / "source").as_posix()
    # The manifest is generated inside the repository. Ignore only this output
    # file when deciding whether the attested source checkout was dirty.
    status_without_manifest = [line for line in status.splitlines()
                               if line[3:].strip('"') != manifest_rel]
    build_logs = []
    for path in args.build_log:
        resolved = path.resolve(strict=True)
        if root not in resolved.parents:
            raise SystemExit("build log escaped persistent workspace")
        build_logs.append(record(resolved))
    test_log = args.ctest_log.resolve(strict=True)
    if root not in test_log.parents:
        raise SystemExit("CTest log escaped persistent workspace")
    manifest = {
        "schema": "keryx-nexus-build-attestation-v1",
        "git_head_at_attestation": git_head,
        "git_worktree_dirty_at_attestation": bool(status_without_manifest),
        "cmake_source_directory": "source",
        "build_directory": build.relative_to(root).as_posix(),
        "build_target_coverage": ["all", "nexus-grpc-submit-mock-test"],
        "cuda_arch": "sm_86",
        "cuda_visible_device_policy": "primary NVIDIA GPU UUID only; build itself made no CUDA allocations",
        "toolchain": {
            "cmake": version(["cmake", "--version"]),
            "cxx": version(["c++", "--version"]),
            "nvcc": version(["/opt/cuda/bin/nvcc", "--version"]).split("release ")[-1],
        },
        "binary_and_objects": artifacts,
        "compiled_source_hashes": source_files,
        "runtime_libraries": linked,
        "build_logs": build_logs,
        "ctest_log": record(test_log),
        "ctest": {"tests": 18, "passed": 18, "failed": 0, "not_run": 0},
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(f"M27_BUILD_ATTESTATION=PASS source_head={git_head} binaries={len(bins)} "
          f"objects={len(artifacts)} source_files={len(source_files)} manifest={output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
