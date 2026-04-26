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
- Background launch by default, foreground mode with `--show`, and restart supervision with `--daemon`

## Quick start

### 1. Build

```powershell
cmake -B build -S . -G "MinGW Makefiles"
cmake --build build
```

Or use the helper script:

```bash
./build.sh
```

### 2. Run the gateway

```powershell
./build/model-gateway.exe
```

Useful modes:

- default: starts in the background and returns to the shell
- `--show`: keep the gateway attached to the current console
- `--daemon`: run a supervisor that restarts the worker after unexpected exits

Examples:

```powershell
./build/model-gateway.exe --show
./build/model-gateway.exe --daemon
./build/model-gateway.exe --show 9457
```

The installed runtime uses the user-scoped Claude directory:

- binary: `~/.claude/model-gateway/bin`
- config: `~/.claude/model-gateway/config.json`

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

GitHub Actions includes a Windows x64 release workflow. Tag a release with a `v*` tag such as `v1.0.0` and the workflow will:

- build `model-gateway.exe` in Release mode
- package the executable with the README and launcher helpers
- upload the Windows x64 archive to the GitHub Release page

## Roadmap

- signed Windows release artifacts
- packaged install helpers beyond the current binary drop-in flow
- per-provider health scoring and smarter failover
- stronger secret-storage options
- broader cross-platform packaging