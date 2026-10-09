# Windows portability note

Retina 0.2.3 uses C++20, `std::filesystem`, and Boost.Process for the core implementation.
The common Agent/File-System/Git/tool-protocol logic is shared across platforms.

Platform-specific runtime tools are selected through:
- `RETINA_UGREP`
- `RETINA_CLANGD`
- `RETINA_GIT`
- or the common `RETINA_TOOL_DIR` directory.

On Windows, bare executable names and path-qualified executable names can resolve to `.exe` automatically.
The test suite avoids POSIX `/bin/sh` fixtures and uses the test executable itself for portable timeout/output checks.
