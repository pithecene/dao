# Examples

Illustrative Dao programs live here.
Keep them small and human-readable.

Every file is a complete program meant to build with `daoc build` and
run.  The playground lists this directory, and each file is a
regression input, so a new language feature should land with an
example that exercises it.  `playground_service_test` compiles and
runs every file, compares its output with `testdata/examples/<name>.out`
(`task update-example-goldens` rewrites them), and requires every token
to be classified.  One file does not build today: `raw_memory.dao`
trips a MIR concreteness invariant in the host compiler (pre-existing);
it stays here as the regression input for that fix and is listed in
`testdata/examples/known_failures.txt` with that diagnostic.

| File | Demonstrates |
|------|--------------|
| `hello.dao` | the smallest program |
| `arithmetic.dao`, `math.dao`, `i64.dao`, `conversions.dao` | integer and float arithmetic, widths, explicit conversions |
| `checked_arithmetic.dao` | `checked_*` / `wrapping_*` / `saturating_*` overflow operations |
| `control_flow.dao`, `loops.dao` | `if` / `while` / `for`, `break` |
| `strings.dao` | string literals, concatenation, length |
| `structs.dao` | classes as value types, construction, field access |
| `methods.dao` | class methods, static methods, chaining, generic classes |
| `concepts.dao` | concepts, `extend T as Concept:`, generic bounds and dispatch on builtin receivers |
| `generics.dao` | generic functions and classes, inference, explicit type arguments, `Option<T>` |
| `enums.dao`, `enums_payload.dao`, `generic_enums.dao`, `match.dao` | enums, payload variants, `match`, `?` |
| `lambdas.dao` | function types, functions as values, `\|>` pipelines |
| `pipelines.dao` | the pipe operator on plain functions |
| `generators.dao`, `iteration.dao` | `yield`, generators, `for ... in` |
| `vectors.dao`, `hashmap.dao` | `Vector<T>` and `HashMap<V>` from the prelude |
| `resource.dao`, `unsafe.dao`, `raw_memory.dao` | `resource memory` domains, `mode unsafe`, pointers and allocation |
| `astar.dao` | a small search algorithm using several of the above |
| `ffi/` | calling C from Dao (needs the C helpers alongside) |
| `bootstrap_probe/` | historical probes that preceded `bootstrap/`; kept as artifacts |
