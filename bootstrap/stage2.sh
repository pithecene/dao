#!/usr/bin/env bash
# Self-hosted bootstrap crossing with per-invocation artifact isolation
# and structured provenance output.
#
#   bootstrap/stage2.sh [build-dir]
#
# Stage 1 (host-built driver, or external STAGE1=<binary>) compiles the
# bootstrap compiler into Stage 2; Stage 2 compiles itself into Stage 3;
# Stage 2 and Stage 3 must emit byte-identical LLVM IR; Stage 3 links
# into a working native compiler passing positive, determinism, and
# fail-closed negative checks; and every bootstrap suite compiled by
# Stage 2 must pass in an isolated run directory with a structured
# result.json record.

set -u
BUILD_DIR="${1:-build/debug}"
if command -v git &>/dev/null && git rev-parse --show-toplevel &>/dev/null; then
  cd "$(git rev-parse --show-toplevel)" || exit 1
else
  cd "$(dirname "$0")/.." || exit 1
fi

DAOC="$BUILD_DIR/compiler/driver/daoc"
RUNTIME="$BUILD_DIR/runtime/core/libdao_runtime.a"
CLANG="${CLANG:-clang}"
RAW_REQ_STACK_KB="${STAGE_STACK_KB:-1048576}"
RAW_LIMIT_KB="${STAGE_MEMORY_KB:-8388608}"

normalize_positive_int() {
  case "$1" in
    ''|*[!0-9]*) return 1 ;;
  esac
  local stripped="${1#"${1%%[!0]*}"}"
  [ -n "$stripped" ] || return 1
  printf '%s' "$stripped"
}

if ! REQ_STACK_KB="$(normalize_positive_int "$RAW_REQ_STACK_KB")"; then
  echo "stage2: STAGE_STACK_KB must be a positive integer KiB bound, got '$RAW_REQ_STACK_KB'" >&2
  exit 1
fi
if ! LIMIT_KB="$(normalize_positive_int "$RAW_LIMIT_KB")"; then
  echo "stage2: STAGE_MEMORY_KB must be a positive integer KiB bound, got '$RAW_LIMIT_KB'" >&2
  exit 1
fi

if ! command -v "$CLANG" > /dev/null 2>&1; then
  echo "stage2: clang not found: $CLANG" >&2
  exit 1
fi
if [ ! -f "$RUNTIME" ]; then
  echo "stage2: runtime archive not found at $RUNTIME" >&2
  exit 1
fi

mkdir -p bootstrap/driver/out
if [ -n "${STAGE2_RUN_DIR:-}" ]; then
  RUN_DIR="$STAGE2_RUN_DIR"
  mkdir -p "$RUN_DIR" || { echo "stage2: cannot create $RUN_DIR" >&2; exit 1; }
  if ! mkdir "$RUN_DIR/.stage2.lock" 2>/dev/null; then
    echo "stage2: STAGE2_RUN_DIR is already claimed: $RUN_DIR" >&2
    exit 1
  fi
  if [ -n "$(find "$RUN_DIR" -mindepth 1 -maxdepth 1 ! -name '.stage2.lock' 2>/dev/null)" ]; then
    rmdir "$RUN_DIR/.stage2.lock" 2>/dev/null || true
    echo "stage2: STAGE2_RUN_DIR must not contain prior artifacts: $RUN_DIR" >&2
    exit 1
  fi
else
  RUN_DIR="$(mktemp -d bootstrap/driver/out/run.XXXXXX)" && [ -n "$RUN_DIR" ] || {
    echo "stage2: cannot create isolated run directory" >&2
    exit 1
  }
  mkdir "$RUN_DIR/.stage2.lock" || exit 1
fi

GEN_DIR="$RUN_DIR/gen"
LLVM_OUT="$RUN_DIR/llvm_out"
DRIVER_TEST_WORK="$RUN_DIR/driver_tests_work"
SMOKE_DIR="$RUN_DIR/smoke"
mkdir -p "$GEN_DIR" "$LLVM_OUT" "$DRIVER_TEST_WORK" "$SMOKE_DIR" || exit 1

sha256_file() {
  if command -v shasum > /dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  elif command -v sha256sum > /dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    openssl dgst -sha256 "$1" | awk '{print $NF}'
  fi
}

