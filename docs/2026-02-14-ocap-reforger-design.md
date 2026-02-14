# OCAP Reforger — Design Document

**Date**: 2026-02-14
**Goal**: MVP Arma Reforger addon + Go receiver that captures gameplay data and produces replays compatible with the existing OCAP2 web frontend.
**Transport**: Built-in Enforce Script RestApi (HTTP POST, JSON)
**Approach**: Live streaming — data is sent incrementally during the mission, assembled on mission end.

---

## 1. High-Level Architecture

```
┌─────────────────────────────────┐
│  Arma Reforger Dedicated Server │
│                                 │
│  ┌───────────────────────────┐  │         HTTP POST (JSON)
│  │  OCAP Reforger Mod        │  │  ──────────────────────────►
│  │                           │  │     /api/session/start
│  │  - CaptureManager         │  │     /api/session/entities
│  │  - EventManager           │  │     /api/session/frames
│  │  - TransportService       │  │     /api/session/events
│  │                           │  │     /api/session/end
│  └───────────────────────────┘  │
└─────────────────────────────────┘
                                          ┌─────────────────────┐
                                          │  OCAP Receiver (Go)  │
                                          │                      │
                                          │  - HTTP server        │
                                          │  - Session buffer     │
                                          │  - JSON/gz builder    │
                                          │  - OCAP web uploader  │
                                          └──────────┬────────────┘
                                                     │
                                          POST /api/v1/operations/add
                                                     │
                                                     ▼
                                          ┌─────────────────────┐
                                          │  OCAP2 Web Server    │
                                          │  (existing)          │
                                          └─────────────────────┘
```

The addon captures game state and streams it as JSON batches to the receiver. The receiver accumulates everything in memory, and on mission end builds the `.json.gz` file in the exact OCAP2 v1 export format, then uploads it to the OCAP2 web server.

---

## 2. Reforger Addon — Components

### 2.1 CaptureManager

The capture loop. Runs on a configurable timer (default 1s).

Each tick:
- Iterates all tracked entities (soldiers + vehicles)
- Collects position, bearing, life state, vehicle crew, etc.
- Assigns sequential OCAP IDs to new entities
- Maintains a frame counter
- Passes frame data to TransportService

### 2.2 EventManager

Event handler registration. Hooks into Reforger's scripted event system:

- `OnControllableDestroyed` / `OnPlayerKilled` — kill events
- `OnPlayerConnected` / `OnPlayerDisconnected`
- Weapon fired events (via weapon component event handlers)
- Vehicle enter/exit

Each event is timestamped with the current frame number and passed to TransportService.

### 2.3 TransportService

HTTP transport layer:
- Maintains a `RestContext` to the receiver URL (configurable)
- Batches data and POSTs as JSON
- Handles async callbacks (success/error/timeout)
- Sends session start (mission metadata) on recording begin
- Sends session end on mission end or manual stop

### 2.4 Session

Singleton holding recording state:
- Recording flag
- Session ID (from receiver)
- Frame counter
- Entity ID counter
- Entity registry (`IEntity` → OCAP ID mapping)

### 2.5 Configuration

Settings exposed via Reforger's addon settings UI:

| Setting | Type | Default | Description |
|---|---|---|---|
| `enabled` | bool | true | Master toggle |
| `captureDelay` | float | 1.0 | Seconds between capture frames (0.5–2.0) |
| `receiverUrl` | string | `http://localhost:8080` | Receiver HTTP endpoint |
| `autoStart` | bool | true | Start recording on mission begin |
| `minPlayerCount` | int | 1 | Minimum players to auto-start |

---

## 3. Addon-to-Receiver API

### `POST /api/session/start`

Sent once when recording begins.

```json
{
  "worldName": "Everon",
  "missionName": "My_Mission",
  "missionAuthor": "Author",
  "captureDelay": 1.0,
  "tag": "TvT",
  "startTime": "2026-02-14T19:30:00Z"
}
```

Response: `{"sessionId": "uuid"}`

### `POST /api/session/entities`

Sent whenever new entities are first seen. Can be sent multiple times.

