# GBFR Low-End Compatibility Kit

Running **Granblue Fantasy: Relink** (incl. the *Endless Ragnarok* demo) on GPUs the game doesn't
support, starting with an **NVIDIA GTX 960M** (Maxwell 1, native D3D11 feature level 11_0),
by reproducing on Windows what makes the game work on Linux/Proton.

The kit has two parts:

1. **GBFR Crash Logger** (`src/`): a diagnostic proxy DLL loaded by the game as `dinput8.dll`. It records
   crashes, hangs, GPU resets, failed shaders, frame times, VRAM and CPU usage, and applies two
   per-process compatibility fixes from an ini file.
2. **DXVK configuration** (`config/`): settings for [DXVK](https://github.com/doitsujin/dxvk)
   (D3D11 → Vulkan) tuned for this game and GPU, plus A/B test profiles.

> Status: the demo **starts, renders correctly and is playable** on a GTX 960M at 720p
> (about 21–25 FPS, GPU-bound). Before this work it either failed to create shaders (native D3D11)
> or hung at startup (DXVK).

---

## Table of contents
- [Goal](#goal)
- [Background: why the game doesn't run](#background-why-the-game-doesnt-run)
- [How it works](#how-it-works)
- [Repository layout](#repository-layout)
- [Prerequisites](#prerequisites)
- [Building](#building)
- [Installation](#installation)
- [Configuration](#configuration)
- [Using the logs](#using-the-logs)
- [Results](#results)
- [Testing](#testing)
- [Troubleshooting](#troubleshooting)
- [Known limitations](#known-limitations)
- [Roadmap](#roadmap)
- [Credits and licences](#credits-and-licences)
- [Disclaimer](#disclaimer)

---

## Goal

Find out exactly what stops the game on older hardware, and remove those blockers without
modifying game files:

- **Diagnose:** a logger inside the game process that shows *where* and *why* it fails.
- **Reproduce Linux behaviour on Windows:** the game is reported to run on Linux on the same
  class of hardware, so the Proton stack (DXVK and its environment) is the reference.
- **Optimise:** measure frame time, stutter, VRAM and CPU so that tuning is driven by data.

---

## Background: why the game doesn't run

Each blocker below was found with the logger. They are listed in the order they were hit.

| # | Symptom | Root cause | Evidence | Fix |
|---|---|---|---|---|
| 1 | Native D3D11: two shaders fail, the game doesn't get far | Two depth‑of‑field **compute shaders** bind UAVs at **u10/u11**. More than 8 UAV slots needs D3D11 **feature level 11_1**, and NVIDIA's D3D11 driver gives Maxwell 1 only **11_0**, so `CreateComputeShader` returns `E_INVALIDARG`. | Logger saved the shaders; their `SFI0` chunk = `64_UAVS`, reflection shows u10/u11 | Run the game through **DXVK**, which reports 11_1–12_1 on the same GPU (this is why Linux works) |
| 2 | DXVK's game fixes don't apply | DXVK's built‑in profile matches only `granblue_fantasy_relink.exe`; the demo is `granblue_fantasy_relink_endless_ragnarok_demo.exe` | DXVK source `src/util/config/config.cpp` | Same options set by hand in `dxvk.conf` |
| 3 | With DXVK: process runs **in the background, no window** | **Deadlock.** DXVK's `dxgi.dll` creates its Vulkan instance while holding a non‑recursive mutex. The Vulkan loader starts Intel's 2018 HD 530 driver (`igvk64.dll`), which calls `CreateDXGIFactory`. That resolves to DXVK's own `dxgi.dll` (already loaded under that name), which tries to take the same mutex on the same thread. | Startup‑hang minidump: `DXVK dxgi → vulkan-1 → igvk64 → DXVK dxgi (lock)`; DXVK `Singleton::acquire` | `VK_LOADER_DRIVERS_DISABLE=*igvk*` for the game process (logger `[Environment]`) |
| 4 | Game runs but **characters/terrain invisible** | The exe links NVIDIA **NVAPI** and loads `nvapi64.dll`. On Windows that succeeds, so the game enables NVIDIA‑driver extensions and passes them DXVK's device, which the driver doesn't recognise. On Proton NVAPI is absent, so the game uses its plain D3D11 path. | Exe strings `nvapi64.dll`, `nvapi_QueryInterface`; DXVK `isNvapiEnabled()` logic | Make `nvapi64.dll` look missing (logger `[BlockDlls]`) |
| 5 | Logger itself could stall startup with DXVK | The logger's optional early D3D probe initialised DXVK from a second thread at the same time as the game | Logs stopping at first DXGI factory creation | Probe auto‑disables when `d3d11.dll`/`dxgi.dll` sit next to the exe; watchdog on its own thread |
| 6 | Low FPS / uneven frames | GPU throughput: ~79 ms/frame at 1080p, ~44–48 ms at 720p; no CPU thread above ~23% of a core; VRAM ≤ 85% | Logger perf lines | 720p + quality settings; DXVK frame limiter for even pacing (profile E) |

---

## How it works

### Proxy loading
The game imports `DINPUT8.dll`. Windows searches the application folder before System32, so it
loads `dinput8.dll` from the game folder. Each real export (`DirectInput8Create`, …) is a
one‑instruction assembly stub (`jmp [g_fwd + 8*i]`) that jumps into the genuine
`System32\dinput8.dll`. Input works unchanged, and the logger's code runs from `DllMain` plus two
background threads. A `version.dll` build of the same code exists for setups where dinput8 is taken.

### Inside the logger (`src/crashlogger.cpp`)

| Area | Mechanism |
|---|---|
| Forwarding | `InitForwarding()` + per‑export asm stubs + `.def` export tables |
| Crash capture | Vectored exception handler (`VehHandler`) + top‑level filter (`OurUnhandledFilter`); `SetUnhandledExceptionFilter` hooked so the game can't remove it (the game's filter is chained) |
| Reports | x64 unwind (`RtlVirtualUnwind`), heuristic stack scan, ISA hint for illegal instructions, breadcrumbs, module list, minidumps via `dbghelp!MiniDumpWriteDump` (written from a helper thread) |
| D3D11/DXGI hooks | IAT hooks on the exe's `D3D11CreateDevice` and `CreateDXGIFactory2`; vtable hooks on the device (`Create*Shader`), factory (`CreateSwapChain*`) and swapchain (`Present`, `Present1`, `ResizeBuffers`) |
| Performance | Frame‑time ring buffer filled in `PresentEnd`; `Watchdog()` reports avg/median/1% low/worst, stutter counts, VRAM via `IDXGIAdapter3::QueryVideoMemoryInfo`, CPU per thread (`CpuSample`) |
| Hang detection | Startup (no device after N s), frames stopped / stuck in `Present`, window not responding; thread snapshot + minidump |
| Compatibility | `[Environment]` → `SetEnvironmentVariableW` before Vulkan starts; `[BlockDlls]` → IAT hooks on `LoadLibrary*`/`GetModuleHandle*` that return `ERROR_MOD_NOT_FOUND` |

Every function in the source has a comment block. A detailed per‑feature description is in
[`docs/LOGGER.md`](docs/LOGGER.md).

---

## Repository layout

```
.
├── README.md                     this file
├── bin/
│   ├── dinput8.dll               prebuilt logger v1.0.6 (default)
│   └── version.dll               same, alternative proxy name
├── src/
│   ├── crashlogger.cpp           all logger code (single file, every function commented)
│   ├── dinput8.def               export table, dinput8 build
│   ├── version.def               export table, version build
│   └── build.sh                  cross-compile script
├── config/
│   ├── GBFRCrashLogger.ini       logger settings + compatibility fixes (goes next to the exe)
│   ├── dxvk.conf                 active DXVK config used for the 720p tests (= profile E)
│   └── profiles/                 A–E DXVK test profiles + README
├── docs/
│   └── LOGGER.md                 logger reference: features, settings, how to read reports
└── tests/                        Wine test programs used to validate the logger
    ├── test.c                    access violation + illegal instruction + filter chaining
    ├── d3dtest.cpp               D3D11 device/swapchain, bad shader, hang, crash
    ├── f2test.cpp                CreateDXGIFactory2 + flip swapchain + timed stutters
    ├── blk.c                     [BlockDlls] LoadLibrary blocking
    ├── envt.c                    [Environment] variables visible via Win32
    ├── sleeper.c                 startup-hang detection
    └── dxbc_checksum.py          DXBC container checksum (verified against game shaders)
```

---

## Prerequisites

### To run (end user)

| Requirement | Version used | Notes |
|---|---|---|
| Windows | 10/11 x64 (tested 11 build 26100) | |
| Game | GBFR Endless Ragnarok demo, exe 1.0.1.0 | Should work for the full game exe too |
| GPU driver with **Vulkan 1.3** | NVIDIA 582.78 (GTX 960M) | Required by DXVK 2.x/3.x |
| Vulkan loader | `vulkan-1.dll` **≥ 1.3.234** (tested 1.4.309) | Needed for `VK_LOADER_DRIVERS_DISABLE`; installed with GPU drivers |
| [DXVK](https://github.com/doitsujin/dxvk/releases) | **3.1.1** (official release) | Use `x64/d3d11.dll` + `x64/dxgi.dll` only |
| Runtime libraries | none | The logger is linked fully static (no VC++/MinGW runtime needed); it uses only Windows system DLLs: kernel32, user32, msvcrt, psapi (K32*), dbghelp (loaded at runtime) |
| Optional | Windows "Graphics Tools" optional feature | Only for `DebugLayer=1` on native D3D11 |

### To build

| Requirement | Version used | Install (Ubuntu / WSL) |
|---|---|---|
| mingw‑w64 GCC (x86_64, C++17) | GCC 13 (`x86_64-w64-mingw32-g++`) | `sudo apt install g++-mingw-w64-x86-64` |
| mingw‑w64 binutils | 2.42 | pulled in by the package above |
| mingw‑w64 headers/import libs | 11.x | pulled in: `windows.h`, `d3d11.h`, `d3d11sdklayers.h`, `dxgi1_2.h`, `dxgi1_4.h`, `dbghelp.h`, `psapi.h`, `tlhelp32.h`, `libpsapi`, `libdbghelp` |
| POSIX shell | any | for `build.sh` |

On Windows you can build under **WSL** or **MSYS2** (`pacman -S mingw-w64-x86_64-gcc`). The code
uses GCC inline asm for the export stubs and `__cpuid`/`xgetbv`, so building with MSVC needs those
parts ported.

### To run the tests / analyse dumps (optional)

| Tool | Purpose | Install |
|---|---|---|
| Wine 9.x (`wine64`) | Run the logger and test programs on Linux | `sudo apt install wine64` |
| Xvfb + Mesa | Headless display for the D3D11 tests (wined3d) | `sudo apt install xvfb libgl1-mesa-dri` |
| Python 3.10+ | `dxbc_checksum.py`, minidump analysis | |
| `minidump` (Python) | Read `.dmp` files: threads, modules, stacks | `pip install minidump` |
| WinDbg / Visual Studio | Full analysis of `.dmp` files on Windows | |

---

## Building

```sh
cd src
./build.sh
# -> out/dinput8.dll   (default)
# -> out/version.dll   (alternative)
```

Flags: `-O2 -std=c++17 -shared -static -static-libgcc -static-libstdc++ -fno-exceptions -fno-rtti -s`,
libraries `-lpsapi -ldbghelp`. Two harmless warnings are expected (a `strncpy` truncation note and a
mingw `NtCurrentTeb` array‑bounds false positive).

Check the exports of a build:
```sh
x86_64-w64-mingw32-objdump -p out/dinput8.dll | sed -n '/Ordinal\/Name Pointer/,/^$/p'
```

---

## Installation

Game folder, e.g.
`E:\SteamLibrary\steamapps\common\Granblue Fantasy Relink Endless Ragnarok Demo\`

1. Copy **`bin/dinput8.dll`** and **`config/GBFRCrashLogger.ini`** next to the game exe.
2. For unsupported GPUs, also copy DXVK 3.1.1 **`x64/d3d11.dll`** and **`x64/dxgi.dll`**, and
   **`config/dxvk.conf`** (edit `dxvk.deviceFilter` to match your GPU name).
3. Start the game. A `CrashLogger\` folder appears next to the exe.

Don't copy `d3d12.dll`/`d3d12core.dll` or older DXVK forks into the folder.

**Uninstall:** delete `dinput8.dll` (and `d3d11.dll`, `dxgi.dll`, `dxvk.conf` to go back to native D3D11).

If no `CrashLogger` folder appears, the DLL wasn't loaded; use `bin/version.dll` instead. If another
mod already ships `dinput8.dll` (for example Relink Logs), also use `version.dll`.

---

## Configuration

### `GBFRCrashLogger.ini`

```ini
[Logger]
DebugLayer=0            ; D3D11 debug layer (native D3D11 only, needs Graphics Tools)
LogWarnings=0           ; also log debug-layer warnings
DumpShaders=0           ; save every shader (failed ones are always saved)
LogShaderCreates=0      ; log every shader creation
HangSeconds=30          ; no frames for N s => hang report (loading screens can take ~10 s)
FullDump=0              ; full-memory minidumps (GBs)
FirstChanceAVReport=0   ; full report on first access violation even if handled
MaxFirstChanceLines=100
HeartbeatSeconds=60     ; "alive" line
PerfSeconds=10          ; perf line: fps, 1% low, stutters, VRAM, CPU per thread
StutterMs=100           ; log each frame >= N ms
ProbeD3D=0              ; hidden early test device (auto-off with DXVK present)
StartupHangSeconds=25   ; no D3D11 device after N s => startup-hang report

[Environment]           ; set for the game process only, before Vulkan starts
VK_LOADER_DRIVERS_DISABLE=*igvk*

[BlockDlls]             ; DLLs the game may not load (name=0 to allow again)
nvapi64.dll=1
```

Machines without an Intel iGPU can leave the `[Environment]` line. It only affects drivers whose
manifest name contains `igvk`.

### `dxvk.conf`

| Option | Value | Why |
|---|---|---|
| `d3d11.relaxedGraphicsBarriers` | `True` | From DXVK's Relink profile (game spams pixel‑shader UAVs) |
| `d3d11.exposeDriverCommandLists` | `False` | From DXVK's Relink profile (AGS assumption) |
| `dxgi.hideNvidiaGpu` | `False` | From DXVK's Relink profile |
| `dxvk.enableNvRawAccessChains` | `False` | NVIDIA GPU‑hang workaround ([DXVK #5785](https://github.com/doitsujin/dxvk/issues/5785)) |
| `dxvk.deviceFilter` | `"GTX 960M"` | Optimus laptop: only expose the NVIDIA GPU (quote values with spaces) |
| `d3d11.maxFeatureLevel` | `12_1` | Shaders need ≥ 11_1 |
| `dxgi.maxFrameRate` | `25` | Even 40 ms pacing on the 960M at 720p (GPU needs ~44 ms; 20 is fully even) |
| `dxgi.syncInterval` | `0` | Avoid 60 Hz vsync snapping 25 FPS into a 30/20 pattern |
| `dxvk.hud` | `fps,frametimes,gpuload` | Light overlay; the full list is `fps,frametimes,gpuload,memory,drawcalls,submissions,pipelines,compiler,version` |

**Test profiles** (`config/profiles/`). Copy one over `dxvk.conf`, then restart the game:

| Profile | Purpose |
|---|---|
| A_baseline | Measurement HUD, vsync off |
| B_fl11_1 | Cap feature level at 11_1 |
| C_fl11_1_relaxed | B + relaxed compute barriers + 4× AF |
| D_vram_budget | Report 3 GB VRAM (`dxgi.maxDeviceMemory = 3072`) |
| E_steady25 | 25 FPS limiter, light HUD (**recommended at 720p on the 960M**) |

**In‑game settings for low‑end GPUs:** Resolution 1280×720, quality Standard or lower, Lighting
Quality lowest, frame cap 30 (DXVK's limiter does the pacing). Graphics options the menu doesn't
have (shadow quality, TAA off, LOD) are available through the community
[GBFRelinkFix](https://zff.dev/kolleje/GBFRelinkFix) (ASI loader, `winmm.dll`; untested with the
demo exe).

**NVIDIA shader cache:** the driver default is already 16 GB on current drivers, so nothing to change.
The DXVK cache lives in `%LOCALAPPDATA%\dxvk`, NVIDIA's in `%LOCALAPPDATA%\NVIDIA\DXCache` and `GLCache`.

---

## Using the logs

`CrashLogger\log_<date>.txt` (path of the newest one is in `latest_log_name.txt`):

```
Env set  : VK_LOADER_DRIVERS_DISABLE=*igvk*
BlockDlls: nvapi64.dll (5 loader imports hooked in the exe)
D3D11CreateDevice(adapter=NVIDIA GeForce GTX 960M, ... featureLevels=[c100 b100 b000 ])
Game D3D11 device uses adapter: NVIDIA GeForce GTX 960M (VEN_10DE DEV_139B) FL=0xc100
Blocked LoadLibraryExW("nvapi64.dll") per [BlockDlls] - the game sees it as missing (like Proton)
perf: 22.4 fps avg | median 44.5 ms | 1% low 21.3 fps | worst 50 ms | stutters >50ms: 0, >100ms: 0 |
      shaders 404 | VRAM 2801/3548 MB (79%) sysmem-for-GPU 136 MB | RAM free 2663 MB |
      CPU 18% of 8 cores, busiest: thread(T3344) 17% dxvk-cs(T31284) 15% thread(T1912) 14%
stutter: 136 ms frame (shaders created during it: 60) <- shader compilation
```

| Pattern | Meaning |
|---|---|
| `CreateCSShader FAILED … E_INVALIDARG` | Feature‑level problem (blocker 1) |
| Log ends without `Process exiting` | Killed / fast‑fail rather than a normal exit |
| `HANG DETECTED: no D3D11 device … startup` | Startup deadlock; check the thread list for DXVK/Vulkan/driver modules |
| Steady median, no thread near 100% | GPU‑bound: lower resolution/settings |
| A named thread near 100% (`dxvk-cs`, main thread) | CPU/driver‑bound |
| Stutter with `<- shader compilation` | First visit to an area; gone once the disk cache is warm |
| Large stutters while `RAM free` drops under ~2 GB | System RAM paging; close background apps |
| VRAM ≥ 95% with growing `sysmem-for-GPU` | VRAM overcommit: lower textures or use profile D |

Minidumps (`*_crash_N.dmp`, `*_hang_N.dmp`, `*_device_removed_N.dmp`) open in Visual Studio or
WinDbg, or with `python -m minidump`.

---

## Results

GTX 960M (4 GB), i7‑6700HQ, 16 GB RAM, Windows 11, NVIDIA 582.78, demo exe 1.0.1.0.

| Configuration | Outcome |
|---|---|
| Native D3D11 | DOF compute shaders fail (FL 11_0) |
| DXVK 2.7.1‑gplasync fork | Stalled after device creation |
| DXVK 3.1.1, no logger fixes | Startup deadlock (Intel Vulkan driver) |
| + `VK_LOADER_DRIVERS_DISABLE=*igvk*` | Starts; 3D scene invisible |
| + `[BlockDlls] nvapi64.dll` | **Renders correctly** |
| 1080p gameplay | 12.6 FPS steady (≈79 ms/frame), VRAM 83%, GPU‑bound |
| 720p gameplay | 21–25 FPS (median 40–48 ms), VRAM 79%, no CPU thread > 23% |
| Menus / cutscenes | 59.5 FPS (vsync‑capped) |

For comparison, the Steam Deck (a GPU in a similar class) is reported to hold about 30 FPS at 720p
with Standard settings ([Dexerto](https://www.dexerto.com/tech/granblue-fantasy-relink-steam-deck-2505526/)).

---

## Testing

The logger was validated under Wine 9 + Xvfb with the programs in `tests/`:

```sh
x86_64-w64-mingw32-gcc  tests/test.c     -o test.exe   -lversion
x86_64-w64-mingw32-g++  tests/f2test.cpp -o f2test.exe -ld3d11 -ldxgi -lversion -static
cp src/out/version.dll .                       # tests import version.dll
export WINEDLLOVERRIDES="version=n,b" DISPLAY=:99
Xvfb :99 & wine64 test.exe av                  # -> CrashLogger/log_*.txt + .dmp
wine64 f2test.exe                              # -> perf + stutter lines
```

| Test | Checks |
|---|---|
| `test.exe av` / `ud` | AV report, illegal‑instruction ISA hint, filter chaining, minidump |
| `d3dtest.exe bad/hang/crash` | Device/swapchain hooks, failed shader saved, hang report, crash during D3D |
| `f2test.exe` | `CreateDXGIFactory2` path, Present hook, perf/stutter detection |
| `blk.exe` | `LoadLibraryW/ExW("nvapi64.dll")` blocked, others unaffected |
| `envt.exe` | `[Environment]` visible via `GetEnvironmentVariable` |
| `sleeper.exe` | Startup‑hang report with ProbeD3D forced off |
| `dxbc_checksum.py` | Reproduces the stored checksum of real game shaders |

Wine's `wined3d` doesn't validate D3D11 like a real driver, so feature‑level behaviour was
verified on the target machine.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| No `CrashLogger` folder | DLL not loaded: use `version.dll`, check nothing else owns `dinput8.dll` |
| Game only in Task Manager "background processes" | Check `[Environment] VK_LOADER_DRIVERS_DISABLE` is present; look for a startup‑hang report |
| Invisible characters/terrain under DXVK | Check `Blocked LoadLibraryExW("nvapi64.dll")` appears in the log |
| DXVK not used (no overlay) | `d3d11.dll`/`dxgi.dll` must be the **x64** files next to the exe |
| `deviceFilter` truncated in DXVK log | Quote values with spaces: `"GTX 960M"` |
| Can't overwrite `dinput8.dll` | Game still running; end it in Task Manager |
| Random stutters at 720p | Use profile E (or `dxgi.maxFrameRate = 20`), close RAM‑heavy apps |

---

## Known limitations

- Performance is **GPU‑bound** on the 960M. The logger and DXVK settings can even out pacing, but
  can't create GPU throughput. Lower resolution/settings or engine tweaks (GBFRelinkFix) are the
  remaining levers.
- A D3D11 game on Windows can't stream from SSD into VRAM the way a console does. Windows already
  spills VRAM to system RAM and RAM to the pagefile, which adds stutter rather than removing it.
- `DebugLayer` works only on native D3D11 and only if the exe imports `D3D11CreateDevice`.
- Single‑player/offline use only. Not tested with online play or anti‑cheat.
- Tested on one machine (GTX 960M + HD 530). Other GPUs may need a different `deviceFilter` and
  may not need the Intel workaround.

---

## Roadmap

- [ ] Test GBFRelinkFix shadow/TAA options on the demo exe and record FPS
- [ ] GPU‑time measurement in the perf line (timestamp queries) to separate GPU cost from pacing
- [ ] Generalise for other D3D11/D3D12 titles (the original goal: a D3D12 → D3D11‑level wrapper for
      Capcom RE Engine games; the earlier feature‑level proxy lives in
      `../d3d12ProxyEdrDx11_0-master`)

---

## Credits and licences

- **DXVK**, Philip Rebohle and contributors, zlib licence. Not bundled here; download from the
  official releases.
- **NVIDIA NVAPI**, setting IDs referenced from the public `NvApiDriverSettings.h` (MIT); no NVAPI
  code is included.
- **Vulkan Loader**, Khronos; behaviour of `VK_LOADER_DRIVERS_DISABLE` per loader source.
- **GBFRelinkFix**, Lyall (original) and kolleje (expansion fork), referenced only.
- Earlier D3D12 feature‑level proxy idea: *d3d12ProxyEdrDx11_0* by tuffee88.
- GBFR Crash Logger: this project. Add a licence of your choice (for example MIT) before publishing.

---

## Disclaimer

Unofficial, not affiliated with Cygames, NVIDIA or Valve. The kit doesn't modify game files; it
loads alongside the game the same way common mod loaders do. Use offline and at your own risk.
