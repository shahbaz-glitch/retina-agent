#include "retina/agent.hpp"

#include <boost/process.hpp>
#include <boost/process/env.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace bp = boost::process;
namespace fs = std::filesystem;
namespace json = boost::json;

namespace retina {
namespace {

constexpr std::size_t kReadLimit = 4ULL * 1024ULL * 1024ULL;
constexpr std::size_t kToolOutputLimit = 512ULL * 1024ULL;
constexpr std::size_t kProcessOutputLimit = 1ULL * 1024ULL * 1024ULL;

std::string required_string(const json::object& o, std::string_view key) {
    const auto* v = o.if_contains(key);
    if (!v || !v->is_string()) throw std::runtime_error("argument must be a string: " + std::string(key));
    const std::string s = v->as_string().c_str();
    if (s.empty()) throw std::runtime_error("argument must not be empty: " + std::string(key));
    return s;
}

std::string optional_string(const json::object& o, std::string_view key, std::string fallback = {}) {
    const auto* v = o.if_contains(key);
    if (!v) return fallback;
    if (!v->is_string()) throw std::runtime_error("argument must be a string: " + std::string(key));
    return v->as_string().c_str();
}

std::int64_t optional_int(const json::object& o, std::string_view key, std::int64_t fallback) {
    const auto* v = o.if_contains(key);
    if (!v) return fallback;
    if (!v->is_int64()) throw std::runtime_error("argument must be an integer: " + std::string(key));
    return v->as_int64();
}

bool optional_bool(const json::object& o, std::string_view key, bool fallback) {
    const auto* v = o.if_contains(key);
    if (!v) return fallback;
    if (!v->is_bool()) throw std::runtime_error("argument must be a boolean: " + std::string(key));
    return v->as_bool();
}

std::vector<std::string> optional_string_array(const json::object& o, std::string_view key) {
    const auto* v = o.if_contains(key);
    if (!v) return {};
    if (!v->is_array()) throw std::runtime_error("argument must be a string array: " + std::string(key));
    std::vector<std::string> result;
    result.reserve(v->as_array().size());
    for (const auto& item : v->as_array()) {
        if (!item.is_string()) throw std::runtime_error("array elements must be strings: " + std::string(key));
        result.emplace_back(item.as_string().c_str());
    }
    return result;
}

json::object error(std::string code, std::string message) {
    return json::object{{"error", std::move(code)}, {"message", std::move(message)}};
}

struct ProcessResult {
    int exit_code{-1};
    bool timed_out{false};
    std::string stdout_text;
    std::string stderr_text;
    bool stdout_truncated{false};
    bool stderr_truncated{false};
};

std::string find_executable(const std::string& program, const fs::path& cwd) {
    fs::path raw(program);
    if (raw.has_parent_path() || raw.is_absolute()) {
        std::error_code ec;
        fs::path direct = raw.is_absolute() ? raw : (cwd / raw).lexically_normal();
        if (fs::is_regular_file(direct, ec) && !ec) return direct.string();
#ifdef _WIN32
        if (!raw.has_extension() && direct.extension().empty()) {
            direct += ".exe";
            ec.clear();
            if (fs::is_regular_file(direct, ec) && !ec) return direct.string();
        }
#endif
        throw std::runtime_error("executable not found: " + program);
    }

    const char* raw_path = std::getenv("PATH");
    if (!raw_path) throw std::runtime_error("PATH is not set");

#ifdef _WIN32
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif

    std::string path_env(raw_path);
    std::size_t start = 0;
    while (start <= path_env.size()) {
        const auto end = path_env.find(separator, start);
        const std::string dir = path_env.substr(start, end == std::string::npos ? std::string::npos : end - start);
        fs::path candidate = (dir.empty() ? cwd : fs::path(dir)) / raw;
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec) && !ec) return candidate.string();
#ifdef _WIN32
        if (!raw.has_extension()) {
            candidate += ".exe";
            ec.clear();
            if (fs::is_regular_file(candidate, ec) && !ec) return candidate.string();
        }
#endif
        if (end == std::string::npos) break;
        start = end + 1;
    }

