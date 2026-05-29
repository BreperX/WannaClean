#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <pdh.h>
#include <string>
#include <vector>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")

struct RamStats {
    double totalGB = 0.0;
    double usedGB = 0.0;
    double freeGB = 0.0;
    double usedPct = 0.0;
    bool   ok = false;
};

struct CpuStats {
    double usedPct = 0.0;
    int    coreCount = 0;
    double tempC = 0.0;
    bool   hasTemp = false;
    bool   ok = false;
};

struct GpuStats {
    std::string name;
    double vramTotalGB = 0.0;
    double vramUsedGB = 0.0;
    double vramFreeGB = 0.0;
    double vramSharedGB = 0.0;
    double vramUsedPct = 0.0;
    double gpuUsePct = 0.0;
    bool   hasUsage = false;
    bool   ok = false;
    std::string reason;
};

struct DiskStats {
    std::string drive;
    double totalGB = 0.0;
    double usedGB = 0.0;
    double freeGB = 0.0;
    double activityMBs = 0.0;
    bool   hasActivity = false;
    bool   ok = false;
};

struct SystemSnapshot {
    RamStats  ram;
    CpuStats  cpu;
    GpuStats  gpu;
    DiskStats disk;
};

struct SystemDiff {
    double ramDeltaGB = 0.0;
    double cpuDelta = 0.0;
    double vramDeltaGB = 0.0;
    double diskDeltaMBs = 0.0;
};

RamStats  GetRamStats();
CpuStats  GetCpuStats();
GpuStats  GetGpuStats(ID3D11Device* pDevice = nullptr);
DiskStats GetDiskStats(const std::string& drive = "C:\\");

SystemSnapshot TakeSnapshot(int cpuSampleMs = 0, ID3D11Device* pDevice = nullptr);
SystemDiff     DiffSnapshots(const SystemSnapshot& before, const SystemSnapshot& after);

void   StartCpuPdhSampler();
double GetCpuPdhLastPct();
double GetCpuWmiLastPct();
double GetCpuNtLastPct();
double GetCpuSysLastPct();
double GetCpuTemperatureWmi();