```json
{
  "sessionId": "uuid",
  "entities": [
    {
      "id": 5,
      "type": "unit",
      "name": "Player Name",
      "group": "Alpha 1",
      "side": "WEST",
      "isPlayer": 1,
      "role": "Rifleman",
      "startFrameNum": 0
    },
    {
      "id": 12,
      "type": "vehicle",
      "name": "M1025",
      "side": "WEST",
      "class": "car",
      "startFrameNum": 0
    }
  ]
}
```

### `POST /api/session/frames`

Sent every N ticks (e.g. every 5–10 frames as a batch). Bulk of the data.

```json
{
  "sessionId": "uuid",
  "frameNum": 42,
  "units": [
    [5, [3021.5, 5200.1, 25.3], 180, 1, 0, "Player Name", 1, "Rifleman"]
  ],
  "vehicles": [
    [12, [3050.0, 5180.0, 24.0], 90, 1, [5, 7]]
  ]
}
```

**Unit array positions**: `[id, [x,y,z], bearing, lifeState, vehicleId, name, isPlayer, role]`

**Vehicle array positions**: `[id, [x,y,z], bearing, alive, [crewIds]]`

Arrays instead of objects to keep payloads compact.

### `POST /api/session/events`

Sent as events occur, batched per tick or small window.

```json
{
  "sessionId": "uuid",
  "events": [
    [42, "killed", 5, [8, "AK-74"], 150.3],
    [42, "hit", 7, [8, "AK-74"], 85.0],
    [43, "connected", "PlayerName"],
    [50, "fired", 8, [3021.5, 5200.1, 25.3]]
  ]
}
```

Event formats match the OCAP2 v1 spec — the receiver passes them through as-is.

### `POST /api/session/end`

Sent when mission ends or recording is stopped.

```json
{
  "sessionId": "uuid",
  "endFrame": 3600,
  "endReason": "missionEnd"
}
```

Triggers the receiver to build `.json.gz` and upload to OCAP2 web server.

---

## 4. Go Receiver Service

### 4.1 Structure

```
ocap-receiver/
├── main.go           # HTTP server setup, config loading
├── config.go         # Configuration (listen addr, OCAP web URL, API secret)
├── handler.go        # HTTP endpoint handlers
├── session.go        # Session state management + concurrent session map
├── builder.go        # Assembles buffered data into OCAP2 v1 JSON
├── uploader.go       # Multipart POST to OCAP2 web server
└── types.go          # Shared data types
```

### 4.2 Session Lifecycle

```
/session/start     → create Session{metadata, entities: [], frames: [], events: []}
/session/entities  → append to session.entities
/session/frames    → append frame states to per-entity position arrays
/session/events    → append to session.events
/session/end       → builder.Build(session) → gzip → uploader.Upload() → delete session
```

### 4.3 Builder Logic

Transforms streamed data into the OCAP2 v1 export format:

- **entities array**: Indexed by entity ID (gaps filled with `null`). Each entity gets its `positions` array built from accumulated frame data. For vehicles, consecutive identical states are RLE-compressed with `[startFrame, endFrame]`.
- **events array**: Passed through as-is (already in the right format from the addon).
- **Markers**: Empty array for MVP (out of scope).
- **times array**: Populated from frame timestamps if time tracking is enabled.

Output: gzipped JSON matching the v1 structure:

```json
{
  "addonVersion": "0.1.0",
  "extensionVersion": "0.1.0",
  "extensionBuild": "reforger",
  "missionName": "My_Mission",
  "missionAuthor": "Author",
  "worldName": "Everon",
  "endFrame": 3600,
  "captureDelay": 1.0,
  "tags": "TvT",
  "times": [],
  "entities": [ ... ],
  "events": [ ... ],
  "Markers": []
}
```

### 4.4 Configuration

```yaml
listen: ":8080"
ocapWebUrl: "https://ocap.example.com"
ocapApiSecret: "your-secret"
sessionTimeout: "3h"
```

### 4.5 Error Handling

- **Session timeout**: If no data received for configurable duration, auto-end and export what exists (crash resilience)
- **Upload failure**: Retry 3 times, then save `.json.gz` to disk as fallback
- **Invalid session ID**: Return 404, addon should re-send `/session/start`

---

## 5. Reforger Addon — Enforce Script Structure

### 5.1 File Layout

