#include "profiles.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <iostream>
#include <algorithm>

#undef min
#undef max

static void Emit(ProgressCallback& cb, const std::string& msg, int pct) {
    if (cb) cb(msg, pct);
}

static std::string NowTimestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm;
    localtime_s(&tm, &t);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

static std::string FormatDelta(double val, const std::string& unit, int decimals = 1) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(decimals);
    if (val < 0) ss << "▼ " << std::abs(val) << " " << unit;
    else         ss << "▲ " << val << " " << unit;
    return ss.str();
}

static void LogResult(const ProfileResult& result, const AppConfig& cfg) {
    std::ofstream f(cfg.logFile, std::ios::app);
    if (!f.is_open()) return;

    f << "\n============================================================\n";
    f << "  " << NowTimestamp() << " — Perfil: " << result.name;
    if (result.dryRun) f << " [DRY RUN]";
    f << "\n============================================================\n";

    f << "\n[ANTES]\n";
    f << "  RAM:  " << std::fixed << std::setprecision(1)
        << result.before.ram.usedGB << " / " << result.before.ram.totalGB << " GB"
        << "  (" << result.before.ram.usedPct << "%)\n";
    f << "  CPU:  " << result.before.cpu.usedPct << "%\n";
    if (result.before.gpu.ok)
        f << "  GPU:  " << result.before.gpu.vramUsedGB << " GB VRAM\n";

    f << "\n[DESPUES]\n";
    f << "  RAM:  " << result.after.ram.usedGB << " / "
        << result.after.ram.totalGB << " GB"
        << "  (" << result.after.ram.usedPct << "%)\n";
    f << "  CPU:  " << result.after.cpu.usedPct << "%\n";
    if (result.after.gpu.ok)
        f << "  GPU:  " << result.after.gpu.vramUsedGB << " GB VRAM\n";

    f << "\n[DELTA]\n";
    f << "  RAM:  " << FormatDelta(result.diff.ramDeltaGB, "GB") << "\n";
    f << "  CPU:  " << FormatDelta(result.diff.cpuDelta, "%", 0) << "\n";
    f << "  GPU:  " << FormatDelta(result.diff.vramDeltaGB, "GB") << "\n";
    f << "  Disk: " << FormatDelta(result.diff.diskDeltaMBs, "MB/s", 0)
        << (result.after.disk.activityMBs > result.before.disk.activityMBs
            ? " (normal: flush de cache)" : "") << "\n";

    f << "\n[PROCESOS]\n";
    f << "  Matados:  " << result.kills.killed << "\n";
    f << "  Omitidos: " << result.kills.skipped << "\n";
    if (!result.kills.failed.empty()) {
        f << "  Fallidos:\n";
        for (auto& fail : result.kills.failed)
            f << "    - " << fail << "\n";
    }

    f << "\n[SERVICIOS]\n";
    f << "  Detenidos: " << result.servicesStopped << "\n";
    f << "  Fallidos:  " << result.servicesFailed << "\n";
    for (auto& sr : result.serviceResults) {
        if (!sr.success)
            f << "    - " << sr.name << ": " << sr.reason << "\n";
    }

    f << "\n[RAM CLEAN]\n";
    for (auto& s : result.ramClean.stepsOk)
        f << "  OK:   " << s << "\n";
    for (auto& s : result.ramClean.stepsFailed)
        f << "  FAIL: " << s << "\n";

    f << "\n";
}

