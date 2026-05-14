#!/bin/bash
set -euo pipefail

binary="$1"
temp_root="$(mktemp -d)"
home_dir="$temp_root/home"
mkdir -p "$home_dir"
mkdir -p "$home_dir/.claude/model-gateway"

port="$((20000 + (RANDOM % 20000)))"
second_port="$((port + 1))"
if [[ "$second_port" -gt 45000 ]]; then
    second_port=45000
fi

cat >"$home_dir/.claude/model-gateway/config.json" <<EOF
{
    "port": $port,
    "bind": "127.0.0.1",
    "thread_pool_size": 4,
    "providers": [
        {
            "id": "local-test",
            "type": "openai",
            "name": "Local Test",
            "api_key": "local-test-key",
            "base_url": "http://127.0.0.1:1/v1",
            "models": ["mock-model"]
        }
    ],
    "models": {
        "local-test-model": {
            "id": "local-test-model",
            "provider": "local-test",
            "upstream_model": "mock-model",
            "protocol": "openai",
            "role": "test"
        }
    },
    "aliases": {
        "test": "local-test-model"
    }
}
EOF

first_log="$temp_root/first.log"
second_log="$temp_root/second.log"
runtime_log="$(cd "$(dirname "$binary")" && pwd)/model-gateway.log"
pid_file="$home_dir/.claude/model-gateway/model-gateway.pid"
first_pid=""
second_pid=""

cleanup() {
    if [[ -n "$second_pid" ]] && kill -0 "$second_pid" 2>/dev/null; then
        kill "$second_pid" 2>/dev/null || true
        wait "$second_pid" 2>/dev/null || true
    fi
    if [[ -n "$first_pid" ]] && kill -0 "$first_pid" 2>/dev/null; then
        kill "$first_pid" 2>/dev/null || true
        wait "$first_pid" 2>/dev/null || true
    fi
    rm -rf "$temp_root"
}
trap cleanup EXIT

HOME="$home_dir" USERPROFILE="$home_dir" "$binary" "$port" >"$first_log" 2>&1 &
first_pid="$!"

for _ in $(seq 1 50); do
    if curl -sf "http://127.0.0.1:$port/v1/models" >/dev/null 2>&1; then
        break
    fi
    sleep 0.2
done

if ! curl -sf "http://127.0.0.1:$port/v1/models" >/dev/null 2>&1; then
    echo "first instance did not become ready" >&2
    cat "$first_log" >&2 || true
    exit 1
fi

if [[ ! -f "$pid_file" ]]; then
    echo "expected runtime pid file to be created" >&2
    ls -la "$home_dir/.claude/model-gateway" >&2 || true
    exit 1
fi

pid_value="$(tr -d '[:space:]' < "$pid_file")"
if [[ "$pid_value" != "$first_pid" ]]; then
    echo "expected runtime pid file to match running process" >&2
    echo "pid file: $pid_value" >&2
    echo "actual : $first_pid" >&2
    exit 1
fi

models_json="$(curl -sf "http://127.0.0.1:$port/v1/models")"
if ! grep -Eq '"id"[[:space:]]*:[[:space:]]*"test"' <<<"$models_json"; then
    echo "expected alias 'test' in /v1/models response" >&2
    echo "$models_json" >&2
    exit 1
fi

monitor_json="$(curl -sf "http://127.0.0.1:$port/api/monitor")"
if ! grep -Eq '"listener"[[:space:]]*:[[:space:]]*"http://127.0.0.1:'"$port"'"' <<<"$monitor_json"; then
    echo "unexpected listener in /api/monitor response" >&2
    echo "$monitor_json" >&2
    exit 1
fi

if ! grep -Eq '"current_thread_pool_size"[[:space:]]*:[[:space:]]*[1-3]' <<<"$monitor_json"; then
    echo "expected fixed thread pool to lazily create fewer than the configured 4 workers under light load" >&2
    echo "$monitor_json" >&2
    exit 1
fi

count_tokens_json="$(curl -sf \
    -H 'Content-Type: application/json' \
    -d '{"model":"test","messages":[{"role":"user","content":"Count these tokens locally."}]}' \
    "http://127.0.0.1:$port/v1/messages/count_tokens")"
if ! grep -Eq '"input_tokens"[[:space:]]*:[[:space:]]*[1-9][0-9]*' <<<"$count_tokens_json"; then
    echo "count_tokens did not return a positive input_tokens value" >&2
    echo "$count_tokens_json" >&2
    exit 1
fi

set +e
HOME="$home_dir" USERPROFILE="$home_dir" "$binary" "$second_port" >"$second_log" 2>&1 &
second_pid="$!"
set -e

sleep 1

if kill -0 "$second_pid" 2>/dev/null; then
    echo "second instance is still running; expected startup failure" >&2
    cat "$second_log" >&2 || true
    exit 1
fi

if wait "$second_pid"; then
    echo "second instance exited successfully; expected failure" >&2
    cat "$second_log" >&2 || true
    exit 1
fi

if ! grep -q "another gateway instance is already running" "$second_log" 2>/dev/null && \
   ! grep -q "another gateway instance is already running" "$runtime_log" 2>/dev/null; then
    echo "second instance did not fail with the expected lock error" >&2
    cat "$second_log" >&2 || true
    cat "$runtime_log" >&2 || true
    exit 1
fi