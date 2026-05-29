#include "resources.h"
#include <iostream>
#include <dxgi1_4.h>
#include <comdef.h>
#include <Wbemidl.h>
#include <thread>
#include <atomic>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <winternl.h>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "wbemuuid.lib")

static ULONGLONG g_prevIdle = 0, g_prevKern = 0, g_prevUser = 0;
static bool      g_cpuInit = false;

static PDH_HQUERY   g_cpuQuery = nullptr;
static PDH_HCOUNTER g_cpuCounter = nullptr;
static bool          g_cpuPdhInit = false;

static std::atomic<bool> g_cpuPdhSamplerRunning{ false };
static std::atomic<double> g_cpuPdhLastPct{ 0.0 };
static std::atomic<double> g_cpuWmiLastPct{ 0.0 };
static std::atomic<double> g_cpuNtLastPct{ 0.0 };
static std::atomic<double> g_cpuSysLastPct{ 0.0 };

static PDH_HQUERY   g_gpuQuery = nullptr;
static PDH_HCOUNTER g_gpuCounter = nullptr;
static bool          g_gpuPdhInit = false;
static bool          g_gpuPdhOk = false;
static double        g_gpuVramUsedBytes = 0.0;

static PDH_HQUERY   g_gpuEngQuery = nullptr;
static PDH_HCOUNTER g_gpuEngCounter = nullptr;
static bool          g_gpuEngInit = false;
static bool          g_gpuEngOk = false;
static std::string  g_gpuLuid;

static PDH_HQUERY   g_hQuery = nullptr;
static PDH_HCOUNTER g_hCounter = nullptr;
static bool          g_pdhInit = false;
static bool          g_pdhOk = false;

static double QueryNtCpu();

RamStats GetRamStats() {
    RamStats s;
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        s.totalGB = static_cast<double>(ms.ullTotalPhys) / (1024.0 * 1024 * 1024);
        s.usedGB = static_cast<double>(ms.ullTotalPhys - ms.ullAvailPhys) / (1024.0 * 1024 * 1024);
        s.freeGB = static_cast<double>(ms.ullAvailPhys) / (1024.0 * 1024 * 1024);
        s.usedPct = static_cast<double>(ms.dwMemoryLoad);
        s.ok = true;
    }
    return s;
}

static double SimulateTaskManagerCpu(double sysPct) {
    if (sysPct <= 0.0) return 0.0;

    double x = sysPct;
    double y = x;

    // Curva de aproximacion para emular el comportamiento de carga del Administrador de Tareas
    if (x <= 6.0) {
        y = x * 6.166;
    }
    else if (x <= 12.0) {
        y = 37.0 + ((x - 6.0) * 1.166);
    }
    else if (x <= 30.0) {
        y = 44.0 + ((x - 12.0) * 1.166);
    }
    else if (x <= 74.0) {
        y = 65.0 + ((x - 30.0) * 0.1136);
    }
    else {
        y = 70.0 + ((x - 74.0) * 0.269);
    }

    if (y < 0.0) return 0.0;
    if (y > 100.0) return 100.0;
    return y;
}

