# ⚠️ WannaClean

> *Your files have not been encrypted. Your RAM, however, has been liberated.*

---

**WannaClean** is a Windows system optimizer disguised — aesthetically, intentionally, and with full commitment — as a ransomware note. It kills background processes, stops unnecessary services, and flushes RAM caches. All with a blood-red UI, a countdown timer labeled **"Time until total collapse"**, and a results screen that opens with the line:

> *"Ooops, your PC has been optimized!"*

It does not encrypt your files. It does not demand Bitcoin. It only asks that you reflect, seriously and at length, on why you let Discord, OneDrive, Cortana, and seventeen instances of `svchost.exe` run at startup. You knew. You always knew.

---

## What it actually does

WannaClean takes a before-snapshot of your system (RAM, CPU, GPU VRAM, disk I/O), runs one of three optimization profiles, then takes an after-snapshot and shows you the delta. Everything is logged.

The three profiles are:

**Gaming** — Clears the non-essential clutter: browsers, Discord, Spotify, Teams, Zoom, OneDrive, Cortana, remote desktop tools. Also stops background services like Windows Search, telemetry (DiagTrack), SysMain, Xbox services, and Windows Update. Your games get the CPU they were always owed.

**Work** — The inverse. Kills all the gaming infrastructure: Steam, Epic, Battle.net, Origin, EA Desktop, Riot Client, GeForce Experience, Discord, Twitch, Overwolf, wallpaper engines. Stops the corresponding background services. Your deadlines get the RAM your launcher collection was hoarding.

**Nuclear** ☢ — Kills *everything* not on the system whitelist. No exceptions, no mercy, no undo. A confirmation dialog appears first. It says `CONFIRM ☢`. If you click it, that's on you.

---

## The whitelist

The whitelist is your system's immune system. WannaClean will never touch:

`system`, `idle`, `registry`, `smss.exe`, `csrss.exe`, `wininit.exe`, `winlogon.exe`, `lsass.exe`, `services.exe`, `svchost.exe`, `dwm.exe`, `explorer.exe`, `audiodg.exe`, `fontdrvhost.exe`, `ctfmon.exe`, `conhost.exe`, `taskhostw.exe`, `sihost.exe`, `msmpeng.exe`, `securityhealthsystray.exe`, `securityhealthservice.exe`, `runtimebroker.exe`, `shellexperiencehost.exe`, `startmenuexperiencehost.exe`, `spoolsv.exe`, `lsaiso.exe`, `memory compression`, `taskmgr.exe`, `wudfhost.exe`, `wmiprvse.exe`, `searchindexer.exe`, `sgrmbroker.exe`

And, crucially: `WannaClean.exe` itself. We are not suicidal.

The whitelist is fully editable in `config.json`. Profile-specific kill lists and service lists are also there. Edit it with the **Edit Config** button in the UI, then hit **Reload Cfg** without restarting the app.

---

## RAM cleaning

Beyond process and service management, WannaClean calls `NtSetSystemInformation` (yes, directly into ntdll) with three commands:

- **EmptyWorkingSets** — trims working sets of all running processes
- **FlushModifiedList** — writes the modified page list to disk
- **PurgeStandbyList** — clears the standby list

This is the same thing RAMMap does, minus the GUI, minus the license agreement, and plus the ransomware aesthetic.

---

## UI overview

The main window opens with a live telemetry panel on the left: CPU usage (with temperature if readable), RAM, GPU VRAM, and disk I/O — all with historical sparklines and color bars. A countdown timer ticks down from 5:00, labeled "Time until total collapse." When it hits zero it is replaced with the message:

> *"The deadline has passed. Your bloatware remains. Your dignity does not. Select a profile. Now."*

