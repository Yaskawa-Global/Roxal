#!/usr/bin/env python3
"""Dev server that deliberately does NOT send COOP/COEP.

Use this to test the coi-sw.js fallback -- the path a plain S3 bucket takes.
`serve.py` sends the real headers, so it never exercises the service worker.

    ./serve-plain.py [port]      then open http://localhost:8001/

http://localhost counts as a secure context, so service workers register here
just as they would over https on S3.
"""
import http.server
import socketserver
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8001


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        '.wasm': 'application/wasm',
        '.js': 'text/javascript',
    }

    def end_headers(self):
        # No COOP/COEP on purpose. Only cache-busting, so repeated test runs
        # do not serve a stale service worker.
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))


socketserver.TCPServer.allow_reuse_address = True
with socketserver.TCPServer(("", PORT), Handler) as httpd:
    print(f"serving on http://localhost:{PORT}/  (NO isolation headers — tests coi-sw.js)")
    httpd.serve_forever()