json_escape() {
  printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'
}

json_limit_value() {
  case "$1" in
    ''|null) printf 'null' ;;
    *[!0-9]*) printf '"%s"' "$(json_escape "$1")" ;;
    *)
      local stripped="${1#"${1%%[!0]*}"}"
      printf '%s' "${stripped:-0}"
      ;;
  esac
}

# Apply and verify stack and virtual-memory bounds explicitly so failed
# limits never masquerade as active bounds.
INITIAL_STACK_KB="$(ulimit -Ss 2>/dev/null || echo unknown)"
HARD_STACK_KB="$(ulimit -Hs 2>/dev/null || echo unknown)"
if ulimit -s "$REQ_STACK_KB" 2>/dev/null; then
  STACK_MODE="requested"
elif ulimit -s hard 2>/dev/null; then
  STACK_MODE="capped_to_hard"
else
  echo "stage2: failed to raise stack limit (initial=${INITIAL_STACK_KB}, hard=${HARD_STACK_KB})" >&2
  exit 1
fi
APPLIED_STACK_KB="$(ulimit -Ss 2>/dev/null || echo unknown)"
case "$APPLIED_STACK_KB" in
  ''|*[!0-9]*)
    ;;
  *)
    if [ "$APPLIED_STACK_KB" -lt 16384 ]; then
      echo "stage2: applied stack limit ${APPLIED_STACK_KB} KiB is below the 16384 KiB minimum" >&2
      exit 1
    fi
    ;;
esac

HOST_OS="$(uname -s)"
HOST_ARCH="$(uname -m)"
if ( ulimit -v "$LIMIT_KB" ) >/dev/null 2>&1; then
  VMEM_MODE="enforced"
  APPLIED_VMEM_JSON="$LIMIT_KB"
elif [ "$HOST_OS" = "Darwin" ]; then
  VMEM_MODE="unsupported_on_darwin"
  APPLIED_VMEM_JSON="null"
else
  echo "stage2: ulimit -v ${LIMIT_KB} failed on ${HOST_OS}" >&2
  exit 1
fi

echo "run directory: $RUN_DIR"
echo "limits: stack=${APPLIED_STACK_KB}KiB (${STACK_MODE}), vmem=${VMEM_MODE}"

bash bootstrap/assemble.sh "$GEN_DIR" > "$RUN_DIR/assemble.log" 2>&1 || {
  echo "stage2: assemble failed (see $RUN_DIR/assemble.log)" >&2
  exit 1
}

