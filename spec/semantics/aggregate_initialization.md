# Unified Call Binding, Aggregate Initialization, and Pattern Wildcards

Status: **proposed** (promotes to `docs/contracts/` upon acceptance)
Date: 2026-11-25

## Decision

1. Unify direct function calls, indirect callable invocations, method calls, `class` initialization, and `enum class` variant initialization under one parenthesized call syntax `Callee(arg, ...)`, with zero leading dots and zero braces.
2. Extend `name = expr` labeled arguments and bare-identifier same-name elision (`ident` meaning `ident = ident`) across both callable invocations and structural initialization.
3. Partition every signature, callable type, and variant field list into a leading **positional prefix** (`_ name: Type` or `Type`) and trailing **named slots** (`name: Type`). Resolution never falls back between name matching and position:
   - Leading positional slots bind in declaration order using unlabeled expressions. Renaming a local variable at a call site never rebinds that argument to another parameter.
   - Trailing `name: Type` slots bind strictly by name. An unlabeled argument in the named region must be a bare unqualified identifier `ident` matching uninitialized slot `ident`. Differently named variables and compound expressions require `name = expr`.
4. Preserve parameter labels and positional prefix length in callable types (`fn(lhs: i32, rhs: i32): i32`, `fn(f64, scale: f64): f64`). Local and generic function references (`let f = sub`) keep identical call binding to direct calls, while adapting a named callable to an unlabeled `fn(T, U): R` requires an explicit lambda (`|a, b| -> sub(lhs = a, rhs = b)`).
5. Keep `class` declarations purely named (`k = 0` positional slots). Positional construction helpers, validation, and normalization belong in ordinary functions (`Span::of(_ offset: i64, _ length: i64)`), never in special constructors.
6. Introduce `_` as a non-binding wildcard pattern in `match` arms and variant destructuring lists alongside `..` rest patterns.

## Context

`DAO-H01` measured three related defects across the 32,245-line Dao corpus:

1. Positional `class` calls with 4 to 8 fields (`DaoType(...)`, `Registry(...)`, `ProgramGraph(...)`) hide field roles at call sites and silently miscompile when two same-typed fields swap order.
2. `ADR_ENUM_CLASS.md` requires `field = expr` on structured variants while functions and `class` types forbid `name = expr`. Callers must remember two incompatible mental models for identical `Callee(...)` syntax, and wrapper variants force repetitive `Option::Some(value = value)` and `Result::Ok(value = total)` noise.
3. `match` statements (`467` sites, `3,758` arms) lack a non-binding `_` wildcard arm or single-slot discard pattern.

Dao already separates types and variants from functions during name resolution (`SymbolKind::Type` and `Enum::Variant` versus `SymbolKind::Function`). Extra call-site punctuation (`.` prefixes or `{}` braces) would duplicate information the compiler and semantic highlighter already track. Sharing one `Callee(...)` grammar and one deterministic argument-binding rule removes the split between calls and construction with **zero new expression AST nodes**.

## 1. Unified Call and Initialization Binding

### 1.1 Rename Hazard of Name-First Positional Fallback

A tempting shortcut tries matching bare identifiers to parameter names first and falls back to parameter position when no parameter name matches. That hybrid rule breaks local lexical alpha-renaming:

```dao
fn sub(lhs: i32, rhs: i32): i32 -> lhs - rhs

let a = 10
let b = 3
let first = sub(a, b)      // fallback binds lhs = 10, rhs = 3 -> 7

let rhs = 10               // developer renames local 'a' to 'rhs'
let second = sub(rhs, b)   // name-first match binds rhs = 10, fallback binds lhs = 3 -> -7
```

Under name-first fallback, renaming local variable `a` to `rhs` silently swaps the two `i32` arguments without any type error. The reverse hazard also holds: renaming internal parameter names from `(x, y)` to `(lhs, rhs)` silently alters callers that happen to pass a local named `rhs` in slot zero.

Sound call binding therefore requires **zero fallback** between positional binding and name-based binding. Each parameter or variant field declares up front whether call sites address it by position or by name.

### 1.2 Positional Prefix and Named Region

Every parameter in a `fn`, `extern fn`, concept method, or inherent method, and every field in an `enum class` variant, chooses one of two external binding modes at its declaration:

