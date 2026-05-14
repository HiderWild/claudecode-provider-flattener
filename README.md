# claudecode-provider-flattener

`claudecode-provider-flattener` is a Windows-first local gateway that makes multiple upstream providers look like one Anthropic-compatible provider for Claude Code.

Its job is simple: keep Claude Code pointed at one stable local endpoint, and let `/model` switch between different upstream vendors and upstream model IDs.

## Why this exists

Claude Code works best when the local endpoint does not change. Real usage is messier:

- one model might be served by an Anthropic-compatible gateway
- another might only exist behind an OpenAI-compatible backend
- a third route may need custom thinking defaults or capability declarations

This gateway flattens those differences into one local Anthropic-compatible surface so Claude Code keeps working while you swap the real backend behind each alias.

## Core capabilities

- Anthropic-compatible local API for Claude Code
- Multi-provider routing across Anthropic-style and OpenAI-style backends
- Alias-based switching through `/model <alias>`
- Full-duplex streaming with downstream disconnect handling and upstream cancellation
- `/v1/messages/count_tokens` support with upstream passthrough and local fallback estimation
- Per-session monitoring keyed by `X-Claude-Code-Session-Id`
- Local control panel for providers, model routes, aliases, capabilities, request overrides, and runtime telemetry
- Single-instance runtime with in-process worker threads on macOS and Windows

## Quick start

### 1. Build

macOS:

```bash
cmake -B build-macos -S .
cmake --build build-macos --clean-first -j"$(sysctl -n hw.ncpu)"
install -m 755 build-macos/model-gateway ~/.claude/model-gateway/bin/model-gateway
```

Windows (MinGW):

```powershell
cmake -B build -S . -G "MinGW Makefiles"
cmake --build build
ctest --test-dir build --output-on-failure
```

Windows (Visual Studio / GitHub Actions style):

```powershell
cmake -B build-msvc -S . -A x64
cmake --build build-msvc --config Release
ctest --test-dir build-msvc -C Release --output-on-failure
```

Or use the helper script:

```bash
./build.sh
```

macOS local validation:

```bash
ctest --test-dir build-macos --output-on-failure
```

### 2. Run the gateway

macOS and Windows now run as single foreground processes. If you want background supervision, use an external supervisor.

macOS:

```bash
./build-macos/model-gateway
```

Detached macOS example:

```bash
nohup ./build-macos/model-gateway >/tmp/model-gateway.log 2>&1 &
```

Verbose startup example:

```bash
./build-macos/model-gateway --show
```

Install a per-user `launchd` agent on macOS:

```bash
bash darwin/install-launchd.sh install
```

Check `launchd` status:

```bash
bash darwin/install-launchd.sh status
```

Windows:

```powershell
./build/model-gateway.exe
```

Windows foreground helper:

```powershell
./start.bat
```

Windows startup options:

```powershell
./build/model-gateway.exe --show
./build/model-gateway.exe --show 9457
```

For background restart supervision on Windows, use Task Scheduler, NSSM, or another external service manager.

The installed runtime uses the user-scoped Claude directory:

- binary: `~/.claude/model-gateway/bin`
- config: `~/.claude/model-gateway/config.json`

On macOS and Windows, a second gateway instance now fails fast instead of sharing the same port or process role. Concurrency stays inside one process via the configured thread pool.

## CI and build environment

GitHub Actions currently uses two release workflows:

- Windows release: [release-windows.yml](.github/workflows/release-windows.yml) runs a matrix on `windows-2025-vs2026` and `windows-11-arm`, configures CMake with the Visual Studio generator (`-A x64` or `-A ARM64`), builds `Release`, runs `ctest -C Release`, then publishes versioned `.exe` and `.zip` assets for both Windows x64 and Windows ARM64.
- macOS release: [release-macos-arm64.yml](.github/workflows/release-macos-arm64.yml) runs a matrix on `macos-14` and `macos-15-intel`, builds with Apple Clang + CMake, runs `ctest`, then publishes versioned binaries and `.tar.gz` packages for both macOS ARM64 and macOS Intel.

Both workflows trigger on `v*` tags and can also be run manually through `workflow_dispatch`.

## Local build stack

The local build and validation flow is intentionally small and conventional:

- Language/runtime: C++17
- Build system: CMake 3.16+
- HTTP/runtime libs: `cpp-httplib`, `nlohmann/json`
- Windows platform APIs: WinHTTP, Winsock (`ws2_32`)
- macOS platform deps: OpenSSL, `Security`, `CoreFoundation`, `CFNetwork`
- Validation tooling: `ctest`, PowerShell on Windows, Bash + `curl` on macOS

For local smoke tests, the repo now includes platform-specific startup validation that:

- boots the compiled gateway under a temporary HOME/USERPROFILE
- checks `/v1/models` and `/api/monitor`
- verifies `/v1/messages/count_tokens` without needing real upstream credentials
- confirms that a second instance fails with the expected single-instance lock error

### 3. Point Claude Code at the gateway

For Claude Code, treat the gateway as your Anthropic endpoint and export the Anthropic environment variables before launching Claude Code:

```powershell
$env:ANTHROPIC_BASE_URL = "http://127.0.0.1:9457"
$env:ANTHROPIC_AUTH_TOKEN = "local-gateway"
```