static ULONGLONG FileTimeToULL(const FILETIME& ft) {
    return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

void StartCpuPdhSampler() {
    if (g_cpuPdhSamplerRunning.load(std::memory_order_acquire)) return;
    if (!g_cpuPdhInit) {
        g_cpuPdhInit = true;
        if (PdhOpenQueryA(nullptr, 0, &g_cpuQuery) == ERROR_SUCCESS) {
            const char* cpuPaths[] = {
                "\\Processor(_Total)\\% Processor Time",
                "\\Procesador(_Total)\\% de tiempo de procesador"
            };
            bool added = false;
            for (const char* cpuPath : cpuPaths) {
                if (PdhAddCounterA(g_cpuQuery, cpuPath, 0, &g_cpuCounter) == ERROR_SUCCESS) {
                    PdhCollectQueryData(g_cpuQuery);
                    added = true; break;
                }
            }
            if (!added) { PdhCloseQuery(g_cpuQuery); g_cpuQuery = nullptr; g_cpuCounter = nullptr; }
        }
    }

    if (!g_cpuQuery || !g_cpuCounter) return;
    g_cpuPdhSamplerRunning.store(true);
    std::thread([]() {
        std::ofstream log("metrics.log", std::ios::app);
        while (g_cpuPdhSamplerRunning.load()) {
            FILETIME idle, kern, user;
            double sysV = -1.0;

            if (GetSystemTimes(&idle, &kern, &user)) {
                static ULONGLONG lastIdle = 0, lastKern = 0, lastUser = 0;

                ULONGLONG curIdle = FileTimeToULL(idle);
                ULONGLONG curKern = FileTimeToULL(kern);
                ULONGLONG curUser = FileTimeToULL(user);

                if (lastIdle != 0) {
                    ULONGLONG dIdle = curIdle - lastIdle;
                    ULONGLONG dKern = curKern - lastKern;
                    ULONGLONG dUser = curUser - lastUser;
                    ULONGLONG dTotal = dKern + dUser;

                    if (dTotal > 0) {
                        double pct = (double)(dTotal - dIdle) * 100.0 / (double)dTotal;
                        if (pct < 0.0) pct = 0.0;
                        if (pct > 100.0) pct = 100.0;
                        sysV = pct;
                    }
                }

                lastIdle = curIdle;
                lastKern = curKern;
                lastUser = curUser;
            }

            double visualV = (sysV >= 0.0) ? SimulateTaskManagerCpu(sysV) : -1.0;

            if (visualV >= 0.0) g_cpuPdhLastPct.store(visualV);
            if (sysV >= 0.0) g_cpuSysLastPct.store(sysV);

            double ntV = QueryNtCpu();
            if (ntV >= 0.0) g_cpuNtLastPct.store(ntV);

            auto now = std::chrono::system_clock::now();
            std::time_t tt = std::chrono::system_clock::to_time_t(now);
            std::tm tm;
            localtime_s(&tm, &tt);

            std::ostringstream oss;
            oss << std::put_time(&tm, "%F %T") << " | PDH: ";
            if (visualV >= 0.0) oss << std::fixed << std::setprecision(2) << visualV << "%"; else oss << "N/A";
            oss << " | SYS: ";
            if (sysV >= 0.0) oss << std::fixed << std::setprecision(2) << sysV << "%"; else oss << "N/A";
            oss << " | NT: ";
            if (ntV >= 0.0) oss << std::fixed << std::setprecision(2) << ntV << "%"; else oss << "N/A";

            std::string line = oss.str();
            if (log) log << line << std::endl;
            OutputDebugStringA((line + "\n").c_str());

            Sleep(1000);
        }
        if (log) log.close();
        }).detach();
}

double GetCpuPdhLastPct() { return g_cpuPdhLastPct.load(); }
double GetCpuWmiLastPct() { return g_cpuWmiLastPct.load(); }
double GetCpuNtLastPct() { return g_cpuNtLastPct.load(); }
double GetCpuSysLastPct() { return g_cpuSysLastPct.load(); }

typedef LONG NTSTATUS;
typedef NTSTATUS(NTAPI* NtQuerySysInfo_t)(ULONG, PVOID, ULONG, PULONG);

static double QueryNtCpu() {
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    if (!hNtdll) return -1.0;
    auto fn = (NtQuerySysInfo_t)GetProcAddress(hNtdll, "NtQuerySystemInformation");
    if (!fn) return -1.0;
    SYSTEM_INFO si; GetSystemInfo(&si);
    DWORD cpus = si.dwNumberOfProcessors;
    std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> infos(cpus);
    ULONG ret = 0;
    NTSTATUS st = fn(8, infos.data(), (ULONG)(sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION) * cpus), &ret);
    if (st < 0) return -1.0;
    static uint64_t lastIdle = 0, lastKernel = 0, lastUser = 0;
    uint64_t sumIdle = 0, sumKern = 0, sumUser = 0;
    for (DWORD i = 0; i < cpus; ++i) {
        sumIdle += static_cast<uint64_t>(infos[i].IdleTime.QuadPart);
        sumKern += static_cast<uint64_t>(infos[i].KernelTime.QuadPart);
        sumUser += static_cast<uint64_t>(infos[i].UserTime.QuadPart);
    }
    double res = -1.0;
    if (lastIdle == 0) {
        lastIdle = sumIdle; lastKernel = sumKern; lastUser = sumUser;
    }
    else {
        uint64_t dIdle = sumIdle - lastIdle;
        uint64_t dK = sumKern - lastKernel;
        uint64_t dU = sumUser - lastUser;
        uint64_t dTotal = dK + dU;
        uint64_t dBusy = dTotal - dIdle;
        if (dTotal > 0) {
            long double pct = (long double)dBusy / (long double)dTotal * 100.0L;
            if (pct < 0) pct = 0; if (pct > 100) pct = 100;
            res = (double)pct;
        }
        lastIdle = sumIdle; lastKernel = sumKern; lastUser = sumUser;
    }
    return res;
}