ProfileResult RunProfile(const std::string& profileName, const AppConfig& cfg, ProgressCallback onProgress) {
    ProfileResult result;
    result.name = profileName;
    result.dryRun = cfg.dryRun;

    const ProfileConfig* profile = GetProfile(cfg, profileName);
    if (!profile) {
        Emit(onProgress, "[ERROR] Perfil no encontrado: " + profileName, 100);
        return result;
    }

    Emit(onProgress, "[SNAP]  Tomando snapshot inicial...", 0);
    result.before = TakeSnapshot(300);
    Emit(onProgress, "[SNAP]  Snapshot listo.", 5);

    Emit(onProgress, "[SCAN]  Escaneando procesos activos...", 8);
    auto snapshot = SnapshotProcesses();
    DWORD selfPid = GetCurrentProcessId();

    std::vector<std::string> whitelist = cfg.whitelist;

    // Proteger el propio proceso ejecutable por nombre
    for (auto& p : snapshot) {
        if (p.pid == selfPid) {
            whitelist.push_back(p.name);
            break;
        }
    }

    Emit(onProgress, "[SCAN]  " + std::to_string(snapshot.size()) + " procesos encontrados.", 12);

    if (profile->useWhitelist) {
        Emit(onProgress, "[KILL]  Modo NUCLEAR — whitelist pura activada.", 15);

        if (!cfg.dryRun) {
            result.kills = KillAllExceptWhitelist(selfPid, snapshot, whitelist);
        }
        else {
            for (auto& p : snapshot) {
                if (p.pid == selfPid || p.pid == 0 || p.pid == 4) continue;
                bool safe = false;
                for (auto& w : whitelist) {
                    std::string low = p.name;
                    std::transform(low.begin(), low.end(), low.begin(), ::tolower);
                    std::string wlow = w;
                    std::transform(wlow.begin(), wlow.end(), wlow.begin(), ::tolower);
                    if (low == wlow) { safe = true; break; }
                }
                if (!safe) result.kills.killed++;
                else       result.kills.skipped++;
            }
        }

        Emit(onProgress, "[KILL]  " + std::to_string(result.kills.killed) + " procesos eliminados, " +
            std::to_string(result.kills.skipped) + " protegidos.", 50);
    }
    else {
        int total = static_cast<int>(profile->extraKill.size());
        int done = 0;

        for (auto& procName : profile->extraKill) {
            int pct = 15 + (done * 35 / std::max(total, 1));
            Emit(onProgress, "[KILL]  " + procName + "...", pct);

            if (!cfg.dryRun) {
                KillResult r = KillByName(procName, selfPid, snapshot, whitelist);

                result.kills.killed += r.killed;
                result.kills.skipped += r.skipped;
                for (auto& f : r.failed)
                    result.kills.failed.push_back(f);

                if (r.killed > 0)
                    Emit(onProgress, "[KILL]  " + procName + " (" + std::to_string(r.killed) + " instancias)", pct);
                else
                    Emit(onProgress, "[SKIP]  " + procName + " — no corria", pct);
            }
            else {
                Emit(onProgress, "[DRY]   " + procName + " (simulado)", pct);
                result.kills.killed++;
            }
            done++;
        }
    }

    Emit(onProgress, "[SVC]   Deteniendo servicios...", 55);
    int svcIdx = 0;
    int svcTotal = static_cast<int>(profile->stopServices.size());

    for (auto& svcName : profile->stopServices) {
        int pct = 55 + (svcIdx * 15 / std::max(svcTotal, 1));
        Emit(onProgress, "[SVC]   " + svcName + "...", pct);

        if (!cfg.dryRun) {
            ServiceResult sr = StopService(svcName);
            result.serviceResults.push_back(sr);

            if (sr.success) {
                result.servicesStopped++;
                Emit(onProgress, "[STOP]  " + svcName, pct);
            }
            else {
                result.servicesFailed++;
                Emit(onProgress, "[SKIP]  " + svcName + " — " + sr.reason, pct);
            }
        }
        else {
            Emit(onProgress, "[DRY]   " + svcName + " (simulado)", pct);
            result.servicesStopped++;
        }
        svcIdx++;
    }

    Emit(onProgress, "[RAM]   Limpiando cache de memoria...", 72);

    if (!cfg.dryRun) {
        result.ramClean = CleanRAM();

        for (auto& step : result.ramClean.stepsOk)
            Emit(onProgress, "[RAM]   " + step + " — ok", 75);
        for (auto& step : result.ramClean.stepsFailed)
            Emit(onProgress, "[RAM]   " + step + " — FALLO", 75);
    }
    else {
        Emit(onProgress, "[DRY]   RAM clean (simulado)", 75);
    }

    Emit(onProgress, "[SNAP]  Tomando snapshot final...", 85);
    Sleep(500);
    result.after = TakeSnapshot(300);
    result.diff = DiffSnapshots(result.before, result.after);

    Emit(onProgress, "[SNAP]  Snapshot final listo.", 95);

    if (!cfg.logFile.empty()) {
        LogResult(result, cfg);
        Emit(onProgress, "[LOG]   Guardado en " + cfg.logFile, 98);
    }

    std::ostringstream summary;
    summary << "[DONE]  "
        << result.kills.killed << " matados | "
        << result.servicesStopped << " servicios detenidos | "
        << std::fixed << std::setprecision(1)
        << std::abs(result.diff.ramDeltaGB) << " GB RAM liberados";

    if (result.kills.failed.size() > 0 || result.servicesFailed > 0)
        summary << " | FALLOS: " << (result.kills.failed.size() + result.servicesFailed);

    Emit(onProgress, summary.str(), 100);
    return result;
}

