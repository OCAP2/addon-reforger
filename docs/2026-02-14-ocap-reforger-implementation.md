# OCAP Reforger Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build an Arma Reforger mod + Go receiver service that captures gameplay data and uploads it to the existing OCAP2 web frontend for replay.

**Architecture:** The Reforger mod captures unit/vehicle positions and events each tick via Enforce Script, POSTs JSON batches to a Go HTTP receiver using the built-in RestApi. On mission end, the receiver assembles the OCAP2 v1 JSON format, gzips it, and uploads to the OCAP2 web server.

**Tech Stack:** Enforce Script (Reforger mod), Go 1.21+ (receiver), OCAP2 v1 JSON format, RestApi (HTTP POST)

**Design Doc:** `docs/plans/2026-02-14-ocap-reforger-design.md`

**API Research:** `docs/plans/reforger-data-transport-research.md`

---

## Task 1: Go Receiver — Project Scaffold + Health Check

**Files:**
- Create: `ocap-reforger-receiver/main.go`
- Create: `ocap-reforger-receiver/config.go`
- Create: `ocap-reforger-receiver/go.mod`

**Step 1: Initialize Go module**

```bash
mkdir -p ocap-reforger-receiver
cd ocap-reforger-receiver
go mod init github.com/OCAP2/ocap-reforger-receiver
```

**Step 2: Write config.go**

```go
package main

import (
	"os"
)

type Config struct {
	ListenAddr     string
	OcapWebURL     string
	OcapAPISecret  string
	SessionTimeout string
}

func LoadConfig() Config {
	return Config{
		ListenAddr:     envOr("LISTEN_ADDR", ":8080"),
		OcapWebURL:     envOr("OCAP_WEB_URL", "http://localhost:5000"),
		OcapAPISecret:  envOr("OCAP_API_SECRET", ""),
		SessionTimeout: envOr("SESSION_TIMEOUT", "3h"),
	}
}

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}
```

**Step 3: Write main.go with health check**

```go
package main

import (
	"log"
	"net/http"
)

func main() {
	cfg := LoadConfig()

	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthcheck", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"status":"ok"}`))
	})

	log.Printf("OCAP Reforger Receiver listening on %s", cfg.ListenAddr)
	log.Fatal(http.ListenAndServe(cfg.ListenAddr, mux))
}
```

**Step 4: Build and test**

```bash
cd ocap-reforger-receiver
go build -o ocap-receiver .
./ocap-receiver &
curl -s http://localhost:8080/healthcheck
# Expected: {"status":"ok"}
kill %1
```

**Step 5: Commit**

```bash
git add ocap-reforger-receiver/
git commit -m "feat(receiver): scaffold Go project with health check endpoint"
```

---

## Task 2: Go Receiver — Types + Session Store

**Files:**
- Create: `ocap-reforger-receiver/types.go`
- Create: `ocap-reforger-receiver/session.go`

**Step 1: Write types.go**

Define all the request/response types and the OCAP2 v1 export types.

```go
package main

import "time"

// --- Inbound request types (from Reforger addon) ---

type StartRequest struct {
	WorldName    string  `json:"worldName"`
	MissionName  string  `json:"missionName"`
	MissionAuthor string `json:"missionAuthor"`
	CaptureDelay float64 `json:"captureDelay"`
	Tag          string  `json:"tag"`
	StartTime    string  `json:"startTime"`
}

type StartResponse struct {
	SessionID string `json:"sessionId"`
}

type EntityDef struct {
	ID            int    `json:"id"`
	Type          string `json:"type"`    // "unit" or "vehicle"
	Name          string `json:"name"`
	Group         string `json:"group,omitempty"`
	Side          string `json:"side"`
	IsPlayer      int    `json:"isPlayer"`
	Role          string `json:"role,omitempty"`
	Class         string `json:"class,omitempty"` // vehicle class: car, tank, etc.
	StartFrameNum int    `json:"startFrameNum"`
}

type EntitiesRequest struct {
	SessionID string      `json:"sessionId"`
	Entities  []EntityDef `json:"entities"`
}

// FramesRequest contains one frame's worth of state for all entities.
// Units and vehicles are raw JSON arrays for compact transport.
type FramesRequest struct {
	SessionID string            `json:"sessionId"`
	FrameNum  int               `json:"frameNum"`
	Units     []json.RawMessage `json:"units"`
	Vehicles  []json.RawMessage `json:"vehicles"`
}

type EventsRequest struct {
	SessionID string            `json:"sessionId"`
	Events    []json.RawMessage `json:"events"`
}

type EndRequest struct {
	SessionID string `json:"sessionId"`
	EndFrame  int    `json:"endFrame"`
	EndReason string `json:"endReason"`
}

// --- OCAP2 v1 export types ---

type V1Export struct {
	AddonVersion     string        `json:"addonVersion"`
	ExtensionVersion string        `json:"extensionVersion"`
	ExtensionBuild   string        `json:"extensionBuild"`
	MissionName      string        `json:"missionName"`
	MissionAuthor    string        `json:"missionAuthor"`
	WorldName        string        `json:"worldName"`
	EndFrame         int           `json:"endFrame"`
	CaptureDelay     float64       `json:"captureDelay"`
	Tags             string        `json:"tags"`
	Times            []interface{} `json:"times"`
	Entities         []interface{} `json:"entities"`
	Events           []interface{} `json:"events"`
	Markers          []interface{} `json:"Markers"` // capital M for web compat
}
```

Note: `FramesRequest` uses `json.RawMessage` — add `"encoding/json"` to imports.

**Step 2: Write session.go**

```go
package main

import (
	"encoding/json"
	"sync"
	"time"

	"github.com/google/uuid"
)

type Session struct {
	ID            string
	Metadata      StartRequest
	Entities      map[int]*EntityDef            // keyed by OCAP ID
	Frames        map[int]map[int]json.RawMessage // frameNum -> entityID -> state array
	Events        []json.RawMessage
	CreatedAt     time.Time
	LastActivity  time.Time
	mu            sync.Mutex
}

type SessionStore struct {
	sessions map[string]*Session
	mu       sync.RWMutex
}

func NewSessionStore() *SessionStore {
	return &SessionStore{
		sessions: make(map[string]*Session),
	}
}

func (s *SessionStore) Create(req StartRequest) *Session {
	sess := &Session{
		ID:           uuid.New().String(),
		Metadata:     req,
		Entities:     make(map[int]*EntityDef),
		Frames:       make(map[int]map[int]json.RawMessage),
		Events:       nil,
		CreatedAt:    time.Now(),
		LastActivity: time.Now(),
	}
	s.mu.Lock()
	s.sessions[sess.ID] = sess
	s.mu.Unlock()
	return sess
}

func (s *SessionStore) Get(id string) *Session {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.sessions[id]
}

func (s *SessionStore) Delete(id string) {
	s.mu.Lock()
	delete(s.sessions, id)
	s.mu.Unlock()
}

func (sess *Session) AddEntities(entities []EntityDef) {
	sess.mu.Lock()
	defer sess.mu.Unlock()
	for i := range entities {
		e := entities[i]
		sess.Entities[e.ID] = &e
	}
	sess.LastActivity = time.Now()
}

func (sess *Session) AddFrame(frameNum int, units, vehicles []json.RawMessage) {
	sess.mu.Lock()
	defer sess.mu.Unlock()
	frame := make(map[int]json.RawMessage)

	// Parse entity ID from first element of each array
	for _, raw := range units {
		var arr []json.RawMessage
		json.Unmarshal(raw, &arr)
		if len(arr) > 0 {
			var id int
			json.Unmarshal(arr[0], &id)
			frame[id] = raw
		}
	}
	for _, raw := range vehicles {
		var arr []json.RawMessage
		json.Unmarshal(raw, &arr)
		if len(arr) > 0 {
			var id int
			json.Unmarshal(arr[0], &id)
			frame[id] = raw
		}
	}

	sess.Frames[frameNum] = frame
	sess.LastActivity = time.Now()
}

