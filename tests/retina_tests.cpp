#include "retina/agent.hpp"
#include "retina/client.hpp"

#include <boost/json.hpp>
#include <boost/process.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace json = boost::json;

namespace {

int failures = 0;

void require(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool ok(const json::object& response) {
    const auto* v = response.if_contains("ok");
    return v && v->is_bool() && v->as_bool();
}

const json::value* result_value(const json::object& response, const char* key) {
    const auto* result = response.if_contains("result");
    if (!result || !result->is_object()) return nullptr;
    return result->as_object().if_contains(key);
}

std::string result_string(const json::object& response, const char* key) {
    const auto* v = result_value(response, key);
    return (v && v->is_string()) ? std::string(v->as_string().c_str()) : std::string{};
}

bool result_bool(const json::object& response, const char* key) {
    const auto* v = result_value(response, key);
    return v && v->is_bool() && v->as_bool();
}

std::int64_t result_int(const json::object& response, const char* key) {
    const auto* v = result_value(response, key);
    return (v && v->is_int64()) ? v->as_int64() : -999;
}

json::object call(retina::Agent& agent, const std::string& tool, json::object args = {}) {
    return agent.dispatch(json::object{{"tool", tool}, {"arguments", std::move(args)}});
}

void write_raw(const fs::path& p, const std::string& data) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.flush();
    require(static_cast<bool>(out), "raw fixture write failed");
}

std::string env_or(const char* name, const std::string& fallback) {
    if (const char* v = std::getenv(name); v && *v) return v;
    return fallback;
}

void set_env_var(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void unset_env_var(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

void test_helper_mode(const fs::path& executable, int argc, char** argv) {
    (void)executable;
    if (argc < 3) return;
    const std::string mode = argv[1];
    const int amount = std::max(0, std::atoi(argv[2]));
    if (mode == "--sleep-ms") {
        std::this_thread::sleep_for(std::chrono::milliseconds(amount));
        std::exit(0);
    }
    if (mode == "--emit-stdout") {
        const std::string chunk(64 * 1024, 'X');
        int remaining = amount;
        while (remaining > 0) {
            const int n = std::min<int>(remaining, static_cast<int>(chunk.size()));
            std::cout.write(chunk.data(), n);
            remaining -= n;
        }
        std::cout.flush();
        std::exit(0);
    }
    if (mode == "--emit-stderr") {
        const std::string chunk(64 * 1024, 'E');
        int remaining = amount;
        while (remaining > 0) {
            const int n = std::min<int>(remaining, static_cast<int>(chunk.size()));
            std::cerr.write(chunk.data(), n);
            remaining -= n;
        }
        std::cerr.flush();
        std::exit(0);
    }
    if (mode == "--spawn-child-sleep") {
        namespace bp = boost::process;
        bp::group group;
        bp::child child(argv[0], std::vector<std::string>{"--sleep-ms", std::to_string(amount)}, group);
        child.wait();
        std::exit(0);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && (std::string(argv[1]) == "--sleep-ms" ||
                      std::string(argv[1]) == "--emit-stdout" ||
                      std::string(argv[1]) == "--emit-stderr" ||
                      std::string(argv[1]) == "--spawn-child-sleep")) {
        test_helper_mode(fs::absolute(argv[0]), argc, argv);
        return 0;
    }
    if (argc < 2) {
        std::cerr << "usage: retina-tests <agent-executable>\n";
        return 2;
    }

    const fs::path agent_exe = fs::absolute(argv[1]);
    const fs::path root = fs::temp_directory_path() / ("retina-0.2.3-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);

    retina::Agent agent(root);

    std::cerr << "[Retina test 1/16] start\n";
    // 1) Tool schema and request validation.
    auto schema = call(agent, "tools");
    require(ok(schema), "tool schema request failed");
    const auto* schema_result = schema.if_contains("result");
    require(schema_result && schema_result->is_array() && schema_result->as_array().size() >= 8, "tool schema incomplete");
    require(!call(agent, "missing_tool").if_contains("ok") || !ok(call(agent, "missing_tool")), "unknown tool accepted");
    require(!ok(agent.dispatch(json::object{{"tool", 42}})), "non-string tool accepted");
    require(!ok(agent.dispatch(json::object{{"tool", "read_file"}, {"arguments", 42}})), "non-object arguments accepted");

    std::cerr << "[Retina test 2/16] start\n";
    // 2) File create/read/line slicing/repeated writes.
    auto w = call(agent, "write_file", json::object{{"path", "src/math.cpp"}, {"content", "#include <iostream>\nint add(int a, int b) { return a + b; }\n"}});
    require(ok(w) && result_bool(w, "written"), "write_file failed");
    auto r = call(agent, "read_file", json::object{{"path", "src/math.cpp"}});
    require(ok(r) && result_string(r, "content").find("return a + b") != std::string::npos, "read_file content mismatch");
    auto slice = call(agent, "read_file", json::object{{"path", "src/math.cpp"}, {"start_line", 2}, {"end_line", 2}});
    require(ok(slice) && result_string(slice, "content").find("int add") != std::string::npos, "line slice failed");
    for (int i = 0; i < 30; ++i) {
        auto rr = call(agent, "write_file", json::object{{"path", "src/repeat.txt"}, {"content", "iteration=" + std::to_string(i) + "\n"}});
        require(ok(rr), "repeated write failed");
    }

    std::cerr << "[Retina test 3/16] start\n";
    // 3) Traversal, outside path, root protection.
    require(!ok(call(agent, "read_file", json::object{{"path", "../outside.txt"}})), "workspace traversal accepted");
    require(!ok(call(agent, "delete_path", json::object{{"path", "."}})), "workspace root deletion accepted");
    require(!ok(call(agent, "write_file", json::object{{"path", "../escape.txt"}, {"content", "x"}})), "write traversal accepted");

    std::cerr << "[Retina test 4/16] start\n";
    // 4) Unicode path.
    const std::string unicode_path = "src/naïve_文件_🙂.txt";
    auto uw = call(agent, "write_file", json::object{{"path", unicode_path}, {"content", "unicode-ok"}});
    auto ur = call(agent, "read_file", json::object{{"path", unicode_path}});
    require(ok(uw) && ok(ur) && result_string(ur, "content") == "unicode-ok", "Unicode filename handling failed");

    std::cerr << "[Retina test 5/16] start\n";
    // 5) Directory target and symlink safety.
    fs::create_directories(root / "adir");
    require(!ok(call(agent, "write_file", json::object{{"path", "adir"}, {"content", "should-fail"}})), "directory overwrite accepted");

    const fs::path outside = fs::temp_directory_path() / ("retina-outside-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(outside);
    write_raw(outside / "secret.txt", "secret");
    std::error_code ec;
    fs::create_symlink(outside / "secret.txt", root / "escape-link.txt", ec);
    require(!ec, "failed to create escape symlink fixture");
    require(!ok(call(agent, "read_file", json::object{{"path", "escape-link.txt"}})), "symlink escape read accepted");
    auto sd = call(agent, "delete_path", json::object{{"path", "escape-link.txt"}});
    require(ok(sd) && fs::exists(outside / "secret.txt"), "safe symlink deletion failed");

    fs::create_symlink(root / "src/math.cpp", root / "safe-link.cpp", ec);
    require(!ec, "failed to create safe symlink fixture");
    require(!ok(call(agent, "write_file", json::object{{"path", "safe-link.cpp"}, {"content", "no-follow"}})), "write through symlink accepted");
    auto safe_del = call(agent, "delete_path", json::object{{"path", "safe-link.cpp"}});
    require(ok(safe_del) && fs::exists(root / "src/math.cpp"), "safe symlink deletion removed target");

    std::error_code parent_symlink_ec;
    fs::create_directory_symlink(outside, root / "linked-dir", parent_symlink_ec);
    if (!parent_symlink_ec) {
        require(!ok(call(agent, "write_file", json::object{{"path", "linked-dir/escape.cpp"}, {"content", "escape"}})), "write through symlinked parent accepted");
    }

    std::cerr << "[Retina test 6/16] start\n";
    // 6) Read limits: exactly 4 MiB succeeds; 4 MiB + 1 rejected.
    write_raw(root / "exact.bin", std::string(4ULL * 1024ULL * 1024ULL, 'A'));
    auto exact = call(agent, "read_file", json::object{{"path", "exact.bin"}});
    require(ok(exact) && result_int(exact, "bytes") == 4LL * 1024LL * 1024LL, "4 MiB boundary failed");
    write_raw(root / "large.bin", std::string(4ULL * 1024ULL * 1024ULL + 1ULL, 'B'));
    require(!ok(call(agent, "read_file", json::object{{"path", "large.bin"}})), "over-limit read accepted");

    std::cerr << "[Retina test 7/16] start\n";
    // 7) ugrep literal/regex/no-match.
    auto search = call(agent, "search_code", json::object{{"query", "int add"}, {"path", "src"}});
    require(ok(search) && result_string(search, "backend") == "ugrep" && result_bool(search, "matched"), "ugrep literal search failed");
    auto regex = call(agent, "search_code", json::object{{"query", "int[[:space:]]+add"}, {"path", "src"}, {"mode", "regex"}});
    require(ok(regex) && result_string(regex, "backend") == "ugrep", "ugrep regex search failed");
    auto none = call(agent, "search_code", json::object{{"query", "definitely_missing_symbol_987"}, {"path", "src"}});
    require(ok(none) && !result_bool(none, "matched") && result_int(none, "exit_code") == 1, "ugrep no-match semantics failed");
    write_raw(root / "src/leading-dash.txt", "-n is a literal pattern\n");
    auto leading_dash = call(agent, "search_code", json::object{{"query", "-n"}, {"path", "src"}});
    require(ok(leading_dash) && result_bool(leading_dash, "matched"), "ugrep leading-dash pattern handling failed");

    std::cerr << "[Retina test 8/16] start\n";
    // 8) C++20 semantic analysis and build/run/fix.
    write_raw(root / "CMakeLists.txt",
              "cmake_minimum_required(VERSION 3.20)\nproject(RetinaFixture LANGUAGES CXX)\n"
              "set(CMAKE_CXX_STANDARD 20)\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
              "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\nadd_executable(app src/main.cpp)\n");
    write_raw(root / "src/main.cpp",
              "#include <iostream>\nint main(){ std::cout << (7 + 5) << '\\n'; return 0; }\n");
    auto cfg = call(agent, "run_process", json::object{{"program", "cmake"}, {"args", json::array{"-S", ".", "-B", "build", "-DCMAKE_BUILD_TYPE=Debug"}}, {"timeout_ms", 120000}});
    require(ok(cfg) && result_bool(cfg, "success"), "CMake configure failed");
    auto analysis = call(agent, "analyze_cpp", json::object{{"path", "src/main.cpp"}});
    require(ok(analysis) && result_string(analysis, "backend") == "clangd" && result_bool(analysis, "success"), "clangd good analysis failed");
    auto build = call(agent, "run_process", json::object{{"program", "cmake"}, {"args", json::array{"--build", "build", "--parallel"}}, {"timeout_ms", 120000}});
    require(ok(build) && result_bool(build, "success"), "CMake build failed");
    auto run = call(agent, "run_process", json::object{{"program", "./build/app"}, {"args", json::array{}}, {"timeout_ms", 10000}});
    require(ok(run) && result_bool(run, "success") && result_string(run, "stdout").find("12") != std::string::npos, "built C++20 executable run failed");
    write_raw(root / "src/bad.cpp", "int broken( { return 0; }\n");
    auto bad = call(agent, "analyze_cpp", json::object{{"path", "src/bad.cpp"}, {"timeout_ms", 30000}});
    require(ok(bad) && !result_bool(bad, "success"), "clangd syntax error not reported");
    require(ok(call(agent, "delete_path", json::object{{"path", "src/bad.cpp"}})), "bad file cleanup failed");

    std::cerr << "[Retina test 9/16] start\n";
    // 9) Process timeout + descendant kill marker.
    const fs::path marker = root / "leaked-marker.txt";
    const std::string cmd = "sleep 2; echo leaked > '" + marker.string() + "'";
    auto timeout = call(agent, "run_process", json::object{{"program", "/bin/sh"}, {"args", json::array{"-c", cmd}}, {"timeout_ms", 250}});
    require(ok(timeout) && result_bool(timeout, "timed_out"), "process timeout was not enforced");
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    require(!fs::exists(marker), "timed-out descendant survived process group termination");

    std::cerr << "[Retina test 10/16] start\n";
    // 10) Output truncation flags.
    auto huge_out = call(agent, "run_process", json::object{{"program", "/bin/sh"}, {"args", json::array{"-c", "yes X | head -c 2000000"}}, {"timeout_ms", 10000}});
    require(ok(huge_out) && result_bool(huge_out, "stdout_truncated") && result_string(huge_out, "stdout").size() <= 1024ULL * 1024ULL, "stdout truncation failed");
    auto huge_err = call(agent, "run_process", json::object{{"program", "/bin/sh"}, {"args", json::array{"-c", "yes E 1>&2 | head -c 2000000"}}, {"timeout_ms", 10000}});
    require(ok(huge_err) && result_bool(huge_err, "stderr_truncated") && result_string(huge_err, "stderr").size() <= 1024ULL * 1024ULL, "stderr truncation failed");
    require(!ok(call(agent, "run_process", json::object{{"program", "definitely-not-real"}, {"args", json::array{}}})), "missing executable accepted");
    require(!ok(call(agent, "run_process", json::object{{"program", "cmake"}, {"args", json::object{}}})), "malformed process args accepted");

    // 10b) Process arguments are passed without shell interpretation.
    const auto arg_probe = call(agent, "run_process", json::object{{"program", "cmake"}, {"args", json::array{"-E", "echo", ";touch retina-not-created;"}}});
    require(ok(arg_probe) && result_string(arg_probe, "stdout") == ";touch retina-not-created;\n", "run_process shell-argument isolation failed");

    std::cerr << "[Retina test 11/16] start\n";
    // 11) Move/rename/delete.
    auto mvw = call(agent, "write_file", json::object{{"path", "src/move_me.hpp"}, {"content", "#pragma once\n"}});
    auto mv = call(agent, "move_path", json::object{{"from", "src/move_me.hpp"}, {"to", "include/moved.hpp"}});
    require(ok(mvw) && ok(mv) && fs::exists(root / "include/moved.hpp"), "move/rename failed");
    auto del = call(agent, "delete_path", json::object{{"path", "include/moved.hpp"}});
    require(ok(del) && !fs::exists(root / "include/moved.hpp"), "delete_path failed");

    std::cerr << "[Retina test 12/16] start\n";
    // 12) Local Git.
    auto gi = call(agent, "git", json::object{{"action", "init"}});
    require(ok(gi) && result_bool(gi, "success"), "git init failed");
    require(ok(call(agent, "run_process", json::object{{"program", "git"}, {"args", json::array{"config", "user.name", "Retina Tests"}}})), "git config name failed");
    require(ok(call(agent, "run_process", json::object{{"program", "git"}, {"args", json::array{"config", "user.email", "retina@example.invalid"}}})), "git config email failed");
    auto gs = call(agent, "git", json::object{{"action", "status"}});
    require(ok(gs) && result_bool(gs, "success"), "git status failed");
    auto ga = call(agent, "git", json::object{{"action", "add"}, {"args", json::array{"CMakeLists.txt", "src", "include"}}});
    require(ok(ga) && result_bool(ga, "success"), "git add failed");
    auto gc = call(agent, "git", json::object{{"action", "commit"}, {"args", json::array{"-m", "initial"}}});
    require(ok(gc) && result_bool(gc, "success"), "git commit failed");
    write_raw(root / "src/math.cpp", "int changed = 1;\n");
    auto gd = call(agent, "git", json::object{{"action", "diff"}});
    require(ok(gd) && result_string(gd, "stdout").find("math.cpp") != std::string::npos, "git diff failed");
    require(!ok(call(agent, "git", json::object{{"action", "not_supported"}})), "unsupported git action accepted");
    require(!ok(call(agent, "git", json::object{{"action", "status"}, {"args", json::array{"-C", "../"}}})), "git -C repository escape accepted");
    require(!ok(call(agent, "git", json::object{{"action", "status"}, {"args", json::array{"--git-dir", "../outside.git"}}})), "git --git-dir repository escape accepted");
    const std::string old_git_dir = env_or("GIT_DIR", "");
    set_env_var("GIT_DIR", (root.parent_path() / "outside.git").string());
    auto env_redirect = call(agent, "git", json::object{{"action", "status"}});
    require(ok(env_redirect), "inherited GIT_DIR broke workspace git isolation");
    if (old_git_dir.empty()) unset_env_var("GIT_DIR"); else set_env_var("GIT_DIR", old_git_dir);

    std::cerr << "[Retina test 13/16] start\n";
    // 13) Remote Git round-trip through the high-level Agent tool.
    const fs::path remote_root = root.parent_path() / (root.filename().string() + "-remote");
    fs::create_directories(remote_root);
    fs::path seed = remote_root / "seed";
    fs::path bare = remote_root / "remote.git";
    fs::create_directories(seed);
    std::string git = env_or("RETINA_GIT", "git");
    auto sh = [&](const fs::path& dir, const std::vector<std::string>& args) {
        std::vector<std::string> full = {"-C", dir.string()};
        full.insert(full.end(), args.begin(), args.end());
        return std::system(([&](){ std::string cmd2 = git; for (const auto& a : full) cmd2 += " \"" + a + "\""; return cmd2; })().c_str()) == 0;
    };
    std::error_code ignore;
    fs::create_directories(bare);
    require(sh(seed, {"init", "-b", "main"}), "remote seed init failed");
    require(sh(seed, {"config", "user.name", "Retina Test"}), "remote seed user failed");
    require(sh(seed, {"config", "user.email", "retina@example.invalid"}), "remote seed email failed");
    write_raw(seed / "app.txt", "v1\n");
    require(sh(seed, {"add", "app.txt"}), "remote seed add failed");
    require(sh(seed, {"commit", "-m", "v1"}), "remote seed commit failed");
    require(sh(remote_root, {"init", "--bare", bare.string()}), "bare remote init failed");
    require(sh(seed, {"remote", "add", "origin", "file://" + bare.string()}), "remote add failed");
    require(sh(seed, {"push", "-u", "origin", "main"}), "seed push failed");

    auto clone = call(agent, "git", json::object{{"action", "clone"}, {"url", "file://" + bare.string()}, {"destination", "cloned"}, {"branch", "main"}, {"depth", 1}});
    require(ok(clone) && result_bool(clone, "success") && fs::exists(root / "cloned/app.txt"), "Agent remote clone failed");
    auto remotes = call(agent, "git", json::object{{"action", "remote"}, {"mode", "list"}, {"path", "cloned"}});
    require(ok(remotes) && result_string(remotes, "stdout").find("origin") != std::string::npos, "Agent remote list failed");
    require(sh(seed, {"config", "user.name", "Retina Test"}), "seed config refresh failed");
    write_raw(seed / "app.txt", "v1\nv2\n");
    require(sh(seed, {"add", "app.txt"}), "seed v2 add failed");
    require(sh(seed, {"commit", "-m", "v2"}), "seed v2 commit failed");
    require(sh(seed, {"push"}), "seed v2 push failed");
    auto fetch = call(agent, "git", json::object{{"action", "fetch"}, {"path", "cloned"}});
    require(ok(fetch) && result_bool(fetch, "success"), "Agent remote fetch failed");
    auto pull = call(agent, "git", json::object{{"action", "pull"}, {"path", "cloned"}, {"remote", "origin"}, {"branch", "main"}});
    require(ok(pull) && result_bool(pull, "success"), "Agent remote pull failed");
    write_raw(root / "cloned/app.txt", "v1\nv2\nv3\n");
    require(sh(root / "cloned", {"add", "app.txt"}), "clone v3 add failed");
    require(sh(root / "cloned", {"config", "user.name", "Retina Test"}), "clone v3 user failed");
    require(sh(root / "cloned", {"config", "user.email", "retina@example.invalid"}), "clone v3 email failed");
    require(sh(root / "cloned", {"commit", "-m", "v3"}), "clone v3 commit failed");
    auto push = call(agent, "git", json::object{{"action", "push"}, {"path", "cloned"}, {"remote", "origin"}, {"branch", "main"}});
    require(ok(push) && result_bool(push, "success"), "Agent remote push failed");
    auto url = call(agent, "git", json::object{{"action", "remote"}, {"mode", "get_url"}, {"path", "cloned"}, {"name", "origin"}});
    require(ok(url) && result_string(url, "stdout").find("file://") != std::string::npos, "Agent remote URL read failed");
    require(!ok(call(agent, "git", json::object{{"action", "clone"}, {"url", "file://" + bare.string()}, {"destination", "../escape"}})), "remote clone path escape accepted");
    require(!ok(call(agent, "git", json::object{{"action", "push"}, {"path", "cloned"}, {"force_with_lease", "yes"}})), "malformed remote boolean accepted");

    std::cerr << "[Retina test 14/16] start\n";
    // 14) Client wire protocol.
    auto client = retina::Client::launch(agent_exe, root);
    require(client.alive(), "Client failed to launch agent");
    require(client.tools().size() >= 8, "Client tool schema too small");
    auto remote_read = client.call("read_file", json::object{{"path", "src/math.cpp"}});
    require(ok(remote_read), "Client remote read failed");
    client.stop();
    require(!client.alive(), "Client stop failed");

    std::cerr << "[Retina test 15/16] start\n";
    // 15) Larger synthetic C++ project: build/search/analyze repeatedly.
    const fs::path large = root / "large_project";
    fs::create_directories(large / "src");
    std::string main_cpp = "#include <iostream>\n";
    for (int i = 0; i < 48; ++i) {
        write_raw(large / ("src/module_" + std::to_string(i) + ".cpp"),
                  "namespace fixture { int module_" + std::to_string(i) + "(int v){ return v + " + std::to_string(i) + "; } }\n");
        main_cpp += "namespace fixture { int module_" + std::to_string(i) + "(int); }\n";
    }
    main_cpp += "int main(){ int sum=0;";
    for (int i = 0; i < 48; ++i) main_cpp += "sum += fixture::module_" + std::to_string(i) + "(0);";
    main_cpp += "std::cout << sum << '\\n'; return sum == 1128 ? 0 : 1;}\n";
    write_raw(large / "src/main.cpp", main_cpp);
    write_raw(large / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\nproject(Large LANGUAGES CXX)\nset(CMAKE_CXX_STANDARD 20)\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\nfile(GLOB SRC CONFIGURE_DEPENDS src/*.cpp)\nadd_executable(large_app ${SRC})\n");
    auto large_search = call(agent, "search_code", json::object{{"query", "module_47"}, {"path", "large_project"}});
    require(ok(large_search) && result_bool(large_search, "matched"), "large ugrep search failed");
    auto large_cfg = call(agent, "run_process", json::object{{"program", "cmake"}, {"args", json::array{"-S", "large_project", "-B", "large_build", "-DCMAKE_BUILD_TYPE=Release"}}, {"timeout_ms", 120000}});
    require(ok(large_cfg) && result_bool(large_cfg, "success"), "large project configure failed");
    auto large_build = call(agent, "run_process", json::object{{"program", "cmake"}, {"args", json::array{"--build", "large_build", "--parallel"}}, {"timeout_ms", 120000}});
    require(ok(large_build) && result_bool(large_build, "success"), "large project build failed");
    auto large_run = call(agent, "run_process", json::object{{"program", "./large_build/large_app"}, {"args", json::array{}}, {"timeout_ms", 10000}});
    require(ok(large_run) && result_bool(large_run, "success") && result_string(large_run, "stdout").find("1128") != std::string::npos, "large project run failed");

    std::cerr << "[Retina test 16/16] start\n";
    // 16) Concurrent isolated Agents.
    std::vector<std::thread> workers;
    std::vector<bool> worker_ok(6, false);
    for (int i = 0; i < 6; ++i) {
        workers.emplace_back([&, i] {
            const fs::path ws = root / ("parallel_" + std::to_string(i));
            retina::Agent a(ws);
            auto ww = call(a, "write_file", json::object{{"path", "data.txt"}, {"content", "agent=" + std::to_string(i)}});
            auto rr = call(a, "read_file", json::object{{"path", "data.txt"}});
            worker_ok[static_cast<std::size_t>(i)] = ok(ww) && ok(rr) && result_string(rr, "content") == "agent=" + std::to_string(i);
        });
    }
    for (auto& t : workers) t.join();
    for (bool v : worker_ok) require(v, "parallel isolated Agent failed");

    // Cleanup.
    fs::remove_all(root, ec);
    fs::remove_all(outside, ec);
    if (failures == 0) {
        std::cout << "RETINA_0_2_3_FULL_TESTS_PASSED\n";
        return 0;
    }

    std::cerr << "RETINA_0_2_3_TESTS_FAILED: " << failures << " failure(s)\n";
    return 1;
}