1. **Positional slot (`_ name: Type`):** leading underscore `_` before the parameter or field name omits the external call-site label. Inside the callable body (or under `Variant as v` field access and hover documentation), the binding still carries the identifier `name`.
2. **Named slot (`name: Type`):** omitting `_` preserves `name` as a required external slot label. All `class` fields use `name: Type`, giving every `class` initializer `k = 0` positional slots.

Declaration rules enforce a clean prefix boundary:

- Positional slots (`_ name: Type`) must form a contiguous leading prefix of `k >= 0` slots (after receiver `self` on methods). Declaring `_ name: Type` after a named `other: Type` parameter diagnoses `"positional parameter '_ <name>' cannot follow named parameter '<other>'"`.
- Parameter and field names within one declaration remain pairwise distinct regardless of `_`.

Representative signatures across the standard library and user code follow natural domain roles:

```dao
// Algebraic and single-payload operations mark leading slots positional:
fn min<T: Comparable>(_ a: T, _ b: T): T
fn sub(_ lhs: i32, _ rhs: i32): i32 -> lhs - rhs
fn range(_ start: i32, _ end: i32): Generator<i32>

enum class Option<T>:
  Some(_ value: T)
  None

enum class Result<T, E>:
  Ok(_ value: T)
  Err(_ error: E)

// Container methods mark receiver-adjacent key or element slots positional:
fn push(self, _ elem: T): Vector<T>
fn get(self, _ index: i64): T
fn set(self, _ key: K, _ value: V): HashMap<K, V>

// Multi-role operations, structured variants, and classes keep named slots:
fn copy_bytes(_ dst: Ptr<u8>, src: Ptr<u8>, count: i64): void
fn connect(host: string, port: i32, timeout_ms: i64): Result<Conn, IoError>

class Point:
  x: f64
  y: f64

enum class Expr:
  IntLit(_ value: i64)
  Binary(op: Operator, left: ExprId, right: ExprId)
```

### 1.3 Deterministic Call-Site Resolution

Consider a target declaration or callable type with `k` positional slots `p_0 .. p_{k-1}` followed by `m` named slots `p_k .. p_{k+m-1}`, invoked with arguments `a_0 .. a_{n-1}`:

1. **Positional prefix (`0 <= i < k`):**
   - Argument `a_i` must carry no `=` label and binds slot `p_i` in declaration order.
   - **Any expression** (literal, compound expression, or bare identifier) fills `p_i`. A bare identifier `rhs` at index `0` of `sub(_ lhs: i32, _ rhs: i32)` unconditionally binds `lhs`, so local renames never permute slots.
   - Writing `name = expr` at index `i < k` diagnoses `"positional argument <i> of '<Callee>' does not take a label"`. Direct calls and indirect calls through `fn(T, U): R` values therefore obey identical rules.
2. **Named region (`i >= k`):**
   - Order among arguments at indices `i >= k` does not affect parameter or field target selection.
   - **Explicit label (`name = expr`):** `name` must match a declared named slot `p_j` (`j >= k`), binding `p_j` to `expr`.
   - **Same-name elision (bare `ident`):** an unlabeled argument at index `i >= k` must be a bare unqualified `IdentifierExpr` (`ident`). Resolution looks up `ident` in the enclosing lexical value scope, records a normal `resolve.uses` entry for IDE hover, go-to-definition, and find-references, and binds the named slot `p_j` (`j >= k`) whose declared name equals `ident`.
   - **No positional fallback:** an unlabeled `a_i` that is not a bare identifier matching a named slot `p_j` (`j >= k`) fails type checking immediately. Resolution never falls back to slot position.
3. **Lexical scope separation:** in `Point(x = y, y = x)`, `State(pos = pos + 1)`, or `connect(host = host, port = port + 1, timeout_ms)`, the left-hand identifier names the target slot while the right-hand expression resolves strictly in the enclosing lexical scope.
4. **Pipelines (`lhs |> target`):** `lhs` supplies slot `0` of `target` (whether positional or named). Any explicit arguments in `lhs |> f(arg_1, ...)` bind against the remaining signature prefix starting at slot `1` under rules 1 and 2.

