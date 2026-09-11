# Task 39 — Bootstrap Match-Arm Binding Resolution

Status: implementation spec
Phase: Tier B-Bootstrap, sequenced by the closure audit after Task 38
Scope: the bootstrap resolver treats the names a match arm's pattern
introduces as declarations of that arm, not as uses of an outer name

## 1. Objective

With the prelude in the program (Task 38) the compiler corpus's
resolve column fell for every program, and what remains is one family.
The audit's resolver histogram now reads:

```
unknown name 'a'   unknown name 't'   unknown name 'b'
unknown name 'c'   unknown name 'lp'
```

Those are not missing declarations.  They are the names match arms
bind:

```dao
match node:
  Node::CallE(callee, args_lp, arg_count, targs_lp, names_lp):
    return lower_call(callee, args_lp)
```

`callee` and `args_lp` are introduced by the arm; the bootstrap
resolver looks them up instead, once at the pattern and again at every
use in the body.  This task gives them the scope the contract and the
host give them, and the family leaves the histogram.

## 2. What the contracts require

`docs/contracts/ADR_ENUM_CLASS.md` ("Named destructuring") and
`docs/contracts/CONTRACT_SYNTAX_SURFACE.md` fix the surface:

1. A variant is reached with `::`, in expressions and in patterns:
   `Expr::Call(callee, args):`.
2. A pattern's parenthesized names are destructuring bindings, in
   field declaration order — not arguments.
3. `..` at the end ignores the remaining fields.
4. `Pattern as c:` binds the whole variant for field access.
5. A parameterless variant's pattern is written without parentheses.

Rules 1, 2 and 5 are what the compiler corpus writes; §4 measures
that, and §6 defers 3 and 4 with the audit as the arbiter.

The scope a binding lives in is the arm's, as the host resolver has
it (`compiler/frontend/resolve/resolve.cpp`, `MatchStatement`): the
pattern's constructor resolves in the enclosing scope, the arm opens a
block scope, each binding is declared into it as a `Local`, and the
body resolves there.

## 3. What the bootstrap has today

- The parser keeps a whole arm pattern as one expression: a
  destructuring arm is a `CallE` whose callee is the variant's
  qualified name and whose arguments are `IdentE` nodes
  (`parse_match_stmt`, `shared/base.dao`).  The host parser instead
  rewrites that call at parse time into a constructor plus a list of
  binding names (`try_destructure_pattern`).
- The resolver resolves the pattern with `resolve_expr` and the body
  with `resolve_body_in_new_scope` (`resolver/impl.dao`, `Node::MatchS`).
  Every binder is therefore an unresolved use, and so is every
  mention of it in the body.
- The type checker already reads the shape correctly: `check_pattern`
  treats a `CallE` pattern's arguments as "bindings the pattern
  introduces", requires each to be a plain identifier, rejects named
  arguments, and checks the count against the variant's parameters.
- HIR lowers a match to an if/else chain over equality with the
  pattern expression (`lower_match_stmt`); MIR does not compile
  patterns (Tier B).
- The resolver's own deferral inventory names this gap: "Match arm
  destructuring bindings (patterns resolved as expressions, but no
  binding introduction)".

So the semantics are already understood one pass later.  This task is
resolver parity, not a new language feature.

## 4. Measured: what the corpus writes

Destructuring arms in the assembled compiler programs, and match
statements for scale:

| Program | Arms with bindings | `match` statements |
|---|---|---|
| lexer | 53 | 9 |
| parser | 70 | 26 |
| graph | 53 | 9 |
| resolver | 140 | 48 |
| typecheck | 241 | 106 |
| hir | 370 | 137 |
| mir | 419 | 144 |
| llvm | 458 | 166 |

The names those arms bind, most frequent first in `llvm.gen.dao`:
`a` (85), `b` (69), `name_tidx` (45), `c` (42), `t` (41), `tidx` (31).
The audit's resolver histogram for the same program reports
`unknown name 'a'`, `'t'`, `'b'`, `'c'`, `'lp'` — the same names, and
the histogram samples only the first 50 diagnostics of the 1860 the
column counts.

Neither `..` nor `as` appears in a match arm anywhere in the corpus,
in the stdlib, or in the examples.

## 5. Design

### 5.1 Where the rule lives

The parser's representation stays as it is, and so does every pass but
the resolver.  A match arm's scope is a resolver rule, so the resolver
reads the AST it already has: a destructuring arm is a `CallE` whose
callee is the variant's qualified name and whose arguments are the
names the arm binds.  No shared description of a pattern is
introduced, and the type checker's `check_pattern` — which already
reads that shape for its own diagnostics — is untouched.

### 5.2 The resolver

`Node::MatchS` resolves as the host's does:

1. the scrutinee, in the enclosing scope;
2. per arm, the pattern's **variant path** as a use — the `CallE`'s
   qualified callee — so `Token::Identifier` and `m::E::Variant` keep
   their uses and their diagnostics, along with any call-site type
   arguments the pattern carries, which are types of the enclosing
   scope as a call's are;
3. the arm's **block scope**, opened before the body;
4. each argument that is a plain identifier declared into that scope
   as a `SymbolKind::Local` whose declaration node is the identifier
   itself, so a binder is a declaration with a span, not a use;
