#include <windows.h>
#include <windowsx.h>
#include <processthreadsapi.h>
#include <psapi.h>
#include <process.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>
#include <tchar.h>
#include <string>
#include <sstream>
#include <iomanip>
#include <vector>
#include <cstdint>
#if !defined(_BEGINTHREADEX_DECLARED)
extern "C" uintptr_t __cdecl _beginthreadex(void*, unsigned,
    unsigned(__stdcall*)(void*), void*, unsigned, unsigned*);
namespace std { using ::_beginthreadex; }
#define _BEGINTHREADEX_DECLARED
#endif
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"

#include "config.h"
#include "resources.h"
#include "ui_result.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "psapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static HWND                    g_hwnd = nullptr;

static void M_CreateRTV() {
    ID3D11Texture2D* bb = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (bb) { g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv); bb->Release(); }
}

static void M_CleanRTV() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

static bool M_CreateDevice(HWND hwnd) {
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
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fls, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &fl, &g_ctx)))
        return false;

    M_CreateRTV();
    return true;
}

static void M_Cleanup() {
    M_CleanRTV();
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx) { g_ctx->Release();  g_ctx = nullptr; }
    if (g_dev) { g_dev->Release();  g_dev = nullptr; }
}

static AppConfig        g_cfg;
static bool             g_cfgLoaded = false;
static std::string      g_cfgError;
static bool             g_dryRun = false;
static std::string      g_pendingProfile;
static bool             g_launchResult = false;
static bool             g_nuclearConfirm = false;

static SystemSnapshot    g_snap;
static std::mutex        g_snapMtx;
static std::atomic<bool> g_snapBusy{ false };

static DWORD   g_countdownStart = 0;
static int     g_countdownSecs = 5 * 60;

static float   g_cpuHistory[60] = {};
static float   g_ramHistory[60] = {};
static float   g_gpuHistory[60] = {};
static float   g_diskHistory[60] = {};
static int     g_histIdx = 0;

static void ApplyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = s.FrameRounding = s.ScrollbarRounding = 0;
    s.GrabRounding = s.TabRounding = s.ChildRounding = s.PopupRounding = 0;
    s.WindowBorderSize = s.FrameBorderSize = s.ChildBorderSize = 1;
    s.WindowPadding = ImVec2(8, 8);
    s.FramePadding = ImVec2(6, 3);
    s.ItemSpacing = ImVec2(6, 4);
    s.ScrollbarSize = 10;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.54f, 0, 0, 1);
    c[ImGuiCol_ChildBg] = ImVec4(0.10f, 0, 0, 1);
    c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0, 0, 0.97f);
    c[ImGuiCol_FrameBg] = ImVec4(0.10f, 0, 0, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0, 0, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.05f, 0, 0, 1);
    c[ImGuiCol_TitleBg] = ImVec4(0.54f, 0, 0, 1);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.80f, 0, 0, 1);
    c[ImGuiCol_Border] = ImVec4(0.33f, 0, 0, 1);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Button] = ImVec4(0.75f, 0.75f, 0.75f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.85f, 0.85f, 0.85f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.60f, 0.60f, 0.60f, 1);
    c[ImGuiCol_Header] = ImVec4(0.20f, 0, 0, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0, 0, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.10f, 0, 0, 1);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.05f, 0, 0, 1);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.40f, 0, 0, 1);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.55f, 0, 0, 1);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.70f, 0, 0, 1);
    c[ImGuiCol_Separator] = ImVec4(0.40f, 0, 0, 1);
    c[ImGuiCol_CheckMark] = ImVec4(0, 1, 0.4f, 1);
    c[ImGuiCol_Text] = ImVec4(1, 1, 1, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.5f, 0.5f, 0.5f, 1);
    c[ImGuiCol_PlotHistogram] = ImVec4(0, 1, 0.4f, 1);
    c[ImGuiCol_PlotLines] = ImVec4(1, 0.2f, 0, 1);
}

