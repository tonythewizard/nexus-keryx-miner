#!/usr/bin/env python3
"""Private GitHub release updates, with isolated staging and cooperative activation."""
import argparse
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import signal
import tarfile
import time
import threading

ROOT = Path(os.environ.get("NEXUS_DATA_DIR", str(Path.home() / ".local/share/nexus-keryx-miner")))
SOURCE = Path(__file__).resolve().parent.parent
STATE = ROOT / 'state/updates'
REPOSITORY = 'tonythewizard/nexus-keryx-miner'
SERVICE = 'nexus-v002-production.service'
LLAMA_SHA = 'c07e7e78d133aeca33a30fedb8d6420e5374c76015cc06ca29022d454ae8ee7d'


def run(args, timeout=30, **kwargs):
    return subprocess.run(args, check=True, timeout=timeout, text=True,
                          capture_output=True, **kwargs).stdout.strip()


def atomic(path, value):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    target = path.with_suffix('.new')
    fd = os.open(target, os.O_CREAT | os.O_TRUNC | os.O_WRONLY | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w') as f:
        json.dump(value, f); f.flush(); os.fsync(f.fileno())
    os.replace(target, path)


def configuration():
    try:
        value = json.loads((STATE / 'config.json').read_text())
        if type(value.get('enabled')) is not bool:
            raise ValueError('invalid update configuration')
        return value
    except FileNotFoundError:
        return {'enabled': False}


def status():
    try:
        value = json.loads((STATE / 'status.json').read_text())
    except (OSError, ValueError):
        value = {'status': 'NOT_CHECKED'}
    value['enabled'] = configuration()['enabled']
    return value


def sha(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def github(path):
    return json.loads(run(['gh', 'api', path]))


def repository_info():
    user = github('user')
    repo = github('repos/' + REPOSITORY)
    if (user['login'] != REPOSITORY.split('/')[0] or repo['owner']['type'] != 'User'
            or repo['owner']['login'] != user['login'] or repo.get('private') is not True
            or repo.get('visibility') != 'private'):
        raise ValueError('authenticated personal PRIVATE repository required')
    return repo


def validate_manifest(value, commit):
    if value.get('schema_version') != 1 or value.get('git_commit') != commit:
        raise ValueError('release manifest does not match the exact remote commit')
    if not re.fullmatch(r'\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?', value.get('version', '')):
        raise ValueError('invalid release version')
    if value.get('platform') != 'linux-x86_64' or value.get('llama_abi') != 3:
        raise ValueError('unsupported platform or llama ABI')
    if value.get('stock_llama_sha256') != LLAMA_SHA:
        raise ValueError('stock llama replacement is not authorized')
    validation = value.get('validation', {})
    if any(validation.get(k) != 'PASS' for k in ('cuda_integration', 'rust_parity', 'escrow_claim_mock')):
        raise ValueError('release validation evidence incomplete')
    for role in ('worker', 'oracle') + (('desktop',) if 'desktop' in value else ()):
        item = value.get(role, {})
        if not re.fullmatch(r'[A-Za-z0-9_.-]+', item.get('asset', '')):
            raise ValueError('invalid asset name')
        if not re.fullmatch(r'[0-9a-f]{64}', item.get('sha256', '')):
            raise ValueError('invalid asset digest')
    return value


def extract_source(archive, destination):
    total = 0
    with tarfile.open(archive) as tar:
        members = tar.getmembers()
        if len(members) > 20000:
            raise ValueError('source archive file limit')
        prefix = None
        for entry in members:
            parts = PurePosixPath(entry.name).parts
            if not parts or entry.name.startswith('/') or '..' in parts:
                raise ValueError('unsafe archive path')
            prefix = prefix or parts[0]
            if parts[0] != prefix or entry.issym() or entry.islnk():
                raise ValueError('unsafe archive link or root')
            if entry.isdir():
                continue
            if not entry.isfile() or entry.size > 32 * 1024 * 1024:
                raise ValueError('unsupported source entry')
            total += entry.size
            if total > 256 * 1024 * 1024:
                raise ValueError('source archive size limit')
            target = destination.joinpath(*parts[1:])
            if target.suffix.lower() == '.gguf' or target.name in ('escrow.key', 'escrow.cert', 'wallet.dat'):
                raise ValueError('operational secrets/models are not update source')
            target.parent.mkdir(parents=True, exist_ok=True)
            with tar.extractfile(entry) as source, target.open('wb') as out:
                out.write(source.read())
            target.chmod(0o700 if entry.mode & 0o111 else 0o600)


def download_asset(asset, path, maximum):
    if asset.get('size', maximum + 1) > maximum or asset.get('state') != 'uploaded':
        raise ValueError('release asset size/state invalid')
    with path.open('wb') as output:
        subprocess.run(['gh', 'api', asset['url'].replace('https://api.github.com/', ''),
                        '-H', 'Accept: application/octet-stream'], stdout=output,
                       stderr=subprocess.PIPE, timeout=90, check=True)
    if path.stat().st_size != asset['size']:
        raise ValueError('incomplete release asset')
    digest = asset.get('digest', '')
    if digest and digest != 'sha256:' + sha(path):
        raise ValueError('GitHub release asset digest mismatch')


def stage(commit, release):
    directory = ROOT / 'updates/releases' / commit
    if directory.exists():
        raise ValueError('partial staging exists; inspect before retry')
    directory.mkdir(parents=True, mode=0o700)
    assets = {a['name']: a for a in release['assets']}
    if 'NEXUS_UPDATE.json' not in assets:
        raise ValueError('waiting for NEXUS_UPDATE.json and validated release binaries')
    download_asset(assets['NEXUS_UPDATE.json'], directory / 'manifest.json', 65536)
    manifest = validate_manifest(json.loads((directory / 'manifest.json').read_text()), commit)
    if 'desktop' not in manifest:
        raise ValueError('release lacks a verified Tauri desktop asset; activation deferred')
    for role in ('worker', 'oracle', 'desktop'):
        item = manifest[role]
        if item['asset'] not in assets:
            raise ValueError('missing release ' + role)
        target = directory / role
        download_asset(assets[item['asset']], target, 64 * 1024 * 1024)
        if sha(target) != item['sha256']:
            raise ValueError(role + ' checksum mismatch')
        target.chmod(0o700)
    archive = directory / 'source.tar.gz'
    with archive.open('wb') as output:
        subprocess.run(['gh', 'api', 'repos/' + REPOSITORY + '/tarball/' + commit],
                       stdout=output, stderr=subprocess.PIPE, check=True, timeout=90)
    if archive.stat().st_size > 128 * 1024 * 1024:
        raise ValueError('source download size limit')
    tree = directory / 'source'
    if tree.exists():
        raise ValueError('staged source exists; inspect before retry')
    extract_source(archive, tree)
    for name in ('nexusmine-next.fish', 'nexus-open.fish', 'nexus_dashboard.py', 'nexus-tauri-open.fish', 'nexus_tauri_bridge.py', 'nexus_updates.py'):
        if not (tree / 'scripts' / name).is_file():
            raise ValueError('release lacks updater/desktop contract')
    if 'NEXUS_RELEASE_HOME' not in (tree / 'scripts/nexusmine-next.fish').read_text():
        raise ValueError('release launcher does not support isolated artifacts')
    if (tree / 'VERSION').read_text().strip() != manifest['version']:
        raise ValueError('release source version mismatch')
    run(['python3', '-B', str(SOURCE / 'scripts/nexus_cuda_runtime_gate.py'),
         str(directory / 'worker'), '/opt/nexus-data/legacy/src/keryx-miner-official/keryxmine-v0.5.7-PoM/libkeryx-llama.so'])
    atomic(directory / 'verified.json', {'git_commit': commit, 'manifest_sha256': sha(directory / 'manifest.json')})
    return directory


def verified_release(directory):
    if directory.parent.resolve() != (ROOT / 'updates/releases').resolve():
        raise ValueError('release outside isolated staging')
    if not re.fullmatch('[0-9a-f]{40}', directory.name):
        raise ValueError('invalid staged commit')
    evidence = json.loads((directory / 'verified.json').read_text())
    if evidence.get('manifest_sha256') != sha(directory / 'manifest.json'):
        raise ValueError('staged manifest changed')
    manifest = validate_manifest(json.loads((directory / 'manifest.json').read_text()), directory.name)
    for role in ('worker', 'oracle') + (('desktop',) if 'desktop' in manifest else ()):
        if sha(directory / role) != manifest[role]['sha256']:
            raise ValueError('staged ' + role + ' changed')
    return manifest


def recover():
    transaction = STATE / 'activation.json'
    if not transaction.exists(): return
    saved = json.loads(transaction.read_text())
    from nexus_agent_api import worker_identity
    run(['systemctl', '--user', 'stop', SERVICE], timeout=65)
    if worker_identity():
        raise ValueError('rollback refused: worker still alive; no duplicate start')
    for unit, text in saved['units'].items():
        if unit not in (SERVICE, 'nexus-matrix-dashboard.service', 'nexus-tray.service'):
            raise ValueError('invalid rollback unit')
        path = Path.home() / '.config/systemd/user' / (unit + '.d') / 'release.conf'
        if text is None: path.unlink(missing_ok=True)
        else: path.write_text(text)
    for name, target in saved['links'].items():
        if name not in ('nexus', 'NEXUS'): raise ValueError('invalid rollback shortcut')
        path = Path.home() / '.local/bin' / name
        path.unlink(missing_ok=True)
        if target is not None: path.symlink_to(target)
    if saved['installed'] is None: (STATE / 'installed.json').unlink(missing_ok=True)
    else: atomic(STATE / 'installed.json', saved['installed'])
    run(['systemctl', '--user', 'daemon-reload'])
    if not (STATE / 'user-stop.json').exists():
        run(['systemctl', '--user', 'start', SERVICE])
    transaction.unlink()


def apply(directory):
    # Never bypass AI safety, force-stop a worker, or restart keryxd.
    manifest = verified_release(directory)
    if 'desktop' not in manifest:
        raise ValueError('verified Tauri desktop required before activation')
    if run(['systemctl', '--user', 'is-active', SERVICE]) != 'active':
        raise ValueError('update staged; mining stopped, no implicit mining start')
    from nexus_agent_api import AgentAPI, worker_identity, recent_snapshot
    import nexus_agent_api
    nexus_agent_api.AUDIT = ROOT / 'resident-search/journal-recovery-audit'
    nexus_agent_api.AUDIT_SHA = '0973148085f772a867c920a3f6ac7548cb1c8108734609e74bf874b3ec9c6106'
    operation = os.open(ROOT / 'agent-api/api-operation.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(operation, fcntl.LOCK_EX | fcntl.LOCK_NB)
        AgentAPI.pre_stop(AgentAPI.__new__(AgentAPI))
        if not configuration()['enabled'] or (STATE / 'user-stop.json').exists(): raise ValueError('updates disabled or manual shutdown requested')
        # An uncertain claim is an economic obligation, not permission to retry.
        claims = {}
        journal = ROOT / 'state/ai-service/claim-submissions.jsonl'
        if journal.exists():
            for line in journal.read_text().splitlines():
                row = json.loads(line)
                if row.get('event') == 'ESCROW_CLAIM_SUBMISSION': claims[row['txid']] = row['state']
        if any(v not in ('RPC_ACKNOWLEDGED', 'RPC_REJECTED', 'CHAIN_CONFIRMED') for v in claims.values()):
            raise ValueError('claim submission unresolved; update deferred')
        tree = directory / 'source'
        units = {
            SERVICE: '[Service]\nWorkingDirectory=' + str(tree) + '\nEnvironment=NEXUS_RELEASE_HOME=' + str(directory) + '\nExecStart=\nExecStart=/usr/bin/fish ' + str(tree / 'scripts/nexusmine-next.fish') + ' live --enable-ai-response --enable-submit --enable-escrow-claims --no-tui\n',
            'nexus-matrix-dashboard.service': '[Service]\nEnvironment=NEXUS_DATA_DIR=' + str(ROOT) + '\nEnvironment=NEXUS_DESKTOP_BIN=' + str(directory / 'desktop') + '\nExecStart=\nExecStart=/usr/bin/fish ' + str(tree / 'scripts/nexus-tauri-open.fish') + '\n',
        }
        paths = {u: Path.home() / '.config/systemd/user' / (u + '.d') / 'release.conf' for u in units}
        previous = {u: f.read_text() if f.exists() else None for u, f in paths.items()}
        links = {}
        for name in ('nexus', 'NEXUS'):
            path = Path.home() / '.local/bin' / name
            if path.exists() and not path.is_symlink(): raise ValueError('shortcut is not a managed symlink')
            links[name] = os.readlink(path) if path.is_symlink() else None
        installed = json.loads((STATE / 'installed.json').read_text()) if (STATE / 'installed.json').exists() else None
        atomic(STATE / 'activation.json', {'units': previous, 'links': links, 'installed': installed})
        try:
            run(['systemctl', '--user', 'stop', SERVICE], timeout=65)
            if worker_identity(): raise ValueError('cooperative shutdown incomplete')
            if (STATE / 'user-stop.json').exists(): raise ValueError('manual shutdown requested')
            for u, text in units.items():
                paths[u].parent.mkdir(parents=True, exist_ok=True)
                temporary = paths[u].with_suffix('.new'); temporary.write_text(text); os.replace(temporary, paths[u])
            run(['systemctl', '--user', 'daemon-reload'])
            if (STATE / 'user-stop.json').exists(): raise ValueError('manual shutdown requested')
            run(['systemctl', '--user', 'start', SERVICE])
            start = time.monotonic()
            while time.monotonic() - start < 180:
                snapshot, peers = recent_snapshot(), worker_identity()
                if (snapshot.get('status') == 'LIVE' and snapshot.get('state', {}).get('mining') == 'ACTIVE'
                        and snapshot.get('wallet_authorization') == 'AUTHORIZED'
                        and snapshot.get('counters', {}).get('pom_kernels_completed', 0) > 0
                        and len(peers) == 1 and sha(Path('/proc') / str(peers[0]['pid']) / 'exe') == manifest['worker']['sha256']): break
                if run(['systemctl', '--user', 'is-active', SERVICE]) != 'active': raise ValueError('updated worker startup failed')
                time.sleep(2)
            else: raise ValueError('updated worker readiness deadline exceeded')
            for name in links:
                path = Path.home() / '.local/bin' / name
                temporary = path.with_suffix('.new'); temporary.unlink(missing_ok=True)
                temporary.symlink_to(tree / 'scripts/nexus-open.fish'); os.replace(temporary, path)
            atomic(STATE / 'installed.json', {'git_commit': directory.name})
            (STATE / 'activation.json').unlink()
        except BaseException:
            recover()
            raise
        # Desktop failures must not roll back a healthy worker or lose shortcuts.
        try: run(['systemctl', '--user', 'restart', 'nexus-matrix-dashboard.service'])
        except subprocess.SubprocessError: pass
    finally:
        os.close(operation)


def check(activate=True, inspect_only=False):
    STATE.mkdir(parents=True, exist_ok=True, mode=0o700)
    with (STATE / 'check.lock').open('w') as lock:
        try: fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError: return
        value = status()
        if (STATE / 'user-stop.json').exists() and not inspect_only:
            value.update(status='STOPPED', detail='Manual shutdown requested; no automatic restart'); atomic(STATE / 'status.json', value); return
        if not value['enabled'] and not inspect_only:
            value.update(status='DISABLED'); atomic(STATE / 'status.json', value); return
        value.update(status='CHECKING', checked_unix=time.time(), repository=REPOSITORY)
        atomic(STATE / 'status.json', value)
        try:
            repo = repository_info()
            remote = github('repos/' + REPOSITORY + '/commits/' + repo['default_branch'])
            commit = remote['sha']
            if not re.fullmatch('[0-9a-f]{40}', commit): raise ValueError('invalid remote commit')
            local = run(['git', '-C', str(SOURCE), 'rev-parse', 'HEAD']) if (SOURCE / '.git').exists() else None
            value.update(remote_commit=commit, local_commit=local, checked_unix=time.time(), next_check_unix=time.time()+7200)
            installed = json.loads((STATE / 'installed.json').read_text()) if (STATE / 'installed.json').exists() else {}
            if commit == installed.get('git_commit') or commit == local:
                value.update(status='UP_TO_DATE', detail='Latest published commit installed')
            else:
                remote_date = datetime.datetime.fromisoformat(remote['commit']['committer']['date'].replace('Z', '+00:00')).timestamp()
                local_date = int(run(['git', '-C', str(SOURCE), 'show', '-s', '--format=%ct', 'HEAD'])) if local else 0
                if not installed and remote_date <= local_date:
                    value.update(status='LOCAL_AHEAD', detail='Local build is newer than GitHub; no downgrade')
                else:
                    value.update(status='UPDATE_AVAILABLE', detail='New private GitHub commit')
                    if value.get('notified_commit') != commit:
                        from nexus_notifications import desktop_notification
                        if desktop_notification('UPDATE_AVAILABLE') is not False:
                            value['notified_commit'] = commit
                    atomic(STATE / 'status.json', value)
                    if inspect_only:
                        return
                    releases = github('repos/' + REPOSITORY + '/releases?per_page=5')
                    release = next((x for x in releases if not x['draft'] and github('repos/'+REPOSITORY+'/commits/'+x['tag_name'])['sha'] == commit), None)
                    if not release: raise ValueError('waiting for a release attached to the exact commit')
                    directory = ROOT / 'updates/releases' / commit
                    if not (directory / 'verified.json').exists(): directory = stage(commit, release)
                    if activate:
                        apply(directory)
                        value.update(status='UP_TO_DATE', detail='Validated release installed', local_commit=commit)
                    else:
                        value.update(status='STAGED', directory=str(directory), detail='Validated release staged; safety-gated activation pending')
        except Exception as error:
            value.update(status='WAITING' if value.get('status') == 'UPDATE_AVAILABLE' else 'ERROR', detail=str(error)[:300])
        atomic(STATE / 'status.json', value)
        if not inspect_only: print(json.dumps(value), flush=True)


def toggle():
    STATE.mkdir(parents=True, exist_ok=True, mode=0o700)
    with (STATE / 'config.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        value=configuration(); value['enabled']=not value['enabled']; atomic(STATE/'config.json',value)
        if value['enabled']: atomic(STATE/'request.json', {'requested_ns': time.time_ns()})
        return value


class EmbeddedUpdater:
    """One bounded checker thread inside the miner's owning runtime."""
    def __init__(self):
        self.stop_event=threading.Event()
        self.thread=threading.Thread(target=self.loop, daemon=True, name='nexus-updates')
    def start(self): self.thread.start()
    def close(self): self.stop_event.set()
    def loop(self):
        deadline=0.; seen=None
        while not self.stop_event.is_set():
            try:
                request=(STATE/'request.json').stat().st_mtime_ns
            except OSError: request=None
            if time.monotonic()>=deadline or request!=seen:
                seen=request; deadline=time.monotonic()+7200
                try:
                    check(activate=False)
                    value=status()
                    if value.get('status')=='STAGED' and configuration()['enabled']:
                        # A self-update needs a controller outside the service being
                        # replaced; only this bounded transaction is detached.
                        code="import sys;sys.path.insert(0,"+repr(str(SOURCE/'scripts'))+");from nexus_updates import main;main()"
                        recovery=code
                        subprocess.run(['systemd-run','--user','--collect','--unit=nexus-update-activation',
                            '--property=SendSIGKILL=no','--property=Restart=no',
                            '--property=ExecStopPost=/usr/bin/python3 -B -c "'+recovery+'" recover',
                            '--setenv=TMPDIR='+str(ROOT/'work/tmp'), '--setenv=TMP='+str(ROOT/'work/tmp'), '--setenv=TEMP='+str(ROOT/'work/tmp'),
                            '/usr/bin/python3','-B','-c',code,'activate',value['directory']],
                            capture_output=True,timeout=10,check=True)
                except Exception as error:
                    print('NEXUS_UPDATE_CHECK_ERROR '+type(error).__name__,flush=True)
            self.stop_event.wait(2)

def main():
    ROOT.joinpath('work/tmp').mkdir(parents=True, exist_ok=True)
    os.environ.update(TMPDIR=str(ROOT/'work/tmp'), TMP=str(ROOT/'work/tmp'), TEMP=str(ROOT/'work/tmp'))
    parser = argparse.ArgumentParser(); parser.add_argument('command', choices=('check','status','toggle','runtime-paths','recover','resume','activate')); parser.add_argument('directory', nargs='?'); args=parser.parse_args()
    def interrupted(signum, frame):
        raise RuntimeError('update interrupted; cooperative rollback required')
    signal.signal(signal.SIGTERM, interrupted); signal.signal(signal.SIGINT, interrupted)
    if args.command == 'activate': apply(Path(args.directory))
    elif args.command == 'resume': (STATE / 'user-stop.json').unlink(missing_ok=True)
    elif args.command == 'recover': recover()
    elif args.command == 'check': check()
    elif args.command == 'status': print(json.dumps(status()))
    elif args.command == 'toggle':
        STATE.mkdir(parents=True, exist_ok=True, mode=0o700)
        with (STATE / 'config.lock').open('w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX); value=configuration(); value['enabled']=not value['enabled']; atomic(STATE/'config.json',value); print(json.dumps(value))
    else:
        directory=Path(args.directory).resolve(); manifest=json.loads((directory/'manifest.json').read_text()); validate_manifest(manifest,directory.name)
        for role in ('worker','oracle'):
            path=directory/role
            if sha(path)!=manifest[role]['sha256']:raise ValueError('installed '+role+' digest mismatch')
            print(path); print(manifest[role]['sha256'])


if __name__ == '__main__':
    main()
