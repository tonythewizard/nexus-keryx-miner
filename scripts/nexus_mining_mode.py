#!/usr/bin/env python3
"""Persistent SOLO/POOL mining mode and Stratum v4 multipool manager."""
from __future__ import annotations

import fcntl
import json
import os
from pathlib import Path
import re
import socket
import ssl
import stat
import subprocess
import sys
import time
from urllib.parse import urlparse
from nexus_agent_api import ROOT, atomic_json

URL_RE = re.compile(
    r"^(?:(?:stratum\+tcp|stratum\+ssl|stratum\+tls|stratum|tcp|ssl|tls)://)?"
    r"(?:\[[0-9a-fA-F:]+\]|[A-Za-z0-9._-]+)(?::(\d{1,5}))?/?$"
)
WORKER_RE = re.compile(r"^[A-Za-z0-9:._/-]{1,256}$")

PRESET_KERYXPOOL = {
    "id": "keryxpool",
    "name": "KeryxPool",
    "url": "stratum+tcp://s1.keryxpool.com:4401",
    "worker": "",
    "password": "x",
    "tls": False,
    "priority": 1,
    "preset": True,
    "configured": True,
    "note": "stratum+tcp://s1.keryxpool.com:4401 (VarDiff PoM 4401)",
}

PRESET_SUPRNOVA = {
    "id": "suprnova",
    "name": "Suprnova",
    "url": "stratum+tcp://krx.suprnova.cc:4404",
    "worker": "",
    "password": "x",
    "tls": False,
    "priority": 2,
    "preset": True,
    "configured": True,
    "note": "stratum+tcp://krx.suprnova.cc:4404 (TLS on 4405)",
}


def _state_dir() -> Path:
    custom = os.environ.get("NEXUS_MINING_MODE_FILE")
    if custom:
        p = Path(custom).resolve()
        if p.is_relative_to(Path("/tmp")) or p.is_relative_to(Path("/var/tmp")):
            raise ValueError("Mining mode configuration must use persistent storage")
        return p.parent
    data_root = Path(os.environ.get("NEXUS_DATA_DIR", str(ROOT))).resolve()
    if data_root.is_relative_to(Path("/tmp")) or data_root.is_relative_to(Path("/var/tmp")):
        raise ValueError("Mining mode configuration must use persistent storage")
    return data_root / "state/setup"


def config_path() -> Path:
    custom = os.environ.get("NEXUS_MINING_MODE_FILE")
    if custom:
        return Path(custom).resolve()
    return _state_dir() / "mining_mode.json"


def default_config() -> dict:
    return {
        "schema_version": 2,
        "mode": "solo",
        "active_pool_id": "keryxpool",
        "pools": [
            dict(PRESET_KERYXPOOL),
            dict(PRESET_SUPRNOVA),
        ],
    }


def _read_private(path: Path, limit: int = 65536) -> str:
    if path.is_symlink() or path.resolve().is_relative_to(Path("/tmp")) or path.resolve().is_relative_to(Path("/var/tmp")):
        raise ValueError("Use an absolute persistent file path")
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd) as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077 or info.st_size > limit:
            raise ValueError("Mining mode file must belong to you, be private (0600), and within size bounds")
        return stream.read()


def validate_pool_url(url: str) -> str:
    url = url.strip()
    if not url:
        return ""
    if len(url) > 512 or any(c.isspace() for c in url):
        raise ValueError("Invalid pool URL")
    match = URL_RE.fullmatch(url)
    if not match:
        raise ValueError("Enter a valid Stratum pool URL (e.g. stratum+tcp://host:port)")
    if match.group(1):
        port = int(match.group(1))
        if not (1 <= port <= 65535):
            raise ValueError("Pool port must be between 1 and 65535")
    if "://" not in url:
        url = "stratum+tcp://" + url
    return url


def parse_endpoint_parts(url: str) -> dict:
    normalized = validate_pool_url(url)
    if not normalized:
        return {"host": "", "port": 0, "tls": False}
    scheme = "stratum+tcp"
    rest = normalized
    if "://" in normalized:
        scheme, rest = normalized.split("://", 1)
    tls = scheme in ("stratum+ssl", "stratum+tls", "ssl", "tls")
    port = 4405 if tls else 4404
    host = rest.rstrip("/")
    if ":" in host:
        host_part, port_part = host.rsplit(":", 1)
        if port_part.isdigit():
            port = int(port_part)
            host = host_part
    return {"host": host, "port": port, "tls": tls}


