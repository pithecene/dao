#!/usr/bin/env bash
# The bootstrap crossing (Task 45): the host compiles the bootstrap
# compiler (stage 1); that compiler compiles itself (stage 2); the
# stage-2 compiler compiles itself again (stage 3).  Stage 2 and stage 3
# must emit the same module -- the fixed point -- and every bootstrap
# suite, compiled by the stage-2 compiler, must pass.
#
#   bootstrap/stage2.sh <build dir>
#
# STAGE1=<binary> runs the crossing from a stage-1 compiler built
# elsewhere (the same bootstrap/driver/driver.gen.dao) instead of
# building one with `daoc build`.
set -u
BUILD_DIR="${1:-build/debug}"
cd "$(git rev-parse --show-toplevel)"
DAOC="$BUILD_DIR/compiler/driver/daoc"
RUNTIME="$BUILD_DIR/runtime/core/libdao_runtime.a"
OUT=bootstrap/driver/out
LIMIT_KB="${STAGE_MEMORY_KB:-8388608}"   # per compiler run: 8 GiB of address space
mkdir -p "$OUT"
bash bootstrap/assemble.sh > /dev/null || { echo "stage2: assemble failed"; exit 1; }

# The recursive-descent passes recurse once per nesting level, and an
# unoptimized frame is large: a long else-if chain outgrows the default
# 8 MiB stack.  The limit is this script's own, not the shell's.
ulimit -s unlimited

# A manifest: where the module goes, the prelude as the host loads it
# (every module under stdlib/core and stdlib/io, in path order;
# CONTRACT_MODULE_SYSTEM.md §7), then the program's files.
manifest() {  # manifest <output .ll> <source>...
  local output="$1"; shift
  { echo "output $output"
    ls stdlib/core/*.dao stdlib/io/*.dao | LC_ALL=C sort | sed 's/^/prelude /'
    for f in "$@"; do echo "source $f"; done
  } > "$OUT/.manifest"
}

compile_with() {  # compile_with <compiler> <output .ll> <source>...
  local compiler="$1" output="$2"; shift 2
  rm -f "$output"
  manifest "$output" "$@"
  ( ulimit -v "$LIMIT_KB"; "$compiler" ) > "$output.log" 2>&1 && [ -f "$output" ]
}

link() {  # link <.ll> <binary>
  clang -O0 -c -x ir "$1" -o "$1.o" 2> "$1.clang" && clang "$1.o" "$RUNTIME" -o "$2" 2>> "$1.clang"
}

if [ -n "${STAGE1:-}" ]; then
  echo "stage 1: $STAGE1"
else
  echo "stage 1: the host builds the bootstrap compiler"
  "$DAOC" build bootstrap/driver/driver.gen.dao > "$OUT/stage1.log" 2>&1 || { echo "stage 1 failed (see $OUT/stage1.log)"; exit 1; }
  STAGE1=bootstrap/driver/driver.gen
fi

echo "stage 2: the bootstrap compiler compiles itself"
compile_with "$STAGE1" "$OUT/stage2.ll" bootstrap/driver/driver.gen.dao || { echo "stage 2 failed (see $OUT/stage2.ll.log)"; exit 1; }
link "$OUT/stage2.ll" "$OUT/stage2" || { echo "stage 2 does not link (see $OUT/stage2.ll.clang)"; exit 1; }

echo "stage 3: the stage-2 compiler compiles itself"
compile_with "$OUT/stage2" "$OUT/stage3.ll" bootstrap/driver/driver.gen.dao || { echo "stage 3 failed (see $OUT/stage3.ll.log)"; exit 1; }

if cmp -s "$OUT/stage2.ll" "$OUT/stage3.ll"; then
  echo "fixed point: stage 2 and stage 3 emit the same module"
else
  echo "no fixed point: stage 2 and stage 3 differ"; exit 1
fi

echo "the suites, compiled by the stage-2 compiler"
# The llvm suite writes the IR of each fixture it lowers, which
# validate_ir.sh then compiles and runs.
rm -rf bootstrap/llvm/out && mkdir -p bootstrap/llvm/out
failures=0
for p in lexer parser graph resolver typecheck hir mir llvm; do
  if ! compile_with "$OUT/stage2" "$OUT/$p.ll" "bootstrap/$p/$p.gen.dao"; then
    echo "  $p: did not compile (see $OUT/$p.ll.log)"; failures=$((failures + 1)); continue
  fi
  if ! link "$OUT/$p.ll" "$OUT/$p"; then
    echo "  $p: does not link (see $OUT/$p.ll.clang)"; failures=$((failures + 1)); continue
  fi
  "$OUT/$p" > "$OUT/$p.run" 2>&1
  status=$?
  result="$(grep -oE '[0-9]+ / [0-9]+ (tests )?passed' "$OUT/$p.run" | tail -1)"
  echo "  $p: ${result:-no result}$([ "$status" -ne 0 ] && echo ", exit $status")"
  passed="$(echo "$result" | awk '{print $1}')"; total="$(echo "$result" | awk '{print $3}')"
  if [ "$status" -ne 0 ] || [ -z "$result" ] || [ "$passed" != "$total" ]; then
    failures=$((failures + 1))
  fi
done
bash bootstrap/validate_ir.sh "$BUILD_DIR" > "$OUT/validate_ir.log" 2>&1
validate_status=$?
sed 's/^/  /' "$OUT/validate_ir.log"
[ "$validate_status" -eq 0 ] || failures=$((failures + 1))
if [ "$failures" -ne 0 ]; then
  echo "$failures suite(s) fail under stage 2"; exit 1
fi
echo "bootstrapped: every suite passes under stage 2"
