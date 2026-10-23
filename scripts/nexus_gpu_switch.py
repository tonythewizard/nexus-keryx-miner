"""Guarded GPU handoff: restart only NEXUS, never a node or another miner."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time
import uuid

from nexus_agent_api import ROOT, recent_snapshot, worker_identity

SOURCE = Path(__file__).resolve().parents[1]
UNIT = 'nexus-v002-production.service'
SWITCH = 'nexus-gpu-switch.service'


def run(args, timeout=10):
    return subprocess.run(args, check=True, capture_output=True, text=True,
                          timeout=timeout).stdout.strip()


def idle_worker():
    workers = worker_identity()
    if not workers:
        return None
    if len(workers) != 1 or workers[0]['kind'] != 'nexus-live-cuda-worker':
        raise ValueError('GPU handoff requires one NEXUS worker and no stock miner')
    worker = workers[0]
    groups = Path('/proc', str(worker['pid']), 'cgroup').read_text()
    if UNIT not in {p for row in groups.splitlines() for p in row.split(':', 2)[-1].split('/')}:
        raise ValueError('GPU handoff requires the managed production service')
    snap = recent_snapshot()
    state = snap.get('state', {})
    if (snap.get('status') != 'LIVE' or state.get('inference') != 'IDLE'
            or state.get('pause_reason') or state.get('submission') not in
            {'ARMED', 'DISABLED', 'RPC_ACKNOWLEDGED', 'RPC_REJECTED',
             'CHAIN_OBSERVED', 'CHAIN_CONFIRMED', 'UNKNOWN_OUTCOME', 'UNKNOWN_AFTER_TIMEOUT'}):
        raise ValueError('GPU handoff deferred: AI or submission active, or telemetry unavailable')
    from nexus_dashboard import TelemetrySessionReader
    reader = TelemetrySessionReader(ROOT)
    try:
        model = reader.poll()
        require_idle_ai(model)
        if state.get('submission') in ('UNKNOWN_OUTCOME', 'UNKNOWN_AFTER_TIMEOUT'):
            if (getattr(model, 'last_submission_state', None) != state['submission'] or
                    getattr(model, 'last_submission_hash', None) not in model._block_seen['chain_observed'] or
                    getattr(model, 'unresolved_block_submissions', {None})):
                raise ValueError('GPU handoff deferred: unresolved block submission outcome')
    finally:
        reader.close()
    if worker_identity() != workers:
        raise ValueError('Worker identity changed during GPU handoff preflight')
    return worker


def require_idle_ai(model):
    """Cumulative discovery is not a pending obligation; an ACK is not AI confirmation."""
    metrics = model.ai_metrics
    if (not model.history_complete or model.native_status() != 'LIVE'
            or metrics['pending'] != 0
            or (metrics['requests_assigned'] or 0) > (metrics['responses_confirmed'] or 0)):
        raise ValueError('GPU handoff deferred: AI session requires reconciliation')


def prepare_switch(selection):
    if subprocess.run(['systemctl', '--user', 'is-active', '--quiet', SWITCH], timeout=3).returncode == 0:
        raise ValueError('A GPU handoff is already in progress')
    worker = idle_worker()
    return bool(worker)


def request_switch(request_id=None):
    temporary = str(ROOT / 'work/tmp')
    run(['systemd-run', '--user', '--collect', '--unit=' + SWITCH,
         '--service-type=exec', '--property=Restart=no'] +
        ['--setenv=' + k + '=' + temporary for k in ('TMPDIR', 'TMP', 'TEMP')] +
        ['/usr/bin/python3', '-B', str(Path(__file__).resolve()), '--apply-runtime'] +
        ([request_id] if request_id else []))


def validate_route(selection, workloads):
    from nexus_workload_admission import admit_routes, installed_worker
    if not any(selection[k] for k in ('nvidia_pom', 'amd_pom')):
        return
    worker, sha = installed_worker()
    admit_routes(selection, workloads, worker, sha)


def request_change(kind, change):
    """Stage confirmed changes; never change a running worker's selection file."""
    from nexus_gpu_selection import configuration, CUDA_UUID, AMD_ID
    from nexus_gpu_devices import STATE, prepare_state, atomic, amd_devices
    from nexus_ui_preferences import preferences_path
    from nexus_workload_admission import read_preferences
    from nexus_gpu_registry import resolve_workload
    if not isinstance(change, dict):
        raise ValueError('Invalid GPU configuration')
    prepare_state()
    fd = os.open(STATE / 'selection.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError('A GPU configuration change is already in progress') from None
        old_selection = configuration(); selection = dict(old_selection)
        preferences = read_preferences(preferences_path()); old_workloads = dict(preferences['workloads'])
        workloads = dict(old_workloads)
        device = change.get('device')
        if kind == 'gpu_select':
            if set(change) != {'device', 'enabled'} or type(change['enabled']) is not bool or device not in (CUDA_UUID, AMD_ID):
                raise ValueError('Select an explicitly authorized GPU')
            if change['enabled'] and device == AMD_ID and not any(d['id'] == AMD_ID for d in amd_devices()):
                raise ValueError('Authorized secondary GPU is not present')
            selection['nvidia_pom' if device == CUDA_UUID else 'amd_pom'] = change['enabled']
        elif kind == 'workload':
            if set(change) != {'device', 'workload'} or device not in (CUDA_UUID, AMD_ID):
                raise ValueError('This GPU has no admitted runtime lane')
            resolve_workload({'vendor': 'nvidia' if device == CUDA_UUID else 'amd'}, change['workload'])
            if device == AMD_ID and not any(d['id'] == AMD_ID for d in amd_devices()):
                raise ValueError('Selected secondary GPU is not present')
            workloads[device] = change['workload']
        else:
            raise ValueError('Unsupported GPU configuration action')
        if selection == old_selection and workloads == old_workloads:
            return 'GPU configuration is already selected; no restart needed'
        admission_selection = dict(selection)
        if kind == 'workload':
            # Check the explicitly requested backend even on an OFF device. This
            # is a read-only capability probe, never permission to enable it.
            admission_selection['nvidia_pom' if device == CUDA_UUID else 'amd_pom'] = True
        validate_route(admission_selection, workloads)
        prepare_switch(selection)
        request_id = uuid.uuid4().hex
        atomic(STATE / 'handoff-request.json', {
            'request_id': request_id, 'kind': kind, 'worker': worker_identity(),
            'requested_unix': time.time(),
            'previous_selection': old_selection, 'selection': selection,
            'previous_workloads': old_workloads, 'workloads': workloads})
        atomic(STATE / 'handoff.json', {'status': 'QUEUED', 'request_id': request_id})
        try:
            request_switch(request_id)
        except Exception:
            atomic(STATE / 'handoff.json', {'status': 'BLOCKED', 'request_id': request_id,
                                           'reason': 'Could not start the restart controller; settings unchanged'})
            raise
        return ('Configuration queued; guarded restart requested' if any(
            selection[k] for k in ('nvidia_pom', 'amd_pom')) else
            'All GPUs OFF requested; mining and AI service will stop after the guarded handoff')


def apply_runtime(request_id=None):
    from nexus_gpu_selection import configuration
    from nexus_gpu_devices import STATE, atomic
    with (STATE / 'selection.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        value = configuration()
        pending = None
        if request_id:
            from nexus_gpu_devices import private_json
            from nexus_ui_preferences import preferences_path
            from nexus_workload_admission import read_preferences
            pending = private_json(STATE / 'handoff-request.json', {})
            if (pending.get('request_id') != request_id or pending.get('previous_selection') != value or
                    pending.get('previous_workloads') != read_preferences(preferences_path())['workloads']):
                raise ValueError('GPU configuration changed before restart; request refused')
            value = pending['selection']
            validate_route(value, pending['workloads'])
        worker = idle_worker()
        if not worker and not pending:
            raise ValueError('Worker already stopped; automatic startup refused')
        if pending and pending['worker'] != ([worker] if worker else []):
            raise ValueError('Worker identity changed after configuration confirmation')
        started = pending['requested_unix'] if pending else time.time()
        marker = ROOT / 'state/updates/user-stop.json'
        if marker.exists() and json.loads(marker.read_text()).get('requested_unix', 0) >= started:
            raise ValueError('Manual shutdown takes priority over GPU handoff')
        atomic(STATE / 'handoff.json', {'status': 'DRAINING' if worker else 'PREPARING_START',
                                      'worker': worker, 'request_id': request_id})
        if worker:
            proc = Path('/proc', str(worker['pid']))
            expected = hashlib.file_digest((proc / 'exe').open('rb'), 'sha256').hexdigest()
            descriptor = os.pidfd_open(worker['pid'])
            try:
                if worker_identity() != [worker]:
                    raise ValueError('Worker changed before cooperative signal')
                if hashlib.file_digest((proc / 'exe').open('rb'), 'sha256').hexdigest() != expected:
                    raise ValueError('Worker executable changed before cooperative signal')
                signal.pidfd_send_signal(descriptor, signal.SIGINT)
            finally:
                os.close(descriptor)
            deadline = time.monotonic() + 45
            while (proc / 'exe').exists() and time.monotonic() < deadline:
                time.sleep(.2)
            if (proc / 'exe').exists():
                raise ValueError('Cooperative drain timed out; no force kill or duplicate startup')
        marker = ROOT / 'state/updates/user-stop.json'
        if marker.exists() and json.loads(marker.read_text()).get('requested_unix', 0) >= started:
            raise ValueError('Manual shutdown takes priority over GPU handoff')
        if worker_identity():
            raise ValueError('Another worker appeared; duplicate startup refused')
        if pending:
            # Only publish after the previous worker is gone; its live watcher
            # cannot partially apply an OFF toggle while AI is still draining.
            from nexus_ui_preferences import preferences_path, save
            from nexus_workload_admission import read_preferences
            if pending['kind'] == 'gpu_select':
                atomic(STATE / 'selection.json', value)
            else:
                save(dict(read_preferences(preferences_path()), workloads=pending['workloads']))
        run(['systemctl', '--user', 'stop', UNIT], timeout=15)
        # A confirmed restart may supersede an older Stop, never a newer one.
        if pending and marker.exists():
            saved_marker = marker.read_bytes()
            if json.loads(saved_marker).get('requested_unix', 0) >= started:
                raise ValueError('Manual shutdown takes priority over GPU handoff')
            if marker.read_bytes() != saved_marker:
                raise ValueError('Manual shutdown changed during GPU handoff')
            marker.unlink()
        if not any(value[k] for k in ('nvidia_pom', 'amd_pom')):
            atomic(STATE / 'handoff.json', {'status': 'STOPPED', 'request_id': request_id,
                   'selection': value, 'reason': 'All GPUs OFF; mining and AI service stopped by confirmation'})
            return
        # Startup calls ensure_runtime(), which takes this same file lock.
        # The active handoff service still excludes competing UI requests.
        fcntl.flock(lock, fcntl.LOCK_UN)
        try:
            start_runtime(value, worker, request_id, started)
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            # One rollback only, and only when there is no process to interrupt.
            # A newer Stop or an occupied/indeterminate worker always wins.
            if (not pending or worker_identity() or
                    run(['systemctl', '--user', 'show', '--property=ActiveState', '--value', UNIT], timeout=3)
                    not in ('inactive', 'failed') or
                    (marker.exists() and json.loads(marker.read_text()).get('requested_unix', 0) >= started)):
                raise
            fcntl.flock(lock, fcntl.LOCK_EX)
            try:
                if configuration() != value:
                    raise ValueError('Configuration changed; automatic rollback refused') from error
                atomic(STATE / 'selection.json', pending['previous_selection'])
                from nexus_ui_preferences import preferences_path, save
                from nexus_workload_admission import read_preferences
                save(dict(read_preferences(preferences_path()), workloads=pending['previous_workloads']))
            finally:
                fcntl.flock(lock, fcntl.LOCK_UN)
            run(['systemctl', '--user', 'stop', UNIT], timeout=15)
            previous = pending['previous_selection']
            if any(previous[k] for k in ('nvidia_pom', 'amd_pom')):
                start_runtime(previous, worker, request_id, started)
            atomic(STATE / 'handoff.json', {'status': 'ROLLED_BACK', 'request_id': request_id,
                   'selection': previous, 'reason': str(error)[:400] + '; previous configuration restored'})


def start_runtime(value, worker, request_id, started):
    from nexus_gpu_devices import STATE, atomic
    marker = ROOT / 'state/updates/user-stop.json'
    def check_manual_stop():
        if marker.exists() and json.loads(marker.read_text()).get('requested_unix', 0) >= started:
            raise ValueError('Manual shutdown takes priority over GPU handoff')
    check_manual_stop()
    # Explicit, confirmed user restarts are not an automatic crash loop.
    run(['systemctl', '--user', 'reset-failed', UNIT], timeout=10)
    readiness_started = time.monotonic()
    run(['systemctl', '--user', 'start', UNIT], timeout=15)
    run(['systemctl', '--user', 'start', 'nexus-matrix-dashboard.service'], timeout=10)
    atomic(STATE / 'handoff.json', {'status': 'START_REQUESTED',
                                  'request_id': request_id, 'selection': value,
                                  'previous_pid': worker['pid'] if worker else None})

    from nexus_gpu_selection import CUDA_UUID, AMD_ID
    from nexus_ui_preferences import preferences_path
    from nexus_workload_admission import read_preferences
    expected_secondary = 'vulkan-bda' if read_preferences(preferences_path())['workloads'].get(AMD_ID) == 'nexus_bda' else 'vulkan-jit'
    expected_backend = ('vulkan-bda' if read_preferences(preferences_path())['workloads'].get(CUDA_UUID) == 'nexus_bda'
                        else 'vulkan-jit' if read_preferences(preferences_path())['workloads'].get(CUDA_UUID) == 'nexus'
                        else 'cuda-driver')
    deadline = time.monotonic() + 150
    while time.monotonic() < deadline:
        check_manual_stop()
        current = worker_identity()
        snap = recent_snapshot()
        # A fresh snapshot from before restart must never acknowledge a new process.
        elapsed = snap.get('elapsed_ns', -1)
        age = time.time() - started
        if (len(current) == 1 and current[0]['kind'] == 'nexus-live-cuda-worker'
                and current != ([worker] if worker else [])
                and snap.get('status') == 'LIVE' and isinstance(elapsed, int)
                and 0 <= elapsed / 1e9 <= age + 5
                and 0 <= snap.get('age_s', float('inf')) <= time.monotonic() - readiness_started
                and snap.get('state', {}).get('mining') == 'ACTIVE'
                and (not value['nvidia_pom'] or
                     snap.get('state', {}).get('pom_backend') == expected_backend)
                and (not value['amd_pom'] or
                     ((snap.get('gpu_backends') or {}).get(AMD_ID) == expected_secondary
                      and (snap.get('gpu_ready') or {}).get(AMD_ID) is True))):
            atomic(STATE / 'handoff.json', {'status': 'APPLIED', 'request_id': request_id,
                   'selection': value, 'worker': current[0]})
            return current[0]
        if subprocess.run(['systemctl', '--user', 'is-failed', '--quiet', UNIT],
                          timeout=3).returncode == 0:
            raise ValueError('Miner startup failed; inspect the service log before retrying')
        time.sleep(1)
    raise ValueError('Miner readiness timed out; no duplicate startup or automatic retry')

if __name__ == '__main__':
    import sys
    if (len(sys.argv) not in (2, 3) or sys.argv[1] != '--apply-runtime' or
            (len(sys.argv) == 3 and (len(sys.argv[2]) != 32 or any(c not in '0123456789abcdef' for c in sys.argv[2])))):
        raise SystemExit('usage: nexus_gpu_switch.py --apply-runtime [REQUEST_ID]')
    try:
        apply_runtime(sys.argv[2] if len(sys.argv) == 3 else None)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        from nexus_gpu_devices import STATE, atomic
        atomic(STATE / 'handoff.json', {'status': 'BLOCKED', 'request_id': sys.argv[2] if len(sys.argv) == 3 else None,
               'reason': str(error)
               if isinstance(error, ValueError) else type(error).__name__})
        raise SystemExit(1)