static void Draw3DRect(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool pressed = false) {
    ImU32 hi = pressed ? IM_COL32(128, 128, 128, 255) : IM_COL32(255, 255, 255, 255);
    ImU32 lo = pressed ? IM_COL32(255, 255, 255, 255) : IM_COL32(128, 128, 128, 255);
    dl->AddLine(p0, ImVec2(p1.x, p0.y), hi);
    dl->AddLine(p0, ImVec2(p0.x, p1.y), hi);
    dl->AddLine(ImVec2(p1.x, p0.y), p1, lo);
    dl->AddLine(ImVec2(p0.x, p1.y), p1, lo);
}

static void SectionHeader(const char* t) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float  w = ImGui::GetContentRegionAvail().x;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 18), IM_COL32(90, 0, 0, 255));
    dl->AddRectFilled(p, ImVec2(p.x + 3, p.y + 18), IM_COL32(255, 68, 68, 255));
    dl->AddText(nullptr, 11.f, ImVec2(p.x + 8, p.y + 4), IM_COL32(255, 204, 204, 255), t);
    ImGui::Dummy(ImVec2(w, 18));
}

static void MiniPlot(const char* id, const float* hist, int count, float minV, float maxV, ImU32 color, float h = 28) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float  w = ImGui::GetContentRegionAvail().x;

    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(10, 0, 0, 255));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(55, 0, 0, 255));

    float range = maxV - minV;
    if (range < 1) range = 1;

    for (int i = 1; i < count; i++) {
        float x0 = p.x + (float)(i - 1) / (count - 1) * w;
        float x1 = p.x + (float)i / (count - 1) * w;
        float y0 = p.y + h - ((hist[(g_histIdx + i - 1 + count) % count] - minV) / range) * (h - 2);
        float y1 = p.y + h - ((hist[(g_histIdx + i + count) % count] - minV) / range) * (h - 2);
        dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), color, 1.2f);
    }
    ImGui::Dummy(ImVec2(w, h));
}

static void ColorBar(float pct, ImU32 col, float h = 7) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float  w = ImGui::GetContentRegionAvail().x;

    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(33, 0, 0, 255));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(85, 0, 0, 255));

    float filled = (w - 2) * std::clamp(pct, 0.f, 1.f);
    if (filled > 0)
        dl->AddRectFilled(ImVec2(p.x + 1, p.y + 1), ImVec2(p.x + 1 + filled, p.y + h - 1), col);

    ImGui::Dummy(ImVec2(w, h + 2));
}

static void TelemetryRow(const char* label, const char* val, float pct, ImU32 colBar, const float* hist, int histCount, float hMin, float hMax, const char* sub1 = nullptr, const char* sub2 = nullptr) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.53f, 0.53f, 0.53f, 1));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    ImGui::SameLine(52);
    float r = ((colBar >> 0) & 0xFF) / 255.f;
    float g = ((colBar >> 8) & 0xFF) / 255.f;
    float b = ((colBar >> 16) & 0xFF) / 255.f;

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(r, g, b, 1));
    ImGui::SetWindowFontScale(1.15f);
    ImGui::TextUnformatted(val);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();

    if (sub1 && sub1[0]) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.40f, 0.40f, 1));
        ImGui::SetWindowFontScale(0.85f);
        ImGui::TextWrapped("%s", sub1);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
    }

    if (sub2 && sub2[0]) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.38f, 0.38f, 0.38f, 1));
        ImGui::SetWindowFontScale(0.85f);
        ImGui::TextWrapped("%s", sub2);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
    }

    ColorBar(pct, colBar, 6);
    MiniPlot(label, hist, histCount, hMin, hMax, colBar, 22);
    ImGui::Spacing();
}

