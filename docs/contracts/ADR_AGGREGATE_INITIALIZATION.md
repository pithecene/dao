# ADR: Structural Aggregate Initialization and Pattern Wildcards

Status: **proposed**
Date: 2026-11-24

## Decision

1. Unify `class` and `enum class` variant construction as **purely structural, order-independent field-name initialization** inside ordinary parentheses `(...)`, with no leading dot and no braces:
   - `field = expr` binds the declared field `field` to `expr`.
   - Bare `field` (when the argument is an unqualified identifier) resolves `field` in the surrounding lexical value scope and initializes field `field` (`field = field` shorthand punning).
2. Retire positional `class` construction rather than mixing or coexisting with it. Construction logic, validation, normalization, and custom allocation policies belong in ordinary named functions (`Point::normalize(x, y)`, `Vector<T>::filled(count, value)`), never in special constructors.
3. Introduce `_` as a non-binding wildcard pattern in `match` arms and variant destructuring lists alongside `..` rest patterns.

## Context

`DAO-H01` identified three closely related ergonomic and safety defects across the 32,245-line Dao corpus:

1. Positional `class` constructors with 4 to 8 fields (`DaoType(...)`, `Registry(...)`, `ProgramGraph(...)`) hide field meaning at call sites and silently miscompile if two same-typed fields swap order.
2. `ADR_ENUM_CLASS.md` already requires `field = expr` on structured variants, forcing repetitive `Option::Some(value = value)` and `Result::Ok(value = value)` bindings while leaving `class` on a conflicting positional convention.
3. `match` statements (`467` sites, `3,758` arms) lack a non-binding `_` wildcard arm or single-slot discard pattern.

Dao already distinguishes types and variants from functions during name resolution (`SymbolKind::Type` and `Enum::Variant` vs. `SymbolKind::Function`). Introducing extra call-site punctuation (`.` prefixes or `{}` braces) would duplicate a semantic distinction the compiler and semantic highlighter already possess, whereas dotless field initialization requires **zero new tokens and zero new parser AST nodes**.

## 1. Structural Aggregate Initialization

### 1.1 Surface syntax

Both `class` values and payload-bearing `enum class` variants use the existing parenthesized call syntax with named or punned field arguments:

```dao
let state = LexState(
  cursor,
  indent,
  tokens,
  diags = diagnostics
)

let same_a = Point(x, y)
let same_b = Point(y, x)

let origin = Point(x = 0, y = 0)

return Expr::Binary(
  op = Operator::Add,
  left,
  right
)
```

Reference grammar (`spec/grammar/dao.ebnf`):

```ebnf
arguments :=
    argument ("," argument)*

argument :=
      identifier "=" expression
    | expression
```

No new grammar production is required: the parser produces `CallExpr` with `arg_names[i]` populated for `identifier "=" expression` and empty for bare `expression`. Name resolution and type checking distinguish structural aggregate initialization from function invocation by callee kind:

| Property | Aggregate Initialization (`Type(...)`, `Enum::Variant(...)`) | Function / Method Invocation (`fn(...)`, `Type::make(...)`, `v.m(...)`) |
|---|---|---|
| Callee resolves to | `class` type or payload-bearing `enum class` variant | Free function, static/inherent/concept method, `Ptr<T>` op, or function value |
| Argument binding | By field name (`field = expr` or bare `field` pun) | By parameter position |
| Argument order | Order-independent (field identity governs slot) | Signature order |
| Execution | Pure structural value assembly (no hidden user code) | Executes function body |

### 1.2 Shorthand punning rule

1. **Only bare unqualified identifiers pun:** in an aggregate initializer, an argument without `=` must be a bare `IdentifierExpr` (`cursor`). It binds the field named `cursor` to the value of `cursor` in the enclosing lexical scope, recording a normal `resolve.uses` entry on that identifier token for hover, go-to-definition, and find-references.
2. **Non-identifiers require `field = expr`:** literals (`0`, `"s"`), member accesses (`self.x`, `other.x`), qualified names (`Mode::Fast`), and call/binary expressions carry no single local binding name and must be written `field = expr` (`Point(x = self.x, y = self.y)`, `Span(offset = 0, length = 1)`).
3. **Lexical scope separation:** in `Point(x = y, y = x)` or `State(pos = pos + 1)`, the left-hand identifier names a field of the target aggregate, while the right-hand expression resolves strictly in the enclosing lexical value scope.

### 1.3 Completeness and required diagnostics

