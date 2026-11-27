# `match` Semantics and the Arrow Taxonomy

Status: **proposed** (promotes to `docs/contracts/` upon acceptance)
Date: 2026-11-27

This note states the place of `match` in Dao's computational model, and derives
from that place a consistent meaning for each of `:`, `->` and `=>`.  It changes
no syntax.  The section "Open Decisions" lists the questions it leaves open.

Each rule carries one of three sources, with one exception.  **Contract** marks a rule that
`CONTRACT_SYNTAX_SURFACE.md`, `CONTRACT_TYPE_SYSTEM_FOUNDATIONS.md` or
`ADR_ENUM_CLASS.md` states.  **Proposed** marks a rule from
`control_flow_expressions.md` or `aggregate_initialization.md`.  **New** marks
a rule that no record states and that this note proposes, so adoption
would newly commit to it.  The exception is the implementation
record that section 1.3 cites: it describes the resolvers and is not a
language rule.
Section 1.6 records the behavior of the two
compilers.

## 1. Semantics of `match`

None of these rules depends on punctuation.

### 1.1 Evaluation

**New.**  No record states these four rules.

1. The program evaluates the subject expression exactly once, before any arm.
2. It tests the arms against the subject's value in written order, and
   selects the first arm whose pattern accepts the value.
3. It initializes the selected arm's bindings from the value, and then
   runs that arm's body.  No other arm's body runs, and control does not
   fall from one arm into the next.
4. Testing a pattern has no effect that a program can observe: a
   pattern holds no call and no user code.

### 1.2 Obligations of the Arm List

The body of a `match` is its arm list.  The header changes the reading of
that text and the duties of that text, in three ways.

1. **Pattern grammar.**  The text before an arm's separator is a pattern.
   `Result::Ok(value = value)` in an expression builds a value from a name
   `value` that is already in scope.  `Result::Ok(value)` as a pattern
   tests for the variant `Ok` and declares a new name `value`.  No
   construction runs, and the pattern reads no outer `value`.
2. **Typing against the subject.**  The type checker checks each pattern
   against the subject's type `S`.  A variant path must name a variant of
   `S`.  **Contract:** destructuring bindings follow the variant's field
   declaration order, so a binding's name is free and its type is the type
   of the field at its position.  **Contract:** `Variant as name` binds the
   whole variant, for field access without destructuring.
3. **Coverage.**  **Contract:** over an `enum class`, the arms must cover
   all variants.  **Proposed:** a value-position `match` over any type
   covers every value.  A scalar or `string` subject needs a trailing
   `_:` arm, except that arms for `true` and `false` cover a `bool`.  **Proposed:** an arm after `_:` is an error, and so is a `_:`
   arm after arms that cover every variant.  No record states a coverage
   rule for a statement-position `match` over a scalar or `string`
   subject, or a rule for two arms that name the same variant.

An `if`, `while` or `for` header imposes none of these on its body: the
body is an ordinary statement suite that reads the same under any header.

### 1.3 Bindings

A pattern's bindings are in scope in that arm's body alone, and neither a
later arm nor the code after the `match` can see them.  Both resolvers
already work this way: `docs/task_specs/TASK_39_BOOTSTRAP_MATCH_ARM_BINDINGS.md`
records that the arm opens a block scope and declares each binding into
it.  Adoption would make that a language rule.  **New:** selection
initializes the bindings once.

### 1.4 Result

- In statement position a `match` yields nothing.  Both compilers accept
  an indented suite alone as an arm's body.  **Proposed:** an arm's body
  may also be an inline expression, in either position.
- **Proposed.**  In value position each arm yields a value or diverges,
  all yielding arms agree on one type `T`, and the `match` has type `T`,
  or `never` when every arm diverges.  A value arm is an inline expression
  or an indented suite whose tail expression is the value.

### 1.5 Control Transfer