| Call Site | Target Declaration or Type | Resolution Outcome |
|---|---|---|
| `sub(rhs, b)` | `fn sub(_ lhs: i32, _ rhs: i32)` | Valid: `lhs = rhs`, `rhs = b` (rename-safe) |
| `Option::Some(x + 1)` | `Some(_ value: T)` | Valid: `value = x + 1` |
| `Option::Some(value = 1)` | `Some(_ value: T)` | Error: positional slot `0` takes no label |
| `Point(x, y)` | `class Point` (`x`, `y`) | Valid: `x = x`, `y = y` |
| `Point(y, x)` | `class Point` (`x`, `y`) | Valid: `y = y`, `x = x` (order-independent) |
| `Point(0.0, 1.0)` | `class Point` (`x`, `y`) | Error: non-identifier in named slot requires `x = ...` |
| `connect(host, port = 8080, timeout_ms)` | `fn connect(host, port, timeout_ms)` | Valid: elides `host` and `timeout_ms`, labels `port` |
| `f(rhs, lhs)` | `f: fn(lhs: i32, rhs: i32): i32` | Valid: `lhs = lhs`, `rhs = rhs` (matches direct call) |

### 1.4 Callable Types, Function References, and Lambdas

Erasing parameter labels when taking a function reference would introduce a refactoring discontinuity between direct and indirect calls:

```dao
fn diff(lhs: i32, rhs: i32): i32 -> lhs - rhs

let lhs = 10
let rhs = 3
let direct = diff(rhs, lhs)   // elides rhs = rhs, lhs = lhs -> 10 - 3 = 7
let f = diff                  // f: fn(lhs: i32, rhs: i32): i32
let aliased = f(rhs, lhs)     // identical named binding -> 10 - 3 = 7
```

If `let f = diff` decayed to positional `fn(i32, i32): i32`, `f(rhs, lhs)` would evaluate `3 - 10 = -7` while `diff(rhs, lhs)` evaluated `7`. Callable types therefore carry the exact positional-prefix and named-slot structure of signatures:

```ebnf
function_type :=
    "fn" "(" fn_type_params? ")" ":" type

fn_type_params :=
    fn_type_param ("," fn_type_param)*

fn_type_param :=
      "_" identifier? ":" type
    | identifier ":" type
    | type
```

Callable type formation, equality, and conversion follow strict, zero-magic rules:

1. **Positional and named type slots:** in `fn(...)` type syntax, bare `Type`, `_: Type`, and `_ doc_name: Type` all denote the same unlabeled positional slot (`doc_name` carries documentation only). `name: Type` denotes a named slot with label `name`, and positional slots must precede all named slots.
2. **Preserved labels on function references:** referencing a function or static method (`let f = diff`, `let c = math::clamp<i32>`) produces a `TypeFunction` retaining the declaration's positional prefix count `k`, parameter types, and named labels `p_k .. p_{k+m-1}`. A function with all `_` parameters (`fn score_v1(_ wind: i32): i32`) already has `k = n` and type `fn(i32): i32`.
3. **Exact callable type equality:** under `CONTRACT_TYPECHECKING_BASELINE.md` §4, two `TypeFunction` types match only when return types, arities, positional prefix counts `k`, parameter types `0 .. n - 1`, and trailing slot names `k .. n - 1` match exactly. `fn(lhs: i32, rhs: i32): i32`, `fn(rhs: i32, lhs: i32): i32`, and `fn(i32, i32): i32` are distinct types.
4. **Explicit lambda adaptation (no implicit label erasure):** assigning a labeled callable `fn(lhs: i32, rhs: i32): i32` to an unlabeled `fn(i32, i32): i32` fails type checking even under an explicit type annotation. Silent positional coercion would tie `fn(i32, i32): i32` to `diff`'s declaration order, causing a later reorder `fn diff(rhs: i32, lhs: i32)` to invert indirect callers while every named `diff(...)` call remained valid. Callers erase or reorder labels explicitly with a lambda:
   ```dao
   let pos_diff: fn(i32, i32): i32 = |a, b| -> diff(lhs = a, rhs = b)
   ```
5. **Lambdas and higher-order generics:** lambda expressions `|x_0, ..., x_{n-1}| -> body` always produce unlabeled positional callable types `fn(T_0, ..., T_{n-1}): R` (`k = n`). Higher-order functions declare positional callback parameters and accept lambdas, `_`-prefixed positional functions, and explicit lambda adapters uniformly:
   ```dao
   fn map_vec<T, U>(_ xs: Vector<T>, f: fn(T): U): Vector<U>

   let doubled = map_vec(winds, f = |w| -> w * 2)
   let scores = map_vec(winds, f = score_v1)
   let padded = map_vec(labels, f = |s| -> pad_left(text = s, width = 8))
   ```

