#include <windows.h>
#include <windowsx.h>
#include <processthreadsapi.h>
#include <process.h>
#include <d3d11.h>
#include <dxgi.h>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <cstdint>
#if !defined(_BEGINTHREADEX_DECLARED)
extern "C" uintptr_t __cdecl _beginthreadex(void*, unsigned,
    unsigned(__stdcall*)(void*), void*, unsigned, unsigned*);
namespace std { using ::_beginthreadex; }
#define _BEGINTHREADEX_DECLARED
#endif
#include <thread>
#include <mutex>
#include <atomic>

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"

#include "ui_result.h"
#include "config.h"
#include "profiles.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device* g_rDev = nullptr;
static ID3D11DeviceContext* g_rCtx = nullptr;
static IDXGISwapChain* g_rSwap = nullptr;
static ID3D11RenderTargetView* g_rRTV = nullptr;
static HWND                    g_rHwnd = nullptr;

static void R_CreateRTV() {
    ID3D11Texture2D* bb = nullptr;
    g_rSwap->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (bb) { g_rDev->CreateRenderTargetView(bb, nullptr, &g_rRTV); bb->Release(); }
}

static void R_CleanRTV() {
    if (g_rRTV) { g_rRTV->Release(); g_rRTV = nullptr; }
}

static bool R_CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fls, 2, D3D11_SDK_VERSION, &sd, &g_rSwap, &g_rDev, &fl, &g_rCtx)))
        return false;

    R_CreateRTV();
    return true;
}

static void R_Cleanup() {
    R_CleanRTV();
    if (g_rSwap) { g_rSwap->Release(); g_rSwap = nullptr; }
    if (g_rCtx) { g_rCtx->Release();  g_rCtx = nullptr; }
    if (g_rDev) { g_rDev->Release();  g_rDev = nullptr; }
}

struct LogLine {
    int         type; // 0=info, 1=ok, 2=warn, 3=err
    std::string time;
    std::string text;
};

struct ProcBar {
    std::string name;
    float       pct;
    ImU32       color;
};

struct ResultWinState {
    std::string   profileName;
    AppConfig     cfg;
    ProfileResult result;

    std::atomic<bool>  running{ true };
    std::atomic<bool>  done{ false };
    std::atomic<int>   progress{ 0 };

    std::mutex           logMtx;
    std::vector<LogLine> logLines;
    bool                 logScrollBottom = false;

    std::vector<ProcBar> procBars;

    HANDLE hThread = nullptr;
};

static ResultWinState* g_rws = nullptr;
static bool            g_closeRequested = false;

