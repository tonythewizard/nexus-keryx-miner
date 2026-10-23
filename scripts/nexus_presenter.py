"""Shared semantic Matrix panels for desktop and authenticated browser clients."""
import re
import time


def dual_gpu_panels(frame, model, gpu, tab, force=False, preferences=None):
    """Expose device scopes; never relabel NVIDIA-only values as fleet totals."""
    from nexus_gpu_metrics import summary, finite
    import nexus_dashboard as dashboard
    data = summary(model, gpu)
    if (not data['dual'] and not force) or tab not in (0, 1): return
    from nexus_gpu_identity import display_devices, secondary_device, display_name
    gpu1,gpu2=display_devices(model.gpu_devices)
    name1,name2=display_name(gpu1),display_name(gpu2)
    amd=secondary_device(model.gpu_devices)
    metrics = amd.get('metrics', {}); nvidia = dashboard.gpu_fields(gpu)
    selection = model.gpu_devices.get('selection', {})
    from nexus_gpu_selection import application_status
    def selection_status(key):
        return application_status(selection, key, model.gpu_selection_runtime, model.native_status() == 'LIVE', model.ai_backend)
    native = model.native or {}; counters = native.get('counters', {}); latency = native.get('latency_ms', {})
    fresh = model.native_status() == 'LIVE'; known = model.native is not None; lane = model.amd_lane
    ai_backend = model.ai_backend.get('backend') if fresh else None
    ai_identity = model.ai_backend.get('device_id') if fresh else None
    ai_gpu = next((d for d in (gpu1,gpu2) if d.get('id') == ai_identity),None)
    ai_device = ai_gpu['name']+' / '+ai_backend.upper() if ai_gpu and ai_backend else 'N/A'
    runtime_selection = model.gpu_selection_runtime if fresh else {}
    rtx_state = ('OFF' if runtime_selection.get('nvidia_applied') and not runtime_selection.get('nvidia_enabled')
                 else native.get('state', {}).get('mining', 'N/A') if fresh else 'N/A')
    state = ('FAILED' if lane.get('failed') else 'PAUSED' if lane.get('paused') else
             'PREPARING' if lane.get('preparing') else 'ACTIVE' if lane.get('busy') else 'WAITING') if data['amd_fresh'] else 'STALE / last counters' if lane else 'N/A'
    if runtime_selection.get('amd_applied') and not runtime_selection.get('amd_enabled'):
        state = 'OFF'
    def number(value, suffix='', decimals=2):
        value = finite(value)
        return f'{value:,.{decimals}f}{suffix}' if value is not None else 'N/A'
    def count(value): return number(value, decimals=0)
    def hz(value): return dashboard.format_hashrate(value)
    def rate(lane, window): return data['rates'][str(window)][lane]
    def series(lane): return ' / '.join(hz(rate(lane, w)) for w in (1, 10, 60))
    def steps(lane): return ' / '.join(number(rate(lane, w) * 256 / 1e6, ' M') if rate(lane, w) is not None else 'N/A' for w in (10, 60))
    def ms(key): return number(latency.get(key) if known else None, ' ms')
    def rows_panel(panel, rows):
        panel['rows'] = [(r[0], r[1], 'warning' if r[1].startswith('BLOCKED') else 'muted' if r[1].startswith('N/A') else 'value', r[2] if len(r)>2 else None) for r in rows]
        panel['free'] = {}
    def block_triplet(key):
        a, b = (model.gpu_block_stats[device][key] for device in ('rtx', 'rx'))
        return f'{a} / {b} / {model.stats[key]}'
    if tab == 0:
        window = next((w for w in (60, 10, 1) if rate('total', w) is not None), 1)
        heroes = [('FRESH HASHRATE / TOTAL', hz(rate('total', window)), f'GPU1 + GPU2 / same {window}s window'),
                  ('GPU1 / FRESH', hz(rate('rtx', window)), gpu1['name']),
                  ('GPU2 / FRESH', hz(rate('rx', window)), gpu2['name']),
                  ('FRESH Mstep/s / TOTAL', number(rate('total', window) * 256 / 1e6, ' M') if rate('total', window) is not None else 'N/A', '256 steps per fresh nonce'),
                  ('REPORTED POWER / TOTAL', number(data['board_power_total_w'], ' W'), 'NVIDIA draw + AMD reported sensor')]
        cards = [p for p in frame.panels if p['y'] == 6]
        for panel, (title, value, note) in zip(cards, heroes):
            panel['title'] = title; panel['rows'] = []; panel['free'] = {0: [(0, value, 'hero')], 1: [(0, note, 'muted')]}
        for panel in frame.panels:
            if panel['title'].startswith('NETWORK'):
                panel['title'] = 'NETWORK / SHARED STATE'
                rows_panel(panel, [('Mining / node', model.status + ' / ' + model.synced.upper()),
                    ('DAA / job', str(model.daa) + ' / ' + str(model.last_job)),
                    ('Fresh nonces / total', count(data['fresh_completed_total'])),
                    ('Fresh rates / 10s / 60s', hz(rate('total', 10)) + ' / ' + hz(rate('total', 60))),
                    ('GPU1 stale completed work', number(native.get('rates', {}).get('stale_work_fraction') * 100 if fresh and native.get('rates', {}).get('valid_stale_work_fraction') else None, '%')),
                    ('GPU2 discarded reservations', count(lane.get('discarded')) + ' / ' + count(lane.get('attempts')) if lane else 'N/A'),
                    ('AI service / device', model.ai_service + ' / ' + ai_device),
                    ('Escrow claimed / ACK', dashboard.krx(model.claimed.get('payout_sompi'), italian=True)),
                    ('Mature escrow / gross', dashboard.krx(model.escrow_available.get('amount_sompi'), italian=True))])
            elif panel['title'].startswith('BLOCK'):
                panel['title']='BLOCK PIPELINE / THIS SESSION'
                table=[('Rust winners',model.gpu_winners['rtx'],model.gpu_winners['rx'],sum(model.gpu_winners.values()))]
                table += [(name,model.gpu_block_stats['rtx'][key],model.gpu_block_stats['rx'][key],model.stats[key])
                          for name,key in [('Submitted','submit_sent'),('RPC acknowledged','rpc_ack'),
                          ('DAG observed','chain_observed'),('Chain confirmed','chain_confirmed'),('RPC rejected','rpc_rejected')]]
                table.append(('Unattributed DAG','—','—',model.gpu_block_unknown['chain_observed']))
                rows_panel(panel,[(row[0],' / '.join(str(x) for x in row[1:])) for row in table])
                panel['table_columns']=[name1,name2,'Total']
                panel['table_rows']=[[str(x) for x in row] for row in table]
            elif panel['title'] == 'GPU / AI SERVICE':
                panel['title'] = 'GPU FLEET / AI'
                def percent(value,capacity=100):
                    value,capacity=finite(value),finite(capacity)
                    return min(100,round(value/capacity*100)) if value is not None and capacity else None
                rows_panel(panel,[
                    ('GPU1 load',number(nvidia.get('util'),'%')+' · '+number(nvidia.get('temp'),' C',0)+' · '+number(nvidia.get('power'),' W',0),percent(nvidia.get('util'))),
                    ('GPU1 VRAM',number(nvidia.get('used',0)/1024 if nvidia else None,' GiB'),percent(nvidia.get('used'),nvidia.get('total'))),
                    ('GPU2 load',number(metrics.get('utilization_pct'),'%')+' · '+number(metrics.get('temperature_c'),' C',0)+' · '+number(metrics.get('power_w'),' W',0),percent(metrics.get('utilization_pct'))),
                    ('GPU2 VRAM',number(metrics.get('vram_used_bytes',0)/2**30 if metrics.get('vram_used_bytes') is not None else None,' GiB'),percent(metrics.get('vram_used_bytes'),amd.get('vram_bytes'))),
                    ('GPU1 / GPU2 state',rtx_state+' / '+state)])

    else:
        def blocks(device):
            d = model.gpu_block_stats[device]
            return f'{d["submit_sent"]} / {d["rpc_ack"]} / {d["chain_observed"]} / {d["chain_confirmed"]}'
        rtx_rows = [('Device enabled', selection_status('nvidia_pom')),
            ('Mining / AI', rtx_state + ' / ' + (ai_backend.upper() if ai_backend else 'N/A')),
            ('AI service device', ai_device),
            ('Load / clock', number(nvidia.get('util'), '%') + ' / ' + number(nvidia.get('clock'), ' MHz')),
            ('Temp / power', number(nvidia.get('temp'), ' C') + ' / ' + number(nvidia.get('power'), ' W')),
            ('VRAM used / total', number(nvidia.get('used') / 1024 if nvidia else None, ' GiB') + ' / ' + number(nvidia.get('total') / 1024 if nvidia else None, ' GiB')),
            ('All rate · 10 / 60s', hz(dashboard.pom_hashrate(model, '10s')) + ' / ' + hz(dashboard.pom_hashrate(model, '60s'))),
            ('Fresh rate · 1 / 10 / 60s', series('rtx')), ('Mstep/s · 10 / 60s', steps('rtx')),
            ('Work launched / done', count(counters.get('gpu_work_units_launched') if known else None) + ' / ' + count(counters.get('gpu_work_units_completed') if known else None)),
            ('Nonces fresh / stale', count(counters.get('eligible_attempts') if known else None) + ' / ' + count(counters.get('stale_work_discarded') if known else None)),
            ('Completed stale', number(native.get('rates', {}).get('stale_work_fraction') * 100 if fresh and native.get('rates', {}).get('valid_stale_work_fraction') else None, '%')),
            ('Kernels/s · 10 / 60s', ' / '.join(number(native.get('rates', {}).get('kernels_s_'+w) if fresh and native.get('rates', {}).get('valid_'+w) else None) for w in ('10s', '60s'))),
            ('Kernel / H2D / D2H', ms('cuda_kernel') + ' / ' + ms('cuda_h2d') + ' / ' + ms('cuda_d2h')),
            ('Gather · wall / CPU', ms('gguf_gather_wall') + ' / ' + ms('gguf_gather_cpu')),
            ('Merkle · wall / CPU', ms('merkle_wall') + ' / ' + ms('merkle_cpu')),
            ('Proof build / Rust', ms('proof_build') + ' / ' + ms('rust_verify')),
            ('Proofs built / verified', count(counters.get('proofs_completed') if known else None) + ' / ' + count(counters.get('proofs_rust_verified') if known else None)),
            ('Sent / ACK / DAG / chain', blocks('rtx'))]
        rx_rows = [('Device enabled', selection_status('amd_pom')),
            ('Mining state', state),
            ('Load / clock', number(metrics.get('utilization_pct'), '%') + ' / ' + number(metrics.get('graphics_clock_mhz'), ' MHz')),
            ('Temp / power', number(metrics.get('temperature_c'), ' C') + ' / ' + number(metrics.get('power_w'), ' W')),
            ('VRAM used / total', number(metrics.get('vram_used_bytes') / 2**30 if metrics.get('vram_used_bytes') is not None else None, ' GiB') + ' / ' + number(amd.get('vram_bytes') / 2**30 if amd.get('vram_bytes') is not None else None, ' GiB')),
            ('All rate incl. stale', 'N/A · not published'),
            ('Fresh rate · 1 / 10 / 60s', series('rx')), ('Mstep/s · 10 / 60s', steps('rx')),
            ('Reserved / completed', count(lane.get('attempts')) + ' / ' + count(lane.get('completed')) if lane else 'N/A'),
            ('Discarded / reserved', count(lane.get('discarded')) + ' / ' + count(lane.get('attempts')) if lane else 'N/A'),
            ('Discarded reservations', number(lane['discarded'] / lane['attempts'] * 100, '%') if lane.get('attempts') else 'N/A'),
            ('Kernels/s / kernel time', 'N/A / not published'),
            ('Transfer / gather / Merkle', 'N/A / not published'),
            ('Proofs built / verified', 'N/A / not published'),
            ('Winning commits', count(lane.get('verified')) if lane else 'N/A'),
            ('Rust winners', str(model.gpu_winners['rx'])),
            ('AI service device', ai_device), ('Sent / ACK / DAG / chain', blocks('rx'))]
        def metered(rows,util,used,total):
            result=[]
            for name,value in rows:
                raw=finite(util) if name=='Load / clock' else (finite(used)/total*100 if finite(used) is not None and finite(total) else None) if name=='VRAM used / total' else None
                result.append((name,value,min(100,round(raw)) if raw is not None else None))
            return result
        from nexus_gpu_registry import automatic_workload,WORKLOADS
        requested = (preferences or {}).get('workloads', {}).get(gpu1.get('id'), 'auto')
        observed = getattr(model,'pom_backend',{}).get('backend') if fresh else None
        actual_workload = {'cuda-driver':'nvidia','vulkan-jit':'nexus','vulkan-bda':'nexus_bda'}.get(observed)
        rtx_rows.insert(1,('Workload',WORKLOADS.get(actual_workload,'N/A / awaiting native acknowledgement')))
        rtx_rows.insert(2,('PoM execution backend',{'cuda-driver':'CUDA Driver API','vulkan-jit':'JIT / Vulkan','vulkan-bda':'Nexus Native (BDA Vulkan)'}.get(observed,'N/A / no live backend acknowledgement')))
        if gpu1.get('vendor', '').lower() == 'nvidia' and requested in ('nexus', 'nexus_bda') and observed not in ('vulkan-jit', 'vulkan-bda'):
            rtx_rows.insert(3, ('Vulkan admission', 'BLOCKED / awaiting attested worker restart; no CUDA fallback'))
        if observed in ('vulkan-jit', 'vulkan-bda'):
            unsupported={'Kernel / H2D / D2H','Gather · wall / CPU','Merkle · wall / CPU','Proof build / Rust'}
            rtx_rows=[(name,'N/A / Vulkan phase timing not published') if name in unsupported else (name,value) for name,value in rtx_rows]
        secondary_backend = (model.amd_lane or {}).get('backend') if fresh else None
        rx_rows.insert(1,('Workload',{'vulkan-jit':WORKLOADS['nexus'],'vulkan-bda':WORKLOADS['nexus_bda']}.get(secondary_backend,'N/A / awaiting native acknowledgement')))
        rx_rows.insert(2,('Requested workload',WORKLOADS.get((preferences or {}).get('workloads',{}).get(gpu2.get('id'),'auto'),'Automatic')))

        rtx_rows=metered(rtx_rows,nvidia.get('util'),nvidia.get('used'),nvidia.get('total'))
        rx_rows=metered(rx_rows,metrics.get('utilization_pct'),metrics.get('vram_used_bytes'),amd.get('vram_bytes'))
        rows_panel(frame.panels[0], rtx_rows)
        frame.panels[0]['title'] = name1+' / DEVICE VALUES'
        frame.panels[0]['device_id']=gpu1.get('id')
        def panel(title, rows, y=6, x=82):
            p = {'title': title, 'y': y, 'x': x, 'h': 20, 'w': 76, 'rows': [], 'free': {}}
            rows_panel(p, rows); return p
        second=panel(name2+' / DEVICE VALUES',rx_rows);second['device_id']=gpu2.get('id');frame.panels.append(second)
        used, cap = data['vram_used_total_bytes'], data['vram_capacity_total_bytes']
        frame.panels.append(panel('TOTALS / MATCHED UNITS AND WINDOWS', [
            ('Fresh hashes/s / 1 / 10 / 60s', series('total')),
            ('Fresh Mstep/s / 10 / 60s', steps('total')),
            ('Fresh completed nonces / sum', count(data['fresh_completed_total'])),
            ('Reported power sum / hottest', number(data['board_power_total_w'], ' W') + ' / ' + number(data['hottest_c'], ' C')),
            ('VRAM used / capacity sum', number(used / 2**30 if used is not None else None, ' GiB') + ' / ' + number(cap / 2**30 if cap is not None else None, ' GiB') + ' / not pooled'),
            ('Combined all-work / stale %', 'N/A / AMD completed-stale counters missing'),
        ], y=30, x=1))

    if not amd:
        for panel in frame.panels:
            panel['title']=panel['title'].replace('GPU2 / FRESH','GPU2 / NOT DETECTED')
            panel['rows']=[(name.replace('RX','Second GPU'),value,role,progress) for name,value,role,progress in panel['rows']]
            panel['free']={key:[(x,text.replace('RTX + RX','Second GPU not detected'),role) for x,text,role in parts]
                           for key,parts in panel['free'].items()}

    def named(text):return re.sub(r'GPU[12]',lambda m:name1 if m.group()=='GPU1' else name2,text)
    for panel in frame.panels:
        panel['title']=named(panel['title'])
        panel['rows']=[(named(k),named(v),role,progress) for k,v,role,progress in panel['rows']]
        panel['free']={key:[(x,named(text),role) for x,text,role in parts] for key,parts in panel['free'].items()}


