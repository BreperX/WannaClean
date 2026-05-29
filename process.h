#pragma once

#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <vector>

struct ProcessInfo {
    DWORD       pid;
    DWORD       parentPid;
    std::string name;
};

struct KillResult {
    int                      killed = 0;
    int                      skipped = 0;
    std::vector<std::string> failed;
};

struct ServiceResult {
    std::string name;
    bool        success = false;
    std::string reason;
};

struct RamCleanResult {
    std::vector<std::string> stepsOk;
    std::vector<std::string> stepsFailed;
};

std::vector<ProcessInfo> SnapshotProcesses();

KillResult KillByName(const std::string& name, DWORD selfPid,
    const std::vector<ProcessInfo>& snapshot,
    const std::vector<std::string>& extraWhitelist = {});

KillResult KillAllExceptWhitelist(DWORD selfPid,
    const std::vector<ProcessInfo>& snapshot,
    const std::vector<std::string>& extraWhitelist = {});

ServiceResult StopService(const std::string& serviceName);

bool           EnableMemoryPrivilege();
RamCleanResult CleanRAM();