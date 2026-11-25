# Third-Party Notices

Dao itself uses the Apache License 2.0 with LLVM Exception
(`Apache-2.0 WITH LLVM-exception`) in `LICENSE`.

Release distributions preserve notices for third-party dependencies separately
from `LICENSE`. This repository neither vendors third-party source trees nor
relicenses external components.

## Native Build and Compiler Dependencies (`conanfile.py`)

| Package | Version | SPDX License | Upstream |
|---|---|---|---|
| `llvm-core` | `19.1.7` | `Apache-2.0 WITH LLVM-exception` | LLVM Project (`https://llvm.org`) |
| `boost-ext-ut` | `2.3.1` | `BSL-1.0` | `boost-ext/ut` (`https://github.com/boost-ext/ut`) |
| `cpp-httplib` | `0.18.3` | `MIT` | Yuji Hirose (`https://github.com/yhirose/cpp-httplib`) |
| `nlohmann_json` | `3.11.3` | `MIT` | Niels Lohmann (`https://github.com/nlohmann/json`) |

When building binary distributions via Conan and CMake, retain each linked
dependency's license file from the Conan package `licenses/` directory alongside
this file.

## Playground Frontend Dependencies (`tools/playground/frontend/package.json`)

| Package | SPDX License | Upstream |
|---|---|---|
| `codemirror`, `@codemirror/*` | `MIT` | Marijn Haverbeke and contributors (`https://codemirror.net`) |
| `typescript` | `Apache-2.0` | Microsoft Corporation (`https://www.typescriptlang.org`) |
| `vite` | `MIT` | Evan You and Vite contributors (`https://vite.dev`) |
