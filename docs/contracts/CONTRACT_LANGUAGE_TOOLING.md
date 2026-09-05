# Contract — Language Tooling Boundaries

This contract is normative.

## Purpose

Dao tooling must derive semantic information directly from the compiler
frontend and semantic analysis layers.

## Invariants

The following must remain true unless this contract is explicitly changed:

1. Tooling must not maintain a shadow parser or shadow semantic model.
2. Semantic highlighting must be produced from compiler analysis, not from
   editor-specific regexes once compiler-backed analysis exists.
3. Diagnostics shown in the CLI, playground, or LSP must originate from the
   same diagnostic structures and span model.
4. The playground is a first-class development surface, not a disposable
   demo.
5. The LSP and playground must consume shared compiler analysis APIs rather
   than reimplementing name resolution, type classification, or symbol
   lookup.
6. Semantic token payloads, hover payloads, completion payloads, and symbol
   identity must share one compiler-owned source of truth.
7. The long-term north star is a web-based IDE backed by compiler analysis,
   not a browser toy with bespoke language logic.

## Required Tooling Surfaces

The repository must reserve dedicated roots for:
- `tools/playground/`
- `tools/lsp/`

Additional tooling roots may exist, but these two are baseline surfaces.

## Frozen Initial Semantic Token Taxonomy

The initial compiler-produced semantic token taxonomy must be able to
classify at least the following categories:

### Keywords and control
- `keyword.module`
- `keyword.import`
- `keyword.type`
- `keyword.extern`
- `keyword.fn`
- `keyword.let`
- `keyword.return`
- `keyword.if`
- `keyword.else`
- `keyword.while`
- `keyword.for`
- `keyword.in`

### Dao execution / resource constructs
- `keyword.mode`
- `keyword.resource`
- `mode.unsafe`
- `mode.gpu`
- `mode.parallel`
- `resource.kind.memory`
- `resource.binding`

### Types and declarations
- `type.builtin`
- `type.nominal`
- `decl.function`
- `decl.type`
- `decl.field`
- `decl.module`

### Symbol uses
- `use.function`
- `use.variable.local`
- `use.variable.param`
- `use.field`
- `use.module`

### Literals and operators
- `literal.string`
- `literal.number`
- `operator.pipe`
- `operator.arrow`
- `operator.context`
- `operator.assignment`
- `operator.namespace`
- `punctuation`

### Lambda / pipeline support
- `lambda.param`

This taxonomy may expand, but downstream tooling must not assume a
smaller baseline than the one above.

### Expansions since the initial freeze

The compiler additionally classifies the following. Tooling may rely on
them; the "at least" rule above still governs the baseline.

Declaration sites of value bindings:
- `decl.variable.local` — `let` binders, `for` binders, `match` arm
  bindings and `as` bindings
- `decl.variable.param` — function and method parameters

Keywords introduced after the initial freeze:
- `keyword.concept`, `keyword.derived`, `keyword.extend`,
  `keyword.deny`, `keyword.as`, `keyword.self`, `keyword.where`,
  `keyword.match`, `keyword.break`, `keyword.yield`

Operators without a dedicated initial category:
- `operator.arithmetic` — `+ - * / %`
- `operator.comparison` — `== != < <= > >=`
- `operator.logical` — `!`, `and`, `or`
- `operator.member` — `.`
- `operator.range` — `..`
- `operator.address` — `&`
- `operator.try` — `?`

Literals and uses:
- `literal.bool` — `true`, `false`
- `use.type` — a type used as a value (constructor call, static-method
  receiver, enum-variant path head, concept reference, generic
  parameter)
- `use.variant` — an enum variant referenced through its enum
  (`Color::Red`, `Option.Some` in expressions and patterns)

## Frozen Initial LSP Capability Slice

The first supported LSP capability slice must include:
- publish diagnostics
- semantic tokens
- hover
- completion
- go-to-definition
- find references
- document symbols

The following are explicitly deferred until symbol identity and edit
safety are mature:
- rename
- code actions
- formatting-on-type
- semantic refactors

## Shared Analysis Ownership

Compiler-owned analysis APIs must be the only supported source for:
- semantic token streams
- hover payloads
- completion payloads
- definition/reference lookup
- document symbol trees

The playground and LSP may adapt transport and presentation, but not
semantic truth.
