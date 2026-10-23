#!/usr/bin/env python3
"""Loopback agent API. Scoped bearer keys, bounded diagnostics and isolated development."""
import argparse
import fcntl
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote

ROOT = Path(os.environ.get("NEXUS_DATA_DIR", str(Path.home() / ".local/share/nexus-keryx-miner")))
SOURCE = Path(__file__).resolve().parent.parent
STATE = ROOT / 'agent-api'
SERVICE = 'nexus-v002-production.service'
AUDIT = ROOT / 'build/nexus-v002-candidate/bin/nexus-ai-backfill-audit'
AUDIT_SHA = 'ff271ab348730fe9be9214861033438593326b8421cacd5aee1f107a6b884307'
COMMANDS = ('diagnostics_capture', 'dashboard_refresh', 'worker_start', 'worker_stop', 'worker_restart')
SOURCE_SUFFIXES = {'.cpp', '.hpp', '.h', '.cu', '.rs', '.py', '.fish', '.md', '.toml', '.txt', '.html', '.css', '.js'}


def atomic_json(path, obj):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    temporary = path.with_name(path.name + '.new')
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w') as f:
        json.dump(obj, f, separators=(',', ':')); f.flush(); os.fsync(f.fileno())
    os.replace(temporary, path)


def run(argv, timeout=8, cwd=SOURCE):
    return subprocess.run(argv, cwd=cwd, env=os.environ.copy(), capture_output=True, text=True, timeout=timeout, check=True).stdout.strip()


def source_path(name):
    if not isinstance(name, str) or not name or '\\' in name or any(x in Path(name).parts for x in ('..', '.git')) or Path(name).is_absolute():
        raise ValueError('invalid source path')
    p = SOURCE / name
    if p.is_symlink() or not p.resolve().is_relative_to(SOURCE) or (p.suffix not in SOURCE_SUFFIXES and name != 'VERSION'):
        raise ValueError('source path not allowed')
    if name.split('/')[0] not in ('src', 'include', 'tests', 'scripts', 'ai-response-bridge', 'vendor') and name not in ('README.md', 'CMakeLists.txt', 'VERSION'):
        raise ValueError('source area not allowed')
    run(['git', 'ls-files', '--error-unmatch', '--', name])
    return p


def worker_identity():
    result = []
    for p in Path('/proc').glob('[0-9]*'):
        try:
            exe = p.joinpath('exe').resolve(strict=True)
            if exe.name not in ('nexus-live-cuda-worker', 'keryx-miner'): continue
            result.append({'pid': int(p.name), 'starttime': p.joinpath('stat').read_text().rsplit(')', 1)[1].split()[19], 'kind': exe.name})
        except (OSError, ValueError): pass
    return result


def recent_snapshot():
    paths = list((ROOT / 'logs').glob('nexus-ui-inference-*.jsonl'))
    if not paths: return {'status': 'UNAVAILABLE'}
    p = max(paths, key=lambda x: x.stat().st_mtime_ns)
    with p.open('rb') as f:
        f.seek(max(0, p.stat().st_size - 512 * 1024)); lines = f.read().splitlines()[1:]
    for line in reversed(lines):
        try:
            obj = json.loads(line); snap = obj.get('worker', obj)
            if snap.get('event') != 'NEXUS_TELEMETRY': continue
            age = (time.monotonic_ns() - snap['monotonic_ns']) / 1e9
            fields = ('schema_version', 'elapsed_ns', 'counters', 'rates', 'eligible_rates', 'latency_ms', 'scheduler', 'state', 'gpu_backends', 'gpu_ready')
            clean = {k: snap.get(k) for k in fields}
            clean.update(status='LIVE' if 0 <= age <= 5 else 'STALE', age_s=round(age, 3), wallet_authorization=snap.get('wallet', {}).get('authorization', 'UNKNOWN'))
            return clean
        except (ValueError, TypeError, KeyError): continue
    return {'status': 'UNAVAILABLE'}


