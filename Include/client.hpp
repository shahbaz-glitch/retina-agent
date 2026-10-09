#pragma once

#include <boost/process.hpp>
#include <boost/json.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace retina {

class Client {
public:
    Client() = default;
    ~Client();
    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    static Client launch(const std::filesystem::path& agent_executable,
                         const std::filesystem::path& workspace);

    boost::json::object call(const std::string& tool, const boost::json::object& arguments = {});
    boost::json::array tools();
    bool alive() const;
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Client(std::unique_ptr<Impl> impl);
};

} // namespace retina