double GetCpuTemperatureWmi() {
    double result = -1.0;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool needUninit = SUCCEEDED(hr);

    IWbemLocator* pLoc = nullptr;
    IWbemServices* pSvc = nullptr;
    IEnumWbemClassObject* pEnum = nullptr;

    hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID*)&pLoc);
    if (FAILED(hr)) goto cleanup;

    hr = pLoc->ConnectServer(_bstr_t(L"ROOT\\WMI"), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &pSvc);
    if (FAILED(hr)) goto cleanup;

    CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

    hr = pSvc->ExecQuery(_bstr_t(L"WQL"), _bstr_t(L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature"), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnum);
    if (FAILED(hr)) goto cleanup;

    {
        IWbemClassObject* pObj = nullptr;
        ULONG returned = 0;
        double sumC = 0.0;
        int    count = 0;

        while (pEnum->Next(WBEM_INFINITE, 1, &pObj, &returned) == S_OK && returned > 0) {
            VARIANT vt;
            VariantInit(&vt);
            if (SUCCEEDED(pObj->Get(L"CurrentTemperature", 0, &vt, nullptr, nullptr)) && vt.vt == VT_I4) {
                // WMI devuelve el valor en décimas de Kelvin
                double tempC = (vt.lVal / 10.0) - 273.15;
                if (tempC > 0.0 && tempC < 150.0) {
                    sumC += tempC;
                    count++;
                }
            }
            VariantClear(&vt);
            pObj->Release();
        }

        if (count > 0)
            result = sumC / count;
    }

cleanup:
    if (pEnum) pEnum->Release();
    if (pSvc)  pSvc->Release();
    if (pLoc)  pLoc->Release();
    if (needUninit) CoUninitialize();

    return result;
}

CpuStats GetCpuStats() {
    CpuStats s;
    s.ok = false;

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    s.coreCount = static_cast<int>(si.dwNumberOfProcessors);

    if (!g_cpuPdhInit) {
        g_cpuPdhInit = true;
        if (PdhOpenQueryA(nullptr, 0, &g_cpuQuery) == ERROR_SUCCESS) {
            const char* cpuPaths[] = {
                "\\Processor(_Total)\\% Processor Time",
                "\\Procesador(_Total)\\% de tiempo de procesador"
            };
            bool added = false;
            for (const char* cpuPath : cpuPaths) {
                if (PdhAddCounterA(g_cpuQuery, cpuPath, 0, &g_cpuCounter) == ERROR_SUCCESS) {
                    PdhCollectQueryData(g_cpuQuery);
                    added = true; break;
                }
            }
            if (!added) {
                PdhCloseQuery(g_cpuQuery);
                g_cpuQuery = nullptr;
                g_cpuCounter = nullptr;
            }
        }
    }

    FILETIME idle, kern, user;

    if (GetSystemTimes(&idle, &kern, &user)) {
        static ULONGLONG lastIdle = 0;
        static ULONGLONG lastKern = 0;
        static ULONGLONG lastUser = 0;
        static double smoothed = 0.0;

        ULONGLONG curIdle = FileTimeToULL(idle);
        ULONGLONG curKern = FileTimeToULL(kern);
        ULONGLONG curUser = FileTimeToULL(user);

        if (lastIdle != 0) {
            ULONGLONG dIdle = curIdle - lastIdle;
            ULONGLONG dKern = curKern - lastKern;
            ULONGLONG dUser = curUser - lastUser;
            ULONGLONG dTotal = dKern + dUser;

            if (dTotal > 0) {
                double realPct = ((double)(dTotal - dIdle) / (double)dTotal) * 100.0;

                if (realPct < 0.0) realPct = 0.0;
                if (realPct > 100.0) realPct = 100.0;

                double visualPct = SimulateTaskManagerCpu(realPct);
                if (realPct == 0.0) visualPct = 0.0;

                // Suavizado reactivo (40% historial, 60% actual)
                if (smoothed == 0.0) smoothed = visualPct;
                smoothed = smoothed * 0.40 + visualPct * 0.60;

                s.usedPct = smoothed;
                s.ok = true;
            }
        }

        lastIdle = curIdle;
        lastKern = curKern;
        lastUser = curUser;

        g_prevIdle = curIdle;
        g_prevKern = curKern;
        g_prevUser = curUser;
        g_cpuInit = true;

        if (s.ok) {
            double t = GetCpuTemperatureWmi();
            if (t > 0.0) { s.tempC = t; s.hasTemp = true; }
            return s;
        }
    }

    if (g_cpuInit && g_cpuQuery && g_cpuCounter) {
        PdhCollectQueryData(g_cpuQuery);
        PDH_FMT_COUNTERVALUE val = {};
        if (PdhGetFormattedCounterValue(g_cpuCounter, PDH_FMT_DOUBLE, nullptr, &val) == ERROR_SUCCESS) {
            s.usedPct = SimulateTaskManagerCpu(val.doubleValue);
            s.ok = true;
        }
    }

    {
        double t = GetCpuTemperatureWmi();
        if (t > 0.0) {
            s.tempC = t;
            s.hasTemp = true;
        }
    }

    return s;
}

static std::string GetFirstGpuAdapterInstance() {
    char  szObj[256] = "GPU Adapter Memory";
    char  szInst[8192] = {};
    char  szCnt[4096] = {};
    DWORD dwInst = sizeof(szInst), dwCnt = sizeof(szCnt);
    if (PdhEnumObjectItemsA(nullptr, nullptr, szObj, szCnt, &dwCnt, szInst, &dwInst, PERF_DETAIL_WIZARD, 0) != ERROR_SUCCESS)
        return {};
    if (szInst[0] == 0) return {};
    return std::string(szInst);
}

GpuStats GetGpuStats(ID3D11Device* pDevice) {
    GpuStats s;
    s.ok = false;
    s.hasUsage = false;

    IDXGIAdapter3* pA3 = nullptr;
    std::string nameFromDesc;
    SIZE_T dedicatedVram = 0;

    if (pDevice) {
        IDXGIDevice* pDXGIDevice = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&pDXGIDevice))) {
            IDXGIAdapter* pAdapter = nullptr;
            if (SUCCEEDED(pDXGIDevice->GetAdapter(&pAdapter))) {
                DXGI_ADAPTER_DESC desc = {};
                if (SUCCEEDED(pAdapter->GetDesc(&desc))) {
                    char buf[128] = {};
                    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, buf, sizeof(buf), nullptr, nullptr);
                    nameFromDesc = buf;
                    dedicatedVram = desc.DedicatedVideoMemory;
                }
                pAdapter->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&pA3);
                pAdapter->Release();
            }
            pDXGIDevice->Release();
        }
    }

    if (!pA3) {
        IDXGIFactory1* pFactory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&pFactory))) return s;

        IDXGIAdapter1* pAdapter = nullptr;
        for (UINT i = 0; pFactory->EnumAdapters1(i, &pAdapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 desc = {};
            if (FAILED(pAdapter->GetDesc1(&desc))) { pAdapter->Release(); continue; }
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { pAdapter->Release(); continue; }

            if (SUCCEEDED(pAdapter->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&pA3))) {
                char buf[128] = {};
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, buf, sizeof(buf), nullptr, nullptr);
                nameFromDesc = buf;
                dedicatedVram = desc.DedicatedVideoMemory;
                pAdapter->Release();
                if (dedicatedVram > (256ULL * 1024 * 1024)) break;
                pA3->Release(); pA3 = nullptr;
            }
            else {
                pAdapter->Release();
            }
        }
        pFactory->Release();
    }

    if (!pA3) return s;

    s.name = nameFromDesc;
    s.vramTotalGB = static_cast<double>(dedicatedVram) / (1024.0 * 1024 * 1024);

    DXGI_QUERY_VIDEO_MEMORY_INFO localMem = {}, nonLocalMem = {};
    bool gotLocal = SUCCEEDED(pA3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &localMem));
    bool gotNonLocal = SUCCEEDED(pA3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocalMem));
    pA3->Release();

    if (gotLocal) {
        if (!g_gpuPdhInit) {
            g_gpuPdhInit = true;
            g_gpuPdhOk = false;
            std::string inst = GetFirstGpuAdapterInstance();
            if (!inst.empty() && PdhOpenQueryA(nullptr, 0, &g_gpuQuery) == ERROR_SUCCESS) {
                std::string path = "\\GPU Adapter Memory(" + inst + ")\\Dedicated Usage";
                if (PdhAddCounterA(g_gpuQuery, path.c_str(), 0, &g_gpuCounter) == ERROR_SUCCESS) {
                    PdhCollectQueryData(g_gpuQuery);
                    g_gpuPdhOk = true;
                    auto phys = inst.find("_phys");
                    if (phys != std::string::npos)
                        g_gpuLuid = inst.substr(0, phys);
                }
                else {
                    PdhCloseQuery(g_gpuQuery);
                    g_gpuQuery = nullptr;
                    g_gpuCounter = nullptr;
                }
            }
        }

        if (g_gpuPdhOk && g_gpuCounter) {
            PdhCollectQueryData(g_gpuQuery);
            PDH_FMT_COUNTERVALUE val = {};
            if (PdhGetFormattedCounterValue(g_gpuCounter, PDH_FMT_DOUBLE, nullptr, &val) == ERROR_SUCCESS && val.doubleValue > 0.0) {
                g_gpuVramUsedBytes = val.doubleValue;
            }
        }

        if (!g_gpuEngInit) {
            g_gpuEngInit = true;
            g_gpuEngOk = false;
            if (PdhOpenQueryA(nullptr, 0, &g_gpuEngQuery) == ERROR_SUCCESS) {
                const char* counterNames[] = {
                    "\\GPU Engine(*)\\Utilization Percentage",
                    "\\GPU Engine(*)\\% Utilization"
                };
                bool added = false;
                for (const char* path : counterNames) {
                    if (PdhAddCounterA(g_gpuEngQuery, path, 0, &g_gpuEngCounter) == ERROR_SUCCESS) {
                        PdhCollectQueryData(g_gpuEngQuery);
                        g_gpuEngOk = true;
                        added = true;
                        break;
                    }
                }
                if (!added) {
                    PdhCloseQuery(g_gpuEngQuery);
                    g_gpuEngQuery = nullptr;
                    g_gpuEngCounter = nullptr;
                }
            }
        }

        double gpuUsePct = 0.0;
        if (g_gpuEngOk && g_gpuEngCounter) {
            PdhCollectQueryData(g_gpuEngQuery);
            DWORD bufSize = 0, itemCount = 0;
            PdhGetFormattedCounterArrayA(g_gpuEngCounter, PDH_FMT_DOUBLE, &bufSize, &itemCount, nullptr);
            if (bufSize > 0) {
                std::vector<PDH_FMT_COUNTERVALUE_ITEM_A> items(bufSize / sizeof(PDH_FMT_COUNTERVALUE_ITEM_A) + 1);
                if (PdhGetFormattedCounterArrayA(g_gpuEngCounter, PDH_FMT_DOUBLE, &bufSize, &itemCount, items.data()) == ERROR_SUCCESS) {
                    for (DWORD i = 0; i < itemCount; i++) {
                        std::string name = items[i].szName ? items[i].szName : "";
                        if (!g_gpuLuid.empty() && name.find(g_gpuLuid) == std::string::npos) continue;
                        if (name.find("engtype_3d") == std::string::npos && name.find("engtype_3D") == std::string::npos) continue;
                        gpuUsePct += items[i].FmtValue.doubleValue;
                    }
                    gpuUsePct = std::min(gpuUsePct, 100.0);
                    if (gpuUsePct < 0.0) gpuUsePct = 0.0;
                }
            }
        }

        double usedBytes = (g_gpuPdhOk && g_gpuVramUsedBytes > 0.0) ? g_gpuVramUsedBytes : static_cast<double>(localMem.CurrentUsage);

        s.vramTotalGB = static_cast<double>(dedicatedVram) / (1024.0 * 1024 * 1024);
        s.vramUsedGB = usedBytes / (1024.0 * 1024 * 1024);
        if (s.vramUsedGB > s.vramTotalGB) s.vramUsedGB = s.vramTotalGB;
        s.vramFreeGB = s.vramTotalGB - s.vramUsedGB;
        if (s.vramFreeGB < 0) s.vramFreeGB = 0;
        s.vramUsedPct = (s.vramTotalGB > 0.01) ? (s.vramUsedGB / s.vramTotalGB * 100.0) : 0.0;
        s.gpuUsePct = gpuUsePct;
        if (gotNonLocal)
            s.vramSharedGB = static_cast<double>(nonLocalMem.CurrentUsage) / (1024.0 * 1024 * 1024);
        s.hasUsage = true;
        s.ok = true;
    }

    return s;
}