    throw std::runtime_error("executable not found in PATH: " + program);
}

template <class Stream>
void drain_stream(Stream& stream, std::string& output, std::size_t limit, bool& truncated) {
    std::array<char, 64 * 1024> buffer{};
    while (stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || stream.gcount() > 0) {
        const std::streamsize count = stream.gcount();
        if (count <= 0) break;
        const std::size_t available = output.size() < limit ? limit - output.size() : 0;
        const std::size_t keep = std::min<std::size_t>(available, static_cast<std::size_t>(count));
        if (keep) output.append(buffer.data(), keep);
        if (keep < static_cast<std::size_t>(count)) truncated = true;
    }
}

ProcessResult run_program(const std::string& program,
                          const std::vector<std::string>& args,
                          const fs::path& cwd,
                          std::chrono::milliseconds timeout,
                          std::size_t max_output,
                          bool git_mode = false) {
    const std::string executable = find_executable(program, cwd);
    bp::ipstream out;
    bp::ipstream err;
    bp::group group;

    bp::environment env = boost::this_process::environment();
    if (git_mode) {
        env["GIT_TERMINAL_PROMPT"] = "0";
        env["GCM_INTERACTIVE"] = "Never";
        // Do not let inherited Git environment variables redirect the
        // repository, work tree, or index outside the Agent workspace.
        env.erase("GIT_DIR");
        env.erase("GIT_WORK_TREE");
        env.erase("GIT_INDEX_FILE");
    }

    bp::child child(executable,
                    bp::args(args),
                    bp::start_dir = cwd.string(),
                    bp::env = env,
                    bp::std_out > out,
                    bp::std_err > err,
                    group);

    std::string stdout_text;
    std::string stderr_text;
    bool stdout_truncated = false;
    bool stderr_truncated = false;

    std::thread out_reader([&] { drain_stream(out, stdout_text, max_output, stdout_truncated); });
    std::thread err_reader([&] { drain_stream(err, stderr_text, max_output, stderr_truncated); });

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool timed_out = false;
    while (child.running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (child.running()) {
        timed_out = true;
        std::error_code ec;
        group.terminate(ec);
        if (ec) child.terminate(ec);
    }

    child.wait();
    const int code = child.exit_code();
    out_reader.join();
    err_reader.join();
    if (group.valid()) group.detach();

    return {code, timed_out, std::move(stdout_text), std::move(stderr_text), stdout_truncated, stderr_truncated};
}

bool path_has_prefix(const fs::path& path, const fs::path& root) {
    auto p = path.begin();
    auto r = root.begin();
    for (; r != root.end(); ++r, ++p) {
        if (p == path.end() || *p != *r) return false;
    }
    return true;
}

std::string env_or(std::string_view name, std::string fallback) {
    if (const char* v = std::getenv(std::string(name).c_str()); v && *v) return v;
    return fallback;
}

std::string tool_executable(std::string_view variable, std::string_view name) {
    if (const char* explicit_path = std::getenv(std::string(variable).c_str()); explicit_path && *explicit_path) {
        return explicit_path;
    }
    const std::string tool_dir = env_or("RETINA_TOOL_DIR", "");
    if (!tool_dir.empty()) {
        return (fs::path(tool_dir) / std::string(name)).string();
    }
    return std::string(name);
}

bool is_remote_url_safe(const std::string& url) {
    const auto lower = [&] {
        std::string s = url;
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }();
    if (lower.find("http://") == 0 || lower.find("https://") == 0 || lower.find("file://") == 0 || lower.find("ssh://") == 0) {
        const auto scheme_end = lower.find("://");
        const auto at = lower.find('@', scheme_end + 3);
        const auto slash = lower.find('/', scheme_end + 3);
        return at == std::string::npos || (slash != std::string::npos && at > slash);
    }
    return false;
}

std::vector<std::string> git_args_for_action(const json::object& args, const std::string& action, const fs::path& workspace) {
    std::vector<std::string> argv;
    if (action == "clone") {
        const auto url = required_string(args, "url");
        const auto destination = required_string(args, "destination");
        if (!is_remote_url_safe(url)) throw std::runtime_error("unsupported or credential-bearing remote URL");
        const fs::path target = fs::path(destination);
        (void)workspace;
        argv = {"clone"};
        if (auto* b = args.if_contains("branch")) {
            if (!b->is_string()) throw std::runtime_error("argument must be a string: branch");
            argv.push_back("--branch"); argv.push_back(b->as_string().c_str());
        }
        if (auto* d = args.if_contains("depth")) {
            if (!d->is_int64()) throw std::runtime_error("argument must be an integer: depth");
            const auto depth = d->as_int64();
            if (depth < 1 || depth > 10000) throw std::runtime_error("depth out of range");
            argv.push_back("--depth"); argv.push_back(std::to_string(depth));
        }
        argv.push_back(url);
        argv.push_back(target.string());
        return argv;
    }
    if (action == "fetch") {
        argv = {"fetch"};
        const auto remote = optional_string(args, "remote", "origin");
        if (optional_bool(args, "prune", false)) argv.push_back("--prune");
        argv.push_back(remote);
        return argv;
    }
    if (action == "pull") {
        argv = {"pull"};
        if (optional_bool(args, "rebase", false)) argv.push_back("--rebase");
        const auto remote = optional_string(args, "remote", "origin");
        const auto branch = optional_string(args, "branch", "");
        argv.push_back(remote);
        if (!branch.empty()) argv.push_back(branch);
        return argv;
    }
    if (action == "push") {
        argv = {"push"};
        const bool set_upstream = optional_bool(args, "set_upstream", false);
        const bool force_with_lease = optional_bool(args, "force_with_lease", false);
        if (force_with_lease) argv.push_back("--force-with-lease");
        const auto remote = optional_string(args, "remote", "origin");
        const auto branch = optional_string(args, "branch", "");
        if (set_upstream) argv.push_back("-u");
        argv.push_back(remote);
        if (!branch.empty()) argv.push_back(branch);
        return argv;
    }
    return {};
}

} // namespace