func (sess *Session) AddEvents(events []json.RawMessage) {
	sess.mu.Lock()
	defer sess.mu.Unlock()
	sess.Events = append(sess.Events, events...)
	sess.LastActivity = time.Now()
}
```

**Step 3: Add uuid dependency**

```bash
cd ocap-reforger-receiver
go get github.com/google/uuid
```

**Step 4: Verify it compiles**

```bash
go build ./...
```

**Step 5: Commit**

```bash
git add ocap-reforger-receiver/
git commit -m "feat(receiver): add types and session store"
```

---

## Task 3: Go Receiver — HTTP Handlers

**Files:**
- Create: `ocap-reforger-receiver/handler.go`
- Modify: `ocap-reforger-receiver/main.go`

**Step 1: Write handler.go**

```go
package main

import (
	"encoding/json"
	"log"
	"net/http"
)

type Handler struct {
	store *SessionStore
}

func NewHandler(store *SessionStore) *Handler {
	return &Handler{store: store}
}

func (h *Handler) HandleStart(w http.ResponseWriter, r *http.Request) {
	var req StartRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "invalid json", http.StatusBadRequest)
		return
	}

	sess := h.store.Create(req)
	log.Printf("Session started: %s (mission: %s, world: %s)", sess.ID, req.MissionName, req.WorldName)

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(StartResponse{SessionID: sess.ID})
}

func (h *Handler) HandleEntities(w http.ResponseWriter, r *http.Request) {
	var req EntitiesRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "invalid json", http.StatusBadRequest)
		return
	}

	sess := h.store.Get(req.SessionID)
	if sess == nil {
		http.Error(w, "session not found", http.StatusNotFound)
		return
	}

	sess.AddEntities(req.Entities)
	log.Printf("Session %s: added %d entities", sess.ID[:8], len(req.Entities))
	w.WriteHeader(http.StatusOK)
}

func (h *Handler) HandleFrames(w http.ResponseWriter, r *http.Request) {
	var req FramesRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "invalid json", http.StatusBadRequest)
		return
	}

	sess := h.store.Get(req.SessionID)
	if sess == nil {
		http.Error(w, "session not found", http.StatusNotFound)
		return
	}

	sess.AddFrame(req.FrameNum, req.Units, req.Vehicles)
	w.WriteHeader(http.StatusOK)
}

func (h *Handler) HandleEvents(w http.ResponseWriter, r *http.Request) {
	var req EventsRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "invalid json", http.StatusBadRequest)
		return
	}

	sess := h.store.Get(req.SessionID)
	if sess == nil {
		http.Error(w, "session not found", http.StatusNotFound)
		return
	}

	sess.AddEvents(req.Events)
	log.Printf("Session %s: added %d events", sess.ID[:8], len(req.Events))
	w.WriteHeader(http.StatusOK)
}

func (h *Handler) HandleEnd(w http.ResponseWriter, r *http.Request) {
	var req EndRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "invalid json", http.StatusBadRequest)
		return
	}

	sess := h.store.Get(req.SessionID)
	if sess == nil {
		http.Error(w, "session not found", http.StatusNotFound)
		return
	}

	log.Printf("Session %s: ending at frame %d (%s)", sess.ID[:8], req.EndFrame, req.EndReason)

	// Build and upload (Task 4)
	export := BuildExport(sess, req.EndFrame)
	if err := CompressAndUpload(export, sess.Metadata, req.EndFrame); err != nil {
		log.Printf("Session %s: upload failed: %v", sess.ID[:8], err)
		// Fallback: save to disk (Task 5)
		if err := SaveToDisk(export, sess.Metadata); err != nil {
			log.Printf("Session %s: disk save also failed: %v", sess.ID[:8], err)
		}
	}

	h.store.Delete(req.SessionID)
	w.WriteHeader(http.StatusOK)
}
```

**Step 2: Update main.go to wire handlers**

```go
package main

import (
	"log"
	"net/http"
)

func main() {
	cfg := LoadConfig()
	store := NewSessionStore()
	h := NewHandler(store)

	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthcheck", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"status":"ok"}`))
	})
	mux.HandleFunc("POST /api/session/start", h.HandleStart)
	mux.HandleFunc("POST /api/session/entities", h.HandleEntities)
	mux.HandleFunc("POST /api/session/frames", h.HandleFrames)
	mux.HandleFunc("POST /api/session/events", h.HandleEvents)
	mux.HandleFunc("POST /api/session/end", h.HandleEnd)

	log.Printf("OCAP Reforger Receiver listening on %s", cfg.ListenAddr)
	log.Fatal(http.ListenAndServe(cfg.ListenAddr, mux))
}
```

**Step 3: Create stubs for BuildExport, CompressAndUpload, SaveToDisk** (so it compiles)

Create `ocap-reforger-receiver/builder.go`:
```go
package main

func BuildExport(sess *Session, endFrame int) *V1Export {
	// Implemented in Task 4
	return &V1Export{}
}
```

Create `ocap-reforger-receiver/uploader.go`:
```go
package main

func CompressAndUpload(export *V1Export, meta StartRequest, endFrame int) error {
	// Implemented in Task 5
	return nil
}

func SaveToDisk(export *V1Export, meta StartRequest) error {
	// Implemented in Task 5
	return nil
}
```

**Step 4: Verify it compiles**

```bash
cd ocap-reforger-receiver && go build ./...
```

**Step 5: Commit**

```bash
git add ocap-reforger-receiver/
git commit -m "feat(receiver): add HTTP handlers for session lifecycle"
```

---

## Task 4: Go Receiver — V1 Export Builder

**Files:**
- Modify: `ocap-reforger-receiver/builder.go`

**Step 1: Implement BuildExport**

This is the core logic that transforms streamed frame data into the OCAP2 v1 format.

```go
package main

import (
	"encoding/json"
	"sort"
)

func BuildExport(sess *Session, endFrame int) *V1Export {
	sess.mu.Lock()
	defer sess.mu.Unlock()

	// Find the max entity ID to size the entities array
	maxID := 0
	for id := range sess.Entities {
		if id > maxID {
			maxID = id
		}
	}

	// Build entities array indexed by ID (gaps = null)
	entities := make([]interface{}, maxID+1)
	for id, eDef := range sess.Entities {
		positions := buildPositions(sess, id, eDef, endFrame)
		if eDef.Type == "unit" {
			entities[id] = map[string]interface{}{
				"id":            eDef.ID,
				"name":          eDef.Name,
				"group":         eDef.Group,
				"side":          eDef.Side,
				"isPlayer":      eDef.IsPlayer,
				"type":          "unit",
				"role":          eDef.Role,
				"startFrameNum": eDef.StartFrameNum,
				"positions":     positions,
				"framesFired":   []interface{}{},
			}
		} else {
			entities[id] = map[string]interface{}{
				"id":            eDef.ID,
				"name":          eDef.Name,
				"side":          eDef.Side,
				"isPlayer":      0,
				"type":          "vehicle",
				"class":         eDef.Class,
				"startFrameNum": eDef.StartFrameNum,
				"positions":     positions,
				"framesFired":   []interface{}{},
			}
		}
	}

	// Build events array — pass through raw JSON
	events := make([]interface{}, len(sess.Events))
	for i, raw := range sess.Events {
		var evt interface{}
		json.Unmarshal(raw, &evt)
		events[i] = evt
	}

	return &V1Export{
		AddonVersion:     "0.1.0",
		ExtensionVersion: "0.1.0",
		ExtensionBuild:   "reforger-receiver",
		MissionName:      sess.Metadata.MissionName,
		MissionAuthor:    sess.Metadata.MissionAuthor,
		WorldName:        sess.Metadata.WorldName,
		EndFrame:         endFrame,
		CaptureDelay:     sess.Metadata.CaptureDelay,
		Tags:             sess.Metadata.Tag,
		Times:            []interface{}{},
		Entities:         entities,
		Events:           events,
		Markers:          []interface{}{},
	}
}