The right panel is a mock ransomware note explaining what happened to your computer, whether you can get your bloatware back (yes, on next reboot, sadly), and how to pay the ransom (you can't; this is a public service).

The result window shows before/after ring charts for RAM, CPU, GPU VRAM, and disk I/O, a summary card row (processes killed, services stopped, GB freed, failures), a breakdown of the top targeted processes, a service failure list if anything went sideways, and a live scrolling operation log.

**Dry Run** mode simulates the full operation without touching anything. Useful if you want to know what would have been killed without actually committing to it. Or if you're nervous. Which is valid.

---

## Building from source

### Prerequisites

- Windows 10/11
- Visual Studio 2022 with the C++ desktop workload
- CMake 3.20+
- ImGui (cloned separately — see below)

### Setup

Clone the repo and add ImGui:

```bash
git clone https://github.com/BreperX/WannaClean.git
cd WannaClean
git clone --depth=1 https://github.com/ocornut/imgui.git
```

Optionally drop a `WannaClean.ico` in the project root for the window icon. The CMake build will warn if it's missing but will still compile.

### Build (Visual Studio)

Open the folder in Visual Studio 2022 (**File → Open → Folder**). Select the `x64-Release` or `x64-Debug` configuration from the toolbar. CMake will configure automatically. Press **Build**.

The output binary lands in `out/build/x64-Release/WannaClean.exe`.

### Build (command line)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Requires an MSVC environment (run from a Visual Studio Developer Command Prompt or use `vcvars64.bat`).

---

## Running the app

WannaClean requires **Administrator privileges** — it needs to open and terminate processes belonging to other users, control services, and call privileged NT APIs. It will show a clear error and refuse to start if not elevated.

Right-click `WannaClean.exe` → **Run as administrator**.

On first run, if no `config.json` is found alongside the executable, one is created automatically with the default whitelist and the three built-in profiles.

---

## Download

> 📦 **[Download latest release](https://github.com/BreperX/WannaClean/releases)** — `WannaClean.exe` (Windows x64, requires Admin)

---

## Configuration reference

`config.json` lives next to the executable. Structure:

```json
{
  "logFile": "WannaClean_log.txt",
  "dryRun": false,
  "showDiskNote": true,
  "whitelist": [ "system", "explorer.exe", "..." ],
  "profiles": {
    "gaming": {
      "useWhitelist": false,
      "extraKill": [ "discord.exe", "chrome.exe", "..." ],
      "stopServices": [ "WSearch", "DiagTrack", "..." ]
    },
    "nuclear": {
      "useWhitelist": true,
      "extraKill": [],
      "stopServices": [ "..." ]
    }
  }
}
```

`useWhitelist: true` activates nuclear mode — the profile ignores `extraKill` entirely and instead kills everything *not* in the global whitelist. `useWhitelist: false` means only the processes in `extraKill` are targeted.

Logs are appended to `logFile` (if set) with before/after snapshots, deltas, kill counts, service results, and RAM clean steps.

---

## Technical notes

- Built with **C++17**, **ImGui** (DX11 backend), and **Win32 APIs** only. No runtime dependencies beyond what ships with Windows.
- Process killing is recursive — children are terminated before parents to avoid orphan processes.
- The app protects itself during nuclear mode by adding its own executable name and PID to the whitelist at runtime. It will not kill itself. (Probably.)
- CPU metrics are sampled via PDH (Performance Data Helper) on a background thread. GPU VRAM comes from DXGI. Disk I/O uses PDH counters. RAM uses GlobalMemoryStatusEx.
- The window is borderless with a custom-drawn Win98-style titlebar and draggable caption area. Yes, this was a deliberate aesthetic choice. No, we are not sorry.

---

## FAQ

**Is this actually ransomware?**
No. It is a system optimizer that has committed to a bit. Your files are safe. Your dignity, however, depends on how many things were running at startup.

**Will it break my PC?**
It should not. The whitelist covers all critical system processes. Nuclear mode is genuinely aggressive — don't run it if you have unsaved work open.

**Can I add my own processes to the kill list?**
Yes. Edit `config.json` and add them to `extraKill` under the relevant profile, or create a new profile entirely. Reload with **Reload Cfg**.

**Why does disk I/O go *up* after running it?**
That's normal. Flushing the modified page list writes cached pages to disk before clearing them. It's the sound of your RAM being cleaned. It passes quickly.

**OneDrive is back.**
Yes. It always comes back. WannaClean is not a cure. It is a temporary relief. Reboot is when the bloatware returns, patient and persistent. We can only do so much.

---

*WannaClean — because WannaCry was taken, and your PC really did need to be cleaned.*