```
OCAP_Reforger/
├── Workbench/                          # Workbench project files
├── Scripts/
│   └── Game/
│       └── OCAP/
│           ├── config.c                # Addon settings
│           ├── OCAP_CaptureManager.c   # Capture loop + entity tracking
│           ├── OCAP_EventManager.c     # Event handler registration + dispatch
│           ├── OCAP_TransportService.c # RestApi HTTP layer
│           ├── OCAP_Session.c          # Session state
│           └── OCAP_Types.c            # Enums, constants, side mapping
└── addon.gproj                         # Enfusion project file
```

### 5.2 Key Classes

**OCAP_Session**:
```csharp
class OCAP_Session
{
    bool m_bRecording;
    string m_sSessionId;
    int m_iFrameNum;
    int m_iNextEntityId;
    ref map<IEntity, int> m_mEntityIds;  // game entity → OCAP ID
}
```

**OCAP_CaptureManager**:
```csharp
class OCAP_CaptureManager
{
    void OnCaptureTick()
    {
        // 1. Get all characters via GetGame().GetPlayerManager() + entity queries
        // 2. For each: read position, bearing, life state
        // 3. Check if in vehicle, get vehicle entity + crew
        // 4. New entities → assign ID, queue to TransportService.SendEntities()
        // 5. Batch frame data → TransportService.SendFrames()
        m_iFrameNum++;
    }
}
```

**OCAP_EventManager**:
```csharp
class OCAP_EventManager
{
    void Init()
    {
        SCR_BaseGameMode gameMode = SCR_BaseGameMode.Cast(GetGame().GetGameMode());
        gameMode.GetOnPlayerKilled().Insert(OnPlayerKilled);
        gameMode.GetOnPlayerConnected().Insert(OnPlayerConnected);
        gameMode.GetOnPlayerDisconnected().Insert(OnPlayerDisconnected);
    }
}
```

**OCAP_TransportService**:
```csharp
class OCAP_TransportService
{
    ref RestContext m_Ctx;

    void Init(string url)
    {
        m_Ctx = GetGame().GetRestApi().GetContext(url);
        m_Ctx.SetHeaders("Content-Type,application/json");
    }

    void SendFrames(string sessionId, int frameNum, string unitsJson, string vehiclesJson)
    {
        string body = "{\"sessionId\":\"" + sessionId + "\",\"frameNum\":" + frameNum + ... "}";
        m_Ctx.POST(new OCAP_RestCallback(), "/api/session/frames", body);
    }
}
```

### 5.3 JSON Serialization

JSON strings are built manually via string concatenation — the common pattern in Reforger mods (no generic `JSON.stringify()` in Enforce Script). For array-heavy frame data this is straightforward:

```csharp
// Unit state: [id, [x,y,z], bearing, lifeState, vehicleId, name, isPlayer, role]
string json = "[" + id + ",[" + x + "," + y + "," + z + "]," + bearing + "," + lifeState + ... "]";
```

---

## 6. MVP Scope

### In Scope

| Feature | Notes |
|---|---|
| Unit position tracking | Position, bearing, life state, name, isPlayer, role |
| Vehicle position tracking | Position, bearing, alive, crew list |
| Kill events | Victim, killer, weapon name, distance |
| Hit events | Same structure as kills |
| Player connect/disconnect | Player name |
| Fired events | Firer ID, impact position |
| End mission event | Winning side if available |
| Session management | Start/end, metadata, auto-export |
| Go receiver + OCAP2 upload | Full pipeline to existing web frontend |

### Out of Scope (Future)

| Feature | Why deferred |
|---|---|
| Markers | Reforger's marker system differs from Arma 3's |
| Unconscious state | No ACE equivalent in Reforger; life state is binary |
| Server performance metrics | Not needed for playback |
| Projectile trajectories | Expensive; MVP tracks fire + impact only |
| Vehicle turret/fuel/damage | Low priority for MVP |
| Respawn ticket tracking | Game-mode specific |
| Custom events API | Add once core works |

### Known Risks

1. **Entity enumeration**: Need to confirm the Enforce Script API for iterating all units/vehicles server-side efficiently.
2. **Weapon/kill attribution**: Reforger's damage system may attribute kills differently. The data available in `OnControllableDestroyed` needs validation.
3. **Side mapping**: Reforger factions don't map 1:1 to Arma 3's WEST/EAST/GUER/CIV. A faction-to-side mapping layer is needed.
4. **Map coordinates**: Reforger may use a different coordinate system. The receiver or frontend may need coordinate transformation.
