#!/usr/bin/env python3
"""UnixFS CIDv0, corrupt responses and connection reuse; no GPU or mainnet."""
import hashlib
import json
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse


def varint(n):
    result = bytearray()
    while n >= 128:
        result.append((n & 127) | 128)
        n >>= 7
    return bytes(result) + bytes([n])


def b58(data):
    alphabet = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
    value, result = int.from_bytes(data, 'big'), ''
    while value:
        value, digit = divmod(value, 58)
        result = alphabet[digit] + result
    return result


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def do_POST(self):
        parsed = urlparse(self.path)
        raw = self.rfile.read(int(self.headers.get('Content-Length', '0')))
        self.server.peers.add(self.client_address)
        if parsed.path.endswith('/add'):
            boundary = self.headers['Content-Type'].split('boundary=', 1)[1].strip('"')
            part = next(p for p in raw.split(b'--' + boundary.encode()) if b'name="file"' in p)
            content = part.split(b'\r\n\r\n', 1)[1].rsplit(b'\r\n', 1)[0]
            unixfs = b'\x08\x02\x12' + varint(len(content)) + content + b'\x18' + varint(len(content))
            root = b'\x0a' + varint(len(unixfs)) + unixfs
            cid = b58(b'\x12\x20' + hashlib.sha256(root).digest())
            self.server.blobs[cid] = content, root
            response = json.dumps({'Hash': cid}).encode()
        else:
            cid = parse_qs(parsed.query).get('arg', [''])[0]
            if cid not in self.server.blobs:
                self.send_error(404)
                return
            content, root = self.server.blobs[cid]
            if parsed.path.endswith('/cat'):
                response = b'corrupt content' if self.server.mode == 'readback' else content
            elif parsed.path.endswith('/block/get'):
                response = b'corrupt root' if self.server.mode == 'root' else root
                if self.server.mode == 'oversized':
                    response = b'x' * (4 * 1024 * 1024 + 1)
            else:
                self.send_error(404)
                return
        self.send_response(200)
        self.send_header('Content-Length', str(len(response)))
        self.end_headers()
        try:
            self.wfile.write(response)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *_):
        pass


def main():
    servers, threads = [], []
    try:
        for mode in ('normal', 'readback', 'root', 'oversized'):
            server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
            server.blobs, server.peers, server.mode = {}, set(), mode
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            servers.append(server)
            threads.append(thread)
        urls = [f'http://127.0.0.1:{s.server_port}' for s in servers]
        result = subprocess.run([sys.argv[1], *urls], capture_output=True, text=True, timeout=30)
        print(result.stdout, end='')
        if result.returncode:
            raise RuntimeError(result.stderr)
        if len(servers[0].peers) != 1:
            raise RuntimeError(f'HTTP keep-alive not reused: {len(servers[0].peers)} connections')
        print('IPFS_UNIXFS_MOCK=PASS keepalive_connections=1 publications=3 corrupt_readback_rejected=true corrupt_root_rejected=true bounded_response=true')
    finally:
        for server in servers:
            server.shutdown()
            server.server_close()
        for thread in threads:
            thread.join(timeout=2)


if __name__ == '__main__':
    main()