Agent::Agent(fs::path workspace) {
    std::error_code ec;
    workspace_ = fs::weakly_canonical(fs::absolute(workspace), ec);
    if (ec) throw std::runtime_error("invalid workspace: " + ec.message());
    fs::create_directories(workspace_, ec);
    if (ec) throw std::runtime_error("cannot create workspace: " + ec.message());
    workspace_ = fs::weakly_canonical(workspace_, ec);
    if (ec) throw std::runtime_error("cannot canonicalize workspace: " + ec.message());
}

fs::path Agent::resolve_workspace_path(const std::string& input, bool must_exist) const {
    if (input.empty()) throw std::runtime_error("path must not be empty");
    fs::path candidate = fs::path(input).is_absolute() ? fs::path(input) : workspace_ / fs::path(input);
    candidate = fs::absolute(candidate).lexically_normal();

    std::error_code ec;
    fs::path canonical;
    if (must_exist || fs::exists(candidate, ec)) {
        canonical = fs::canonical(candidate, ec);
    } else {
        const fs::path parent = candidate.parent_path().empty() ? workspace_ : candidate.parent_path();
        const fs::path canonical_parent = fs::weakly_canonical(parent, ec);
        canonical = (canonical_parent / candidate.filename()).lexically_normal();
    }
    if (ec || !path_has_prefix(canonical, workspace_)) throw std::runtime_error("path escapes workspace");
    return canonical;
}