// buildPositions assembles the per-frame position array for one entity.
// For units: each frame → [pos, bearing, lifeState, vehicleId, name, isPlayer, role]
// For vehicles: each frame → [pos, bearing, alive, crewIds]
// Missing frames are filled with the last known state.
func buildPositions(sess *Session, entityID int, eDef *EntityDef, endFrame int) []interface{} {
	// Get sorted frame numbers
	frameNums := make([]int, 0, len(sess.Frames))
	for fn := range sess.Frames {
		frameNums = append(frameNums, fn)
	}
	sort.Ints(frameNums)

	if len(frameNums) == 0 {
		return []interface{}{}
	}

	startFrame := eDef.StartFrameNum
	positions := make([]interface{}, 0, endFrame-startFrame+1)
	var lastState json.RawMessage

	for frame := startFrame; frame <= endFrame; frame++ {
		if frameData, ok := sess.Frames[frame]; ok {
			if state, ok := frameData[entityID]; ok {
				lastState = state
			}
		}
		if lastState != nil {
			// Strip the ID from the front of the array — v1 format doesn't include it in positions
			var arr []json.RawMessage
			json.Unmarshal(lastState, &arr)
			if len(arr) > 1 {
				// arr[0] is the ID, arr[1:] is the position data
				stripped := make([]interface{}, len(arr)-1)
				for i := 1; i < len(arr); i++ {
					var v interface{}
					json.Unmarshal(arr[i], &v)
					stripped[i-1] = v
				}
				positions = append(positions, stripped)
			}
		}
	}

	return positions
}
```

**Step 2: Verify it compiles**

```bash
cd ocap-reforger-receiver && go build ./...
```

**Step 3: Commit**

```bash
git add ocap-reforger-receiver/builder.go
git commit -m "feat(receiver): implement V1 export builder with frame assembly"
```

---

## Task 5: Go Receiver — Gzip + Upload + Disk Fallback

**Files:**
- Modify: `ocap-reforger-receiver/uploader.go`
- Modify: `ocap-reforger-receiver/config.go` (make cfg accessible globally)

**Step 1: Implement uploader.go**

```go
package main

import (
	"bytes"
	"compress/gzip"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"mime/multipart"
	"net/http"
	"os"
	"strings"
	"time"
)

func CompressAndUpload(export *V1Export, meta StartRequest, endFrame int, cfg Config) error {
	// Serialize to JSON
	jsonData, err := json.Marshal(export)
	if err != nil {
		return fmt.Errorf("marshal: %w", err)
	}

	// Gzip compress
	var gzBuf bytes.Buffer
	gzWriter := gzip.NewWriter(&gzBuf)
	if _, err := gzWriter.Write(jsonData); err != nil {
		return fmt.Errorf("gzip write: %w", err)
	}
	if err := gzWriter.Close(); err != nil {
		return fmt.Errorf("gzip close: %w", err)
	}

	// Build filename
	timestamp := time.Now().Format("20060102_150405")
	safeName := strings.ReplaceAll(meta.MissionName, " ", "_")
	filename := fmt.Sprintf("%s_%s", safeName, timestamp)

	// Calculate duration
	duration := float64(endFrame) * meta.CaptureDelay

	// Upload as multipart form
	return uploadToOCAP(cfg, filename, meta, duration, gzBuf.Bytes())
}

func uploadToOCAP(cfg Config, filename string, meta StartRequest, duration float64, gzData []byte) error {
	var body bytes.Buffer
	writer := multipart.NewWriter(&body)

	writer.WriteField("secret", cfg.OcapAPISecret)
	writer.WriteField("filename", filename)
	writer.WriteField("worldName", meta.WorldName)
	writer.WriteField("missionName", meta.MissionName)
	writer.WriteField("missionDuration", fmt.Sprintf("%.1f", duration))
	writer.WriteField("tag", meta.Tag)

	part, err := writer.CreateFormFile("file", filename+".json.gz")
	if err != nil {
		return fmt.Errorf("create form file: %w", err)
	}
	if _, err := part.Write(gzData); err != nil {
		return fmt.Errorf("write form file: %w", err)
	}
	writer.Close()

	url := strings.TrimRight(cfg.OcapWebURL, "/") + "/api/v1/operations/add"
	req, err := http.NewRequest("POST", url, &body)
	if err != nil {
		return fmt.Errorf("create request: %w", err)
	}
	req.Header.Set("Content-Type", writer.FormDataContentType())

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return fmt.Errorf("upload: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode >= 300 {
		respBody, _ := io.ReadAll(resp.Body)
		return fmt.Errorf("upload returned %d: %s", resp.StatusCode, string(respBody))
	}

	log.Printf("Uploaded %s to OCAP web server (%d bytes gzipped)", filename, len(gzData))
	return nil
}

func SaveToDisk(export *V1Export, meta StartRequest) error {
	jsonData, err := json.Marshal(export)
	if err != nil {
		return fmt.Errorf("marshal: %w", err)
	}

	timestamp := time.Now().Format("20060102_150405")
	safeName := strings.ReplaceAll(meta.MissionName, " ", "_")
	filename := fmt.Sprintf("fallback_%s_%s.json.gz", safeName, timestamp)

	var gzBuf bytes.Buffer
	gzWriter := gzip.NewWriter(&gzBuf)
	gzWriter.Write(jsonData)
	gzWriter.Close()

	if err := os.WriteFile(filename, gzBuf.Bytes(), 0644); err != nil {
		return fmt.Errorf("write file: %w", err)
	}

	log.Printf("Saved fallback file: %s (%d bytes)", filename, gzBuf.Len())
	return nil
}
```

**Step 2: Update handler.go to pass config**

Update `Handler` struct and `HandleEnd` to pass `cfg` to `CompressAndUpload`. Add a `cfg Config` field to `Handler`, set it in `NewHandler`, and pass `h.cfg` in `HandleEnd`.

**Step 3: Update main.go**

Pass `cfg` to `NewHandler`:
```go
h := NewHandler(store, cfg)
```

**Step 4: Verify it compiles**

```bash
cd ocap-reforger-receiver && go build ./...
```

**Step 5: Commit**

```bash
git add ocap-reforger-receiver/
git commit -m "feat(receiver): add gzip compression, OCAP upload, and disk fallback"
```

---

## Task 6: Go Receiver — Session Timeout Cleanup

**Files:**
- Modify: `ocap-reforger-receiver/session.go`
- Modify: `ocap-reforger-receiver/main.go`

**Step 1: Add cleanup goroutine to SessionStore**

Add a method to SessionStore that periodically checks for stale sessions and auto-exports them:

```go
func (s *SessionStore) StartCleanup(interval, timeout time.Duration, cfg Config) {
	go func() {
		ticker := time.NewTicker(interval)
		defer ticker.Stop()
		for range ticker.C {
			s.mu.RLock()
			var stale []string
			for id, sess := range s.sessions {
				sess.mu.Lock()
				if time.Since(sess.LastActivity) > timeout {
					stale = append(stale, id)
				}
				sess.mu.Unlock()
			}
			s.mu.RUnlock()

			for _, id := range stale {
				sess := s.Get(id)
				if sess == nil {
					continue
				}
				log.Printf("Session %s timed out, auto-exporting", id[:8])
				// Find max frame
				maxFrame := 0
				sess.mu.Lock()
				for fn := range sess.Frames {
					if fn > maxFrame {
						maxFrame = fn
					}
				}
				sess.mu.Unlock()

				export := BuildExport(sess, maxFrame)
				if err := CompressAndUpload(export, sess.Metadata, maxFrame, cfg); err != nil {
					log.Printf("Session %s: timeout upload failed: %v", id[:8], err)
					SaveToDisk(export, sess.Metadata)
				}
				s.Delete(id)
			}
		}
	}()
}
```

**Step 2: Wire up in main.go**

Parse timeout duration from config and start cleanup:

```go
timeout, _ := time.ParseDuration(cfg.SessionTimeout)
store.StartCleanup(5*time.Minute, timeout, cfg)
```

**Step 3: Verify it compiles**

```bash
cd ocap-reforger-receiver && go build ./...
```

**Step 4: Commit**

```bash
git add ocap-reforger-receiver/
git commit -m "feat(receiver): add session timeout auto-export"
```

---

## Task 7: Go Receiver — Integration Test with curl

**Files:**
- Create: `ocap-reforger-receiver/test.sh`

**Step 1: Write an end-to-end test script**

This script simulates the full addon → receiver flow using curl:

```bash
#!/bin/bash
set -e

