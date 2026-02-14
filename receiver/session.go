package main

import (
	"encoding/json"
	"log"
	"sync"
	"time"

	"github.com/google/uuid"
)

type Session struct {
	ID           string
	Metadata     StartRequest
	Entities     map[int]*EntityDef
	Frames       map[int]map[int]json.RawMessage // frameNum -> entityID -> state
	Events       []json.RawMessage
	CreatedAt    time.Time
	LastActivity time.Time
	mu           sync.Mutex
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

// TakeSession atomically removes and returns a session from the store.
// Returns nil if the session doesn't exist (already taken by another goroutine).
func (s *SessionStore) TakeSession(id string) *Session {
	s.mu.Lock()
	sess, ok := s.sessions[id]
	if ok {
		delete(s.sessions, id)
	}
	s.mu.Unlock()
	if !ok {
		return nil
	}
	return sess
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

	for _, raw := range units {
		var arr []json.RawMessage
		if err := json.Unmarshal(raw, &arr); err != nil {
			log.Printf("WARN: failed to parse unit frame data: %v", err)
			continue
		}
		if len(arr) > 0 {
			var id int
			if err := json.Unmarshal(arr[0], &id); err != nil {
				log.Printf("WARN: failed to parse unit ID: %v", err)
				continue
			}
			frame[id] = raw
		}
	}
	for _, raw := range vehicles {
		var arr []json.RawMessage
		if err := json.Unmarshal(raw, &arr); err != nil {
			log.Printf("WARN: failed to parse vehicle frame data: %v", err)
			continue
		}
		if len(arr) > 0 {
			var id int
			if err := json.Unmarshal(arr[0], &id); err != nil {
				log.Printf("WARN: failed to parse vehicle ID: %v", err)
				continue
			}
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
				// Atomically take the session to prevent double-export
				sess := s.TakeSession(id)
				if sess == nil {
					continue
				}
				log.Printf("Session %s timed out, auto-exporting", id[:8])
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
					if err := SaveToDisk(export, sess.Metadata); err != nil {
						log.Printf("Session %s: timeout disk save also failed: %v", id[:8], err)
					}
				}
			}
		}
	}()
}
