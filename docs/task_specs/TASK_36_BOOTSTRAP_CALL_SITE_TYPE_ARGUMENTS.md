# Task 36 — Bootstrap Parser: Call-Site Type Arguments

Status: implemented
Phase: Tier B-Bootstrap, first construct (after Task 35's capacity work)
Scope: the bootstrap parser accepts generic arguments on a callee in
expression position — `Type<Args>::method(args)` and `f<Args>(args)` —
with the AST shape the host parser produces

## 1. Objective

Make the bootstrap parser accept the one construct behind every
parse-stage rejection in the closure audit: generic arguments between a
name and the `::` or `(` that follows it, in expression position.

Today `Vector<i64>::new()` is read as the comparison `Vector < i64 > …`
and stops at `::` with "expected expression".  After this task the
bootstrap parser reads it as the host does — a call whose callee is the
qualified name `Vector::new` and whose call-site type arguments are
`[i64]` — and the audit's parse column is zero for every program.

## 2. Why this task exists now

`docs/bootstrap_closure.md` §3 measured, after Task 35, that every
bootstrap program reaches its parse stage under 160 MiB (the llvm
program peaks at 156) and that every parse-stage diagnostic is this
construct: 53–208 sites per program, 84–2008 diagnostics (one per site
at statement level, more inside argument lists), and every later
stage's histogram is dominated by its shadow — `unknown name 'Vector'`,
`unknown name 'new'` — because the sites parsed as comparisons carry no
callee.

The construct has two shapes.  The compiler sources (the tracked
`bootstrap/**/*.dao`, test strings excluded) use one; the prelude the
corpus instantiates (`docs/bootstrap_closure.md` §2), which the
bootstrap must compile for itself, uses the other:

| Shape | Sites in the compiler sources | Sites in `stdlib/core` |
|---|---|---|
| `Vector<T>::new(`, `HashMap<V>::new(` — a static call on a generic type | 268 | — |
| `f<T>(…)` — a call with explicit type arguments (`size_of`, `align_of`, `ptr_cast`, `null_ptr`, `copy_out`, …) | 0 | 48 |

Both shapes are one rule in the host parser and are read together;
the second is covered by goldens rather than by the corpus.  No bare
`Type<Args>::Variant` without a call occurs; none is in scope.

## 3. Syntax scope

In expression position, after a primary that is an identifier or a
qualified name, the bootstrap parser accepts:

1. `Name<Type, …>::member(args)` — a static call on a generic type.
2. `Name<Type, …>(args)` — a call with explicit type arguments.

Everything else stays as it is: `a < b` remains a comparison, and
`a < b > c` remains two comparisons.  The rule that separates the two
readings is the host's (`compiler/frontend/parser/parser.cpp`,
`try_parse_call_type_args`): after `<`, parse a comma-separated list of
*types*; the reading is accepted only when `>` follows and then `(` or
`::`.  Otherwise the parser returns to the `<` and reads a comparison.

The type-argument list is parsed by `parse_type`, so nested generics
(`Vector<Option<i64>>`) and pointer types (`Vector<*T>`) are covered by
the existing type grammar.  The lexer emits `>>` as two `Gt` tokens
already (the type grammar depends on it); no lexer change.

## 4. AST

`Node.CallE` gains the call-site type arguments, as the host's
`CallExpr.type_args`:

```
CallE(callee: i64, args_lp: i64, arg_count: i64, targs_lp: i64, targ_count: i64)
```

`targs_lp` is a flushed list of type nodes — whatever `parse_type`
produces: `NamedT`, `GenericT`, `PointerT`, or `QualNameE` for a
qualified type such as `mod::T` — and `targ_count` its length; a call
without type arguments has `targ_count = 0`.  Shape 1 lowers to
`CallE(QualNameE([Name, member]), args, targs)`; shape 2 to
`CallE(IdentE(Name), args, targs)` (or a `QualNameE` callee when the
name was qualified).

Parity with the host is the type-argument list.  The callee shape
differs on purpose: the host parser spells a static call's callee as
one `IdentifierExpr` named `Type.method` (a host-internal mangling its
own resolver expects), while the bootstrap keeps the qualified name
`QualNameE([Type, member])`, which its resolver already handles for
`Type::member` (a `Type` prefix with remaining segments is recorded
and left to the type checker).  Only the type arguments are new.

`node_kind_name` keeps reporting `CallE`; the bootstrap has no AST
printer, and its goldens assert node kinds and child counts.

