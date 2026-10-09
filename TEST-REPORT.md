# Retina 0.2.3 — Linux x86_64 Production Validation

## Build
- Release build: PASS
- C++ standard: C++20
- Primary compiler: GCC 14.2.0
- Independent sanitizer build: Clang 17 build was attempted; the sanitizer configuration hit a child-process/toolchain interaction before completing the full suite and is not claimed as PASS.

## Functional validation
- Full integration/adversarial suite: PASS
- Scenario groups: 16/16 PASS
- Tool schema and malformed request validation: PASS
- File create/read/edit/rewrite: PASS
- Line slicing and size boundary: PASS
- Move/rename/delete: PASS
- Unicode paths: PASS
- Workspace traversal and absolute-outside rejection: PASS
- Symlink escape and safe symlink deletion: PASS
- Symlink-parent write escape: PASS
- ugrep recursive literal/regex/no-match search: PASS
- ugrep leading-dash pattern safety: PASS
- C++20 CMake configure/build/run: PASS
- clangd good/bad diagnostics: PASS
- Process timeout + descendant termination: PASS
- Process stdout/stderr caps: PASS
- Direct argv/no-shell argument isolation: PASS
- Missing executable and malformed args: PASS
- Local Git workflow: PASS
- Git repository-boundary override rejection: PASS
- Inherited GIT_DIR isolation: PASS
- Remote Git clone/fetch/pull/push/remote: PASS against an isolated local bare remote
- JSONL Client launch/schema/call/stop: PASS
- 48-source-file synthetic C++20 project: PASS
- 6 isolated Agents in parallel: PASS

## Runtime validation
The shipped runtime launcher was tested with the freshly rebuilt Retina agent plus bundled ugrep/clangd/Git. The final runtime-only tree excludes source, tests, CMake and build intermediates.

## Tool versions in this validated runtime
- ugrep 7.8.5
- clang/clangd 17.0.0
- Git 2.47.3

## Latest-version note
As of the external verification performed for this release, LLVM 23.1.2, Git 2.56.0 and Boost 1.92.0 were verified as newer upstream releases. The sandbox cannot resolve outbound hosts, so those newer binaries could not be downloaded and substituted into this runtime. The source/CI design can target those newer versions on GitHub Actions.