static DWORD WINAPI ProfileThread(LPVOID param) {
    ResultWinState* s = (ResultWinState*)param;

    auto cb = [s](const std::string& msg, int pct) {
        std::string out = msg;
        auto rep = [&](const std::string& f, const std::string& t) {
            size_t p = out.find(f);
            while (p != std::string::npos) {
                out.replace(p, f.size(), t);
                p = out.find(t, p + t.size());
            }
            };

        rep("procesos encontrados", "processes found");
        rep("procesos eliminados", "processes terminated");
        rep("protegidos", "protected");
        rep("no corria", "not running");
        rep("ya estaba detenido", "already stopped");
        rep("Deteniendo servicios", "Stopping services");
        rep("Limpiando cache de memoria", "Cleaning memory cache");
        rep("Tomando snapshot", "Taking snapshot");
        rep("Snapshot listo", "Snapshot ready");
        rep("Snapshot final listo", "Final snapshot ready");
        rep("matados", "killed");
        rep("servicios detenidos", "services stopped");
        rep("GB RAM liberados", "GB RAM freed");
        rep("FALLOS", "FAILURES");
        rep("Guardado en", "Saved to");
        rep("no encontrado", "not found");
        rep("sin acceso", "access denied");
        rep("Servicio", "Service");
        rep("Modo NUCLEAR", "NUCLEAR mode");
        rep("whitelist pura activada", "pure whitelist activated");
        rep("Escaneando procesos activos", "Scanning active processes");
        rep("simulado", "simulated");

        int type = 0;
        if (out.find("[KILL]") != std::string::npos ||
            out.find("[STOP]") != std::string::npos ||
            out.find("[RAM]") != std::string::npos ||
            out.find("[DONE]") != std::string::npos ||
            out.find("[SNAP]") != std::string::npos) type = 1;
        else if (out.find("[SKIP]") != std::string::npos ||
            out.find("[DRY]") != std::string::npos) type = 2;
        else if (out.find("ERROR") != std::string::npos ||
            out.find("FAIL") != std::string::npos ||
            out.find("FALLO") != std::string::npos) type = 3;

        SYSTEMTIME st; GetLocalTime(&st);
        char ts[12]; sprintf_s(ts, "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);

        std::lock_guard<std::mutex> lk(s->logMtx);
        s->logLines.push_back({ type, ts, out });
        s->logScrollBottom = true;
        s->progress = pct;
        };

    s->result = RunProfile(s->profileName, s->cfg, cb);

    const ImU32 palette[] = {
        IM_COL32(255,51,0,255),  IM_COL32(255,102,0,255),
        IM_COL32(255,136,0,255), IM_COL32(255,170,0,255),
        IM_COL32(255,204,0,255), IM_COL32(221,204,0,255)
    };

    auto it = s->cfg.profiles.find(s->profileName);
    if (it != s->cfg.profiles.end()) {
        const auto& kills = it->second.extraKill;
        int   n = (int)std::min(kills.size(), size_t(6));
        float pcts[] = { 34,22,18,11,8,5 };
        for (int i = 0; i < n; i++)
            s->procBars.push_back({ kills[i], pcts[i], palette[i % 6] });
    }

    s->running = false;
    s->done = true;
    s->progress = 100;

    return 0;
}

static void SectionTitle(const char* t) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float  w = ImGui::GetContentRegionAvail().x;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 18), IM_COL32(90, 0, 0, 255));
    dl->AddRectFilled(p, ImVec2(p.x + 3, p.y + 18), IM_COL32(255, 68, 68, 255));
    dl->AddText(nullptr, 11.f, ImVec2(p.x + 8, p.y + 4), IM_COL32(255, 204, 204, 255), t);
    ImGui::Dummy(ImVec2(w, 18));
}

static void Separator98() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float  w = ImGui::GetContentRegionAvail().x;
    dl->AddLine(p, ImVec2(p.x + w, p.y), IM_COL32(85, 0, 0, 255));
    ImGui::Dummy(ImVec2(w, 2));
}

static void DrawRing(ImDrawList* dl, ImVec2 center, float r, float thick, float pct, ImU32 fg) {
    const float PI = 3.14159265f;
    dl->AddCircle(center, r, IM_COL32(51, 0, 0, 255), 64, thick);
    if (pct > 0) {
        float sa = -PI / 2.f, ea = sa + 2 * PI * std::min(pct, 1.f);
        dl->PathArcTo(center, r, sa, ea, 64);
        dl->PathStroke(fg, false, thick);
    }
}

static void SummaryCard(const char* id, const char* val, const char* label, ImU32 valColor) {
    float fullW = ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x;
    float cardW = (fullW - 12.f) / 4.f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0, 0, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.f);
    ImGui::BeginChild(id, ImVec2(cardW, 32), true, ImGuiWindowFlags_NoScrollbar);

    float r = ((valColor >> 0) & 0xFF) / 255.f;
    float g = ((valColor >> 8) & 0xFF) / 255.f;
    float b = ((valColor >> 16) & 0xFF) / 255.f;

    ImGui::SetCursorPos(ImVec2(8, 7));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(r, g, b, 1));
    ImGui::Text("%s", val);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 4);
}

