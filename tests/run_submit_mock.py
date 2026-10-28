#!/usr/bin/env python3
"""Run the authentic-GGUF SubmitBlock mock with the configured public payout data."""
import os
import pathlib
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 10:
        raise SystemExit("usage: run_submit_mock.py EXE MODEL TREE META CAPTURE ORACLE OUT LAUNCHER ESCROW_KEY")
    source = pathlib.Path(__file__).resolve().parents[1]
    root = source.parent
    sys.path.insert(0, str(source / "scripts"))
    import nexus_force_shadow

    executable, model, tree, metadata, captures, oracle, output, launcher, escrow = sys.argv[1:]
    if not launcher or not escrow:
        print("SKIP: authentic SubmitBlock mock requires explicit launcher and test-key fixtures")
        return 77
    payout, extra = nexus_force_shadow.resolve_stock_mining_fields(launcher, escrow)
    persistent = source / ".local/tmp/submit-mock"
    persistent.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update({"TMPDIR": str(persistent), "TMP": str(persistent), "TEMP": str(persistent),
                "PYTHONDONTWRITEBYTECODE": "1", "PYTHONPYCACHEPREFIX": str(source / ".local/tmp/submit-mock-python")})
    command = [executable, model, tree, metadata, captures, oracle, output, payout, extra]
    result = subprocess.run(command, env=env, check=False)
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