def rig_panels(frame, model, gpu, tab, preferences):
    """Render a read-only, identity-based inventory for larger GPU rigs."""
    if tab not in (0, 1):
        return
    import math
    import nexus_dashboard as dashboard
    from nexus_gpu_registry import records, reported_vram_bytes
    from nexus_gpu_metrics import fresh_rates

    inventory = model.gpu_devices if isinstance(model.gpu_devices, dict) else {}
    devices = records(inventory)
    nvidia = dashboard.gpu_fields(gpu)
    amd_by_id = {d.get('id'): d for d in inventory.get('amd_devices', [])
                 if isinstance(d, dict) and isinstance(d.get('id'), str)}
    rates = {str(window): fresh_rates(model, window) for window in (1, 10, 60)}
    native = model.native or {}
    fresh = model.native_status() == 'LIVE'
    backend = getattr(model, 'pom_backend', {}).get('backend') if fresh else None
    backend_names = {'cuda-driver': 'NVIDIA / CUDA Driver',
                     'vulkan-jit': 'Nexus / JIT + Vulkan',
                     'vulkan-bda': 'Nexus Native / BDA Vulkan'}

    def number(value, suffix='', decimals=1):
        if type(value) not in (int, float) or not math.isfinite(value) or value < 0:
            return 'N/A'
        return f'{value:,.{decimals}f}{suffix}'

    def row(name, value, role='value', progress=None):
        return (name, str(value), role, progress)

    cards = []
    capacities = []
    measured = 0
    eligible = 0
    observed_rates = []
    for device in devices:
        ident = device.get('id')
        lane = device.get('runtime_lane')
        amd = amd_by_id.get(ident, {})
        metrics = dict(device.get('metrics') or {}) if isinstance(device.get('metrics'), dict) else {}
        if lane == 'rtx' and nvidia:
            metrics.update({'utilization_pct': nvidia.get('util'),
                            'vram_used_bytes': nvidia.get('used') * 2**20,
                            'temperature_c': nvidia.get('temp'),
                            'power_w': nvidia.get('power'),
                            'graphics_clock_mhz': nvidia.get('clock')})
        elif amd:
            metrics.update(amd.get('metrics') or {})

        primary_capacity = nvidia.get('total') * 2**20 if lane == 'rtx' and type(nvidia.get('total')) in (int,float) else None
        capacity = reported_vram_bytes(device, primary_capacity)
        used = metrics.get('vram_used_bytes')
        if used is None and lane == 'rtx' and nvidia and type(nvidia.get('used')) in (int, float):
            used = nvidia['used'] * 2**20
        if type(capacity) in (int, float) and math.isfinite(capacity) and capacity > 0:
            capacities.append(capacity)
            if capacity >= 7 * 2**30:
                eligible += 1
        util = metrics.get('utilization_pct', metrics.get('util'))
        if type(util) in (int, float) and math.isfinite(util):
            measured += 1
        util_ratio = min(100, round(util)) if type(util) in (int, float) and math.isfinite(util) else None
        vram_ratio = min(100, round(used / capacity * 100)) if (
            type(used) in (int, float) and math.isfinite(used) and
            type(capacity) in (int, float) and math.isfinite(capacity) and capacity > 0) else None

        rate_value = None
        if lane in ('rtx', 'rx'):
            for window in (60, 10, 1):
                rate_value = rates[str(window)][lane]
                if rate_value is not None:
                    break
        if rate_value is not None:
            observed_rates.append(rate_value)

        if lane == 'rtx':
            state = native.get('state', {}).get('mining', 'N/A') if fresh else 'STALE / N/A'
            workload = backend_names.get(backend, 'N/A / no live backend report')
        elif lane == 'rx':
            lane_state = model.amd_lane or {}
            state = ('FAILED' if lane_state.get('failed') else 'PAUSED' if lane_state.get('paused') else
                     'ACTIVE' if lane_state.get('busy') else 'WAITING') if lane_state else 'N/A'
            workload = backend_names.get(lane_state.get('backend'), 'N/A / no live backend report')
        else:
            state = str(device.get('workload') or 'DISPLAY ONLY · NOT ASSIGNED')
            workload = 'NOT ASSIGNED · display only'

        title = device.get('name') or device.get('tag') or 'GPU'
        rows = [
            row('Vendor / identity', f"{device.get('vendor', 'unknown').upper()} · {ident or 'N/A'}", 'muted'),
            row('Workload', workload),
            row('Device enabled', 'ON' if model.gpu_devices.get('selection',{}).get({'rtx':'nvidia_pom','rx':'amd_pom'}.get(lane,''),False) else 'OFF') if lane else row('Admission','Display only / no runtime lane'),
            row('Mining state', state, 'warning' if 'NOT' in state or state == 'N/A' else 'value'),
            row('VRAM used / total', number(used / 2**30 if type(used) in (int, float) else None, ' / ' + number(capacity / 2**30 if type(capacity) in (int, float) else None, ' GiB')),
                'value', vram_ratio),
            row('GPU load', number(util, '%'), 'value', util_ratio),
            row('Temperature / power', number(metrics.get('temperature_c'), ' °C') + ' / ' + number(metrics.get('power_w'), ' W')),
            row('Fresh PoM rate', dashboard.format_hashrate(rate_value), 'value'),
        ]
        cards.append({'title': title, 'y': 0, 'x': 0, 'h': len(rows) + 2, 'w': 1,
                      'rows': rows, 'free': {}, 'kind': 'rig-device', 'device_id': ident})

    reported_rate = sum(observed_rates) if observed_rates else None
    capacity_sum = sum(capacities) if capacities else None
    mode = preferences.get('gpu_view', 'auto').upper()
    summary_rows = [
        row('Display mode', 'MINER RIG · ' + mode),
        row('Inventory entries', str(len(devices))),
        row('GPUs with at least 7 GiB', f'{eligible} · automatic rig threshold: 3'),
        row('Load telemetry', f'{measured} / {len(devices)} devices reported'),
        row('Fresh PoM · reported lanes', dashboard.format_hashrate(reported_rate) if reported_rate is not None else 'N/A · no fresh lane rates'),
        row('VRAM capacity sum', number(capacity_sum / 2**30 if capacity_sum is not None else None, ' GiB · not pooled')),
        row('GPU admission', 'Display only · no workload or GPU selection changed', 'muted'),
    ]
    summary = {'title': 'MINER RIG / FLEET SUMMARY', 'y': 0, 'x': 0,
               'h': len(summary_rows) + 2, 'w': 1, 'rows': summary_rows,
               'free': {}, 'kind': 'rig-summary'}

    if tab == 1:
        frame.panels = [summary, *cards]
        return

    network = next((p for p in frame.panels if p['title'].startswith('NETWORK')), None)
    pipeline = next((p for p in frame.panels if p['title'].startswith('BLOCK')), None)
    activity = next((p for p in frame.panels if p['title'].startswith('LATEST')), None)
    preserved = []
    for panel, kind in ((network, 'rig-network'), (pipeline, 'rig-pipeline'), (activity, 'rig-activity')):
        if panel is None:
            continue
        clone = dict(panel)
        clone['kind'] = kind
        if kind == 'rig-network': clone['title'] = 'NETWORK / SHARED MINER STATE'
        if kind == 'rig-activity': clone['title'] = 'LATEST ACTIVITY'
        preserved.append(clone)
    frame.panels = [summary, *cards, *preserved]

