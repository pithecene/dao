# Idiomaticity, Repetition, and Parity Audit

This note is explanatory. It records the `DAO-H01` audit across all 97 authored
Dao files (`32,245` lines, excluding generated `*.gen.dao` outputs and
`testdata/` fixtures) alongside empirical parity probes compiled through both
`daoc` and the self-hosted Stage-2 compiler (`stage2`).

Around 70% of the structural repetition in `bootstrap/` needs zero grammar
changes and zero new compiler features. Both compilers already support local
`let` inference, contextual integer literals in calls and constructors, direct
fieldless `enum` equality, nested value-semantic field assignment, `for` loops,
and multi-line parenthesized expressions.

## Corpus Repetition Measurements

| Subsystem | Files | Lines | `to_i64(<lit>)` | `let x: T =` | `let x =` | `while` | Indexed `while` | `for` | `Vector::new()` | `== true/false` | Field `a.b =` | `*_set_*` Clones | `> 100` col (`> 140`) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `bootstrap/shared` | 1 | 104 | 1 | 4 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 1 | 6 (3) |
| `bootstrap/ascii` | 1 | 26 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 (0) |
| `bootstrap/lexer` | 2 | 1,124 | 28 | 96 | 0 | 11 | 2 | 0 | 5 | 4 | 43 | 0 | 14 (1) |
| `bootstrap/parser` | 2 | 2,735 | 45 | 628 | 0 | 42 | 7 | 0 | 36 | 50 | 0 | 4 | 46 (9) |
| `bootstrap/graph` | 2 | 621 | 33 | 138 | 0 | 33 | 24 | 0 | 28 | 3 | 0 | 0 | 28 (7) |
| `bootstrap/resolver` | 2 | 2,786 | 46 | 595 | 0 | 69 | 51 | 0 | 51 | 3 | 15 | 0 | 134 (27) |
| `bootstrap/typecheck` | 2 | 7,880 | 410 | 1,403 | 0 | 156 | 149 | 0 | 115 | 44 | 86 | 3 | 606 (358) |
| `bootstrap/hir` | 2 | 2,944 | 141 | 570 | 0 | 73 | 69 | 0 | 48 | 9 | 4 | 0 | 151 (60) |
| `bootstrap/mir` | 2 | 3,734 | 428 | 734 | 0 | 110 | 107 | 0 | 109 | 12 | 133 | 2 | 267 (117) |
| `bootstrap/llvm` | 2 | 4,022 | 429 | 643 | 0 | 76 | 65 | 0 | 59 | 26 | 58 | 3 | 221 (85) |
| `bootstrap/driver` | 3 | 377 | 39 | 58 | 0 | 4 | 2 | 0 | 6 | 8 | 0 | 0 | 23 (7) |
| `stdlib/` | 22 | 1,130 | 73 | 98 | 0 | 19 | 11 | 0 | 2 | 2 | 0 | 0 | 13 (0) |
| `examples/` | 49 | 4,697 | 152 | 574 | 19 | 71 | 26 | 24 | 51 | 73 | 4 | 0 | 71 (16) |
| `spec/` | 5 | 65 | 0 | 1 | 4 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 (0) |
| **Total** | **97** | **32,245** | **1,825** | **5,542** | **23** | **665** | **513** | **24** | **510** | **234** | **344** | **13** | **1,580 (690)** |

Additional structural counts across `bootstrap/`:

- **Stringified enum comparisons** (`tk_name(...) == "..."`, `symbol_kind_name(...) == "..."`): `78` sites.
- **Composite string keys for `HashMap<i64>`** (`i64_to_string(a) + ":" + i64_to_string(b)`): `269` sites.
- **Two-letter state classes**: `10` classes (`PC`, `PS`, `PR`, `RC`, `RS`, `TC`, `TS`, `HS`, `MS`, `BS`).
- **Whole-aggregate reconstruction wrappers** (`ps_set_*`, `reg_with_*`, `ctx_with_*`, `bs_set_*`): `24+` helpers.