class AgentAPI:
    def __init__(self):
        self.lock = threading.RLock()
        self.action_lock = threading.Lock()
        self.dev_lock = threading.Lock()
        self.requests = self.denied = 0
        self.last_request = None
        self.rate = {}
        self.last_action = None
        self.started = time.time()
        self.node_probe_cache = (0, {'status': 'UNAVAILABLE'})
        self.mirror = None
        from nexus_dashboard import TelemetrySessionReader
        self.diagnostic_reader = TelemetrySessionReader(ROOT)
        from nexus_escrow_availability import EscrowAvailabilityPoller
        self.availability_poller = EscrowAvailabilityPoller()
        self.keys = json.loads((STATE / 'keys-hashed.json').read_text())
        if (STATE / 'keys-hashed.json').stat().st_mode & 0o077: raise ValueError('unsafe key permissions')
        for p in (STATE / 'jobs').glob('*.json'):
            job = json.loads(p.read_text())
            if job.get('status') in ('QUEUED', 'RUNNING'):
                job.update(status='INTERRUPTED', detail='API restarted; action will not be replayed')
                atomic_json(p, job)

    def publish(self):
        with self.lock:
            try:
                path = STATE / 'keys-hashed.json'
                if path.stat().st_size <= 65536 and not path.is_symlink() and not path.stat().st_mode & 0o077:
                    keys = json.loads(path.read_text())
                    if isinstance(keys, list) and all(isinstance(k, dict) and {'name', 'scopes', 'sha256'} <= k.keys() for k in keys): self.keys = keys
            except (OSError, ValueError): pass
            jobs = sorted((STATE / 'jobs').glob('*.json'), key=lambda p: p.stat().st_mtime_ns)
            latest = json.loads(jobs[-1].read_text()) if jobs else None
            status = {'schema_version': 1, 'unix': time.time(), 'started_unix': self.started, 'endpoint': 'http://127.0.0.1:3340', 'status': 'ACTIVE',
                      'requests': self.requests, 'denied': self.denied, 'last_request_unix': self.last_request,
                      'keys': [{'name': k['name'], 'scopes': k['scopes'], 'fingerprint': k['sha256'][:12], 'active': k.get('active', True)} for k in self.keys],
                      'commands': list(COMMANDS), 'proposals': len(list((STATE / 'proposals').glob('*.json'))),
                      'latest_job': latest, 'command_busy': self.action_lock.locked(), 'development_busy': self.dev_lock.locked()}
            atomic_json(STATE / 'status.json', status)
            return status

    def auth(self, token, scope):
        path = STATE / 'keys-hashed.json'
        try:
            if path.is_symlink() or path.stat().st_mode & 0o077 or path.stat().st_size > 65536: return None, 503
            keys = json.loads(path.read_text())
            if not isinstance(keys, list) or len(keys) > 16: return None, 503
            if not all(isinstance(k, dict) and isinstance(k.get('name'), str) and isinstance(k.get('sha256'), str)
                       and re.fullmatch(r'[0-9a-f]{64}', k['sha256']) and isinstance(k.get('scopes'), list)
                       and set(k['scopes']) <= {'diagnostics', 'development', 'live'} for k in keys): return None, 503
        except (OSError, ValueError): return None, 503
        digest = hashlib.sha256(token.encode()).hexdigest()
        with self.lock:
            self.keys = keys
            key = next((k for k in self.keys if hmac.compare_digest(k['sha256'], digest) and k.get('active', True)), None)
            now = time.monotonic()
            if key:
                timestamps = [t for t in self.rate.get(key['name'], []) if now - t < 60]
                if len(timestamps) >= 120: return None, 429
                self.rate[key['name']] = timestamps + [now]
            if not key or scope not in key['scopes']:
                self.denied += 1
                return None, 403
            self.requests += 1; self.last_request = time.time()
            return key['name'], 200

    def diagnose(self):
        unit = run(['systemctl', '--user', 'show', SERVICE, '-p', 'ActiveState', '-p', 'MainPID', '-p', 'NRestarts'])
        memory = {l.split(':')[0]: int(l.split()[1]) for l in Path('/proc/meminfo').read_text().splitlines() if l.startswith(('MemTotal:', 'MemAvailable:', 'SwapTotal:', 'SwapFree:'))}
        nodes = []
        for proc in Path('/proc').glob('[0-9]*'):
            try:
                if not proc.joinpath('exe').resolve(strict=True).name.startswith('keryxd'): continue
                measured = {l.split(':')[0]: int(l.split()[1]) for l in proc.joinpath('status').read_text().splitlines() if l.startswith(('VmRSS:', 'VmSwap:'))}
                nodes.append({'pid': int(proc.name), 'starttime': proc.joinpath('stat').read_text().rsplit(')', 1)[1].split()[19], **measured})
            except (OSError, ValueError): pass
        if time.monotonic() - self.node_probe_cache[0] > 10:
            try:
                probe = run([str(ROOT / 'build/nexus-ui-inference/bin/nexus-ai-service-probe'), '--node', 'grpc://127.0.0.1:22110'])
                result = dict(re.findall(r'(network|version|synced)=([^ ]+)', probe))
                result['status'] = 'AVAILABLE'
            except (OSError, subprocess.SubprocessError): result = {'status': 'UNAVAILABLE'}
            self.node_probe_cache = (time.monotonic(), result)
        from nexus_dashboard import gpu_sample
        # Reuse the dashboard's session-aware stream reader. An empty model
        # would silently report zero claims on every diagnostic request.
        with self.lock:
            model = self.diagnostic_reader.poll()
        return {'service': dict(l.split('=', 1) for l in unit.splitlines() if '=' in l), 'workers': worker_identity(), 'telemetry': recent_snapshot(),
                'node_rpc': self.node_probe_cache[1], 'host_memory_kib': memory, 'memory_psi': Path('/proc/pressure/memory').read_text().strip(), 'nodes': nodes,
                'gpu_uuid': os.environ.get("NEXUS_PRIMARY_GPU_UUID", 'GPU-00000000-0000-0000-0000-000000000000'), 'gpu': gpu_sample(), 'escrow_claimed': model.claimed, 'escrow_available': self.availability_poller.pump()}

    def pre_stop(self):
        if hashlib.sha256(AUDIT.read_bytes()).hexdigest() != AUDIT_SHA: raise ValueError('read-only audit hash mismatch')
        from nexus_wallet_authorization import selected_certificate, DEFAULT_LAUNCHER, DEFAULT_KEY, DEFAULT_CERT, DEFAULT_PROBE
        payout, pubkey, _, _ = selected_certificate(DEFAULT_LAUNCHER, DEFAULT_KEY, DEFAULT_CERT, DEFAULT_PROBE)
        node = run([str(DEFAULT_PROBE), '--node', 'grpc://127.0.0.1:22110'])
        if 'synced=true' not in node or 'network=mainnet' not in node: raise ValueError('node not synced mainnet')
        journal = ROOT / 'state/ai-service/ai-response.journal'
        before = hashlib.sha256(journal.read_bytes()).hexdigest()
        peers = worker_identity()
        output = STATE / 'audit-last.jsonl'
        # No CUDA or transactions: exact already-tested reorg-safe audit executable.
        run([str(AUDIT), 'grpc://127.0.0.1:22110', payout, pubkey,
             'bd34568cd89f5f19c6c3a6e1a61b929bc868709409eaad8e672d85f3c1eb5710', '0', '2', '120', str(journal), str(output)], timeout=135)
        rows = [json.loads(l) for l in output.read_text().splitlines() if l]
        sums = [r for r in rows if r.get('event') == 'AUDIT_SUMMARY']
        if len(sums) != 1: raise ValueError('audit summary ambiguous')
        s = sums[0]
        zeros = ('blocks_unavailable', 'rpc_errors', 'live_blocks_unavailable', 'live_queue_overflow', 'invalid_ai_requests', 'assigned_open', 'assigned_expired', 'journal_ambiguous')
        if any(s.get(k) is not True for k in ('complete', 'live_watermark_complete')) or any(type(s.get(k)) is not int or s[k] != 0 for k in zeros): raise ValueError('AI obligations/coverage unresolved')
        if s.get('audit_index_truncated') is not False or s.get('pending_class') != 'AI_PENDING_NONE_VERIFIED' or s.get('transactions') != 'READ_ONLY' or s.get('cuda') != 'NOT_LINKED': raise ValueError('AI audit indeterminate')
        if type(s.get('live_blocks_seen')) is not int or s.get('live_blocks_seen') != s.get('live_blocks_recovered'): raise ValueError('live coverage incomplete')
        snap = recent_snapshot()
        if snap.get('status') != 'LIVE' or snap.get('state', {}).get('inference') != 'IDLE': raise ValueError('inference active or telemetry stale')
        if before != hashlib.sha256(journal.read_bytes()).hexdigest() or peers != worker_identity(): raise ValueError('AI journal or worker changed during audit')
        if run(['systemctl', '--user', 'show', SERVICE, '-p', 'SendSIGKILL', '--value']) != 'no': raise ValueError('cooperative shutdown policy missing')

    def action(self, command):
        if command == 'diagnostics_capture':
            atomic_json(STATE / 'diagnostics-last.json', self.diagnose()); return 'diagnostics saved'
        if command == 'dashboard_refresh':
            run(['systemctl', '--user', 'restart', 'nexus-matrix-dashboard.service']); return 'read-only dashboard reopened'
        if command in ('worker_stop', 'worker_restart'):
            self.pre_stop()
            run(['systemctl', '--user', 'stop', SERVICE], timeout=60)
            if worker_identity(): raise ValueError('worker still alive; start refused')
        if command in ('worker_start', 'worker_restart'):
            if worker_identity(): raise ValueError('existing miner; start refused')
            node = run([str(ROOT / 'build/nexus-ui-inference/bin/nexus-ai-service-probe'), '--node', 'grpc://127.0.0.1:22110'])
            if 'network=mainnet' not in node or 'synced=true' not in node: raise ValueError('node not ready')
            run(['systemctl', '--user', 'start', SERVICE])
            # systemd start acknowledgement is not GPU/service readiness.
            return 'start requested; inspect telemetry for readiness'
        return 'worker stopped cooperatively'

    def submit_job(self, kind, key, request):
        ident = request.get('request_id')
        if not isinstance(ident, str) or not re.fullmatch(r'[A-Za-z0-9_-]{8,64}', ident): raise ValueError('request_id required (8-64 safe characters)')
        path = STATE / 'jobs' / (ident + '.json')
        with self.lock:
            if path.exists():
                old = json.loads(path.read_text())
                if old.get('key') != key or old.get('request_digest') != hashlib.sha256(json.dumps(request, sort_keys=True).encode()).hexdigest(): raise ValueError('request_id reused with different request')
                return old
            lock = self.action_lock if kind in ('live', 'ui') else self.dev_lock
            if len(list((STATE / 'jobs').glob('*.json'))) >= 128: raise ValueError('job history limit reached')
            if kind == 'development' and len(list((STATE / 'worktrees').iterdir())) >= 4: raise ValueError('isolated worktree limit reached')
            if not lock.acquire(blocking=False): raise ValueError('another operation is active')
            operation_fd = os.open(STATE / 'api-operation.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
            try: fcntl.flock(operation_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                os.close(operation_fd); lock.release(); raise ValueError('API control or another operation active')
            job = {'id': ident, 'kind': kind, 'key': key, 'status': 'QUEUED', 'created_unix': time.time(), 'command': request.get('command') if kind == 'live' else request.get('action') if kind == 'ui' else 'apply_proposal', 'request_digest': hashlib.sha256(json.dumps(request, sort_keys=True).encode()).hexdigest()}
            try: atomic_json(path, job)
            except Exception:
                os.close(operation_fd); lock.release(); raise
        def work():
            try:
                job['status'] = 'RUNNING'; atomic_json(path, job); self.publish()
                if kind == 'live': detail = self.action(request['command'])
                elif kind == 'ui': detail = self.ui_action(request)
                else:
                    proposal = request.get('proposal_id')
                    if not isinstance(proposal, str) or not re.fullmatch(r'[0-9a-f]{24}', proposal): raise ValueError('invalid proposal_id')
                    metadata = json.loads((STATE / 'proposals' / (proposal + '.json')).read_text())
                    if metadata.get('key') != key: raise ValueError('proposal belongs to another key')
                    worktree = STATE / 'worktrees' / ident
                    run(['git', '-c', 'core.hooksPath=/dev/null', 'worktree', 'add', '--detach', str(worktree), metadata['base_commit']], timeout=30)
                    patch = STATE / 'proposals' / (proposal + '.patch')
                    run(['git', 'apply', '--check', str(patch)], cwd=worktree)
                    run(['git', 'apply', str(patch)], cwd=worktree)
                    detail = 'patch applied in isolated worktree; not built, validated or deployed'
                    job['worktree'] = str(worktree)
                job.update(status='SUCCEEDED', detail=detail)
            except Exception as exc:
                # Subprocess output can contain operational data: retain only exception type.
                job.update(status='BLOCKED', detail=str(exc) if isinstance(exc, ValueError) else type(exc).__name__)
            finally:
                job['finished_unix'] = time.time()
                try: atomic_json(path, job)
                finally: os.close(operation_fd); lock.release()
                self.publish()
        threading.Thread(target=work, name='agent-' + kind, daemon=True).start()
        return job

    def miner_page(self, tab, log_filter):
        if type(tab) is not int or not 0 <= tab < 8 or log_filter not in ('ALL', 'AI', 'SUBMIT', 'JOB', 'ERROR'):
            raise ValueError('Invalid page/filter')
        # Reuse the live desktop feed instead of rereading multi-GiB telemetry
        # and duplicating GPU/explorer polling for each web client.
        try:
            shared = json.loads(run(['qdbus6', 'org.keryx.Nexus.Desktop', '/Window',
                'org.keryx.Nexus.Desktop.Control.matrixPage', str(tab), log_filter], timeout=2))
            if shared.get('schema_version') == 1 and shared.get('tab') == tab and isinstance(shared.get('panels'), list):
                return shared
        except (OSError, ValueError, subprocess.SubprocessError): pass
        with self.lock:
            if self.mirror is None:
                from nexus_web_mirror import WebMirror
                self.mirror = WebMirror()
        return self.mirror.snapshot(tab, log_filter)

    def ui_action(self, request):
        action, value = request.get('action'), request.get('value', '')
        if not isinstance(value, str) or len(value) > 1024: raise ValueError('Invalid setting value')
        if action == 'gpu_rx570_mode':
            from nexus_gpu_devices import configure_mode
            return configure_mode(value)
        if action == 'gpu_capabilities_refresh':
            from nexus_gpu_devices import refresh_capabilities
            return refresh_capabilities()
        if action in ('payout', 'key', 'new_key', 'certificate', 'verify'):
            from nexus_setup import configure
            return configure(action, value).replace("press V to verify", "use Verify and save")
        if action == 'update_check':
            from nexus_updates import check, status
            check(activate=False, inspect_only=True)
            return 'Version: ' + status().get('status', 'UNKNOWN')
        if action == 'updates_toggle':
            from nexus_updates import toggle
            return 'Automatic updates: ' + ('ON' if toggle()['enabled'] else 'OFF')
        if action == 'mcp_toggle':
            from nexus_mcp_control import toggle
            return 'MCP: ' + ('ON' if toggle() else 'OFF')
        if action == 'notifications_toggle':
            from nexus_notifications import notifications_enabled, set_notifications_enabled
            enabled = not notifications_enabled(); set_notifications_enabled(enabled)
            return 'Notifications: ' + ('ON' if enabled else 'OFF')
        if action == 'api_stop':
            run(['systemd-run', '--user', '--collect', '--unit=nexus-api-stop-' + secrets.token_hex(6),
                 '--on-active=1s', '/usr/bin/systemctl', '--user', 'stop', 'nexus-agent-api.service'])
            return 'API shutdown requested; miner unchanged'
        raise ValueError('Unsupported setting action')

    def proposal(self, request, key):
        patch = request.get('patch')
        if not isinstance(patch, str) or not patch.startswith('diff --git ') or len(patch.encode()) > 60000: raise ValueError('bounded unified Git patch required')
        if any(x in patch for x in ('GIT binary patch', 'new file mode', 'deleted file mode', 'old mode', 'new mode', 'rename from', 'rename to', 'copy from', 'copy to')): raise ValueError('only existing text source modifications allowed')
        paths = re.findall(r'^diff --git a/([^\s]+) b/([^\s]+)$', patch, re.M)
        if not paths: raise ValueError('patch paths required')
        for a, b in paths:
            if a != b: raise ValueError('renames not allowed')
            source_path(a)
        # Check all file headers too; git apply independently checks traversal/symlinks.
        for prefix, name in re.findall(r'^(---|\+\+\+) ([^\n]+)$', patch, re.M):
            if not name.startswith(('a/', 'b/')): raise ValueError('invalid patch file header')
            source_path(name[2:])
        if len(list((STATE / 'proposals').glob('*.json'))) >= 32: raise ValueError('proposal limit reached')
        ident = secrets.token_hex(12)
        meta = {'id': ident, 'key': key, 'created_unix': time.time(), 'base_commit': run(['git', 'rev-parse', 'HEAD']), 'files': sorted(set(a for a, b in paths)), 'status': 'PROPOSED'}
        fd = os.open(STATE / 'proposals' / (ident + '.patch'), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as f: f.write(patch)
        atomic_json(STATE / 'proposals' / (ident + '.json'), meta)
        self.publish(); return meta


class Handler(BaseHTTPRequestHandler):
    server_version = 'NexusAgentAPI/1'
    def log_message(self, *args): pass  # never log Authorization or URL secrets
    def setup(self):
        super().setup(); self.connection.settimeout(5)
    def send(self, status, data):
        body = json.dumps(data, separators=(',', ':')).encode()
        self.send_response(status); self.send_header('Content-Type', 'application/json'); self.send_header('X-Content-Type-Options', 'nosniff'); self.send_header('Cache-Control', 'no-store'); self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body)
    def dispatch(self):
        host = self.headers.get('Host', '')
        if host not in ('127.0.0.1:3340', 'localhost:3340'):
            return self.send(403, {'error': 'loopback Host required'})
        origin = self.headers.get('Origin')
        if origin and origin != 'http://' + host:
            return self.send(403, {'error': 'cross-origin requests not accepted'})
        assets = {'/': ('index.html', 'text/html; charset=utf-8'), '/app.css': ('app.css', 'text/css; charset=utf-8'), '/app.js': ('app.js', 'text/javascript; charset=utf-8')}
        if self.command == 'GET' and self.path in assets:
            name, kind = assets[self.path]
            body = (SOURCE / 'scripts/agent_web' / name).read_bytes()
            self.send_response(200)
            for name, value in [('Content-Type', kind), ('Content-Length', str(len(body))), ('Cache-Control', 'no-store'),
                                ('X-Content-Type-Options', 'nosniff'), ('X-Frame-Options', 'DENY'), ('Referrer-Policy', 'no-referrer'),
                                ('Content-Security-Policy', "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'")]:
                self.send_header(name, value)
            self.end_headers(); self.wfile.write(body); return
        scope = 'diagnostics'
        if self.path.startswith('/v1/dev/'): scope = 'development'
        if self.path == '/v1/live/commands': scope = 'live'
        if self.path in ('/v1/ui/actions', '/v1/ui/key'): scope = 'development'
        if '?' in self.path: return self.send(403, {'error': 'URL credentials not accepted'})
        authorization = self.headers.get('Authorization', '')
        token = authorization[7:] if authorization.startswith('Bearer ') else ''
        if not token or len(token) > 256: return self.send(401, {'error': 'bearer key required'})
        key, status = self.server.api.auth(token, scope)
        if not key: return self.send(status, {'error': 'scope denied or rate limit'})
        try:
            if self.command == 'GET':
                match = re.fullmatch(r'/v1/miner/([0-7])/(ALL|AI|SUBMIT|JOB|ERROR)', self.path)
                if match: return self.send(200, self.server.api.miner_page(int(match[1]), match[2]))
                if self.path == '/v1/status': return self.send(200, self.server.api.publish())
                if self.path == '/v1/diagnostics': return self.send(200, self.server.api.diagnose())
                if self.path == '/v1/dev/source-index': return self.send(200, {'commit': run(['git', 'rev-parse', 'HEAD']), 'paths': [p for p in run(['git', 'ls-files']).splitlines() if p.startswith(('src/', 'include/', 'tests/', 'scripts/', 'vendor/', 'ai-response-bridge/')) and Path(p).suffix in SOURCE_SUFFIXES]})
                if self.path.startswith('/v1/dev/source/'):
                    name = unquote(self.path[len('/v1/dev/source/'):]); p = source_path(name)
                    if p.stat().st_size > 256 * 1024: raise ValueError('source file too large')
                    return self.send(200, {'path': name, 'text': p.read_text(), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()})
                if self.path == '/v1/jobs':
                    jobs = [json.loads(p.read_text()) for p in (STATE / 'jobs').glob('*.json')]
                    return self.send(200, {'jobs': sorted([j for j in jobs if j.get('key') == key], key=lambda j: j.get('created_unix', 0), reverse=True)})
                if self.path == '/v1/dev/proposals':
                    proposals = [json.loads(p.read_text()) for p in (STATE / 'proposals').glob('*.json')]
                    return self.send(200, {'proposals': sorted([p for p in proposals if p.get('key') == key], key=lambda p: p.get('created_unix', 0), reverse=True)})
                if self.path.startswith('/v1/dev/proposals/'):
                    ident = self.path[len('/v1/dev/proposals/'):]
                    if not re.fullmatch(r'[0-9a-f]{24}', ident): raise ValueError('invalid proposal ID')
                    meta = json.loads((STATE / 'proposals' / (ident + '.json')).read_text())
                    if meta.get('key') != key: return self.send(403, {'error': 'proposal belongs to another key'})
                    return self.send(200, {'proposal': meta, 'patch': (STATE / 'proposals' / (ident + '.patch')).read_text()})
                if self.path.startswith('/v1/jobs/'):
                    ident = self.path[len('/v1/jobs/'):]
                    if not re.fullmatch(r'[A-Za-z0-9_-]{8,64}', ident): raise ValueError('invalid job ID')
                    job = json.loads((STATE / 'jobs' / (ident + '.json')).read_text())
                    if job['key'] != key: return self.send(403, {'error': 'job belongs to another key'})
                    return self.send(200, job)
            if self.command == 'POST':
                if self.headers.get('Transfer-Encoding'): raise ValueError('chunked requests not accepted')
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= 65536: raise ValueError('request too large/empty')
                obj = json.loads(self.rfile.read(length))
                if not isinstance(obj, dict): raise ValueError('JSON object required')
                if self.path == '/v1/ui/actions':
                    return self.send(202, self.server.api.submit_job('ui', key, obj))
                if self.path == '/v1/ui/key':
                    if obj != {'rotate': True}: raise ValueError('Explicit key rotation required')
                    ensure_agent_key(rotate=True)
                    from nexus_mcp_control import client_token
                    return self.send(200, {'key': client_token()})
                if self.path == '/v1/dev/proposals': return self.send(201, self.server.api.proposal(obj, key))
                if self.path == '/v1/dev/apply': return self.send(202, self.server.api.submit_job('development', key, obj))
                if self.path == '/v1/live/commands':
                    if obj.get('command') not in COMMANDS: raise ValueError('unsupported command')
                    return self.send(202, self.server.api.submit_job('live', key, obj))
            self.send(404, {'error': 'unknown endpoint'})
        except (ValueError, KeyError): self.send(400, {'error': 'invalid request or operation busy'})
        except (OSError, subprocess.SubprocessError): self.send(503, {'error': 'resource unavailable'})
        finally: self.server.api.publish()
    def do_GET(self): self.dispatch()
    def do_POST(self): self.dispatch()


class Server(ThreadingHTTPServer):
    daemon_threads = True
    def __init__(self, api):
        self.api = api; self.slots = threading.BoundedSemaphore(8)
        super().__init__(('127.0.0.1', 3340), Handler)
    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False): request.close(); return
        try: super().process_request(request, address)
        except Exception: self.slots.release(); raise
    def process_request_thread(self, request, address):
        try: super().process_request_thread(request, address)
        finally: self.slots.release()


def ensure_agent_key(rotate=False):
    """Called explicitly by the miner dashboard; never print the secret."""
    STATE.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(STATE, 0o700)
    fd = os.open(STATE / 'key-management.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        existing = None
        try:
            raw = json.loads((STATE / 'client-keys.json').read_text())
            entries = json.loads((STATE / 'keys-hashed.json').read_text())
            token = raw.get('agent')
            if isinstance(token, str) and re.fullmatch(r'nx_[A-Za-z0-9_-]{43}', token) and len(entries) == 1:
                k = entries[0]
                if k.get('active') and k.get('name') == 'agent' and k.get('sha256') == hashlib.sha256(token.encode()).hexdigest() and set(k.get('scopes', [])) == {'diagnostics', 'development', 'live'}:
                    existing = token
        except (OSError, ValueError, TypeError): pass
        if existing and not rotate: return 'Single key already available · press C to copy'
        token = 'nx_' + secrets.token_urlsafe(32)
        # Replacing the verifier revokes all old scoped keys immediately.
        atomic_json(STATE / 'keys-hashed.json', [{'name': 'agent', 'sha256': hashlib.sha256(token.encode()).hexdigest(),
                    'scopes': ['diagnostics', 'development', 'live'], 'active': True}])
        atomic_json(STATE / 'client-keys.json', {'agent': token})
        return 'Single key generated · all permissions · press C to copy'
    finally: os.close(fd)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--create-key', action='store_true')
    parser.add_argument('--rotate-key', action='store_true')
    parser.add_argument('--revoke', action='store_true')
    args = parser.parse_args()
    os.umask(0o077)
    STATE.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(STATE, 0o700)
    for name in ('tmp', 'jobs', 'proposals', 'worktrees'): (STATE / name).mkdir(parents=True, exist_ok=True, mode=0o700)
    os.environ.update(TMPDIR=str(STATE / 'tmp'), TMP=str(STATE / 'tmp'), TEMP=str(STATE / 'tmp'))
    if args.revoke:
        keys = json.loads((STATE / 'keys-hashed.json').read_text())
        for key in keys: key['active'] = False
        atomic_json(STATE / 'keys-hashed.json', keys)
        print('Key revoked; miner unchanged.'); return
    if args.create_key or args.rotate_key:
        print(ensure_agent_key(rotate=args.rotate_key)); return
    api = AgentAPI(); server = Server(api)
    def heartbeat():
        while True: api.publish(); time.sleep(2)
    threading.Thread(target=heartbeat, daemon=True).start()
    server.serve_forever()


if __name__ == '__main__': main()
