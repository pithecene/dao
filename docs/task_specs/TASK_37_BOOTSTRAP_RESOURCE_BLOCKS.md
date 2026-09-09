# Task 37 — Bootstrap Pipeline: `resource` Blocks

Status: implemented
Phase: Tier B-Bootstrap, second construct (after Task 36)
Scope: the bootstrap parser, resolver, type checker, and HIR carry a
`resource <kind> <name> =>` block; MIR rejects it, fail-closed, until
the domain's MIR representation lands

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
- MIR: rejected, fail-closed — `mir_lower_stmt` emits its
  unsupported-statement diagnostic naming `HirResource`, as it does
  for `HirBreak`.  `CONTRACT_MIR_BOUNDARY.md` §4 requires MIR to
  preserve resource regions by explicit construct until backend
  lowering; lowering the body in place would erase the region, and
  could change values where a class defines its own `copy_out(self)`
  (the copy-out at exit is then that method's, and skipping it skips
  whatever it computes).  The domain's MIR representation — region
  markers, enter/exit hooks, copy-out at exit over the concrete
  aggregate types — is the slice the audit's `mir` column sequences
  once the resolve and typecheck columns close on the corpus.

## 6. Delivery

One PR on Task 36's implementation:

1. `Node.ResourceS`, `parse_resource_stmt`, `node_kind_name`.
2. Resolver, type checker, HIR, MIR arms.
3. Tests (§7); `bootstrap/README.md`, every stage's inventory: the
   parser's Tier B-Bootstrap coverage list gains `resource` blocks,
   the resolver's deferrals lose "resource block scoping" and its scope
   gains the block, the HIR coverage list gains `HirResource`, and the
   MIR deferrals name the block as rejected fail-closed;
   `docs/IMPLEMENTATION_PLAN.md` Task 37 entry; the audit rerun.

## 7. Tests

- Parser: `resource memory pool =>` with a two-statement suite parses to
  `ResourceS` with two body statements and no diagnostics; a `let`
  after the block is a sibling statement, so the suite closed at the
  dedent.
- Resolver: a name declared inside the block is unknown after it.
- HIR: `HirResource` appears for the block, its body lowered.
- MIR: a function with a resource block yields the unsupported-
  statement diagnostic naming `HirResource`; nothing of the block is
  lowered silently.
- Corpus: the closure audit rerun shows `parse = 0` for all eight
  programs — the acceptance test.

## 8. Non-goals

- Domain semantics in the bootstrap's MIR/LLVM (region markers, arena
  hooks, copy-out at exit, escape diagnostics) — the next slice, not a
  silent omission.
- `mode` blocks.
- Naming a domain from inside its block.

## 9. One-sentence summary

Every bootstrap program parses clean once the pipeline carries the
`resource memory` blocks it now runs itself in, with MIR rejecting the
block until its domain is represented there.
