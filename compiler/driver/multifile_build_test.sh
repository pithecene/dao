#!/usr/bin/env bash
# End-to-end multi-file compilation: root-file discovery and explicit
# file lists build to executables with the expected exit code, and the emitted IR of an
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

# The other two shared fixtures: both compilers read them,
# neither task edits them.  Each is a library set with no `fn main`, so it
# is CHECKED rather than built.  An explicit file set owes an entry
# module under every command (CONTRACT_MODULE_SYSTEM.md §8.3), and these
# fixtures are libraries, so each is checked alongside a scratch entry.
for fixture in cross_module_enum extend_isolation; do
  dir="$ROOT/testdata/bootstrap/multifile/$fixture"
  printf 'module fixture_main\n\nfn main(): i32\n  return 0\n' > "$WORK/$fixture-main.dao"
  args="--source $WORK/$fixture-main.dao"
  for f in "$dir"/*.dao; do args="$args --source $f"; done
  # shellcheck disable=SC2086
  if ! "$DAOC" check $args > "$WORK/$fixture.txt" 2>&1; then
    fail "$fixture: daoc check failed: $(tail -2 "$WORK/$fixture.txt")"
  fi
  grep -q 'error:' "$WORK/$fixture.txt" && fail "$fixture: reported an error: $(grep -m1 'error:' "$WORK/$fixture.txt")"
done

# Link inputs pass through unchanged, dash-led ones included,
# and `--` ends option parsing.  A bad link input reaches the linker, which
# is where it is diagnosed — the driver does not reject it.
out=$("$DAOC" build --source "$BM/app_main.dao" --source "$BM/app_math.dao" --source "$BM/core_fmt.dao" -Wl,--no-such-option 2>&1) || true
echo "$out" | grep -q 'unknown option' && fail "build rejected a link input instead of forwarding it"
out=$("$DAOC" build --source "$BM/app_main.dao" --source "$BM/app_math.dao" --source "$BM/core_fmt.dao" -- -Wl,--no-such-option 2>&1) || true
echo "$out" | grep -q 'unknown option' && fail "build rejected a link input after --"
# The analysis commands still reject one.
"$DAOC" check --source "$BM/core_fmt.dao" --bogus > "$WORK/bogus.txt" 2>&1 && fail "check accepted an unknown option"
grep -q 'unknown option' "$WORK/bogus.txt" || fail "check did not name the unknown option"

# Diagnostics come out in source order, not grouped by phase (§8.4).
mkdir -p "$WORK/order"
printf 'module first\nfn f(): i32\n  return @\n' > "$WORK/order/first.dao"
printf 'module second\nfn g(): i32\n  return @\n' > "$WORK/order/second.dao"
"$DAOC" check --source "$WORK/order/first.dao" --source "$WORK/order/second.dao" > "$WORK/order.txt" 2>&1 || true
first_line=$(grep -n 'first.dao' "$WORK/order.txt" | head -1 | cut -d: -f1)
second_line=$(grep -n 'second.dao' "$WORK/order.txt" | head -1 | cut -d: -f1)
if [ -n "$first_line" ] && [ -n "$second_line" ] && [ "$first_line" -gt "$second_line" ]; then
  fail "diagnostics are not in file order: first.dao reported after second.dao"
fi

# A directory where a source file belongs is reported, not crashed on:
# opening one succeeds and only the first read fails.
"$DAOC" check --source "$BM" > "$WORK/dir.txt" 2>&1 && fail "check accepted a directory as a source"
grep -q 'not a source file' "$WORK/dir.txt" || fail "a directory input was not reported: $(head -1 "$WORK/dir.txt")"
grep -q 'terminate called' "$WORK/dir.txt" && fail "a directory input crashed the driver"

# The default output name is a function of the file set, not of the
# order the files are given in or the way their paths are spelled.
build_out() {
  "$DAOC" build "$@" > "$WORK/named.txt" || fail "build failed: $(tail -2 "$WORK/named.txt")"
  realpath "$(tail -1 "$WORK/named.txt")"
}
plain=$(build_out --source "$BM/app_main.dao" --source "$BM/app_math.dao" --source "$BM/core_fmt.dao")
permuted=$(build_out --source "$BM/core_fmt.dao" --source "$BM/app_math.dao" --source "$BM/app_main.dao")
detour=$(build_out --source "$BM/../smoke/app_main.dao" --source "$BM/app_math.dao" --source "$BM/core_fmt.dao")
[ "$plain" = "$permuted" ] || fail "output name changed with input order: $plain vs $permuted"
[ "$plain" = "$detour" ] || fail "output name changed with path spelling: $plain vs $detour"
rm -f "$plain"

# Every command that reports puts its diagnostics in file order, not in
# the order its phase produced them: `a.dao` imports `b.dao`, so the
# resolver reaches b first while a reader reaches a first.
mkdir -p "$WORK/phase"
printf 'module a\nimport b\n\nfn dup(): i32 -> 1\nfn dup(): i32 -> 2\n\nfn main(): i32\n  return 0\n' > "$WORK/phase/a.dao"
printf 'module b\n\nfn twin(): i32 -> 1\nfn twin(): i32 -> 2\n' > "$WORK/phase/b.dao"
for cmd in resolve check; do
  "$DAOC" "$cmd" --source "$WORK/phase/a.dao" --source "$WORK/phase/b.dao" --entry a \
    > "$WORK/phase-$cmd.txt" 2>&1 || true
  first=$(grep -n 'a.dao.*duplicate' "$WORK/phase-$cmd.txt" | head -1 | cut -d: -f1)
  second=$(grep -n 'b.dao.*duplicate' "$WORK/phase-$cmd.txt" | head -1 | cut -d: -f1)
  [ -n "$first" ] && [ -n "$second" ] || fail "$cmd did not report both files: $(cat "$WORK/phase-$cmd.txt")"
  [ "$first" -lt "$second" ] || fail "$cmd reported b.dao before a.dao (topological, not file, order)"
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
