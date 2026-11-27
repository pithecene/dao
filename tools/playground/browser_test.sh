#!/usr/bin/env bash
# Runs the frontend's browser suite against the worktree this script lives
# in: starts the compiler service on a port of its own, builds the suite
# against that address, runs it, and stops the service whatever the result.
# `task playground-browser-test` runs this after building both sides.
set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${DAO_BUILD_DIR:-$ROOT/build/debug}"
BIN="$BUILD_DIR/tools/playground/compiler_service/dao_playground"
FRONTEND="$ROOT/tools/playground/frontend"
PORT="${DAO_PLAYGROUND_TEST_PORT:-8099}"
URL="http://127.0.0.1:$PORT/"

fail() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

[ -x "$BIN" ] || fail "no compiler service at $BIN (run: task build)"
[ -f "$FRONTEND/dist/index.html" ] || fail "no built frontend in $FRONTEND/dist (run: task playground-build)"

"$BIN" --root "$ROOT" --port "$PORT" > /dev/null &
service_pid=$!
trap 'kill "$service_pid" 2>/dev/null; wait "$service_pid" 2>/dev/null' EXIT

# The service exits at once when the port is taken, so its being alive is
# checked before each request: an answer then comes from this service and
# not from whatever else holds the port.
answered=""
for _ in $(seq 1 50); do
  sleep 0.2
  kill -0 "$service_pid" 2>/dev/null || fail "the service exited before it answered (is port $PORT in use?)"
  if curl -fs -o /dev/null "$URL"; then
    answered=yes
    break
  fi
done
[ -n "$answered" ] || fail "the service did not answer on $URL within ten seconds"

cd "$FRONTEND"
VITE_PLAYGROUND_URL="$URL" npm run test:browser
