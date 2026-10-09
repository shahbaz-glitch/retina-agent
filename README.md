# Retina 0.2.3 — C++20 Source Package

This is the buildable developer/source project for Retina 0.2.3.

Contents:
- `src/` — Retina C++20 implementation
- `include/` — public headers
- `tests/` — comprehensive integration/adversarial test suite
- `CMakeLists.txt` — C++20 build configuration
- `VERSION.txt` — version/source-status note

The runtime release is intentionally separate and does not contain this source or test tree.

Build:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
    ctest --test-dir build --output-on-failure

Runtime tools are selected through `RETINA_UGREP`, `RETINA_CLANGD`, and `RETINA_GIT`; otherwise the normal PATH is used.

## Runtime tool selection
Retina resolves `ugrep`, `clangd`, and `git` through `RETINA_UGREP`, `RETINA_CLANGD`, and `RETINA_GIT` when set. `RETINA_TOOL_DIR` may be used as a common directory for bundled tools.
