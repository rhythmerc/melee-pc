#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Phone test server: built browser target + overlay files + Range-served disc."""
import http.server, os, re, sys
OUT, OVERLAY, ISO, PORT = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])

class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=OUT, **k)
    def translate_path(self, path):
        name = path.split('?')[0].lstrip('/')
        if name and os.path.exists(os.path.join(OVERLAY, name)):
            return os.path.join(OVERLAY, name)
        return super().translate_path(path)
    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cross-Origin-Resource-Policy', 'same-origin')
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()
    def do_POST(self):
        if self.path.split('?')[0] != '/log':
            self.send_error(404); return
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        with open(os.path.join(OVERLAY, '..', 'phone.log'), 'ab') as f:
            f.write(body)
        self.send_response(204); self.end_headers()
    def do_HEAD(self):
        if self.path.split('?')[0] == '/disc.iso':
            self.send_response(200)
            self.send_header('Content-Length', str(os.path.getsize(ISO)))
            self.send_header('Accept-Ranges', 'bytes')
            self.send_header('Content-Type', 'application/octet-stream')
            self.end_headers(); return
        super().do_HEAD()
    def do_GET(self):
        if self.path.split('?')[0] != '/disc.iso':
            return super().do_GET()
        size = os.path.getsize(ISO)
        m = re.match(r'bytes=(\d+)-(\d*)', self.headers.get('Range', ''))
        if not m:
            self.send_error(416); return
        a = int(m.group(1)); b = min(int(m.group(2) or size - 1), size - 1)
        if a > b:
            self.send_error(416); return
        self.send_response(206)
        self.send_header('Content-Range', f'bytes {a}-{b}/{size}')
        self.send_header('Content-Length', str(b - a + 1))
        self.send_header('Content-Type', 'application/octet-stream')
        self.end_headers()
        with open(ISO, 'rb') as f:
            f.seek(a); self.wfile.write(f.read(b - a + 1))
    def log_message(self, *a):
        pass

http.server.ThreadingHTTPServer(('127.0.0.1', PORT), H).serve_forever()
