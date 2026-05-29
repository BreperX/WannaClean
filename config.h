#pragma once

#include <string>
#include <vector>
#include <map>

struct ProfileConfig {
    std::string              name;
    std::vector<std::string> extraKill;
    std::vector<std::string> stopServices;
    bool                     useWhitelist;   // true = usar whitelist pura (nuclear)
};

struct AppConfig {
    std::vector<std::string> whitelist;
    std::map<std::string, ProfileConfig> profiles;
    std::string logFile;
    bool        dryRun;         // true = simula sin aplicar cambios
    bool        showDiskNote;
};

bool LoadConfig(const std::string& path, AppConfig& out, std::string& errorMsg);

bool SaveConfig(const std::string& path, const AppConfig& cfg, std::string& errorMsg);

bool CreateDefaultConfig(const std::string& path, std::string& errorMsg);

const ProfileConfig* GetProfile(const AppConfig& cfg, const std::string& name);