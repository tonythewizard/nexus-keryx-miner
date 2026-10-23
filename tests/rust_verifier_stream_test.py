#!/usr/bin/env python3
"""CPU-only official-proof transport regression; fixtures remain outside Git."""
import argparse
import pathlib
import struct
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("oracle")
    parser.add_argument("context", type=pathlib.Path)
    parser.add_argument("trace", type=pathlib.Path)
    parser.add_argument("proof", type=pathlib.Path)
    args = parser.parse_args()
    context, trace, proof = (p.read_bytes() for p in (args.context, args.trace, args.proof))
    assert len(context) == 225 + len(proof)
    assert context[225:] == proof

    def frame(sequence, body=proof):
        return (b"NXVR" + struct.pack("<IQIII", 1, sequence, 225, len(trace), len(body))
                + context[:225] + trace + body)

    expected = subprocess.check_output([
        args.oracle, "verify-real", str(args.context), str(args.trace), str(args.proof)
    ]).strip()
    result = subprocess.run([args.oracle, "verify-real-stream"], input=frame(1) + frame(2),
                            capture_output=True, check=True, timeout=10)
    offset = 0
    for sequence in (1, 2):
        header = result.stdout[offset:offset + 64]
        assert len(header) == 64 and header[:4] == b"NXVA"
        assert struct.unpack_from("<I", header, 4)[0] == 1
        assert struct.unpack_from("<Q", header, 8)[0] == sequence
        assert struct.unpack_from("<I", header, 16)[0] == 0
        length = struct.unpack_from("<I", header, 60)[0]
        assert 0 < length <= 2048
        offset += 64
        assert result.stdout[offset:offset + length] == expected
        offset += length
    assert offset == len(result.stdout)
    mutated = bytearray(proof)
    mutated[len(mutated) // 2] ^= 1
    failures = {
        "replay": frame(1) + frame(1),
        "truncation": frame(1)[:-1],
        "version": frame(1)[:4] + struct.pack("<I", 2) + frame(1)[8:],
        "proof_mutation": frame(1, mutated),
        "oversize": b"NXVR" + struct.pack("<IQIII", 1, 1, 225, len(trace), 2 * 1024 * 1024 + 1),
    }
    for name, invalid in failures.items():
        failed = subprocess.run([args.oracle, "verify-real-stream"], input=invalid,
                                capture_output=True, timeout=10)
        assert failed.returncode != 0, name
    print("RUST_VERIFIER_STREAM=PASS valid_frames=2 oneshot_summary_identical=true malformed_frames_rejected=5")


if __name__ == "__main__":
    main()
