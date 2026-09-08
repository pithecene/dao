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
7. A value allocated in a domain does not outlive it.  A heap-owning
   value — a string, a generator, or a class or enum holding one by
   value — may not leave the block: the compiler rejects a store to a
   binding declared outside the block (directly, or through a field
   or index rooted at it) and a `return` of such a value from inside
   the block.  Copy-out — the compiler copying what leaves into the
   enclosing domain — is Task 35 E1 and amends this law when it lands.
   Pointer values are the author's responsibility, as everywhere.
8. A `resource memory` block may not contain `yield`: a domain cannot
   stay current across a suspension.

## Intent

Dao distinguishes:
- *what code does* (`if`, `while`, `for`)
- *what rules are in effect* (`mode`)
- *what scoped domain is bound* (`resource`)