def validate_pool_worker(worker: str) -> str:
    worker = worker.strip()
    if not worker:
        return ""
    if len(worker) > 256 or not WORKER_RE.fullmatch(worker):
        raise ValueError("Enter a valid wallet.worker identifier")
    return worker


def validate_pool_password(password: str) -> str:
    if len(password) > 256 or any(ord(c) < 32 or ord(c) == 127 for c in password):
        raise ValueError("Invalid pool password characters")
    return password


def test_connection(url: str, timeout: float = 3.0) -> dict:
    url = (url or "").strip()
    if not url:
        return {"ok": False, "ping_ms": 0.0, "error": "URL not configured"}
    try:
        parts = parse_endpoint_parts(url)
        if not parts["host"] or not parts["port"]:
            return {"ok": False, "ping_ms": 0.0, "error": "Invalid host or port"}
        t0 = time.perf_counter()
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        sock.connect((parts["host"], parts["port"]))
        if parts["tls"]:
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            ss = ctx.wrap_socket(sock, server_hostname=parts["host"])
            ss.close()
        else:
            sock.close()
        ping_ms = round((time.perf_counter() - t0) * 1000.0, 1)
        return {"ok": True, "ping_ms": ping_ms, "error": None}
    except Exception as exc:
        return {"ok": False, "ping_ms": 0.0, "error": str(exc)}


def load() -> dict:
    path = config_path()
    try:
        raw = _read_private(path)
    except FileNotFoundError:
        return default_config()
    try:
        obj = json.loads(raw)
    except json.JSONDecodeError:
        return default_config()
    if not isinstance(obj, dict):
        raise ValueError("Invalid mining mode schema")

    # Migrate v1 schema if needed
    if obj.get("schema_version") == 1:
        cfg = default_config()
        cfg["mode"] = str(obj.get("mode", "solo")).lower()
        pool_url = str(obj.get("pool_url", ""))
        pool_worker = str(obj.get("pool_worker", ""))
        pool_pw = str(obj.get("pool_password", "x"))
        if pool_url:
            if "suprnova" in pool_url.lower():
                cfg["pools"][0]["url"] = pool_url
                cfg["pools"][0]["worker"] = pool_worker
                cfg["pools"][0]["password"] = pool_pw
                cfg["active_pool_id"] = "suprnova"
            else:
                custom_pool = {
                    "id": "custom_migrated",
                    "name": "Custom Pool",
                    "url": pool_url,
                    "worker": pool_worker,
                    "password": pool_pw,
                    "tls": False,
                    "priority": 1,
                    "preset": False,
                    "configured": True,
                    "note": "Migrated from config v1",
                }
                cfg["pools"].insert(0, custom_pool)
                cfg["active_pool_id"] = "custom_migrated"
        return cfg

    mode = str(obj.get("mode", "solo")).lower()
    if mode not in ("solo", "pool"):
        mode = "solo"
    active_pool_id = str(obj.get("active_pool_id", "keryxpool"))
    pools_raw = obj.get("pools", [])
    if not isinstance(pools_raw, list) or not pools_raw:
        return default_config()

    pools = []
    has_suprnova = False
    has_keryxpool = False
    for p in pools_raw:
        if not isinstance(p, dict):
            continue
        pid = str(p.get("id", ""))
        if not pid:
            continue
        name = str(p.get("name", pid))
        url = str(p.get("url", ""))
        worker = str(p.get("worker", ""))
        pw = str(p.get("password", ""))
        is_preset = bool(p.get("preset", False))
        if pid == "suprnova":
            has_suprnova = True
        elif pid == "keryxpool":
            has_keryxpool = True
        is_conf = bool(url.strip())
        note = str(p.get("note", ""))
        if pid == "keryxpool" and not is_conf:
            url = PRESET_KERYXPOOL["url"]
            is_conf = True
            note = PRESET_KERYXPOOL["note"]
            if not pw:
                pw = PRESET_KERYXPOOL["password"]
        pools.append({
            "id": pid,
            "name": name,
            "url": validate_pool_url(url) if url else "",
            "worker": validate_pool_worker(worker) if worker else "",
            "password": validate_pool_password(pw),
            "tls": bool(p.get("tls", False)),
            "priority": int(p.get("priority", len(pools) + 1)),
            "preset": is_preset,
            "configured": is_conf,
            "note": note,
        })

    # Ensure standard presets are present
    if not has_keryxpool:
        pools.insert(0, dict(PRESET_KERYXPOOL))
    if not has_suprnova:
        pools.append(dict(PRESET_SUPRNOVA))

    # Sort pools by priority
    pools.sort(key=lambda x: x["priority"])

    return {
        "schema_version": 2,
        "mode": mode,
        "active_pool_id": active_pool_id,
        "pools": pools,
    }