## 5. Parser design

`parse_postfix`, at the top of its loop, when the current node is an
`IdentE` or `QualNameE` and the next token is `Lt`:

- speculate: from the state after `<`, parse types separated by commas
  into a collected vector; accept when the next token is `Gt` and the
  one after it is `LParen` or `ColonColon`.
- on `::`: read `member`, extend the callee into a `QualNameE` (the
  identifier's segments plus `member`), then parse the argument list
  and build `CallE` with the type arguments.
- on `(`: parse the argument list and build `CallE` with the type
  arguments on the current callee.
- on failure: continue with the pre-speculation state; the relational
  level reads `<` as before.  The parser state is a value, so the
  speculation's diagnostics and nodes are dropped by discarding its
  state — no rollback code.

The argument-list parsing already in the `LParen` arm is factored into
one helper used by both paths, so a third copy is never written.

## 6. Downstream stages (in scope: pass-through only)

Every `match` over `Node` is closed, so the resolver, type checker, and
HIR lowering each update their `CallE` arm to the five-field shape.  The
resolver resolves the type arguments as type nodes (as it does a
generic type's arguments) beside the callee and arguments, so
`Vector<i64>::new()` resolves its `Vector` prefix and records the use;
the type checker and HIR lowering ignore them in this task.  Binding the type arguments —
instantiating `new` for `Vector<i64>` in the bootstrap type checker —
is the next Tier B-Bootstrap slice, sequenced by the audit's typecheck
histogram once the parse column is zero.

## 7. Delivery

One PR on the host-compiler fix that lets the `llvm` program reach its
parse stage (the audit's first item; a loop's temporaries overflowed
the stack), containing:

1. `Node.CallE` five-field shape.
2. `parse_postfix` speculation and both call shapes.
3. Resolver / type checker / HIR arms updated (pass-through).
4. Parser golden tests (§8) and the audit rerun (§8).
5. `bootstrap/README.md`: the parser section gains a **Tier
   B-Bootstrap coverage** list (as the LLVM section's "Tier B coverage
   (added)") naming call-site type arguments; the Tier A list stays as
   it is.  `docs/IMPLEMENTATION_PLAN.md`: the Task 36 entry.

## 8. Tests

Parser goldens in `bootstrap/parser/tests.dao`:

- `Vector<i64>::new()` → `CallE` with a `QualNameE` callee of two
  segments and one type argument.
- `HashMap<Vector<i64>>::new()` → nested type argument; `>>` closes both.
- `identity<i64>(x)` → `CallE` with an `IdentE` callee and one type
  argument.
- `a < b` → `BinaryE`; `a < b > c` → `BinaryE` over `BinaryE`; `f(a < b)`
  → a call whose argument is a comparison (the speculation fails and
  leaves the comparison reading).
- `Vector<i64>::new().push(1)` → `FieldE`/`CallE` chain on the result.

Corpus: the closure audit rerun on the head shows `parse = 0` for the
five programs whose sources hold no `resource` block (lexer, parser,
graph, resolver, typecheck) — the acceptance test — and every
remaining parse diagnostic in the other three is the `resource memory`
block Task 35 E3 put into the HIR test driver and the probe (39 sites;
the bootstrap parser's next construct, deferred in `bootstrap/README.md`).
The resolver histogram still names `Vector`: the type checker binds
nothing yet, so a static call's `Vector` prefix is now reached and
left unresolved as a class in expression position — the next slice's
first item.

Self-parse: `task bootstrap-test` (the parser's self-parse of real Dao
source) passes.

## 9. Non-goals

- Binding the type arguments in the bootstrap type checker (next slice).
- Bare `Type<Args>::Variant` without a call (absent from the corpus).
- Named arguments or rest markers in the new call paths beyond what the
  existing argument-list helper already accepts.
- Any host-compiler change beyond the stack-slot fix it is stacked on.

## 10. Risks

- Speculation cost: the type list is parsed once per `<` after a name;
  the parser state is a value and the rejected state is dropped, so the
  cost is the type list's length, bounded by source.
- Ambiguity: `a < b > (c)` reads as a call with type argument `b` — the
  same rule as the host, whose corpus and goldens accept it.

## 11. One-sentence summary

The bootstrap parser reads `Type<Args>::member(args)` and `f<Args>(args)`
as the host does — a call carrying type arguments — so the closure
audit's parse column reaches zero.
