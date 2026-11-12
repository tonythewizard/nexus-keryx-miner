#!/usr/bin/env python3
"""Hash-pinned gRPC 1.81.1 / MSVC 19.50-19.51 compile workaround.

Force deduction of the six interceptor method pointers before their decltype
queries. The return expression and all runtime behavior remain unchanged.
Upstream report/workaround: https://github.com/grpc/grpc/issues/41970
Also reproduced with VS2026: https://github.com/grpc/grpc/issues/42763
"""
import argparse
import hashlib
from pathlib import Path

ORIGINAL_SHA256 = 'f311ba7bc5853c3eb7e5e8d752e4dea83e6706582178df83f385d3bc7a654c97'
PATCHED_SHA256 = '38b4e1f208b11799e9b31cb874811e62c6d82b775d452b12a5fae54f5535262d'
MARKER = b'inline constexpr bool CallHasChannelAccess() {\n'
METHODS = ('OnClientInitialMetadata', 'OnClientToServerMessage', 'OnServerInitialMetadata',
           'OnServerToClientMessage', 'OnServerTrailingMetadata', 'OnFinalize')
INSERT = b''.join(('  [[maybe_unused]] constexpr auto nexus_msvc_method_' + str(i) +
                   ' = &Derived::Call::' + name + ';\n').encode('ascii')
                  for i, name in enumerate(METHODS))


def apply(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError('Regular gRPC source header required')
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest == PATCHED_SHA256:
        return False
    if digest != ORIGINAL_SHA256 or data.count(MARKER) != 1:
        raise ValueError('Unexpected gRPC source; review the pinned MSVC workaround')
    result = data.replace(MARKER, MARKER + INSERT, 1)
    if hashlib.sha256(result).hexdigest() != PATCHED_SHA256:
        raise ValueError('gRPC workaround output checksum mismatch')
    path.write_bytes(result)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('header', type=Path)
    args = parser.parse_args()
    changed = apply(args.header)
    print('gRPC MSVC workaround: ' + ('applied' if changed else 'already verified'))


if __name__ == '__main__':
    main()
