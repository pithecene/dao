# CONTRACT_EXECUTION_CONTEXTS.md

## Purpose

Defines Dao's visual taxonomy for execution and resource context.

## Control vs Context

- structural control flow uses `:`
- semantic or execution context uses `=>`

These forms are not interchangeable.

## Modes

Modes alter execution, safety, or scheduling rules.

Canonical early modes:
- `mode unsafe =>`
- `mode gpu =>`
- `mode parallel =>`

Laws:
1. `mode` blocks are unary.
2. `mode` blocks do not bind user-defined names.
3. `mode` blocks alter execution semantics, not ordinary control flow.

## Resources

Resources bind named scoped domains.

Canonical early resource form:

```dao
resource memory Search =>
    ...
```

Laws:
1. `resource` blocks are parameterized.
2. Resource names are not types.
3. Resource names are not allocator objects.
4. Resource semantics are lexical and scope-bounded.
5. Resource-specific implementation strategies may vary under the hood,
   but visible semantics must remain stable.
6. `resource memory` binds an **allocation domain**: every allocation
   made while the block is current comes from the domain, and leaving
   the block — by falling off its end, `break`, or `return` — reclaims
   the domain wholesale.  Domains nest; the innermost open block's
   domain is current; outside every block the process is the domain.
7. A value allocated in a domain does not outlive it.  What leaves a
   block is copied into the enclosing domain at the block's exit, by
   the compiler: a binding declared outside the block that the block
   stores to (directly, or through a field or index rooted at it) is
   copied whole at a fall-through or `break` exit when a store to it
   ran on the path taken; the value a `return` or `?` carries out of
   the block is copied at that exit.  The copy follows the static
   type: a `string` by its bytes; a class or enum field by field,
   unless the class declares a method `copy_out(self)` returning its
   own type, which the compiler calls instead (`Vector` and `HashMap`
   do, since their raw pointer field owns what it points at; a method
   of that name with any other signature is an ordinary method);
   scalars and pointer values unchanged.  A generator is not copied:
   the compiler rejects a generator leaving a block -- by itself, or
   inside a class or container that holds one, through a raw pointer
   as `Vector` does -- stored to an outer binding or returned from
   inside it.  A pointer value itself is the
   author's responsibility, as everywhere.
8. A `resource memory` block may not contain `yield`: a domain cannot
   stay current across a suspension.
9. A `resource` block scopes part of a body.  A block that is a
   function's whole body is a domain policy hidden from every caller --
   each pays a copy of the result, none can share a domain across
   calls -- and is diagnosed (a warning: the program is sound); the
   block belongs at the call site.

## Intent

Dao distinguishes:
- *what code does* (`if`, `while`, `for`)
- *what rules are in effect* (`mode`)
- *what scoped domain is bound* (`resource`)
