# Building Dao

## Prerequisites

- [mise](https://mise.jdx.dev/) (task runner and env manager)
- Conan 2.x (C++ dependency manager)
- CMake 3.30+
- Clang 21+

## Quick start

```sh
task build        # install deps, configure, build
task test         # run tests
task playground   # start playground at http://localhost:8090
```

## Driver usage

```
daoc <command> <root.dao> [--module-root DIR]... [--stdlib-root DIR]
daoc <command> --source a.dao [--source b.dao]... [--entry a::b]
daoc build <inputs as above> [link-inputs...]
```

Commands: `lex`, `parse`, `ast` (single file, before module structure
exists), `tokens`, `resolve`, `check`, `hir`, `mir`, `llvm-ir`, `build`.

- **Root-file mode** — the root's imports drive discovery: `import
  a::b::c` is satisfied by the first `<root>/a/b/c.dao` over the root
  file's directory, each `--module-root DIR` in order, and the stdlib
  root; the located file must declare `a::b::c`.  The root's module is
  the entry module.
- **Explicit mode** — `--source` names every file; nothing is searched
  and an import outside the set is an error.  The entry module is
  `--entry a::b`, or the one module declaring `fn main`.
- `--stdlib-root DIR` replaces the default prelude source
  (`<repo>/stdlib`); `stdlib/core` and `stdlib/io` under it form the
  prelude group.

File ids, diagnostics order, and every output are independent of the
order files are given (`CONTRACT_MODULE_SYSTEM.md` §9).

## Build parallelism

Build parallelism is centrally managed through a single variable:
**`DAO_BUILD_JOBS`**.

### How it works

`mise.toml` defines `DAO_BUILD_JOBS` using a tera template that
computes `min(4, nproc)`:

```toml
DAO_BUILD_JOBS = "{% set cpus = num_cpus() | int %}{% if cpus > 4 %}4{% else %}{{ cpus }}{% endif %}"
```

This variable then propagates to every build tool:

| Variable                      | Set from              | Caps                          |
|-------------------------------|-----------------------|-------------------------------|
| `DAO_BUILD_JOBS`              | mise template         | Central value, all others key off this |
| `CMAKE_BUILD_PARALLEL_LEVEL`  | `DAO_BUILD_JOBS`      | `cmake --build`, any generator |
| `MAKEFLAGS`                   | `-j$DAO_BUILD_JOBS`   | Raw `make` invocations        |
| `tools.build:jobs` (Conan)    | `$DAO_BUILD_JOBS`     | Conan from-source dependency builds |

The Conan profile reads `DAO_BUILD_JOBS` via Jinja:

```ini
tools.build:jobs={{ os.getenv("DAO_BUILD_JOBS", default="4") }}
```

### Overriding

**Temporarily** (single command):

```sh
DAO_BUILD_JOBS=8 CMAKE_BUILD_PARALLEL_LEVEL=8 task build
```

**Persistently** (local machine only):

Create `mise.local.toml` (gitignored):

```toml
[env]
DAO_BUILD_JOBS = "8"
CMAKE_BUILD_PARALLEL_LEVEL = "8"
MAKEFLAGS = "-j8"
```

This overrides the repo-tracked `mise.toml` values. The file is
gitignored so it never affects other contributors.

### Why not just set it in the Taskfile?

`cmake --build -jN` only caps that one invocation. It doesn't cap:

- Conan from-source builds (e.g. building LLVM, which defaults to
  `nproc` parallel jobs)
- Raw `make` invocations outside the Taskfile
- IDE-triggered builds that use CMake presets directly

The env-var approach caps everything that runs inside a mise-activated
shell, regardless of how it was invoked.
