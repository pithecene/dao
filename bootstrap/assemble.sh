#!/usr/bin/env bash
# assemble.sh — Compose each bootstrap program from explicit source
# files: the shared base, the library fragments a program builds on,
# and finally that program's own test runner.
#
# Run from repository root:
#   bash bootstrap/assemble.sh
#
# Source layout (no marker surgery — every input is a whole file):
#   bootstrap/shared/base.dao        token model, lexer, AST, parser, module graph
#   bootstrap/<sub>/impl.dao         a subsystem's library, no test runner
#   bootstrap/<sub>/tests.dao        a subsystem's test runner, never compiled alone
#
# A program includes the library fragments of every earlier phase it
# depends on, in dependency order, then exactly one test runner.

set -euo pipefail
if command -v git &>/dev/null && git rev-parse --show-toplevel &>/dev/null; then
  cd "$(git rev-parse --show-toplevel)"
else
  cd "$(dirname "$0")/.."
fi

SHARED=bootstrap/shared/base.dao

# Strip a leading `module <path>` line (plus the blank/comment lines
# before it) from a file on stdout.  Each real input carries its own
# module declaration; the generated file gets exactly one synthetic
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

# Library fragments, in dependency order.  A program lists the fragments
# it needs, then its own test runner last.
RESOLVER=bootstrap/resolver/impl.dao
TYPECHECK=bootstrap/typecheck/impl.dao
HIR=bootstrap/hir/impl.dao
MIR=bootstrap/mir/impl.dao
LLVM=bootstrap/llvm/impl.dao

# lexer, parser and graph draw their library entirely from the shared
# base, so each is just the base plus its own test runner.
assemble bootstrap/lexer/lexer.gen.dao \
  bootstrap/lexer/tests.dao

assemble bootstrap/parser/parser.gen.dao \
  bootstrap/parser/tests.dao

assemble bootstrap/graph/graph.gen.dao \
  bootstrap/graph/tests.dao

assemble bootstrap/resolver/resolver.gen.dao \
  "$RESOLVER" \
  bootstrap/resolver/tests.dao

assemble bootstrap/typecheck/typecheck.gen.dao \
  "$RESOLVER" \
  "$TYPECHECK" \
  bootstrap/typecheck/tests.dao

assemble bootstrap/hir/hir.gen.dao \
  "$RESOLVER" \
  "$TYPECHECK" \
  "$HIR" \
  bootstrap/hir/tests.dao

assemble bootstrap/mir/mir.gen.dao \
  "$RESOLVER" \
  "$TYPECHECK" \
  "$HIR" \
  "$MIR" \
  bootstrap/mir/tests.dao

assemble bootstrap/llvm/llvm.gen.dao \
  "$RESOLVER" \
  "$TYPECHECK" \
  "$HIR" \
  "$MIR" \
  "$LLVM" \
  bootstrap/llvm/tests.dao

echo "done — 8 files assembled"
