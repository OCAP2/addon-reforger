# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

OCAP Reforger — Arma Reforger addon that records gameplay data and streams it to a Go receiver service for web-based playback via the existing [OCAP2 web frontend](https://github.com/OCAP2/web). Port of OCAP2 from Arma 3 to Reforger.

## Build & Test

### Receiver (Go 1.23+)

```bash
cd receiver
go build -o ocap-receiver .
```

### Integration Test

```bash
cd receiver
bash test.sh
```

`test.sh` builds the receiver, starts it, simulates a full session lifecycle with curl, validates the `.json.gz` output against OCAP2 v1 format, then cleans up. Requires `python3` for JSON validation.

### Addon (Enforce Script)

No build step — deployed directly to Reforger server mod directory. Edited/tested in Enfusion Workbench.

## Architecture

Two components communicate over HTTP:

1. **Addon** (`addon/Scripts/Game/OCAP/`) — Enforce Script (.c files) running on Arma Reforger server. Captures game state on a timer and POSTs JSON batches to the receiver.
2. **Receiver** (`receiver/`) — Go HTTP service. Buffers streamed data in memory, then on mission end assembles OCAP2 v1 format JSON, gzips it, and uploads to the OCAP2 web server (with disk fallback on failure).

### Data Flow

Addon → `POST /api/session/{start,entities,frames,events,end}` → Receiver → `POST /api/v1/operations/add` → OCAP2 Web Server

### Addon Components

- `OCAP_GameModeComponent.c` — Entry point, configurable via Enfusion editor attributes (enable, capture delay, receiver URL, auto-start, min players, tag)
- `OCAP_CaptureManager.c` — Timer-driven capture loop for players, AI, and vehicles
- `OCAP_EventManager.c` — Hooks into game mode events (kills, connections)
- `OCAP_Session.c` — Singleton state: recording flag, session ID, entity ID registry
- `OCAP_TransportService.c` — HTTP transport via built-in `RestApi`/`RestContext`
- `OCAP_Types.c` — Constants, `EscapeJson()`, faction-to-side mapper

### Receiver Components

- `main.go` — HTTP server setup, 5 endpoints + healthcheck
- `handler.go` — Request handlers; `/session/end` triggers async export via goroutine
- `session.go` — Thread-safe session store (RWMutex), UUID session IDs, timeout cleanup
- `builder.go` — Transforms streamed frame data into OCAP2 v1 format (gap-filling, entity array padding with nulls)
- `uploader.go` — Gzip compression, multipart upload with 3 retries + exponential backoff, disk fallback
- `config.go` — Environment variable config (`LISTEN_ADDR`, `OCAP_WEB_URL`, `OCAP_API_SECRET`, `SESSION_TIMEOUT`)
- `types.go` — Request/response structs, V1Export struct matching OCAP2 v1 format

## Key Patterns

- **Enforce Script has no JSON library** — all JSON in the addon is built via manual string concatenation. Use `OCAP_Types.EscapeJson()` for string values.
- **Frame data uses compact arrays, not objects** — unit state is `[id, [x,y,z], bearing, lifeState, vehicleId, name, isPlayer, role]`; the builder strips the leading `id` when assembling v1 output.
- **`json.RawMessage`** — the receiver uses raw JSON for frame data to avoid deserialize/reserialize overhead.
- **Atomic session removal** — `TakeSession()` in session.go prevents double-export races between `/session/end` and timeout cleanup.
- **Only dependency** — `github.com/google/uuid` (receiver). No npm, no Makefile.