static void MetricCard(const char* id, const char* label, const char* vBefore, float pBefore, ImU32 cBefore, const char* vAfter, float pAfter, ImU32 cAfter, const char* delta, ImU32 cDelta, const char* subNote = nullptr) {
    float fullW = ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x;
    float cardW = (fullW - 9.f) / 4.f;
    float cardH = 170.f;
    float rR = 20.f, thick = 5.f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0, 0, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.f);
    ImGui::BeginChild(id, ImVec2(cardW, cardH), true, ImGuiWindowFlags_NoScrollbar);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2      orig = ImGui::GetWindowPos();
    float       cx = orig.x + cardW * 0.5f;
    ImFont* font = ImGui::GetFont();

    ImVec2 ts = ImGui::CalcTextSize(label);
    ImGui::SetCursorPosX((cardW - ts.x) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.53f, 0.53f, 0.53f, 1));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    float cy = orig.y + ImGui::GetCursorPosY();
    ImVec2 bc = ImVec2(cx, cy + rR + 4);
    DrawRing(dl, bc, rR, thick, pBefore, cBefore);
    ts = font->CalcTextSizeA(13.f, FLT_MAX, 0.0f, vBefore);
    dl->AddText(nullptr, 13.f, ImVec2(bc.x - ts.x * 0.5f, bc.y - 11.f), cBefore, vBefore);

    {
        const char* lbl = "BEFORE";
        ImVec2 ls = font->CalcTextSizeA(8.f, FLT_MAX, 0.0f, lbl);
        dl->AddText(nullptr, 8.f, ImVec2(bc.x - ls.x * 0.5f, bc.y + 3.f), IM_COL32(100, 100, 100, 255), lbl);
    }
    ImGui::Dummy(ImVec2(cardW, rR * 2 + 2));

    ts = ImGui::CalcTextSize(delta);
    ImGui::SetCursorPosX((cardW - ts.x) * 0.5f);
    {
        float dr = ((cDelta >> 0) & 0xFF) / 255.f;
        float dg = ((cDelta >> 8) & 0xFF) / 255.f;
        float db = ((cDelta >> 16) & 0xFF) / 255.f;
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(dr, dg, db, 1));
        ImGui::SetWindowFontScale(1.5f);
        ImGui::TextUnformatted(delta);
        ImGui::SetWindowFontScale(1.f);
        ImGui::PopStyleColor();
    }

    if (subNote) {
        ts = ImGui::CalcTextSize(subNote);
        ImGui::SetCursorPosX((cardW - ts.x) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.4f, 1));
        ImGui::TextUnformatted(subNote);
        ImGui::PopStyleColor();
    }
    else {
        ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
    }

    cy = orig.y + ImGui::GetCursorPosY();
    ImVec2 ac = ImVec2(cx, cy + rR + 4);
    DrawRing(dl, ac, rR, thick, pAfter, cAfter);
    ts = font->CalcTextSizeA(13.f, FLT_MAX, 0.0f, vAfter);
    dl->AddText(nullptr, 13.f, ImVec2(ac.x - ts.x * 0.5f, ac.y - 11.f), cAfter, vAfter);

    {
        const char* lbl = "AFTER";
        ImVec2 ls = font->CalcTextSizeA(8.f, FLT_MAX, 0.0f, lbl);
        dl->AddText(nullptr, 8.f, ImVec2(ac.x - ls.x * 0.5f, ac.y + 3.f), IM_COL32(100, 100, 100, 255), lbl);
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 3);
}

static void Draw3DRect(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
    dl->AddLine(p0, ImVec2(p1.x, p0.y), IM_COL32(255, 255, 255, 255));
    dl->AddLine(p0, ImVec2(p0.x, p1.y), IM_COL32(255, 255, 255, 255));
    dl->AddLine(ImVec2(p1.x, p0.y), p1, IM_COL32(128, 128, 128, 255));
    dl->AddLine(ImVec2(p0.x, p1.y), p1, IM_COL32(128, 128, 128, 255));
}

static void DrawTitlebarButtons(ImDrawList* dl, float winX, float winY, float winW, HWND hwnd, bool running) {
    float bx = winX + winW - 56;
    const char* lbl[] = { "_", "o", "x" };

    for (int i = 0; i < 3; i++) {
        ImVec2 b0 = ImVec2(bx + i * 18, winY + 5);
        ImVec2 b1 = ImVec2(b0.x + 16, b0.y + 14);
        dl->AddRectFilled(b0, b1, IM_COL32(192, 192, 192, 255));
        Draw3DRect(dl, b0, b1);
        ImVec2 ts = ImGui::CalcTextSize(lbl[i]);
        dl->AddText(nullptr, 10.f, ImVec2(b0.x + (16 - ts.x) * 0.5f, b0.y + (14 - ts.y) * 0.5f), IM_COL32(0, 0, 0, 255), lbl[i]);
    }

    ImGui::SetCursorScreenPos(ImVec2(bx, winY + 5));
    if (ImGui::InvisibleButton("##r_min", ImVec2(16, 14)))
        ShowWindow(hwnd, SW_MINIMIZE);

    ImGui::SetCursorScreenPos(ImVec2(bx + 18, winY + 5));
    if (ImGui::InvisibleButton("##r_max", ImVec2(16, 14)))
        ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);

    ImGui::SetCursorScreenPos(ImVec2(bx + 36, winY + 5));
    if (ImGui::InvisibleButton("##r_cls", ImVec2(16, 14))) {
        if (running) {
            int r = MessageBoxA(hwnd, "Profile is still running.\nForce close?", "Warning", MB_YESNO | MB_ICONWARNING);
            if (r == IDYES) g_closeRequested = true;
        }
        else {
            g_closeRequested = true;
        }
    }
}