fs::path Agent::resolve_workspace_entry(const std::string& input, bool must_exist) const {
    if (input.empty()) throw std::runtime_error("path must not be empty");
    fs::path candidate = fs::absolute(fs::path(input).is_absolute() ? fs::path(input) : workspace_ / fs::path(input)).lexically_normal();
    std::error_code ec;
    const fs::path parent = candidate.parent_path().empty() ? workspace_ : candidate.parent_path();
    const fs::path canonical_parent = fs::weakly_canonical(parent, ec);
    if (ec || !path_has_prefix(canonical_parent, workspace_)) throw std::runtime_error("path escapes workspace");

    const fs::file_status st = fs::symlink_status(candidate, ec);
    if (ec && ec != std::make_error_code(std::errc::no_such_file_or_directory)) throw std::runtime_error("unable to inspect path: " + ec.message());
    if (ec == std::make_error_code(std::errc::no_such_file_or_directory)) ec.clear();
    if (!ec && fs::is_symlink(st)) {
        if (must_exist && !fs::exists(st)) throw std::runtime_error("path does not exist");
        return candidate;
    }
    if (must_exist || fs::exists(candidate, ec)) {
        const fs::path canonical = fs::canonical(candidate, ec);
        if (ec || !path_has_prefix(canonical, workspace_)) throw std::runtime_error("path escapes workspace");
        return canonical;
    }
    return (canonical_parent / candidate.filename()).lexically_normal();
}

json::array Agent::tool_schema() const {
    return json::array{
        json::object{{"name", "search_code"}, {"description", "Search project source using ugrep"},
                     {"parameters", json::object{{"query", "string"}, {"path", "string?"}, {"mode", "literal|regex?"}}}},
        json::object{{"name", "read_file"}, {"description", "Read a UTF-8 text file inside the workspace"},
                     {"parameters", json::object{{"path", "string"}, {"start_line", "integer?"}, {"end_line", "integer?"}}}},
        json::object{{"name", "write_file"}, {"description", "Create or replace a UTF-8 text file inside the workspace"},
                     {"parameters", json::object{{"path", "string"}, {"content", "string"}, {"create_dirs", "boolean?"}}}},
        json::object{{"name", "delete_path"}, {"description", "Delete a file or directory inside the workspace"},
                     {"parameters", json::object{{"path", "string"}}}},
        json::object{{"name", "move_path"}, {"description", "Move or rename a file or directory inside the workspace"},
                     {"parameters", json::object{{"from", "string"}, {"to", "string"}}}},
        json::object{{"name", "analyze_cpp"}, {"description", "Run clangd diagnostics/semantic parsing for a C++ file"},
                     {"parameters", json::object{{"path", "string"}, {"timeout_ms", "integer?"}}}},
        json::object{{"name", "run_process"}, {"description", "Run an executable directly, without a shell, inside the workspace"},
                     {"parameters", json::object{{"program", "string"}, {"args", "string[]?"}, {"working_directory", "string?"}, {"timeout_ms", "integer?"}}}},
        json::object{{"name", "git"}, {"description", "Execute supported Git operations in the workspace"},
                     {"parameters", json::object{{"action", "status|diff|log|branch|init|add|commit|clone|fetch|pull|push|remote"}, {"args", "string[]?"}}}}
    };
}

json::object Agent::dispatch(const json::object& request) {
    const auto* tool_v = request.if_contains("tool");
    if (!tool_v || !tool_v->is_string()) return json::object{{"ok", false}, {"error", "invalid_request"}, {"message", "tool must be a string"}};
    const std::string tool = tool_v->as_string().c_str();

    const json::object* args = nullptr;
    if (auto* v = request.if_contains("arguments")) {
        if (!v->is_object()) return json::object{{"ok", false}, {"error", "invalid_arguments"}, {"message", "arguments must be an object"}};
        args = &v->as_object();
    }
    const json::object empty_args;
    if (!args) args = &empty_args;

    try {
        auto wrap = [](json::object result) -> json::object {
            if (result.if_contains("error")) {
                json::object response{{"ok", false}};
                response["error"] = result["error"];
                if (result.if_contains("message")) response["message"] = result["message"];
                return response;
            }
            return json::object{{"ok", true}, {"result", std::move(result)}};
        };
        if (tool == "tools") return json::object{{"ok", true}, {"result", tool_schema()}};
        if (tool == "search_code") return wrap(search_code(*args));
        if (tool == "read_file") return wrap(read_file(*args));
        if (tool == "write_file") return wrap(write_file(*args));
        if (tool == "delete_path") return wrap(delete_path(*args));
        if (tool == "move_path") return wrap(move_path(*args));
        if (tool == "analyze_cpp") return wrap(analyze_cpp(*args));
        if (tool == "run_process") return wrap(run_process(*args));
        if (tool == "git") return wrap(git(*args));
        return json::object{{"ok", false}, {"error", "unknown_tool"}, {"message", "unknown tool: " + tool}};
    } catch (const std::exception& ex) {
        return json::object{{"ok", false}, {"error", "tool_error"}, {"message", ex.what()}};
    }
}

