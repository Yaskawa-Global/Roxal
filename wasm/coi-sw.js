// Cross-origin isolation via service worker.
//
// COOP/COEP are HTTP response headers with no <meta http-equiv> equivalent, and
// plain S3 cannot emit arbitrary headers. A service worker can: it intercepts
// its own origin's responses and re-issues them with the headers attached, which
// is enough for the browser to grant cross-origin isolation (and therefore
// SharedArrayBuffer, and therefore pthreads).
//
// This is a workaround for static hosting. The real fix is a CloudFront
// Response Headers Policy -- see README. Limits:
//   * needs a secure context (https, or localhost)
//   * the FIRST visit registers the worker and reloads once
//   * unavailable where service workers are blocked (some private modes)

if (typeof window === 'undefined') {
    // ---- service worker side ----
    self.addEventListener('install', () => self.skipWaiting());
    self.addEventListener('activate', e => e.waitUntil(self.clients.claim()));

    self.addEventListener('fetch', event => {
        const req = event.request;
        if (req.cache === 'only-if-cached' && req.mode !== 'same-origin') return;

        event.respondWith(
            fetch(req)
                .then(res => {
                    if (res.status === 0) return res;   // opaque; leave alone
                    const headers = new Headers(res.headers);
                    headers.set('Cross-Origin-Opener-Policy', 'same-origin');
                    headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
                    headers.set('Cross-Origin-Resource-Policy', 'same-origin');
                    return new Response(res.body, {
                        status: res.status,
                        statusText: res.statusText,
                        headers,
                    });
                })
                .catch(err => { console.error('coi-sw:', err); throw err; })
        );
    });
} else {
    // ---- page side ----
    if (window.crossOriginIsolated) {
        // Already isolated (real headers present) -- nothing to do.
    } else if (!window.isSecureContext) {
        console.error('coi-sw: not a secure context; serve over https or localhost');
    } else if (!navigator.serviceWorker) {
        console.error('coi-sw: service workers unavailable');
    } else {
        navigator.serviceWorker.register(window.document.currentScript.src).then(
            reg => {
                // A fresh registration only takes effect for the NEXT navigation,
                // so reload once as soon as it controls this page.
                reg.addEventListener('updatefound', () => window.location.reload());
                if (reg.active && !navigator.serviceWorker.controller) window.location.reload();
            },
            err => console.error('coi-sw: registration failed', err)
        );
    }
}
