# Non-Dao Inventory

This note is explanatory.  The rule it serves is in `AGENTS.md`, under Language
Discipline.  It lists every program and build input in this repository not
written in Dao, and what must exist in Dao before each can go.

The compiler chain is the one exception, and its tiers all stay permanently,
so a machine with no Dao on it can always rebuild the language:

| Tier | Where | Role |
|---|---|---|
| Bootstrap | `compiler/`, in C++ | builds Dao from nothing but a C++ toolchain |
| Self-hosted | `bootstrap/`, in Dao | compiles itself, and still uses LLVM and the C runtime |
| Fully hosted | not yet written | all Dao, with its own runtime and code generation |

Line counts reflect the current branch over `fb5369c`, on 2026-11-25.  The table counts
33,418 lines of authored Dao for comparison.

## Programs

| Stopgap | Language | Files | Lines | Replacement |
|---|---|---|---|---|
| `compiler/` bootstrap tier and its tests | C++ | 114 | 42,739 | nothing: this tier stays permanently |
| `runtime/` allocator, strings, I/O and generators | C | 11 | 1,632 | Dao over `Ptr<T>` and `mode unsafe` in the fully hosted tier, which needs direct system calls from Dao |
| `tools/playground/compiler_service/` | C++ | 20 | 3,300 | a Dao service, which needs sockets and HTTP in the standard library |
| `tools/playground/frontend/` | TypeScript, CSS, HTML | 20 | 1,660 | a Dao browser target, which does not exist |
| `examples/forecast_device/metal_runner.swift` | Swift | 1 | 977 | Dao validating kernel manifests and buffer lifetimes and driving Metal command queues directly |
| `examples/ffi/` | C | 3 | 177 | the C side of an interoperability test, which is foreign by design and goes only if C interoperability does |

## Build and Scripts

| Stopgap | Language | Lines | Replacement |
|---|---|---|---|
| `bootstrap/assemble.sh` | Shell | 113 | real modules, one import boundary at a time, after which assembly goes away |
| `bootstrap/stage2.sh`, `audit_closure.sh`, `validate_ir.sh` | Shell | 1,007 | Dao programs, which need a process API in the standard library |
| `compiler/driver/multifile_build_test.sh`, `tools/playground/dev.sh` | Shell | 194 | the same process API |
| `Taskfile.yml` | YAML for Task | 205 | a build tool written in Dao |
| `CMakeLists.txt` files and `CMakePresets.json` | CMake | 603 | stays with the bootstrap tier, and a Dao build tool serves the other tiers |
| `conanfile.py`, `profiles/` | Python, Conan | 101 | stays with the bootstrap tier |
| `.github/workflows/` | YAML for GitHub Actions | 270 | Bosun orchestrating compiler and crossing validation directly |

## External Dependencies

The bootstrap compiler writes LLVM IR as text, and `clang` compiles and links
it.  LLVM and `clang` are not in this repository.  The fully hosted tier needs
a native code generator and linker in Dao, and that work is not yet planned.

## Data, Not Programs

These hold no logic and have no replacement to build: the grammar in
`spec/grammar/`, goldens in `testdata/`, tool configuration such as
`.clang-format` and `mise.toml`, and lockfiles.