def find_worker_pids() -> list[int]:
    pids = []
    try:
        for entry in Path('/proc').iterdir():
            if not entry.name.isdecimal():
                continue
            try:
                if entry.stat().st_uid != os.getuid():
                    continue
                exe = Path(os.readlink(entry / 'exe')).name.removesuffix(' (deleted)')
                if exe in ('nexus-live-cuda-worker', 'nexus-m21-oracle', 'nexus-live-vulkan-worker'):
                    pids.append(int(entry.name))
            except (OSError, ValueError):
                continue
    except OSError:
        pass
    return pids


def is_miner_paused() -> bool:
    pause_file = _state_dir() / "miner_pause.json"
    if not pause_file.is_file():
        return False
    workers = find_worker_pids()
    if not workers:
        try:
            pause_file.unlink(missing_ok=True)
        except OSError:
            pass
        return False
    try:
        data = _read_private(pause_file)
        stored_pids = set(data.get("pids", []))
        if stored_pids and not (stored_pids & set(workers)):
            pause_file.unlink(missing_ok=True)
            return False
        stopped = False
        for pid in (stored_pids & set(workers)) if stored_pids else set(workers):
            try:
                with open(f"/proc/{pid}/stat", "r") as f:
                    stat_parts = f.read().split(")")
                    if len(stat_parts) >= 2 and stat_parts[1].split()[0] == "T":
                        stopped = True
                        break
            except (OSError, IndexError):
                continue
        if not stopped and (time.time() - data.get("timestamp", 0) > 5.0):
            pause_file.unlink(missing_ok=True)
            return False
    except Exception:
        pass
    return True


def pause_miner() -> dict:
    import signal
    workers = find_worker_pids()
    if not workers:
        raise ValueError("Miner worker is not running; start or restart miner first")
    for pid in workers:
        try:
            os.kill(pid, signal.SIGSTOP)
        except OSError as e:
            raise ValueError(f"Failed to pause worker {pid}: {e}")
    pause_file = _state_dir() / "miner_pause.json"
    atomic_json(pause_file, {"paused": True, "pids": workers, "timestamp": time.time()})
    return {"paused": True, "message": "Miner paused: process kept open, GPU computation stopped"}


def resume_miner() -> dict:
    import signal
    pause_file = _state_dir() / "miner_pause.json"
    workers = find_worker_pids()
    for pid in workers:
        try:
            os.kill(pid, signal.SIGCONT)
        except OSError:
            pass
    try:
        if pause_file.exists():
            pause_file.unlink(missing_ok=True)
    except OSError:
        pass
    return {"paused": False, "message": "Miner resumed: GPU mining active"}


def toggle_miner_pause() -> dict:
    if is_miner_paused():
        return resume_miner()
    else:
        return pause_miner()