`ANTHROPIC_AUTH_TOKEN` only needs to be non-empty for Claude Code to initialize. The gateway itself authenticates to upstream providers using the keys stored in its own config.

Then switch models inside Claude Code with local aliases:

```text
/model haiku
/model sonnet
/model opus
```

Those names are gateway aliases, not necessarily vendor model IDs.

## Configuration model

The gateway uses three routing layers:

- `providers`: upstream endpoints and credentials
- `models`: concrete gateway routes, each binding one provider to one upstream model
- `aliases`: Claude Code-facing short names selected through `/model`

Example:

```json
{
  "port": 9457,
  "bind": "127.0.0.1",
  "thread_pool_size": 8,
  "admin_token": "optional-control-plane-token",
  "providers": [
    {
      "id": "zai",
      "type": "anthropic",
      "name": "ZAI Astron",
      "api_key": "<your-api-key>",
      "base_url": "https://your-provider.example",
      "models": ["astron-code-latest"]
    },
    {
      "id": "deepseek",
      "type": "openai",
      "name": "DeepSeek",
      "api_key": "<your-api-key>",
      "base_url": "https://your-provider.example/v1",
      "models": ["deepseek-v4-flash", "deepseek-v4-pro-thinking"]
    }
  ],
  "models": {
    "astron-code-latest": {
      "id": "astron-code-latest",
      "provider": "zai",
      "upstream_model": "astron-code-latest",
      "protocol": "anthropic",
      "capabilities": {
        "tools": true,
        "streaming": true,
        "thinking": true,
        "count_tokens": true
      },
      "request_overrides": {
        "thinking": {"type": "enabled"},
        "effort": "medium"
      }
    }
  },
  "aliases": {
    "sonnet": "astron-code-latest",
    "haiku": "deepseek-v4-flash"
  }
}
```

Notes:

- `thread_pool_size: 0` means unbounded mode
- `admin_token` protects `/api/config`, `/api/config/test`, and `/api/monitor`
- provider API keys and the admin token are masked in the control panel and preserved on save when the masked value is left unchanged

## Claude Code compatibility matrix

| Surface | Status | Notes |
| --- | --- | --- |
| `POST /v1/messages` | Yes | Anthropic-compatible request/response surface |
| Streaming SSE | Yes | Downstream disconnect detection and upstream cancellation included |
| `POST /v1/messages/count_tokens` | Yes | Pass-through when available, local estimation fallback otherwise |
| `GET /v1/models` | Yes | Alias and model metadata listing |
| `anthropic-version` passthrough | Yes | Forwarded from Claude Code when present |
| `anthropic-beta` passthrough | Yes | Forwarded to Anthropic-compatible upstreams |
| `X-Claude-Code-Session-Id` tracking | Yes | Aggregated into the monitor dashboard |
| Tool blocks and tool results | Yes | Anthropic ↔ OpenAI translation supported |
| Thinking flags | Yes | Per-model override support via `request_overrides` |

## Control panel

The web control panel lives at:

```text
http://127.0.0.1:9457/
```

It supports:

- editing providers, model routes, aliases, bind address, thread pool size, and optional admin token
- editing model capabilities and request overrides such as thinking and effort
- masked secret handling for provider API keys and the control-plane token
- provider connectivity testing
- runtime counters, active stream state, buffered byte pressure, and thread-pool view
- session-level aggregation including request counts, count-token calls, active streams, and errors

## Runtime APIs

- `GET /` — control panel
- `GET /api/config` — current runtime config
- `POST /api/config` — save config
- `GET /api/config/test?provider_id=...` — test one provider
- `GET /api/monitor` — runtime and session monitor data
- `GET /v1/models` — list model aliases and metadata
- `POST /v1/messages` — Anthropic-compatible messages endpoint
- `POST /v1/messages/count_tokens` — token counting endpoint

## Security notes

This project is a local gateway, not a hardened internet-facing reverse proxy.

- Upstream API keys are stored in the user config file because the gateway must use them at runtime.
- The control panel masks secrets on read and preserves them correctly on save.
- Runtime APIs redact local absolute paths in monitor output.
- You can protect the control plane with `admin_token`, but the data plane remains a local trusted interface.
- There is no built-in TLS termination, user management, or encrypted secret store.

If you need stronger guarantees, put this gateway behind a local-only firewall policy or an additional authenticated reverse proxy.

## Comparison

Compared with directly pointing Claude Code at one provider:

- you keep one stable local endpoint
- `/model` becomes the switch point instead of editing provider config repeatedly
- Anthropic-compatible and OpenAI-compatible upstreams can coexist behind the same Claude Code session

Compared with a generic API proxy:

- this project is intentionally Claude Code-specific
- the compatibility work is centered on Anthropic message semantics, Claude session headers, and `/model` alias routing
- the control panel is built around provider flattening rather than general API gateway features

## Releases

GitHub Actions includes release workflows for Windows x64, Windows ARM64, macOS ARM64, and macOS Intel. Tag a release with a `v*` tag such as `v1.0.0` and the workflows will:

- build release binaries for all four targets
- package each binary with the README and platform-specific launcher helpers
- upload the generated archives to the GitHub Release page

## Roadmap

- signed Windows release artifacts
- packaged install helpers beyond the current binary drop-in flow
- per-provider health scoring and smarter failover
- stronger secret-storage options
- broader cross-platform packaging