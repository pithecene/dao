#!/usr/bin/env bash
# Bootstrap closure audit (Task 34): what the Dao compiler's own source
# corpus needs, measured rather than assumed, written to
# docs/bootstrap_closure.md.
#
# Three mechanical sources:
#   1. the host AST printer over every assembled bootstrap program — which
#      constructs the corpus uses, and how often
#   2. the host LLVM lowering of the largest program — which prelude
#      functions the corpus instantiates (the stdlib the bootstrap must
#      be able to compile for itself)
#   3. the bootstrap pipeline over its own programs, stage by stage
#      (bootstrap/llvm/llvm.gen with the closure probe enabled) — where
#      the bootstrap stops today and on what
#
#   bootstrap/audit_closure.sh [--report-only] <build dir>
#
# --report-only rewrites the document from the last run's probe outputs
# under bootstrap/llvm/out without re-running the probes (pass the same
# AUDIT_MEMORY_KB / AUDIT_SECONDS the run used, so the document states
# its bounds correctly); the inventory and instantiations are recomputed.
set -u
REPORT_ONLY=0
if [ "${1:-}" = "--report-only" ]; then
  REPORT_ONLY=1
  shift
fi
BUILD_DIR="${1:-build/debug}"
DAOC="$BUILD_DIR/compiler/driver/daoc"
OUT="bootstrap/llvm/out"
DOC="docs/bootstrap_closure.md"
PROGRAMS="lexer parser graph resolver typecheck hir mir llvm"

bash bootstrap/assemble.sh > /dev/null || { echo "audit: assemble failed"; exit 1; }
mkdir -p "$OUT"
# The probe marker must not outlive this run: left behind, every later
# bootstrap/llvm/llvm.gen run would probe instead of testing.
trap 'rm -f "$OUT/.closure_probe"' EXIT

# 1. Construct inventory: the labels the host AST printer prints, counted
#    over every program.  Only the printer's own vocabulary counts, so a
#    bare type-argument line (`SourceInput`) is not taken for a construct.
#    A program the host cannot print is an audit failure, not a smaller
#    inventory.
LABELS="$(grep -ohE '"[A-Z][A-Za-z]+' compiler/frontend/ast/ast_printer.cpp compiler/frontend/ast/ast.cpp | tr -d '"' | sort -u)"
: > "$OUT/inventory.ast"
for p in $PROGRAMS; do
  "$DAOC" ast "bootstrap/$p/$p.gen.dao" >> "$OUT/inventory.ast" || { echo "audit: daoc ast failed for $p"; exit 1; }
done
INVENTORY="$(awk -v labels="$LABELS" 'BEGIN { n = split(labels, a, "\n"); for (i = 1; i <= n; i++) label[a[i]] = 1 }
  { name = $1; sub(/:$/, "", name); if (name in label) print name }' "$OUT/inventory.ast" | sort | uniq -c | sort -rn)"