DiskStats GetDiskStats(const std::string& drive) {
    DiskStats s;
    s.drive = drive;
    s.ok = false;
    s.hasActivity = false;
    s.activityMBs = 0.0;

    ULARGE_INTEGER freeBytes, totalBytes, totalFree;
    if (GetDiskFreeSpaceExA(drive.c_str(), &freeBytes, &totalBytes, &totalFree)) {
        s.totalGB = static_cast<double>(totalBytes.QuadPart) / (1024.0 * 1024 * 1024);
        s.freeGB = static_cast<double>(freeBytes.QuadPart) / (1024.0 * 1024 * 1024);
        s.usedGB = s.totalGB - s.freeGB;
        s.ok = true;
    }

    if (!g_pdhInit) {
        g_pdhInit = true;
        g_pdhOk = false;

        if (PdhOpenQueryA(nullptr, 0, &g_hQuery) == ERROR_SUCCESS) {
            char szObj[256] = {}, szCnt[256] = {};
            DWORD d1 = 256, d2 = 256;

            bool gotObj = (PdhLookupPerfNameByIndexA(nullptr, 234, szObj, &d1) == ERROR_SUCCESS);
            const DWORD counterIdx[] = { 220, 1402, 1158 };
            bool gotCnt = false;
            for (DWORD idx : counterIdx) {
                d2 = 256;
                if (PdhLookupPerfNameByIndexA(nullptr, idx, szCnt, &d2) == ERROR_SUCCESS && szCnt[0]) {
                    gotCnt = true;
                    break;
                }
            }

            if (gotObj && gotCnt && szObj[0] && szCnt[0]) {
                std::string path = "\\" + std::string(szObj) + "(_Total)\\" + std::string(szCnt);
                if (PdhAddCounterA(g_hQuery, path.c_str(), 0, &g_hCounter) == ERROR_SUCCESS) {
                    PdhCollectQueryData(g_hQuery);
                    g_pdhOk = true;
                }
            }

            if (!g_pdhOk) {
                d1 = 256; d2 = 256;
                char szLogObj[256] = {};
                if (PdhLookupPerfNameByIndexA(nullptr, 236, szLogObj, &d1) == ERROR_SUCCESS) {
                    for (DWORD idx : counterIdx) {
                        d2 = 256;
                        if (PdhLookupPerfNameByIndexA(nullptr, idx, szCnt, &d2) == ERROR_SUCCESS && szCnt[0]) {
                            std::string path = "\\" + std::string(szLogObj) + "(_Total)\\" + std::string(szCnt);
                            if (PdhAddCounterA(g_hQuery, path.c_str(), 0, &g_hCounter) == ERROR_SUCCESS) {
                                PdhCollectQueryData(g_hQuery);
                                g_pdhOk = true;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    if (g_pdhOk && g_hCounter) {
        PdhCollectQueryData(g_hQuery);
        PDH_FMT_COUNTERVALUE val = {};
        if (PdhGetFormattedCounterValue(g_hCounter, PDH_FMT_DOUBLE, nullptr, &val) == ERROR_SUCCESS) {
            s.activityMBs = val.doubleValue / (1024.0 * 1024.0);
            if (s.activityMBs < 0.0) s.activityMBs = 0.0;
            s.hasActivity = true;
        }
    }

    return s;
}

SystemSnapshot TakeSnapshot(int cpuSampleMs, ID3D11Device* pDevice) {
    SystemSnapshot snap;
    snap.cpu = GetCpuStats();
    snap.ram = GetRamStats();
    snap.gpu = GetGpuStats(pDevice);
    snap.disk = GetDiskStats("C:\\");
    return snap;
}

SystemDiff DiffSnapshots(const SystemSnapshot& before, const SystemSnapshot& after) {
    SystemDiff d;
    d.ramDeltaGB = after.ram.usedGB - before.ram.usedGB;
    d.cpuDelta = after.cpu.usedPct - before.cpu.usedPct;
    d.vramDeltaGB = after.gpu.vramUsedGB - before.gpu.vramUsedGB;
    d.diskDeltaMBs = after.disk.activityMBs - before.disk.activityMBs;
    return d;
}