json::object Agent::search_code(const json::object& args) {
    const std::string query = required_string(args, "query");
    const std::string relative_path = optional_string(args, "path", ".");
    const std::string mode = optional_string(args, "mode", "literal");
    if (mode != "literal" && mode != "regex") return error("invalid_arguments", "mode must be literal or regex");

    const fs::path path = resolve_workspace_path(relative_path, true);
    std::vector<std::string> argv{"-R", "-n", "--color=never", "--no-heading", "--exclude-dir=.git"};
    if (mode == "literal") argv.push_back("-F");
    // Use -e so a literal/regex query beginning with '-' is always treated
    // as the pattern, never as an option.
    argv.push_back("-e");
    argv.push_back(query);
    argv.push_back(path.string());

    const std::string ugrep = tool_executable("RETINA_UGREP", "ugrep");
    auto r = run_program(ugrep, argv, workspace_, std::chrono::seconds(30), kToolOutputLimit);
    return json::object{
        {"backend", "ugrep"},
        {"exit_code", r.exit_code},
        {"matched", r.exit_code == 0},
        {"output", r.stdout_text},
        {"stderr", r.stderr_text},
        {"stdout_truncated", r.stdout_truncated},
        {"stderr_truncated", r.stderr_truncated}
    };
}

json::object Agent::read_file(const json::object& args) {
    const fs::path path = resolve_workspace_path(required_string(args, "path"), true);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) return error("invalid_target", "path is not a regular file");
    const auto size = fs::file_size(path, ec);
    if (ec) return error("read_failed", ec.message());
    if (size > kReadLimit) return error("file_too_large", "read_file is capped at 4 MiB");

    std::ifstream in(path, std::ios::binary);
    if (!in) return error("read_failed", "unable to open file");
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    const auto start = std::max<std::int64_t>(1, optional_int(args, "start_line", 1));
    const auto end = optional_int(args, "end_line", -1);
    if (end == 0 || end < -1) return error("invalid_arguments", "end_line must be -1 or >= 1");
    if (start == 1 && end < 0) {
        return json::object{{"path", fs::relative(path, workspace_).string()}, {"content", content}, {"bytes", static_cast<std::int64_t>(content.size())}};
    }

    std::istringstream lines(content);
    std::ostringstream sliced;
    std::string line;
    std::int64_t line_no = 0;
    while (std::getline(lines, line)) {
        ++line_no;
        if (line_no < start) continue;
        if (end >= 0 && line_no > end) break;
        if (sliced.tellp() > 0) sliced << '\n';
        sliced << line;
    }
    return json::object{{"path", fs::relative(path, workspace_).string()}, {"content", sliced.str()}, {"start_line", start}, {"end_line", end}};
}

