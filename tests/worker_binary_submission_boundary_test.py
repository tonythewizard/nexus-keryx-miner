#!/usr/bin/env python3
"""Exercise the compiled worker's mode gate before model/GPU initialization."""
from __future__ import annotations

import os
import subprocess
import sys

GPU_UUID = "GPU-00000000-0000-0000-0000-000000000000"


def run(binary: str, mode: str, flag: str, ai_flag: str, claim_flag=None) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env.update({
        "CUDA_VISIBLE_DEVICES": GPU_UUID,
        "NEXUS_CUDA_DEVICE_UUID": GPU_UUID,
        "NEXUS_SUBMIT_ENABLED": "1",
        "NEXUS_SUBMIT_ENDPOINT": "grpcs://submit-override.invalid:443",
    })
    return subprocess.run(
        [binary, "grpc://127.0.0.1:22110", "payout", "extra", "unused.gguf",
         "unused.tree", "unused.json", "unused-oracle", "unused-output", "257",
         "1", mode, flag, ai_flag] + ([claim_flag] if claim_flag else []),
        env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        check=False,
    )


def main() -> int:
    binary = sys.argv[1]
    for mode in ("shadow", "live"):
        result = run(binary, mode, "--no-submit", "--no-ai-response")
        expected = "nonce limit must be" if mode == "shadow" else "explicit --enable-ai-response authorization"
        if result.returncode == 0 or expected not in result.stdout:
            raise RuntimeError(f"{mode} no-submit did not pass the mode gate before resources: {result.stdout}")
        if "submit=ENABLED" in result.stdout:
            raise RuntimeError(f"ambient submit environment enabled {mode}: {result.stdout}")

    rejected = run(binary, "shadow", "--enable-submit", "--no-ai-response")
    if rejected.returncode == 0 or "shadow worker cannot enable SubmitBlock" not in rejected.stdout:
        raise RuntimeError(f"compiled worker accepted shadow submit opt-in: {rejected.stdout}")
    if "submit=ENABLED" in rejected.stdout:
        raise RuntimeError(f"shadow rejection reported submission enabled: {rejected.stdout}")

    rejected_ai = run(binary, "live", "--no-submit", "--no-ai-response")
    if rejected_ai.returncode == 0 or "explicit --enable-ai-response authorization" not in rejected_ai.stdout:
        raise RuntimeError(f"live worker mined without explicit AI-response authorization: {rejected_ai.stdout}")

    explicit = run(binary, "live", "--enable-submit", "--enable-ai-response")
    if explicit.returncode == 0 or "nonce limit must be" not in explicit.stdout:
        raise RuntimeError(f"explicit live flag did not reach post-policy validation: {explicit.stdout}")
    for claim_flag, submit_flag, expected in (("--enable-escrow-claims", "--no-submit", "escrow claims require live mode"),
                                             ("--invalid-claim-policy", "--enable-submit", "invalid explicit escrow claim policy"),
                                             ("--enable-escrow-claims", "--enable-submit", "nonce limit must be")):
        result = run(binary, "live", submit_flag, "--enable-ai-response", claim_flag)
        if result.returncode == 0 or expected not in result.stdout:
            raise RuntimeError(f"claim opt-in gate failed before resources: {result.stdout}")
    print("WORKER_BINARY_SUBMISSION_BOUNDARY=PASS shadow_env_optin_ignored=true "
          "shadow_enable_rejected=true live_ai_authorization_required=true "
          "pom_and_ai_flags_independent=true explicit_live_flags_parsed=true "
          "cuda_initialized=false submit_rpc_calls=0")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"WORKER_BINARY_SUBMISSION_BOUNDARY=FAIL detail={error}", file=sys.stderr)
        raise SystemExit(1)
