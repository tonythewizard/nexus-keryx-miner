#!/usr/bin/env python3
"""Local API client: credentials are read from a private file, never argv/stdout."""
import os
import argparse
import json
from pathlib import Path
from urllib.request import Request, build_opener, ProxyHandler
from urllib.error import HTTPError, URLError


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--method', choices=('GET', 'POST'), default='GET')
    p.add_argument('--json-file', type=Path)
    p.add_argument('route')
    a = p.parse_args()
    if not a.route.startswith('/v1/') or '?' in a.route or '\r' in a.route or '\n' in a.route: p.error('use an API route without a query')
    keys = Path(os.environ.get("NEXUS_DATA_DIR", str(Path.home() / ".local/share/nexus-keryx-miner"))) / "agent-api/client-keys.json"
    if keys.is_symlink() or keys.stat().st_mode & 0o077: p.error('unsafe key file')
    token = json.loads(keys.read_text())['agent']
    body = None
    if a.json_file:
        if a.json_file.resolve().is_relative_to(Path('/tmp')): p.error('persistent request file required')
        if a.json_file.stat().st_size > 65536: p.error('request too large')
        body = json.dumps(json.loads(a.json_file.read_text())).encode()
    req = Request('http://127.0.0.1:3340' + a.route, data=body, method=a.method,
                  headers={'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json'})
    try:
        with build_opener(ProxyHandler({})).open(req, timeout=10) as result:
            print(json.dumps(json.load(result), indent=2))
    except HTTPError as e:
        print(json.dumps({'http_status': e.code, 'result': json.loads(e.read())})); return 1
    except URLError:
        print(json.dumps({'error': 'API server OFF or starting; use T in DEVELOPMENT'})); return 1
    return 0


if __name__ == '__main__': raise SystemExit(main())
