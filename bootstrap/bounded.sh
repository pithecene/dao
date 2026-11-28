#!/usr/bin/env bash
# bounded.sh — Run one command under a wall-clock bound, so a compile that
# never ends fails the task instead of holding a processor until a person
# finds it.
#
# The bound is BOOTSTRAP_SECONDS, 600 by default, the bound of
# audit_closure.sh's probes.  A command that outlasts it is ended with its
# whole process group, and the script exits 124, the status of `timeout`.
# A bound that is no whole number of seconds above zero is refused before
# the command starts, with status 2.  The script needs no `timeout`
# program, which macOS does not ship.
#
#   bash bootstrap/bounded.sh <command> [argument ...]

set -uo pipefail

if [ "$#" -eq 0 ]; then
  echo "usage: bash bootstrap/bounded.sh <command> [argument ...]" >&2
  exit 2
fi

LIMIT_S="${BOOTSTRAP_SECONDS:-600}"
case "$LIMIT_S" in
  ''|0*|*[!0-9]*)
    echo "bounded: BOOTSTRAP_SECONDS must be a whole number of seconds above zero, got '$LIMIT_S'" >&2
    exit 2
    ;;
esac

# The record of an expiry lies in a directory that only this run owns, and
# it exists before the command starts, so a failure to make it starts
# nothing.
STATE="$(mktemp -d)" || {
  echo "bounded: no temporary directory for the bound's state" >&2
  exit 2
}

command_pid=""
watch_pid=""

# Ends the command's group, at once when `now` is 1 and after five seconds
# of grace otherwise.
end_group() {
  local now="$1"
  [ -n "$command_pid" ] || return 0
  if [ "$now" -ne 1 ]; then
    kill -TERM -- "-$command_pid" 2>/dev/null
    for _ in 1 2 3 4 5; do
      kill -0 -- "-$command_pid" 2>/dev/null || return 0
      sleep 1
    done
  fi
  kill -KILL -- "-$command_pid" 2>/dev/null
}

# Job control gives the command a process group of its own, so the bound
# ends the programs that it started as well.  The same separation keeps a
# signal to this script from the command, so the script passes it on.
cancelled() {
  local status="$1"
  [ -n "$watch_pid" ] && kill -TERM "$watch_pid" 2>/dev/null
  end_group 0
  rm -rf "$STATE"
  exit "$status"
}
trap 'cancelled 129' HUP
trap 'cancelled 130' INT
trap 'cancelled 143' TERM

set -m
"$@" &
command_pid=$!

(
  trap - HUP INT TERM
  sleep "$LIMIT_S" &
  sleep_pid=$!
  trap 'kill "$sleep_pid" 2>/dev/null; exit 0' TERM
  if wait "$sleep_pid"; then
    : > "$STATE/expired"
  else
    # A timer that failed bounds nothing, so the command ends with it.
    : > "$STATE/timer-failed"
  fi
  trap - TERM
  kill -TERM -- "-$command_pid" 2>/dev/null
  for _ in 1 2 3 4 5; do
    kill -0 -- "-$command_pid" 2>/dev/null || exit 0
    sleep 1
  done
  kill -KILL -- "-$command_pid" 2>/dev/null
) &
watch_pid=$!

status=0
wait "$command_pid" || status=$?

if [ -e "$STATE/expired" ] || [ -e "$STATE/timer-failed" ]; then
  # The watchdog finishes its escalation before the script reports.
  wait "$watch_pid" 2>/dev/null
  if [ -e "$STATE/expired" ]; then
    rm -rf "$STATE"
    echo "bounded: exceeded ${LIMIT_S}s: $*" >&2
    exit 124
  fi
  rm -rf "$STATE"
  echo "bounded: the timer of ${LIMIT_S}s failed, so the command was ended: $*" >&2
  exit 2
fi

kill -TERM "$watch_pid" 2>/dev/null
wait "$watch_pid" 2>/dev/null
rm -rf "$STATE"
exit "$status"
