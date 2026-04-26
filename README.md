# claudecode-provider-flater

`claudecode-provider-flater` is a local gateway that flattens multiple upstream LLM providers into one Anthropic-compatible local provider for Claude Code.

The core purpose of this repository is:

> Wrap multiple providers behind a single local provider, so Claude Code can switch between them with `/model`.

## What it does

- Exposes a local Anthropic-compatible endpoint for Claude Code.
- Routes requests to multiple upstream providers such as Anthropic-compatible or OpenAI-compatible backends.
- Maps friendly local model aliases like `haiku`, `sonnet`, `opus`, or custom names to different upstream providers and upstream model IDs.
- Lets Claude Code switch routes by using `/model <alias>` instead of rewriting provider settings every time.
- Includes a local web control panel for configuration, connectivity checks, runtime monitoring, and active stream visibility.
- Supports background launch, explicit foreground mode with `--show`, and a restart supervisor with `--daemon`.

## Typical use case

Instead of pointing Claude Code at one vendor directly, you point it at this local gateway:

- Claude Code talks to `http://127.0.0.1:9457`
- The gateway receives the request
- The selected local alias determines which upstream provider and upstream model to use
- In Claude Code, you switch with `/model haiku`, `/model sonnet`, `/model opus`, or any custom alias you configured

This means one Claude Code setup can transparently route across multiple providers.

## Build

### Windows with CMake

```powershell
cmake -B build -S . -G "MinGW Makefiles"
cmake --build build
```

### Convenience script

```bash
./build.sh
```

The launcher script for Windows is:

```bat
start.bat
```

## Install and run

The project installs and runs from the user-scoped Claude directory:

- Binary directory: `~/.claude/model-gateway/bin`
- Config file: `~/.claude/model-gateway/config.json`

### Start modes

- Default: starts as a background worker and returns control to the shell.
- `--show`: keeps the gateway in the current console.
- `--daemon`: starts a supervisor that restarts the worker if it exits unexpectedly.

Examples:

```powershell
./build/model-gateway.exe
./build/model-gateway.exe --show
./build/model-gateway.exe --daemon
./build/model-gateway.exe --show 9457
```

## Configuration model

The gateway uses three layers:

- `providers`: upstream endpoints and API keys
- `models`: gateway model definitions that bind a provider to a concrete upstream model name
- `aliases`: short names that Claude Code can select with `/model`

Example shape:

```json
{
  "port": 9457,
  "bind": "127.0.0.1",
  "thread_pool_size": 8,
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
      "protocol": "anthropic"
    },
    "deepseek-v4-flash": {
      "id": "deepseek-v4-flash",
      "provider": "deepseek",
      "upstream_model": "deepseek-v4-flash",
      "protocol": "openai"
    }
  },
  "aliases": {
    "opus": "astron-code-latest",
    "haiku": "deepseek-v4-flash"
  }
}
```

## Use with Claude Code

### 1. Start the gateway

Run the gateway locally so it listens on `127.0.0.1:9457` or your chosen port.

### 2. Point Claude Code to the local gateway

Use the gateway as the local custom provider endpoint:

```json
{
  "provider": "custom",
  "customBaseUrl": "http://127.0.0.1:9457",
  "model": "sonnet"
}
```

### 3. Switch models inside Claude Code

Once aliases are configured in the gateway, switch providers and upstream models from Claude Code with:

```text
/model haiku
/model sonnet
/model opus
```

Those alias names are local gateway aliases, not necessarily upstream vendor model IDs.

## Control panel

The gateway exposes a local web UI at:

```text
http://127.0.0.1:9457/
```

The control panel supports:

- editing providers, models, aliases, bind address, and thread pool size
- testing provider connectivity
- monitoring runtime counters
- viewing active streams and buffering state
- inspecting runtime file locations and monitor metadata

## Runtime APIs

- `GET /` — web control panel
- `GET /api/config` — current runtime config
- `POST /api/config` — save config
- `GET /api/config/test` — test one provider
- `GET /api/monitor` — runtime monitor data
- `GET /v1/models` — list model aliases and model metadata
- `POST /v1/messages` — Anthropic-compatible messages endpoint

## Why this exists

Claude Code is most convenient when the local tool surface stays stable.

This gateway keeps Claude Code pointed at one local provider while you change the actual upstream provider behind it using aliases and `/model`. That removes repeated provider reconfiguration and makes multi-provider experiments much easier.