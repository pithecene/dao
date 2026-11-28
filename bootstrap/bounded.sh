#!/usr/bin/env bash
# bounded.sh — Run one command under a wall-clock bound, so a compile that
# never ends fails the task instead of holding a processor until a person
# finds it.
#
# The bound is BOOTSTRAP_SECONDS, 600 by default, the bound of
# audit_closure.sh's probes.  A command that outlasts it is ended with its
# whole process group, and the script exits 124, the status of `timeout`.
# The script needs no `timeout` program, which macOS does not ship.
#
#   bash bootstrap/bounded.sh <command> [argument ...]

set -uo pipefail

if [ "$#" -eq 0 ]; then
  echo "usage: bash bootstrap/bounded.sh <command> [argument ...]" >&2
  exit 2
fi

LIMIT_S="${BOOTSTRAP_SECONDS:-600}"

# Job control gives the command a process group of its own, so the bound
# ends the programs that it started as well.
set -m
"$@" &
command_pid=$!

expired="$(mktemp)"
rm -f "$expired"
(
  sleep "$LIMIT_S" &
  sleep_pid=$!
  trap 'kill "$sleep_pid" 2>/dev/null; exit 0' TERM
  wait "$sleep_pid"
  : > "$expired"
  kill -TERM -- "-$command_pid" 2>/dev/null
  # A program that ignores the request gets five seconds, then no choice.
  for _ in 1 2 3 4 5; do
    kill -0 -- "-$command_pid" 2>/dev/null || exit 0
    sleep 1
  done
  kill -KILL -- "-$command_pid" 2>/dev/null
) &
watch_pid=$!

status=0
wait "$command_pid" || status=$?

if [ -e "$expired" ]; then
  rm -f "$expired"
  wait "$watch_pid" 2>/dev/null
  echo "bounded: exceeded ${LIMIT_S}s: $*" >&2
  exit 124
fi

kill -TERM "$watch_pid" 2>/dev/null
wait "$watch_pid" 2>/dev/null
exit "$status"
