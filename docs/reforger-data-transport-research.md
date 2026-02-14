# Arma Reforger Data Transport Research

Research into viable methods for getting gameplay capture data out of an Arma Reforger mod to an external system (database, web service, etc.) — in the context of a potential OCAP2 port.

**Date**: 2026-02-13

---

## Overview

Arma Reforger uses the Enfusion engine with Enforce Script (C#-like syntax) instead of Arma 3's SQF. The Arma 3 `callExtension` DLL mechanism does not exist natively. Several alternative data transport mechanisms are available, ranging from built-in engine features to community frameworks.

| Method                        | Viability | Server-Side | Direction     | Real-Time | Platform    |
|-------------------------------|-----------|-------------|---------------|-----------|-------------|
| **RestApi (HTTP)**            | HIGH      | Yes         | Outbound      | Yes       | All         |
| **FileIO (files)**            | HIGH      | Yes         | Write to disk | Blocking  | All         |
| **Enfusion DB Framework**     | HIGH      | Yes         | Both          | Async+Sync| All         |
| **InterceptAR (C++ ext)**     | HIGH      | Yes (only)  | Both          | Yes       | Win + Linux |
| **Named Pipes (IPC)**         | LOW       | Yes         | Bidirectional | Yes       | Windows only|
| **Log Parsing**               | MEDIUM    | External    | Read-only     | Near-RT   | All         |
| **RCON**                      | NONE      | External    | Inbound only  | N/A       | All         |
| **BackendApi (Bohemia cloud)**| NONE      | Yes         | Bohemia only  | N/A       | All         |

---

## 1. REST API (Built-in)

**The primary built-in mechanism for outbound HTTP communication.**

Enforce Script provides native `RestApi` and `RestContext` classes for making HTTP requests.

### Usage

```csharp
// Get a RestContext for a base URL
RestContext ctx = GetGame().GetRestApi().GetContext("https://your-api.example.com/");

// Async POST with JSON body
ctx.POST(myCallback, "endpoint", jsonString);

// Blocking (synchronous) variant
ctx.POST_now("endpoint", jsonString);

// Async GET
ctx.GET(myCallback, "endpoint?param=value");

// PUT, DELETE also available (async and blocking)
ctx.PUT_now("endpoint", data);
ctx.DELETE_now("endpoint", data);
```

Callbacks extend `RestCallback` and implement:
- `OnSuccess(string data, int dataSize)`
- `OnError(int errorCode)`
- `OnTimeout()`

### Details

- **Server-side**: Yes — confirmed by multiple production mods (GTG Live Map, PG-RestApi, Logging Enhanced)
- **Methods**: GET, POST, PUT, DELETE (async + blocking `_now` variants)
- **Custom headers**: Supported via `RestContext.SetHeaders(string)` — comma-separated key,value pairs: `"Content-Type,application/json,User-Agent,MyApp"`
- **JSON support**: Built-in via `JsonSaveContainer` / `ContainerSerializationSaveContext`, or `SCR_JsonSaveContext` / `SCR_JsonLoadContext`
- **Data size limit**: ~1 MB per request
- **No raw sockets**: Only HTTP — no TCP/UDP socket support

### Limitations

- 1 MB payload limit per request (requires batching/chunking for large data)
- No streaming or chunked transfer support
- HTTP only — no WebSocket, TCP, or UDP

### Documentation

- [REST API Usage — BI Wiki](https://community.bistudio.com/wiki/Arma_Reforger:REST_API_Usage)
- [JsonApiStruct Usage — BI Wiki](https://community.bistudio.com/wiki/Arma_Reforger:JsonApiStruct_Usage)
- [RestContext Interface Reference](https://community.bistudio.com/wikidata/external-data/arma-reforger/EnfusionScriptAPIPublic/interfaceRestContext.html)

### Assessment

**Viability: HIGH** — Most practical and widely-used method. The 1 MB limit is workable with periodic batched POSTs of capture frames.

---

## 2. File I/O (Built-in)

**Direct file writing to the server's filesystem.**

Enforce Script provides `FileIO` (static utility class) and `FileHandle` for reading/writing files.

### Usage

```csharp
// Write a file
FileHandle fh = FileIO.OpenFile("$profile:data.json", FileMode.WRITE);
if (fh)
{
    fh.WriteLine("[");
    fh.WriteLine("  {\"key\": \"value\"}");
    fh.WriteLine("]");
    fh.Close();
}

// Other operations
FileIO.MakeDirectory("$profile:ocap");
FileIO.FileExists("$profile:myfile.json");
FileIO.DeleteFile("$profile:myfile.json");
FileIO.FindFiles(callback, "$profile:mydir", ".json");
FileIO.CopyFile(source, destination);
```

JSON serialization to file:

```csharp
ContainerSerializationSaveContext writer();
JsonSaveContainer jsonContainer = new JsonSaveContainer();
writer.SetContainer(jsonContainer);
writer.WriteValue("", myObject);
jsonContainer.SaveToFile("$profile:output.json");
```

### Details

- **Server-side**: Yes — used by multiple mods (KillLog, Logging Enhanced, Enfusion DB Framework)
- **Path restrictions**: Sandboxed to `$profile:`, `$logs:`, or `$saves:` directories. `$profile:` maps to the server's profile directory on disk
- **Operations**: OpenFile (read/write/append), MakeDirectory, DeleteFile, FindFiles, CopyFile, FileExists
- **Synchronous only**: All operations are blocking

### Limitations

- Sandboxed — cannot write to arbitrary filesystem paths
- All operations blocking — heavy writes cause frame hitches
- Requires a **sidecar process** to pick up files and forward to external systems
- Not suitable for high-frequency real-time streaming

### Documentation

- [FileIO Interface — BI Script API](https://community.bistudio.com/wikidata/external-data/arma-reforger/EnfusionScriptAPIPublic/interfaceFileIO.html)
- [Serialisation — BI Wiki](https://community.bistudio.com/wiki/Arma_Reforger:Serialisation)
- [Scripting: JSON — BI Wiki](https://community.bistudio.com/wiki/Arma_Reforger:Scripting:_JSON)

### Assessment

**Viability: HIGH for local capture, MEDIUM for external systems.** Simple and reliable, but requires a companion process to move data off-disk. Good fallback option.

---

## 3. Enfusion Database Framework (Community Mod)

**A full ORM-like database abstraction layer by Arkensor, wrapping FileIO and RestApi.**

### Supported Backends

| Driver         | Transport              | Status         | Notes                                      |
|----------------|------------------------|----------------|--------------------------------------------|
| **JsonFile**   | FileIO (local)         | Stable         | JSON files at `$profile:/.db/`             |
| **BinaryFile** | FileIO (local)         | Stable         | Binary files, smaller than JSON            |
| **InMemory**   | RAM only               | Stable         | For unit testing                           |
| **MongoDB**    | HTTP via C#/.NET proxy | Stable         | Via Docker-deployable proxy application    |
| **BIBackend**  | Bohemia cloud          | In Development | Cloud-synced binary files                  |
| **HTTP Proxy** | RestApi                | In Development | Generic web API intermediary               |
| **SQL**        | HTTP proxy (planned)   | In Development | MySQL/PostgreSQL/SQLite via future proxy   |

### Architecture

- **Local drivers** (JsonFile, BinaryFile): Use `FileIO` directly, writing to `$profile:/.db/DatabaseName/CollectionName/EntityId.json`
- **Proxy drivers** (MongoDB, future SQL): Use `RestContext` to communicate with an external HTTP proxy that translates requests to native database operations

MongoDB proxy connection string:
```
MongoDb://MyDatabase?Host=1.2.3.4&Port=8008&Headers=Content-Type,application/json,api-key,123456
```

### Details

- **Server-side**: Yes — designed for dedicated servers
- **API**: Sync and async CRUD operations, query builder, pagination, sorting
- **MongoDB proxy**: Docker-deployable C# (.NET 7.0) app with IP whitelisting and TLS
- **Status**: BETA — no backward compatibility guarantee until v1.0.0
- **License**: Permitted for commercial/monetized servers

### Limitations

- Beta — APIs may change before v1.0.0
- SQL drivers not yet implemented
- Proxy-based drivers add network latency
- MongoDB proxy requires running a separate Docker service
- Entity classes cannot have constructors with parameters

### Links

- [GitHub: EnfusionDatabaseFramework](https://github.com/Arkensor/EnfusionDatabaseFramework)
- [GitHub: MongoDB Web Proxy](https://github.com/Arkensor/EnfusionDatabaseFramework.Drivers.WebProxy.MongoDB)
- [Arma Reforger Workshop](https://reforger.armaplatform.com/workshop/5D6EA74A94173EDF)

### Assessment

**Viability: HIGH** if MongoDB is acceptable. Provides a clean API and handles all the plumbing. The proxy model could also be extended for other backends.

---

## 4. InterceptAR — Native C++ Extensions (Community)

**C++20 binding interface for writing native plugin DLLs callable from Enforce Script.** This is the Reforger equivalent of Arma 3's `callExtension` system.

### How It Works

1. Create a C++20 plugin using Visual Studio 2022
2. Register C++ functions that map to Enforce Script method signatures
3. Deploy the DLL (Windows) or SO (Linux) alongside the server executable
4. Call native methods from Enforce Script as if they were built-in `proto` methods

### Details

- **Server-side**: Yes — dedicated server and Workbench only (not on game clients)
- **Platform**: Windows and Linux
- **Capabilities**: Unlimited — raw TCP/UDP sockets, direct database drivers (Redis, PostgreSQL, etc.), WebSockets, binary protocols, anything C++ can do
- **Maturity**: Alpha (6 releases as of May 2025), functional

### Limitations

- Dedicated server / Workbench only — will not work on game clients
- Early stage — "functionality is limited but it should be sufficient for most use cases"
- Requires C++20 / Visual Studio 2022 knowledge
- Host library (InterceptHost) is closed-source
- Small community (~25 GitHub stars)
- Crash responsibility falls on the plugin developer

### Links

- [GitHub: interceptAR](https://github.com/intercept/interceptAR)

### Assessment

**Viability: HIGH potential** — Most powerful option. Direct Redis, PostgreSQL, or any custom protocol connection without HTTP overhead. Higher development effort than RestApi, but no payload limits or protocol restrictions. Best escape hatch if the REST approach proves insufficient.

---

## 5. Named Pipes via FileIO (Community Workaround)

**Abuses FileIO to read/write Windows named pipes for IPC.**

### How It Works

A proof-of-concept by CallMeMax that uses `FileIO.OpenFile()` pointed at named pipe paths (`\\.\pipe\ArmaIn`) for bidirectional communication between the game and an external Python service.

### Details

- **Server-side**: Yes
- **Platform**: Windows ONLY
- **Bidirectional**: Can send and receive data
- **Not production-ready**: Author explicitly states stability and performance issues

### Limitations

- Windows only — no Linux support
- May fail in published Workshop builds (works in Workbench editor)
- Requires external Python service running alongside the server
- Proof-of-concept only

### Links

- [GitHub: enfusion-pipe-ipc](https://github.com/CallMeMax/enfusion-pipe-ipc)

### Assessment

**Viability: LOW** — Interesting proof of concept but not suitable for production.

---

## 6. Log File Parsing (External Workaround)

**Parse server log files externally using a sidecar process.**

### How It Works

1. Server writes to `console.log` and `script.log` in the profile directory
2. Use `-logStats` launch parameter for performance statistics logging
3. External process tails/watches log files and extracts data
4. Optionally, a mod writes structured logs via `Print()` or FileIO

### Existing Implementations

- **ReforgerJS**: Tails `console.log`, parses events, stores in MySQL — no mod required
- **Logging Enhanced**: Mod that writes structured JSON logs to `$profile:` for external parsing
- **Grok patterns**: Community-made patterns for parsing Reforger logs into InfluxDB/Grafana

### Limitations

- Passive — only captures what the game logs or what a mod explicitly `Print()`s
- Requires custom parsing logic for unstructured log data
- Near-real-time but with polling latency

### Links

- [GitHub: ReforgerJS](https://github.com/ZSU-GG-Reforger/ReforgerJS)
- [Logging Enhanced (Workshop)](https://reforger.armaplatform.com/workshop/6316335D6A19E51C-LoggingEnhancedbyflabby)
- [Grok patterns for Reforger logs](https://gist.github.com/simi/c713b91fbe8f9dcdcefed7a65908b3c6)

### Assessment

**Viability: MEDIUM** — Useful as a supplementary approach but insufficient for the volume and structure of OCAP-style capture data.

---

## 7. RCON (Built-in) — Not Viable

BattlEye-based remote console for server administration. Supports kick, ban, restart commands only. No custom commands, no data export capability.

- [Server Management — BI Wiki](https://community.bistudio.com/wiki/Arma_Reforger:Server_Management)

---

## 8. BackendApi / Bohemia Backend (Built-in) — Not Viable

Bohemia's internal platform API for admin checks, session management, cloud saves. Cannot be used to send data to custom external systems.

---

## Serialization Format: Protobuf vs JSON

### Can protobuf be used?

Protocol Buffers (protobuf) would offer more compact and structured data than JSON. However, native support in Enforce Script is limited.

### The constraint: Built-in RestApi is string-only

`RestContext.POST()` accepts a **`string` parameter** for the request body. Protobuf is a binary format. The Enfusion REST API has no concept of raw binary payloads — it is designed around text/JSON.

### Options for protobuf transport

| Approach | How | Practical? |
|---|---|---|
| **Base64-encoded protobuf over RestApi** | Serialize → base64 → POST as string → decode on receiver | Works but adds ~33% size overhead, partially negating protobuf's compactness. Still subject to ~1 MB limit. Adds encoding/decoding overhead on both sides. |
| **InterceptAR (C++ extension)** | Native protobuf C++ library, send via raw TCP socket | Full protobuf support, no size limits, no encoding overhead. Requires C++20 development. |
| **Compact JSON instead** | Use short keys, omit defaults, batch aggressively | Gets 60-70% of protobuf's benefit with zero extra complexity. |

### Assessment

For OCAP-style data (positions, events, frame captures), **the bandwidth savings of protobuf vs. compact JSON likely don't justify the added complexity**:

- OCAP captures at ~1 second intervals, not milliseconds
- Typical frame data for 100 units is ~20-50 KB as JSON
- With short keys and no null values, JSON is already fairly compact
- The receiver typically runs on localhost or LAN, so bandwidth is rarely the bottleneck

**If binary efficiency is truly needed**, InterceptAR is the only viable path — it provides full C++ with native protobuf serialization and raw sockets. But this is a significant development investment.

**Pragmatic recommendation**: Start with compact JSON over RestApi. Graduate to InterceptAR + protobuf only if actual performance limits are hit.

---

## Recommended Architecture for OCAP Reforger

### Primary: RestApi → Companion Web Service → Database

```
┌─────────────────────┐         HTTP POST          ┌──────────────────┐
│   Arma Reforger     │  ───────────────────────►   │  Web Service     │
│   (Enforce Script)  │   JSON batches (~1MB max)   │  (Node/Go/Python)│
│                     │                             │                  │
│  - Capture loop     │                             │  - Receives JSON │
│  - Event handlers   │                             │  - Writes to DB  │
│  - JSON serialize   │                             │  - Any backend   │
│  - RestApi POST     │                             │    (PG/Redis/...)│
└─────────────────────┘                             └────────┬─────────┘
                                                             │
                                                             ▼
                                                    ┌──────────────────┐
                                                    │   Database       │
                                                    │  (Postgres/Redis/│
                                                    │   MongoDB/SQLite)│
                                                    └────────┬─────────┘
                                                             │
                                                             ▼
                                                    ┌──────────────────┐
                                                    │  OCAP Web        │
                                                    │  Frontend        │
                                                    │  (existing)      │
                                                    └──────────────────┘
```

**Why this approach:**
- Uses only built-in engine features (no mod/framework dependencies)
- Cross-platform (Windows + Linux dedicated servers)
- Async HTTP avoids blocking the game loop
- Web service layer decouples game from database choice — can use any backend
- 1 MB request limit is workable with periodic batched POSTs (e.g. every N capture frames)
- Existing OCAP web frontend can be reused with minimal changes
- Production-proven pattern (GTG Live Map, Enfusion DB Framework MongoDB driver)

### Fallback: InterceptAR for Direct Connections

If the REST approach proves limiting (latency, payload size, lack of streaming), InterceptAR provides an escape hatch for:
- Direct Redis/PostgreSQL connections without HTTP overhead
- WebSocket streaming for real-time data
- Custom binary protocols for efficiency
- No payload size limits

This requires C++20 development but provides capabilities equivalent to Arma 3's extension DLL system.

---

## Key Reforger Scripting References

- [From SQF to Enforce Script](https://community.bistudio.com/wiki/Arma_Reforger:From_SQF_to_Enforce_Script)
- [Event Handlers](https://community.bistudio.com/wiki/Arma_Reforger:Event_Handlers)
- [ScriptInvoker Usage](https://community.bistudio.com/wiki/Arma_Reforger:ScriptInvoker_Usage)
- [Script API Class List](https://community.bistudio.com/wikidata/external-data/arma-reforger/ArmaReforgerScriptAPIPublic/annotated.html)
- [Arma Reforger Explorer](https://arexplorer.zeroy.com/)
- [Modding Boot Camps](https://reforger.armaplatform.com/news/modding-boot-camps-introduction)
- [Scripting Changes in 1.1](https://reforger.armaplatform.com/news/modding-update-scripting-1-1)
