#include "process.h"
#include <iostream>
#include <algorithm>
#include <cctype>

// ============================================================
// WHITELIST - Procesos que NUNCA se tocan
// ============================================================
static const std::vector<std::string> DEFAULT_WHITELIST = {
    "system", "idle", "registry",
    "smss.exe", "csrss.exe", "wininit.exe",
    "winlogon.exe", "lsass.exe", "services.exe",
    "svchost.exe", "dwm.exe", "explorer.exe",
    "audiodg.exe", "fontdrvhost.exe", "ctfmon.exe",
    "conhost.exe", "cmd.exe", "taskhostw.exe",
    "msmpeng.exe", "securityhealthsystray.exe",
    "runtimebroker.exe", "shellexperiencehost.exe",
    "startmenuexperiencehost.exe", "sihost.exe",
    "spoolsv.exe", "lsaiso.exe", "memory compression"
};

// ============================================================
// HELPERS
// ============================================================
static std::string ToLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return out;
}

static bool IsWhitelisted(const std::string& name,
    const std::vector<std::string>& extraWhitelist) {
    std::string low = ToLower(name);
    for (const auto& w : DEFAULT_WHITELIST)
        if (low == w) return true;
    for (const auto& w : extraWhitelist)
        if (low == ToLower(w)) return true;
    return false;
}

// ============================================================
// SNAPSHOT - Captura todo el arbol de procesos de una vez
// ============================================================
std::vector<ProcessInfo> SnapshotProcesses() {
    std::vector<ProcessInfo> result;

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return result;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(hSnap, &pe)) {
        do {
            ProcessInfo info;
            info.pid = pe.th32ProcessID;
            info.parentPid = pe.th32ParentProcessID;

            // Convertir nombre de wchar a string
            char buf[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1,
                buf, MAX_PATH, nullptr, nullptr);
            info.name = buf;

            result.push_back(info);
        } while (Process32NextW(hSnap, &pe));
    }

    CloseHandle(hSnap);
    return result;
}

// ============================================================
// KILL RECURSIVO - Mata hijos antes que el padre
// ============================================================
static void KillTree(DWORD pid, DWORD selfPid, const std::vector<ProcessInfo>& snapshot,
    KillResult& result) {
    // PROTECCIÓN ABSOLUTA: Si el proceso actual a evaluar soy yo mismo, abortar rama
    if (pid == selfPid || pid == 0 || pid == 4) return;

    // Primero matar todos los hijos recursivamente
    for (const auto& p : snapshot) {
        if (p.parentPid == pid && p.pid != pid) {
            // Pasamos el selfPid en la recursión
            KillTree(p.pid, selfPid, snapshot, result);
        }
    }

    // Doble verificación antes de abrir el proceso
    if (pid == selfPid) return;

    // Luego matar el proceso actual
    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (hProc) {
        if (TerminateProcess(hProc, 1)) {
            result.killed++;
        }
        else {
            result.failed.push_back("PID " + std::to_string(pid) +
                " (TerminateProcess fallo)");
        }
        CloseHandle(hProc);
    }
}

// ============================================================
// KILL POR NOMBRE
// ============================================================
KillResult KillByName(const std::string& name, DWORD selfPid,
    const std::vector<ProcessInfo>& snapshot,
    const std::vector<std::string>& extraWhitelist) {
    KillResult result;
    std::string low = ToLower(name);

    for (const auto& p : snapshot) {
        if (ToLower(p.name) == low) {
            if (p.pid == selfPid) continue; // Ignorarme por completo
            if (IsWhitelisted(p.name, extraWhitelist)) {
                result.skipped++;
                continue;
            }
            KillTree(p.pid, selfPid, snapshot, result);
        }
    }
    return result;
}