## State Since the Audit

The counts and the parity matrix in this note are the audit's
measurements, and they stay as the audit took them. This section covers
six findings that have since changed, and it does not survey the rest:

- **Map keys.** `HashMap<K, V>` now takes any key that is `Hashable` and
  `Equatable`, so the `IntMap<V>` that this note recommends has no
  purpose. The resolver's two program tables take a class of two `i64`
  fields as their key (`UseSite`, `ExtendSite`). Such a class derives
  both concepts because each of its fields conforms, and because no
  explicit implementation of either concept exists for it, in its body
  or in an `extend` block. The other composite string keys still wait
  for the same treatment.
- **`Vector<T>` operations.** `stdlib/core/vector.dao` now has the seven
  operations that this note recommends.
- **Payload `enum class` equality.** The bootstrap type checker rejects
  `==` and `!=` on a payload-bearing `enum class`.
- **Partial destructuring.** The bootstrap compiler compiles
  `Node::CallE(callee, ..)`.
- **Try propagation.** The audit's probe now passes: the bootstrap
  compiler compiles `operand?` on an `Option` or a `Result`, into a
  function that returns the same declaration. For a `Result`, the
  function's error type must equal the operand's, and the two success
  types may differ. The compiler still rejects `?` in a lambda whose
  return type it infers from the body.
- **Pipeline.** The bootstrap compiler compiles `left |> right`, for a
  target of one parameter.

The type checker, the HIR pass, the MIR pass, the LLVM backend, and the
driver are now the real modules `bootstrap::typecheck`, `bootstrap::hir`,
`bootstrap::mir`, `bootstrap::llvm`, and `bootstrap::driver`.  No script
concatenates them any more.

## Native vs. Bootstrap Parity Matrix

Each probe below ran through both `daoc` (`compiler/`) and `stage2` (`bootstrap/`):

| Area | Probe | Native `daoc` | Self-Hosted `stage2` | Bucket |
|---|---|---|---|---|
| Local inference | `let x = f()` | Pass | Pass | Refactorable today |
| Open enum context | `let o: Option<i64> = Option::None` | Pass | Pass | Refactorable today |
| Concrete integer sinks | `let x: i64 = 0`, `Span(0, 1)`, `v.get(0)`, `v_i64.push(0)`, `hm.set("a", 0)` | Pass | Pass | Refactorable today |
| Expression-body / `yield` / generic variant literals | `fn f(): i64 -> 0`, `yield 0`, `Option::Some(value = 0)` under `Option<i64>` | Fail (`expr_fn`, `yield`, variant) | `expr_fn` Pass, `yield` and variant Fail | Contract + parity (`DAO-H04`) |
| Collection literals | `let xs: Vector<i64> = [1, 2, 3]`, `[]` | Fail (`type_checker.cpp`) | Fail (`hir/impl.dao`) | Proposal + compiler (`spec/semantics/`) |
| Aggregate initialization | `LexState(cursor, diags = d)` | Parsed, rejected in `typecheck` | Parsed, rejected in `typecheck` | Proposal + compiler (`spec/semantics/`) |
| Nested field mutation | `s.pos = s.pos + 1`, `s.inner.x = 2` | Pass | Pass | Refactorable today |
| Partial destructuring | `Node::CallE(callee, ..)` | Pass | Fail (`parser` + `typecheck`) | Bootstrap parity (`DAO-H04`) |
| Wildcard pattern | `_:` arm and `_` binding | Fail (`unknown name '_'`) | Fail (`unknown name '_'`) | Proposal + compiler (`spec/semantics/`) |
| Fieldless `enum` `==` | `tok.kind == TK::Plus` | Pass | Pass | Refactorable today |
| Payload `enum class` `==` | `E::A(x = 1) == E::A(x = 1)` | Rejected (`§13.1`) | Defect: accepted in `typecheck` | Immediate correctness fix |
| Generator `for` loop | `for x in vec.iter():`, `for i in range(n):` | Pass | Pass | Refactorable today |
| Try propagation (`?`) | `let v = fallible()?` | Pass | Fail (`hir/impl.dao`) | Bootstrap parity (`DAO-H04`) |
| Pipeline (`\|>`) | `0 \|> inc` | Pass | Fail (`mir/impl.dao`) | Bootstrap parity (`DAO-H04`) |
| Multi-line parentheses | Newlines inside `(...)` | Pass | Pass | Refactorable today |