BASE="http://localhost:8080"

echo "=== Starting session ==="
SESSION_ID=$(curl -s -X POST "$BASE/api/session/start" \
  -H "Content-Type: application/json" \
  -d '{
    "worldName": "Everon",
    "missionName": "Test_Mission",
    "missionAuthor": "TestAuthor",
    "captureDelay": 1.0,
    "tag": "test",
    "startTime": "2026-02-14T19:00:00Z"
  }' | jq -r '.sessionId')
echo "Session ID: $SESSION_ID"

echo "=== Sending entities ==="
curl -s -X POST "$BASE/api/session/entities" \
  -H "Content-Type: application/json" \
  -d "{
    \"sessionId\": \"$SESSION_ID\",
    \"entities\": [
      {\"id\":0,\"type\":\"unit\",\"name\":\"Player1\",\"group\":\"Alpha\",\"side\":\"WEST\",\"isPlayer\":1,\"role\":\"Rifleman\",\"startFrameNum\":0},
      {\"id\":1,\"type\":\"unit\",\"name\":\"Enemy1\",\"group\":\"Bravo\",\"side\":\"EAST\",\"isPlayer\":0,\"role\":\"Rifleman\",\"startFrameNum\":0},
      {\"id\":2,\"type\":\"vehicle\",\"name\":\"HMMWV\",\"side\":\"WEST\",\"class\":\"car\",\"isPlayer\":0,\"startFrameNum\":0}
    ]
  }"

echo "=== Sending frames ==="
for FRAME in 0 1 2 3 4; do
  X=$((3000 + FRAME * 10))
  curl -s -X POST "$BASE/api/session/frames" \
    -H "Content-Type: application/json" \
    -d "{
      \"sessionId\": \"$SESSION_ID\",
      \"frameNum\": $FRAME,
      \"units\": [
        [0, [$X.0, 5200.0, 25.0], 180, 1, 0, \"Player1\", 1, \"Rifleman\"],
        [1, [4000.0, 5200.0, 25.0], 0, 1, 0, \"Enemy1\", 0, \"Rifleman\"]
      ],
      \"vehicles\": [
        [2, [$X.0, 5180.0, 24.0], 90, 1, []]
      ]
    }"
done

echo "=== Sending events ==="
curl -s -X POST "$BASE/api/session/events" \
  -H "Content-Type: application/json" \
  -d "{
    \"sessionId\": \"$SESSION_ID\",
    \"events\": [
      [3, \"killed\", 1, [0, \"M4A1\"], 1000.0],
      [0, \"connected\", \"Player1\"]
    ]
  }"

echo "=== Ending session ==="
curl -s -X POST "$BASE/api/session/end" \
  -H "Content-Type: application/json" \
  -d "{
    \"sessionId\": \"$SESSION_ID\",
    \"endFrame\": 4,
    \"endReason\": \"missionEnd\"
  }"

echo ""
echo "=== Done. Check receiver logs for output. ==="
echo "If no OCAP web server configured, look for fallback_*.json.gz in working directory."
```

**Step 2: Run the test**

```bash
cd ocap-reforger-receiver
go build -o ocap-receiver . && ./ocap-receiver &
sleep 1
bash test.sh
kill %1
```

Verify:
- All curl commands return 200
- Receiver logs show session start, entities, end
- A `fallback_*.json.gz` file is created (since no OCAP web server is running)
- Decompress it and verify the JSON structure matches v1 format

```bash
gunzip -k fallback_Test_Mission_*.json.gz
cat fallback_Test_Mission_*.json | jq '.entities | length'
# Expected: 3
```

**Step 3: Commit**

```bash
git add ocap-reforger-receiver/test.sh
git commit -m "test(receiver): add integration test script"
```

---

## Task 8: Reforger Addon — Project Scaffold

**Files:**
- Create: `ocap-reforger-addon/addon.gproj`
- Create: `ocap-reforger-addon/Scripts/Game/OCAP/OCAP_Types.c`

**Step 1: Create project structure**

```bash
mkdir -p ocap-reforger-addon/Scripts/Game/OCAP
```

**Step 2: Write addon.gproj**

This is the Enfusion project file. Minimal viable version:

```json
{
  "guid": "OCAP-Reforger-MVP",
  "type": "addon",
  "name": "OCAP Reforger",
  "description": "Operation Capture and Playback for Arma Reforger",
  "version": "0.1.0",
  "dependencies": [
    {
      "type": "game",
      "name": "ArmaReforger"
    }
  ]
}
```

**Step 3: Write OCAP_Types.c**

Side mapping and constants:

```csharp
// OCAP_Types.c — Enums, constants, faction-to-side mapping

class OCAP_Constants
{
    // Life states matching OCAP2 v1 format
    static const int LIFESTATE_DEAD = 0;
    static const int LIFESTATE_ALIVE = 1;
    static const int LIFESTATE_UNCONSCIOUS = 2;
}

class OCAP_SideMapper
{
    // Maps Reforger faction keys to OCAP2 side strings
    // Default Reforger factions: "US", "USSR", "FIA"
    static string GetSide(string factionKey)
    {
        if (factionKey == "US" || factionKey == "USA")
            return "WEST";
        if (factionKey == "USSR" || factionKey == "RUSSIA")
            return "EAST";
        if (factionKey == "FIA")
            return "GUER";
        return "CIV";
    }
}
```

**Step 4: Commit**

```bash
git add ocap-reforger-addon/
git commit -m "feat(addon): scaffold Reforger mod project with types"
```

---

## Task 9: Reforger Addon — TransportService (RestApi Layer)

**Files:**
- Create: `ocap-reforger-addon/Scripts/Game/OCAP/OCAP_TransportService.c`

**Step 1: Write OCAP_TransportService.c**

```csharp
// OCAP_TransportService.c — HTTP transport via RestApi

class OCAP_RestCallback : RestCallback
{
    string m_sEndpoint;

    void OCAP_RestCallback(string endpoint)
    {
        m_sEndpoint = endpoint;
    }

    override void OnSuccess(string data, int dataSize)
    {
        // Success — no action needed for most endpoints
    }

    override void OnError(int errorCode)
    {
        Print("[OCAP] REST error on " + m_sEndpoint + ": " + errorCode.ToString(), LogLevel.ERROR);
    }

    override void OnTimeout()
    {
        Print("[OCAP] REST timeout on " + m_sEndpoint, LogLevel.WARNING);
    }
}

