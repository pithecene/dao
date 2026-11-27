# `match` Semantics and the Arrow Taxonomy

Status: **proposed** (promotes to `docs/contracts/` upon acceptance)
Date: 2026-11-27

This note states the place of `match` in Dao's computational model, and derives
from that place a consistent meaning for each of `:`, `->` and `=>`.  It changes
no syntax.  The section "Open Decisions" lists the questions it leaves open.

## 1. Semantics of `match`

These rules describe the construct that exists today
(`CONTRACT_SYNTAX_SURFACE.md`, `ADR_ENUM_CLASS.md`) together with the
value form that `control_flow_expressions.md` proposes.  None depends on
punctuation.

### 1.1 Evaluation

1. The program evaluates the subject expression exactly once, before any arm.
2. It tests the arms against the subject's value in written order, and
   selects the first arm whose pattern accepts the value.
3. It initializes the selected arm's bindings from the value, and then
   runs that arm's body.  No other arm's body runs, and control does not
   fall from one arm into the next.
4. Testing a pattern has no effect that a program can observe: a pattern
   holds no call and no user code.  This rule is a proposal, as section
   1.6 explains.

### 1.2 Obligations of the Arm List

The body of a `match` is its arm list.  The header changes the reading of
that text and the duties of that text, in three ways.

1. **Pattern grammar.**  The text before an arm's separator is a pattern,
   not an expression.  `Result::Ok(value)` in an expression is a call
   that uses a name `value`.  The same text as an arm's pattern is a
   test for the variant `Ok` that declares a new name `value`.
2. **Typing against the subject.**  The type checker checks each pattern
   against the subject's type `S`.  A variant path must name a variant of
   `S`, a field name must be a field of that variant, and a binding takes
   its field's type.  `Variant as name` binds the matched value.
3. **Coverage.**  The arms together must accept every value of `S`.  A
   missing variant is an error, and so is an arm that no value can reach.
   The contracts state the first half alone, as section 1.6 explains.

An `if`, `while` or `for` header imposes none of these on its body: the
body is an ordinary statement suite that reads the same under any header.

### 1.3 Bindings

A pattern's bindings are in scope in that arm's body alone.  Selection
initializes them once, and neither a later arm nor the code after the
`match` can see them.

### 1.4 Result

- In statement position a `match` yields nothing, and arm bodies are
  suites.
- In value position (proposed) each arm yields a value or diverges, all
  yielding arms agree on one type `T`, and the `match` has type `T`, or
  `never` when every arm diverges.

### 1.5 Control Transfer

A `match` is not a callable boundary.  `return` in an arm returns from
the enclosing function, and `break` leaves the enclosing loop.  An arm's
value is the value of the `match` expression, never of the function.

### 1.6 Differences in the Implementation

The contracts say "match is exhaustive" (`CONTRACT_SYNTAX_SURFACE.md`,
`CONTRACT_TYPE_SYSTEM_FOUNDATIONS.md`), and `ADR_ENUM_CLASS.md` says that
the compiler requires arms for all variants.  The grammar gives a pattern
as `"_" | inline_expression ("as" identifier)?`, and the type checker
checks it as an expression against the subject's type.  The host compiler
at this note's date accepts each of these programs without a diagnostic:

| Program | Observed |
| --- | --- |
| a `match` on a three-variant `enum` with an arm for one variant | compiles, and the other values fall out of the `match` |
| two arms with the same variant | compiles, and the second never runs |
| an arm `1 + 1:` on an `i32` subject | compiles, and the subject `2` selects it |
| an arm `noisy(Color::Blue):` whose callee prints | compiles, and the call runs when the program tests the arm |

Neither compiler holds a coverage check: no diagnostic for a missing
variant exists in the host type checker or in `bootstrap/typecheck`,
`bootstrap/hir` or `bootstrap/mir`.  These probes ran through the host
compiler alone.

So today a pattern is an expression that the program compares with the
subject, and its evaluation can have effects.  Rules 1.1.4 and 1.2.3 are
therefore proposals.  Each needs a decision before the punctuation does,
because the case for the header's mark in section 3 rests on them:

- whether the grammar restricts a pattern to a variant path with
  bindings, a literal, or `_`, which makes rule 1.1.4 true by construction
