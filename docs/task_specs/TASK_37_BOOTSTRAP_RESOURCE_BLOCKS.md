# Task 37 — Bootstrap Pipeline: `resource` Blocks

Status: implementation spec
Phase: Tier B-Bootstrap, second construct (after Task 36)
Scope: the bootstrap parser, resolver, type checker, HIR, and MIR carry
a `resource <kind> <name> =>` block; the block's body is compiled, its
domain is a documented deferral

## 1. Objective

Make the bootstrap pipeline accept the `resource memory <name> =>`
blocks Task 35 E3 placed in its own sources — the one construct left in
the closure audit's parse column after Task 36 — so that every
bootstrap program parses clean and the audit's later columns measure
the later stages on the whole corpus.

## 2. Why this task exists now

After Task 36, `docs/bootstrap_closure.md` §3 shows `parse = 0` for the
five programs whose sources hold no `resource` block and, for the other
three (hir, mir, llvm), one "expected expression" per block followed by
one "expected declaration" per statement the parser skips while
recovering — 231, 1029, and 1623 diagnostics from 4, 10, and 25 blocks.
The blocks are the pipeline drivers' per-stage domains and the probe's
(`bootstrap/hir/impl.dao`, `bootstrap/llvm/impl.dao`).

| Program | `resource` blocks | parse diagnostics |
|---|---|---|
| hir | 4 | 231 |
| mir | 10 | 1029 |
| llvm | 25 | 1623 |

The corpus holds no `mode` block.

## 3. Syntax scope

`resource <kind> <name> =>` followed by an indented suite, at statement
position — the host's `parse_resource_block`: keyword, two identifiers,
`=>`, suite.  The lexer already produces `KwResource` and `FatArrow`.
`mode <name> =>` stays deferred (no site in the corpus).

## 4. AST, HIR

```
Node.ResourceS(kind: i64, name: i64, body_lp: i64)      // token indexes and the suite
HirNode.HirResource(kind: i64, name: i64, body_lp: i64)
```

The block keeps its identity through HIR, as it does in the host, so a
later slice can lower the domain without re-deriving it.

## 5. Stage behaviour

- Parser: `parse_resource_stmt`, dispatched from `parse_statement` on
  `KwResource`; the suite is parsed by `parse_suite`.
- Resolver: the body in a fresh block scope (`resolve_body_in_new_scope`,
  as `while`).  The name introduces no symbol in Tier A: the corpus
  never refers to a domain by name.
- Type checker: the body's statements (`check_body_stmts`).  The
  host's whole-body warning (contract law 9) and its escape rules are
  the host's; the bootstrap adds none in this task.
- HIR: `HirResource` with the lowered body.
- MIR: the body's statements, lowered in place (`lower_stmt_list`); the
  domain itself — its arena, its copy-out at exit — is a documented
  deferral (`bootstrap/README.md`, MIR Tier B deferrals): the bootstrap
  compiles a program whose `resource memory` blocks reclaim nothing,
  as every program did before Task 35.  Values are unchanged by that
  (the block's semantics are memory, and the copy-out makes what
  leaves a block equal to what a plain block would yield); only peak
  memory differs, which the audit's peak column measures on the host
  build, not the bootstrap's.

## 6. Delivery

One PR on Task 36's implementation:

1. `Node.ResourceS`, `parse_resource_stmt`, `node_kind_name`.
2. Resolver, type checker, HIR, MIR arms.
3. Tests (§7); `bootstrap/README.md` parser Tier A list gains
   `resource` blocks and the MIR deferrals name the domain;
   `docs/IMPLEMENTATION_PLAN.md` Task 37 entry; the audit rerun.

## 7. Tests

- Parser: `resource memory pool =>` with a two-statement suite parses to
  `ResourceS` with two body statements and no diagnostics; a `let`
  after the block is a sibling statement, so the suite closed at the
  dedent.
- Resolver: a name declared inside the block is unknown after it.
- HIR: `HirResource` appears for the block, its body lowered.
- MIR: a function whose block returns a value lowers to the same
  instruction kinds as the plain body.
- Corpus: the closure audit rerun shows `parse = 0` for all eight
  programs — the acceptance test.

## 8. Non-goals

- Domain semantics in the bootstrap's MIR/LLVM (arena hooks, copy-out
  at exit, escape diagnostics).
- `mode` blocks.
- Naming a domain from inside its block.

## 9. One-sentence summary

Every bootstrap program parses clean once the pipeline carries the
`resource memory` blocks it now runs itself in, with the domain's
lowering named as the deferral it is.