## Three-Bucket Classification

### Refactorable Today

1. **Redundant `to_i64(<literal>)` wrappers (`~1,550+` of `1,825` sites):** `program_check` now accumulates `type_info` across all modules before checking bodies, so prelude calls (`Vector::push`, `Span`, `substring`, `HashMap::set`) already fit integer literals to `i64`.
2. **Repeated local type annotations (`4,869` sites in `bootstrap/`):** `let x = expr` already infers concrete types wherever the initializer is non-literal.
3. **Whole-aggregate setter clones and two-letter state names:** direct value-semantic field assignment (`state.pos = state.pos + 1`) already works in both compilers.
4. **Stringified enum comparisons (`78` sites):** fieldless `enum` values already support `==` and `!=` directly.
5. **Indexed `while` scans (`513` sites), `== false` tests (`234` sites), and `> 100`-column lines (`1,580` sites):** `for x in vec.iter():`, `!flag`, and parenthesized line breaks work today.

### Foundational Standard Library (`DAO-H03`)

1. **`Vector<T>` operations:** `filled(count, value)`, `with_capacity`, `is_empty`, `last`, `pop`, `reverse`, and `append_all` retire manual flag loops and cursor stacks.
2. **`IntMap<V>` and sorting.** `HashMap<K, V>` has superseded the map, as State Since the Audit records. At the audit, `stdlib/core/hashmap.dao` fixed keys to `string`, forcing `269` `"a:b"` string allocations across hot compiler passes and inflating self-hosted RSS. A deterministic `i64`-keyed map and standard `O(n log n)` sort retire ad hoc string keys and quadratic insertion sorts.
3. **`Option<T>` and `Result<T, E>` methods:** both files currently hold 4-line declarations with zero methods (`is_some`, `is_none`, `is_ok`, `is_err`, `unwrap_or`).
4. **String and `Builder` composition:** `split`, `trim`, `join`, `parse_i64`, `parse_i32`, and `Builder.push_i64` replace `1,750` `+` concatenations and bespoke integer parsers.

### Compiler Parity (`DAO-H04`) and Semantic Proposals

1. **Immediate correctness repair:** `bootstrap/typecheck/impl.dao` must reject `==` and `!=` on payload-bearing `enum class` types to match `CONTRACT_TYPE_SYSTEM_FOUNDATIONS.md` §13.1.
2. **Bootstrap parity (`DAO-H04`):** implement `..` rest destructuring (`3,758` match arms), `?` error propagation, and `|>` MIR lowering in `bootstrap/`.
3. **Semantic proposals (`spec/semantics/`):** `contextual_and_collection_literals.md` and `aggregate_initialization.md` specify contextual literal sinks, homogeneous `Vector<T>` literals, `_` wildcards, and dotless structural aggregate initialization (`Point(x, y)`, `Point(x = 0, y = 0)`) for promotion to `docs/contracts/` upon acceptance.

## Delivery Sequence

1. Repair bootstrap payload-enum equality immediately alongside compiler module extraction (`DAO-R03` through `DAO-R05`).
2. Land `DAO-H03` (`stdlib`) and `DAO-H04` (bootstrap parity and approved literal/pattern contracts) in bounded pull requests before the repository-wide `DAO-H02` sweep.
3. Execute `DAO-H02` and `DAO-H05` module-by-module, now that `assemble.sh` concatenation has retired, followed by automated structural checks (`DAO-H06`).
