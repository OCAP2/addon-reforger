#!/bin/bash
set -e

cd "$(dirname "$0")"

echo "=== Building receiver ==="
go build -o ocap-receiver .

echo "=== Starting receiver ==="
OCAP_WEB_URL="http://localhost:19999" ./ocap-receiver &
SERVER_PID=$!
sleep 1

# Ensure cleanup on exit
cleanup() {
    kill $SERVER_PID 2>/dev/null || true
    rm -f ocap-receiver fallback_*.json.gz fallback_*.json
}
trap cleanup EXIT

BASE="http://localhost:8080"

echo "=== Health check ==="
HEALTH=$(curl -s "$BASE/healthcheck")
echo "Health: $HEALTH"

echo "=== Starting session ==="
SESSION_ID=$(curl -s -X POST "$BASE/api/session/start" \
  -H "Content-Type: application/json" \
  -d '{
    "worldName": "Everon",
    "missionName": "Test Mission",
    "missionAuthor": "TestAuthor",
    "captureDelay": 1.0,
    "tag": "test",
    "startTime": "2026-02-14T19:00:00Z"
  }' | python3 -c "import sys,json; print(json.load(sys.stdin)['sessionId'])")
echo "Session ID: $SESSION_ID"

echo "=== Sending entities ==="
curl -sf -X POST "$BASE/api/session/entities" \
  -H "Content-Type: application/json" \
  -d "{
    \"sessionId\": \"$SESSION_ID\",
    \"entities\": [
      {\"id\":0,\"type\":\"unit\",\"name\":\"Player1\",\"group\":\"Alpha\",\"side\":\"WEST\",\"isPlayer\":1,\"role\":\"Rifleman\",\"startFrameNum\":0},
      {\"id\":1,\"type\":\"unit\",\"name\":\"Enemy1\",\"group\":\"Bravo\",\"side\":\"EAST\",\"isPlayer\":0,\"role\":\"Rifleman\",\"startFrameNum\":0},
      {\"id\":2,\"type\":\"vehicle\",\"name\":\"HMMWV\",\"side\":\"WEST\",\"class\":\"car\",\"isPlayer\":0,\"startFrameNum\":0}
    ]
  }"

echo "=== Sending 5 frames ==="
for FRAME in 0 1 2 3 4; do
  X=$((3000 + FRAME * 10))
  curl -sf -X POST "$BASE/api/session/frames" \
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
curl -sf -X POST "$BASE/api/session/events" \
  -H "Content-Type: application/json" \
  -d "{
    \"sessionId\": \"$SESSION_ID\",
    \"events\": [
      [3, \"killed\", 1, [0, \"M4A1\"], 1000.0],
      [0, \"connected\", \"Player1\"]
    ]
  }"

echo "=== Ending session ==="
curl -sf -X POST "$BASE/api/session/end" \
  -H "Content-Type: application/json" \
  -d "{
    \"sessionId\": \"$SESSION_ID\",
    \"endFrame\": 4,
    \"endReason\": \"missionEnd\"
  }"

echo ""
echo "=== Waiting for async export (retries + fallback) ==="
sleep 8

echo "=== Validating output ==="

# Find the fallback file
FALLBACK=$(ls fallback_Test_Mission_*.json.gz 2>/dev/null | head -1)
if [ -z "$FALLBACK" ]; then
  echo "FAIL: No fallback file created"
  exit 1
fi
echo "Found: $FALLBACK"

# Decompress
JSONFILE="${FALLBACK%.gz}"
gunzip -k "$FALLBACK"

# Validate structure
echo "--- Checking entities ---"
ENTITY_COUNT=$(python3 -c "import json; d=json.load(open('$JSONFILE')); print(len([e for e in d['entities'] if e is not None]))")
echo "Entity count (non-null): $ENTITY_COUNT"
[ "$ENTITY_COUNT" = "3" ] || { echo "FAIL: Expected 3 entities, got $ENTITY_COUNT"; exit 1; }

echo "--- Checking unit positions ---"
POS_COUNT=$(python3 -c "import json; d=json.load(open('$JSONFILE')); print(len(d['entities'][0]['positions']))")
echo "Player1 position count: $POS_COUNT"
[ "$POS_COUNT" = "5" ] || { echo "FAIL: Expected 5 positions, got $POS_COUNT"; exit 1; }

echo "--- Checking events ---"
EVENT_COUNT=$(python3 -c "import json; d=json.load(open('$JSONFILE')); print(len(d['events']))")
echo "Event count: $EVENT_COUNT"
[ "$EVENT_COUNT" = "2" ] || { echo "FAIL: Expected 2 events, got $EVENT_COUNT"; exit 1; }

echo "--- Checking Markers (capital M) ---"
HAS_MARKERS=$(python3 -c "import json; d=json.load(open('$JSONFILE')); print('Markers' in d)")
echo "Has Markers key: $HAS_MARKERS"
[ "$HAS_MARKERS" = "True" ] || { echo "FAIL: Missing 'Markers' key"; exit 1; }

echo "--- Checking metadata ---"
python3 -c "
import json
d = json.load(open('$JSONFILE'))
assert d['worldName'] == 'Everon', f\"worldName: {d['worldName']}\"
assert d['missionName'] == 'Test Mission', f\"missionName: {d['missionName']}\"
assert d['endFrame'] == 4, f\"endFrame: {d['endFrame']}\"
assert d['captureDelay'] == 1.0, f\"captureDelay: {d['captureDelay']}\"
print('Metadata OK')
"

echo "--- Checking position format ---"
python3 -c "
import json
d = json.load(open('$JSONFILE'))
pos = d['entities'][0]['positions'][0]
# Should be: [[x,y,z], bearing, lifeState, vehicleId, name, isPlayer, role]
assert isinstance(pos[0], list) and len(pos[0]) == 3, f'pos[0] not [x,y,z]: {pos[0]}'
assert isinstance(pos[1], (int, float)), f'bearing not number: {pos[1]}'
print(f'Position format OK: {pos}')
"

echo ""
echo "=== ALL TESTS PASSED ==="
