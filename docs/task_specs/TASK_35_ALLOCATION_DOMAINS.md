# Task 35 — Allocation Domains

Status: **spec** — implementation not started.  Order 4 of the delivery
sequence moves behind this task: the bootstrap closure audit (Task 34)
found capacity, not a construct, to be the first blocker.

## 1. Objective

Make `resource memory <name> =>` mean what the language already says it
means: a scoped allocation domain.  Every allocation performed while a
domain is the current domain comes from that domain, and leaving the
domain reclaims all of it at once.  Values that must outlive the domain
are copied into the enclosing domain at exit, by a rule the compiler
applies and the contract states.

## 2. Why this task exists now

The closure audit measured the bootstrap needing 5–16 GiB and 16–54 s
to bring its own 3–9k-line programs through its pipeline (each stage
measured in its own process; the three largest programs exhaust a
16 GiB bound before reaching MIR), and an unbounded run was OOM-killed
on a 32 GB machine.  The cause is not the
threading of state by value (a class carrying a million-element
`Vector` through two thousand calls costs 18 MB — the generation-counter
handle works) but the string model: `__dao_str_concat` and every
string-producing hook `malloc` a fresh buffer and nothing ever frees
one, so a builder loop retains O(n²) bytes (100k appends of ten bytes:
31 GB, killed).  The runtime ABI already records this posture
("allocations leak until process exit; future arena or GC integration
will reclaim them") and already reserves the domain handle ABI for
"future arena semantics without signature churn"; `resource memory`
already parses, resolves, type-checks, and lowers to paired
`__dao_mem_resource_enter` / `__dao_mem_resource_exit` calls on
fall-through and `return` (`break` over-exits today; §3.1).  The
runtime side is a counter and a no-op.  This task fills that in.

Alternatives considered and set aside:

- **Reference-counted strings.**  Retain/release on every copy and
  scope exit, emitted by the backend.  General, syntax-free, and
  fine-grained — but it is a second memory model beside domains, and
  its cost is paid on every assignment everywhere rather than at a
  scope boundary the author chose.
- **Free-on-reassignment for locals.**  Unsound without aliasing
  knowledge: `let t = s` then `s = s + x` would free `t`'s buffer.
- **A builder type alone.**  A `Vector<u8>`-backed builder removes the
  quadratic pattern but not the leak; it is adopted here as the
  idiom for text assembly, not as the fix.

## 3. Semantic model

### 3.1 Domains

- A **domain** is an allocation region with a lifetime bounded by a
  lexical `resource memory` block.  Domains nest; the innermost active
  domain is the **current domain**.
- The **root domain** is the process: allocations outside every block
  have process lifetime, as today.
- Entering a block makes its domain current; leaving it — by falling
  off the end, `break`, or `return` — reclaims every allocation the
  domain made, then restores the enclosing domain.  The MIR builder
  pairs every enter with an exit on fall-through and on `return`; on
  `break` it exits *every* active region, including a `resource` that
  encloses the loop, whose fall-through exit is then emitted again
  (`compiler/ir/mir/mir_builder.cpp`, `HirBreak` → `emit_region_exits`).
  That is harmless while exit is a no-op and a double reclaim once it
  is not, so E0 makes `break` unwind only the regions entered inside
  the loop (the builder records the active-region depth at loop entry
  beside the loop's exit block); the pairing is then exact on every
  path.
- Reclamation is wholesale: no per-object frees, no destructors, no
  finalization order.  Domains are arenas.
- A domain does not stay current across a generator suspension: a
  `resource` block in a generator function may not contain `yield`.
  Were it allowed, the consumer would run with the generator's domain
  current (its own allocations landing there), a value yielded from
  inside the block would be reclaimed under the consumer at the next
  resume, and an iterator abandoned mid-block would never exit the
  domain at all.  E0 rejects it in the type checker (`yield` inside a
  `resource` block: a diagnostic naming the block); detaching a domain
  at `yield` and reattaching at resume is a non-goal (§11).  A
  generator *created* inside a domain allocates its frame there and,
  like any heap-owning value, may not escape it (§3.3).

### 3.2 What allocates

Every heap allocation in a Dao program goes through the runtime's
memory hooks and therefore lands in the current domain:

- `__dao_mem_alloc` / `__dao_mem_realloc` / `__dao_mem_free` (already
  the stdlib's path for `Vector`, `HashMap`, and raw memory)
- string buffers: `__dao_str_concat`, `__dao_str_substring`, every
  `__dao_conv_*_to_string`, `__dao_io_read_file` (today: raw `malloc`)
- generator frames: `__dao_gen_alloc` / `__dao_gen_free`
- class values are not heap-allocated by the language (no implicit
  heap allocation is a vision commitment); a class value's heap-owning
  fields are the fields' own allocations

`__dao_mem_free` inside a domain becomes a no-op for domain memory:
reclamation is the domain's.  In the root domain it frees as today.

### 3.3 Escape: values that outlive their domain

A value allocated in a domain is invalid after the domain is reclaimed.
The compiler therefore **copies escaping values into the enclosing
domain at exit**.  What is copied depends on the exit being taken:

1. at a fall-through or `break` exit: each binding declared outside
   the block that was assigned inside it *on the path that reaches
   this exit*.  The region-wide set of such bindings is static; which
   of them were assigned on the path taken is not (`let out: string`
   with no initializer, assigned in one arm of an `if` and not the
   other), so each escaping binding carries a per-region dirty flag —
   cleared at entry, set at every assignment to it inside the block —
   and the exit copies a binding only when its flag is set.  A binding
   never assigned on the path taken is not read, let alone copied.
2. at a `return` exit from inside the block: the return value.  Outer
   bindings are not copied on a return exit: the function is leaving
   them, and nothing reads them afterwards.

Copy-out is type-directed and deep for heap-owning types — `string`,
`Vector<T>`, `HashMap<V>`, and any class or enum whose fields
transitively hold one — and a no-op for scalars and pointer values
(pointers are the author's responsibility, as everywhere in `mode
unsafe`).  A generator is not copied: its frame layout is private to
the generator function (`CONTRACT_RUNTIME_ABI.md`, generator
representation), so a copier chosen by the static type `Generator<T>`
cannot know the frame's size or which of its slots own heap memory.
An escaping generator stays rejected (the E0 diagnostic remains for
it) until frames carry a per-function copy descriptor, which is
outside this task (§11).  The rule is explicit and predictable: the cost of a
domain is one copy of what leaves it, paid at the boundary the author
wrote.  Nothing is copied for values that do not leave.

Bindings declared outside the block and only *read* inside it are not
touched.  A binding declared inside the block that is not reachable
from the outside is reclaimed with the domain.

### 3.4 Determinism

Domain behaviour is a pure function of the program's control flow; no
allocation is reclaimed at an unspecified time.  Peak memory of a
domain-structured program is the sum of live domains, which the author
can read off the source.

## 4. Contract changes (land first)

- `CONTRACT_EXECUTION_CONTEXTS.md` §Resources: add the allocation
  semantics of `resource memory` (§3.1), the escape rule (§3.3), and
  the statement that reclamation is wholesale.
- `CONTRACT_RUNTIME_ABI.md`: replace the "scope/lifetime bookkeeping
  only" status with the arena semantics; require every string- and
  frame-producing hook to allocate through the memory hooks; state
  that `__dao_mem_free` on domain memory is a no-op; keep the handle
  ABI unchanged (it was designed for this) and add two hooks:
  `__dao_mem_alloc_outer(size, align)`, which allocates in the parent
  of the current domain, for copy-out; and
  `__dao_str_from_bytes(ptr, len)`, which returns a `dao_string` whose
  buffer is a copy of the bytes owned by the current domain — the
  string surface today can only inspect and combine strings, and a
  byte-backed builder (§7) needs a way to make one.  Rule 3/6 of
  "Ownership and lifetime" change from "leak until process exit" to
  "owned by the current domain".

## 5. Runtime design

- A domain is a chunk list (geometrically growing, first chunk sized
  for small blocks) with a bump pointer; `__dao_mem_alloc` bumps in the
  current domain; `__dao_mem_realloc` reallocates within the domain
  (copy into a new bump allocation when growth does not fit; the old
  block is simply abandoned to the domain).
- The runtime keeps the domain stack; `__dao_mem_resource_enter`
  pushes a new domain and returns it as the handle,
  `__dao_mem_resource_exit(handle)` pops to it, reclaiming every domain
  above and including it (a `return` through nested blocks exits each
  in order already; the runtime tolerates exiting an outer handle with
  inner domains still open).
- The root domain uses the system allocator directly so process-lifetime
  behaviour is unchanged and `__dao_mem_free` keeps freeing there.
- Large allocations (above a chunk) get their own chunk in the domain.
- No thread-safety in this slice (single-threaded runtime today; `mode
  parallel` is not implemented).

## 6. Compiler changes

- **Backend, copy-out.**  At every `MirResourceExit` the backend emits,
  before the exit call, a copy into the enclosing domain for each
  escaping value (§3.3).  The MIR builder already tracks active regions
  and emits exits on every path; it gains, per region, the set of
  outer bindings assigned within it, one dirty flag per such binding
  (a local the region's entry clears and each assignment inside sets),
  and, on a `return` exit, the return value.  A fall-through or
  `break` exit copies each binding under its flag; a `return` exit
  copies the return value only (§3.3).  Copies are emitted through per-type copier functions the
  backend generates on demand (`dao.copy.<mangled type>`).  The copy
  must be made *before* the exit call — its source is in the domain
  about to be reclaimed — and must land in the *enclosing* domain, so
  copiers allocate through one new hook, `__dao_mem_alloc_outer(size,
  align)`, which allocates in the parent of the current domain: the
  domain that becomes current at the exit about to happen.  Which
  domain that is only the runtime knows.  A function whose outermost
  `resource` block runs while its caller has a domain open must copy
  into the caller's domain, and no `MirResourceEnter` in the callee
  names it; a hook taking a handle inferred from the callee's MIR
  (the enclosing enter, or null for "root") would copy such values
  into process-lifetime memory and recreate the leak this task
  removes.  The runtime's domain stack has the answer; the hook asks
  it.
- **Type checker.**  Records, for each `resource` block, the outer
  bindings assigned inside it (it already knows binding scopes); no
  new diagnostics in this slice.
- **HIR/MIR.**  `HirResource` gains the escaping-binding list; the MIR
  region record carries it to the exit instruction.

## 7. Standard library

- Route every string-producing runtime hook through `__dao_mem_alloc`
  (runtime change; no stdlib surface change).
- Add `core::text::Builder`: a `Vector<u8>`-backed accumulator with
  `push(string)`, `push_char`, and `to_string()`, the last through
  `__dao_str_from_bytes` (§4): one copy of the bytes into a string the
  current domain owns.  The idiom for assembling text; its growth is
  amortized and its dead buffers are the domain's.

## 8. Bootstrap adoption (the reason for the task)

- Each pipeline stage runs inside a `resource memory` block whose
  result is copied out (one copy of a `LexResult` / `ParseOutput` / …
  per stage), so a stage's scratch — every intermediate string, every
  abandoned vector buffer — is reclaimed when the stage ends.
- The LLVM text serializer and the diagnostic builders use
  `core::text::Builder`.
- `bootstrap/audit_closure.sh` then measures the result: the peak-memory
  column is the acceptance test.

## 9. Delivery

- **E0 — Runtime arenas.**  Domain stack, chunked arenas, hooks routed
  (strings, conversions, file reads, generator frames); the MIR
  builder's `break` unwinds only the regions the loop entered (§3.1);
  the type checker rejects `yield` inside a `resource` block (§3.1).
  Behaviour unchanged for programs without `resource memory` (root
  domain); programs with blocks reclaim at exit.  No copy-out yet: a value that
  escapes a domain is a use-after-reclaim, so E0 lands with the type
  checker **rejecting** escapes (a diagnostic naming the binding) until
  E1 — the conservative rule keeps E0 sound.
- **E1 — Copy-out.**  §6; the E0 diagnostic is removed for heap-owning
  types the copiers cover.
- **E2 — `core::text::Builder`** and stdlib sweep for quadratic
  concatenation.
- **E3 — Bootstrap adoption** (§8) and the audit rerun.

## 10. Tests

- runtime: enter/alloc/exit reclaims; nested domains; realloc within a
  domain; large allocations; `__dao_mem_free` no-op inside, real
  outside; root domain unchanged
- runtime: `__dao_str_from_bytes` copies into the current domain
- MIR: `break` out of a loop inside a `resource` block exits only the
  regions the loop entered (one exit per enter on every path, checked
  on the MIR text); `break` out of a `for` inside a `resource`
  destroys the iterator exactly once (today it is destroyed during
  unwinding and again in the loop's exit block)
- backend: copy-out of a string, a `Vector<i32>`, a class holding both;
  early `return` from nested blocks copies the return value through
  each and no outer binding; an outer `let out: string` assigned in
  one `if` arm and not the other is copied only on the path that
  assigned it (the other path reads no flagless binding); a value not
  escaping is not copied (IR contains no copier call); a callee's
  outermost block copies into the caller's open domain, not the root
  (the caller's domain exit reclaims it)
- typechecker (E0): escaping heap-owning binding diagnosed; scalar
  escape not; `yield` inside a `resource` block diagnosed; an escaping
  generator stays diagnosed after E1
- examples: `resource.dao` extended with an escaping string and an
  escaping vector; output unchanged
- audit: peak-memory column below 2 GiB for every bootstrap program is
  the acceptance criterion for E3

## 11. Non-goals

- garbage collection, reference counting, destructors, or finalizers
- per-object deallocation inside a domain
- cross-domain sharing without copying
- copying a generator out of a domain (its frame would need a
  per-function copy descriptor; escaping generators stay rejected)
- a domain staying current across a generator suspension (detach at
  `yield`, reattach at resume, exit on abandonment); `yield` inside a
  `resource` block stays rejected
- `mode parallel` interaction (no concurrent domains)
- `resource` kinds other than `memory`

## 12. Risks

- **Copy-out cost on hot paths.**  A stage result copied once per stage
  is negligible; a domain inside an inner loop whose result escapes
  each iteration is not.  The idiom is documented; the compiler does
  not warn in this slice.
- **Deep copy of graphs.**  Copiers follow the static type; a class
  value that holds pointers into domain memory (`mode unsafe`) is the
  author's responsibility, as the contract says for pointers.
- **Generic instantiations of copiers.**  One copier per concrete
  type, generated on demand by the backend — the same machinery as
  intrinsics.

## 13. One-sentence summary

`resource memory` becomes a real arena: everything allocated inside is
reclaimed at exit, what leaves is copied out by the compiler, the
runtime's string hooks stop leaking, and the bootstrap's stages run in
domains — which is how the closure audit's first blocker is removed
without a new syntax or a garbage collector.