- whether statement-position `match` must be exhaustive, as the
  contracts say, or only value-position `match`, as
  `control_flow_expressions.md` §2.2 words it
- whether an unreachable arm is an error

## 2. Two Positions for a Mark

A `match` has two places where punctuation separates a head from the text
that it governs:

- **header → arm list**: `match subject ⟨H⟩` followed by the arms
- **pattern → arm body**: `Pattern ⟨A⟩` followed by a suite or a value

They are different relations.  Section 1.2 describes the first.  The
second relates one alternative to the computation that runs under it
with that alternative's bindings.

### 2.1 Three Readings of the Arm Mark

| Reading | Meaning of `Pattern ⟨A⟩ body` | Holds for a suite arm | Holds for a value arm | Holds when the arm returns |
| --- | --- | --- | --- | --- |
| Result of decomposition | "taking the value apart this way gives `body`" | no: a suite of effects is not a result | yes | no |
| Value of the alternative | "this alternative's value is `body`" | no | yes | no: `return` gives a value to the function |
| Scope under the alternative | "with these bindings in effect, run `body`" | yes | yes: the body's tail is the value | yes |

Only the third reading covers every arm that Dao allows.  The first two
describe value arms alone.

## 3. Candidate Meanings of the Marks

A mark has a consistent meaning when one sentence is true of every
construct that carries the mark.

| Mark | Constructs today | Candidate meaning |
| --- | --- | --- |
| `:` | type annotation, `class`/`enum`/`concept`/`extend` bodies, `if`/`while`/`for`, `match` header, `match` arms | a declaration's content, or an ordinary statement suite whose text reads the same under any header |
| `->` | expression-bodied functions, lambdas | the one expression whose value the construct produces |
| `=>` | `mode`, `resource` | a suite that its header's rules govern, in its reading or in its execution |

Under these meanings:

- The `match` header meets the `=>` sentence and fails the `:` sentence,
  by section 1.2: its body follows the pattern grammar, takes its types
  from the subject, and owes coverage.
- A suite arm meets the `:` sentence when the pattern counts as a
  declaration of names with a suite under it, as a `for` header declares
  its loop variable.
- A value arm meets the `->` sentence.  So does each branch of an inline
  value `if` (`if c: a else: b`), which `control_flow_expressions.md`
  writes with `:`.

## 4. Candidate Surfaces

All four keep the semantics of section 1.

| | Header | Suite arm | Value arm | Notes |
| --- | --- | --- | --- | --- |
| A | `match s =>` | `P:` suite | `P: expr` | arms stay as `control_flow_expressions.md` proposes, and one token changes per `match` |
| B | `match s =>` | `P:` suite | `P -> expr` | `->` means value production everywhere, which raises the same question for inline value `if` |
| C | `match s:` | `P =>` suite | `P => expr` | puts `=>` on the arm, where no rule set changes, and leaves `:` on the header, where one does |
| D | `match s:` | `P:` suite | `P: expr` | today's surface, in which `:` keeps two meanings |

A header change touches 1144 `match` headers across `bootstrap/`,
`stdlib/`, `examples/`, `spec/` and `testdata/`.  It also touches both
parsers, the formatter, and the grammar files.  In the contracts it
touches the syntax contract's "Arrow Taxonomy" and "Modes and Resources"
sections, and "Control vs Context" in `CONTRACT_EXECUTION_CONTEXTS.md`.

`CONTRACT_EXECUTION_CONTEXTS.md` says that a `mode` block binds no
user-defined names, and that a `resource` block binds a named domain.  A
`match` on `=>` would be a third context: one whose body binds names per
arm.

## 5. Open Decisions

1. The three questions of section 1.6, which fix the semantics of `match`.
2. Whether the header moves to `=>` (A or B), and whether the contracts'
   wording of `=>` widens from "mode and resource suites" to the meaning
   in section 3.
3. Whether a value arm takes the form `P: expr` or `P -> expr`, and, with
   `->`, whether the inline value `if` follows.
4. Whether `match` joins `CONTRACT_EXECUTION_CONTEXTS.md` as a context,
   or the syntax contract alone defines `=>`.
5. The order of work for a syntax revision: the contract first, then both
   parsers accepting the new form, then a formatter-driven migration,
   then removal of the old form.
