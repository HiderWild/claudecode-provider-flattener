#!/usr/bin/env python3
"""Header-rewrite proxy: Authorization: Bearer -> x-api-key."""
import http.server, urllib.request, urllib.error, ssl, sys

UPSTREAMS = {
    "deepseek": "https://api.deepseek.com/anthropic",
    "zai": "https://maas-coding-api.cn-huabei-1.xf-yun.com/anthropic",
}
PORT = 9999

class Proxy(http.server.BaseHTTPRequestHandler):
    def do_ANY(self):
        parts = self.path.lstrip("/").split("/", 1)
        provider = parts[0]
        if provider not in UPSTREAMS:
            self.send_error(404)
            return
        upstream_path = "/" + (parts[1] if len(parts) > 1 else "")
        upstream_url = UPSTREAMS[provider] + upstream_path
        cl = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(cl) if cl else b""
        req = urllib.request.Request(upstream_url, data=body, method=self.command)
        api_key = None
        for k, v in self.headers.items():
            kl = k.lower()
            if kl == "host":
                continue
            if kl == "authorization" and v.startswith("Bearer "):
                api_key = v[7:]
                continue
            req.add_header(k, v)
        if api_key:
            req.add_header("x-api-key", api_key)
        try:
            ctx = ssl.create_default_context()
            resp = urllib.request.urlopen(req, timeout=120, context=ctx)
            self.send_response(resp.status)
            for k, v in resp.getheaders():
                if k.lower() not in ("transfer-encoding", "connection"):
                    self.send_header(k, v)
            self.end_headers()
            while True:
                chunk = resp.read(8192)
                if not chunk:
                    break
                self.wfile.write(chunk)
        except urllib.error.HTTPError as e:
            self.send_response(e.code)
            self.end_headers()
            self.wfile.write(e.read())

    do_GET = do_POST = do_PUT = do_DELETE = do_PATCH = do_OPTIONS = do_HEAD = do_ANY
    def log_message(self, fmt, *args):
        pass

if __name__ == "__main__":
    http.server.HTTPServer(("127.0.0.1", PORT), Proxy).serve_forever()
