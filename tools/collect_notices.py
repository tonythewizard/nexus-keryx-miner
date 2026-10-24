#!/usr/bin/env python3
"""Collect dependency notices without copying package caches or credentials."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import tomllib

ROOT = Path(__file__).resolve().parents[1]
NOTICE = re.compile(r'^(?:LICENSE|LICENCE|COPYING|NOTICE|UNLICENSE)(?:[._-].*)?$', re.I)


def copy_notice(source, boundary, destination):
    if source.is_symlink() or not source.is_file():
        return False
    if not source.resolve().is_relative_to(boundary.resolve()):
        raise ValueError('Notice escaped its source package')
    if source.stat().st_size > 4 * 1024 * 1024:
        raise ValueError('Oversized dependency notice')
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cargo-home', type=Path, action='append', required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    tracked = subprocess.check_output(['git', '-C', str(ROOT), 'ls-files', '-z']).decode().split('\0')
    for name in tracked:
        relative = Path(name)
        if name and relative.parts[0] in ('vendor', 'consensus') and (
                NOTICE.fullmatch(relative.name) or relative.name == 'RESIDENT_KERNEL_LICENSE-MIT'):
            copy_notice(ROOT / relative, ROOT, args.output / 'source' / relative)
    upstream = ROOT / '_deps/keryx-node'
    if not upstream.is_dir():
        raise ValueError('Pinned oracle source missing')
    for notice in upstream.iterdir():
        if NOTICE.fullmatch(notice.name):
            copy_notice(notice, upstream, args.output / 'keryx-node' / notice.name)
    packages = {}
    for manifest in ('ai-response-bridge', 'oracle-rust'):
        lock = tomllib.loads((ROOT / manifest / 'Cargo.lock').read_text())
        for package in lock['package']:
            if package.get('source', '').startswith('registry+'):
                packages[(package['name'], package['version'])] = package
    inventory = []
    for name, version in sorted(packages):
        if not re.fullmatch(r'[A-Za-z0-9_.+-]+', name + '-' + version):
            raise ValueError('Invalid package identity')
        found = False
        for home in args.cargo_home:
            for directory in (home / 'registry/src').glob('*/' + name + '-' + version):
                manifest = tomllib.loads((directory / 'Cargo.toml').read_text())['package']
                notices = [path for path in directory.iterdir() if NOTICE.fullmatch(path.name)]
                if manifest.get('license-file'):
                    notices.append(directory / manifest['license-file'])
                for notice in notices:
                    copy_notice(notice, directory,
                                args.output / 'rust' / (name + '-' + version) / notice.name)
                inventory.append({'name': name, 'version': version,
                                  'license': manifest.get('license'),
                                  'source': packages[(name, version)]['source']})
                found = True
                break
            if found:
                break
    (args.output / 'rust-dependencies.json').write_text(json.dumps(inventory, indent=2) + '\n')
    print(f'Collected notices for {len(inventory)} available locked Rust packages and vendored sources')


if __name__ == '__main__':
    main()