write_manifest() {  # write_manifest <manifest-path> <output .ll> <source>...
  local manifest_path="$1" output="$2"; shift 2
  { echo "output $output"
    ls stdlib/core/*.dao stdlib/io/*.dao | LC_ALL=C sort | sed 's/^/prelude /'
    for f in "$@"; do echo "source $f"; done
  } > "$manifest_path"
}

run_compiler() {  # run_compiler <compiler> <manifest-path> <log-path>
  local compiler="$1" manifest_path="$2" log_path="$3"
  if [ "$VMEM_MODE" = "enforced" ]; then
    ( ulimit -v "$LIMIT_KB" || exit 1; "$compiler" "$manifest_path" ) > "$log_path" 2>&1
  else
    "$compiler" "$manifest_path" > "$log_path" 2>&1
  fi
}

compile_with() {  # compile_with <compiler> <output .ll> <source>...
  local compiler="$1" output="$2"; shift 2
  local manifest_path="${output}.manifest"
  rm -f "$output"
  write_manifest "$manifest_path" "$output" "$@"
  run_compiler "$compiler" "$manifest_path" "${output}.log" && [ -s "$output" ]
}

link() {  # link <.ll> <binary>
  "$CLANG" -O0 -c -x ir "$1" -o "$1.o" 2> "$1.clang" &&
    "$CLANG" "$1.o" "$RUNTIME" -o "$2" 2>> "$1.clang"
}

if [ -n "${STAGE1:-}" ]; then
  if [ ! -x "$STAGE1" ]; then
    echo "stage2: external STAGE1 binary is not executable: $STAGE1" >&2
    exit 1
  fi
  SEED_KIND="external_stage1"
  echo "stage 1: $STAGE1 (external seed)"
else
  if [ ! -x "$DAOC" ]; then
    echo "stage2: host compiler not found or not executable at $DAOC" >&2
    exit 1
  fi
  SEED_KIND="host_daoc"
  echo "stage 1: the host builds the bootstrap compiler"
  "$DAOC" build --source "$GEN_DIR/driver/driver.gen.dao" --source bootstrap/mir/impl.dao --source bootstrap/hir/impl.dao --source bootstrap/typecheck/impl.dao --source bootstrap/resolver/impl.dao --source bootstrap/shared/base.dao --source bootstrap/graph/impl.dao --source bootstrap/parser/impl.dao --source bootstrap/lexer/impl.dao --source bootstrap/ascii.dao > "$RUN_DIR/stage1.log" 2>&1 || {
    echo "stage 1 failed (see $RUN_DIR/stage1.log)" >&2
    exit 1
  }
  STAGE1="$GEN_DIR/driver/driver.gen"
fi

echo "stage 2: the bootstrap compiler compiles itself"
compile_with "$STAGE1" "$RUN_DIR/stage2.ll" "$GEN_DIR/driver/driver.gen.dao" bootstrap/mir/impl.dao bootstrap/hir/impl.dao bootstrap/typecheck/impl.dao bootstrap/resolver/impl.dao bootstrap/shared/base.dao bootstrap/graph/impl.dao bootstrap/parser/impl.dao bootstrap/lexer/impl.dao bootstrap/ascii.dao || {
  echo "stage 2 failed (see $RUN_DIR/stage2.ll.log)" >&2
  exit 1
}
link "$RUN_DIR/stage2.ll" "$RUN_DIR/stage2" || {
  echo "stage 2 does not link (see $RUN_DIR/stage2.ll.clang)" >&2
  exit 1
}

echo "stage 3: the stage-2 compiler compiles itself"
compile_with "$RUN_DIR/stage2" "$RUN_DIR/stage3.ll" "$GEN_DIR/driver/driver.gen.dao" bootstrap/mir/impl.dao bootstrap/hir/impl.dao bootstrap/typecheck/impl.dao bootstrap/resolver/impl.dao bootstrap/shared/base.dao bootstrap/graph/impl.dao bootstrap/parser/impl.dao bootstrap/lexer/impl.dao bootstrap/ascii.dao || {
  echo "stage 3 failed (see $RUN_DIR/stage3.ll.log)" >&2
  exit 1
}

if cmp -s "$RUN_DIR/stage2.ll" "$RUN_DIR/stage3.ll"; then
  echo "fixed point: stage 2 and stage 3 emit the same module"
else
  echo "no fixed point: stage 2 and stage 3 differ" >&2
  exit 1
fi

link "$RUN_DIR/stage3.ll" "$RUN_DIR/stage3" || {
  echo "stage 3 does not link (see $RUN_DIR/stage3.ll.clang)" >&2
  exit 1
}

# Verify the linked Stage-3 compiler end-to-end: multi-module compilation,
# source-order determinism, native execution, and fail-closed rejection
# without publishing partial output artifacts.
cat > "$SMOKE_DIR/lib.dao" <<'EOF'
module smoke::lib

fn answer(): i32 -> 42
EOF
cat > "$SMOKE_DIR/main.dao" <<'EOF'
module smoke::main
import smoke::lib

fn main(): i32 -> lib::answer() - 42
EOF
cat > "$SMOKE_DIR/broken.dao" <<'EOF'
module smoke::broken

fn main(): i32 -> "wrong"
EOF

compile_with "$RUN_DIR/stage3" "$SMOKE_DIR/fwd.ll" "$SMOKE_DIR/lib.dao" "$SMOKE_DIR/main.dao" || {
  echo "stage 3 smoke compile failed (see $SMOKE_DIR/fwd.ll.log)" >&2
  exit 1
}
compile_with "$RUN_DIR/stage3" "$SMOKE_DIR/rev.ll" "$SMOKE_DIR/main.dao" "$SMOKE_DIR/lib.dao" || {
  echo "stage 3 reversed-order smoke compile failed (see $SMOKE_DIR/rev.ll.log)" >&2
  exit 1
}
cmp -s "$SMOKE_DIR/fwd.ll" "$SMOKE_DIR/rev.ll" || {
  echo "stage 3 smoke output depends on source order" >&2
  exit 1
}
link "$SMOKE_DIR/fwd.ll" "$SMOKE_DIR/smoke_bin" || {
  echo "stage 3 smoke binary failed to link (see $SMOKE_DIR/fwd.ll.clang)" >&2
  exit 1
}
"$SMOKE_DIR/smoke_bin" > "$SMOKE_DIR/smoke_bin.log" 2>&1 || {
  echo "stage 3 smoke binary failed at runtime" >&2
  exit 1
}

# Negative process-level checks on Stage 3: no arguments (exit 2),
# truncated manifest without trailing newline (exit 2, no output),
# unreadable source path (exit 2, not abort, no output), and compile
# error (exit 1, no output).
"$RUN_DIR/stage3" > "$SMOKE_DIR/neg_noarg.log" 2>&1
neg_noarg_rc=$?
printf 'output %s\nsource %s' "$SMOKE_DIR/neg_trunc.ll" "$SMOKE_DIR/main.dao" > "$SMOKE_DIR/neg_trunc.manifest"
run_compiler "$RUN_DIR/stage3" "$SMOKE_DIR/neg_trunc.manifest" "$SMOKE_DIR/neg_trunc.log"
neg_trunc_rc=$?
write_manifest "$SMOKE_DIR/neg_missing.manifest" "$SMOKE_DIR/neg_missing.ll" "$SMOKE_DIR/no_such_file.dao"
run_compiler "$RUN_DIR/stage3" "$SMOKE_DIR/neg_missing.manifest" "$SMOKE_DIR/neg_missing.log"
neg_missing_rc=$?
write_manifest "$SMOKE_DIR/neg_compile.manifest" "$SMOKE_DIR/neg_compile.ll" "$SMOKE_DIR/broken.dao"
run_compiler "$RUN_DIR/stage3" "$SMOKE_DIR/neg_compile.manifest" "$SMOKE_DIR/neg_compile.log"
neg_compile_rc=$?

if [ "$neg_noarg_rc" -ne 2 ] || [ "$neg_trunc_rc" -ne 2 ] || [ "$neg_missing_rc" -ne 2 ] || [ "$neg_compile_rc" -ne 1 ] ||
   [ -e "$SMOKE_DIR/neg_trunc.ll" ] || [ -e "$SMOKE_DIR/neg_missing.ll" ] || [ -e "$SMOKE_DIR/neg_compile.ll" ]; then
  echo "stage 3 negative checks failed (noarg=$neg_noarg_rc trunc=$neg_trunc_rc missing=$neg_missing_rc compile=$neg_compile_rc)" >&2
  exit 1
fi

# Module-graph negative checks on Stage 3: missing imported module,
# duplicate module declaration, import binding collision, and import
# cycle must each fail closed (exit 1) without writing output.
cat > "$SMOKE_DIR/dup_lib.dao" <<'EOF'
module smoke::lib

fn other(): i32 -> 1
EOF
cat > "$SMOKE_DIR/col_a.dao" <<'EOF'
module alpha::util

fn val(): i32 -> 1
EOF
cat > "$SMOKE_DIR/col_b.dao" <<'EOF'
module beta::util

fn val(): i32 -> 2
EOF
cat > "$SMOKE_DIR/col_main.dao" <<'EOF'
module smoke::col
import alpha::util
import beta::util

fn main(): i32 -> 0
EOF
cat > "$SMOKE_DIR/cyc_a.dao" <<'EOF'
module cyc::a
import cyc::b

fn f(): i32 -> 0
EOF
cat > "$SMOKE_DIR/cyc_b.dao" <<'EOF'
module cyc::b
import cyc::a

fn main(): i32 -> 0
EOF

write_manifest "$SMOKE_DIR/neg_mod_missing.manifest" "$SMOKE_DIR/neg_mod_missing.ll" "$SMOKE_DIR/main.dao"
run_compiler "$RUN_DIR/stage3" "$SMOKE_DIR/neg_mod_missing.manifest" "$SMOKE_DIR/neg_mod_missing.log"
neg_mod_missing_rc=$?

write_manifest "$SMOKE_DIR/neg_mod_dup.manifest" "$SMOKE_DIR/neg_mod_dup.ll" "$SMOKE_DIR/lib.dao" "$SMOKE_DIR/dup_lib.dao" "$SMOKE_DIR/main.dao"
run_compiler "$RUN_DIR/stage3" "$SMOKE_DIR/neg_mod_dup.manifest" "$SMOKE_DIR/neg_mod_dup.log"
neg_mod_dup_rc=$?

write_manifest "$SMOKE_DIR/neg_mod_col.manifest" "$SMOKE_DIR/neg_mod_col.ll" "$SMOKE_DIR/col_a.dao" "$SMOKE_DIR/col_b.dao" "$SMOKE_DIR/col_main.dao"
run_compiler "$RUN_DIR/stage3" "$SMOKE_DIR/neg_mod_col.manifest" "$SMOKE_DIR/neg_mod_col.log"
neg_mod_col_rc=$?

write_manifest "$SMOKE_DIR/neg_mod_cyc.manifest" "$SMOKE_DIR/neg_mod_cyc.ll" "$SMOKE_DIR/cyc_a.dao" "$SMOKE_DIR/cyc_b.dao"
run_compiler "$RUN_DIR/stage3" "$SMOKE_DIR/neg_mod_cyc.manifest" "$SMOKE_DIR/neg_mod_cyc.log"
neg_mod_cyc_rc=$?

if [ "$neg_mod_missing_rc" -ne 1 ] || [ "$neg_mod_dup_rc" -ne 1 ] || [ "$neg_mod_col_rc" -ne 1 ] || [ "$neg_mod_cyc_rc" -ne 1 ] ||
   [ -e "$SMOKE_DIR/neg_mod_missing.ll" ] || [ -e "$SMOKE_DIR/neg_mod_dup.ll" ] || [ -e "$SMOKE_DIR/neg_mod_col.ll" ] || [ -e "$SMOKE_DIR/neg_mod_cyc.ll" ] ||
   ! grep -q "smoke::lib" "$SMOKE_DIR/neg_mod_missing.log" ||
   ! grep -q "smoke::lib" "$SMOKE_DIR/neg_mod_dup.log" ||
   ! grep -q "util" "$SMOKE_DIR/neg_mod_col.log" ||
   ! grep -q "cyc::" "$SMOKE_DIR/neg_mod_cyc.log"; then
  echo "stage 3 module negative checks failed (missing=$neg_mod_missing_rc dup=$neg_mod_dup_rc col=$neg_mod_col_rc cyc=$neg_mod_cyc_rc)" >&2
  exit 1
fi
echo "stage 3: linked binary passed smoke, order-determinism, and fail-closed checks"

suite_sources() {
  if [ "$1" = "lexer" ]; then
    printf '%s\n' bootstrap/lexer/tests.dao bootstrap/lexer/impl.dao bootstrap/ascii.dao
  elif [ "$1" = "parser" ]; then
    printf '%s\n' bootstrap/parser/tests.dao bootstrap/parser/impl.dao bootstrap/lexer/impl.dao bootstrap/ascii.dao
  elif [ "$1" = "graph" ]; then
    printf '%s\n' bootstrap/graph/tests.dao bootstrap/graph/impl.dao bootstrap/parser/impl.dao bootstrap/lexer/impl.dao bootstrap/ascii.dao
  elif [ "$1" = "resolver" ]; then
    printf '%s\n' bootstrap/resolver/tests.dao bootstrap/resolver/impl.dao bootstrap/shared/base.dao bootstrap/graph/impl.dao bootstrap/parser/impl.dao bootstrap/lexer/impl.dao bootstrap/ascii.dao
  elif [ "$1" = "typecheck" ]; then
    printf '%s\n' bootstrap/typecheck/tests.dao bootstrap/typecheck/impl.dao bootstrap/resolver/impl.dao bootstrap/shared/base.dao bootstrap/graph/impl.dao bootstrap/parser/impl.dao bootstrap/lexer/impl.dao bootstrap/ascii.dao
  elif [ "$1" = "hir" ]; then
    printf '%s\n' bootstrap/hir/tests.dao bootstrap/hir/impl.dao bootstrap/typecheck/impl.dao bootstrap/resolver/impl.dao bootstrap/shared/base.dao bootstrap/graph/impl.dao bootstrap/parser/impl.dao bootstrap/lexer/impl.dao bootstrap/ascii.dao
  elif [ "$1" = "mir" ]; then
    printf '%s\n' bootstrap/mir/tests.dao bootstrap/mir/impl.dao bootstrap/hir/impl.dao bootstrap/typecheck/impl.dao bootstrap/resolver/impl.dao bootstrap/shared/base.dao bootstrap/graph/impl.dao bootstrap/parser/impl.dao bootstrap/lexer/impl.dao bootstrap/ascii.dao
  elif [ "$1" = "driver_tests" ]; then
    printf '%s\n' "$GEN_DIR/driver/driver_tests.gen.dao" bootstrap/graph/impl.dao bootstrap/parser/impl.dao bootstrap/lexer/impl.dao bootstrap/ascii.dao
  else
    printf '%s\n' "$GEN_DIR/$1/$1.gen.dao" bootstrap/mir/impl.dao bootstrap/hir/impl.dao bootstrap/typecheck/impl.dao bootstrap/resolver/impl.dao bootstrap/shared/base.dao bootstrap/graph/impl.dao bootstrap/parser/impl.dao bootstrap/lexer/impl.dao bootstrap/ascii.dao
  fi
}

echo "the suites, compiled by the stage-2 compiler"
failures=0
SUITES_JSON=""
for p in lexer parser graph resolver typecheck hir mir llvm driver_tests; do
  # shellcheck disable=SC2046
  if ! compile_with "$RUN_DIR/stage2" "$RUN_DIR/$p.ll" $(suite_sources "$p"); then
    echo "  $p: did not compile (see $RUN_DIR/$p.ll.log)"
    failures=$((failures + 1))
    SUITES_JSON="${SUITES_JSON}${SUITES_JSON:+, }\"$p\": {\"passed\": 0, \"total\": 0, \"exit_code\": 1, \"ok\": false}"
    continue
  fi
  if ! link "$RUN_DIR/$p.ll" "$RUN_DIR/$p"; then
    echo "  $p: does not link (see $RUN_DIR/$p.ll.clang)"
    failures=$((failures + 1))
    SUITES_JSON="${SUITES_JSON}${SUITES_JSON:+, }\"$p\": {\"passed\": 0, \"total\": 0, \"exit_code\": 1, \"ok\": false}"
    continue
  fi
  if [ "$p" = "llvm" ]; then
    "$RUN_DIR/$p" "$LLVM_OUT" > "$RUN_DIR/$p.run" 2>&1
  elif [ "$p" = "driver_tests" ]; then
    "$RUN_DIR/$p" "$DRIVER_TEST_WORK" > "$RUN_DIR/$p.run" 2>&1
  else
    "$RUN_DIR/$p" > "$RUN_DIR/$p.run" 2>&1
  fi
  status=$?
  result="$(grep -oE '[0-9]+ / [0-9]+ (tests )?passed' "$RUN_DIR/$p.run" | tail -1)"
  echo "  $p: ${result:-no result}$([ "$status" -ne 0 ] && echo ", exit $status")"
  passed="$(echo "$result" | awk '{print $1}')"
  total="$(echo "$result" | awk '{print $3}')"
  suite_ok="true"
  if [ "$status" -ne 0 ] || [ -z "$result" ] || [ "$passed" != "$total" ]; then
    failures=$((failures + 1))
    suite_ok="false"
  fi
  SUITES_JSON="${SUITES_JSON}${SUITES_JSON:+, }\"$p\": {\"passed\": ${passed:-0}, \"total\": ${total:-0}, \"exit_code\": $status, \"ok\": $suite_ok}"
done

bash bootstrap/validate_ir.sh "$BUILD_DIR" "$LLVM_OUT" > "$RUN_DIR/validate_ir.log" 2>&1
validate_status=$?
sed 's/^/  /' "$RUN_DIR/validate_ir.log"
[ "$validate_status" -eq 0 ] || failures=$((failures + 1))

val_line="$(grep -E '^validate_ir: [0-9]+ IR files checked' "$RUN_DIR/validate_ir.log" | tail -1)"
val_checked="$(echo "$val_line" | awk '{print $2}')"
val_accepted="$(echo "$val_line" | awk '{print $6}')"
val_executed="$(echo "$val_line" | awk '{print $10}')"
val_failures="$(echo "$val_line" | awk '{print $12}')"

# Compute source-closure digest over every authored file feeding the crossing.
{
  for src in $(ls stdlib/core/*.dao stdlib/io/*.dao bootstrap/ascii.dao bootstrap/shared/base.dao bootstrap/*/impl.dao bootstrap/*/tests.dao bootstrap/driver/main.dao | LC_ALL=C sort -u); do
    printf '%s  %s\n' "$(sha256_file "$src")" "$src"
  done
} > "$RUN_DIR/source_closure.sha256"
SOURCE_CLOSURE_SHA="$(sha256_file "$RUN_DIR/source_closure.sha256")"
STAGE1_SHA="$(sha256_file "$STAGE1")"
RUNTIME_SHA="$(sha256_file "$RUNTIME")"
STAGE2_LL_SHA="$(sha256_file "$RUN_DIR/stage2.ll")"
STAGE3_LL_SHA="$(sha256_file "$RUN_DIR/stage3.ll")"
STAGE2_BIN_SHA="$(sha256_file "$RUN_DIR/stage2")"
STAGE3_BIN_SHA="$(sha256_file "$RUN_DIR/stage3")"
if [ -f "$DAOC" ]; then
  DAOC_SHA_JSON="\"$(sha256_file "$DAOC")\""
else
  DAOC_SHA_JSON="null"
fi
GIT_HEAD="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
CLANG_VERSION="$("$CLANG" --version 2>/dev/null | head -1)"
if [ "$HOST_OS" = "Darwin" ]; then
  UNVERIFIED_TARGETS='["linux-x86_64"]'
else
  UNVERIFIED_TARGETS='["darwin-arm64"]'
fi
OVERALL_OK="true"
[ "$failures" -eq 0 ] || OVERALL_OK="false"

cat > "$RUN_DIR/result.json" <<EOF
{
  "ok": $OVERALL_OK,
  "git_head": "$(json_escape "$GIT_HEAD")",
  "run_dir": "$(json_escape "$RUN_DIR")",
  "host": {
    "os": "$(json_escape "$HOST_OS")",
    "arch": "$(json_escape "$HOST_ARCH")",
    "clang": "$(json_escape "$CLANG_VERSION")",
    "unverified_targets": $UNVERIFIED_TARGETS
  },
  "seed": {
    "kind": "$SEED_KIND",
    "stage1_path": "$(json_escape "$STAGE1")",
    "stage1_sha256": "$STAGE1_SHA",
    "daoc_sha256": $DAOC_SHA_JSON,
    "runtime_sha256": "$RUNTIME_SHA",
    "source_closure_sha256": "$SOURCE_CLOSURE_SHA"
  },
  "limits": {
    "requested_stack_kb": $(json_limit_value "$REQ_STACK_KB"),
    "initial_stack_kb": $(json_limit_value "$INITIAL_STACK_KB"),
    "hard_stack_kb": $(json_limit_value "$HARD_STACK_KB"),
    "applied_stack_kb": $(json_limit_value "$APPLIED_STACK_KB"),
    "stack_mode": "$STACK_MODE",
    "requested_vmem_kb": $(json_limit_value "$LIMIT_KB"),
    "applied_vmem_kb": $(json_limit_value "$APPLIED_VMEM_JSON"),
    "vmem_mode": "$VMEM_MODE"
  },
  "crossing": {
    "fixed_point": true,
    "stage2_ll_sha256": "$STAGE2_LL_SHA",
    "stage3_ll_sha256": "$STAGE3_LL_SHA",
    "stage2_bin_sha256": "$STAGE2_BIN_SHA",
    "stage3_bin_sha256": "$STAGE3_BIN_SHA",
    "stage3_smoke_ok": true,
    "stage3_negative_checks_ok": true
  },
  "suites": {
    $SUITES_JSON
  },
  "validate_ir": {
    "ok": $([ "$validate_status" -eq 0 ] && echo true || echo false),
    "checked": ${val_checked:-0},
    "accepted": ${val_accepted:-0},
    "executed": ${val_executed:-0},
    "failures": ${val_failures:-1}
  }
}
EOF

echo "evidence: $RUN_DIR/result.json"
if [ "$failures" -ne 0 ]; then
  echo "$failures suite(s) fail under stage 2" >&2
  exit 1
fi
echo "bootstrapped: every suite passes under stage 2"