- **No implicit defaults:** every declared field of the `class` or `enum class` variant must be initialized exactly once.
- **No `field = expr` on functions or methods:** passing `name = expr` to a function, method, or `Ptr<T>` operation continues to diagnose `"arguments are given by position; named fields apply only to class and variant initialization"`.
- **Non-identifier without field name:** passing a non-identifier expression without `field =` to an aggregate initializer diagnoses `"argument in initialization of '<Type>' requires an explicit field name ('<field> = ...')"`.
- **Unknown field:** `"no field '<name>' on '<Type>'"` (at the field/identifier span; when `<name>` is a bare identifier pun, notes both that `<Type>` has no field `<name>` and lists valid uninitialized fields).
- **Duplicate field:** `"field '<name>' of '<Type>' is initialized more than once"` (at the duplicate argument span, covering both `Point(x = 1, x = 2)` and `Point(x, x = 2)`).
- **Missing field(s):** `"missing field '<name>' in initialization of '<Type>'"` (at the call span, reported in field declaration order).
- **Unresolved shorthand binding:** when `Type` has a field `foo` and the caller writes bare `Type(foo, ...)` with no value `foo` in lexical scope, resolution diagnoses `"unknown name 'foo'"` at the `foo` token span.

### 1.4 Evaluation order and generic inference

1. **Left-to-right runtime evaluation:** argument expressions evaluate strictly left to right in **written source order**, storing into temporaries before populating the aggregate's layout slots in field declaration order.
2. **Order-independent generic type inference and contextual literal fitting:** explicit type arguments (`Box<i64>(value = 0)`) or enclosing expected type (`let o: Option<i64> = Option::Some(value = 0)`) seed type bindings first. Remaining unbound type parameters of the `class` or `enum class` infer across arguments ordered by **field declaration index** (preserving `type_checker.cpp`'s existing `declaration_order` rule for `enum class`), so `Pair(first = 1, second = x)` and `Pair(second = x, first = 1)` infer the exact same type.

### 1.5 Safe migration without silent positional reinterpretation

Flipping existing positional `class` constructors directly to field-name punning would fail at compile time on almost all sites, **except** one hazardous case: a positional call `Edge(to, from)` on `class Edge: from: i64 / to: i64` whose arguments are bare identifiers forming a non-identity permutation of the class's own field names. Under positional rules `Edge(to, from)` reversed the endpoints; under field-identity punning `Edge(to, from)` would silently construct the unreversed edge `(from = from, to = to)`.

Migration therefore follows a strict two-step protocol:

1. **Mechanical pre-flip rewrite (under existing positional semantics):** inspect every resolved `TypeStruct` constructor call across `stdlib/`, `bootstrap/`, `examples/`, and `testdata/`. Any argument `arg_i` at field index `i` that is **not** a bare `IdentifierExpr` spelled identically to `fields[i].name` is rewritten to `fields[i].name = arg_i` (turning `Edge(to, from)` into `Edge(from = to, to = from)` and `Span(0, 1)` into `Span(offset = 0, length = 1)`, while leaving already-matching `Point(x, y)` as `Point(x, y)`).
2. **Semantic cutover (both compilers together):** enable named/punned `class` initialization and `enum class` identifier punning in `compiler/` and `bootstrap/`, rejecting non-identifier unnamed arguments on aggregates from that commit onward. Because Step 1 rewrote every non-identity permutation explicitly, **zero call sites can silently change runtime meaning**.

## 2. Pattern Matching Ergonomics (`..` and `_`)

### 2.1 Bootstrap parity for `..` (`DAO-H04`)

`CONTRACT_SYNTAX_SURFACE.md` and `ADR_ENUM_CLASS.md` already freeze trailing `..` in `enum class` destructuring (`Expr::Function(name, ret, ..):`). Native `daoc` implements `..`; `DAO-H04` brings `bootstrap/parser`, `bootstrap/typecheck`, and `bootstrap/hir` to full parity before `DAO-H02`.

### 2.2 Wildcard `_` pattern

`_` is reserved as a non-binding wildcard pattern in two pattern positions (and remains illegal as an expression or declaration name):

1. **Catch-all `match` arm (`_:`):**
   ```dao
   match tok.kind:
     TK::Plus:
       return 1
     _:
       return 0
   ```
   - `_:` matches any remaining value of the scrutinee type without introducing a lexical binding.
   - `_:` must appear as the final arm of a `match`; any arm after `_:` diagnoses `"unreachable match arm after wildcard '_'"`.
   - If prior arms already cover every variant of a closed `enum` or `enum class`, `_:` diagnoses `"redundant wildcard '_' arm: all variants of '<Type>' are already covered"` so closed-sum exhaustiveness stays compiler-checked.
2. **Ignored slot inside variant destructuring (`Variant(first, _, third)`):**
   - Inside `Enum::Variant(p_0, ..., p_{k-1})`, `_` occupies one field position in declaration order without binding a local symbol, allowing interior fields to be skipped cleanly while trailing fields use `..`.

### 2.3 Exhaustiveness and definite return

When a `match` over an `enum` or `enum class` covers every variant (explicitly or via a non-redundant trailing `_:` arm) and every arm terminates with `return` or `break`, control flow after the `match` is unreachable, removing mandatory dead `return` statements after exhaustive matches.
