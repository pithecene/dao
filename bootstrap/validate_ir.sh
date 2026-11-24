#!/usr/bin/env bash
# Mechanical validation of the bootstrap LLVM backend's output.
#
# The bootstrap's LLVM test program writes the IR of every fixture it
# lowers to <ir-dir>/<test>.ll, and for fixtures with a known result a
# <test>.exit file holding the expected exit code. This script proves
# each .ll is accepted by LLVM (clang compiles it), links fixtures with
# an .exit file against the runtime, runs them, compares exit codes, and
# verifies minimum expected fixture counts.
#
#   bootstrap/validate_ir.sh [build-dir] [ir-dir]
set -u
BUILD_DIR="${1:-build/debug}"
OUT="${2:-bootstrap/llvm/out}"
RUNTIME="$BUILD_DIR/runtime/core/libdao_runtime.a"
CLANG="${CLANG:-clang}"
MIN_CHECKED="${VALIDATE_IR_MIN_CHECKED:-39}"
MIN_RAN="${VALIDATE_IR_MIN_RAN:-32}"

if ! command -v "$CLANG" > /dev/null; then
  echo "validate_ir: $CLANG not found"; exit 1
fi
if [ ! -f "$RUNTIME" ]; then
  echo "validate_ir: runtime library not found at $RUNTIME"; exit 1
fi
if [ ! -d "$OUT" ] || ! ls "$OUT"/*.ll > /dev/null 2>&1; then
  echo "validate_ir: no IR under $OUT (run the bootstrap llvm suite first)"; exit 1
fi

checked=0; accepted=0; ran=0; failures=0
WORK="$(mktemp -d)" && [ -n "$WORK" ] || { echo "validate_ir: cannot create a scratch directory"; exit 1; }
trap 'rm -rf "$WORK"' EXIT

for ll in "$OUT"/*.ll; do
  name="$(basename "$ll" .ll)"
  checked=$((checked + 1))
  if ! "$CLANG" -c -x ir "$ll" -o "$WORK/$name.o" 2> "$WORK/$name.err"; then
    echo "FAIL $name: LLVM rejected the IR"
    sed 's/^/    /' "$WORK/$name.err" | head -8
    failures=$((failures + 1))
    continue
  fi
  accepted=$((accepted + 1))
  expect_file="$OUT/$name.exit"
  if [ -f "$expect_file" ]; then
    expected="$(cat "$expect_file")"
    if ! "$CLANG" "$WORK/$name.o" "$RUNTIME" -o "$WORK/$name" 2> "$WORK/$name.link"; then
      echo "FAIL $name: link failed"; sed 's/^/    /' "$WORK/$name.link" | head -5
      failures=$((failures + 1)); continue
    fi
    "$WORK/$name" > /dev/null 2>&1; actual=$?
    ran=$((ran + 1))
    if [ "$actual" -ne "$expected" ]; then
      echo "FAIL $name: expected exit $expected, got $actual"
      failures=$((failures + 1))
    fi
  fi
done

if [ "$checked" -lt "$MIN_CHECKED" ]; then
  echo "FAIL validate_ir: expected at least $MIN_CHECKED IR files under $OUT, found $checked"
  failures=$((failures + 1))
fi
if [ "$ran" -lt "$MIN_RAN" ]; then
  echo "FAIL validate_ir: expected at least $MIN_RAN executed fixtures under $OUT, ran $ran"
  failures=$((failures + 1))
fi

echo "validate_ir: $checked IR files checked, $accepted accepted by LLVM, $ran executed, $failures failure(s)"
[ "$failures" -eq 0 ]