static bool ProfileButton(const char* id, const char* label, const char* desc, ImVec4 accent, bool isNuclear = false) {
    ImVec2 sz = ImVec2(ImGui::GetContentRegionAvail().x, 66);
    ImGui::PushStyleColor(ImGuiCol_Button, isNuclear ? ImVec4(0.23f, 0, 0, 1) : ImVec4(0.10f, 0, 0, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, isNuclear ? ImVec4(0.32f, 0, 0, 1) : ImVec4(0.17f, 0, 0, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.05f, 0, 0, 1));

    bool hit = ImGui::Button(id, sz);
    ImGui::PopStyleColor(3);

    ImVec2 p = ImGui::GetItemRectMin();
    ImVec2 p2 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(p, ImVec2(p.x + 3, p2.y), ImGui::ColorConvertFloat4ToU32(accent));
    dl->AddRect(p, p2, IM_COL32(85, 0, 0, 255));
    dl->AddText(nullptr, 14.f, ImVec2(p.x + 12, p.y + 8), IM_COL32(255, 255, 255, 255), label);
    dl->AddText(nullptr, 10.f, ImVec2(p.x + 12, p.y + 28), IM_COL32(136, 136, 136, 255), desc);

    if (isNuclear)
        dl->AddText(nullptr, 10.f, ImVec2(p2.x - 72, p.y + (p2.y - p.y) / 2.f - 6), IM_COL32(255, 51, 0, 255), "\xE2\x98\xa2 NO UNDO");
    else
        dl->AddText(nullptr, 13.f, ImVec2(p2.x - 18, p.y + (p2.y - p.y) / 2.f - 7), ImGui::ColorConvertFloat4ToU32(accent), ">");

    return hit;
}

static void DrawTitlebarButtons(ImDrawList* dl, float winX, float winY, float winW, HWND hwnd) {
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
    if (ImGui::InvisibleButton("##tb_min", ImVec2(16, 14))) ShowWindow(hwnd, SW_MINIMIZE);

    ImGui::SetCursorScreenPos(ImVec2(bx + 18, winY + 5));
    if (ImGui::InvisibleButton("##tb_max", ImVec2(16, 14))) ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);

    ImGui::SetCursorScreenPos(ImVec2(bx + 36, winY + 5));
    if (ImGui::InvisibleButton("##tb_cls", ImVec2(16, 14))) PostQuitMessage(0);
}

// Fallback de PDH por cálculo nativo (intervalo 1000ms)
static void LiveMetricsThread() {
    Sleep(1000);
    while (true) {
        if (!g_snapBusy) {
            g_snapBusy = true;
            SystemSnapshot s = TakeSnapshot(400, nullptr);

            { std::lock_guard<std::mutex> lk(g_snapMtx); g_snap = s; }
            g_cpuHistory[g_histIdx] = s.cpu.ok ? (float)s.cpu.usedPct : 0;
            g_ramHistory[g_histIdx] = s.ram.ok ? (float)s.ram.usedPct : 0;
            g_gpuHistory[g_histIdx] = (s.gpu.ok && s.gpu.hasUsage) ? (float)s.gpu.vramUsedPct : 0;
            g_diskHistory[g_histIdx] = s.disk.ok ? (float)std::min(s.disk.activityMBs, 200.0) : 0;
            g_histIdx = (g_histIdx + 1) % 60;
            g_snapBusy = false;
        }
        Sleep(1000);
    }
}

