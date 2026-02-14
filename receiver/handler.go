package main

import (
	"encoding/json"
	"log"
	"net/http"
)

type Handler struct {
	store *SessionStore
	cfg   Config
}

func NewHandler(store *SessionStore, cfg Config) *Handler {
	return &Handler{store: store, cfg: cfg}
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
	// Atomically take the session to prevent double-export with cleanup goroutine
	sess := h.store.TakeSession(req.SessionID)
	if sess == nil {
		http.Error(w, "session not found", http.StatusNotFound)
		return
	}
	log.Printf("Session %s: ending at frame %d (%s)", sess.ID[:8], req.EndFrame, req.EndReason)

	// Export asynchronously so the HTTP response returns quickly
	go func() {
		export := BuildExport(sess, req.EndFrame)
		if err := CompressAndUpload(export, sess.Metadata, req.EndFrame, h.cfg); err != nil {
			log.Printf("Session %s: upload failed: %v", sess.ID[:8], err)
			if err := SaveToDisk(export, sess.Metadata); err != nil {
				log.Printf("Session %s: disk save also failed: %v", sess.ID[:8], err)
			}
		}
	}()

	w.WriteHeader(http.StatusOK)
}
