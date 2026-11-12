#!/usr/bin/env python3
"""Prepare an allowlisted Tauri bridge and checksum-pinned embedded Python.

Only repository source and the official Python archive are read. No installation
state, wallet, model, worker log or host profile is collected.
"""
import argparse
import hashlib
from pathlib import Path
import shutil
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PYTHON_VERSION = '3.14.10'
PYTHON_SHA256 = '90905e64a840557d8c1bc4f5b74db3d7ee85e3ffb217611cac1c7813acea4159'
SCRIPTS = ('nexus_tauri_bridge', 'nexus_windows_monitor', 'nexus_hardware_metrics',
           'nexus_dashboard', 'nexus_web_mirror', 'nexus_presenter', 'nexus_ui_preferences',
           'nexus_gpu_metrics', 'nexus_gpu_identity', 'nexus_gpu_registry',
           'nexus_gpu_selection', 'nexus_gpu_devices', 'nexus_file_lock', 'nexus_agent_api')


def prepare(archive, target):
    data = archive.read_bytes()
    if hashlib.sha256(data).hexdigest() != PYTHON_SHA256:
        raise ValueError('Embedded Python checksum mismatch')
    if target.exists():
        raise ValueError('Use a fresh desktop resource directory')
    target.mkdir(parents=True)
    try:
        with zipfile.ZipFile(archive) as bundle:
            for item in bundle.infolist():
                path = Path(item.filename)
                if path.is_absolute() or '..' in path.parts or '\\' in item.filename or (item.external_attr >> 16) & 0o170000 == 0o120000:
                    raise ValueError('Unexpected Python archive path')
            bundle.extractall(target / 'python')
        (target / 'python/python314._pth').write_text('python314.zip\n.\n../scripts\n', encoding='utf-8')
        scripts = target / 'scripts'
        scripts.mkdir()
        for name in SCRIPTS:
            shutil.copyfile(ROOT / 'scripts' / (name + '.py'), scripts / (name + '.py'))
        for name in ('VERSION', 'LICENSE', 'THIRD_PARTY.md'):
            shutil.copyfile(ROOT / name, target / name)
    except Exception:
        shutil.rmtree(target)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path)
    args = parser.parse_args()
    cache = ROOT / '.local/tmp/windows-desktop'
    cache.mkdir(parents=True, exist_ok=True)
    archive = args.archive or cache / ('python-' + PYTHON_VERSION + '-embed-amd64.zip')
    if not archive.exists():
        urllib.request.urlretrieve('https://www.python.org/ftp/python/' + PYTHON_VERSION + '/' + archive.name, archive)
    prepare(archive, ROOT / 'nexus-ui-desktop/src-tauri/.local/bridge')


if __name__ == '__main__':
    main()