class OCAP_StartCallback : RestCallback
{
    override void OnSuccess(string data, int dataSize)
    {
        // Parse sessionId from response: {"sessionId":"uuid"}
        // Simple string extraction (no JSON parser needed for this)
        int start = data.IndexOf("\"sessionId\":\"");
        if (start < 0)
            return;
        start += 14; // length of "sessionId":"
        int end = data.IndexOfFrom(start, "\"");
        if (end < 0)
            return;

        string sessionId = data.Substring(start, end - start);
        OCAP_Session session = OCAP_Session.GetInstance();
        if (session)
            session.OnSessionStarted(sessionId);
    }

    override void OnError(int errorCode)
    {
        Print("[OCAP] Failed to start session: error " + errorCode.ToString(), LogLevel.ERROR);
    }

    override void OnTimeout()
    {
        Print("[OCAP] Session start request timed out", LogLevel.WARNING);
    }
}

class OCAP_TransportService
{
    protected ref RestContext m_Ctx;
    protected string m_sBaseUrl;

    void Init(string baseUrl)
    {
        m_sBaseUrl = baseUrl;
        m_Ctx = GetGame().GetRestApi().GetContext(baseUrl);
        m_Ctx.SetHeaders("Content-Type,application/json");
        Print("[OCAP] TransportService initialized: " + baseUrl, LogLevel.NORMAL);
    }

    void SendStart(string worldName, string missionName, string missionAuthor, float captureDelay, string tag)
    {
        string body = "{";
        body += "\"worldName\":\"" + worldName + "\",";
        body += "\"missionName\":\"" + missionName + "\",";
        body += "\"missionAuthor\":\"" + missionAuthor + "\",";
        body += "\"captureDelay\":" + captureDelay.ToString() + ",";
        body += "\"tag\":\"" + tag + "\"";
        body += "}";

        m_Ctx.POST(new OCAP_StartCallback(), "/api/session/start", body);
    }

    void SendEntities(string sessionId, string entitiesJson)
    {
        string body = "{\"sessionId\":\"" + sessionId + "\",\"entities\":[" + entitiesJson + "]}";
        m_Ctx.POST(new OCAP_RestCallback("/api/session/entities"), "/api/session/entities", body);
    }

    void SendFrames(string sessionId, int frameNum, string unitsJson, string vehiclesJson)
    {
        string body = "{\"sessionId\":\"" + sessionId + "\",\"frameNum\":" + frameNum.ToString();
        body += ",\"units\":[" + unitsJson + "]";
        body += ",\"vehicles\":[" + vehiclesJson + "]}";
        m_Ctx.POST(new OCAP_RestCallback("/api/session/frames"), "/api/session/frames", body);
    }

    void SendEvents(string sessionId, string eventsJson)
    {
        if (eventsJson.IsEmpty())
            return;
        string body = "{\"sessionId\":\"" + sessionId + "\",\"events\":[" + eventsJson + "]}";
        m_Ctx.POST(new OCAP_RestCallback("/api/session/events"), "/api/session/events", body);
    }

    void SendEnd(string sessionId, int endFrame, string reason)
    {
        string body = "{\"sessionId\":\"" + sessionId + "\",\"endFrame\":" + endFrame.ToString();
        body += ",\"endReason\":\"" + reason + "\"}";
        m_Ctx.POST(new OCAP_RestCallback("/api/session/end"), "/api/session/end", body);
    }
}
```

**Step 2: Commit**

```bash
git add ocap-reforger-addon/Scripts/Game/OCAP/OCAP_TransportService.c
git commit -m "feat(addon): add TransportService REST layer"
```

---

## Task 10: Reforger Addon — Session State

**Files:**
- Create: `ocap-reforger-addon/Scripts/Game/OCAP/OCAP_Session.c`

**Step 1: Write OCAP_Session.c**

```csharp
// OCAP_Session.c — Singleton session state

class OCAP_Session
{
    private static ref OCAP_Session s_Instance;

    protected bool m_bRecording;
    protected bool m_bWaitingForSession;
    protected string m_sSessionId;
    protected int m_iFrameNum;
    protected int m_iNextEntityId;
    protected ref map<IEntity, int> m_mEntityIds;
    protected ref array<string> m_aPendingEvents;

    void OCAP_Session()
    {
        m_bRecording = false;
        m_bWaitingForSession = false;
        m_sSessionId = "";
        m_iFrameNum = 0;
        m_iNextEntityId = 0;
        m_mEntityIds = new map<IEntity, int>();
        m_aPendingEvents = {};
    }

    static OCAP_Session GetInstance()
    {
        if (!s_Instance)
            s_Instance = new OCAP_Session();
        return s_Instance;
    }

    bool IsRecording()
    {
        return m_bRecording && !m_sSessionId.IsEmpty();
    }

    void OnSessionStarted(string sessionId)
    {
        m_sSessionId = sessionId;
        m_bWaitingForSession = false;
        m_bRecording = true;
        Print("[OCAP] Session started: " + sessionId, LogLevel.NORMAL);
    }

    void StartRecording()
    {
        m_bWaitingForSession = true;
        m_iFrameNum = 0;
        m_iNextEntityId = 0;
        m_mEntityIds.Clear();
        m_aPendingEvents.Clear();
    }

    void StopRecording()
    {
        m_bRecording = false;
        m_bWaitingForSession = false;
    }

    string GetSessionId()
    {
        return m_sSessionId;
    }

    int GetFrameNum()
    {
        return m_iFrameNum;
    }

    void IncrementFrame()
    {
        m_iFrameNum++;
    }

    // Returns the OCAP ID for an entity, or -1 if not tracked
    int GetEntityId(IEntity entity)
    {
        if (!entity || !m_mEntityIds.Contains(entity))
            return -1;
        return m_mEntityIds.Get(entity);
    }

    // Assigns a new OCAP ID to an entity. Returns the new ID.
    int RegisterEntity(IEntity entity)
    {
        int id = m_iNextEntityId;
        m_mEntityIds.Set(entity, id);
        m_iNextEntityId++;
        return id;
    }

    bool IsEntityTracked(IEntity entity)
    {
        return entity && m_mEntityIds.Contains(entity);
    }

    void QueueEvent(string eventJson)
    {
        m_aPendingEvents.Insert(eventJson);
    }

    string FlushEvents()
    {
        if (m_aPendingEvents.IsEmpty())
            return "";

        string result = "";
        for (int i = 0; i < m_aPendingEvents.Count(); i++)
        {
            if (i > 0)
                result += ",";
            result += m_aPendingEvents[i];
        }
        m_aPendingEvents.Clear();
        return result;
    }
}
```

**Step 2: Commit**

```bash
git add ocap-reforger-addon/Scripts/Game/OCAP/OCAP_Session.c
git commit -m "feat(addon): add Session state singleton"
```

---

## Task 11: Reforger Addon — CaptureManager

**Files:**
- Create: `ocap-reforger-addon/Scripts/Game/OCAP/OCAP_CaptureManager.c`

**Step 1: Write OCAP_CaptureManager.c**

This is the core capture loop. It runs every N seconds, iterates entities, and sends frame data.

```csharp
// OCAP_CaptureManager.c — Core capture loop

class OCAP_CaptureManager
{
    protected ref OCAP_TransportService m_Transport;
    protected float m_fCaptureDelay;

    void Init(OCAP_TransportService transport, float captureDelay)
    {
        m_Transport = transport;
        m_fCaptureDelay = captureDelay;
    }

    void Start()
    {
        int delayMs = (int)(m_fCaptureDelay * 1000);
        GetGame().GetCallqueue().CallLater(OnCaptureTick, delayMs, true);
        Print("[OCAP] Capture loop started (interval: " + m_fCaptureDelay.ToString() + "s)", LogLevel.NORMAL);
    }