void PrintSnapshot(const SystemSnapshot& snap, const std::string& label) {
    std::cout << "\n  [" << label << "]\n";
    std::cout << "  RAM:  " << std::fixed << std::setprecision(1)
        << snap.ram.usedGB << " / " << snap.ram.totalGB << " GB"
        << "  (" << (int)snap.ram.usedPct << "%)\n";
    std::cout << "  CPU:  " << (int)snap.cpu.usedPct << "%"
        << "  (" << snap.cpu.coreCount << " nucleos)\n";

    if (snap.gpu.ok) {
        std::cout << "  GPU:  " << snap.gpu.name << "\n";
        if (snap.gpu.hasUsage)
            std::cout << "  VRAM: " << snap.gpu.vramUsedGB << " / "
            << snap.gpu.vramTotalGB << " GB\n";
        else
            std::cout << "  VRAM: " << snap.gpu.vramTotalGB << " GB total\n";
    }
    else {
        std::cout << "  GPU:  no disponible\n";
    }

    if (snap.disk.ok) {
        std::cout << "  Disk: " << snap.disk.freeGB << " GB libres";
        if (snap.disk.hasActivity)
            std::cout << " | " << (int)snap.disk.activityMBs << " MB/s";
        std::cout << "\n";
    }
}

void PrintResult(const ProfileResult& result) {
    std::cout << "\n  ============================================\n";
    std::cout << "  RESULTADO — " << result.name;
    if (result.dryRun) std::cout << " [DRY RUN]";
    std::cout << "\n  ============================================\n";

    PrintSnapshot(result.before, "ANTES");
    PrintSnapshot(result.after, "DESPUES");

    std::cout << "\n  [DELTA]\n";
    std::cout << "  RAM:  " << FormatDelta(result.diff.ramDeltaGB, "GB") << "\n";
    std::cout << "  CPU:  " << FormatDelta(result.diff.cpuDelta, "%", 0) << "\n";
    std::cout << "  GPU:  " << FormatDelta(result.diff.vramDeltaGB, "GB") << "\n";
    std::cout << "  Disk: " << FormatDelta(result.diff.diskDeltaMBs, "MB/s", 0) << "\n";

    std::cout << "\n  [PROCESOS]\n";
    std::cout << "  Matados:  " << result.kills.killed << "\n";
    std::cout << "  Omitidos: " << result.kills.skipped << "\n";
    if (!result.kills.failed.empty()) {
        std::cout << "  Fallidos:\n";
        for (auto& f : result.kills.failed)
            std::cout << "    - " << f << "\n";
    }

    std::cout << "\n  [SERVICIOS]\n";
    std::cout << "  Detenidos: " << result.servicesStopped << "\n";
    std::cout << "  Fallidos:  " << result.servicesFailed << "\n";
    for (auto& sr : result.serviceResults) {
        if (!sr.success)
            std::cout << "    - " << sr.name << ": " << sr.reason << "\n";
    }

    std::cout << "\n  [RAM CLEAN]\n";
    for (auto& s : result.ramClean.stepsOk)
        std::cout << "  OK:   " << s << "\n";
    for (auto& s : result.ramClean.stepsFailed)
        std::cout << "  FAIL: " << s << "\n";

    std::cout << "\n";
}