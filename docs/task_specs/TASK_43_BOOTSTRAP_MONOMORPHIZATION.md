# Task 43 — Bootstrap Monomorphization

Status: implementation spec
Phase: Tier B-Bootstrap, authorized after Task 42
Scope: make the program's MIR concrete, as the host's `monomorphize`
(`compiler/ir/mir/mir_monomorphize.cpp`) does: every call of a generic
function names a concrete copy of it, specialized for what the call
decided; a call through a type parameter's bound names the method the
concrete type provides; a value copied out of a memory domain is copied
by what its concrete type needs.  After it, no module function mentions
a type parameter.

## 1. What Task 42 leaves

Task 42 lowers the whole program to MIR with generic functions as
templates, kept out of each module's function list.  Concrete code
calls them: `Vector<i64>::new()`, `v.push(x)`, `HashMap<i64>::get`,
`copy_out(x)` at a domain's exit.  The backend rejects every such
reference (`generic function '…' has no concrete code until an
instantiation names it`), so no compiler program lowers to LLVM.

## 2. How the host does it (the parity target)

- Templates are program-wide, keyed by symbol; the pass iterates to a
  fixpoint over the module's functions, rewriting each reference to a
  template into a reference to a specialization, cloning the template
  with its type parameters substituted when a specialization is new
  (`specialize_call_site`, `clone_function`).
- A specialization is keyed by the template and what each of its
  parameters stands for; it is named `name$T1_T2` (`MangledTypeNames`).
- A call's substitution is inferred from its argument types, with type
  arguments written at the call deciding their positions.
- After a clone, a member access on a now-concrete non-class value is
  the concrete type's method (`fixup_method_calls`): the access becomes
  a reference to it and the receiver its first argument.
- `copy_out<T>(x)` is expanded per concrete T before specialization:
  `copy_out_string` for a string, the class's own `copy_out(self)`
  copier, per field otherwise; per variant for an enum; the identity for
  what owns no heap memory (`expand_copy_out_calls`).
- Intrinsics (`size_of<T>`) have no body; the reference's type is made
  concrete from the call's written type arguments.
- Concreteness invariant: a module function whose signature, locals or
  instruction types mention a type parameter is an internal error.

## 3. The bootstrap's shape

The bootstrap's checker already decides every call's type parameters
(`decided`, Task 41 slice 7) — from the receiver, the written type
arguments and each argument.  So the substitution a specialization
needs is recorded where it is decided, not re-inferred from MIR value
types: the checker records each call's decided map; HIR and MIR carry
it on the call; a clone substitutes it by its own substitution.  Types
are made by the checker's own machinery (`substitute_type`,
`instantiate_generic`), which needs only the type tables and the
program-wide registry, so the checker publishes its registry beside the
program (as `program_lower_hir` publishes the HIR arena).

## 4. Slices

1. **The checker's registry and each call's substitution, published** —
   `ProgramTyped` carries the registry; the checker records the decided
   map of every call to a generic callee; HirCall / MirCall carry it.
2. **Specialization of generic functions** — a fixpoint over the
   program's concrete functions: a reference to a template, with its
   call's substitution composed with the enclosing specialization's,
   names the specialization keyed by (template, arguments), cloned with
   every type index substituted, named `name$…`; the concreteness
   invariant replaces the backend's template-reference check.
3. **Methods of generic classes and static factories** — the receiver's
   instantiation and the type written on the callee decide the class's
   parameters; `Vector<i64>::new()`, `v.push(x)`.
4. **Calls through a bound** — after substitution a member access on a
   concrete value is the concrete type's method, dispatched as the
   checker dispatches it.
5. **`copy_out` expansion** — per concrete type, as the host does.
6. **Intrinsics** — what the audit's intrinsic list shows the corpus
   uses (`size_of<T>` and its kin).

Each slice: probe the host (`daoc mir` prints post-monomorphization
MIR), write the tests from its output, implement, one review pass
checked against the host.  The audit gains a `mono` measure: the
compiler program's functions that are concrete after the pass, and the
concreteness diagnostics left.

## 5. Non-goals

Program-level LLVM (Task 44); the Stage-2 crossing (Task 45).

## 6. Acceptance

Every compiler program and the prelude it uses are concrete after the
pass: no template reference and no type parameter left in any module
function; the audit shows it.

## 7. Delivered

Five slices, each checked against `daoc mir`: every call's substitution
recorded where the checker decides it and carried on HIR and MIR calls
(1); specializations keyed by template and arguments, cloned with every
type substituted and named `name$…`, the concreteness invariant in place
of the backend's template-reference check (2), which covered generic
classes' methods and static factories; a call through a bound
dispatched to the concrete receiver's method (3); `copy_out` expanded
per concrete type (4); the audit's `mono` column (5).  The intrinsics
`size_of$T` / `align_of$T` stay calls for the backend to inline, as the
host's do.  The closure report reads zero `mono` diagnostics on every
compiler program.
