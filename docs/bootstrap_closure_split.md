# Closure Split — Compiler, Test Harness, Stdlib

Measurement only.  It exists so the decision about what the bootstrap
compiles next rests on the split rather than on a single aggregate
number.  It is the opening measurement of the authorized corpus/source
hygiene task: that task acts on this split (physical implementation /
test separation, retiring the marker-based source slicing); this
document does not itself change any source.

## 1. Why the split matters

`bootstrap/audit_closure.sh` measures the **assembled** programs under
`bootstrap/*/*.gen.dao`.  Each assembled program carries its subsystem's
test harness, because `assemble.sh` concatenates the whole subsystem
file.  The audit therefore reports one closure figure over two
populations with different standing:

- **compiler implementation closure** — what the Stage-2 compiler must
  compile in order to compile Dao;
- **bootstrap test-harness closure** — what the bootstrap's own suites
  use to report pass and fail.

A feature required only by the second is a poor reason to move a
feature ahead of the first.

## 2. Method

Each subsystem file is cut at its `// BEGIN_<X>_TESTS` marker — the
same line `assemble.sh` cuts at when it takes a subsystem's library
portion for a downstream program.  Text before the marker is library;
text from the marker on is harness; `*/tests.dao` is harness entire;
`shared/base.dao` is library entire.  Comments are stripped before
counting, and a call site is a prelude name followed by `(` that is
neither a method selector nor part of a longer identifier.

Reproduced with:

```sh
python3 - <<'EOF'
import pathlib, re
prelude = {}
for p in sorted(pathlib.Path("stdlib/core").glob("*.dao")) + sorted(pathlib.Path("stdlib/io").glob("*.dao")):
    for m in re.finditer(r'^\s*(?:extern\s+)?fn\s+([a-z_][A-Za-z0-9_]*)', p.read_text(), re.M):
        prelude.setdefault(m.group(1), str(p))
strip = lambda s: re.sub(r'//.*$', '', s, flags=re.M)
lib, test = {}, {}
root = pathlib.Path("bootstrap")
for p in sorted(root.glob("*/impl.dao")) + sorted(root.glob("*/tests.dao")) + [root/"shared/base.dao"]:
    text = p.read_text()
    m = re.search(r'^// BEGIN_[A-Z]+_TESTS\s*$', text, re.M)
    if p.name == "tests.dao":     a, b = "", text
    elif m:                       a, b = text[:m.start()], text[m.start():]
    else:                         a, b = text, ""
    for name in prelude:
        pat = re.compile(r'(?<![A-Za-z0-9_.])' + re.escape(name) + r'\s*\(')
        lib[name]  = lib.get(name, 0)  + len(pat.findall(strip(a)))
        test[name] = test.get(name, 0) + len(pat.findall(strip(b)))
for n in sorted(prelude, key=lambda n: -(lib[n]+test[n])):
    if lib[n] or test[n]: print(f"{n:<22}{lib[n]:>7}{test[n]:>7}  {prelude[n]}")
EOF
```

## 3. Prelude call sites, library against harness

| Prelude function | Library | Harness | Declared in |
|---|---:|---:|---|
| `to_i64` | 617 | 148 | `core/convert.dao` |
| `print` | **0** | **569** | `core/printable.dao` |
| `new` | 222 | 83 | `core/hashmap.dao`, `core/vector.dao` |
| `i64_to_string` | 42 | 144 | `core/to_string.dao` |
| `substring` | 26 | 11 | `core/string.dao` |
| `char_at` | 22 | 0 | `core/string.dao` |
| `read_file` | 0 | 19 | `io/file.dao` |
| `make_error` | 16 | 0 | `core/diagnostic.dao` |
| `file_exists` | 0 | 16 | `io/file.dao` |
| `length` | 4 | 11 | `core/hashmap.dao`, `core/vector.dao` |
| `i32_to_string` | 0 | 15 | `core/to_string.dao` |
| `to_string` | 0 | 11 | `core/printable.dao` |
| `eprint` | 0 | 11 | `io/file.dao` |
| `index_of` | 0 | 8 | `core/string.dao` |
| `write_file` | 1 | 2 | `io/file.dao` |
| `starts_with` | 2 | 0 | `core/string.dao` |
| `str_compare` | 1 | 0 | `core/string.dao` |
| `size_of`, `ptr_offset`, `copy_out` | 0 | 1 each | `core/builtins.dao` |
| `eq`, `abs` | 0 | 2, 1 | `core/equatable.dao`, `core/math.dao` |
| **Total** | **953** | **1054** | |

