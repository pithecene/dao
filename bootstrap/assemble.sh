#!/usr/bin/env bash
# assemble.sh — Compose each bootstrap program from explicit source
# files: the shared base, the library fragments a program builds on,
# and finally that program's own test runner or entry point.
#
# Run from repository root:
#   bash bootstrap/assemble.sh [output-root]
#
# When output-root is omitted, assembled files land under `bootstrap/`
# (`bootstrap/<sub>/<name>.gen.dao`). Crossing runs pass a per-run
# directory so concurrent invocations never overwrite shared `.gen.dao`
# files.

set -euo pipefail
if command -v git &>/dev/null && git rev-parse --show-toplevel &>/dev/null; then
  cd "$(git rev-parse --show-toplevel)"
else
  cd "$(dirname "$0")/.."
fi

OUT_ROOT="${1:-bootstrap}"
SHARED=bootstrap/shared/base.dao

# Strip a leading `module <path>` line (plus the blank/comment lines
# before it) from a file on stdout. Each real input carries its own
# module declaration, and the generated file gets one synthetic
# declaration at the top.
strip_module() {
  awk '
    BEGIN { found = 0 }
    {
      if (!found) {
        if ($0 ~ /^[[:space:]]*$/) { next }
        if ($0 ~ /^[[:space:]]*\/\//) { next }
        if ($0 ~ /^[[:space:]]*#/) { next }
        if ($0 ~ /^[[:space:]]*module[[:space:]]/) { found = 1; next }
        found = 1
      }
      print
    }
  ' "$1"
}

# assemble <out.gen.dao> <input>...
# The shared base is prepended to every program automatically.
assemble() {
  local out="$1"
  shift
  mkdir -p "$(dirname "$out")"
  local gen_module="${out##*/}"
  gen_module="${gen_module%.gen.dao}"
  echo "module bootstrap::${gen_module}::gen" > "$out"
  echo "// GENERATED — do not edit. Edit bootstrap/shared/base.dao or the" >> "$out"
  echo "// subsystem impl.dao / tests.dao, then run: bash bootstrap/assemble.sh" >> "$out"
  echo "" >> "$out"
  strip_module "$SHARED" >> "$out"
  for src in "$@"; do
    echo "" >> "$out"
    strip_module "$src" >> "$out"
  done
  echo "  assembled $out"
}

# Library fragments, in dependency order. A program lists the fragments
# it needs, then its own test runner or entry point last.
RESOLVER=bootstrap/resolver/impl.dao
TYPECHECK=bootstrap/typecheck/impl.dao
HIR=bootstrap/hir/impl.dao
MIR=bootstrap/mir/impl.dao
LLVM=bootstrap/llvm/impl.dao
DRIVER=bootstrap/driver/impl.dao

# lexer, parser and graph draw their library entirely from the shared
# base, so each is just the base plus its own test runner.
assemble "$OUT_ROOT/lexer/lexer.gen.dao" \
  bootstrap/lexer/tests.dao

assemble "$OUT_ROOT/parser/parser.gen.dao" \
  bootstrap/parser/tests.dao

assemble "$OUT_ROOT/graph/graph.gen.dao" \
  bootstrap/graph/tests.dao

assemble "$OUT_ROOT/resolver/resolver.gen.dao" \
  "$RESOLVER" \
  bootstrap/resolver/tests.dao

assemble "$OUT_ROOT/typecheck/typecheck.gen.dao" \
  "$RESOLVER" \
  "$TYPECHECK" \
  bootstrap/typecheck/tests.dao

assemble "$OUT_ROOT/hir/hir.gen.dao" \
  "$RESOLVER" \
  "$TYPECHECK" \
  "$HIR" \
  bootstrap/hir/tests.dao

assemble "$OUT_ROOT/mir/mir.gen.dao" \
  "$RESOLVER" \
  "$TYPECHECK" \
  "$HIR" \
  "$MIR" \
  bootstrap/mir/tests.dao

assemble "$OUT_ROOT/llvm/llvm.gen.dao" \
  "$RESOLVER" \
  "$TYPECHECK" \
  "$HIR" \
  "$MIR" \
  "$LLVM" \
  bootstrap/llvm/tests.dao

assemble "$OUT_ROOT/driver/driver_tests.gen.dao" \
  "$DRIVER" \
  bootstrap/driver/tests.dao

# The bootstrap compiler itself: every library fragment and the driver
# that runs the whole pipeline over an explicit manifest's files.
assemble "$OUT_ROOT/driver/driver.gen.dao" \
  "$RESOLVER" \
  "$TYPECHECK" \
  "$HIR" \
  "$MIR" \
  "$LLVM" \
  "$DRIVER" \
  bootstrap/driver/main.dao

echo "done — 10 files assembled"