// ============================================================
// NUCLEAR - Mata todo lo que no este en whitelist
// ============================================================
KillResult KillAllExceptWhitelist(DWORD selfPid,
    const std::vector<ProcessInfo>& snapshot,
    const std::vector<std::string>& extraWhitelist) {
    KillResult total;

    for (const auto& p : snapshot) {
        // No suicidarse
        if (p.pid == selfPid || p.pid == 0 || p.pid == 4) continue;

        // Respetar whitelist
        if (IsWhitelisted(p.name, extraWhitelist)) {
            total.skipped++;
            continue;
        }

        KillResult r;
        // Blindado: Pasamos el selfPid para proteger la app en el modo nuclear
        KillTree(p.pid, selfPid, snapshot, r);

        total.killed += r.killed;
        total.skipped += r.skipped;
        for (auto& f : r.failed)
            total.failed.push_back(p.name + ": " + f);
    }

    return total;
}

// ============================================================
// SERVICIOS
// ============================================================
ServiceResult StopService(const std::string& serviceName) {
    ServiceResult result;
    result.name = serviceName;

    SC_HANDLE hSCM = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) {
        result.success = false;
        result.reason = "No se pudo abrir SCManager";
        return result;
    }

    SC_HANDLE hSvc = OpenServiceA(hSCM, serviceName.c_str(),
        SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!hSvc) {
        result.success = false;
        result.reason = "Servicio no encontrado o sin acceso";
        CloseServiceHandle(hSCM);
        return result;
    }

    SERVICE_STATUS_PROCESS ssp;
    DWORD needed;
    QueryServiceStatusEx(hSvc, SC_STATUS_PROCESS_INFO,
        (LPBYTE)&ssp, sizeof(ssp), &needed);

    if (ssp.dwCurrentState == SERVICE_STOPPED) {
        result.success = false;
        result.reason = "Ya estaba detenido";
        CloseServiceHandle(hSvc);
        CloseServiceHandle(hSCM);
        return result;
    }

    SERVICE_STATUS ss;
    if (ControlService(hSvc, SERVICE_CONTROL_STOP, &ss)) {
        result.success = true;
    }
    else {
        result.success = false;
        result.reason = "ControlService fallo (codigo: " +
            std::to_string(GetLastError()) + ")";
    }

    CloseServiceHandle(hSvc);
    CloseServiceHandle(hSCM);
    return result;
}

// ============================================================
// LIMPIEZA DE RAM (reemplaza RAMMap)
// ============================================================
bool EnableMemoryPrivilege() {
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(),
        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;

    TOKEN_PRIVILEGES tp;
    LookupPrivilegeValueA(nullptr, "SeProfileSingleProcessPrivilege",
        &tp.Privileges[0].Luid);
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    AdjustTokenPrivileges(hToken, FALSE, &tp, 0, nullptr, nullptr);
    CloseHandle(hToken);
    return true;
}

RamCleanResult CleanRAM() {
    RamCleanResult result;
    EnableMemoryPrivilege();

    typedef LONG(WINAPI* PFN_NtSetSystemInformation)(INT, PVOID, ULONG);
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    auto NtSetSysInfo = hNtdll
        ? (PFN_NtSetSystemInformation)GetProcAddress(hNtdll, "NtSetSystemInformation")
        : nullptr;

    if (!NtSetSysInfo) {
        result.stepsFailed.push_back("NtSetSystemInformation no disponible");
        return result;
    }

    const int cmds[] = { 2, 3, 4 };
    const char* names[] = { "EmptyWorkingSets", "FlushModifiedList", "PurgeStandbyList" };

    for (int i = 0; i < 3; i++) {
        INT cmd = cmds[i];
        LONG status = NtSetSysInfo(80, &cmd, sizeof(cmd));

        if (status == 0) {
            result.stepsOk.push_back(names[i]);
        }
        else {
            result.stepsFailed.push_back(
                std::string(names[i]) + " (NTSTATUS: " + std::to_string(status) + ")"
            );
        }
    }

    return result;
}