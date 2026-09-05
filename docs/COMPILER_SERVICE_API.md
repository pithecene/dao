# Compiler Service API — Dao

This document is explanatory. It defines the initial analysis payloads that
should be shared across the compiler CLI, playground, and LSP.

## Why This Is Explanatory

The repository freezes the existence of shared compiler-owned analysis and
its minimum semantic surface in contracts.

This document stays freestanding because the exact transport, payload
shape, incremental model, and caching strategy may evolve while the
compiler, playground, and LSP harden.

## Goal

One compiler analysis surface should power all interactive tooling.

## Recommended Service Families

The exact transport is intentionally not frozen. The service may begin as an
in-process library API and later gain a JSON, IPC, or daemon boundary.

Recommended request families:
- analyze document
- semantic tokens
- hover
- completion
- definition
- references
- document symbols

## Recommended Semantic Token Payload

Recommended payload shape:
- `kind` — one frozen token category from the tooling contract
- `span` — file-local start/end or line/column span
- `symbol_id` — optional stable symbol identity when available
- `type_id` — optional type identity when classification is type-driven
- `modifiers` — optional flags such as declaration, readonly, or generated

## Recommended Hover Payload

Hover should be able to report:
- symbol kind
- display type
- declaration span
- optional short doc text
- optional module path

## Recommended Completion Payload

Completion entries should report:
- label
- insert text
- symbol kind
- signature summary where applicable
- optional doc snippet
- optional origin module

## Recommended Definition / Reference Payload

Definition and reference responses should report:
- stable symbol identity
- target file path
- target span
- optional symbol kind

## Recommended Document Symbol Payload

Document symbols should report:
- name
- symbol kind
- declaration span
- selection span
- parent-child nesting where applicable

## Concrete Surface

The recommendations above are realised, for the playground transport, by
the tables in `compiler/analysis/tooling_surface.h`: token kinds, lexical
categories, diagnostic severities, payload shapes, and routes.  That
header is the machine-checked definition — the frontend's TypeScript is
generated from it and `ctest` verifies the service's replies against it
(see `docs/PLAYGROUND_ARCHITECTURE.md`, "Sync Discipline").  This
document explains intent; the header says what is served today.

## Principle

The playground and LSP should consume these payloads without needing their
own language logic.
