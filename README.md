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

## Building

### Addon

The addon has no build step. Enforce Script `.c` files are loaded directly by the Arma Reforger engine at runtime — there is no ahead-of-time compilation.

### Receiver (Go 1.23+)

```bash
cd receiver
go build -o ocap-receiver .
```

## Quick Start

Three things need to run together: the **OCAP2 web server** (for playback), the **receiver** (collects data from the game), and the **addon** (runs inside Arma Reforger). Here's how to set them up from scratch.

### Step 1: Set up the OCAP2 web server

Follow the instructions at [OCAP2/web](https://github.com/OCAP2/web) to get the web frontend running. Note the URL it's running on (e.g. `http://your-server:5000`) and the API secret you configured — you'll need both in the next step.

### Step 2: Build and start the receiver

Build the receiver by following the instructions in the [Building](#building) section, then start it pointing at your web server:

```bash
OCAP_WEB_URL=http://your-server:5000 OCAP_API_SECRET=your-secret ./ocap-receiver
```

The receiver listens on port `8080` by default. It will accept data from the game and, when a mission ends, package it up and upload it to the web server for playback.

| Variable | Default | Description |
|----------|---------|-------------|
| `LISTEN_ADDR` | `:8080` | HTTP listen address |
| `OCAP_WEB_URL` | `http://localhost:5000` | OCAP2 web server URL |
| `OCAP_API_SECRET` | _(empty)_ | Secret for OCAP2 web uploads |
| `SESSION_TIMEOUT` | `30m` | Auto-export stale sessions after this duration |

### Step 3: Install the addon on your Reforger server

1. Copy the `addon/` folder into your Arma Reforger dedicated server's mod directory
2. Open your scenario in **Enfusion Workbench**
3. Find your scenario's **GameMode** entity (e.g. `SCR_BaseGameMode`)
4. Click **Add Component** and select `OCAP_GameModeComponent`
5. In the component's properties, set the **Receiver URL** to the address where the receiver is running (e.g. `http://your-server:8080`)
6. Adjust any other settings as needed (see table below)
7. Save the scenario and start your server

| Setting | Default | Description |
|---------|---------|-------------|
| Enable OCAP | `true` | Master recording toggle |
| Capture Delay | `1.0s` | Interval between position captures |
| Receiver URL | `http://localhost:8080` | Go receiver address |
| Auto Start | `true` | Start recording when player threshold is met |
| Min Player Count | `1` | Players required for auto-start |
| Tag | _(empty)_ | Mission tag (e.g. TvT, COOP) |

### Step 4: Play and review

Once players join and the minimum player threshold is reached, recording starts automatically. When the mission ends (or all players leave), the receiver exports the data to the web server. Open the OCAP2 web frontend in your browser to replay the mission.

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