### 1.5 Contrasted Design Option

Reviewers also weighed an **arity-1 implicit positional + `/` separator** rule:

- Single-parameter functions `fn sin(x: f64)` and single-field variants `Option::Some(value: T)` would accept unlabeled positional arguments automatically at arity `1`.
- Functions and variants of arity `>= 2` would require named or elided arguments unless the signature placed a Python-style `/` divider after its positional prefix (`fn sub(lhs: i32, rhs: i32, /)`).

Leading `_ name: Type` declarations outperform the arity-1 and `/` alternative:

1. **Arity stability:** adding a second parameter to `fn parse(src: string)` never silently flips slot `0` from positional to named unless the author changes `_ src` or `src` deliberately.
2. **Token clarity:** `/` already denotes division in expressions, whereas Dao already reserves `_` as the non-binding marker in patterns.
3. **Local legibility:** each parameter carries its own call convention directly at its declaration site without scanning ahead for a trailing `/`.

### 1.6 Completeness and Diagnostics

- **No implicit defaults:** every call or aggregate initializer must bind every declared parameter or field exactly once.
- **Too few positional arguments:** supplying fewer than `k` arguments diagnoses `"missing positional argument <i> in call to '<Callee>'"`.
- **Label on positional slot:** passing `name = expr` at index `i < k` (including on unlabeled `fn(T, U): R` callables) diagnoses `"positional argument <i> of '<Callee>' does not take a label"`.
- **Non-identifier in named region:** passing `0`, `self.x`, `Mode::Fast`, or `a + b` without `name =` at index `i >= k` diagnoses `"argument in '<Callee>' requires an explicit name ('<slot> = ...')"`.
- **Unknown slot name:** `"no parameter '<name>' on '<Callee>'"` (or `"no field '<name>' on '<Type>'"`) at the label or identifier span, listing remaining unbound named slots.
- **Duplicate slot binding:** `"'<name>' in '<Callee>' is bound more than once"` at the duplicate argument span, covering both `Point(x = 1.0, x = 2.0)` and `Point(x, x = 2.0)`.
- **Missing named slot(s):** `"missing '<name>' in '<Callee>'"` at the call span, reported in declaration order.
- **Unresolved elided identifier:** writing `Point(x, y)` when `x` does not exist in lexical value scope diagnoses `"unknown name 'x'"` at `x`.
- **Implicit label erasure:** assigning `fn(lhs: i32, rhs: i32): i32` to `fn(i32, i32): i32` diagnoses `"cannot assign 'fn(lhs: i32, rhs: i32): i32' to 'fn(i32, i32): i32': adapt named parameters explicitly with a lambda"`.

### 1.7 Evaluation Order and Generic Inference

1. **Written-order left-to-right evaluation:** runtime lowering evaluates `a_0, a_1, ..., a_{n-1}` strictly left to right in **call-site source order** into temporaries. Only after all arguments finish evaluating does lowering pass parameters in signature order or populate aggregate fields in layout order. If `a_i` traps or propagates early via `?`, arguments `a_{i+1} .. a_{n-1}` never run.
2. **Deterministic declaration-order generic inference:** explicit type arguments (`Box<i64>(value = 0)`, `min<i64>(0, x)`) and enclosing expected types (`let r: Result<i64, string> = Result::Ok(0)`) seed generic bindings first (`spec/semantics/contextual_and_collection_literals.md`). Remaining unbound type parameters infer across bound arguments ordered by **declaration slot index `0 .. k + m - 1`**, never call-site permutation order. `Pair(first = 1, second = x)` and `Pair(second = x, first = 1)` therefore infer identical types.

### 1.8 Sequenced Compiler-First Migration

Existing `daoc` and self-hosted `stage2` compilers reject `_ name: Type` declarations, labeled `fn(...)` types, and `name = expr` on `class` and function calls. Compiler support must therefore land across both compiler tiers **before** any authored `.dao` source adopts the new syntax, keeping `bootstrap/stage2.sh` green at every commit:

