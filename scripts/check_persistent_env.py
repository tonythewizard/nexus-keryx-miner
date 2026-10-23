#!/usr/bin/env python3
"""Fail closed unless compiler, Rust and CUDA scratch/cache paths persist here."""
import os
import pathlib
import sys

REQUIRED = (
    "TMPDIR", "TMP", "TEMP", "CUDA_CACHE_PATH", "CARGO_TARGET_DIR",
    "CARGO_HOME", "XDG_CACHE_HOME", "PYTHONPYCACHEPREFIX", "CCACHE_DIR",
    "PIP_CACHE_DIR",
)


def validate(root: pathlib.Path, env) -> list[str]:
    root = root.resolve(strict=True)
    failures = []
    for name in REQUIRED:
        raw = env.get(name)
        if not raw:
            failures.append(f"{name}=unset")
            continue
        path = pathlib.Path(raw)
        if not path.is_absolute():
            failures.append(f"{name} is not absolute: {raw}")
            continue
        resolved = path.resolve(strict=False)
        if resolved != root and root not in resolved.parents:
            failures.append(f"{name} escapes persistent workspace: {resolved}")
    return failures


def main() -> int:
    import os
    if len(sys.argv) != 2:
        print("usage: check_persistent_env.py WORKSPACE", file=sys.stderr)
        return 2
    failures = validate(pathlib.Path(sys.argv[1]), os.environ)
    if failures:
        print("PERSISTENT_TOOLCHAIN_PATHS=BLOCKED " + "; ".join(failures), file=sys.stderr)
        return 2
    print("PERSISTENT_TOOLCHAIN_PATHS=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
