package main

import (
	"encoding/json"
	"log"
	"sort"
)

func BuildExport(sess *Session, endFrame int) *V1Export {
	sess.mu.Lock()
	defer sess.mu.Unlock()

	// Find max entity ID to size the entities array
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

	// Build events — unmarshal raw JSON to interface{}
	events := make([]interface{}, len(sess.Events))
	for i, raw := range sess.Events {
		var evt interface{}
		if err := json.Unmarshal(raw, &evt); err != nil {
			log.Printf("WARN: failed to parse event: %v", err)
			events[i] = nil
			continue
		}
		events[i] = evt
	}

	return &V1Export{
		AddonVersion:     "0.1.0",
		ExtensionVersion: "0.1.0",
		ExtensionBuild:   "reforger",
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
// Strips the entity ID from the front of each frame's raw data.
// Missing frames before first data get empty placeholders.
// Missing frames after first data are filled with the last known state.
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
		if lastState == nil {
			// No data yet for this entity — emit empty placeholder
			positions = append(positions, []interface{}{})
		} else {
			// Strip the ID from the front of the array
			// Input: [id, [x,y,z], bearing, ...]
			// Output: [[x,y,z], bearing, ...]
			var arr []json.RawMessage
			if err := json.Unmarshal(lastState, &arr); err != nil {
				log.Printf("WARN: failed to parse position state for entity %d: %v", entityID, err)
				positions = append(positions, []interface{}{})
				continue
			}
			if len(arr) > 1 {
				stripped := make([]interface{}, len(arr)-1)
				for i := 1; i < len(arr); i++ {
					var v interface{}
					if err := json.Unmarshal(arr[i], &v); err != nil {
						log.Printf("WARN: failed to parse position field %d for entity %d: %v", i, entityID, err)
					}
					stripped[i-1] = v
				}
				positions = append(positions, stripped)
			} else {
				positions = append(positions, []interface{}{})
			}
		}
	}

	return positions
}