json::object Agent::write_file(const json::object& args) {
    const std::string relative_path = required_string(args, "path");
    const auto* content_v = args.if_contains("content");
    if (!content_v || !content_v->is_string()) return error("invalid_arguments", "content must be a string");
    const std::string content = content_v->as_string().c_str();
    const fs::path path = resolve_workspace_entry(relative_path, false);
    const bool create_dirs = optional_bool(args, "create_dirs", true);

    std::error_code ec;
    const auto st = fs::symlink_status(path, ec);
    if (!ec && fs::is_symlink(st)) return error("invalid_target", "write_file will not replace a symlink");
    if (!ec && fs::exists(st) && !fs::is_regular_file(st)) return error("invalid_target", "path is not a regular file");

    if (create_dirs) fs::create_directories(path.parent_path(), ec);
    if (ec) return error("directory_failed", ec.message());

    static std::atomic_uint64_t counter{0};
    const fs::path tmp = path.string() + ".retina.tmp." + std::to_string(++counter);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return error("write_failed", "unable to create temporary file");
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out) return error("write_failed", "unable to write file");
    }

    if (fs::exists(path, ec)) {
        if (ec) { fs::remove(tmp); return error("write_failed", ec.message()); }
        fs::remove(path, ec);
        if (ec) { fs::remove(tmp); return error("write_failed", ec.message()); }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp);
        return error("write_failed", ec.message());
    }
    return json::object{{"path", relative_path}, {"bytes", static_cast<std::int64_t>(content.size())}, {"written", true}};
}

json::object Agent::delete_path(const json::object& args) {
    const std::string relative_path = required_string(args, "path");
    const fs::path path = resolve_workspace_entry(relative_path, true);
    if (path == workspace_) return error("protected_path", "workspace root cannot be deleted");

    std::error_code ec;
    const auto st = fs::symlink_status(path, ec);
    if (ec) return error("delete_failed", ec.message());
    if (fs::is_symlink(st)) {
        if (!fs::remove(path, ec) || ec) return error("delete_failed", ec ? ec.message() : "unable to delete symlink");
    } else {
        if (!fs::exists(st)) return error("not_found", "path does not exist");
        fs::remove_all(path, ec);
        if (ec) return error("delete_failed", ec.message());
    }
    return json::object{{"path", relative_path}, {"deleted", true}};
}

json::object Agent::move_path(const json::object& args) {
    const std::string from = required_string(args, "from");
    const std::string to = required_string(args, "to");
    const fs::path source = resolve_workspace_entry(from, true);
    const fs::path target = resolve_workspace_entry(to, false);
    if (source == workspace_ || target == workspace_) return error("protected_path", "workspace root cannot be moved");

    std::error_code ec;
    const auto source_status = fs::symlink_status(source, ec);
    if (ec) return error("move_failed", ec.message());
    if (fs::is_directory(source_status) && path_has_prefix(target, source)) return error("invalid_target", "cannot move a directory into itself");
    const auto target_status = fs::symlink_status(target, ec);
    if (!ec && fs::exists(target_status)) return error("invalid_target", "destination already exists");

    fs::create_directories(target.parent_path(), ec);
    if (ec) return error("move_failed", ec.message());
    fs::rename(source, target, ec);
    if (ec) return error("move_failed", ec.message());
    return json::object{{"from", from}, {"to", to}, {"moved", true}};
}

json::object Agent::analyze_cpp(const json::object& args) {
    const fs::path path = resolve_workspace_path(required_string(args, "path"), true);
    const auto timeout_ms = std::clamp<std::int64_t>(optional_int(args, "timeout_ms", 60000), 1000, 300000);
    const std::string clangd = tool_executable("RETINA_CLANGD", "clangd");
    const std::vector<std::string> argv{
        "--check=" + path.string(),
        "--compile-commands-dir=" + workspace_.string(),
        "--log=error"
    };
    auto r = run_program(clangd, argv, workspace_, std::chrono::milliseconds(timeout_ms), kToolOutputLimit);
    return json::object{
        {"backend", "clangd"},
        {"exit_code", r.exit_code},
        {"timed_out", r.timed_out},
        {"diagnostics", r.stderr_text},
        {"output", r.stdout_text},
        {"stdout_truncated", r.stdout_truncated},
        {"stderr_truncated", r.stderr_truncated},
        {"success", r.exit_code == 0 && !r.timed_out}
    };
}

