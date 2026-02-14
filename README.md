# OCAP Reforger

Arma Reforger addon for Operation Capture and Playback (OCAP). Records gameplay data during missions and streams it to a companion receiver service for web-based playback via the existing [OCAP2 web frontend](https://github.com/OCAP2/web).

> **Note:** This is the Arma Reforger port. For Arma 3, see [OCAP2/addon](https://github.com/OCAP2/addon).

## Architecture

```
Arma Reforger Server                         OCAP Receiver (Go)
┌──────────────────────┐   HTTP POST (JSON)  ┌──────────────────┐
│  OCAP Reforger Mod   │ ──────────────────► │  Session buffer   │
│                      │   /session/start    │  V1 export builder│
│  - CaptureManager    │   /session/entities │  Gzip + upload    │
│  - EventManager      │   /session/frames   └────────┬─────────┘
│  - TransportService  │   /session/events             │
│  - Session           │   /session/end     POST /api/v1/operations/add
└──────────────────────┘                               │
                                                       ▼
                                             ┌──────────────────┐
                                             │  OCAP2 Web Server │
                                             │  (existing)       │
                                             └──────────────────┘
```

The addon captures game state every tick and streams JSON batches to the receiver. On mission end, the receiver assembles a `.json.gz` in OCAP2 v1 format and uploads it to the web server.

## Requirements

- Arma Reforger Dedicated Server
- Go 1.23+ (for building the receiver)
- [OCAP2 Web Server](https://github.com/OCAP2/web) (for playback)

## Setup

### 1. Receiver

```bash
cd receiver
go build -o ocap-receiver .
```

Configure via environment variables:

| Variable | Default | Description |
|----------|---------|-------------|
| `LISTEN_ADDR` | `:8080` | HTTP listen address |
| `OCAP_WEB_URL` | `http://localhost:5000` | OCAP2 web server URL |
| `OCAP_API_SECRET` | _(empty)_ | Secret for OCAP2 web uploads |
| `SESSION_TIMEOUT` | `30m` | Auto-export stale sessions after this duration |

```bash
OCAP_WEB_URL=http://your-ocap-server:5000 OCAP_API_SECRET=your-secret ./ocap-receiver
```

### 2. Addon

Place the `addon/` folder in your Reforger server's mod directory. Add the `OCAP_GameModeComponent` to your scenario's game mode entity in the Enfusion Workbench.

Component settings (configurable in editor):

| Setting | Default | Description |
|---------|---------|-------------|
| Enable OCAP | `true` | Master recording toggle |
| Capture Delay | `1.0s` | Interval between position captures |
| Receiver URL | `http://localhost:8080` | Go receiver address |
| Auto Start | `true` | Start recording when player threshold is met |
| Min Player Count | `1` | Players required for auto-start |
| Tag | _(empty)_ | Mission tag (e.g. TvT, COOP) |

## What Gets Captured

- **Units**: Position, bearing, life state (alive/dead/unconscious), faction, role, vehicle occupancy — for both players and AI
- **Vehicles**: Position, bearing, alive/destroyed, crew list, vehicle class (car/heli/plane/sea)
- **Events**: Kills (with weapon and distance), player connections/disconnections, mission end

## Testing

Run the integration test against a local receiver:

```bash
cd receiver
go build -o ocap-receiver .
./ocap-receiver &
bash test.sh
kill %1
```

This simulates a full session lifecycle with curl and validates the output.

## Project Structure

```
addon/                          # Arma Reforger mod (Enforce Script)
├── addon.gproj
└── Scripts/Game/OCAP/
    ├── OCAP_Types.c            # Constants, JSON utility, faction mapper
    ├── OCAP_TransportService.c # RestApi HTTP transport
    ├── OCAP_Session.c          # Singleton session state
    ├── OCAP_CaptureManager.c   # Per-frame capture loop
    ├── OCAP_EventManager.c     # Kill/connection event handlers
    └── OCAP_GameModeComponent.c# Entry point component

receiver/                       # Go receiver service
├── main.go                     # Entry point, HTTP routing
├── config.go                   # Environment-based configuration
├── types.go                    # Request/response types, V1 export struct
├── handler.go                  # HTTP handlers for 5 endpoints
├── session.go                  # Thread-safe session store
├── builder.go                  # V1 export builder (frame assembly)
├── uploader.go                 # Gzip, multipart upload, disk fallback
└── test.sh                     # Integration test

docs/                           # Design and research documents
```

## License

GNU General Public License v3.0
