#!/usr/bin/env bash
# bounded.sh — Run one command under a wall-clock bound, so a compile that
# never ends fails the task instead of holding a processor until a person
# finds it.
#
# The bound is BOOTSTRAP_SECONDS, 600 by default, the bound of
# audit_closure.sh's probes.  A command that outlasts it is ended with its
# whole process group, and the script exits 124, the status of `timeout`.
# A bound that is no whole number of seconds from 1 to 86400 is refused
# before the command starts, with status 2.  The script needs no `timeout`
# program, which macOS does not ship.
#
#   bash bootstrap/bounded.sh <command> [argument ...]

# The script reads each status itself, so a shell option that the
# environment carries cannot end it between two steps.
set +e
set -uo pipefail

if [ "$#" -eq 0 ]; then
  echo "usage: bash bootstrap/bounded.sh <command> [argument ...]" >&2
  exit 2
fi

LIMIT_S="${BOOTSTRAP_SECONDS:-600}"
case "$LIMIT_S" in
  ''|0*|*[!0-9]*|??????*)
    LIMIT_S_VALID=0
    ;;
  *)
    LIMIT_S_VALID=1
    [ "$LIMIT_S" -le 86400 ] || LIMIT_S_VALID=0
    ;;
esac
if [ "$LIMIT_S_VALID" -ne 1 ]; then
  echo "bounded: BOOTSTRAP_SECONDS must be a whole number of seconds from 1 to 86400, got '$LIMIT_S'" >&2
  exit 2
fi

STATE=""
command_pid=""
watch_pid=""
starting=1
pending=""

# Ends the command's group: a request, five seconds of grace, then no
# choice.  The group may be gone at any step, which is the aim.
end_group() {
  kill -TERM -- "-$command_pid" 2>/dev/null
  for _ in 1 2 3 4 5; do
    kill -0 -- "-$command_pid" 2>/dev/null || return 0
    sleep 1
  done
  kill -KILL -- "-$command_pid" 2>/dev/null
  return 0
}

# Job control gives the command a process group of its own, so the bound
# ends the programs that it started as well.  The same separation keeps a
# signal to this script from the command, so the script passes it on.  A
# signal that arrives while the script starts its children waits until
# both are recorded, so neither is left running.
cancelled() {
  if [ "$starting" -eq 1 ]; then
    pending="$1"
    return 0
  fi
  [ -n "$watch_pid" ] && kill -TERM -- "-$watch_pid" 2>/dev/null
  [ -n "$command_pid" ] && end_group
  [ -n "$STATE" ] && rm -rf "$STATE"
  exit "$1"
}
trap 'cancelled 129' HUP
trap 'cancelled 130' INT
trap 'cancelled 143' TERM

# The script tells the watchdog that the command ended by a file in a
# directory that only this run owns.  A process number cannot say it,
# because the system gives a finished command's number to another program.
STATE="$(mktemp -d)" || {
  echo "bounded: no temporary directory for the bound's state" >&2
  exit 2
}

set -m
"$@" &
command_pid=$!

# The watchdog sleeps one second LIMIT_S times, so it never ends the
# command before the bound, and it ends by itself within a second of the
# command.  The script therefore sends it no signal on the ordinary path.
# Its status carries the outcome: 0 when the command ended first, 124 when
# the bound passed, and 125 when its timer failed.
(
  trap - HUP INT TERM
  set +m
  outcome=124
  slept=0
  while [ "$slept" -lt "$LIMIT_S" ]; do
    [ -e "$STATE/done" ] && exit 0
    if ! sleep 1; then
      outcome=125
      break
    fi
    slept=$((slept + 1))
  done
  [ -e "$STATE/done" ] && exit 0
  end_group
  exit "$outcome"
) &
watch_pid=$!

starting=0
[ -z "$pending" ] || cancelled "$pending"

status=0
wait "$command_pid" || status=$?

# A watchdog that cannot learn of the end is stopped instead.
: > "$STATE/done" || kill -TERM -- "-$watch_pid" 2>/dev/null
outcome=0
wait "$watch_pid" 2>/dev/null || outcome=$?
rm -rf "$STATE"

case "$outcome" in
  0)
    exit "$status"
    ;;
  124)
    echo "bounded: exceeded ${LIMIT_S}s: $*" >&2
    exit 124
    ;;
  125)
    # A timer that failed bounds nothing, so the command ended with it.
    echo "bounded: the timer of ${LIMIT_S}s failed, so the command was ended: $*" >&2
    exit 2
    ;;
  *)
    # Only a watchdog that ended cleanly vouches for the command's status.
    echo "bounded: the watchdog ended with status ${outcome}, so the bound is unproven: $*" >&2
    exit 2
    ;;
esac
