#include "config.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <windows.h>

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

// Extrae el valor crudo de una clave en un bloque JSON manual
static std::string ExtractRaw(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";

    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos) return "";

    char start = json[pos];
    if (start == '"') {
        size_t end = json.find('"', pos + 1);
        while (end != std::string::npos && json[end - 1] == '\\')
            end = json.find('"', end + 1);
        return (end == std::string::npos) ? "" : json.substr(pos, end - pos + 1);
    }
    if (start == '[' || start == '{') {
        char close = (start == '[') ? ']' : '}';
        int depth = 1;
        size_t i = pos + 1;
        while (i < json.size() && depth > 0) {
            if (json[i] == start)  depth++;
            if (json[i] == close)  depth--;
            i++;
        }
        return json.substr(pos, i - pos);
    }
    size_t end = json.find_first_of(",}\n", pos);
    return (end == std::string::npos) ? json.substr(pos) : json.substr(pos, end - pos);
}

static std::vector<std::string> ParseStringArray(const std::string& arr) {
    std::vector<std::string> result;
    size_t pos = 0;
    while ((pos = arr.find('"', pos)) != std::string::npos) {
        pos++;
        size_t end = arr.find('"', pos);
        while (end != std::string::npos && arr[end - 1] == '\\')
            end = arr.find('"', end + 1);
        if (end == std::string::npos) break;
        result.push_back(arr.substr(pos, end - pos));
        pos = end + 1;
    }
    return result;
}

static bool ParseBool(const std::string& raw, bool fallback = false) {
    std::string t = Trim(raw);
    if (t == "true")  return true;
    if (t == "false") return false;
    return fallback;
}

// Extrae objetos internos dentro del nodo "profiles"
static std::map<std::string, std::string> ExtractProfileBlocks(const std::string& profilesObj) {
    std::map<std::string, std::string> result;

    size_t pos = profilesObj.find('{');
    if (pos == std::string::npos) return result;
    pos++;

    while (true) {
        size_t nameStart = profilesObj.find('"', pos);
        if (nameStart == std::string::npos) break;
        size_t nameEnd = profilesObj.find('"', nameStart + 1);
        if (nameEnd == std::string::npos) break;
        std::string name = profilesObj.substr(nameStart + 1, nameEnd - nameStart - 1);
        pos = nameEnd + 1;

        size_t blockStart = profilesObj.find('{', pos);
        if (blockStart == std::string::npos) break;

        int depth = 1;
        size_t i = blockStart + 1;
        while (i < profilesObj.size() && depth > 0) {
            if (profilesObj[i] == '{') depth++;
            if (profilesObj[i] == '}') depth--;
            i++;
        }
        result[name] = profilesObj.substr(blockStart, i - blockStart);
        pos = i;
    }
    return result;
}

bool LoadConfig(const std::string& path, AppConfig& out, std::string& errorMsg) {
    std::ifstream f(path);
    if (!f.is_open()) {
        errorMsg = "No se pudo abrir: " + path;
        return false;
    }

    std::ostringstream ss;
    ss << f.rdbuf();
    std::string json = ss.str();

    std::string wlRaw = ExtractRaw(json, "whitelist");
    out.whitelist = ParseStringArray(wlRaw);

    std::string logRaw = ExtractRaw(json, "logFile");
    std::string dryRaw = ExtractRaw(json, "dryRun");
    std::string diskRaw = ExtractRaw(json, "showDiskNote");

    out.logFile = (logRaw.size() >= 2 && logRaw.front() == '"')
        ? logRaw.substr(1, logRaw.size() - 2)
        : "";
    out.dryRun = ParseBool(dryRaw, false);
    out.showDiskNote = ParseBool(diskRaw, true);

    std::string profilesRaw = ExtractRaw(json, "profiles");
    auto blocks = ExtractProfileBlocks(profilesRaw);

    for (auto& [name, block] : blocks) {
        ProfileConfig pc;
        pc.name = name;

        std::string killRaw = ExtractRaw(block, "extraKill");
        std::string svcRaw = ExtractRaw(block, "stopServices");
        std::string nuclearRaw = ExtractRaw(block, "useWhitelist");

        pc.extraKill = ParseStringArray(killRaw);
        pc.stopServices = ParseStringArray(svcRaw);
        pc.useWhitelist = ParseBool(nuclearRaw, false);

        out.profiles[name] = pc;
    }

    if (out.whitelist.empty()) {
        errorMsg = "Whitelist vacia o JSON malformado";
        return false;
    }

    return true;
}