def public_view() -> dict:
    try:
        cfg = load()
    except (OSError, ValueError):
        cfg = default_config()
    active_id = cfg.get("active_pool_id", "keryxpool")
    pools = cfg.get("pools", [])
    active_pool = next((p for p in pools if p["id"] == active_id), None)
    if not active_pool and pools:
        active_pool = pools[0]
        active_id = active_pool["id"]

    public_pools = []
    backups = []
    for p in pools:
        pub = {
            "id": p["id"],
            "name": p["name"],
            "url": p["url"],
            "worker": p["worker"],
            "password_set": bool(p["password"]),
            "tls": p.get("tls", False),
            "priority": p["priority"],
            "preset": p.get("preset", False),
            "configured": p.get("configured", False),
            "note": p.get("note", ""),
            "active": (p["id"] == active_id),
        }
        public_pools.append(pub)
        if p["id"] != active_id and p.get("configured") and p.get("url"):
            backups.append(p["url"])

    return {
        "mode": cfg["mode"],
        "active_pool_id": active_id,
        "is_paused": is_miner_paused(),
        "active_pool": {
            "id": active_pool["id"],
            "name": active_pool["name"],
            "url": active_pool["url"],
            "worker": active_pool["worker"],
            "password_set": bool(active_pool["password"]),
            "priority": active_pool["priority"],
            "configured": active_pool["configured"],
            "note": active_pool.get("note", ""),
        } if active_pool else {},
        "pools": public_pools,
        "backup_urls": backups,
        # Legacy compatibility fields:
        "pool_url": active_pool["url"] if active_pool else "",
        "pool_worker": active_pool["worker"] if active_pool else "",
        "password_set": bool(active_pool["password"]) if active_pool else False,
    }


