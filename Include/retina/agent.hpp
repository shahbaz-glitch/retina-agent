#pragma once

#include <boost/json.hpp>

#include <filesystem>

namespace retina {

class Agent {
public:
    explicit Agent(std::filesystem::path workspace);

    const std::filesystem::path& workspace() const noexcept { return workspace_; }

    boost::json::object dispatch(const boost::json::object& request);
    boost::json::array tool_schema() const;

private:
    std::filesystem::path workspace_;

    boost::json::object search_code(const boost::json::object& args);
    boost::json::object read_file(const boost::json::object& args);
    boost::json::object write_file(const boost::json::object& args);
    boost::json::object delete_path(const boost::json::object& args);
    boost::json::object move_path(const boost::json::object& args);
    boost::json::object analyze_cpp(const boost::json::object& args);
    boost::json::object run_process(const boost::json::object& args);
    boost::json::object git(const boost::json::object& args);

    std::filesystem::path resolve_workspace_path(const std::string& input, bool must_exist) const;
    std::filesystem::path resolve_workspace_entry(const std::string& input, bool must_exist) const;
};

} // namespace retina
