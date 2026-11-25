# Semantics

This directory contains semantic proposals and design notes below binding
contract level (`docs/contracts/`) and above syntax probes.

Accepted proposals promote into `docs/contracts/`.

Contents:
- `aggregate_initialization.md`: proposed unified call binding (`_ name: Type` positional prefix, `name = expr` labels, same-name elision), dotless structural aggregate initialization, and `_` pattern wildcards
- `contextual_and_collection_literals.md`: proposed contextual literal sinks and homogeneous `Vector<T>` collection literals
- `control_flow_expressions.md`: proposed value-producing `if` and `match` expressions, bottom type `never` for diverging arms, and definite-divergence rules
