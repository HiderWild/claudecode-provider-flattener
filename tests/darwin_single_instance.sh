#!/bin/bash
set -euo pipefail

binary="$1"
temp_root="$(mktemp -d)"
home_dir="$temp_root/home"
mkdir -p "$home_dir"

port="$((20000 + (RANDOM % 20000)))"
second_port="$((port + 1))"
if [[ "$second_port" -gt 45000 ]]; then
    second_port=45000
fi

first_log="$temp_root/first.log"
second_log="$temp_root/second.log"
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

HOME="$home_dir" USERPROFILE="$home_dir" "$binary" --show "$port" >"$first_log" 2>&1 &
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

set +e
HOME="$home_dir" USERPROFILE="$home_dir" "$binary" --show "$second_port" >"$second_log" 2>&1 &
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

if ! grep -q "another gateway instance is already running" "$second_log"; then
    echo "second instance did not fail with the expected lock error" >&2
    cat "$second_log" >&2 || true
    exit 1
fi