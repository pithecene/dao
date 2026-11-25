#!/usr/bin/env bash
# assemble.sh — Compose bootstrap programs that still share `base.dao`
# from explicit source files: `bootstrap/shared/base.dao`, upstream
# subsystem library fragments, and that program's test runner or entry
# point.
#
# `bootstrap::ascii`, `bootstrap::lexer`, `bootstrap::parser`, and
# `bootstrap::graph` are real modules compiled directly from authored sources
# and are not assembled here.
#
# Run from repository root:
#   bash bootstrap/assemble.sh [output-root]

set -euo pipefail
if command -v git &>/dev/null && git rev-parse --show-toplevel &>/dev/null; then
  cd "$(git rev-parse --show-toplevel)"
else
  cd "$(dirname "$0")/.."
fi

OUT_ROOT="${1:-bootstrap}"
SHARED=bootstrap/shared/base.dao

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

RESOLVER=bootstrap/resolver/impl.dao
TYPECHECK=bootstrap/typecheck/impl.dao
HIR=bootstrap/hir/impl.dao
MIR=bootstrap/mir/impl.dao
LLVM=bootstrap/llvm/impl.dao
DRIVER=bootstrap/driver/impl.dao

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

assemble "$OUT_ROOT/driver/driver.gen.dao" \
  "$RESOLVER" \
  "$TYPECHECK" \
  "$HIR" \
  "$MIR" \
  "$LLVM" \
  "$DRIVER" \
  bootstrap/driver/main.dao

echo "done — 7 files assembled"
