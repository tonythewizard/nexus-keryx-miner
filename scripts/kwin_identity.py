"""Bounded, opt-in correlation for KWin EACCES; never proof of child exe inode."""
import errno
import hashlib
import json
import os
import pathlib
import re
import subprocess

KWIN = "/usr/bin/kwin_wayland"
WRAPPER = "/usr/bin/kwin_wayland_wrapper"
METHOD = "systemd-cgroup-parent-cmdline-fallback"


def read_process(pid):
    base = pathlib.Path(f"/proc/{int(pid)}")
    stat = (base / "stat").read_text()
    tail = stat[stat.rfind(")") + 2:].split()
    status = dict(line.split(":", 1) for line in (base / "status").read_text().splitlines()
                  if ":" in line)
    row = dict(pid=int(pid), starttime=tail[19], state=tail[0], ppid=int(tail[1]),
               uids=list(map(int, status["Uid"].split())),
               gids=list(map(int, status["Gid"].split())),
               comm=(base / "comm").read_text().strip(),
               cmdline=[s.decode("utf-8", errors="strict") for s in
                        (base / "cmdline").read_bytes().split(b"\0") if s],
               cgroup=(base / "cgroup").read_text().strip(),
               exe=None, exe_errno=None)
    if int(status["PPid"]) != row["ppid"] or status["State"].strip()[0] in "ZX":
        raise RuntimeError("KWin procfs parent/state contradiction")
    try:
        row["exe"] = os.readlink(base / "exe")
    except OSError as exc:
        # Disappearance, EPERM, I/O errors are not the documented EACCES case.
        if exc.errno != errno.EACCES:
            raise
        row["exe_errno"] = exc.errno
    end = (base / "stat").read_text()
    end_tail = end[end.rfind(")") + 2:].split()
    if end_tail[19] != row["starttime"] or int(end_tail[1]) != row["ppid"]:
        raise RuntimeError("KWin PID/starttime/parent changed during read")
    if end_tail[0] in "ZX":
        raise RuntimeError("KWin process terminated during read")
    return row


def read_service():
    p = subprocess.run(["systemctl", "--user", "show", "plasma-kwin_wayland.service",
                        "-p", "ActiveState", "-p", "MainPID", "-p", "ControlGroup",
                        "-p", "ExecStart"], capture_output=True, text=True, timeout=3)
    if p.returncode:
        raise RuntimeError("KWin systemd identity unavailable: " + p.stderr.strip())
    return dict(line.split("=", 1) for line in p.stdout.splitlines() if "=" in line)


def stable(row):
    return {k: v for k, v in row.items() if k != "state"}


def validate(evidence, observed):
    """Pure verification of facts; called at collection AND policy decision."""
    try:
        uid = os.geteuid()
        cgroup = f"/user.slice/user-{uid}.slice/user@{uid}.service/session.slice/plasma-kwin_wayland.service"
        child, parent = evidence["child_before"], evidence["parent_before"]
        service = evidence["service_before"]
        for role in ("child", "parent"):
            before, after = evidence[role + "_before"], evidence[role + "_after"]
            if stable(before) != stable(after) or before["state"] in "ZX" or after["state"] in "ZX":
                raise ValueError(role + " identity changed or exited")
            if before["uids"] != [uid] * 4 or before["gids"] != [os.getegid()] * 4:
                raise ValueError(role + " UID/GID mismatch")
            if before["cgroup"] != "0::" + cgroup or int(before["starttime"]) <= 0:
                raise ValueError(role + " cgroup/starttime mismatch")
        if service != evidence["service_after"]:
            raise ValueError("systemd identity changed")
        if (service["ActiveState"] != "active" or service["ControlGroup"] != cgroup or
                int(service["MainPID"]) != parent["pid"] or child["ppid"] != parent["pid"]):
            raise ValueError("systemd MainPID/state/cgroup or parent mismatch")
        if not re.fullmatch(r"\{ path=/usr/bin/kwin_wayland_wrapper ; argv\[\]=/usr/bin/kwin_wayland_wrapper(?: [^;]*)? ; .* \}",
                            service["ExecStart"]):
            raise ValueError("unexpected systemd ExecStart")
        if (parent["exe"] != WRAPPER or parent["exe_errno"] is not None or
                parent["comm"] != "kwin_wayland_wr" or not parent["cmdline"] or
                parent["cmdline"][0] != WRAPPER):
            raise ValueError("wrapper real executable/comm/cmdline mismatch")
        if child["comm"] != "kwin_wayland" or not child["cmdline"] or child["cmdline"][0] != KWIN:
            raise ValueError("child comm/cmdline mismatch")
        fallback = child["exe_errno"] == errno.EACCES and child["exe"] is None
        if fallback:
            if evidence["opt_in"] is not True or observed["identity_method"] != METHOD:
                raise ValueError("EACCES fallback not explicitly enabled")
        elif child["exe"] != KWIN or child["exe_errno"] is not None or observed["identity_method"] != "proc-exe":
            raise ValueError("child executable mismatch or unsupported errno")
        expected = dict(pid=child["pid"], starttime=child["starttime"], uid=uid,
                        comm=child["comm"], exe=KWIN, cmdline=child["cmdline"],
                        ppid=child["ppid"], cgroup=cgroup)
        if any(observed[k] != value for k, value in expected.items()):
            raise ValueError("procfs observation mismatch")
        # State (S/R) is dynamic; all identity evidence is bound across the session.
        token = hashlib.sha256(json.dumps(
            dict(child=stable(child), parent=stable(parent), service=service),
            sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        return token
    except (KeyError, TypeError, ValueError) as exc:
        raise RuntimeError("KWin correlated identity BLOCKED: " + str(exc)) from exc


def collect(observed):
    child = read_process(observed["pid"])
    parent = read_process(child["ppid"])
    service = read_service()
    evidence = dict(child_before=child, parent_before=parent, service_before=service,
                    child_after=read_process(child["pid"]),
                    parent_after=read_process(parent["pid"]), service_after=read_service(),
                    opt_in=os.environ.get("KERYX_ALLOW_KWIN_PROCFS_FALLBACK") == "1")
    return evidence, validate(evidence, observed)