static void RenderResultFrame() {
    if (!g_rws) return;
    ResultWinState& s = *g_rws;
    bool            done = s.done.load();
    bool            running = s.running.load();
    int             pct = s.progress.load();

    ImGuiIO& io = ImGui::GetIO();
    float W = io.DisplaySize.x;
    float H = io.DisplaySize.y;

    const float TITLE_H = 26.f;
    const float BOTTOM_H = 40.f;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##result", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus);

    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();

        dl->AddRectFilledMultiColor(
            wp, ImVec2(wp.x + W, wp.y + TITLE_H),
            IM_COL32(204, 0, 0, 255), IM_COL32(119, 0, 0, 255),
            IM_COL32(119, 0, 0, 255), IM_COL32(204, 0, 0, 255));

        std::string title = "  WannaClean";
        if (s.cfg.dryRun) title += "  [DRY RUN]";
        dl->AddText(nullptr, 12.f, ImVec2(wp.x + 8, wp.y + 7), IM_COL32(255, 255, 255, 200), title.c_str());

        const char* h1 = done ? "Optimization complete!" : "Running profile...";
        ImVec2 ts = ImGui::CalcTextSize(h1);
        dl->AddText(nullptr, 14.f, ImVec2(wp.x + (W - ts.x) * 0.5f, wp.y + 5), IM_COL32(255, 255, 255, 255), h1);

        DrawTitlebarButtons(dl, wp.x, wp.y, W, g_rHwnd, running);
        ImGui::Dummy(ImVec2(W, TITLE_H));
    }

    float contentTop = ImGui::GetCursorPosY();
    float contentH = H - contentTop - BOTTOM_H - 4;

    ImGui::BeginChild("##scroll", ImVec2(-1, contentH), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_HorizontalScrollbar);

    if (pct < 100) {
        SectionTitle("* Progress");
        ImGui::Spacing();
        {
            ImU32       barCol = done ? IM_COL32(0, 204, 68, 255) : IM_COL32(255, 34, 0, 255);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2      p = ImGui::GetCursorScreenPos();
            float       w = ImGui::GetContentRegionAvail().x;
            float       bh = 14.f;

            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + bh), IM_COL32(33, 0, 0, 255));
            dl->AddRect(p, ImVec2(p.x + w, p.y + bh), IM_COL32(85, 0, 0, 255));
            float filled = (w - 2) * std::clamp(pct / 100.f, 0.f, 1.f);
            if (filled > 0)
                dl->AddRectFilled(ImVec2(p.x + 1, p.y + 1), ImVec2(p.x + 1 + filled, p.y + bh - 1), barCol);

            char ps[8]; sprintf_s(ps, "%d%%", pct);
            ImVec2 ts = ImGui::CalcTextSize(ps);
            dl->AddText(nullptr, 10.f, ImVec2(p.x + (w - ts.x) * 0.5f, p.y + 2), IM_COL32(255, 255, 255, 180), ps);
            ImGui::Dummy(ImVec2(w, bh + 2));
        }
        ImGui::Spacing();
        Separator98();
    }

    SectionTitle("* Summary");
    ImGui::Spacing();
    {
        char buf[32];
        sprintf_s(buf, "%d", done ? s.result.kills.killed : 0);
        SummaryCard("##sk", buf, "killed", IM_COL32(255, 68, 0, 255));

        sprintf_s(buf, "%d", done ? s.result.servicesStopped : 0);
        SummaryCard("##ss", buf, "stopped", IM_COL32(255, 170, 0, 255));

        double freed = done ? std::max(0.0, -s.result.diff.ramDeltaGB) : 0.0;
        sprintf_s(buf, "%.1f GB", freed);
        SummaryCard("##sr", buf, "RAM freed", IM_COL32(0, 255, 102, 255));

        int failed = done ? (int)s.result.kills.failed.size() + s.result.servicesFailed : 0;
        sprintf_s(buf, "%d", failed);
        SummaryCard("##sf", buf, "failed / skipped", failed > 0 ? IM_COL32(255, 68, 0, 255) : IM_COL32(100, 100, 100, 255));

        ImGui::NewLine();
    }

    SectionTitle("* Resource delta -- Before vs After");
    ImGui::Spacing();
    {
        auto& bef = s.result.before;
        auto& aft = s.result.after;
        bool  hasBef = bef.ram.ok;
        bool  hasAft = aft.ram.ok && done;

        ImU32 RED = IM_COL32(255, 51, 0, 255);
        ImU32 ORG = IM_COL32(255, 136, 0, 255);
        ImU32 GRN = IM_COL32(0, 204, 68, 255);

        {
            char vb[16], va[16], dv[32], sub[32];
            float pb = hasBef ? (float)(bef.ram.usedPct / 100.0) : 0;
            float pa = hasAft ? (float)(aft.ram.usedPct / 100.0) : 0;
            sprintf_s(vb, "%.0f%%", hasBef ? bef.ram.usedPct : 0.0);
            sprintf_s(va, "%.0f%%", hasAft ? aft.ram.usedPct : 0.0);
            double d = done ? s.result.diff.ramDeltaGB : 0.0;
            ImU32 dc = d < 0 ? GRN : RED;
            sprintf_s(dv, d < 0 ? "v %.1f GB" : "^ %.1f GB", std::abs(d));
            sprintf_s(sub, "%.1f->%.1f GB", hasBef ? bef.ram.usedGB : 0.0, hasAft ? aft.ram.usedGB : 0.0);
            MetricCard("##rmc", "RAM USED", vb, pb, RED, hasAft ? va : "--", pa, GRN, hasAft ? dv : "...", dc, hasAft ? sub : nullptr);
        }

        {
            char vb[16], va[16], dv[32], sub[32];
            float pb = hasBef ? (float)(bef.cpu.usedPct / 100.0) : 0;
            float pa = hasAft ? (float)(aft.cpu.usedPct / 100.0) : 0;
            sprintf_s(vb, "%.0f%%", hasBef ? bef.cpu.usedPct : 0.0);
            sprintf_s(va, "%.0f%%", hasAft ? aft.cpu.usedPct : 0.0);
            double d = done ? s.result.diff.cpuDelta : 0.0;
            ImU32 dc = d < 0 ? GRN : RED;
            sprintf_s(dv, d < 0 ? "v %.0f%%" : "^ %.0f%%", std::abs(d));
            sprintf_s(sub, "%.0f%%->%.0f%%", hasBef ? bef.cpu.usedPct : 0.0, hasAft ? aft.cpu.usedPct : 0.0);
            MetricCard("##cmc", "CPU LOAD", vb, pb, RED, hasAft ? va : "--", pa, GRN, hasAft ? dv : "...", dc, hasAft ? sub : nullptr);
        }

        {
            char vb[16], va[16], dv[32], sub[32];
            bool  gpuOk = bef.gpu.ok && bef.gpu.hasUsage;
            float pb = gpuOk ? (float)(bef.gpu.vramUsedPct / 100.0) : 0;
            float pa = (hasAft && aft.gpu.ok && aft.gpu.hasUsage) ? (float)(aft.gpu.vramUsedPct / 100.0) : 0;
            sprintf_s(vb, gpuOk ? "%.0f%%" : "N/A", gpuOk ? bef.gpu.vramUsedPct : 0.0);
            sprintf_s(va, (hasAft && aft.gpu.ok && aft.gpu.hasUsage) ? "%.0f%%" : "--", (hasAft && aft.gpu.ok) ? aft.gpu.vramUsedPct : 0.0);
            double d = done ? s.result.diff.vramDeltaGB : 0.0;
            ImU32 dc = d < 0 ? GRN : ORG;
            sprintf_s(dv, d < 0 ? "v %.1f GB" : "^ %.1f GB", std::abs(d));
            sprintf_s(sub, "%.1f->%.1f GB", gpuOk ? bef.gpu.vramUsedGB : 0.0, (hasAft && aft.gpu.ok) ? aft.gpu.vramUsedGB : 0.0);
            MetricCard("##gmc", "GPU VRAM", vb, pb, ORG, hasAft ? va : "--", pa, GRN, hasAft ? dv : "...", dc, hasAft ? sub : nullptr);
        }

        {
            char vb[16], va[16], dv[32];
            float pb = hasBef ? std::min((float)(bef.disk.activityMBs / 200.f), 1.f) : 0;
            float pa = hasAft ? std::min((float)(aft.disk.activityMBs / 200.f), 1.f) : 0;
            sprintf_s(vb, hasBef ? "%.0f" : "--", bef.disk.activityMBs);
            sprintf_s(va, hasAft ? "%.0f" : "--", aft.disk.activityMBs);
            double d = done ? s.result.diff.diskDeltaMBs : 0.0;
            sprintf_s(dv, d > 0 ? "^ %.0f MB/s" : "v %.0f MB/s", std::abs(d));
            MetricCard("##dmc", "DISK I/O", vb, pb, ORG, hasAft ? va : "--", pa, ORG, hasAft ? dv : "...", ORG, d > 0 ? "normal: cache flush" : nullptr);
        }
        ImGui::NewLine();
    }

    ImGui::Spacing();
    Separator98();

    if (done && !s.procBars.empty()) {
        SectionTitle("* Top processes killed  (% of CPU consumed before termination)");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0, 0, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.f);
        float procH = (float)s.procBars.size() * 22 + 8;
        ImGui::BeginChild("##procs", ImVec2(-1, procH), true);

        for (auto& pb : s.procBars) {
            float bw = ImGui::GetContentRegionAvail().x;
            std::string name = pb.name;
            if (name.size() > 22) name = name.substr(0, 21) + ".";
            float r = ((pb.color >> 0) & 0xFF) / 255.f;
            float g = ((pb.color >> 8) & 0xFF) / 255.f;
            float b = ((pb.color >> 16) & 0xFF) / 255.f;

            char pctStr[8]; sprintf_s(pctStr, "%.0f%%", pb.pct);
            std::string rowLabel = name + "  " + pctStr;

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(r, g, b, 1));
            ImGui::TextUnformatted(rowLabel.c_str());
            ImGui::PopStyleColor();

            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2      p = ImGui::GetCursorScreenPos();
            float       bh = 8.f;
            dl->AddRectFilled(p, ImVec2(p.x + bw, p.y + bh), IM_COL32(33, 0, 0, 255));
            dl->AddRect(p, ImVec2(p.x + bw, p.y + bh), IM_COL32(68, 0, 0, 255));
            float filled = (bw - 2) * (pb.pct / 100.f);
            if (filled > 0)
                dl->AddRectFilled(ImVec2(p.x + 1, p.y + 1), ImVec2(p.x + 1 + filled, p.y + bh - 1), pb.color);
            ImGui::Dummy(ImVec2(bw, bh + 1));
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::Spacing();
        Separator98();
    }

    if (done && s.result.servicesFailed > 0) {
        SectionTitle("* Services failed / skipped");
        ImGui::Spacing();
        for (auto& sr : s.result.serviceResults) {
            if (!sr.success) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.67f, 0, 1));
                ImGui::Text("  %s", sr.name.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.4f, 1));
                ImGui::Text("-- %s", sr.reason.c_str());
                ImGui::PopStyleColor();
            }
        }
        ImGui::Spacing();
        Separator98();
    }

    SectionTitle("* Operation log");
    ImGui::Spacing();
    {
        float logH = contentH - ImGui::GetCursorPosY() - 10;
        if (logH < 80) logH = 80;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 1));
        ImGui::BeginChild("##log", ImVec2(-1, logH), false, ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::lock_guard<std::mutex> lk(s.logMtx);
            for (auto& ln : s.logLines) {
                ImVec4 col;
                switch (ln.type) {
                case 1:  col = ImVec4(0, 0.8f, 0.27f, 1); break;
                case 2:  col = ImVec4(1, 0.67f, 0, 1);    break;
                case 3:  col = ImVec4(1, 0.20f, 0, 1);    break;
                default: col = ImVec4(0.27f, 0.6f, 1, 1); break;
                }
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.4f, 1));
                ImGui::Text("%s", ln.time.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine(0, 6);
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::TextUnformatted(ln.text.c_str());
                ImGui::PopStyleColor();
            }
            if (s.logScrollBottom) {
                ImGui::SetScrollHereY(1.f);
                s.logScrollBottom = false;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();

    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2      wp = ImGui::GetWindowPos();
        float       bbY = H - BOTTOM_H;
        float       bbScrY = wp.y + bbY;

        dl->AddRectFilled(ImVec2(wp.x, bbScrY), ImVec2(wp.x + W, wp.y + H), IM_COL32(192, 192, 192, 255));
        dl->AddLine(ImVec2(wp.x, bbScrY), ImVec2(wp.x + W, bbScrY), IM_COL32(128, 128, 128, 255));
        dl->AddLine(ImVec2(wp.x, bbScrY + 1), ImVec2(wp.x + W, bbScrY + 1), IM_COL32(255, 255, 255, 255));

        ImGui::SetCursorPos(ImVec2(8, bbY + 6));

        ImGui::PushStyleColor(ImGuiCol_Text, done ? ImVec4(0, 0.5f, 0, 1) : ImVec4(0.8f, 0.5f, 0, 1));
        ImGui::TextUnformatted(done ? "v DONE" : "* RUNNING");
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 8);

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 1));
        ImGui::BeginChild("##logbar", ImVec2(W - 280, 22), false, ImGuiWindowFlags_NoScrollbar);
        {
            static bool  blk = true;
            static DWORD lastBlk = 0;
            DWORD now = GetTickCount();
            if (now - lastBlk > 600) { blk = !blk; lastBlk = now; }

            SYSTEMTIME st; GetLocalTime(&st);
            char clk[12]; sprintf_s(clk, "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
            std::string modeStr = "Profile: " + s.profileName;
            if (s.cfg.dryRun) modeStr += " [DRY RUN]";

            char msg[160];
            sprintf_s(msg, "%s %s  |  %s", running && blk ? "*" : " ", clk, modeStr.c_str());

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0.8f, 0.27f, 1));
            ImGui::TextUnformatted(msg);
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 6);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.75f, 0.75f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.85f, 0.85f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.60f, 0.60f, 0.60f, 1));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 1));

        if (ImGui::Button("< Back", ImVec2(60, 22)))
            g_closeRequested = true;
        ImGui::SameLine(0, 4);
        if (ImGui::Button("Close", ImVec2(60, 22))) {
            if (running) {
                int r = MessageBoxA(nullptr, "Profile is still running.\nForce close?", "Warning", MB_YESNO | MB_ICONWARNING);
                if (r == IDYES) g_closeRequested = true;
            }
            else {
                g_closeRequested = true;
            }
        }
        ImGui::PopStyleColor(4);
    }

    ImGui::End();
}

