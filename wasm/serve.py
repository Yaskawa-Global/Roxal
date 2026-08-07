#!/usr/bin/env python3
"""Dev server that sets the cross-origin isolation headers the pthreads build needs.

    ./serve.py [port]        then open http://localhost:8000/

SharedArrayBuffer -- and therefore Emscripten pthreads -- is only available to a
cross-origin isolated document. That needs BOTH headers below. Without them the
page loads but every pthread_create fails, which surfaces as
"thread constructor failed: Not supported".

COEP require-corp (rather than credentialless) because credentialless is
Chromium-only and Safari has said they will not implement it. require-corp means
every cross-origin subresource must opt in via CORP or CORS -- fine here since
the app serves all its own assets.

In production these are set at the CDN: CloudFront Response Headers Policy with
the same two values. No server-side compute needed.
"""
import http.server
import socketserver
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8000


class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))


socketserver.TCPServer.allow_reuse_address = True
with socketserver.TCPServer(("", PORT), Handler) as httpd:
    print(f"serving {sys.path[0] or '.'} on http://localhost:{PORT}/  (COOP+COEP on)")
    httpd.serve_forever()