    void Stop()
    {
        GetGame().GetCallqueue().Remove(OnCaptureTick);
        Print("[OCAP] Capture loop stopped", LogLevel.NORMAL);
    }

    protected void OnCaptureTick()
    {
        OCAP_Session session = OCAP_Session.GetInstance();
        if (!session.IsRecording())
            return;

        string newEntitiesJson = "";
        string unitsJson = "";
        string vehiclesJson = "";
        int newEntityCount = 0;

        // --- Capture players and AI characters ---
        PlayerManager playerMgr = GetGame().GetPlayerManager();
        array<int> playerIds = {};
        playerMgr.GetPlayers(playerIds);

        // Track which vehicles we've seen this frame
        ref set<IEntity> vehiclesSeen = new set<IEntity>();

        foreach (int playerId : playerIds)
        {
            IEntity entity = playerMgr.GetPlayerControlledEntity(playerId);
            if (!entity)
                continue;

            ChimeraCharacter character = ChimeraCharacter.Cast(entity);
            if (!character)
                continue;

            string unitJson = CaptureUnit(character, true, playerMgr.GetPlayerName(playerId));
            if (unitJson.IsEmpty())
                continue;

            // Check for new entity registration
            if (!session.IsEntityTracked(entity))
            {
                string entDef = RegisterUnit(character, true, playerMgr.GetPlayerName(playerId));
                if (!entDef.IsEmpty())
                {
                    if (newEntityCount > 0) newEntitiesJson += ",";
                    newEntitiesJson += entDef;
                    newEntityCount++;
                }
            }

            if (!unitsJson.IsEmpty()) unitsJson += ",";
            unitsJson += unitJson;

            // Check if in vehicle
            CaptureVehicleFromOccupant(character, vehiclesSeen, newEntitiesJson, newEntityCount, vehiclesJson);
        }

        // --- Capture AI (non-player characters) via entity query ---
        ref array<IEntity> aiEntities = {};
        GetGame().GetWorld().QueryEntitiesBySphere(
            "0 0 0", 50000, null, null,
            EQueryEntitiesFlags.DYNAMIC
        );
        // Note: QueryEntitiesBySphere uses callbacks. For the MVP, we rely on
        // player-controlled entities + vehicles they interact with.
        // Full AI tracking can be added by implementing the query callback pattern.

        // --- Send new entities if any ---
        if (newEntityCount > 0)
            m_Transport.SendEntities(session.GetSessionId(), newEntitiesJson);

        // --- Send frame data ---
        m_Transport.SendFrames(session.GetSessionId(), session.GetFrameNum(), unitsJson, vehiclesJson);

        // --- Send queued events ---
        string eventsJson = session.FlushEvents();
        if (!eventsJson.IsEmpty())
            m_Transport.SendEvents(session.GetSessionId(), eventsJson);

        session.IncrementFrame();
    }

    protected string CaptureUnit(ChimeraCharacter character, bool isPlayer, string playerName)
    {
        OCAP_Session session = OCAP_Session.GetInstance();
        int id = session.GetEntityId(character);
        if (id < 0)
            return "";

        vector pos = character.GetOrigin();
        float bearing = character.GetYawPitchRoll()[0];
        if (bearing < 0) bearing += 360;
        int dir = (int)Math.Round(bearing);

        // Life state
        int lifeState = OCAP_Constants.LIFESTATE_ALIVE;
        CharacterControllerComponent ctrl = CharacterControllerComponent.Cast(
            character.FindComponent(CharacterControllerComponent));
        if (ctrl && ctrl.IsDead())
            lifeState = OCAP_Constants.LIFESTATE_DEAD;

        // Vehicle check
        int vehicleId = 0;
        CompartmentAccessComponent compAccess = CompartmentAccessComponent.Cast(
            character.FindComponent(CompartmentAccessComponent));
        if (compAccess)
        {
            IEntity vehicle = compAccess.GetVehicle();
            if (vehicle)
                vehicleId = session.GetEntityId(vehicle);
            if (vehicleId < 0)
                vehicleId = 0;
        }

        // Name and role
        string name = playerName;
        string role = "Rifleman"; // Default — can be refined later

        int isPlayerInt = 0;
        if (isPlayer) isPlayerInt = 1;

        // Build array: [id, [x,y,z], bearing, lifeState, vehicleId, name, isPlayer, role]
        string json = "[" + id.ToString();
        json += ",[" + pos[0].ToString() + "," + pos[1].ToString() + "," + pos[2].ToString() + "]";
        json += "," + dir.ToString();
        json += "," + lifeState.ToString();
        json += "," + vehicleId.ToString();
        json += ",\"" + name + "\"";
        json += "," + isPlayerInt.ToString();
        json += ",\"" + role + "\"]";

        return json;
    }

    protected string RegisterUnit(ChimeraCharacter character, bool isPlayer, string playerName)
    {
        OCAP_Session session = OCAP_Session.GetInstance();
        int id = session.RegisterEntity(character);

        // Get faction/side
        string side = "CIV";
        FactionAffiliationComponent factionComp = FactionAffiliationComponent.Cast(
            character.FindComponent(FactionAffiliationComponent));
        if (factionComp)
        {
            Faction faction = factionComp.GetAffiliatedFaction();
            if (faction)
                side = OCAP_SideMapper.GetSide(faction.GetFactionKey());
        }

        string name = playerName;
        string role = "Rifleman";
        string group = "";
        int isPlayerInt = 0;
        if (isPlayer) isPlayerInt = 1;

        // Build entity definition JSON object
        string json = "{\"id\":" + id.ToString();
        json += ",\"type\":\"unit\"";
        json += ",\"name\":\"" + name + "\"";
        json += ",\"group\":\"" + group + "\"";
        json += ",\"side\":\"" + side + "\"";
        json += ",\"isPlayer\":" + isPlayerInt.ToString();
        json += ",\"role\":\"" + role + "\"";
        json += ",\"startFrameNum\":" + session.GetFrameNum().ToString() + "}";

        return json;
    }

