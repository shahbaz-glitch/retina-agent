#include "retina/agent.hpp"

#include <boost/json.hpp>

#include <filesystem>
#include <iostream>
#include <string>

namespace json = boost::json;

int main(int argc, char** argv) {
    bool stdio = false;
    std::filesystem::path workspace = std::filesystem::current_path();

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--stdio") {
            stdio = true;
        } else if (arg == "--workspace" && i + 1 < argc) {
            workspace = argv[++i];
        } else if (arg == "--help") {
            std::cout << "Retina Agent\\n  --stdio --workspace <path>\\n";
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 2;
        }
    }

    try {
        retina::Agent agent(workspace);
        if (!stdio) {
            std::cout << "Retina Agent ready\\n";
            return 0;
        }

        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty()) continue;
            try {
                const auto request = json::parse(line).as_object();
                const auto response = agent.dispatch(request);
                std::cout << json::serialize(response) << '\n' << std::flush;
            } catch (const std::exception& ex) {
                json::object response{{"ok", false}, {"error", "invalid_request"}, {"message", ex.what()}};
                std::cout << json::serialize(response) << '\n' << std::flush;
            }
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "startup failure: " << ex.what() << '\n';
        return 1;
    }
}
