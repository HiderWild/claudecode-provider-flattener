#!/bin/bash
set -euo pipefail

binary="$1"
temp_root="$(mktemp -d)"
home_dir="$temp_root/home"
mkdir -p "$home_dir/.claude/model-gateway"

gateway_port="$((20000 + (RANDOM % 15000)))"
upstream_port="$((gateway_port + 1))"

cat >"$home_dir/.claude/model-gateway/config.json" <<EOF
{
  "port": $gateway_port,
  "bind": "127.0.0.1",
  "thread_pool_size": 1,
  "providers": [
    {
      "id": "local-test",
      "type": "anthropic",
      "name": "Local Test",
      "api_key": "local-test-key",
      "base_url": "http://127.0.0.1:$upstream_port",
      "models": ["slow-start", "stream-start"]
    }
  ],
  "models": {
    "slow-start": {
      "id": "slow-start",
      "provider": "local-test",
      "upstream_model": "slow-start",
      "protocol": "anthropic",
      "role": "test"
    },
    "stream-start": {
      "id": "stream-start",
      "provider": "local-test",
      "upstream_model": "stream-start",
      "protocol": "anthropic",
      "role": "test"
    }
  },
  "aliases": {
    "slow": "slow-start",
    "stream": "stream-start"
  }
}
EOF

upstream_script="$temp_root/upstream.py"
cat >"$upstream_script" <<'PYEOF'
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def write_chunk(handler, payload: bytes) -> None:
    handler.wfile.write(f"{len(payload):X}\r\n".encode("ascii"))
    handler.wfile.write(payload)
    handler.wfile.write(b"\r\n")
    handler.wfile.flush()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_POST(self):
        if self.path != "/v1/messages":
            self.send_error(404)
            return

        length = int(self.headers.get("Content-Length", "0"))
        payload = self.rfile.read(length) if length else b"{}"
        body = json.loads(payload.decode("utf-8"))
        model = body.get("model", "")

        if model == "slow-start":
            time.sleep(5)
            response = json.dumps({
                "type": "message",
                "id": "msg-test",
                "role": "assistant",
                "model": model,
                "content": [{"type": "text", "text": "late"}],
                "stop_reason": "end_turn",
                "usage": {"input_tokens": 1, "output_tokens": 1}
            }).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(response)))
            self.end_headers()
            try:
                self.wfile.write(response)
                self.wfile.flush()
            except BrokenPipeError:
                pass
            return

        if model == "stream-start":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()

            event = (
                'event: message_start\n'
                'data: {"type":"message_start","message":{"id":"msg-test","type":"message","role":"assistant","model":"stream-start","content":[],"stop_reason":null,"stop_sequence":null,"usage":{"input_tokens":1,"output_tokens":0}}}\n\n'
            ).encode("utf-8")
            try:
                write_chunk(self, event)
                time.sleep(5)
            except BrokenPipeError:
                pass
            return

        self.send_error(400, "unexpected model")

    def log_message(self, fmt, *args):
        return


if __name__ == "__main__":
    port = int(sys.argv[1])
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.serve_forever()
PYEOF

gateway_log="$temp_root/gateway.log"
upstream_log="$temp_root/upstream.log"
gateway_pid=""
upstream_pid=""

cleanup() {
    if [[ -n "$gateway_pid" ]] && kill -0 "$gateway_pid" 2>/dev/null; then
        kill "$gateway_pid" 2>/dev/null || true
        wait "$gateway_pid" 2>/dev/null || true
    fi
    if [[ -n "$upstream_pid" ]] && kill -0 "$upstream_pid" 2>/dev/null; then
        kill "$upstream_pid" 2>/dev/null || true
        wait "$upstream_pid" 2>/dev/null || true
    fi
    rm -rf "$temp_root"
}
trap cleanup EXIT

python3 "$upstream_script" "$upstream_port" >"$upstream_log" 2>&1 &
upstream_pid="$!"

HOME="$home_dir" USERPROFILE="$home_dir" "$binary" >"$gateway_log" 2>&1 &
gateway_pid="$!"