    protected void CaptureVehicleFromOccupant(ChimeraCharacter character, set<IEntity> vehiclesSeen,
        inout string newEntitiesJson, inout int newEntityCount, inout string vehiclesJson)
    {
        OCAP_Session session = OCAP_Session.GetInstance();

        CompartmentAccessComponent compAccess = CompartmentAccessComponent.Cast(
            character.FindComponent(CompartmentAccessComponent));
        if (!compAccess)
            return;

        IEntity vehicle = compAccess.GetVehicle();
        if (!vehicle || vehiclesSeen.Contains(vehicle))
            return;

        vehiclesSeen.Insert(vehicle);

        // Register if new
        if (!session.IsEntityTracked(vehicle))
        {
            int vId = session.RegisterEntity(vehicle);

            string side = "UNKNOWN";
            string vName = vehicle.GetPrefabData().GetPrefabName();
            string vClass = "car"; // Default — could inspect vehicle type

            string entJson = "{\"id\":" + vId.ToString();
            entJson += ",\"type\":\"vehicle\"";
            entJson += ",\"name\":\"" + vName + "\"";
            entJson += ",\"side\":\"" + side + "\"";
            entJson += ",\"class\":\"" + vClass + "\"";
            entJson += ",\"isPlayer\":0";
            entJson += ",\"startFrameNum\":" + session.GetFrameNum().ToString() + "}";

            if (newEntityCount > 0) newEntitiesJson += ",";
            newEntitiesJson += entJson;
            newEntityCount++;
        }

        // Capture vehicle state
        int vId = session.GetEntityId(vehicle);
        if (vId < 0)
            return;

        vector pos = vehicle.GetOrigin();
        float bearing = vehicle.GetYawPitchRoll()[0];
        if (bearing < 0) bearing += 360;
        int dir = (int)Math.Round(bearing);

        int alive = 1;
        DamageManagerComponent dmg = DamageManagerComponent.Cast(
            vehicle.FindComponent(DamageManagerComponent));
        if (dmg && dmg.IsDestroyed())
            alive = 0;

        // Get crew IDs
        string crewJson = "";
        BaseCompartmentManagerComponent compMgr = BaseCompartmentManagerComponent.Cast(
            vehicle.FindComponent(BaseCompartmentManagerComponent));
        if (compMgr)
        {
            array<BaseCompartmentSlot> slots = {};
            compMgr.GetCompartments(slots);
            bool first = true;
            foreach (BaseCompartmentSlot slot : slots)
            {
                IEntity occupant = slot.GetOccupant();
                if (!occupant)
                    continue;
                int occId = session.GetEntityId(occupant);
                if (occId < 0)
                    continue;
                if (!first) crewJson += ",";
                crewJson += occId.ToString();
                first = false;
            }
        }

        // Build array: [id, [x,y,z], bearing, alive, [crewIds]]
        string json = "[" + vId.ToString();
        json += ",[" + pos[0].ToString() + "," + pos[1].ToString() + "," + pos[2].ToString() + "]";
        json += "," + dir.ToString();
        json += "," + alive.ToString();
        json += ",[" + crewJson + "]]";

        if (!vehiclesJson.IsEmpty()) vehiclesJson += ",";
        vehiclesJson += json;
    }
}
```

**Step 2: Commit**

```bash
git add ocap-reforger-addon/Scripts/Game/OCAP/OCAP_CaptureManager.c
git commit -m "feat(addon): add CaptureManager with unit and vehicle tracking"
```

---

## Task 12: Reforger Addon — EventManager

**Files:**
- Create: `ocap-reforger-addon/Scripts/Game/OCAP/OCAP_EventManager.c`

**Step 1: Write OCAP_EventManager.c**

```csharp
// OCAP_EventManager.c — Event handler registration and dispatch

class OCAP_EventManager
{
    void Init()
    {
        SCR_BaseGameMode gameMode = SCR_BaseGameMode.Cast(GetGame().GetGameMode());
        if (!gameMode)
        {
            Print("[OCAP] No SCR_BaseGameMode found, events will not be tracked", LogLevel.WARNING);
            return;
        }

        // Kill events
        gameMode.GetOnPlayerKilled().Insert(OnPlayerKilled);
        gameMode.GetOnControllableDestroyed().Insert(OnControllableDestroyed);

        // Connection events
        gameMode.GetOnPlayerConnected().Insert(OnPlayerConnected);
        gameMode.GetOnPlayerDisconnected().Insert(OnPlayerDisconnected);

        Print("[OCAP] EventManager initialized", LogLevel.NORMAL);
    }

    void Cleanup()
    {
        SCR_BaseGameMode gameMode = SCR_BaseGameMode.Cast(GetGame().GetGameMode());
        if (!gameMode)
            return;

        gameMode.GetOnPlayerKilled().Remove(OnPlayerKilled);
        gameMode.GetOnControllableDestroyed().Remove(OnControllableDestroyed);
        gameMode.GetOnPlayerConnected().Remove(OnPlayerConnected);
        gameMode.GetOnPlayerDisconnected().Remove(OnPlayerDisconnected);
    }

    protected void OnPlayerKilled(int playerId, IEntity playerEntity, IEntity killerEntity, notnull Instigator killer)
    {
        OCAP_Session session = OCAP_Session.GetInstance();
        if (!session.IsRecording())
            return;

        int frameNum = session.GetFrameNum();
        int victimId = session.GetEntityId(playerEntity);
        if (victimId < 0)
            return;

        // Get killer info
        IEntity killerEnt = killer.GetInstigatorEntity();
        int killerId = -1;
        string weaponName = "Unknown";
        float distance = 0;

        if (killerEnt)
        {
            killerId = session.GetEntityId(killerEnt);

            // Try to get weapon name
            BaseWeaponManagerComponent weaponMgr = BaseWeaponManagerComponent.Cast(
                killerEnt.FindComponent(BaseWeaponManagerComponent));
            if (weaponMgr)
            {
                BaseWeaponComponent weapon = weaponMgr.GetCurrentWeapon();
                if (weapon)
                {
                    UIInfo uiInfo = weapon.GetUIInfo();
                    if (uiInfo)
                        weaponName = uiInfo.GetName();
                }
            }

            // Distance
            if (playerEntity)
                distance = vector.Distance(playerEntity.GetOrigin(), killerEnt.GetOrigin());
        }

        // Build kill event: [frameNum, "killed", victimId, [killerId, "weaponName"], distance]
        string json = "[" + frameNum.ToString();
        json += ",\"killed\"";
        json += "," + victimId.ToString();
        json += ",[" + killerId.ToString() + ",\"" + weaponName + "\"]";
        json += "," + Math.Round(distance).ToString() + "]";

        session.QueueEvent(json);
    }

    protected void OnControllableDestroyed(IEntity entity, IEntity killerEntity, notnull Instigator killer)
    {
        // For non-player entities (AI, vehicles)
        // Skip if already handled by OnPlayerKilled
        OCAP_Session session = OCAP_Session.GetInstance();
        if (!session.IsRecording())
            return;

        // Check if this is a player — if so, OnPlayerKilled already handled it
        PlayerManager playerMgr = GetGame().GetPlayerManager();
        int playerId = playerMgr.GetPlayerIdFromControlledEntity(entity);
        if (playerId > 0)
            return;

        int frameNum = session.GetFrameNum();
        int victimId = session.GetEntityId(entity);
        if (victimId < 0)
            return;

        IEntity killerEnt = killer.GetInstigatorEntity();
        int killerId = -1;
        string weaponName = "Unknown";
        float distance = 0;

        if (killerEnt)
        {
            killerId = session.GetEntityId(killerEnt);
            if (entity)
                distance = vector.Distance(entity.GetOrigin(), killerEnt.GetOrigin());
        }

        string json = "[" + frameNum.ToString();
        json += ",\"killed\"";
        json += "," + victimId.ToString();
        json += ",[" + killerId.ToString() + ",\"" + weaponName + "\"]";
        json += "," + Math.Round(distance).ToString() + "]";

        session.QueueEvent(json);
    }

    protected void OnPlayerConnected(int playerId)
    {
        OCAP_Session session = OCAP_Session.GetInstance();
        if (!session.IsRecording())
            return;

        string playerName = GetGame().GetPlayerManager().GetPlayerName(playerId);
        int frameNum = session.GetFrameNum();

        string json = "[" + frameNum.ToString() + ",\"connected\",\"" + playerName + "\"]";
        session.QueueEvent(json);
    }

    protected void OnPlayerDisconnected(int playerId, KickCauseCode cause, int timeout)
    {
        OCAP_Session session = OCAP_Session.GetInstance();
        if (!session.IsRecording())
            return;

        string playerName = GetGame().GetPlayerManager().GetPlayerName(playerId);
        int frameNum = session.GetFrameNum();

        string json = "[" + frameNum.ToString() + ",\"disconnected\",\"" + playerName + "\"]";
        session.QueueEvent(json);
    }
}
```

**Step 2: Commit**

```bash
git add ocap-reforger-addon/Scripts/Game/OCAP/OCAP_EventManager.c
git commit -m "feat(addon): add EventManager with kill and connection tracking"
```

---

## Task 13: Reforger Addon — Main Game Mode Component (Entry Point)

**Files:**
- Create: `ocap-reforger-addon/Scripts/Game/OCAP/OCAP_GameModeComponent.c`

This is the entry point — a `SCR_BaseGameModeComponent` that wires everything together and starts/stops recording.

**Step 1: Write OCAP_GameModeComponent.c**

```csharp
// OCAP_GameModeComponent.c — Main entry point, wires all OCAP components

