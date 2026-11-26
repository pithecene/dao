#!/usr/bin/env bash
# assemble.sh — Compose bootstrap programs not yet extracted into standalone
# modules from upstream subsystem library fragments and that program's test
# runner or entry point.
#
# `bootstrap::ascii`, `bootstrap::lexer`, `bootstrap::parser`,
# `bootstrap::graph`, `bootstrap::base`, and `bootstrap::resolver` are real
# modules compiled directly from authored sources and are not assembled here.
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

collect_imports() {
  awk '
    BEGIN { in_hdr = 1 }
    {
      if (in_hdr) {
        if ($0 ~ /^[[:space:]]*$/ || $0 ~ /^[[:space:]]*\/\// || $0 ~ /^[[:space:]]*#/) { next }
        if ($0 ~ /^[[:space:]]*module[[:space:]]/) { next }
        if ($0 ~ /^[[:space:]]*import[[:space:]]/) {
          if (!seen[$0]++) print $0
          next
        }
        in_hdr = 0
      }
    }
    FNR == 1 { in_hdr = 1 }
  ' "$@"
}

strip_header() {
  awk '
    BEGIN { in_hdr = 1 }
    {
      if (in_hdr) {
        if ($0 ~ /^[[:space:]]*$/ || $0 ~ /^[[:space:]]*\/\// || $0 ~ /^[[:space:]]*#/) { next }
        if ($0 ~ /^[[:space:]]*module[[:space:]]/) { next }
        if ($0 ~ /^[[:space:]]*import[[:space:]]/) { next }
        in_hdr = 0
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
  echo "// GENERATED — do not edit. Edit the subsystem impl.dao / tests.dao," >> "$out"
  echo "// then run: bash bootstrap/assemble.sh" >> "$out"
  echo "" >> "$out"
  collect_imports "$@" >> "$out"
  for src in "$@"; do
    echo "" >> "$out"
    strip_header "$src" >> "$out"
  done
  echo "  assembled $out"
}

TYPECHECK=bootstrap/typecheck/impl.dao
HIR=bootstrap/hir/impl.dao
MIR=bootstrap/mir/impl.dao
LLVM=bootstrap/llvm/impl.dao
DRIVER=bootstrap/driver/impl.dao

assemble "$OUT_ROOT/typecheck/typecheck.gen.dao" \
  "$TYPECHECK" \
  bootstrap/typecheck/tests.dao

assemble "$OUT_ROOT/hir/hir.gen.dao" \
  "$TYPECHECK" \
  "$HIR" \
  bootstrap/hir/tests.dao

assemble "$OUT_ROOT/mir/mir.gen.dao" \
  "$TYPECHECK" \
  "$HIR" \
  "$MIR" \
  bootstrap/mir/tests.dao

assemble "$OUT_ROOT/llvm/llvm.gen.dao" \
  "$TYPECHECK" \
  "$HIR" \
  "$MIR" \
  "$LLVM" \
  bootstrap/llvm/tests.dao

assemble "$OUT_ROOT/driver/driver_tests.gen.dao" \
  "$DRIVER" \
  bootstrap/driver/tests.dao

assemble "$OUT_ROOT/driver/driver.gen.dao" \
  "$TYPECHECK" \
  "$HIR" \
  "$MIR" \
  "$LLVM" \
  "$DRIVER" \
  bootstrap/driver/main.dao

echo "done — 6 files assembled"