class NativeFrame:
    """Collect semantic panels from the shared presenter, without terminal cells."""
    def __init__(self, height, width):
        self.height, self.width = height, width
        self.panels = []

    def panel(self, y, x):
        items = [p for p in self.panels if p['y'] <= y < p['y']+p['h'] and p['x'] <= x < p['x']+p['w']]
        return min(items, key=lambda p:p['h']*p['w']) if items else None

    def box(self, y, x, height, width, title):
        self.panels.append({'y':y,'x':x,'h':height,'w':width,'title':title,'rows':[],'free':{}})

    def kv(self, y, x, width, label, value, role='value'):
        p = self.panel(y,x)
        if p:
            value = 'N/A' if value is None else str(value)
            if value in ('N/A','UNKNOWN','NOT REPORTED','OFF','OFF / UNAVAILABLE'): role = 'muted'
            elif value in ('BLOCKED','WAITING','CHECK REQUIRED'):role='warning'
            elif value in ('ERROR','FAILED','INVALID','CHECK FAILED'):role='error'
            label=re.sub(r'\s*·\s*[A-Z](?:\s+(?:toggle|copy|import))?$', '', label)
            p['rows'].append((label,value,role,None))

    def rows(self, y, x, height, width, title, rows):
        rows = list(rows);self.box(y,x,max(height,len(rows)+2),width,title)
        for i,row in enumerate(rows): self.kv(y+i+1,x+2,width-4,row[0],row[1],row[2] if len(row)>2 else 'value')

    def meter(self, y, x, width, label, value, maximum, suffix):
        p = self.panel(y,x)
        if p:
            ratio = min(100,max(0,round(value/maximum*100))) if value is not None and maximum else None
            p['rows'].append((label,suffix if ratio is not None else 'N/A','text',ratio))

    def text(self, y, x, text, role='text', limit=None):
        p = self.panel(y,x)
        if p and role != 'border': p['free'].setdefault(y,[]).append((x,str(text),role))



