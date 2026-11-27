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
STARTUP_SECONDS=10

fail() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

[ -x "$BIN" ] || fail "no compiler service at $BIN (run: task build)"
[ -f "$FRONTEND/dist/index.html" ] || fail "no built frontend in $FRONTEND/dist (run: task playground-build)"

# A port the system hands out for this run.  The service shares a port it
# is told to use with any other process that asks for the same one, so a
# fixed port could answer from another worktree's service.
PORT="$(node -e 'const s = require("node:net").createServer().listen(0, "127.0.0.1", () => { console.log(s.address().port); s.close(); });')"
URL="http://127.0.0.1:$PORT/"

"$BIN" --root "$ROOT" --port "$PORT" > /dev/null &
service_pid=$!
# The service ends by the signal sent here, and its status must not
# replace the suite's.
trap 'kill "$service_pid" 2>/dev/null || true; wait "$service_pid" 2>/dev/null || true' EXIT

deadline=$((SECONDS + STARTUP_SECONDS))
answered=""
while [ "$SECONDS" -lt "$deadline" ]; do
  kill -0 "$service_pid" 2>/dev/null || fail "the service exited before it answered on port $PORT"
  if curl -fs -o /dev/null --max-time "$((deadline - SECONDS))" "$URL"; then
    answered=yes
    break
  fi
  sleep 0.2
done
[ -n "$answered" ] || fail "the service did not answer on $URL within $STARTUP_SECONDS seconds"

cd "$FRONTEND"
VITE_PLAYGROUND_URL="$URL" npm run test:browser
