"""Device-ID registry and workload policy. Display order never determines a backend.

Policy decisions are not runtime admission. The existing native worker remains
responsible for capability checks, Rust parity and confirmed per-device binding.
"""
from nexus_gpu_identity import clean_name

WORKLOADS={'nvidia':'Nvidia','nexus':'Nexus (JIT+Vulkan)','nexus_bda':'Nexus Native (BDA Vulkan)'}


def vendor(device):
    value=str(device.get('vendor','')).strip().lower()
    ident=str(device.get('vendor_id','')).lower()
    if value in ('nvidia','0x10de') or ident in ('4318','0x10de'):return 'nvidia'
    if value in ('amd','ati','0x1002') or ident in ('4098','0x1002'):return 'amd'
    if value in ('intel','0x8086') or ident in ('32902','0x8086'):return 'intel'
    return 'unknown'


def automatic_workload(device):
    return {'nvidia':'nvidia','amd':'nexus','intel':'nexus'}.get(vendor(device))


def workload_options(device):
    options=[('Automatic','auto')]
    if vendor(device)=='nvidia':options += [('Nvidia','nvidia'),('Nexus (JIT+Vulkan) · Not recommended','nexus'),('Nexus Native (BDA Vulkan)','nexus_bda')]
    elif vendor(device) in ('amd','intel'):
        options += [('Nexus (JIT+Vulkan)','nexus'),('Nexus Native (BDA Vulkan)','nexus_bda')]
    return options


def resolve_workload(device,requested='auto'):
    if requested not in {value for _,value in workload_options(device)}:
        raise ValueError('Workload is not supported by this GPU vendor')
    return automatic_workload(device) if requested=='auto' else requested


def records(inventory):
    from nexus_gpu_selection import CUDA_UUID,AMD_ID
    if isinstance(inventory.get('devices'),list):source=inventory['devices']
    else:
        primary=dict(inventory.get('cuda_primary',{}))
        primary.setdefault('id',CUDA_UUID);primary.setdefault('vendor','NVIDIA')
        primary.setdefault('name','Primary GPU');primary['runtime_lane']='rtx'
        source=[primary]
        source += [dict(d,vendor=d.get('vendor','NVIDIA'),runtime_lane=None)
                   for d in inventory.get('nvidia_devices',[])]
        source += [dict(d,vendor=d.get('vendor','AMD'),runtime_lane='rx' if d.get('id')==AMD_ID else None)
                   for d in inventory.get('amd_devices',[])]
        source += [dict(d,runtime_lane=None) for d in inventory.get('intel_devices',[])]
    result=[];seen=set()
    for item in source:
        if not isinstance(item,dict):continue
        ident=item.get('id')
        if not isinstance(ident,str) or not ident or ident in seen:continue
        seen.add(ident);value=dict(item);value['vendor']=vendor(item)
        value['name']=clean_name(item.get('name')) or value['vendor'].title()+' GPU'
        value['tag']='GPU'+str(len(result)+1)
        value['automatic_workload']=automatic_workload(value)
        # No counter reassignment based on ordinal or vendor. Runtime lane must be explicit.
        if value.get('runtime_lane') not in ('rtx','rx'):value['runtime_lane']=None
        result.append(value)
    return result


def reported_vram_bytes(device, primary_vram_bytes=None):
    """Return reported VRAM; only the authorized primary may use live fallback."""
    value=device.get('vram_bytes')
    if type(value) in (int,float) and value >= 0:
        return value
    from nexus_gpu_selection import CUDA_UUID
    if (device.get('runtime_lane') == 'rtx' or device.get('id') == CUDA_UUID) and type(primary_vram_bytes) in (int,float):
        return primary_vram_bytes
    metrics=device.get('metrics')
    if isinstance(metrics,dict):
        value=metrics.get('vram_total_bytes')
        if type(value) in (int,float) and value >= 0:return value
    return None


def lane_device(inventory,lane):
    return next((d for d in records(inventory) if d.get('runtime_lane')==lane),None)