def build_presenter_frame(height, width, model, gpu, tab, log_filter='ALL', preferences=None):
    import nexus_dashboard as dashboard
    from nexus_ui_preferences import load, resolved_view, large_gpu_count
    preferences = load() if preferences is None else preferences
    if tab == 8:
        frame = NativeFrame(height, width)
        frame.rows(1,1,9,width-2,'MINER CONTROLS',[
            ('Stop / restart','Use the desktop Controls page'),
            ('GPU overview view',preferences['gpu_view'].upper()),
            ('Effective overview',resolved_view(preferences,model,gpu).upper()),
            ('GPUs with at least 7 GiB',str(large_gpu_count(model,gpu))),
            ('Background',preferences['background']),('Text',preferences['text']),
            ('Mining selection','Unchanged by presentation preferences')])
        return frame
    frame = dashboard.build_matrix_frame(height, width, model, gpu, 'READ ONLY', tab, log_filter, frame_class=NativeFrame)
    if tab == 4:
        panel = frame.panels[0] if frame.panels else {
            'y': 6, 'x': 1, 'h': max(8, height - 9), 'w': width - 3,
        }
        events = [item for item in model.events if dashboard.event_matches(item[1], log_filter)]
        limit = max(1, min(100, height - 11))
        rows = []
        for stamp, event, detail in events[-limit:]:
            upper = event.upper()
            category = ('AI' if any(token in upper for token in ('AI_', 'OPOI', 'INFERENCE')) else
                        'JOB' if any(token in upper for token in ('JOB', 'TEMPLATE', 'STALE')) else
                        'SUBMIT' if any(token in upper for token in ('SUBMIT', 'CHAIN', 'RPC_ACKNOWLEDGED', 'RPC_REJECTED', 'TARGET')) else
                        'ERROR' if any(token in upper for token in ('ERROR', 'FAIL', 'BLOCKED', 'REJECTED')) else 'EVENT')
            rows.append([stamp, category, event, detail])
        if not rows:
            rows.append(['—', 'INFO', 'No matching events', f'Filter: {log_filter}'])
        panel.update({'title': 'EVENT LOG / ' + log_filter,
                      'kind': 'event-log', 'rows': [], 'free': {},
                      'table_columns': ['TIME', 'TYPE', 'EVENT', 'DETAIL'],
                      'table_rows': rows, 'h': max(8, height - 9), 'w': width - 3})
        frame.panels = [panel]
    effective = resolved_view(preferences, model, gpu)
    if effective == 'rig' and tab in (0, 1):
        rig_panels(frame, model, gpu, tab, preferences)
    elif tab == 1 or (tab == 0 and effective == 'dual'):
        dual_gpu_panels(frame, model, gpu, tab, force=tab == 0, preferences=preferences)
    elif tab == 0 and model.gpu_devices.get('amd_devices'):
        cards=[p for p in frame.panels if p['y']==6]
        if cards:cards[0]['title']='PoM HASHRATE / PRIMARY GPU'
    if tab==0:
        count=24
        for panel in frame.panels:
            if panel['title'].startswith('LATEST'):
                panel['free']={}
                for i,(stamp,event,detail) in enumerate(list(model.milestones)[-count:]):
                    category='BLOCK' if any(word in event for word in ('SUBMIT','CHAIN')) else 'STALE' if 'STALE' in event else 'AI' if 'AI_' in event else 'PoM'
                    role='error' if any(word in event for word in ('REJECTED','FAIL','ERROR')) else 'text'
                    panel['free'][i]=[(0,stamp,'muted'),(1,category,'warning' if category=='STALE' else 'label'),(2,detail,role)]
                if not panel['free']:panel['free'][0]=[(0,'Waiting for reported events','muted')]
    if tab in (6,7):
        for panel in frame.panels:
            panel['free']={}
    return frame