[ComponentEditorProps(category: "GameScripted/OCAP", description: "OCAP Recording Component")]
class OCAP_GameModeComponentClass : SCR_BaseGameModeComponentClass
{
}

class OCAP_GameModeComponent : SCR_BaseGameModeComponent
{
    [Attribute("1", UIWidgets.CheckBox, "Enable OCAP recording")]
    protected bool m_bEnabled;

    [Attribute("1.0", UIWidgets.Slider, "Capture interval in seconds", "0.5 2.0 0.1")]
    protected float m_fCaptureDelay;

    [Attribute("http://localhost:8080", UIWidgets.EditBox, "OCAP Receiver URL")]
    protected string m_sReceiverUrl;

    [Attribute("1", UIWidgets.CheckBox, "Auto-start recording on mission begin")]
    protected bool m_bAutoStart;

    [Attribute("1", UIWidgets.Slider, "Minimum player count to auto-start", "0 64 1")]
    protected int m_iMinPlayerCount;

    [Attribute("", UIWidgets.EditBox, "Mission tag (e.g. TvT, COOP)")]
    protected string m_sTag;

    protected ref OCAP_TransportService m_Transport;
    protected ref OCAP_CaptureManager m_CaptureManager;
    protected ref OCAP_EventManager m_EventManager;

    override void OnPostInit(IEntity owner)
    {
        super.OnPostInit(owner);

        if (!Replication.IsServer())
            return;

        if (!m_bEnabled)
        {
            Print("[OCAP] Disabled via settings", LogLevel.NORMAL);
            return;
        }

        // Initialize components
        m_Transport = new OCAP_TransportService();
        m_Transport.Init(m_sReceiverUrl);

        m_CaptureManager = new OCAP_CaptureManager();
        m_CaptureManager.Init(m_Transport, m_fCaptureDelay);

        m_EventManager = new OCAP_EventManager();
        m_EventManager.Init();

        if (m_bAutoStart)
        {
            // Delay start to let players connect
            GetGame().GetCallqueue().CallLater(TryAutoStart, 10000, true);
        }

        Print("[OCAP] Initialized on server", LogLevel.NORMAL);
    }

    protected void TryAutoStart()
    {
        OCAP_Session session = OCAP_Session.GetInstance();
        if (session.IsRecording())
        {
            GetGame().GetCallqueue().Remove(TryAutoStart);
            return;
        }

        PlayerManager playerMgr = GetGame().GetPlayerManager();
        if (playerMgr.GetPlayerCount() >= m_iMinPlayerCount)
        {
            GetGame().GetCallqueue().Remove(TryAutoStart);
            StartRecording();
        }
    }

    void StartRecording()
    {
        OCAP_Session session = OCAP_Session.GetInstance();
        session.StartRecording();

        // Get world/mission info
        string worldName = GetGame().GetWorldFile();
        // Extract map name from path (e.g. "{...}/Worlds/Everon/..." → "Everon")
        // Simplified — just use the raw world file for now
        string missionName = "Reforger_Mission";
        string missionAuthor = "";

        m_Transport.SendStart(worldName, missionName, missionAuthor, m_fCaptureDelay, m_sTag);
        m_CaptureManager.Start();

        Print("[OCAP] Recording started", LogLevel.NORMAL);
    }

    void StopRecording()
    {
        OCAP_Session session = OCAP_Session.GetInstance();
        if (!session.IsRecording())
            return;

        m_CaptureManager.Stop();

        // Send end mission event
        int endFrame = session.GetFrameNum();
        string endEvent = "[" + endFrame.ToString() + ",\"endMission\",[\"UNKNOWN\",\"Mission ended\"]]";
        session.QueueEvent(endEvent);

        // Flush remaining events
        string eventsJson = session.FlushEvents();
        if (!eventsJson.IsEmpty())
            m_Transport.SendEvents(session.GetSessionId(), eventsJson);

        m_Transport.SendEnd(session.GetSessionId(), endFrame, "missionEnd");
        session.StopRecording();

        Print("[OCAP] Recording stopped at frame " + endFrame.ToString(), LogLevel.NORMAL);
    }

    override void OnGameModeEnd(SCR_GameModeEndData endData)
    {
        super.OnGameModeEnd(endData);
        StopRecording();
    }

    override void OnPlayerDisconnected(int playerId, KickCauseCode cause, int timeout)
    {
        super.OnPlayerDisconnected(playerId, cause, timeout);

        // Auto-stop if all players left
        OCAP_Session session = OCAP_Session.GetInstance();
        if (!session.IsRecording())
            return;

        PlayerManager playerMgr = GetGame().GetPlayerManager();
        if (playerMgr.GetPlayerCount() <= 1) // 1 because this one hasn't fully left yet
            StopRecording();
    }

    void ~OCAP_GameModeComponent()
    {
        if (m_EventManager)
            m_EventManager.Cleanup();
        StopRecording();
    }
}
```

**Step 2: Commit**

```bash
git add ocap-reforger-addon/Scripts/Game/OCAP/OCAP_GameModeComponent.c
git commit -m "feat(addon): add GameModeComponent entry point with auto-start"
```

---

## Task 14: End-to-End Verification Checklist

This is a manual verification task — no code to write, but critical to validate the full pipeline.

**Step 1: Start the Go receiver**

```bash
cd ocap-reforger-receiver
go build -o ocap-receiver .
./ocap-receiver
```

**Step 2: Run the curl integration test (Task 7)**

```bash
bash test.sh
```

Verify:
- [ ] All endpoints return 200
- [ ] Receiver logs show session lifecycle
- [ ] A fallback `.json.gz` file is created
- [ ] Decompress and validate JSON structure:
  - [ ] Has `entities` array with 3 entries (2 units, 1 vehicle)
  - [ ] Each unit has `positions` array with 5 entries (frames 0-4)
  - [ ] Has `events` array with 2 entries (kill + connected)
  - [ ] Has `"Markers": []` (capital M)
  - [ ] Has correct `endFrame`, `captureDelay`, `worldName`

**Step 3: Test with OCAP2 web frontend (optional, if available)**

- Upload the generated `.json.gz` via the OCAP2 web UI or API
- Open the replay and verify:
  - [ ] Units appear on the map and move across frames
  - [ ] Kill event shows in the event log
  - [ ] Vehicle appears with crew

**Step 4: Document any issues found**

If the v1 format needs adjustments (coordinate mapping, missing fields), note them for follow-up.

**Step 5: Commit any fixes**

```bash
git add -A
git commit -m "fix(receiver): adjustments from end-to-end verification"
```

---

## Summary

| Task | Component | What it builds |
|------|-----------|----------------|
| 1 | Go receiver | Project scaffold + health check |
| 2 | Go receiver | Types + session store |
| 3 | Go receiver | HTTP handlers for all endpoints |
| 4 | Go receiver | V1 export builder (frame assembly) |
| 5 | Go receiver | Gzip compression + OCAP upload + disk fallback |
| 6 | Go receiver | Session timeout auto-cleanup |
| 7 | Go receiver | Integration test with curl |
| 8 | Reforger addon | Project scaffold + types |
| 9 | Reforger addon | TransportService (RestApi layer) |
| 10 | Reforger addon | Session state singleton |
| 11 | Reforger addon | CaptureManager (capture loop) |
| 12 | Reforger addon | EventManager (kills, connections) |
| 13 | Reforger addon | GameModeComponent (entry point) |
| 14 | Both | End-to-end verification |

Tasks 1–7 (Go receiver) can be built and tested independently. Tasks 8–13 (Reforger addon) can only be fully tested in Reforger Workbench or a dedicated server. Task 14 validates the full pipeline.