json::object Agent::run_process(const json::object& args) {
    const std::string program = required_string(args, "program");
    const auto argv = optional_string_array(args, "args");
    const auto cwd = resolve_workspace_path(optional_string(args, "working_directory", "."), true);
    const auto timeout_ms = std::clamp<std::int64_t>(optional_int(args, "timeout_ms", 120000), 100, 600000);

    auto r = run_program(program, argv, cwd, std::chrono::milliseconds(timeout_ms), kProcessOutputLimit);
    return json::object{
        {"program", program},
        {"exit_code", r.exit_code},
        {"timed_out", r.timed_out},
        {"stdout", r.stdout_text},
        {"stderr", r.stderr_text},
        {"stdout_truncated", r.stdout_truncated},
        {"stderr_truncated", r.stderr_truncated},
        {"success", r.exit_code == 0 && !r.timed_out}
    };
}

json::object Agent::git(const json::object& args) {
    const std::string action = required_string(args, "action");
    const std::string git_exe = tool_executable("RETINA_GIT", "git");

    fs::path repo = workspace_;
    if (args.if_contains("path")) {
        repo = resolve_workspace_path(optional_string(args, "path", "."), true);
        if (!fs::is_directory(repo)) return error("invalid_target", "git path is not a directory");
    }

    std::vector<std::string> argv;
    if (action == "clone") {
        const std::string destination = required_string(args, "destination");
        const fs::path target = resolve_workspace_entry(destination, false);
        std::error_code ec;
        if (fs::exists(target, ec) || fs::is_symlink(target, ec)) return error("invalid_target", "clone destination already exists");
        argv = git_args_for_action(args, action, workspace_);
        repo = workspace_;
    } else if (action == "fetch" || action == "pull" || action == "push") {
        argv = git_args_for_action(args, action, workspace_);
    } else if (action == "remote") {
        const std::string mode = optional_string(args, "mode", "list");
        if (mode == "list") argv = {"remote", "-v"};
        else if (mode == "get_url") argv = {"remote", "get-url", required_string(args, "name")};
        else if (mode == "add") {
            const auto name = required_string(args, "name");
            const auto url = required_string(args, "url");
            if (!is_remote_url_safe(url)) return error("invalid_arguments", "unsupported or credential-bearing remote URL");
            argv = {"remote", "add", name, url};
        } else return error("invalid_arguments", "remote mode must be list, get_url, or add");
    } else {
        static const std::vector<std::string> allowed{"status", "diff", "log", "branch", "init", "add", "commit"};
        if (std::find(allowed.begin(), allowed.end(), action) == allowed.end()) return error("unsupported_action", action);
        argv = {action};
        auto extras = optional_string_array(args, "args");
        for (const auto& extra : extras) {
            // These Git options can redirect repository/work-tree/index
            // handling outside the Agent workspace. Keep Git useful and
            // flexible, but reject repository-boundary overrides.
            if (extra == "-C" || extra == "--git-dir" || extra == "--work-tree" ||
                extra == "--separate-git-dir" || extra == "--exec-path" ||
                extra.rfind("--git-dir=", 0) == 0 ||
                extra.rfind("--work-tree=", 0) == 0 ||
                extra.rfind("--separate-git-dir=", 0) == 0 ||
                extra.rfind("--exec-path=", 0) == 0) {
                return error("invalid_arguments", "Git argument would override the workspace repository boundary");
            }
        }
        argv.insert(argv.end(), extras.begin(), extras.end());
    }

    auto r = run_program(git_exe, argv, repo, std::chrono::seconds(action == "clone" || action == "fetch" || action == "pull" || action == "push" ? 120 : 60), kProcessOutputLimit, true);
    return json::object{
        {"backend", "git-cli"},
        {"action", action},
        {"exit_code", r.exit_code},
        {"stdout", r.stdout_text},
        {"stderr", r.stderr_text},
        {"stdout_truncated", r.stdout_truncated},
        {"stderr_truncated", r.stderr_truncated},
        {"timed_out", r.timed_out},
        {"success", r.exit_code == 0 && !r.timed_out}
    };
}

} // namespace retina
