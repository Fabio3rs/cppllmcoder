# Repository Guidelines

## Project Structure & Module Organization
- `src/` holds the C++23 runtime; `main.cpp` is the CLI entry point and other logic belongs in library units such as `lib.cpp`.
- `include/` contains shared headers; `stdafx.hpp` is the precompiled-header anchor for common includes.
- `tests/` houses GoogleTest cases and its own CMake target; keep new coverage close to the code it exercises.
- `docs/` stores design notes and research ideas; favor out-of-source builds in `build/` to keep the tree clean.
- Runtime data described in the README lives under `.cppllmcoder/` (e.g., `brain.db`).
- The SQLite schema uses `created_at` / `updated_at` with millisecond precision (`STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')`) and triggers to refresh `updated_at`; `execution_logs` also stores `duration_ms` to expose slow Lua/tool runs.

## Build, Test, and Development Commands
- `cmake -S . -B build -DENABLE_TESTS=ON -DENABLE_SANITIZERS=ON` — configure with tests and sanitizers enabled.
- `cmake --build build` — compile `cppllmcoder` and the static library target.
- `ctest --test-dir build` — run the test suite; add `CTEST_OUTPUT_ON_FAILURE=1` when debugging.
- Disable sanitizers for profiling or unsupported toolchains with `cmake -S . -B build -DENABLE_SANITIZERS=OFF`.

## Coding Style & Naming Conventions
- C++23, 4-space indentation, no tabs; keep headers lightweight and include `stdafx.hpp` when using the PCH.
- Warnings are treated as errors across compilers; keep builds warning-free.
- Naming: namespaces lower_snake_case, classes/structs PascalCase, functions/methods camelCase, variables lower_snake_case, constants `kPascalCase`.
- Prefer `std::` facilities, `std::expected`-style error handling, and `std::span`-style views as outlined in the README.

## Testing Guidelines
- Framework: GoogleTest; add cases in `tests/*.cpp` with `TEST(SuiteName, CaseName)` naming.
- Cover new behaviors with focused assertions; avoid brittle timing-dependent expectations.
- Run `ctest --test-dir build` before pushing and ensure sanitizer builds pass.

## Commit & Pull Request Guidelines
- Commits: short, imperative subjects (e.g., "Add vector search stub"), scoped to a single concern; current history favors concise summaries.
- Pull requests: describe intent and scope, link related issues, list commands executed (configure/build/tests), and note sanitizer/OS context when relevant. Include output snippets when fixing regressions or test failures.
