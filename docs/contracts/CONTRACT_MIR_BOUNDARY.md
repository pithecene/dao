# CONTRACT_MIR_BOUNDARY

Status: normative
Scope: Dao MIR boundary
Authority: compiler architecture boundary

## 1. Purpose

This contract freezes the role of MIR in the Dao compiler.

MIR is the typed, target-independent operational IR between HIR and
backend lowering.

## 2. MIR must be

- typed
- target-independent
- operational
- explicit in control flow
- explicit in evaluation order

## 3. MIR must include

- functions
- basic blocks
- terminators
- typed values/instructions

## 4. MIR must preserve Dao-relevant execution semantics

MIR must preserve, by explicit construct or annotation, the semantics
of:

- mode regions
- resource regions

These must not disappear before backend lowering decisions are made.

## 5. MIR lowering policy

MIR may lower:

- pipe expressions into explicit operational form
- structured control flow into branches and blocks
- nested expressions into explicit temporaries

MIR must lower `and` and `or` into branches, so that the right
operand's instructions lie only on the path that evaluates it
(`CONTRACT_TYPECHECKING_BASELINE.md`, "Logical operators"). MIR holds
no binary instruction for either operator.

## 6. MIR must not contain

- parser-only syntax structure
- unresolved names
- AST syntax types
- LLVM-specific values or types
- target ABI details

## 7. Boundary rule

HIR feeds MIR.
MIR feeds backend lowering.

MIR must not redo:

- parsing
- resolution
- type checking