1. **Additive compiler support in `compiler/` and `bootstrap/`:** land `_ name: Type` declarations, labeled `fn(...)` types, and `name = expr` binding across both compilers. Keep legacy unlabeled positional calls enabled temporarily so existing Stage 1, Stage 2, and Stage 3 builds stay green.
2. **Mechanical signature and call-site rewrite:** using the Phase-1 compilers, annotate genuine positional operations in `stdlib/` and `bootstrap/` with `_` (`Option::Some(_ value: T)`, `Result::Ok(_ value: T)`, `Result::Err(_ error: E)`, `Vector.push(self, _ elem: T)`, `Vector.get(self, _ index: i64)`, `HashMap.set(self, _ key: K, _ value: V)`, `range`, scalar and string helpers). Rewrite every non-matching unlabeled argument `arg_i` at a named slot into `slot_i.name = arg_i` (turning `Edge(to, from)` into `Edge(from = to, to = from)`). Drop `value =` and `error =` labels on `_`-prefixed `Option` and `Result` constructors.
3. **Strict unified-binding cutover:** remove legacy unlabeled positional fallback on `class` initializers and named `name: Type` parameters in both `compiler/` and `bootstrap/`, activating order-independent same-name elision (`ident` meaning `ident = ident`). Because Phase 2 labeled every non-identity argument while positional rules still held, **zero call sites can silently swap runtime values**.

## 2. Pattern Matching Ergonomics (`..` and `_`)

### 2.1 Bootstrap Parity for `..` (`DAO-H04`)

`CONTRACT_SYNTAX_SURFACE.md` and `ADR_ENUM_CLASS.md` already freeze trailing `..` in `enum class` destructuring (`Expr::Function(name, ret, ..):`). Native `daoc` implements `..`, and `DAO-H04` brings `bootstrap/parser`, `bootstrap/typecheck`, and `bootstrap/hir` to full parity.

### 2.2 Wildcard `_` Pattern

Dao reserves `_` as a non-binding marker in positional parameter declarations (`§1.2`), unlabeled callable type slots (`§1.4`), and two pattern positions, while keeping `_` illegal as an expression or local variable name:

1. **Catch-all `match` arm (`_:`):**
   ```dao
   match tok.kind:
     TK::Plus:
       return 1
     _:
       return 0
   ```
   - `_:` matches any remaining scrutinee value without introducing a lexical binding.
   - `_:` must appear as the final arm of a `match`, and any arm after `_:` diagnoses `"unreachable match arm after wildcard '_'"`.
   - If prior arms already cover every variant of a closed `enum` or `enum class`, `_:` diagnoses `"redundant wildcard '_' arm: all variants of '<Type>' are already covered"`.
2. **Ignored slot inside variant destructuring (`Variant(first, _, third)`):**
   - Inside `Enum::Variant(p_0, ..., p_{r-1})`, `_` occupies one field position in declaration order without binding a local symbol. Callers skip interior fields with `_` and ignore trailing fields with `..`.

Exhaustiveness and definite-return interaction with value-producing `if` and `match` expressions live in `spec/semantics/control_flow_expressions.md`.

## 3. Formatting

Canonical formatting (`tools/formatter/` target rules) keeps calls and aggregate initializers diff-stable and readable without column alignment churn:

1. **Single-line calls:** keep `Callee(a, b)` on one line when all arguments are short and total line width stays within `100` columns:
   ```dao
   let origin = Point(x = 0.0, y = 0.0)
   let clamped = clamp(speed, min = 0, max = 120)
   let wrapped = Result::Ok(clamped)
   ```
2. **Multiline trailing-paren layout:** when a call wraps, break right after `(` and place one argument per line at a **2-space** indent. Place the closing `)` on its own line matching the opening line's indent:
   ```dao
   let state = LexState(
     cursor,
     indent,
     tokens,
     diagnostics = diags
   )

   let node = Expr::Binary(
     op = Operator::Add,
     left,
     right
   )
   ```
3. **Single-space `=` without column padding:** write `name = expr` with exactly one space on each side of `=`. Never pad `=` across adjacent lines to align columns:
   ```dao
   // Canonical: adding or renaming a field touches one line.
   let conn = connect(
     host = primary_host,
     port = 5432,
     timeout_ms = 2500
   )

   // Non-canonical: column-aligned '=' churns unrelated lines on edits.
   let bad = connect(
     host       = primary_host,
     port       = 5432,
     timeout_ms = 2500
   )
   ```
