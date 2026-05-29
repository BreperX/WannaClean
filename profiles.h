#pragma once

#include "config.h"
#include "process.h"
#include "resources.h"
#include <string>
#include <functional>

struct ProfileResult {
    std::string name;
    SystemSnapshot  before;
    SystemSnapshot  after;
    SystemDiff      diff;
    KillResult      kills;
    int             servicesStopped = 0;
    int             servicesFailed  = 0;
    RamCleanResult  ramClean;
    std::vector<ServiceResult> serviceResults;
    bool dryRun = false;
};

using ProgressCallback = std::function<void(const std::string& msg, int pct)>;
ProfileResult RunProfile(const std::string&   profileName,
                         const AppConfig&     cfg,
                         ProgressCallback     onProgress = nullptr);

void LogResult(const ProfileResult& result, const AppConfig& cfg);
void PrintSnapshot(const SystemSnapshot& snap, const std::string& label);
void PrintResult(const ProfileResult& result);