**Proposed** (`control_flow_expressions.md` §1.3 and §2.3).  A `match` is
not a callable boundary.  `return` in an arm returns from
the enclosing function, and `break` leaves the enclosing loop.  An arm's
value is the value of the `match` expression, never of the function.

### 1.6 Behavior of the Compilers

Both compilers analyze coverage to decide whether control can fall out of
a `match`: `TypeChecker::match_terminates_all_paths` in the host, and
`mir_reachable_blocks` in `bootstrap/llvm/impl.dao`.  Each therefore
rejects a function that must return a value and whose partial `match`
lets control reach its end.  Neither
reports a partial `match` whose fall-through is otherwise legal.

The host compiler has two paths for a pattern.  When the subject's enum
has a variant with a payload, the host compares the subject's
discriminant with the tag of the variant that the pattern names.  The
bindings decide only whether it extracts payload fields, so `Option::None`
takes this path too.  The host checks every other pattern as an
expression against the subject's type, and lowers it as that expression.
The comparison's other operand stays the discriminant for such an enum,
and is the subject's value for every other subject.  The host compiler
at this note's date gives these results:

| Program | Observed |
| --- | --- |
| a statement `match` on a three-variant `enum` with an arm for one variant, followed by a `return` | compiles, and the other values fall out of the `match` |
| two arms with the same variant | compiles, and the second never runs |
| an arm `1 + 1:` on an `i32` subject | compiles, and the subject `2` selects it |
| an arm `noisy(Color::Blue):` whose callee prints | compiles, and the call runs when the program tests the arm |
| an arm `_:` | error: unknown identifier `_` |
| an arm `Shape::Circle as c:` for a variant with one field | error: the variant expects one binding |
| an arm `V::Empty as whole:` for a fieldless variant | compiles, and `whole` has the subject's type |
| an arm `V::Pair(label, n):` for `Pair(count: i32, label: string)` | compiles, and the checker gives `label` the type `string` |

So an expression pattern can run user code today, against rule 1.1.4.
The wildcard is a proposal that neither compiler implements.  The host
accepts `as` after a fieldless variant and after a destructuring list,
such as `V::Pair(count, label) as whole:` or `V::Pair(..) as whole:`.  It rejects the contract's
bare form, `V::Pair as whole:`, which gives access to a payload's fields
without destructuring, so that form works in neither compiler.
The bootstrap parser lacks the `as` syntax: it expects `:`
directly after the pattern.  The host checker types a binding by a
matching field name before position, against the contract's
declaration-order rule, while its lowering extracts fields by position.
Neither compiler accepts an inline arm `P: expr`, since both parse a
suite after an arm's `:`.

The case for the header's mark in section 3 rests on rules 1.1.4 and
1.2.3, so each of these questions needs a decision before the punctuation
does:

- whether the grammar restricts a pattern to a variant path with
  bindings, a literal, or `_`, which makes rule 1.1.4 true by construction
- whether a statement-position `match` must cover its subject's type.
  For a scalar or `string` subject no record answers.  For an `enum` or
  `enum class` subject, `ADR_ENUM_CLASS.md` says that `enum` contributes
  "closedness and exhaustive matching", so accepting a partial `match`
  there would revise that record.
- whether a second arm for one variant is an error

## 2. Two Positions for a Mark

A `match` has two places where punctuation separates a head from the text
that it governs:

- **header → arm list**: `match subject ⟨H⟩` followed by the arms
- **pattern → arm body**: `Pattern ⟨A⟩` followed by a suite or an inline
  expression

They are different relations.  Section 1.2 describes the first.  The
second relates one alternative to the computation that runs under it
with that alternative's bindings.

### 2.1 Three Readings of the Arm Mark

An arm's body has a position and a form, and the two are independent.
The position is statement or value.  The form is an indented suite or,
as proposed, an inline expression.

