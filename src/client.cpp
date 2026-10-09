#include "retina/client.hpp"

#include <stdexcept>

namespace bp = boost::process;
namespace json = boost::json;

namespace retina {

struct Client::Impl {
    bp::opstream in;
    bp::ipstream out;
    bp::child child;
    std::uint64_t next_id{1};

    Impl(const std::filesystem::path& exe, const std::filesystem::path& workspace)
        : child(exe.string(), std::vector<std::string>{"--stdio", "--workspace", workspace.string()},
                bp::std_in < in, bp::std_out > out) {}
};

Client::Client(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Client::~Client() { stop(); }
Client::Client(Client&& other) noexcept = default;
Client& Client::operator=(Client&& other) noexcept = default;

Client Client::launch(const std::filesystem::path& agent_executable, const std::filesystem::path& workspace) {
    return Client(std::make_unique<Impl>(agent_executable, workspace));
}

bool Client::alive() const {
    return impl_ && impl_->child.running();
}

json::object Client::call(const std::string& tool, const json::object& arguments) {
    if (!impl_ || !impl_->child.running()) throw std::runtime_error("Retina agent is not running");
    const auto id = std::to_string(impl_->next_id++);
    json::object request{{"id", id}, {"tool", tool}, {"arguments", arguments}};
    impl_->in << json::serialize(request) << '\n';
    impl_->in.flush();

    std::string line;
    if (!std::getline(impl_->out, line)) throw std::runtime_error("Retina agent closed its output");
    auto parsed = json::parse(line);
    if (!parsed.is_object()) throw std::runtime_error("Retina agent returned non-object response");
    return parsed.as_object();
}

json::array Client::tools() {
    const auto response = call("tools");
    if (auto* ok = response.if_contains("ok"); ok && ok->is_bool() && ok->as_bool()) {
        if (auto* v = response.if_contains("result"); v && v->is_array()) return v->as_array();
    }
    return {};
}

void Client::stop() {
    if (!impl_) return;
    if (impl_->child.running()) {
        try { impl_->child.terminate(); } catch (...) {}
        try { impl_->child.wait(); } catch (...) {}
    }
    impl_.reset();
}

} // namespace retina
