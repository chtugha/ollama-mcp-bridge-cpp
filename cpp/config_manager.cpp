#include "config_manager.h"

#include <fstream>
#include <stdexcept>

#include "utils.h"

namespace omb {

using json = nlohmann::json;

ConfigManager::ConfigManager(const std::string& config_file_path)
    : config_path_(config_file_path) {
    {
        std::ifstream ifs(config_path_);
        if (!ifs) {
            throw std::runtime_error("ConfigManager: cannot open config file: " + config_path_);
        }
        mcp_config_ = json::parse(ifs);
    }

    auto state_path = state_file_path();
    if (std::filesystem::exists(state_path)) {
        std::ifstream ifs(state_path);
        if (ifs) {
            try {
                bridge_state_ = json::parse(ifs);
            } catch (...) {
                bridge_state_ = json::object();
            }
        } else {
            bridge_state_ = json::object();
        }
    } else {
        bridge_state_ = json::object();
        atomic_write_file(state_path.string(), bridge_state_.dump(2));
    }
}

json ConfigManager::get_mcp_config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mcp_config_;
}

void ConfigManager::save_mcp_config(const json& cfg) {
    std::lock_guard<std::mutex> lock(mutex_);
    atomic_write_file(config_path_, cfg.dump(2));
    mcp_config_ = cfg;
}

std::string ConfigManager::get_active_cert() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bridge_state_.contains("web_tls_active_cert") &&
        bridge_state_["web_tls_active_cert"].is_string()) {
        return bridge_state_["web_tls_active_cert"].get<std::string>();
    }
    return "";
}

void ConfigManager::set_active_cert(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    bridge_state_["web_tls_active_cert"] = name;
    atomic_write_file(state_file_path().string(), bridge_state_.dump(2));
}

std::filesystem::path ConfigManager::config_dir() const {
    auto p = std::filesystem::path(config_path_).parent_path();
    return p.empty() ? std::filesystem::current_path() : p;
}

std::filesystem::path ConfigManager::state_file_path() const {
    return config_dir() / "bridge-state.json";
}

}