static void RenderFrame() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##main", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    SystemSnapshot snap;
    { std::lock_guard<std::mutex> lk(g_snapMtx); snap = g_snap; }

    float W = io.DisplaySize.x;
    float H = io.DisplaySize.y;

    const float TITLE_H = 26.f;
    const float BOTTOM_H = 38.f;
    const float CONTENT_H = H - TITLE_H - BOTTOM_H;

    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        dl->AddRectFilledMultiColor(
            wp, ImVec2(wp.x + W, wp.y + TITLE_H),
            IM_COL32(139, 0, 0, 255), IM_COL32(204, 0, 0, 255),
            IM_COL32(204, 0, 0, 255), IM_COL32(139, 0, 0, 255));
        dl->AddText(nullptr, 11.f, ImVec2(wp.x + 8, wp.y + 7), IM_COL32(255, 200, 0, 255), "!");
        dl->AddText(nullptr, 12.f, ImVec2(wp.x + 22, wp.y + 7), IM_COL32(255, 255, 255, 255), "WannaClean");
        DrawTitlebarButtons(dl, wp.x, wp.y, W, g_hwnd);
        ImGui::Dummy(ImVec2(W, TITLE_H));
    }

    const float LEFT_W = 230.f;
    const float RIGHT_W = W - LEFT_W - 20.f;
    const float INNER_H = CONTENT_H - 8.f;

    ImGui::BeginChild("##left", ImVec2(LEFT_W, INNER_H - BOTTOM_H), false);

    {
        DWORD elapsed = (GetTickCount64() - g_countdownStart) / 1000;
        int remain = (int)g_countdownSecs - (int)elapsed;
        if (remain < 0) remain = 0;
        int mm = remain / 60, ss = remain % 60;

        SectionHeader("! System status before optimization");
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0, 0, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.f);
        ImGui::BeginChild("##bef_inner", ImVec2(-1, INNER_H - 275), true);

        {
            char v[48], s1[64], s2[64];
            float pct;
            ImU32 colCPU = IM_COL32(255, 34, 0, 255);
            ImU32 colRAM = IM_COL32(255, 210, 0, 255);
            ImU32 colGPU = IM_COL32(255, 120, 0, 255);
            ImU32 colDISK = IM_COL32(0, 200, 180, 255);

            if (snap.cpu.ok) {
                sprintf_s(v, "%.0f%%", snap.cpu.usedPct);
                DWORD procCount = 0;
                {
                    DWORD bufSize = 1024 * sizeof(DWORD);
                    std::vector<DWORD> pids;
                    for (;;) {
                        pids.resize(bufSize / sizeof(DWORD));
                        DWORD needed = 0;
                        if (!EnumProcesses(pids.data(), bufSize, &needed)) break;
                        if (needed < bufSize) { procCount = needed / sizeof(DWORD); break; }
                        bufSize *= 2;
                    }
                }
                sprintf_s(s1, "%d cores  |  %d procs", snap.cpu.coreCount, (int)procCount);

                if (snap.cpu.hasTemp) {
                    const char* tempIcon = snap.cpu.tempC >= 85.0 ? "[!!]" : snap.cpu.tempC >= 70.0 ? "[ !]" : "[OK]";
                    sprintf_s(s2, "Temp: %.0f C  %s", snap.cpu.tempC, tempIcon);
                }
                else {
                    s2[0] = 0;
                }
            }
            else {
                sprintf_s(v, "---"); s1[0] = 0; s2[0] = 0;
            }
            pct = snap.cpu.ok ? (float)(snap.cpu.usedPct / 100.0) : 0;
            TelemetryRow("CPU", v, pct, colCPU, g_cpuHistory, 60, 0, 100, s1, s2);

            if (snap.ram.ok) {
                sprintf_s(v, "%.0f%%", snap.ram.usedPct);
                sprintf_s(s1, "%.0fMB / %.0fMB", snap.ram.usedGB * 1024.0, snap.ram.totalGB * 1024.0);
            }
            else {
                sprintf_s(v, "---"); s1[0] = 0;
            }
            pct = snap.ram.ok ? (float)(snap.ram.usedPct / 100.0) : 0;
            TelemetryRow("RAM", v, pct, colRAM, g_ramHistory, 60, 0, 100, s1);

            bool gpuOk = snap.gpu.ok && snap.gpu.hasUsage;
            if (gpuOk) {
                sprintf_s(v, "%.1fGB / %.1fGB", snap.gpu.vramUsedGB, snap.gpu.vramTotalGB);
                char shortName[24] = {};
                strncpy_s(shortName, snap.gpu.name.c_str(), 22);
                if (snap.gpu.name.size() > 22) { shortName[21] = '.'; shortName[22] = '.'; shortName[23] = 0; }
                if (snap.gpu.hasUsage && snap.gpu.gpuUsePct > 0.0)
                    sprintf_s(s1, "%.0f%% VRAM | %.0f%% GPU | %s", snap.gpu.vramUsedPct, snap.gpu.gpuUsePct, shortName);
                else
                    sprintf_s(s1, "%.0f%% VRAM  |  %s", snap.gpu.vramUsedPct, shortName);
            }
            else {
                sprintf_s(v, "N/A"); s1[0] = 0;
            }
            pct = 0;
            if (gpuOk) {
                if (snap.gpu.hasUsage && snap.gpu.gpuUsePct > 0.0f) pct = (float)(snap.gpu.gpuUsePct / 100.0f);
                else pct = (float)(snap.gpu.vramUsedPct / 100.0f);
            }
            TelemetryRow("GPU", v, pct, colGPU, g_gpuHistory, 60, 0, 100, s1);

            bool diskOk = snap.disk.ok && snap.disk.hasActivity;
            if (diskOk) {
                sprintf_s(v, "%.1fMB/s", snap.disk.activityMBs);
                sprintf_s(s1, "%.1f / %.1f GB free", snap.disk.freeGB, snap.disk.totalGB);
            }
            else if (snap.disk.ok) {
                sprintf_s(v, "0.0MB/s");
                sprintf_s(s1, "%.1f / %.1f GB free", snap.disk.freeGB, snap.disk.totalGB);
            }
            else {
                sprintf_s(v, "---"); s1[0] = 0;
            }
            pct = diskOk ? std::min((float)(snap.disk.activityMBs / 200.f), 1.f) : 0;
            TelemetryRow("DISK", v, pct, colDISK, g_diskHistory, 60, 0, 200, s1);
        }

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        ImGui::Spacing();
        char timerBuf[16];
        if (remain == 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.13f, 0.0f, 1.0f));
            ImGui::SetWindowFontScale(0.85f);
            ImGui::TextWrapped("The deadline has passed. Your bloatware remains. Your dignity does not. Select a profile. Now.");
            ImGui::SetWindowFontScale(1.f);
            ImGui::PopStyleColor();
        }
        else {
            sprintf_s(timerBuf, "%02d:%02d", mm, ss);
            ImGui::SetWindowFontScale(1.6f);
            float tw = ImGui::CalcTextSize(timerBuf).x;
            ImGui::SetCursorPosX((LEFT_W - tw) * 0.5f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.13f, 0, 1));
            ImGui::TextUnformatted(timerBuf);
            ImGui::PopStyleColor();
            ImGui::SetWindowFontScale(1.f);

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.4f, 1));
            float lw = ImGui::CalcTextSize("Time until total collapse").x;
            ImGui::SetCursorPosX((LEFT_W - lw) * 0.5f);
            ImGui::TextUnformatted("Time until total collapse");
            ImGui::PopStyleColor();
        }
    }

    ImGui::Spacing();

    {
        SectionHeader("System Management");
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.0f, 0.0f, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.f);
        ImGui::BeginChild("##management_inner", ImVec2(-1, 92), true);
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.75f, 0.75f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.85f, 0.85f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.60f, 0.60f, 0.60f, 1));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 1));

        float btnWidth = (ImGui::GetContentRegionAvail().x - 6) * 0.5f;

        if (ImGui::Button("Edit config", ImVec2(btnWidth, 24))) {
            ShellExecuteA(nullptr, "open", "config.json", nullptr, nullptr, SW_SHOW);
        }
        ImGui::SameLine(0, 6);
        if (ImGui::Button("Reload Cfg", ImVec2(btnWidth, 24))) {
            std::string e;
            g_cfgLoaded = LoadConfig("config.json", g_cfg, e);
            if (!g_cfgLoaded) g_cfgError = e;
        }

        ImGui::Spacing();

        if (ImGui::Button("Exit Application", ImVec2(-1, 24))) {
            PostQuitMessage(0);
        }

        ImGui::PopStyleColor(4);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.4f, 1));
    if (g_cfgLoaded)
        ImGui::Text("cfg: %d profiles | %d whitelist", (int)g_cfg.profiles.size(), (int)g_cfg.whitelist.size());
    else
        ImGui::TextColored(ImVec4(1, 0.2f, 0, 1), "cfg error");
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::SameLine(0, 8);

    ImGui::BeginChild("##right", ImVec2(RIGHT_W, INNER_H - BOTTOM_H), false);

    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilledMultiColor(
            p, ImVec2(p.x + RIGHT_W, p.y + 32),
            IM_COL32(204, 0, 0, 255), IM_COL32(119, 0, 0, 255),
            IM_COL32(119, 0, 0, 255), IM_COL32(204, 0, 0, 255));
        const char* h1 = "Ooops, your PC has been optimized!";
        ImVec2 ts = ImGui::CalcTextSize(h1);
        dl->AddText(nullptr, 15.f, ImVec2(p.x + (RIGHT_W - ts.x) * 0.5f, p.y + 8), IM_COL32(255, 255, 255, 255), h1);
        ImGui::Dummy(ImVec2(RIGHT_W, 34));
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1, 1, 1, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 2.f);
    float contentH = INNER_H - BOTTOM_H - 45;
    ImGui::BeginChild("##content", ImVec2(-1, contentH), true);

    auto H2 = [](const char* t) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.54f, 0, 0, 1));
        ImGui::TextUnformatted(t);
        ImGui::PopStyleColor();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddLine(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y), IM_COL32(204, 0, 0, 255));
        ImGui::Dummy(ImVec2(0, 3));
        };
    auto Body = [](const char* t) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.13f, 0.13f, 0.13f, 1));
        ImGui::TextWrapped("%s", t);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        };

    H2("WHAT HAPPENED TO MY COMPUTER?");
    Body("Your system was found guilty of negligence against its own hardware. Background processes -- uninvited, unnecessary, and deeply committed to wasting your resources -- had formed an illegal coalition against your CPU. We intervened. Your files are untouched. Your RAM has been liberated. Windows, however, is still Windows. We can only do so much.");

    H2("CAN I GET MY BLOATWARE BACK?");
    Body("Yes. OneDrive, the Edge update daemon, Cortana, and seventeen instances of svchost.exe will return the moment you reboot -- patient, persistent, and completely useless. They always come back. But for now, your PC is running the way it was always meant to.");

    H2("HOW DO I PAY THE RANSOM?");
    Body("Nothing. This is a public service. We only ask that you reflect, seriously and at length, on why you allowed all of that to run at startup. You knew. You always knew.");

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImGui::EndChild();

    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        float  bbY = H - BOTTOM_H;
        float  bbScrY = wp.y + bbY;

        dl->AddRectFilled(ImVec2(wp.x, bbScrY), ImVec2(wp.x + W, wp.y + H), IM_COL32(192, 192, 192, 255));
        dl->AddLine(ImVec2(wp.x, bbScrY), ImVec2(wp.x + W, bbScrY), IM_COL32(128, 128, 128, 255));
        dl->AddLine(ImVec2(wp.x, bbScrY + 1), ImVec2(wp.x + W, bbScrY + 1), IM_COL32(255, 255, 255, 255));

        ImGui::SetCursorPos(ImVec2(8, bbY + 6));

        ImGui::PushStyleColor(ImGuiCol_Text, g_cfgLoaded ? ImVec4(0, 0.5f, 0, 1) : ImVec4(0.7f, 0, 0, 1));
        ImGui::TextUnformatted(g_cfgLoaded ? "v CONFIG OK" : "x CONFIG ERR");
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 8);

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 1));
        ImGui::BeginChild("##logbar", ImVec2(W - 380, 22), false);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0.8f, 0.27f, 1));
        static bool blk = true; static DWORD lastBlk = 0;
        DWORD now = GetTickCount64();
        if (now - lastBlk > 700) { blk = !blk; lastBlk = now; }
        char msg[128];
        if (snap.ram.ok)
            sprintf_s(msg, "%s [LIVE] RAM: %.1f/%.1fGB | CPU: %.0f%% | Select a profile", blk ? "*" : "", snap.ram.usedGB, snap.ram.totalGB, snap.cpu.ok ? snap.cpu.usedPct : 0.0);
        else
            strcpy_s(msg, "  [INIT] Loading system metrics...");
        ImGui::TextUnformatted(msg);
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 6);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.75f, 0.75f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.85f, 0.85f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.60f, 0.60f, 0.60f, 1));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 1));

        if (ImGui::Button("Gaming", ImVec2(58, 22))) {
            g_pendingProfile = "gaming";
            g_launchResult = true;
        }
        ImGui::SameLine(0, 2);

        if (ImGui::Button("Work", ImVec2(58, 22))) {
            g_pendingProfile = "work";
            g_launchResult = true;
        }
        ImGui::SameLine(0, 2);

        if (ImGui::Button("Nuclear", ImVec2(72, 22)))
            g_nuclearConfirm = true;

        ImGui::SameLine(0, 8);

        ImGui::PopStyleColor(4);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.15f, 0.15f, 0.15f, 1));
        ImGui::Checkbox("Dry Run", &g_dryRun);
        ImGui::PopStyleColor();

        if (g_nuclearConfirm) {
            ImGui::OpenPopup("Nuclear##confirm");
            g_nuclearConfirm = false;
        }

        ImGui::SetNextWindowSize(ImVec2(400, 160), ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));

        if (ImGui::BeginPopupModal("Nuclear##confirm", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1, 0.2f, 0, 1), "  [!]  WARNING: NUCLEAR MODE");
            ImGui::Spacing();
            ImGui::TextWrapped("This will terminate ALL processes except the system whitelist. Browsers, Steam, Discord -- everything goes. Cannot be undone.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            float btnConfirmW = 130.0f;
            float btnCancelW = 70.0f;
            float spaceBetween = 5.0f;
            float totalW = btnConfirmW + btnCancelW + spaceBetween;

            ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - totalW + 8);

            if (ImGui::Button("CONFIRM \xE2\x98\xa2", ImVec2(btnConfirmW, 24))) {
                g_pendingProfile = "nuclear";
                g_launchResult = true;
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine(0, spaceBetween);

            if (ImGui::Button("Cancel", ImVec2(btnCancelW, 24)))
                ImGui::CloseCurrentPopup();

            ImGui::EndPopup();
        }
    }

    ImGui::End();
}

