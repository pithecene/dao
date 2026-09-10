# Task 40 — Bootstrap Generic Parameter Bounds

Status: implementation spec
Phase: Tier B-Bootstrap, sequenced by the closure audit after Task 39
Scope: the bootstrap parser reads a bound on a generic parameter
(`fn print<T: Printable>(x: T): void`), so the declarations that carry
one reach the program instead of being lost to recovery

## 1. Objective

After Task 39 the compiler corpus's resolve column is one name.  Every
program's resolve diagnostics — 29 for `lexer`, 68 for `llvm` — read:

```
unknown name 'print'
```

`print` is a prelude declaration (`stdlib/core/printable.dao:54`):

```dao
fn print<T: Printable>(x: T): void -> __dao_io_write_stdout(x.to_string())
```

The bootstrap parser reads a type-parameter list (`<T>`) but not a
bound (`<T: Printable>`): it stops at the colon, the declaration is
lost to recovery, and 557 call sites across the corpus resolve to
nothing.  This task teaches the parser the bound, and the corpus's
resolve column closes.

## 2. Measured: one construct, one declaration the corpus needs

Fed the prelude group, the bootstrap parser today loses exactly the
declarations that carry a bound.  Measured with the closure probe over
purpose-built prelude modules — one declaring, one using:

| Construct | Cost | The declaration | Its neighbours |
|---|---|---|---|
| `fn f<T: Numeric>(x: T): T` | 18 parse diagnostics | lost (`unknown name 'f'` from a sibling module) | survive |
| `derived concept Shown:` | 1 parse diagnostic | lost | survive |

So recovery is not the problem — a bound costs its own declaration and
nothing else, and a rejected `derived concept` costs only the concept.

The whole prelude carries a bound on four declarations:

| Declaration | File | Corpus call sites |
|---|---|---|
| `print<T: Printable>` | `core/printable.dao` | **557** |
| `min<T: Numeric>` | `core/math.dao` | 0 |
| `max<T: Numeric>` | `core/math.dao` | 0 |
| `clamp<T: Numeric>` | `core/math.dao` | 0 |

One of the four is a name the compiler corpus needs, and its absence is
100% of the remaining resolve frontier.

The bound's own concept (`Printable`, `Numeric`) is declared with
`derived concept`, which the parser also rejects — so a bound will name
a type the resolver cannot find.  That is not an obstacle: an
unresolved type node is silent in the resolver by design ("No
diagnostic on unresolved type — matches host resolver"), so `print` is
declared, callable, and resolvable with its bound unresolved.  Closing
`derived concept` is a later slice, and the audit says when.

## 3. What the bootstrap has today

- `parse_type_params` (`shared/base.dao`) reads `<`, an identifier, a
  comma-separated tail, and `>`.  A colon after the identifier is
  unexpected, so the declaration fails and recovery skips it.
- `Node::GenericParamT(tok)` carries the parameter's name and nothing
  else.  Three consumers read it: the resolver (declares the parameter
  as a `GenericParam` symbol), the HIR builder (lowers the parameter
  list), and the AST printer's label.
- The host models the same thing as
  `GenericParam { name, name_span, constraints }`, parsing `T: C` and
  `T: C + D` (`compiler/frontend/parser/parser.cpp`,
  `parse_type_params`).

## 4. Design

### 4.1 Parser

`parse_type_params` reads, after a parameter's identifier, an optional
bound: `:` then a type, and `+` then a type while the token is `+` —
the host's grammar, no more.  Each bound is an ordinary type node, so
`T: Vector<i64>` and `T: m::C` cost nothing extra.

### 4.2 AST

`GenericParamT(tok, bounds_lp)`: the parameter's name token and the
list position of its bound type nodes, `-1` when it has none.  The
three consumers move with it in the same change; the AST printer's
label is unchanged.

### 4.3 Resolver

A parameter's bounds are type nodes of the scope the declaration sits
in, resolved with `resolve_type_node` before the parameter itself is
declared — a bound may not name the parameter it bounds.  An
unresolved bound stays silent, as every unresolved type node does.

### 4.4 Type checker and HIR

Out of scope: a bound is a conformance obligation, and the bootstrap
checks no conformance for generic parameters yet.  The type checker
and the HIR builder read the widened node and ignore the bounds, so a
bounded declaration types and lowers exactly as an unbounded one does
today.

## 5. Non-goals

- Checking that a type argument satisfies a bound; `where` clauses;
  variance; defaults.
- `derived concept`, `mode` blocks, `yield` — the prelude's other
  three parse gaps.  Each costs only its own declaration (§2), and the
  audit sequences them.
- Monomorphization, generic method typing, the prelude's bodies.
- The later-stage program pipeline; Tasks 32 and 33.

## 6. Delivery

Two PRs:

1. This spec; `docs/IMPLEMENTATION_PLAN.md` (Task 40 entry).
2. The implementation: §4, the tests of §7, `bootstrap/README.md`
   (the parser's Tier A coverage gains generic parameter bounds; the
   Tier B deferral keeps conformance), and the audit rerun on the head.

## 7. Tests

Parser suite:

- `fn f<T: C>(x: T): T` parses; the parameter carries one bound.
- `fn f<T: C + D>(x: T): T` parses; two bounds, in written order.
- `fn f<T: C, U: D>(x: T, y: U): T` parses; a bound per parameter.
- `fn f<T>(x: T): T` still parses, with no bound.
- `class Box<T: C>:` and `enum class E<T: C>:` parse.
- A malformed bound (`fn f<T: >(x: T): T`) diagnoses rather than
  hanging or swallowing the declaration.

Resolver suite:

- a bounded declaration is declared and callable: `fn f<T: C>` with a
  concept `C` in scope resolves, and a call to `f` resolves;
- the bound's name records a use when it resolves;
- an unresolved bound (`fn f<T: Missing>`) leaves the declaration
  intact and emits no diagnostic;
- the parameter is still a `GenericParam` in the declaration's scope.

Type checker suite: a bounded declaration types as its unbounded
counterpart does.

Corpus, through the audit:

- `print` resolves in every compiler program and the resolve column
  reads 0 for all eight;
- `math.dao`'s parse diagnostics fall to 0 and `printable.dao`'s to
  the `derived concept` alone;
- the typecheck column's movement is reported, not hidden: 557 call
  sites that resolved to nothing now type against a generic signature,
  and whatever that surfaces belongs to the audit and to the task the
  audit names next.

## 8. Acceptance

1. The parser reads the host's bound grammar, and no declaration is
   lost to a bound.
2. The §7 tests pass; every existing suite stays green; `validate_ir`
   stays green.
3. `bash bootstrap/audit_closure.sh` is rerun and
   `docs/bootstrap_closure.md` committed from the head.
4. The resolve column is 0 for all eight compiler programs — the
   corpus resolves, with the real prelude, against the bootstrap's own
   resolver.
5. The prelude section shows `math.dao` at 0 parse diagnostics.
6. The next task is chosen from that audit — the typecheck column and
   the prelude's remaining parse gaps are the candidates — and written
   after this task lands.

## 9. One-sentence summary

A bound is part of a generic parameter, so the parser reads it, and the
one prelude declaration the compiler corpus still cannot see becomes
visible.
