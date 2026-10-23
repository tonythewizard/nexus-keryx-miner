"""Read-only per-device metrics; sums require equal units and paired worker clocks."""
import math
import time


def finite(value):
    return value if type(value) in (int, float) and math.isfinite(value) and value >= 0 else None


def paired_sample(model, snapshot):
    stamp = snapshot['monotonic_ns']
    counters = snapshot['counters']
    amd = model._pending_amd
    model._pending_amd = None
    if amd is not None:
        values = {key: amd[key] for key in ('attempts', 'completed', 'verified', 'discarded')}
        if model.amd_samples and any(values[k] < model.amd_samples[-1][1][k] for k in values):
            model.amd_samples.clear(); model.gpu_samples.clear()
        model.amd_lane = dict(amd, observed_monotonic_ns=stamp)
        model.amd_samples.append((stamp, values))
    model.gpu_samples.append((stamp, {'rtx': counters.get('eligible_attempts'),
                                     'rx': amd['completed'] if amd is not None else None}))


def fresh_rates(model, seconds):
    result = {'rtx': None, 'rx': None, 'total': None, 'seconds': None}
    if model.native_status() != 'LIVE' or len(model.gpu_samples) < 2:
        return result
    end, current = model.gpu_samples[-1]
    if end != (model.native or {}).get('monotonic_ns'):
        return result
    previous = next(((t, c) for t, c in reversed(model.gpu_samples)
                     if end - t >= seconds * 1_000_000_000), None)
    if previous is None:
        return result
    start, old = previous
    duration = (end - start) / 1e9
    result['seconds'] = duration
    for lane in ('rtx', 'rx'):
        a, b = old[lane], current[lane]
        if type(a) is int and type(b) is int and 0 <= a <= b:
            # Any missing endpoint within the window leaves that lane unknown.
            if all(c[lane] is not None for t, c in model.gpu_samples if start <= t <= end):
                result[lane] = (b - a) / duration
    if result['rtx'] is not None and result['rx'] is not None:
        result['total'] = result['rtx'] + result['rx']
    return result


def summary(model, gpu):
    import nexus_dashboard as dashboard
    devices = model.gpu_devices.get('amd_devices', [])
    from nexus_gpu_identity import secondary_device
    amd = secondary_device(model.gpu_devices) or None
    nvidia = dashboard.gpu_fields(gpu)
    metrics = amd.get('metrics', {}) if amd else {}
    fresh = model.native_status() == 'LIVE'
    lane = model.amd_lane
    amd_fresh = bool(fresh and lane and
        0 <= time.monotonic_ns() - lane.get('observed_monotonic_ns', 0) <= 3_000_000_000)
    rates = {str(n): fresh_rates(model, n) for n in (1, 10, 60)}
    def total(a, b):
        a, b = finite(a), finite(b)
        return a + b if a is not None and b is not None else None
    used = total(nvidia.get('used', 0) * 2**20 if nvidia else None, metrics.get('vram_used_bytes'))
    capacity = total(nvidia.get('total', 0) * 2**20 if nvidia else None, amd.get('vram_bytes') if amd else None)
    temps = [finite(nvidia.get('temp')), finite(metrics.get('temperature_c'))]
    counters = (model.native or {}).get('counters', {})
    aligned = bool(model.gpu_samples and model.gpu_samples[-1][0] == (model.native or {}).get('monotonic_ns')
                   and model.gpu_samples[-1][1]['rx'] is not None)
    completed = total(counters.get('eligible_attempts'), lane.get('completed')) if aligned else None
    return {'dual': amd is not None, 'amd_fresh': amd_fresh, 'rates': rates,
            'fresh_completed_total': completed,
            'board_power_total_w': total(nvidia.get('power'), metrics.get('power_w')),
            'vram_used_total_bytes': used,
            'vram_capacity_total_bytes': capacity,
            'hottest_c': max(temps) if all(v is not None for v in temps) else None,
            'combined_raw_hashrate': None,
            'combined_stale_fraction': None,
            'missing_reason': 'AMD does not publish all completed kernels or completed-stale work; reserved discards are not completed hashes'}


def header_hashrate(model, gpu):
    import nexus_dashboard as dashboard
    data = summary(model, gpu)
    if not data['dual']:
        return 'PoM', dashboard.pom_hashrate(model, '10s')
    window = next((str(w) for w in (60, 10, 1) if data['rates'][str(w)]['total'] is not None), '10')
    return 'Fresh PoM Σ', data['rates'][window]['total']