5. the body, resolved in that scope;
6. the scope closed at the arm's end.

A binder is not resolved as a use, and a duplicate binder in one
pattern is `scope_declare`'s existing duplicate diagnostic.  An
argument that is not a plain identifier declares nothing and keeps the
type checker's existing diagnostic.

### 5.3 What stays a use

- A bare identifier pattern (`some_constant:`) is a use, as the host
  has it: only a parenthesized pattern binds.
- A literal pattern is a value.
- A parameterless variant pattern (`Option::None:`) binds nothing.
- A call-shaped pattern whose callee is not a qualified name is an
  ordinary expression — only a variant reached with `::` takes a value
  apart — so `Box(x):` uses `x` rather than declaring it.
- A pattern whose parenthesized arguments are not all plain
  identifiers keeps `check_pattern`'s diagnostic; the resolver
  declares the identifiers it does have and leaves the rest alone,
  so one malformed pattern does not cascade.

### 5.4 Later passes

The type checker is untouched by this task: `check_pattern` already
treats a pattern's arguments as bindings and keeps its diagnostics and
their wording.  Binder symbols
have no declared type node, so their types come from the variant's
parameters when the type checker learns to type a binding — that is
not this task, and the type checker does not regress: it types today's
patterns exactly as it does now.

HIR keeps its equality-chain lowering.  Its binder identifiers now
resolve to arm locals rather than to nothing, which is strictly more
information; pattern compilation stays Tier B, and the `hir`, `mir`,
`llvm` audit columns still run the single-source adapters and remain
outside frontier selection (Task 38 §5.4).

## 6. Non-goals

- `..` (rest) and `as` (whole-variant) arms: contract syntax the
  bootstrap parser does not represent and the closure corpus does not
  write (§4).  If a later audit makes either the frontier — a prelude
  file or a compiler source that needs it — it becomes its own slice.
- Exhaustiveness checking, guards, nested patterns, literal or range
  patterns beyond what the parser reads today.
- Typing a binder from its variant's parameter; pattern compilation in
  HIR or MIR; the later-stage program pipeline.
- Generics, the prelude's unparsed constructs, resource MIR, Task 32,
  Task 33's remaining rows.
- Any shared cross-pass description of a pattern: one lexical rule
  that one pass needs does not earn a new semantic layer in
  `shared/base.dao`.

## 7. Delivery

Two PRs:

1. This spec; `docs/IMPLEMENTATION_PLAN.md` (Task 39 entry).
2. The implementation: §5.2–§5.3 in the resolver alone, the tests of
   §8, `bootstrap/README.md`
   (the resolver's Tier A scope gains match-arm binding scopes; the
   Tier B deferral loses that line, keeping `..`/`as`), the resolver's
   in-source deferral inventory, and the audit rerun on the head.

## 8. Tests

Resolver suite:

- **variant destructuring** — `E::Both(a, b):` with `a` and `b` used in
  the body resolves with no diagnostic, and both uses reach the arm's
  own symbols.
- **binder is a declaration** — the binder token itself produces no
  `unknown name`, and its symbol is a `Local` whose declaration node is
  the binder.
- **arm isolation** — `E::A(x):` and `E::B(y):` in one match: `x` is
  unknown in B's body, `y` unknown in A's.
- **post-match isolation** — a binder is unknown after the match.
- **shadowing** — with an outer `let x`, `E::A(x):` binds a new arm
  local; the body's `x` resolves to it, and the outer `x` is intact
  after the match.
- **fieldless variant** — `E::None:` resolves as today and binds
  nothing; a pattern that is not a call resolves as an expression.
- **non-variant call pattern** — `Box(x):` declares nothing; `x` is a
  use.
- **pattern type arguments** — `E<Cell>::One(a):` records the use of
  `Cell`.
- **cross-module variant** — `m::E::V(x)` resolves the qualified
  constructor through the import binding while `x` is arm-local.
- **duplicate binder** — `E::Both(a, a):` diagnoses a duplicate
  declaration.

Type checker suite: unchanged — the existing pattern diagnostics
(parameterless variant written with parentheses, named argument in a
pattern, arity, a non-identifier binder) keep their tests and their
messages because the pass is not touched.

Corpus, through the audit:

- the `unknown name` family of match binders leaves the resolver
  histogram, and the resolve column falls for every program;
- no new diagnostic family appears in its place from a scoping
  regression;
- `task bootstrap-test`, `task test`, and `validate_ir` stay green.

## 9. Acceptance

1. Match-arm bindings are arm-local declarations in the bootstrap
   resolver, mirroring the host's lexical semantics — no corpus-specific
   name handling anywhere.
2. The §8 tests pass; every existing suite stays green; `validate_ir`
   stays green.
3. The change is confined to the resolver: no other pass and no
   shared type is touched.
4. `bash bootstrap/audit_closure.sh` is rerun and
   `docs/bootstrap_closure.md` committed from the head.
5. The binder `unknown name` family is gone from the compiler
   resolver histogram, and the resolve column's fall is reported.
6. The next task is chosen from that regenerated audit — the resolver
   histogram, the prelude section, or the typecheck histogram — and
   written after this task lands, not before.

## 10. One-sentence summary

A match arm's parenthesized names are declarations of that arm, so the
bootstrap declares them in the arm's scope instead of looking them up
and failing.