static LRESULT CALLBACK ResultWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (g_rDev && wParam != SIZE_MINIMIZED) {
            R_CleanRTV();
            g_rSwap->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            R_CreateRTV();
        }
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = 840;
        mmi->ptMinTrackSize.y = 680;
        return 0;
    }

    case WM_NCHITTEST: {
        LRESULT def = DefWindowProcA(hwnd, msg, wParam, lParam);
        if (def == HTCLIENT) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            RECT  rc; GetClientRect(hwnd, &rc);
            MapWindowPoints(hwnd, nullptr, (POINT*)&rc, 2);
            int relX = pt.x - rc.left;
            int relY = pt.y - rc.top;
            if (relY < 45 && relX < (rc.right - rc.left) - 60)
                return HTCAPTION;
        }
        return def;
    }

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;

    case WM_CLOSE:
        if (g_rws && g_rws->running) {
            int r = MessageBoxA(hwnd, "Profile is still running.\nForce close?", "Warning", MB_YESNO | MB_ICONWARNING);
            if (r != IDYES) return 0;
            if (g_rws->hThread) {
                TerminateThread(g_rws->hThread, 0);
                CloseHandle(g_rws->hThread);
                g_rws->hThread = nullptr;
            }
        }
        g_closeRequested = true;
        return 0;

    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void RunResultWindow(HINSTANCE hInstance, const char* profileName, const AppConfig& cfg) {
    ImGuiContext* prevCtx = ImGui::GetCurrentContext();
    ResultWinState state;
    state.profileName = profileName;
    state.cfg = cfg;
    g_rws = &state;
    g_closeRequested = false;

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = ResultWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "BKResult2";
    wc.hIcon = LoadIconA(hInstance, "APP_ICON");
    if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExA(&wc);

    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    int winW = 840, winH = 680;

    g_rHwnd = CreateWindowExA(
        0, "BKResult2",
        "WannaClean -- Check Performance",
        WS_POPUP | WS_VISIBLE | WS_SYSMENU,
        (scrW - winW) / 2, (scrH - winH) / 2,
        winW, winH,
        nullptr, nullptr, hInstance, nullptr);

    if (!R_CreateDevice(g_rHwnd)) {
        R_Cleanup();
        UnregisterClassA("BKResult2", hInstance);
        g_rws = nullptr;
        return;
    }

    ShowWindow(g_rHwnd, SW_SHOW);
    UpdateWindow(g_rHwnd);

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGui::GetIO().IniFilename = nullptr;

    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = st.FrameRounding = 0;
    st.WindowBorderSize = st.FrameBorderSize = 1;
    st.WindowPadding = ImVec2(8, 6);
    st.ItemSpacing = ImVec2(6, 3);

    ImVec4* col = st.Colors;
    col[ImGuiCol_WindowBg] = ImVec4(0.50f, 0, 0, 1);
    col[ImGuiCol_ChildBg] = ImVec4(0.10f, 0, 0, 1);
    col[ImGuiCol_Text] = ImVec4(1, 1, 1, 1);
    col[ImGuiCol_TextDisabled] = ImVec4(0.5f, 0.5f, 0.5f, 1);
    col[ImGuiCol_Border] = ImVec4(0.33f, 0, 0, 1);
    col[ImGuiCol_FrameBg] = ImVec4(0.10f, 0, 0, 1);
    col[ImGuiCol_ScrollbarBg] = ImVec4(0.05f, 0, 0, 1);
    col[ImGuiCol_ScrollbarGrab] = ImVec4(0.40f, 0, 0, 1);
    col[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.55f, 0, 0, 1);
    col[ImGuiCol_Separator] = ImVec4(0.33f, 0, 0, 1);
    col[ImGuiCol_Button] = ImVec4(0.75f, 0.75f, 0.75f, 1);
    col[ImGuiCol_ButtonHovered] = ImVec4(0.85f, 0.85f, 0.85f, 1);
    col[ImGuiCol_ButtonActive] = ImVec4(0.60f, 0.60f, 0.60f, 1);
    col[ImGuiCol_Header] = ImVec4(0.20f, 0, 0, 1);
    col[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0, 0, 1);
    col[ImGuiCol_TitleBgActive] = ImVec4(0.54f, 0, 0, 1);

    ImGui_ImplWin32_Init(g_rHwnd);
    ImGui_ImplDX11_Init(g_rDev, g_rCtx);

    state.hThread = CreateThread(nullptr, 0, ProfileThread, &state, 0, nullptr);

    const float clr[4] = { 0.50f, 0, 0, 1 };
    while (!g_closeRequested) {
        MSG m;
        while (PeekMessage(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessage(&m);
            if (m.message == WM_QUIT) g_closeRequested = true;
        }
        if (g_closeRequested) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        RenderResultFrame();
        ImGui::Render();
        g_rCtx->OMSetRenderTargets(1, &g_rRTV, nullptr);
        g_rCtx->ClearRenderTargetView(g_rRTV, clr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_rSwap->Present(1, 0);
    }

    if (state.running && state.hThread) {
        state.running = false;
        WaitForSingleObject(state.hThread, 3000);
    }
    if (state.hThread) CloseHandle(state.hThread);

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prevCtx);
    R_Cleanup();
    DestroyWindow(g_rHwnd);
    UnregisterClassA("BKResult2", hInstance);
    g_rws = nullptr;
}