static std::string EscapeJson(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"')       out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

static std::string ArrayToJson(const std::vector<std::string>& v, int indent) {
    if (v.empty()) return "[]";
    std::string pad(indent, ' ');
    std::string inner(indent + 2, ' ');
    std::string out = "[\n";
    for (size_t i = 0; i < v.size(); i++) {
        out += inner + "\"" + EscapeJson(v[i]) + "\"";
        if (i + 1 < v.size()) out += ",";
        out += "\n";
    }
    out += pad + "]";
    return out;
}

bool SaveConfig(const std::string& path, const AppConfig& cfg, std::string& errorMsg) {
    std::ofstream f(path);
    if (!f.is_open()) {
        errorMsg = "No se pudo escribir: " + path;
        return false;
    }

    f << "{\n";
    f << "  \"logFile\": \"" << EscapeJson(cfg.logFile) << "\",\n";
    f << "  \"dryRun\": " << (cfg.dryRun ? "true" : "false") << ",\n";
    f << "  \"showDiskNote\": " << (cfg.showDiskNote ? "true" : "false") << ",\n";
    f << "\n";

    f << "  \"whitelist\": " << ArrayToJson(cfg.whitelist, 2) << ",\n";
    f << "\n";

    f << "  \"profiles\": {\n";
    size_t pi = 0;
    for (auto& [name, pc] : cfg.profiles) {
        f << "    \"" << EscapeJson(name) << "\": {\n";
        f << "      \"useWhitelist\": " << (pc.useWhitelist ? "true" : "false") << ",\n";
        f << "      \"extraKill\": " << ArrayToJson(pc.extraKill, 6) << ",\n";
        f << "      \"stopServices\": " << ArrayToJson(pc.stopServices, 6) << "\n";
        f << "    }";
        if (++pi < cfg.profiles.size()) f << ",";
        f << "\n";
    }
    f << "  }\n";
    f << "}\n";

    return true;
}

bool CreateDefaultConfig(const std::string& path, std::string& errorMsg) {
    AppConfig cfg;

    cfg.logFile = "WannaClean_log.txt";
    cfg.dryRun = false;
    cfg.showDiskNote = true;

    // Procesos esenciales del sistema para evitar BSOD o bloqueos
    cfg.whitelist = {
        "system", "idle", "registry",
        "smss.exe", "csrss.exe", "wininit.exe",
        "winlogon.exe", "lsass.exe", "services.exe",
        "svchost.exe", "dwm.exe", "explorer.exe",
        "audiodg.exe", "fontdrvhost.exe", "ctfmon.exe",
        "conhost.exe", "taskhostw.exe", "sihost.exe",
        "msmpeng.exe", "securityhealthsystray.exe",
        "securityhealthservice.exe", "runtimebroker.exe",
        "shellexperiencehost.exe", "startmenuexperiencehost.exe",
        "spoolsv.exe", "lsaiso.exe", "memory compression",
        "taskmgr.exe", "wudfhost.exe", "wmiprvse.exe",
        "searchindexer.exe", "sgrmbroker.exe", "WannaClean.exe"
    };

    ProfileConfig gaming;
    gaming.name = "gaming";
    gaming.useWhitelist = false;
    gaming.extraKill = {
        "discord.exe", "chrome.exe", "msedge.exe", "firefox.exe",
        "brave.exe", "opera.exe", "spotify.exe", "slack.exe",
        "teams.exe", "zoom.exe", "webex.exe", "skype.exe",
        "onedrive.exe", "googledrivesync.exe", "dropbox.exe",
        "widgets.exe", "searchhost.exe", "musnotifyicon.exe",
        "asuslinkremote.exe", "asuslinknear.exe", "vlc.exe",
        "anydesk.exe", "teamviewer.exe", "cortana.exe"
    };
    gaming.stopServices = {
        "WSearch", "DiagTrack", "SysMain",
        "XblAuthManager", "XblGameSave", "XboxNetApiSvc",
        "wuauserv", "BITS"
    };
    cfg.profiles["gaming"] = gaming;

    ProfileConfig work;
    work.name = "work";
    work.useWhitelist = false;
    work.extraKill = {
        "steam.exe", "steamwebhelper.exe",
        "epicgameslauncher.exe", "epicwebhelper.exe",
        "battle.net.exe", "agent.exe", "upc.exe",
        "origin.exe", "originwebhelperservice.exe",
        "eadesktop.exe", "eabackgroundservice.exe",
        "riotclientservices.exe", "valorant.exe", "leagueclient.exe",
        "galaxyclient.exe", "galaxyclientservice.exe",
        "discord.exe", "spotify.exe", "twitch.exe", "overwolf.exe",
        "xboxgamebarwidgets.exe", "gamebarpresencewriter.exe",
        "wallpaper64.exe", "wallpaper32.exe", "translucenttb.exe",
        "geforceexperience.exe", "nvsphelper64.exe"
    };
    work.stopServices = {
        "XblGameSave", "XboxNetApiSvc", "XboxGipSvc",
        "DiagTrack", "WSearch", "Origin Client Service",
        "Steam Client Service", "EpicOnlineServices"
    };
    cfg.profiles["work"] = work;

    ProfileConfig nuclear;
    nuclear.name = "nuclear";
    nuclear.useWhitelist = true;
    nuclear.extraKill = {};
    nuclear.stopServices = {
        "WSearch", "DiagTrack", "SysMain",
        "XblAuthManager", "XblGameSave", "XboxNetApiSvc",
        "spacedesk", "ollama", "AsusAppService",
        "AsusSA", "TabletInputService", "AdobeARMservice",
        "wuauserv", "BITS", "Spooler", "DPS"
    };
    cfg.profiles["nuclear"] = nuclear;

    return SaveConfig(path, cfg, errorMsg);
}

const ProfileConfig* GetProfile(const AppConfig& cfg, const std::string& name) {
    auto it = cfg.profiles.find(name);
    return (it != cfg.profiles.end()) ? &it->second : nullptr;
}