#!/usr/bin/env bash
# Playground development loop for the worktree this script lives in:
#   - the compiler service on :8090, restarted whenever its binary is
#     rebuilt (mtime watch, 2 s poll, waits for the linker to finish)
#   - the Vite dev server on :5173 with HMR, proxying /api to :8090
# Stop both with Ctrl-C (or SIGTERM).  `task playground-dev` runs this;
# it can also be detached: `nohup tools/playground/dev.sh > dev.log &`.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${DAO_BUILD_DIR:-$ROOT/build/debug}"
BIN="$BUILD_DIR/tools/playground/compiler_service/dao_playground"
PORT="${DAO_PLAYGROUND_PORT:-8090}"

log() { printf '%s %s\n' "$(date +%T)" "$*"; }

backend_pid=""

stop_backend() {
  if [ -n "$backend_pid" ] && kill -0 "$backend_pid" 2>/dev/null; then
    kill "$backend_pid" 2>/dev/null
    wait "$backend_pid" 2>/dev/null
  fi
  backend_pid=""
}

# Restart the service each time the binary's mtime settles on a new value.
watch_backend() {
  local last="" now settled
  while true; do
    if [ -x "$BIN" ]; then
      now="$(stat -c %Y "$BIN" 2>/dev/null || echo 0)"
      if [ "$now" != "$last" ]; then
        sleep 2
        settled="$(stat -c %Y "$BIN" 2>/dev/null || echo 0)"
        if [ "$settled" = "$now" ]; then
          [ -n "$backend_pid" ] && log "backend: binary changed, restarting"
          stop_backend
          log "backend: starting on :$PORT"
          "$BIN" --root "$ROOT" --port "$PORT" &
          backend_pid=$!
          last="$now"
        fi
      fi
    fi
    sleep 2
  done
}

watch_backend &
watcher_pid=$!

cd "$ROOT/tools/playground/frontend" || exit 1
npx vite --clearScreen false &
vite_pid=$!

cleanup() {
  kill "$watcher_pid" "$vite_pid" 2>/dev/null
  pkill -P "$watcher_pid" 2>/dev/null
  exit 0
}
trap cleanup INT TERM
wait
