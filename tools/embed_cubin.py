#!/usr/bin/env python3
"""Embed CUDA device code as portable C++ data, without an ELF-only objcopy."""
import argparse
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('symbol')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', args.symbol):
        parser.error('invalid C++ symbol')
    data = args.input.read_bytes()
    if not data or len(data) > 64 * 1024 * 1024:
        parser.error('invalid cubin size')
    lines = ['// Generated CUDA device image; do not edit.',
             f'extern "C" {{ alignas(16) extern const unsigned char {args.symbol}[] = {{']
    lines += [','.join(f'0x{b:02x}' for b in data[i:i + 32]) + ','
              for i in range(0, len(data), 32)]
    lines.append('}; }\n')
    args.output.write_text('\n'.join(lines), encoding='ascii')


if __name__ == '__main__':
    main()