def _with_lock(action_fn):
    state_dir = _state_dir()
    state_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
    if state_dir.stat().st_uid != os.getuid() or state_dir.stat().st_mode & 0o077:
        raise ValueError("Setup directory must belong to you and be private (0700)")
    lock_path = state_dir / "mining-mode.lock"
    fd = os.open(lock_path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        current = load()
        updated = action_fn(current)
        atomic_json(config_path(), updated)
        return public_view()
    finally:
        os.close(fd)


def save(updates: dict, auto_restart: bool = False) -> dict:
    if not isinstance(updates, dict):
        raise ValueError("Invalid mining mode update")

    def _apply(current):
        if "mode" in updates and updates["mode"] is not None:
            m = str(updates["mode"]).strip().lower()
            if m not in ("solo", "pool"):
                raise ValueError("Mining mode must be SOLO or POOL")
            current["mode"] = m
        if "active_pool_id" in updates and updates["active_pool_id"]:
            current["active_pool_id"] = str(updates["active_pool_id"]).strip()
        # Backward compatibility for legacy pool_url, pool_worker, pool_password
        if any(k in updates for k in ("pool_url", "pool_worker", "pool_password")):
            active_id = current.get("active_pool_id", "suprnova")
            pool = next((p for p in current["pools"] if p["id"] == active_id), None)
            if not pool and current["pools"]:
                pool = current["pools"][0]
            if pool:
                if "pool_url" in updates and updates["pool_url"] is not None:
                    pool["url"] = validate_pool_url(updates["pool_url"])
                    pool["configured"] = bool(pool["url"])
                if "pool_worker" in updates and updates["pool_worker"] is not None:
                    pool["worker"] = validate_pool_worker(updates["pool_worker"])
                if "pool_password" in updates and updates["pool_password"] is not None:
                    pool["password"] = validate_pool_password(updates["pool_password"])
        return current

    res = _with_lock(_apply)
    if auto_restart:
        res["restart"] = restart_miner(force=True)
    return res


def save_pool(pool_data: dict, auto_restart: bool = False) -> dict:
    if not isinstance(pool_data, dict):
        raise ValueError("Invalid pool data")

    def _apply(current):
        pid = str(pool_data.get("id", "")).strip()
        name = str(pool_data.get("name", "")).strip()
        url = str(pool_data.get("url", "")).strip()
        worker = str(pool_data.get("worker", "")).strip()
        password = str(pool_data.get("password", "")).strip()

        if url:
            url = validate_pool_url(url)
        if worker:
            worker = validate_pool_worker(worker)
        if password:
            password = validate_pool_password(password)

        if not pid:
            # Generate new custom pool id
            pid = f"custom_{int(time.time())}"
            if not name:
                name = f"Pool {pid}"
            pool_entry = {
                "id": pid,
                "name": name,
                "url": url,
                "worker": worker,
                "password": password,
                "tls": bool(pool_data.get("tls", False)),
                "priority": len(current["pools"]) + 1,
                "preset": False,
                "configured": bool(url),
                "note": "Custom pool",
            }
            current["pools"].append(pool_entry)
        else:
            # Update existing pool
            matched = next((p for p in current["pools"] if p["id"] == pid), None)
            if matched:
                if name:
                    matched["name"] = name
                matched["url"] = url
                matched["configured"] = bool(url)
                if worker:
                    matched["worker"] = worker
                if "password" in pool_data and pool_data["password"] is not None:
                    matched["password"] = password
                if "tls" in pool_data:
                    matched["tls"] = bool(pool_data["tls"])
                if "note" in pool_data:
                    matched["note"] = str(pool_data["note"])
            else:
                # Add new pool with specified ID
                if not name:
                    name = pid
                current["pools"].append({
                    "id": pid,
                    "name": name,
                    "url": url,
                    "worker": worker,
                    "password": password,
                    "tls": bool(pool_data.get("tls", False)),
                    "priority": len(current["pools"]) + 1,
                    "preset": False,
                    "configured": bool(url),
                    "note": "Custom pool",
                })
        return current

    res = _with_lock(_apply)
    if auto_restart:
        res["restart"] = restart_miner(force=True)
    return res


def delete_pool(pool_id: str, auto_restart: bool = False) -> dict:
    pool_id = str(pool_id).strip()
    if not pool_id:
        raise ValueError("Pool ID required")

    def _apply(current):
        matched = next((p for p in current["pools"] if p["id"] == pool_id), None)
        if not matched:
            return current
        if matched.get("preset"):
            # Presets cannot be removed, only cleared/unconfigured
            matched["url"] = ""
            matched["worker"] = ""
            matched["password"] = ""
            matched["configured"] = False
            if pool_id == "keryxpool":
                matched["note"] = "Not configured (unverified endpoint)"
        else:
            current["pools"] = [p for p in current["pools"] if p["id"] != pool_id]
        if current.get("active_pool_id") == pool_id:
            current["active_pool_id"] = "suprnova"
        # Re-index priorities
        for idx, p in enumerate(current["pools"]):
            p["priority"] = idx + 1
        return current

    res = _with_lock(_apply)
    if auto_restart:
        res["restart"] = restart_miner(force=True)
    return res


def activate_pool(pool_id: str, auto_restart: bool = False) -> dict:
    pool_id = str(pool_id).strip()
    if not pool_id:
        raise ValueError("Pool ID required")

    def _apply(current):
        matched = next((p for p in current["pools"] if p["id"] == pool_id), None)
        if not matched:
            raise ValueError(f"Pool '{pool_id}' not found")
        current["active_pool_id"] = pool_id
        # Set active pool to priority 1, other pools follow
        matched["priority"] = 1
        other_priority = 2
        for p in current["pools"]:
            if p["id"] != pool_id:
                p["priority"] = other_priority
                other_priority += 1
        current["pools"].sort(key=lambda x: x["priority"])
        return current

    res = _with_lock(_apply)
    if auto_restart:
        res["restart"] = restart_miner(force=True)
    return res


def reorder_pools(ordered_ids: list[str], auto_restart: bool = False) -> dict:
    if not isinstance(ordered_ids, list):
        raise ValueError("Ordered pool ID list required")

    def _apply(current):
        id_to_pool = {p["id"]: p for p in current["pools"]}
        reordered = []
        priority = 1
        for pid in ordered_ids:
            if pid in id_to_pool:
                p = id_to_pool.pop(pid)
                p["priority"] = priority
                reordered.append(p)
                priority += 1
        for remaining in id_to_pool.values():
            remaining["priority"] = priority
            reordered.append(remaining)
            priority += 1
        current["pools"] = reordered
        if reordered:
            current["active_pool_id"] = reordered[0]["id"]
        return current

    res = _with_lock(_apply)
    if auto_restart:
        res["restart"] = restart_miner(force=True)
    return res


def restart_miner(force: bool = True) -> dict:
    """Forcibly and automatically restart the production miner service and native worker."""
    if os.environ.get("NEXUS_SKIP_AUTO_RESTART") == "1":
        return {"restarted": False, "skipped": True, "reason": "NEXUS_SKIP_AUTO_RESTART is set"}

    # Clear user-stop record if present so the service isn't held stopped
    user_stop = ROOT / "state/updates/user-stop.json"
    try:
        if user_stop.exists():
            user_stop.unlink(missing_ok=True)
    except OSError:
        pass

    # Clear restart failure status if present
    status_file = ROOT / "state/restart/status.json"
    try:
        if status_file.exists():
            status_file.unlink(missing_ok=True)
    except OSError:
        pass

    # Clear pause file and wake any paused workers so they can terminate cleanly
    pause_file = _state_dir() / "miner_pause.json"
    try:
        if pause_file.exists():
            pause_file.unlink(missing_ok=True)
    except OSError:
        pass
    import signal
    for pid in find_worker_pids():
        try:
            os.kill(pid, signal.SIGCONT)
        except OSError:
            pass

    unit = "nexus-v002-production.service"
    amd_unit = "nexus-rx570.service"
    systemctl = "/usr/bin/systemctl"

    nvidia_enabled = True
    amd_enabled = False
    try:
        from nexus_gpu_selection import configuration as gpu_selection
        sel = gpu_selection()
        nvidia_enabled = sel.get("nvidia_pom", True)
        amd_enabled = sel.get("amd_pom", False)
    except Exception:
        nvidia_enabled = True
        amd_enabled = False

    # Step 1: Explicitly stop services that should be stopped or restarted
    for u in (unit, amd_unit):
        try:
            subprocess.run([systemctl, "--user", "stop", u], capture_output=True, text=True, timeout=10)
        except (OSError, subprocess.SubprocessError):
            pass

    # Step 2: Ensure any lingering worker processes are fully terminated
    stop_deadline = time.monotonic() + 4.0
    known_workers = {'nexus-live-cuda-worker', 'nexus-live-vulkan-worker'}
    while time.monotonic() < stop_deadline:
        workers_found = False
        try:
            for entry in Path('/proc').iterdir():
                if not entry.name.isdecimal():
                    continue
                try:
                    if entry.stat().st_uid != os.getuid():
                        continue
                    exe = Path(os.readlink(entry / 'exe')).name.removesuffix(' (deleted)')
                    if exe in known_workers:
                        workers_found = True
                        break
                except (OSError, ValueError):
                    continue
        except OSError:
            pass
        if not workers_found:
            break
        if force and time.monotonic() > stop_deadline - 2.5:
            subprocess.run(["/usr/bin/pkill", "-9", "-u", str(os.getuid()), "-f", "nexus-live-cuda-worker"], capture_output=True)
            subprocess.run(["/usr/bin/pkill", "-9", "-u", str(os.getuid()), "-f", "nexus-live-vulkan-worker"], capture_output=True)
        time.sleep(0.2)

    # Step 3: Clear any orphan GPU lease locks
    try:
        lease_dir = ROOT / "state/gpu-devices"
        if lease_dir.is_dir():
            for lk in lease_dir.glob("lease-*.lock"):
                try:
                    lk_fd = os.open(lk, os.O_RDWR | os.O_NOFOLLOW)
                    try:
                        fcntl.flock(lk_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        fcntl.flock(lk_fd, fcntl.LOCK_UN)
                    finally:
                        os.close(lk_fd)
                except (OSError, BlockingIOError):
                    pass
    except OSError:
        pass

    # Step 4: Reset any failed state in systemd
    for u in (unit, amd_unit):
        try:
            subprocess.run([systemctl, "--user", "reset-failed", u], capture_output=True, timeout=5)
        except (OSError, subprocess.SubprocessError):
            pass

    # Step 5: Start enabled services
    nvidia_started = False
    if nvidia_enabled:
        try:
            res = subprocess.run([systemctl, "--user", "start", unit], capture_output=True, text=True, timeout=15)
            if res.returncode == 0:
                nvidia_started = True
        except (OSError, subprocess.SubprocessError):
            pass

    amd_started = False
    if amd_enabled:
        try:
            res = subprocess.run([systemctl, "--user", "start", amd_unit], capture_output=True, text=True, timeout=15)
            if res.returncode == 0:
                amd_started = True
        except (OSError, subprocess.SubprocessError):
            pass

    # Step 6: Write handoff confirmation
    try:
        handoff_file = ROOT / "state/gpu-devices/handoff.json"
        with open(handoff_file, "w") as f:
            json.dump({
                "status": "APPLIED",
                "selection": {"nvidia_pom": nvidia_enabled, "amd_pom": amd_enabled},
                "updated_unix": time.time()
            }, f)
    except Exception:
        pass

    # Step 7: Wait up to 5s for the primary enabled service to report active
    is_active = False
    check_unit = unit if nvidia_enabled else (amd_unit if amd_enabled else unit)
    poll_deadline = time.monotonic() + 5.0
    while time.monotonic() < poll_deadline:
        try:
            chk = subprocess.run([systemctl, "--user", "is-active", check_unit], capture_output=True, text=True, timeout=3)
            if chk.stdout.strip() == "active":
                is_active = True
                break
        except (OSError, subprocess.SubprocessError):
            pass
        time.sleep(0.3)

    return {
        "restarted": (nvidia_started if nvidia_enabled else True) and (amd_started if amd_enabled else True),
        "is_active": is_active,
        "unit": check_unit,
        "nvidia_active": nvidia_started,
        "amd_active": amd_started,
        "forced": force,
    }


def main(argv: list[str]) -> int:
    if len(argv) < 2 or argv[1] == "status":
        print(json.dumps(public_view()))
        return 0
    if argv[1] == "resolve":
        cfg = load()
        active_id = cfg.get("active_pool_id", "keryxpool")
        pools = cfg.get("pools", [])
        active_pool = next((p for p in pools if p["id"] == active_id), None)
        if not active_pool and pools:
            active_pool = pools[0]
        if cfg["mode"] == "pool":
            if not active_pool or not active_pool.get("url"):
                print(f"NEXUS_POOL=BLOCKED pool mode selected but active pool '{active_id}' is not configured", file=sys.stderr)
                return 13
        pool_url = active_pool.get("url", "") if active_pool else ""
        pool_worker = active_pool.get("worker", "") if active_pool else ""
        pool_pw = active_pool.get("password", "") if active_pool else ""
        backup_urls = [p["url"] for p in pools if p["id"] != active_id and p.get("configured") and p.get("url")]
        backup_str = ",".join(backup_urls)
        print(cfg["mode"])
        print(pool_url)
        print(pool_worker)
        print(pool_pw)
        print(backup_str)
        return 0
    if argv[1] == "pause-miner":
        res = pause_miner()
        print(json.dumps(res))
        return 0
    if argv[1] == "resume-miner":
        res = resume_miner()
        print(json.dumps(res))
        return 0
    if argv[1] == "toggle-pause":
        res = toggle_miner_pause()
        print(json.dumps(res))
        return 0
    if argv[1] == "restart-miner":
        res = restart_miner(force=True)
        print(json.dumps(res))
        return 0 if res.get("restarted") else 1
    if argv[1] == "test-connection" and len(argv) >= 3:
        res = test_connection(argv[2])
        print(json.dumps(res))
        return 0 if res.get("ok") else 1
    if argv[1] == "activate-pool" and len(argv) >= 3:
        view = activate_pool(argv[2], auto_restart=True)
        print(json.dumps(view))
        return 0
    if argv[1] == "set-mode" and len(argv) == 3:
        view = save({"mode": argv[2]}, auto_restart=True)
        print(json.dumps(view))
        return 0
    if argv[1] == "set-pool" and 3 <= len(argv) <= 5:
        updates = {"pool_url": argv[2]}
        if len(argv) >= 4:
            updates["pool_worker"] = argv[3]
        if len(argv) == 5:
            updates["pool_password"] = argv[4]
        view = save(updates, auto_restart=True)
        print(json.dumps(view))
        return 0
    print("usage: nexus_mining_mode.py [status|resolve|restart-miner|test-connection URL|activate-pool ID|set-mode solo|pool|set-pool URL [WORKER [PASSWORD]]]", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