for _ in $(seq 1 50); do
    if curl -sf "http://127.0.0.1:$gateway_port/v1/models" >/dev/null 2>&1; then
        break
    fi
    sleep 0.2
done

if ! curl -sf "http://127.0.0.1:$gateway_port/v1/models" >/dev/null 2>&1; then
    echo "gateway did not become ready" >&2
    cat "$gateway_log" >&2 || true
    exit 1
fi

slow_body='{"model":"slow","stream":true,"max_tokens":16,"messages":[{"role":"user","content":"hello"}]}'
set +e
curl -sS --max-time 1 \
    -H 'Content-Type: application/json' \
    -d "$slow_body" \
    "http://127.0.0.1:$gateway_port/v1/messages" \
    >"$temp_root/slow.out" 2>"$temp_root/slow.err"
slow_status="$?"
set -e

if [[ "$slow_status" -eq 0 ]]; then
    echo "slow-start request unexpectedly completed" >&2
    cat "$temp_root/slow.out" >&2 || true
    exit 1
fi

sleep 1

if ! curl -sf --max-time 2 "http://127.0.0.1:$gateway_port/v1/models" >/dev/null 2>&1; then
    echo "gateway did not recover worker capacity after downstream disconnect" >&2
    cat "$gateway_log" >&2 || true
    cat "$temp_root/slow.err" >&2 || true
    exit 1
fi

stream_headers="$temp_root/stream.headers"
stream_body_file="$temp_root/stream.body"
stream_body='{"model":"stream","stream":true,"max_tokens":16,"messages":[{"role":"user","content":"hello"}]}'
set +e
curl -sS -N --max-time 2 \
    -D "$stream_headers" \
    -o "$stream_body_file" \
    -H 'Content-Type: application/json' \
    -d "$stream_body" \
    "http://127.0.0.1:$gateway_port/v1/messages"
stream_status="$?"
set -e

if [[ "$stream_status" -ne 0 && "$stream_status" -ne 28 ]]; then
    echo "stream request failed unexpectedly with status $stream_status" >&2
    cat "$stream_body_file" >&2 || true
    exit 1
fi

if ! grep -q '^HTTP/1.1 200' "$stream_headers"; then
    echo "expected downstream stream to start with HTTP 200" >&2
    cat "$stream_headers" >&2 || true
    cat "$stream_body_file" >&2 || true
    exit 1
fi

if ! grep -Eq 'data: .*"type":"message_start"' "$stream_body_file"; then
    echo "expected downstream stream to forward the first SSE event promptly" >&2
    cat "$stream_headers" >&2 || true
    cat "$stream_body_file" >&2 || true
    exit 1
fi

monitor_ok="0"
for _ in $(seq 1 10); do
    monitor_json="$(curl -sf --max-time 1 "http://127.0.0.1:$gateway_port/api/monitor" || true)"
    if grep -Eq '"active_streams"[[:space:]]*:[[:space:]]*0' <<<"$monitor_json"; then
        monitor_ok="1"
        break
    fi
    sleep 0.2
done

if [[ "$monitor_ok" != "1" ]]; then
    echo "expected active_streams to drain quickly after downstream disconnect" >&2
    curl -sf --max-time 2 "http://127.0.0.1:$gateway_port/api/monitor" >&2 || true
    cat "$gateway_log" >&2 || true
    exit 1
fi

upstream_conn_ok="0"
for _ in $(seq 1 10); do
    if ! lsof -nP -a -p "$gateway_pid" -iTCP:"$upstream_port" 2>/dev/null | grep -q ESTABLISHED; then
        upstream_conn_ok="1"
        break
    fi
    sleep 0.2
done

if [[ "$upstream_conn_ok" != "1" ]]; then
    echo "expected upstream TCP connection to close quickly after downstream disconnect" >&2
    lsof -nP -a -p "$gateway_pid" -iTCP:"$upstream_port" >&2 || true
    cat "$gateway_log" >&2 || true
    exit 1
fi