# Non-Dao Inventory

Status: binding
Scope: every program and build input in this repository not written in Dao

## Rule

Everything in this project is eventually written in Dao.  Code in any other
language is a stopgap, and this table records each one and what must exist
in Dao before it can go.  A change that adds non-Dao code adds or updates its
row in the same change.

The compiler chain is the one exception, and its tiers all stay permanently,
so a machine with no Dao on it can always rebuild the language:

| Tier | Where | Role |
|---|---|---|
| Bootstrap | `compiler/`, in C++ | builds Dao from nothing but a C++ toolchain |
| Self-hosted | `bootstrap/`, in Dao | compiles itself, and still uses LLVM and the C runtime |
| Fully hosted | not yet written | all Dao, with its own runtime and code generation |

Line counts come from `main` at `cb98b0a`, on 2026-11-04.  The table counts
31,697 lines of Dao for comparison.

## Programs

| Stopgap | Language | Files | Lines | What replaces it |
|---|---|---|---|---|
| `compiler/` bootstrap tier and its tests | C++ | 108 | 40,050 | nothing: this tier stays permanently |
| `runtime/` allocator, strings, I/O and generators | C | 11 | 1,499 | Dao over `Ptr<T>` and `mode unsafe` in the fully hosted tier, which needs direct system calls from Dao |
| `tools/playground/compiler_service/` | C++ | 20 | 3,300 | a Dao service, which needs sockets and HTTP in the standard library |
| `tools/playground/frontend/` | TypeScript, CSS, HTML | 20 | 1,660 | a Dao browser target, which does not exist |
| `examples/forecast_device/metal_runner.swift` | Swift | 1 | 403 | Dao lowering kernels and driving a device, which needs a GPU target |
| The Metal kernels inside that runner | Metal | 1 | 9 | one Dao definition compiled for the CPU and the GPU |
| `examples/ffi/` | C | 3 | 177 | the C side of an interoperability test, which is foreign by design and goes only if C interoperability does |

## Build and scripts

| Stopgap | Language | Lines | What replaces it |
|---|---|---|---|
| `bootstrap/assemble.sh` | Shell | 124 | real modules, one import boundary at a time, after which assembly is deleted |
| `bootstrap/stage2.sh`, `audit_closure.sh`, `validate_ir.sh` | Shell | 567 | Dao programs, which need a process API in the standard library |
| `compiler/driver/multifile_build_test.sh`, `tools/playground/dev.sh` | Shell | 194 | the same process API |
| `Taskfile.yml` | YAML for Task | 200 | a build tool written in Dao |
| `CMakeLists.txt` files and `CMakePresets.json` | CMake | 500 | stays with the bootstrap tier, and a Dao build tool serves the other tiers |
| `conanfile.py`, `profiles/` | Python, Conan | 88 | stays with the bootstrap tier |

## External dependencies

The bootstrap compiler writes LLVM IR as text, and `clang` compiles and links
it.  LLVM and `clang` are not in this repository.  The fully hosted tier needs
a native code generator and linker in Dao, and that work is not yet planned.

## Data, not programs

These hold no logic and have no replacement to build: the grammar in
`spec/grammar/`, goldens in `testdata/`, tool configuration such as
`.clang-format` and `mise.toml`, and lockfiles.