| Reading | Meaning of `Pattern ⟨A⟩ body` | Holds for a statement arm | Holds for a value arm | Holds when the arm returns |
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
| `:` | type annotation, type-parameter constraint (`T: Concept`, `where T: Concept`), `class`/`enum`/`concept`/`extend` bodies, `if`/`while`/`for`, `match` header, `match` arms | a declaration's content, or an ordinary statement suite whose text reads the same under any header |
| `->` | expression-bodied functions, lambdas | the one expression whose value the construct produces |
| `=>` | `mode`, `resource` | a suite that its header's rules govern, in its reading or in its execution |

Here "a declaration's content" covers a name's type, a type parameter's
constraint, and a type's members.

Under these meanings:

- The `match` header meets the `=>` sentence and fails the `:` sentence,
  by section 1.2: its body follows the pattern grammar, takes its types
  from the subject, and owes coverage.
- A suite arm in statement position meets the `:` sentence when the
  pattern counts as a declaration of names with a suite under it, as a
  `for` header declares its loop variable.
- An inline value arm meets the `->` sentence.  So does each branch of an
  inline value `if` (`if c: a else: b`), which
  `control_flow_expressions.md` writes with `:`.
- A value arm with an indented suite meets neither sentence as written.
  It is no single expression, so `->` does not fit.  The same suite
  yields its tail in value position and nothing in statement position,
  so its reading depends on its position, and `:` does not fit either.
  The `:` sentence would need a second clause: a suite's tail expression
  is its value in value position.

## 4. Candidate Surfaces

All four keep the semantics of section 1.  Each row gives the header, the
mark of an arm with an indented suite in either position, and the mark of
an inline value arm.

| | Header | Suite arm | Inline value arm | Notes |
| --- | --- | --- | --- | --- |
| A | `match s =>` | `P:` suite | `P: expr` | arms stay as `control_flow_expressions.md` proposes, and one token changes per `match` |
| B | `match s =>` | `P:` suite | `P -> expr` | `->` marks every single yielded expression, which raises the same question for inline value `if` |
| C | `match s:` | `P =>` suite | `P => expr` | puts `=>` on the arm, where no rule set changes, and leaves `:` on the header, where one does |
| D | `match s:` | `P:` suite | `P: expr` | keeps today's punctuation and adds the proposed inline arm, so `:` keeps two meanings |

A header change touches 549 `match` headers in tracked `.dao` files: 485
in `bootstrap/`, 56 in `examples/` and 8 in `stdlib/`.  Programs inside
test strings and snippets in prose add to that count.  The change also
touches both parsers, the formatter, and the grammar files.  In the
contracts it touches the syntax contract's "Arrow Taxonomy" and "Modes
and Resources" sections, and "Control vs Context" in
`CONTRACT_EXECUTION_CONTEXTS.md`.

`CONTRACT_EXECUTION_CONTEXTS.md` says that a `mode` block binds no
user-defined names, and that a `resource` block binds a named domain.  A
`match` on `=>` would be a third context: one whose body binds names per
arm.

The proposal also admits an inline arm in statement position, such as an
arm that only calls `print`.  Candidate B gives that arm no mark: `->`
there would mark an expression whose value the program discards.
Candidates A, C and D give it the mark of their inline value arm.

## 5. Open Decisions

1. The three questions of section 1.6, which fix the semantics of `match`.
2. Whether the header moves to `=>` (A or B), and whether the contracts'
   wording of `=>` widens from "mode and resource suites" to the meaning
   in section 3.
3. Whether an arm takes `=>` in place of the header (C).
4. Whether an inline value arm takes the form `P: expr` or `P -> expr`,
   and, with `->`, whether the inline value `if` follows and which mark
   an inline arm in statement position takes.
5. Whether the meaning of `:` gains the clause for a value suite's tail
   that section 3 describes.
6. Whether `match` joins `CONTRACT_EXECUTION_CONTEXTS.md` as a context,
   or the syntax contract alone defines `=>`.
7. The order of work for a syntax revision: the contract first, then both
   parsers accepting the new form, then a formatter-driven migration,
   then removal of the old form.