// Drag + hit-test para la zona de titlebar (excluyendo botones)
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_dev && wParam != SIZE_MINIMIZED) {
            M_CleanRTV();
            g_swap->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            M_CreateRTV();
        }
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = 780;
        mmi->ptMinTrackSize.y = 620;
        return 0;
    }

    case WM_NCHITTEST: {
        LRESULT def = DefWindowProcA(hWnd, msg, wParam, lParam);
        if (def == HTCLIENT) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            RECT rc; GetClientRect(hWnd, &rc);
            MapWindowPoints(hWnd, nullptr, (POINT*)&rc, 2);
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
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

int RunMainWindow(HINSTANCE hInstance, int nCmdShow) {
    std::string err;
    g_cfgLoaded = LoadConfig("config.json", g_cfg, err);
    if (!g_cfgLoaded) {
        CreateDefaultConfig("config.json", err);
        g_cfgLoaded = LoadConfig("config.json", g_cfg, err);
        if (!g_cfgLoaded) g_cfgError = err;
    }

    g_countdownStart = GetTickCount();

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "BKMain";
    wc.hIcon = LoadIconA(hInstance, "APP_ICON");
    if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExA(&wc);

    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    int winW = 780, winH = 620;

    g_hwnd = CreateWindowExA(
        WS_EX_APPWINDOW,
        "BKMain", "WannaClean",
        WS_POPUP | WS_VISIBLE | WS_SYSMENU,
        (scrW - winW) / 2, (scrH - winH) / 2,
        winW, winH,
        nullptr, nullptr, hInstance, nullptr);

    SetWindowPos(g_hwnd, nullptr, 0, 0, winW, winH, SWP_NOMOVE | SWP_NOZORDER);

    if (!M_CreateDevice(g_hwnd)) {
        M_Cleanup();
        UnregisterClassA("BKMain", hInstance);
        return 1;
    }

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ApplyTheme();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    std::thread tMet(LiveMetricsThread);
    tMet.detach();
    StartCpuPdhSampler();

    bool running = true;
    while (running) {
        MSG m;
        while (PeekMessage(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessage(&m);
            if (m.message == WM_QUIT) running = false;
        }
        if (!running) break;

        if (g_launchResult) {
            g_launchResult = false;
            g_cfg.dryRun = g_dryRun;
            ShowWindow(g_hwnd, SW_HIDE);
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            RunResultWindow(hInstance, g_pendingProfile.c_str(), g_cfg);
            ImGui_ImplWin32_Init(g_hwnd);
            ImGui_ImplDX11_Init(g_dev, g_ctx);
            ShowWindow(g_hwnd, SW_SHOW);
            SetForegroundWindow(g_hwnd);
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RenderFrame();
        ImGui::Render();

        const float clr[4] = { 0.54f, 0, 0, 1 };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    M_Cleanup();
    DestroyWindow(g_hwnd);
    UnregisterClassA("BKMain", hInstance);

    return 0;
}