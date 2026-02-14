package main

import (
	"log"
	"time"
	"net/http"
)

func main() {
	cfg := LoadConfig()
	store := NewSessionStore()

	timeout, err := time.ParseDuration(cfg.SessionTimeout)
	if err != nil {
		log.Printf("WARN: invalid SESSION_TIMEOUT %q, using default 3h: %v", cfg.SessionTimeout, err)
		timeout = 3 * time.Hour
	} else if timeout == 0 {
		timeout = 3 * time.Hour
	}
	store.StartCleanup(5*time.Minute, timeout, cfg)
	h := NewHandler(store, cfg)

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
