#!/usr/bin/env bash
# End-to-end multi-file compilation (Task 31 acceptance §19.1, §19.2,
# §19.8): root-file discovery and explicit file lists build to
# executables with the expected exit code, and the emitted IR of an
# explicit set is byte-identical under input-order permutation.
#   multifile_build_test.sh <daoc> <repo root>
set -u
DAOC="$1"
ROOT="$2"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
failures=0

fail() { echo "FAIL: $*"; failures=$((failures + 1)); }

# Root-file mode: testdata/module/smoke — three user modules across two
# directories plus a prelude import; main returns math::add(1, 2).
cp -r "$ROOT/testdata/module/smoke" "$WORK/smoke"
if "$DAOC" build "$WORK/smoke/main.dao" > /dev/null; then
  "$WORK/smoke/main"; code=$?
  [ "$code" -eq 3 ] || fail "smoke: expected exit 3, got $code"
else
  fail "smoke: daoc build failed"
fi

# Explicit mode: the bootstrap's own fixture; main returns math::add(1, 2).
BM="$ROOT/testdata/bootstrap/multifile/smoke"
if "$DAOC" build --source "$BM/app_main.dao" --source "$BM/app_math.dao" --source "$BM/core_fmt.dao" > "$WORK/out.txt"; then
  exe="$(tail -1 "$WORK/out.txt")"
  "$exe"; code=$?
  [ "$code" -eq 3 ] || fail "bootstrap smoke: expected exit 3, got $code"
  rm -f "$exe"
else
  fail "bootstrap smoke: daoc build failed"
fi

# The other two shared fixtures (Task 31 §16.4): both compilers read them,
# neither task edits them.  Each is a library set with no `fn main`, so it
# is CHECKED rather than built — a missing entry is a warning for analysis
# and an error only when producing an executable.
for fixture in cross_module_enum extend_isolation; do
  dir="$ROOT/testdata/bootstrap/multifile/$fixture"
  args=""
  for f in "$dir"/*.dao; do args="$args --source $f"; done
  # shellcheck disable=SC2086
  if ! "$DAOC" check $args > "$WORK/$fixture.txt" 2>&1; then
    fail "$fixture: daoc check failed: $(tail -2 "$WORK/$fixture.txt")"
  fi
  grep -q 'error:' "$WORK/$fixture.txt" && fail "$fixture: reported an error: $(grep -m1 'error:' "$WORK/$fixture.txt")"
done

# Determinism: the same explicit set in two orders emits identical IR.
"$DAOC" llvm-ir --source "$BM/app_main.dao" --source "$BM/app_math.dao" --source "$BM/core_fmt.dao" > "$WORK/a.ll" || fail "llvm-ir (order 1) failed"
"$DAOC" llvm-ir --source "$BM/core_fmt.dao" --source "$BM/app_math.dao" --source "$BM/app_main.dao" > "$WORK/b.ll" || fail "llvm-ir (order 2) failed"
cmp -s "$WORK/a.ll" "$WORK/b.ll" || fail "emitted IR differs under input-order permutation"

# Module-qualified names: both user modules' functions are present and
# only the entry module's main is `main`.
grep -q 'define .*@"app::math::add"' "$WORK/a.ll" || fail "app::math::add not mangled by module"
grep -q 'define .*@main(' "$WORK/a.ll" || fail "entry main is not named main"

if [ "$failures" -ne 0 ]; then
  exit 1
fi
echo "ok"