# 2. Prelude instantiations forced by the largest program.  Prelude
#    functions are told apart by their module-qualified LLVM names
#    (`core::...`), which the backend emits from Task 31 D4 on; an
#    earlier backend names everything bare, and this section cannot be
#    measured with it.  The intrinsic family (`size_of`, `align_of`,
#    `ptr_offset`; CONTRACT_MODULE_SYSTEM.md §7 rule 8) never reaches
#    LLVM as a definition -- the host lowers each specialization inline
#    -- so it is counted from the MIR, where every specialization is a
#    `fn_ref <name>$<type>`.
INSTANCES="$("$DAOC" llvm-ir bootstrap/llvm/llvm.gen.dao 2>/dev/null \
  | grep -oE 'define [^@]*@"core::[^"]*"' | sed -E 's/.*@"//; s/"$//; s/\$.*//' | sort | uniq -c | sort -rn)"
INSTANCES_TABLE="$(echo "$INSTANCES" | awk 'NF {printf "| `%s` | %s |\n", $2, $1}')"
INTRINSICS="$("$DAOC" mir bootstrap/llvm/llvm.gen.dao 2>/dev/null \
  | grep -oE 'fn_ref (size_of|align_of|ptr_offset)\$[^ ]+' | sed 's/^fn_ref //' | sort -u | sed 's/\$.*//' | uniq -c | sort -rn)"
INTRINSICS_TABLE="$(echo "$INTRINSICS" | awk 'NF {printf "| `%s` (intrinsic; inlined by the host) | %s |\n", $2, $1}')"
if [ -z "$INSTANCES_TABLE" ]; then
  if [ "$REPORT_ONLY" -eq 1 ] && [ -f "$DOC" ]; then
    # Keep what the last run measured rather than publish an empty table.
    INSTANCES_TABLE="$(awk '/^## 2\./ {f=1; next} /^## 3\./ {f=0} f && /^\| `/' "$DOC")"
    echo "audit: this backend does not name prelude functions; keeping the last measured §2" >&2
  else
    echo "audit: no core:: functions in the IR — §2 needs the module-qualified backend (Task 31 D4)"; exit 1
  fi
fi

# 3. Self-compile probe: one process per program and stage, from source.
#    The bootstrap frees nothing, so a stage's cost can only be measured
#    alone; and a Dao panic aborts the process, so a program that panics
#    gets a line naming the stage it died in and the panic message.
#    Stages run in pipeline order and stop at the first that dies: every
#    later one would die the same way.
# Each probe is bounded (virtual memory and wall time) so a runaway
# bootstrap run is recorded as such instead of exhausting the machine.
LIMIT_KB="${AUDIT_MEMORY_KB:-6291456}"   # 6 GiB
LIMIT_S="${AUDIT_SECONDS:-600}"
STAGES="parse typecheck hir mir llvm"
if [ "$REPORT_ONLY" -eq 0 ]; then
"$DAOC" build bootstrap/llvm/llvm.gen.dao > /dev/null || { echo "audit: building llvm.gen failed"; exit 1; }
rm -f "$OUT/closure.txt"
for p in $PROGRAMS; do
  : > "$OUT/probe-$p.log"
  record="$p"
  why=""
  for stage in $STAGES; do
    printf '%s\t%s' "$p" "$stage" > "$OUT/.closure_probe"
    rm -f "$OUT/closure.txt"
    start=$SECONDS
    ( ulimit -v "$LIMIT_KB"; /usr/bin/time -f 'probe: peak_rss_kb=%M' -o "$OUT/probe-$p.time" timeout "$LIMIT_S" ./bootstrap/llvm/llvm.gen ) > "$OUT/probe-$p-$stage.log" 2>&1
    status=$?
    elapsed=$((SECONDS - start))
    cat "$OUT/probe-$p-$stage.log" >> "$OUT/probe-$p.log"
    peak_kb="$(grep -oE 'peak_rss_kb=[0-9]+' "$OUT/probe-$p.time" 2>/dev/null | tail -1 | cut -d= -f2)"
    peak_mb=$(( ${peak_kb:-0} / 1024 ))
    # The deepest stage attempted is the cost of one self-compilation
    # attempt through that stage: its peak and time are the program's.
    deepest="$stage"; deepest_peak=$peak_mb; deepest_seconds=$elapsed
    if [ -f "$OUT/closure.txt" ] && grep -q "^$p	" "$OUT/closure.txt"; then
      record="$record$(grep "^$p	" "$OUT/closure.txt" | head -1 | sed "s/^$p//")"
      continue
    fi
    if [ "$status" -eq 124 ]; then
      why="exceeded ${LIMIT_S}s"
    elif grep -q 'dao panic:' "$OUT/probe-$p-$stage.log"; then
      why="panic: $(grep -m1 -oE 'dao panic: .*' "$OUT/probe-$p-$stage.log" | sed 's/^dao panic: //')"
    else
      why="process died (status $status; memory bound $((LIMIT_KB / 1048576)) GiB)"
    fi
    # Stage counts and a first diagnostic survive on stderr even when the
    # stage dies part-way.
    counts="$(grep -oE "^probe: $p (lex|parse|resolve|typecheck|hir|mir|llvm)=[0-9]+" "$OUT/probe-$p-$stage.log" | sed "s/^probe: $p //" | tr '\n' '\t' | sed 's/\t$//')"
    [ -n "$counts" ] && record="$record	$counts"
    break
  done
  earlier="$(grep -m1 -oE "^probe: $p first [a-z]+: .*" "$OUT/probe-$p.log" | sed "s/^probe: $p first //")"
  if [ -n "$why" ]; then
    first="${earlier:+$earlier; then }in ${deepest}: $why"
  else
    first="$earlier"
  fi
  printf '%s\tpeak_mb=%s\tseconds=%s\tfirst=%s\n' "$record" "$deepest_peak" "$deepest_seconds" "$first" >> "$OUT/closure.merged"
done
mv "$OUT/closure.merged" "$OUT/closure.txt"
rm -f "$OUT/.closure_probe"
fi
[ -f "$OUT/closure.txt" ] || { echo "audit: no probe outputs under $OUT to report on"; exit 1; }

# Sites of the one construct behind every parse-stage rejection in the
# first audit: generic arguments on a qualified name in expression
# position (`Vector<i64>::new()`, `Option<T>::None`).  The bootstrap
# parser reads `Vector<i64>` as the comparison `Vector < i64 > ...` and
# stops at `::`.  Counted per assembled program to set against the
# parse column; the parse column reaches zero when the parser closes it.
generic_qualified_sites() {
  grep -o -E '[A-Z][A-Za-z]*<[^;()=]*>::' "bootstrap/$1/$1.gen.dao" 2>/dev/null | wc -l
}

{
  echo "# Bootstrap Closure — Dao"
  echo
  echo "Generated by \`bootstrap/audit_closure.sh\` (Task 34); do not edit by hand."
  echo "Measured on the assembled bootstrap programs under \`bootstrap/*/*.gen.dao\`."
  echo
  echo "## 1. Constructs the compiler corpus uses"
  echo
  echo "Labels of the host AST printer over all eight programs, with counts."
  echo "A construct absent here is not a bootstrap blocker whatever its Tier B status."
  echo
  echo "| Construct | Count |"
  echo "|---|---|"
  echo "$INVENTORY" | awk '{printf "| `%s` | %s |\n", $2, $1}'
  echo
  echo "## 2. Prelude functions the corpus instantiates"
  echo
  echo "Distinct \`core::\` functions the host lowers for \`llvm.gen\` (the largest"
  echo "program), with the number of instantiations each: the stdlib the"
  echo "bootstrap must compile for itself."
  echo
  echo "| Function | Instantiations |"
  echo "|---|---|"
  echo "$INSTANCES_TABLE"
  echo "$INTRINSICS_TABLE"
  echo
  echo "The intrinsic family is counted from the MIR (distinct specializations"
  echo "of each), since the host lowers every specialization inline and emits no"
  echo "definition for it."
  echo
  echo "## 3. The bootstrap pipeline over its own programs"
  echo
  echo "Diagnostics per stage when each program is fed through the bootstrap"
  echo "pipeline, and the earliest failing stage's first diagnostic."
  echo
  echo "Each stage ran in its own process from source, bounded to $((LIMIT_KB / 1048576)) GiB of"
  echo "virtual memory and ${LIMIT_S} s (the bootstrap frees nothing, so a stage's cost"
  echo "can only be measured alone); peak memory and time are the deepest stage's --"
  echo "one self-compilation attempt through that stage."
  echo
  echo "| Program | Peak MiB | Seconds | lex | parse | resolve | typecheck | hir | mir | llvm | First blocking diagnostic |"
  echo "|---|---|---|---|---|---|---|---|---|---|---|"
  awk -F'\t' '{
    name=$1; lex=""; parse=""; resolve=""; typecheck=""; hir=""; mir=""; llvm=""; first=""; peak=""; secs="";
    for (i = 2; i <= NF; i++) {
      split($i, kv, "="); key=kv[1]; val=substr($i, length(key) + 2);
      if (key == "lex") lex=val; else if (key == "parse") parse=val; else if (key == "resolve") resolve=val;
      else if (key == "typecheck") typecheck=val; else if (key == "hir") hir=val;
      else if (key == "mir") mir=val; else if (key == "llvm") llvm=val; else if (key == "first") first=val;
      else if (key == "peak_mb") peak=val; else if (key == "seconds") secs=val;
      else if (key == "missing") first="not assembled: " val;
    }
    gsub(/\|/, "\\|", first);
    printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n", name, peak, secs, lex, parse, resolve, typecheck, hir, mir, llvm, (first == "" ? "—" : first);
  }' "$OUT/closure.txt"
  echo
  echo "### What each stage rejects"
  echo
  echo "The most frequent diagnostic messages per program and stage (the probe"
  echo "reports at most 50 per stage), so the construct to implement next is"
  echo "named, not inferred."
  echo
  for p in $PROGRAMS; do
    if grep -q "^probe: $p diag " "$OUT/probe-$p.log" 2>/dev/null; then
      echo "**$p**"
      echo
      for stage in lex parse resolve typecheck hir mir llvm; do
        grep "^probe: $p diag $stage: " "$OUT/probe-$p.log" | sed "s/^probe: $p diag $stage: //" \
          | sort | uniq -c | sort -rn | head -5 \
          | awk -v stage="$stage" '{ n=$1; $1=""; sub(/^ /, ""); gsub(/\|/, "\\|"); printf "- `%s` ×%s: %s\n", stage, n, $0 }'
      done
      echo
    fi
  done
  echo "### Parse-stage attribution"
  echo
  echo "Every parse-stage rejection in the first audit is one construct: generic"
  echo "arguments on a qualified name in expression position"
  echo "(\`Vector<i64>::new()\`, \`HashMap<i64>::new()\`, \`Option<T>::None\`)."
  echo "The bootstrap parser reads \`Vector<i64>\` as the comparison"
  echo "\`Vector < i64 > ...\` and stops at \`::\` with \"expected expression\";"
  echo "one diagnostic per site at statement level, two for nested arguments,"
  echo "three inside an argument list (the \"expected RParen, got Identifier\""
  echo "lines) — measured by feeding one-construct programs through the probe."
  echo "Sites per program against the parse column above:"
  echo
  echo "| Program | \`Type<Args>::\` sites | parse diagnostics |"
  echo "|---|---|---|"
  for p in $PROGRAMS; do
    parse="$(grep "^$p	" "$OUT/closure.txt" | grep -oE '	parse=[0-9]+' | sed 's/.*=//')"
    echo "| $p | $(generic_qualified_sites "$p") | ${parse:-—} |"
  done
  echo
  echo "## 4. Reading the matrix"
  echo
  echo "- **Tier B-Bootstrap** is the set of constructs in §1 whose lowering the"
  echo "  bootstrap rejects in §3, plus the prelude functions in §2 the bootstrap"
  echo "  cannot yet compile.  Everything else in the Tier B backlog can follow"
  echo "  the first bootstrap crossing."
  echo "- A stage with zero diagnostics on every program is closed for the corpus."
  echo "- The first blocking diagnostic names the construct to implement next for"
  echo "  that stage; rerun the audit after each slice."
} > "$DOC"

echo "audit: wrote $DOC"
echo "--- self-compile ---"
cat "$OUT/closure.txt"
