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
# program, which macOS does not ship, and it writes no file.
#
#   bash bootstrap/bounded.sh <command> [argument ...]

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

command_pid=""
watch_pid=""
starting=1
pending=""

# Ends the command's group: a request, five seconds of grace, then no
# choice.
end_group() {
  kill -TERM -- "-$command_pid" 2>/dev/null
  for _ in 1 2 3 4 5; do
    kill -0 -- "-$command_pid" 2>/dev/null || return 0
    sleep 1
  done
  kill -KILL -- "-$command_pid" 2>/dev/null
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
  exit "$1"
}
trap 'cancelled 129' HUP
trap 'cancelled 130' INT
trap 'cancelled 143' TERM

set -m
"$@" &
command_pid=$!

# The watchdog looks once a second whether the command still runs, and it
# ends by itself within a second of the command.  The script therefore
# sends it no signal on the ordinary path, where a signal could arrive
# before the watchdog was ready for it and leave its timer behind.  Its
# status carries the outcome: 0 when the command ended first, 124 when the
# bound passed, and 125 when its timer failed.
(
  trap - HUP INT TERM
  set +m
  outcome=124
  SECONDS=0
  while [ "$SECONDS" -lt "$LIMIT_S" ]; do
    kill -0 "$command_pid" 2>/dev/null || exit 0
    if ! sleep 1; then
      outcome=125
      break
    fi
  done
  kill -0 "$command_pid" 2>/dev/null || exit 0
  end_group
  exit "$outcome"
) &
watch_pid=$!

starting=0
[ -z "$pending" ] || cancelled "$pending"

status=0
wait "$command_pid" || status=$?

outcome=0
wait "$watch_pid" 2>/dev/null || outcome=$?

case "$outcome" in
  124)
    echo "bounded: exceeded ${LIMIT_S}s: $*" >&2
    exit 124
    ;;
  125)
    # A timer that failed bounds nothing, so the command ended with it.
    echo "bounded: the timer of ${LIMIT_S}s failed, so the command was ended: $*" >&2
    exit 2
    ;;
esac
exit "$status"
