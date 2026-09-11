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
# --report-only rewrites the document from the last run's recorded
# outputs under <build dir>/bootstrap_audit without re-running the
# probes (pass the same AUDIT_MEMORY_KB / AUDIT_SECONDS the run used, so
# the document states its bounds correctly); the inventory and
# instantiations are recomputed.  The records live in the build
# directory, not under bootstrap/llvm/out, which `task bootstrap-test`
# clears before every run.
set -u
REPORT_ONLY=0
if [ "${1:-}" = "--report-only" ]; then
  REPORT_ONLY=1
  shift
fi
BUILD_DIR="${1:-build/debug}"
DAOC="$BUILD_DIR/compiler/driver/daoc"
OUT="bootstrap/llvm/out"                 # where llvm.gen looks for its marker and writes a stage's line
AUDIT_OUT="$BUILD_DIR/bootstrap_audit"   # this run's records: per-stage logs, times, the merged matrix
DOC="docs/bootstrap_closure.md"
PROGRAMS="lexer parser graph resolver typecheck hir mir llvm"

bash bootstrap/assemble.sh > /dev/null || { echo "audit: assemble failed"; exit 1; }
mkdir -p "$OUT" "$AUDIT_OUT"
# The prelude the program pipeline runs with: every module under
# stdlib/core/ and stdlib/io/ in path order (CONTRACT_MODULE_SYSTEM.md
# §7.1; the host's prelude_files).  The probe reads the list -- it has
# no directory listing -- so membership is this script's explicit input.
PRELUDE_LIST="$OUT/.closure_prelude"
ls stdlib/core/*.dao stdlib/io/*.dao | LC_ALL=C sort > "$PRELUDE_LIST"
# The probe marker and prelude list must not outlive this run: left
# behind, every later bootstrap/llvm/llvm.gen run would probe instead of
# testing.  Nor may a half-merged matrix: the next run would append to it.
trap 'rm -f "$OUT/.closure_probe" "$PRELUDE_LIST" "$AUDIT_OUT/closure.merged"' EXIT

# 1. Construct inventory: the labels the host AST printer prints, counted
#    over every program.  Only the printer's own vocabulary counts, so a
#    bare type-argument line (`SourceInput`) is not taken for a construct.
#    A program the host cannot print is an audit failure, not a smaller
#    inventory.
LABELS="$(grep -ohE '"[A-Z][A-Za-z]+' compiler/frontend/ast/ast_printer.cpp compiler/frontend/ast/ast.cpp | tr -d '"' | sort -u)"
: > "$AUDIT_OUT/inventory.ast"
for p in $PROGRAMS; do
  "$DAOC" ast "bootstrap/$p/$p.gen.dao" >> "$AUDIT_OUT/inventory.ast" || { echo "audit: daoc ast failed for $p"; exit 1; }
done
INVENTORY="$(awk -v labels="$LABELS" 'BEGIN { n = split(labels, a, "\n"); for (i = 1; i <= n; i++) label[a[i]] = 1 }
  { name = $1; sub(/:$/, "", name); if (name in label) print name }' "$AUDIT_OUT/inventory.ast" | sort | uniq -c | sort -rn)"

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
    # Only the measured `core::` rows: the intrinsic rows are recomputed below.
    INSTANCES_TABLE="$(awk '/^## 2\./ {f=1; next} /^## 3\./ {f=0} f && /^\| `core::/' "$DOC")"
    echo "audit: this backend does not name prelude functions; keeping the last measured §2" >&2
  else
    echo "audit: no core:: functions in the IR — §2 needs the module-qualified backend (Task 31 D4)"; exit 1
  fi
fi

# 3. Self-compile probe: one process per program and stage, from source.
#    One process per stage so a stage's peak is its own -- a process
#    running the whole pipeline would hold every earlier stage's result
#    while measuring the next; and a Dao panic aborts the process, so a program that panics
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
rm -f "$OUT/closure.txt" "$AUDIT_OUT/closure.merged"
# The prelude alone, once: each file's parse and resolve counts, then
# its typecheck count, for the document's prelude section.  A probe
# that dies leaves the lines it wrote before dying, and why it died.
printf 'prelude\tprelude' > "$OUT/.closure_probe"
( ulimit -v "$LIMIT_KB"; timeout "$LIMIT_S" ./bootstrap/llvm/llvm.gen ) > "$AUDIT_OUT/probe-prelude.log" 2>&1
prelude_status=$?
rm -f "$OUT/closure.txt" "$AUDIT_OUT/probe-prelude.died"
if [ "$prelude_status" -eq 124 ]; then
  echo "exceeded ${LIMIT_S}s" > "$AUDIT_OUT/probe-prelude.died"
elif [ "$prelude_status" -ne 0 ]; then
  echo "process died (status $prelude_status; memory bound $((LIMIT_KB / 1048576)) GiB) $(grep -m1 -oE 'dao panic: .*' "$AUDIT_OUT/probe-prelude.log")" > "$AUDIT_OUT/probe-prelude.died"
fi
for p in $PROGRAMS; do
  : > "$AUDIT_OUT/probe-$p.log"
  record="$p"
  why=""
  for stage in $STAGES; do
    printf '%s\t%s' "$p" "$stage" > "$OUT/.closure_probe"
    rm -f "$OUT/closure.txt"
    start=$SECONDS
    ( ulimit -v "$LIMIT_KB"; /usr/bin/time -f 'probe: peak_rss_kb=%M' -o "$AUDIT_OUT/probe-$p.time" timeout "$LIMIT_S" ./bootstrap/llvm/llvm.gen ) > "$AUDIT_OUT/probe-$p-$stage.log" 2>&1
    status=$?
    elapsed=$((SECONDS - start))
    cat "$AUDIT_OUT/probe-$p-$stage.log" >> "$AUDIT_OUT/probe-$p.log"
    peak_kb="$(grep -oE 'peak_rss_kb=[0-9]+' "$AUDIT_OUT/probe-$p.time" 2>/dev/null | tail -1 | cut -d= -f2)"
    peak_mb=$(( ${peak_kb:-0} / 1024 ))
    # The deepest stage attempted is the cost of one self-compilation
    # attempt through that stage: its peak and time are the program's.
    deepest="$stage"; deepest_peak=$peak_mb; deepest_seconds=$elapsed
    # The probe wrote its stage's columns to $OUT/closure.txt; take them.
    if [ -f "$OUT/closure.txt" ] && grep -q "^$p	" "$OUT/closure.txt"; then
      record="$record$(grep "^$p	" "$OUT/closure.txt" | head -1 | sed "s/^$p//")"
      continue
    fi
    # A stage that exits cleanly has written its line; none means the
    # audit is not reading its probe, not a program with nothing to say.
    if [ "$status" -eq 0 ]; then
      echo "audit: stage $stage of $p exited 0 without writing its line (see $AUDIT_OUT/probe-$p-$stage.log)"; exit 1
    fi
    if [ "$status" -eq 124 ]; then
      why="exceeded ${LIMIT_S}s"
    elif grep -q 'dao panic:' "$AUDIT_OUT/probe-$p-$stage.log"; then
      why="panic: $(grep -m1 -oE 'dao panic: .*' "$AUDIT_OUT/probe-$p-$stage.log" | sed 's/^dao panic: //')"
    else
      why="process died (status $status; memory bound $((LIMIT_KB / 1048576)) GiB)"
    fi
    # Stage counts and a first diagnostic survive on stderr even when the
    # stage dies part-way.
    counts="$(grep -oE "^probe: $p (lex|parse|resolve|typecheck|hir|mir|llvm)=[0-9]+" "$AUDIT_OUT/probe-$p-$stage.log" | sed "s/^probe: $p //" | tr '\n' '\t' | sed 's/\t$//')"
    [ -n "$counts" ] && record="$record	$counts"
    break
  done
  earlier="$(grep -m1 -oE "^probe: $p first [a-z]+: .*" "$AUDIT_OUT/probe-$p.log" | sed "s/^probe: $p first //")"
  if [ -n "$why" ]; then
    first="${earlier:+$earlier; then }in ${deepest}: $why"
  else
    first="$earlier"
  fi
  printf '%s\tpeak_mb=%s\tseconds=%s\tfirst=%s\n' "$record" "$deepest_peak" "$deepest_seconds" "$first" >> "$AUDIT_OUT/closure.merged"
done
mv "$AUDIT_OUT/closure.merged" "$AUDIT_OUT/closure.txt"
rm -f "$OUT/.closure_probe" "$OUT/closure.txt"
fi
[ -f "$AUDIT_OUT/closure.txt" ] || { echo "audit: no recorded run under $AUDIT_OUT to report on"; exit 1; }

# Sites of the one construct behind every parse-stage rejection in the
# first audit: generic arguments on a qualified name in expression
# position (`Vector<i64>::new()`, `Option<T>::None`).  The bootstrap
# parser reads `Vector<i64>` as the comparison `Vector < i64 > ...` and
# stops at `::`.  Counted per assembled program to set against the
# parse column; the parse column reaches zero when the parser closes it.
generic_qualified_sites() {
  grep -o -E '[A-Z][A-Za-z]*<[^;()=]*>::' "bootstrap/$1/$1.gen.dao" 2>/dev/null | wc -l
}
resource_block_sites() {
  grep -c -E '^[[:space:]]*resource[[:space:]]+[a-z]+[[:space:]]+[A-Za-z_]+[[:space:]]*=>' "bootstrap/$1/$1.gen.dao" 2>/dev/null
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
  echo "virtual memory and ${LIMIT_S} s (one process per stage, so a stage's peak is its"
  echo "own: a process running the whole pipeline would hold every earlier stage's"
  echo "result while measuring the next); peak memory and time are the deepest stage's --"
  echo "one self-compilation attempt through that stage."
  echo
  echo "A stage the program never reached reads as —; the last column says where"
  echo "it died and why."
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
    # A stage the program never reached (it died earlier; the last column
    # says where and why) reads as a dash, not as an empty count.
    if (lex == "") lex = "—"; if (parse == "") parse = "—"; if (resolve == "") resolve = "—"; if (typecheck == "") typecheck = "—";
    if (hir == "") hir = "—"; if (mir == "") mir = "—"; if (llvm == "") llvm = "—";
    printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n", name, peak, secs, lex, parse, resolve, typecheck, hir, mir, llvm, (first == "" ? "—" : first);
  }' "$AUDIT_OUT/closure.txt"
  echo
  echo "### What each stage rejects"
  echo
  echo "The most frequent diagnostic messages per program and stage (the probe"
  echo "reports at most 50 per stage), so the construct to implement next is"
  echo "named, not inferred."
  echo
  for p in $PROGRAMS; do
    if grep -q "^probe: $p diag " "$AUDIT_OUT/probe-$p.log" 2>/dev/null; then
      echo "**$p**"
      echo
      for stage in lex parse resolve typecheck hir mir llvm; do
        grep "^probe: $p diag $stage: " "$AUDIT_OUT/probe-$p.log" | sed "s/^probe: $p diag $stage: //" \
          | sort | uniq -c | sort -rn | head -5 \
          | awk -v stage="$stage" '{ n=$1; $1=""; sub(/^ /, ""); gsub(/\|/, "\\|"); printf "- `%s` ×%s: %s\n", stage, n, $0 }'
      done
      echo
    fi
  done
  echo "### The prelude through the bootstrap"
  echo
  echo "The \`resolve\` and \`typecheck\` columns above are measured with the"
  echo "prelude in the program -- every module under \`stdlib/core/\` and"
  echo "\`stdlib/io/\` (CONTRACT_MODULE_SYSTEM.md §7), loaded as declarations,"
  echo "whatever the bootstrap parser keeps of each file.  The \`hir\`, \`mir\`,"
  echo "and \`llvm\` columns run the single-source adapters without it (the"
  echo "bootstrap has no program-level MIR or LLVM driver) and do not choose"
  echo "the frontier until the program pipeline reaches them."
  echo
  echo "Per prelude file, the prelude group alone through the pipeline:"
  echo "diagnostics per stage, and the first."
  echo
  echo "| File | parse | resolve | typecheck | First diagnostic |"
  echo "|---|---|---|---|---|"
  awk -F'\t' '
    /^probe: prelude file\t/ { path = $2; split($3, a, "="); split($4, b, "="); parse[path] = a[2]; resolve[path] = b[2]; first[path] = substr($5, 7); order[++n] = path }
    /^probe: prelude typecheck\t/ { tc[$2] = $3; if (first[$2] == "") first[$2] = $4 }
    END { for (i = 1; i <= n; i++) { p = order[i]; f = first[p]; gsub(/\|/, "\\|", f); printf "| `%s` | %s | %s | %s | %s |\n", p, parse[p], resolve[p], (p in tc ? tc[p] : "—"), (f == "" ? "—" : f) } }
  ' "$AUDIT_OUT/probe-prelude.log"
  if [ -f "$AUDIT_OUT/probe-prelude.died" ]; then
    echo
    echo "The prelude probe died: $(cat "$AUDIT_OUT/probe-prelude.died").  A stage it never reached reads as —."
  fi
  echo
  echo "### Parse-stage attribution"
  echo
  echo "Every parse-stage rejection in the first audit was one construct: generic"
  echo "arguments on a qualified name in expression position"
  echo "(\`Vector<i64>::new()\`, \`HashMap<i64>::new()\`), read as the comparison"
  echo "\`Vector < i64 > ...\` — one diagnostic per site at statement level, more"
  echo "inside argument lists.  Task 36 taught the parser that construct, which"
  echo "left the \`resource memory\` blocks Task 35 E3 placed in the pipeline"
  echo "drivers (one \"expected expression\" per block, then one \"expected"
  echo "declaration\" per statement skipped recovering); Task 37 taught it those."
  echo "The parse column is closed for the corpus; the resolve column is the"
  echo "next.  Sites per program against the parse column above:"
  echo
  echo "| Program | \`Type<Args>::\` sites | \`resource\` blocks | parse diagnostics |"
  echo "|---|---|---|---|"
  for p in $PROGRAMS; do
    parse="$(grep "^$p	" "$AUDIT_OUT/closure.txt" | grep -oE '	parse=[0-9]+' | sed 's/.*=//')"
    echo "| $p | $(generic_qualified_sites "$p") | $(resource_block_sites "$p") | ${parse:-—} |"
  done
  echo
  echo "### Closure surfaces: compiler implementation vs test harness"
  echo
  echo "Prelude call sites the corpus makes, split by the physical file"
  echo "boundary Task 40 established: \`impl.dao\` and \`base.dao\` (the"
  echo "compiler the Stage-2 crossing runs) against \`tests.dao\` (the"
  echo "bootstrap test harness).  Measurement only -- it selects no task and"
  echo "changes no column above; it exists so a compiler requirement is not"
  echo "confused with a reporting convenience.  A function with zero compiler"
  echo "call sites is needed only to print test status."
  echo
  python3 - <<'PYSPLIT'
import pathlib, re
prelude = {}
for f in sorted(pathlib.Path("stdlib/core").glob("*.dao")) + sorted(pathlib.Path("stdlib/io").glob("*.dao")):
    for m in re.finditer(r'^\s*(?:extern\s+)?fn\s+([a-z_][A-Za-z0-9_]*)', f.read_text(), re.M):
        prelude.setdefault(m.group(1), str(f))
strip = lambda s: re.sub(r'//.*$', '', s, flags=re.M)
lib, test = {}, {}
files = sorted(pathlib.Path("bootstrap").glob("*/impl.dao")) + sorted(pathlib.Path("bootstrap").glob("*/tests.dao")) + [pathlib.Path("bootstrap/shared/base.dao")]
for f in files:
    text = strip(f.read_text()); dst = test if f.name == "tests.dao" else lib
    for n in prelude:
        dst[n] = dst.get(n, 0) + len(re.findall(r'(?<![A-Za-z0-9_.])' + re.escape(n) + r'\s*\(', text))
rows = [(n, lib.get(n, 0), test.get(n, 0)) for n in prelude if lib.get(n, 0) or test.get(n, 0)]
rows.sort(key=lambda r: -(r[1] + r[2]))
print("| Prelude function | Compiler | Harness |")
print("|---|---:|---:|")
for n, l, tt in rows:
    print(f"| `{n}` | {l} | {tt} |")
print(f"| **Total** | **{sum(r[1] for r in rows)}** | **{sum(r[2] for r in rows)}** |")
PYSPLIT
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
  echo "- The \`hir\`, \`mir\`, \`llvm\` columns are measured without the prelude"
  echo "  (see the prelude section) and are not frontier evidence yet."
} > "$DOC"

echo "audit: wrote $DOC"
echo "--- self-compile ($AUDIT_OUT/closure.txt) ---"
cat "$AUDIT_OUT/closure.txt"
