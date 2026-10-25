# GBFR Crash Logger (v1.0.4)

A diagnostic **proxy DLL** for *Granblue Fantasy: Relink* and the *Endless Ragnarok* demo
(x64 Windows). It loads into the game, records why the game crashes, hangs or fails to render,
and it can apply two compatibility fixes. Together with DXVK, those fixes got the demo running on
a **GTX 960M** (Maxwell, native D3D11 feature level 11_0), which the game doesn't support out of the box.

---

## Folder layout

```
GBFR Crash Logger\
  dinput8.dll              <- the logger (default build)
  alt\version.dll          <- same logger built as version.dll (use if another mod owns dinput8.dll)
  GBFRCrashLogger.ini      <- settings + compatibility fixes
  README.md                <- this file
  source\
    crashlogger.cpp        <- all code, every function commented
    dinput8.def            <- export table for the dinput8.dll build
    version.def            <- export table for the version.dll build
    build.sh               <- build script (mingw-w64)
```

Files in the **demo's game folder**
(`E:\SteamLibrary\steamapps\common\Granblue Fantasy Relink Endless Ragnarok Demo\`):

| File | Purpose |
|---|---|
| `dinput8.dll` + `GBFRCrashLogger.ini` | this logger + its fixes |
| `d3d11.dll`, `dxgi.dll` | official **DXVK 3.1.1** (D3D11 → Vulkan, what Proton uses on Linux) |
| `dxvk.conf` | active DXVK config (game profile applied by hand, see below) |
| `dxvk.conf.bak` | your previous DXVK config |
| `dxvk_profiles\` | A/B/C performance test configs + README |
| `CrashLogger\` | logs, minidumps, saved shaders (created by the logger) |

---

## How it loads

The game imports `DINPUT8.dll`. Windows looks in the game folder before System32, so it loads
our `dinput8.dll` instead. Every real dinput8 export is a small assembly stub
(`jmp [g_fwd + 8*i]`) that jumps into the **genuine System32 dinput8.dll**. Input keeps
working exactly as before. The logger's own code runs from `DllMain` and from two background threads.

The `version.dll` build works the same way for games or setups where dinput8 isn't usable.

---

## What it does

### 1. Logging (`CrashLogger\log_<date>.txt`)
- Game exe, version, command line, Windows build (and whether it's running under Wine/Proton)
- CPU model and instruction sets: SSE4.x, AVX, AVX2, FMA3, F16C, BMI, AVX‑512, and whether the OS has AVX state saving enabled
- Every `D3D11CreateDevice` call: adapter, flags, requested and obtained **feature level**
- Swapchain creation and resolution changes
- **Shader creation failures**: each failed shader is logged with its HRESULT and saved as
  `CrashLogger\shaders\<stage>_<hash>.cso` (all shaders if `DumpShaders=1`)
- Heartbeat every 60 s: frames, shader count, working set, free RAM

### 2. Crash reports
- **Vectored exception handler**: sees every exception before the game does. Fatal ones get a
  report even if the game's own handler then swallows them.
- **Unhandled exception filter**: kept on top even when the game installs its own (the game's
  filter is chained afterwards, so its normal crash behaviour still happens).
- A report contains: exception and meaning, `module+offset`, what an access violation touched,
  registers, the instruction bytes with an **ISA hint** (e.g. "VEX = AVX2"), the unwound call
  stack, a heuristic stack scan, recent D3D operations ("breadcrumbs"), the module list and a **minidump**.

### 3. Hang reports
Written when:
- the game hasn't created its D3D11 device **25 s** after launch (startup hang)
- frames stop for `HangSeconds`, or a thread is stuck inside `Present()`
- the window is "Not Responding"

Each report lists **every thread** (name, current module+offset, likely callers) and writes a
`hang` minidump. A hang report is how the DXVK/Intel deadlock below was found.

### 4. GPU device-removed reports
If `Present`/`ResizeBuffers` returns `DEVICE_REMOVED`/`HUNG`/`RESET`, the report logs DXGI's reason,
the recent D3D operations and all threads, and writes a `device_removed` minidump.

### 5. Compatibility fixes (from the ini)

**`[Environment]`** sets environment variables **for the game process only**, before Vulkan starts:
```ini
VK_LOADER_DRIVERS_DISABLE=*igvk*
```

**`[BlockDlls]`** makes listed DLLs look **missing** to the game (`LoadLibrary*` returns
"module not found"):
```ini
nvapi64.dll=1      ; set to 0 to allow it again
```

---

## How the GTX 960M was made to run the demo

| # | Blocker | How it was found | Fix |
|---|---|---|---|
| 1 | Two depth‑of‑field compute shaders use UAV slots **u10/u11**, which need feature level **11_1**. NVIDIA's native D3D11 driver gives Maxwell 1 cards only **11_0**, so `CreateComputeShader` fails with `E_INVALIDARG`. | Shader hook saved the shaders; their `SFI0` flags say "64 UAVs" and their reflection data shows u10/u11. | Run through **DXVK**, which reports 11_1 or higher on the same GPU (this is why the game works on Linux). |
| 2 | DXVK's built‑in fix profile for this game only matches `granblue_fantasy_relink.exe`, not the demo's exe name. | DXVK source (`config.cpp`). | Same settings put in `dxvk.conf` by hand, plus `dxvk.enableNvRawAccessChains = False` (NVIDIA hang workaround, DXVK #5785). |
| 3 | The game ran only as a **background process**, with no window. The Intel HD 530's 2018 Vulkan driver (`igvk64.dll`) calls back into DXVK's `dxgi.dll` while DXVK is creating its Vulkan instance under a lock, so the main thread waits on itself forever. | Startup‑hang report + minidump: `DXVK dxgi → vulkan-1 → igvk64 → DXVK dxgi (lock)`. | `[Environment] VK_LOADER_DRIVERS_DISABLE=*igvk*`: the Vulkan loader skips the Intel driver (DXVK skips it anyway because it lacks Vulkan 1.3). |
| 4 | The game started but **characters and terrain were invisible**. The exe links NVIDIA's NVAPI and loads `nvapi64.dll`. On Windows that succeeds, so the game turns on NVIDIA‑driver extensions and hands them DXVK's device, which the driver doesn't recognise. On Linux/Proton NVAPI is missing, so the game uses its plain D3D11 path. | Exe strings (`nvapi64.dll`, `nvapi_QueryInterface`) + DXVK's NVAPI logic. | `[BlockDlls] nvapi64.dll=1`: the game sees NVAPI as missing, the same as under Proton. |
| 5 | Logger startup thread initialising DXVK at the same time as the game | Earlier logs stopping at the first DXGI factory | `ProbeD3D` turns itself off when `d3d11.dll`/`dxgi.dll` are next to the exe; the watchdog runs on its own thread. |

---

## Install / uninstall

**Install:** copy `dinput8.dll` and `GBFRCrashLogger.ini` next to the game exe. For the 960M setup,
also copy DXVK's `d3d11.dll`, `dxgi.dll` and the `dxvk.conf`.

**Uninstall:** delete `dinput8.dll` (and DXVK's `d3d11.dll`/`dxgi.dll` to go back to native D3D11).

If no `CrashLogger` folder appears after launch, the DLL wasn't loaded. Try `alt\version.dll`.

Use offline only. Don't take it into online/co‑op lobbies.

---

## Settings (`GBFRCrashLogger.ini`)

| Key (`[Logger]`) | Default | Meaning |
|---|---|---|
| `DebugLayer` | 0 | Adds the D3D11 debug layer (needs Windows "Graphics Tools"; native D3D11 only) |
| `LogWarnings` | 0 | Also log debug‑layer warnings |
| `DumpShaders` | 0 | Save every created shader, not just failed ones |
| `LogShaderCreates` | 0 | Log every shader creation |
| `HangSeconds` | 10 | Seconds without frames / not responding before a hang report |
| `StartupHangSeconds` | 25 | Seconds without a D3D11 device before a startup‑hang report (0 = off) |
| `FullDump` | 0 | Full‑memory minidumps (very large) |
| `FirstChanceAVReport` | 0 | Full report on first access violation even if the game handles it |
| `MaxFirstChanceLines` | 100 | Limit for short first‑chance lines |
| `HeartbeatSeconds` | 60 | "alive" line interval (0 = off) |
| `ProbeD3D` | 1 | Early hidden test device for hooks (auto‑off when DXVK/wrappers are present) |

`[Environment]`: `NAME=value` lines, set for the game process only.
`[BlockDlls]`: `file.dll=1` lines, DLLs the game can't load.

---

## Reading a log

- A log that ends with `Process exiting (normal ExitProcess…)` means the game closed itself. A log
  that just stops means it was killed (TerminateProcess or a fast‑fail).
- `[first-chance]` lines are faults the game *might* handle. They only matter if a crash report follows.
- In crash/hang reports, look at the first non‑Windows module in the call stack (game exe, DXVK
  `dxgi.dll`/`d3d11.dll`, a driver such as `nvwgf2umx.dll`/`igvk64.dll`). That's usually the culprit.
- Open `.dmp` files in **Visual Studio** (File → Open → .dmp) or **WinDbg** for full call stacks.

---

## Building

The source is plain C++17 with no external dependencies. From Linux/WSL:

```sh
sudo apt install g++-mingw-w64-x86-64
cd source && ./build.sh        # -> out/dinput8.dll, out/version.dll
```

**Code map** (`crashlogger.cpp`, in file order):
proxy forwarding stubs → config → logging → module table / symbols → breadcrumbs →
name tables → CPU/system info → stack walking → minidumps → exception reports and handlers →
IAT/vtable patching → D3D11/DXGI hooks → hang detection → Worker/Watchdog threads →
config/env/block‑list → `DllMain`.
