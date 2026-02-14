package main

import "encoding/json"

// --- Inbound request types (from Reforger addon) ---

type StartRequest struct {
	WorldName     string  `json:"worldName"`
	MissionName   string  `json:"missionName"`
	MissionAuthor string  `json:"missionAuthor"`
	CaptureDelay  float64 `json:"captureDelay"`
	Tag           string  `json:"tag"`
	StartTime     string  `json:"startTime"`
}

type StartResponse struct {
	SessionID string `json:"sessionId"`
}

type EntityDef struct {
	ID            int    `json:"id"`
	Type          string `json:"type"`
	Name          string `json:"name"`
	Group         string `json:"group,omitempty"`
	Side          string `json:"side"`
	IsPlayer      int    `json:"isPlayer"`
	Role          string `json:"role,omitempty"`
	Class         string `json:"class,omitempty"`
	StartFrameNum int    `json:"startFrameNum"`
}

type EntitiesRequest struct {
	SessionID string      `json:"sessionId"`
	Entities  []EntityDef `json:"entities"`
}

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
	Markers          []interface{} `json:"Markers"`
}
