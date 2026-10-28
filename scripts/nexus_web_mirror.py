"""Authenticated web presentation using the desktop's exact semantic presenter."""
import threading
import time
import nexus_dashboard as dashboard
from nexus_presenter import build_presenter_frame
from nexus_agent_api import ROOT

TABS = ('OVERVIEW', 'GPU', 'AI SERVICE', 'ESCROW', 'LOG', 'WALLET', 'DEVELOPMENT', 'SETUP', 'CONTROLS')
FILTERS = ('ALL', 'AI', 'SUBMIT', 'JOB', 'ERROR')


class WebMirror:
    def __init__(self, poll_interval=2):
        if type(poll_interval) not in (int, float) or not .5 <= poll_interval <= 10:
            raise ValueError('Invalid presentation polling interval')
        self.poll_interval = poll_interval
        self.lock = threading.Lock()
        self.refresh = threading.Event()
        self.ready = threading.Event()
        self.model = dashboard.DashboardModel()
        self.gpu = 'UNKNOWN'
        self.thread = threading.Thread(target=self.loop, daemon=True, name='nexus-web-presentation')
        self.thread.start()

    def loop(self):
        reader = dashboard.TelemetrySessionReader(ROOT)
        explorer = dashboard.OfficialExplorerPoller()
        from nexus_escrow_availability import EscrowAvailabilityPoller
        availability = EscrowAvailabilityPoller()
        wallet_due = gpu_due = 0
        gpu = 'UNKNOWN'
        while True:
            try:
                model = reader.poll()
                if self.refresh.is_set():
                    model._development_poll = model._setup_poll = 0
                    wallet_due = 0
                    self.refresh.clear()
                now = time.monotonic()
                if now >= wallet_due:
                    try: dashboard.load_wallet_status(model)
                    except Exception: pass
                    wallet_due = now + (30 if self.poll_interval < 2 else 2)
                for operation in (lambda: dashboard.refresh_development_status(model, ROOT),
                                  lambda: explorer.pump(model)):
                    try: operation()
                    except Exception: pass  # Missing sources stay UNKNOWN/STALE, never stop telemetry.
                try: model.escrow_available = availability.pump()
                except Exception: model.escrow_available = {'status': 'UNAVAILABLE'}
                if now >= gpu_due:
                    try: gpu = dashboard.gpu_sample()
                    except Exception: gpu = 'UNKNOWN'
                    try:
                        from nexus_gpu_devices import status
                        from nexus_gpu_identity import enrich_inventory
                        model.gpu_devices = enrich_inventory(status())
                    except (OSError, ValueError): model.gpu_devices = {'error': 'GPU inventory unavailable'}
                    gpu_due = now + 2
                view = model.presentation_copy()
                with self.lock: self.model, self.gpu = view, gpu
                self.ready.set()
            except Exception: pass
            time.sleep(self.poll_interval)

    def snapshot(self, tab, log_filter='ALL'):
        if type(tab) is not int or not 0 <= tab < len(TABS) or log_filter not in FILTERS:
            raise ValueError('Invalid page/filter')
        with self.lock: model, gpu = self.model.presentation_copy(), self.gpu
        return snapshot_for_model(model, gpu, tab, log_filter)


def snapshot_for_model(model, gpu, tab, log_filter='ALL', *, height=46, preferences=None, log_before=None):
    if type(tab) is not int or not 0 <= tab < len(TABS) or log_filter not in FILTERS:
        raise ValueError('Invalid page/filter')
    from nexus_gpu_metrics import header_hashrate, summary
    metrics = summary(model, gpu)
    frame = build_presenter_frame(height, 160, model, gpu, tab, log_filter,
                                  preferences=preferences, gpu_metrics=metrics, log_before=log_before)
    native = model.native or {}
    hashrate_label, hashrate = header_hashrate(model, gpu, metrics=metrics)
    return {'schema_version': 1, 'version': dashboard.RELEASE_VERSION,
            'tabs': TABS, 'tab': tab, 'status': model.native_status(),
            'network': model.network, 'synced': model.synced,
            'hashrate': dashboard.format_hashrate(hashrate), 'hashrate_label': hashrate_label,
            'gpu_metrics': metrics,
            'uptime': dashboard.uptime_text(model.snapshot()['uptime']),
            'memory': dashboard.host_memory(model), 'state': native.get('state', {}),
            'panels': frame.panels, 'updates': model.updates,
            'mcp': model.development.get('mcp', {}),
            'gpu_devices': model.gpu_devices,
            'escrow_public_key': model.setup.get('pubkey') or model.wallet.get('escrow_public_key'),
            'setup_key_exists': bool(model.setup.get('key_path') or model.wallet.get('escrow_public_key'))}