Library-only: `char_at`, `make_error`, `starts_with`, `str_compare`.

Harness-only: `print`, `to_string`, `eprint`, `read_file`,
`file_exists`, `i32_to_string`, `index_of`, `eq`, `abs`, `size_of`,
`ptr_offset`, `copy_out`.

## 4. The finding

**`print` has zero call sites in compiler library code.**  All 569 are
harness reporting — `print("PASS ...")`, `print("FAIL ...")`, and the
closure probe's own output, which lives in `llvm/impl.dao`'s test
section.  The single library-looking hit at `typecheck/impl.dao:1473`
is a comment (`// D4: Module-qualified call (e.g. fmt::print(...))`)
and disappears once comments are stripped.

`to_string` is harness-only for the same reason.

This matters because `print` is declared as

```dao
derived concept Printable:
  fn to_string(self): string

fn print<T: Printable>(x: T): void -> __dao_io_write_stdout(x.to_string())
```

so the audit's remaining resolver family stands on a chain of
`derived concept`, generic parameter constraints, concept identity,
conformance of a concrete `T`, constrained method lookup for
`x.to_string()`, and specialization of a generic body — a chain the
Stage-2 *compiler* never exercises.  It enters the closure figure
entirely through test status strings.

## 5. What the compiler library actually asks of the prelude

Ranked by library call sites, the Stage-2 compiler path needs:

| Prelude file | Library surface used |
|---|---|
| `core/convert.dao` | `to_i64` (617) |
| `core/vector.dao`, `core/hashmap.dao` | `new`, `length`, plus method surface |
| `core/to_string.dao` | `i64_to_string` (42) |
| `core/string.dao` | `substring`, `char_at`, `starts_with`, `str_compare` |
| `core/diagnostic.dao` | `make_error` |
| `io/file.dao` | `write_file` (1) |

`core/printable.dao`, `core/math.dao`, `core/equatable.dao`,
`core/comparable.dao`, `core/range.dao` carry no library call site at
all.  Three of those five are exactly the files whose `derived concept`
the bootstrap parser rejects; a fourth, `math.dao`, is the file whose
generic bounds it rejects.

## 6. What this measurement does and does not establish

Established:

- the aggregate closure figure mixes two populations, and the larger
  share of the remaining prelude demand is the harness';
- the `print` dependency chain, the costliest semantic vertical the
  audit currently points at, is harness-only at the call-site level.

Open, and deliberately unanswered here:

- type-level demand is uncounted.  `Vector<T>`, `HashMap<V>`,
  `Option<T>`, `Result<T>`, `Span` are used throughout the library and
  carry their own generic and method requirements, which a call-site
  count of free functions misses entirely.  Generic support is
  therefore still a compiler-path requirement on its own evidence,
  independent of `print`.
- whether the harness should move to a lower-level reporting facility
  is a question about bootstrap architecture, with its own costs, and
  belongs to a decision that has yet to be taken.
- whether `audit_closure.sh` should report the split directly.  It
  currently does not; this document measures it by hand first, so the
  instrument changes only if the split proves worth carrying.

## 7. Where this leaves the audit

The matrix in `docs/bootstrap_closure.md` stays as it is: an
observation of the assembled programs, which is what the bootstrap
actually compiles today.  This document sits beside it so that a
reading of that matrix can separate what the compiler needs from what
its tests print.
