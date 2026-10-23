"""GPU display identities, separate from compute admission and vendor lane keys.

NVIDIA names are queried only for the configured authorized UUID while enabled.
AMD names come from existing inventory or read-only udev/PCI metadata. Naming a
new model never authorizes its kernels, inference, or GPU selection.
"""
import csv
from pathlib import Path
import re
import subprocess
import time

_CACHE = {}


def clean_name(value):
    if not isinstance(value, str): return ''
    return ' '.join(''.join(c for c in value if c.isprintable()).split())[:128]


def nvidia_name(uuid, enabled):
    from nexus_gpu_selection import CUDA_UUID
    if uuid != CUDA_UUID: return ''  # No enumeration or queries to excluded GPUs.
    cached = _CACHE.get(uuid)
    if not enabled: return cached[1] if cached else ''
    if cached and time.monotonic() < cached[0]: return cached[1]
    name = cached[1] if cached else ''
    try:
        result = subprocess.run(['nvidia-smi', '-i', uuid, '--query-gpu=name,uuid',
                                 '--format=csv,noheader'], capture_output=True,text=True,check=True,timeout=2)
        rows = list(csv.reader(result.stdout.strip().splitlines()))
        if len(rows)==1 and len(rows[0])==2 and rows[0][1].strip()==uuid:
            name=clean_name(rows[0][0])
    except (OSError,subprocess.SubprocessError): pass
    _CACHE[uuid]=(time.monotonic()+300,name)
    return name


def amd_name(device):
    """Use reported metadata, not a fabricated model name from a shared PCI ID."""
    key='amd:'+str(device)
    if key in _CACHE and time.monotonic()<_CACHE[key][0]:return _CACHE[key][1]
    name=''
    try:
        result=subprocess.run(['udevadm','info','--query=property','--path='+str(device)],
                              capture_output=True,text=True,check=True,timeout=2)
        props=dict(line.split('=',1) for line in result.stdout.splitlines() if '=' in line)
        name=clean_name(props.get('ID_MODEL_FROM_DATABASE',''))
        # udev appends a subsystem-specific board after the ambiguous chip family.
        board=re.search(r'\(([^()]*Radeon[^()]*)\)\s*$',name,re.I)
        if board:name='AMD '+board.group(1).strip()
    except (OSError,subprocess.SubprocessError):pass
    if not name:
        try: name='AMD GPU (PCI 1002:'+Path(device,'device').read_text().strip().removeprefix('0x')+')'
        except OSError:name='AMD GPU'
    _CACHE[key]=(time.monotonic()+300,name)
    return name


def secondary_device(inventory):
    devices=inventory.get('amd_devices',[])
    from nexus_gpu_selection import AMD_ID
    ident=inventory.get('secondary_device_id') or AMD_ID
    match=next((d for d in devices if d.get('id')==ident),None)
    # A sole device is unambiguous for presentation; compute gates are unchanged.
    return match if match is not None else devices[0] if len(devices)==1 else {}


def display_devices(inventory):
    # Legacy telemetry lanes are joined by identity, never by GPU1/GPU2 position.
    from nexus_gpu_registry import lane_device
    primary=lane_device(inventory,'rtx') or {'tag':'GPU1','id':None,'name':'Primary GPU','vendor':'unknown','runtime_lane':None}
    secondary=lane_device(inventory,'rx') or {'tag':'GPU2','id':None,'name':'Not detected','vendor':'unknown','runtime_lane':None}
    return primary,secondary


def enrich_inventory(inventory):
    result=dict(inventory)
    primary=dict(result.get('cuda_primary',{}))
    primary['name']=nvidia_name(primary.get('id'),result.get('selection',{}).get('nvidia_pom',False)) or primary.get('name','')
    result['cuda_primary']=primary
    return result


def display_name(device):
    name=clean_name(device.get('name')) or device.get('tag','GPU')
    # Strip branding, never invent a marketed model from a shared PCI identifier.
    name=re.sub(r'^(?:NVIDIA\s+)?GeForce\s+|^NVIDIA\s+|^AMD\s+Radeon\s+', '',name,flags=re.I)
    # Keep ambiguous families explicit rather than guessing a retail model.
    return name
