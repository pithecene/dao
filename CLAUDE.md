# Repository Constitution of Dao

## 1. Constitutional Order of Authority

1. This file (`CLAUDE.md`)
2. `AGENTS.md`
3. `docs/contracts/CONTRACT_*.md`
4. `docs/ARCH_INDEX.md`
5. explanatory docs in `docs/` such as roadmap, architecture notes, and tooling design notes

## 2. Identity

Dao is a programming language project focused on explicit semantics,
predictable syntax, scoped execution modes, custom allocation domains,
and numerically serious workloads.

This repository is governance-first and spec-first. Until implementation
exists, contracts and structure are the authoritative source of intent.

## 3. Role of Supporting Documents

### `AGENTS.md`

Provides behavioral expectations for Claude/Codex and contributors.
It does not override this constitution.

### `docs/contracts/`

Normative contracts. These define what must remain true about:
- repository layout
- syntax surface
- execution contexts
- compiler phase boundaries
- bootstrap and interop posture
- tooling boundaries and baseline semantic tooling surface

If code disagrees with a contract, the contract wins until explicitly
updated.

### `docs/ARCH_INDEX.md`

Navigation only. It answers where things live, not what behavior is
normative.

### Other `docs/*.md`

Explanatory planning material. These may sequence work or capture
preferred architecture, but they do not override contracts.

Use this rule when classifying documentation:
- put a decision in a contract if violating it would mean the repository,
  language, or tooling surface is no longer recognizably Dao as currently
  defined
- keep a decision freestanding if it is likely to evolve through
  implementation learning, transport changes, UI iteration, or staging
  strategy

## 4. Structural Invariants

Required top-level directories:
- `bootstrap/`
- `compiler/`
- `runtime/`
- `stdlib/`
- `spec/`
- `docs/`
- `examples/`
- `testdata/`
- `tools/`
- `ai/`

Required root files:
- `README.md`
- `CLAUDE.md`
- `AGENTS.md`
- `.grove.yaml`

Forbidden:
- orphan top-level directories without `docs/ARCH_INDEX.md` coverage
- duplicate responsibility across top-level directories
- symlinks in any git-tracked content
- hidden global state introduced by tooling or generated scripts

## 5. Repository Development Rules

- Treat syntax churn as architecture work, not cosmetic cleanup.
- Do not change frozen syntax decisions without updating the relevant
  `CONTRACT_*.md` first.
- Compiler architecture must preserve a clean split between frontend,
  target-agnostic IR layers, target-specific backends, and runtime.
- Initial backend planning assumes LLVM, but frontend and IR must remain
  independent of LLVM details.
- Initial host implementation language is C++.
- The initial compiler must preserve an explicit phase split of lexer → parser → AST → resolve → typecheck → HIR → MIR → LLVM backend → driver, even if some directories remain placeholders at the start.
- Interactive tooling is a first-class concern. The compiler must preserve enough structure for semantic highlighting, diagnostics, hover, completion, and navigation.
- The playground is a first-class development surface and long-term web-IDE north star. It must consume compiler analysis rather than bespoke language logic.
- Prefer narrow, explicit contracts over aspirational prose.
- Do not introduce build tooling, package managers, or dependency ecosystems speculatively.

## 6. Output Requirements for AI Sessions

- Prefer minimal diffs.
- Do not rewrite complete documents when a scoped patch is sufficient.
- Preserve authority ordering and contract naming.
- When changing structure, update `docs/ARCH_INDEX.md` in the same diff.
