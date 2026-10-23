"""Presentation preferences only. Never writes worker or GPU-selection settings."""
import json
import math
import os
from pathlib import Path
import re
import tempfile

DEFAULTS = {'schema_version': 1, 'gpu_view': 'auto', 'background': '#050805', 'text': '#37ff59', 'font_size': 12, 'glow': 60, 'workloads': {}}
MODES = ('auto', 'single', 'dual', 'rig')
THEMES = {'Matrix': ('#050805','#37ff59'), 'Ice': ('#041019','#61dcff'),
          'Amber': ('#100b03','#ffc85c'), 'Light': ('#f1f7f3','#153c25')}


def preferences_path():
    from nexus_agent_api import ROOT
    return ROOT / 'state/desktop/presentation.json'


def rgb(value):
    if not isinstance(value, str) or not re.fullmatch(r'#[0-9a-fA-F]{6}', value):
        raise ValueError('Choose an RGB color in #RRGGBB format')
    return tuple(int(value[i:i+2], 16) for i in (1, 3, 5))


def blend(a, b, fraction):
    return '#' + ''.join(f'{round(x*fraction+y*(1-fraction)):02x}' for x, y in zip(rgb(a), rgb(b)))


def contrast(a, b):
    def light(color):
        values = [v / 255 for v in rgb(color)]
        values = [v / 12.92 if v <= .04045 else ((v + .055) / 1.055)**2.4 for v in values]
        return sum(v*w for v, w in zip(values, (.2126, .7152, .0722)))
    x, y = sorted((light(a), light(b)))
    return (y + .05) / (x + .05)


def validate(values):
    if not isinstance(values, dict) or values.get('schema_version') != 1:
        raise ValueError('Unsupported UI preferences')
    if values.get('gpu_view') not in MODES:
        raise ValueError('Choose Automatic, Single GPU, Dual GPU or Miner Rig')
    result = dict(DEFAULTS)
    for key in ('gpu_view', 'background', 'text'):
        result[key] = values[key]
    result['font_size']=values.get('font_size',12)
    result['glow']=values.get('glow',60)
    if type(result['font_size']) is not int or not 10<=result['font_size']<=14:
        raise ValueError('Text size must be between 10 and 14 pixels')
    if type(result['glow']) is not int or not 0<=result['glow']<=100:
        raise ValueError('Neon intensity must be between 0 and 100')
    workloads=values.get('workloads',{})
    if not isinstance(workloads,dict) or len(workloads)>16 or any(
        not isinstance(k,str) or not re.fullmatch(r'(GPU-[0-9a-fA-F-]{36}|pci:[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7])',k)
        or v not in ('auto','nvidia','nexus','nexus_bda') for k,v in workloads.items()):
        raise ValueError('Invalid per-device workload preferences')
    result['workloads']=dict(workloads)
    for key in ('background', 'text'):
        rgb(result[key]); result[key] = result[key].lower()
    if contrast(result['background'], result['text']) < 4.5:
        raise ValueError('Text and background need more contrast for readable values')
    return result


def load(path=None):
    try:
        return validate(json.loads(Path(path or preferences_path()).read_text()))
    except (OSError, ValueError, TypeError, KeyError):
        return dict(DEFAULTS)


def save(values, path=None):
    result = validate(values)
    target = Path(path or preferences_path())
    target.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd, temporary = tempfile.mkstemp(prefix='.presentation-', dir=target.parent)
    try:
        with os.fdopen(fd, 'w') as stream:
            json.dump(result, stream, indent=2); stream.write('\n'); stream.flush(); os.fsync(stream.fileno())
        os.replace(temporary, target)
    finally:
        Path(temporary).unlink(missing_ok=True)
    return result


def large_gpu_count(model, gpu):
    """Count distinct reported devices with at least 7 GiB; never probes or admits a GPU."""
    import nexus_dashboard as dashboard
    from nexus_gpu_registry import records, reported_vram_bytes
    inventory = model.gpu_devices if isinstance(model.gpu_devices, dict) else {}
    primary = dashboard.gpu_fields(gpu)
    primary_bytes = primary.get('total') * 2**20 if type(primary.get('total')) in (int,float) else None
    count = 0
    for item in records(inventory):
        size = reported_vram_bytes(item, primary_bytes)
        if type(size) in (int, float) and math.isfinite(size) and size >= 7*2**30:
            count += 1
    return count


def resolved_view(values, model, gpu):
    mode = values.get('gpu_view', 'auto')
    if mode != 'auto':
        return mode
    eligible = large_gpu_count(model, gpu)
    return 'rig' if eligible > 2 else 'dual' if eligible >= 2 else 'single'
