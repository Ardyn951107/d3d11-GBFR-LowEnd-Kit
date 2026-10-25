// ============================================================================
// GBFR Crash Logger  -  diagnostic proxy DLL for Granblue Fantasy: Relink
//                       (and the Endless Ragnarok demo), x64 Windows.
//
// HOW IT GETS LOADED
//   Built twice from this one file: as dinput8.dll (default; the game imports
//   DINPUT8.dll) and as version.dll (alternative). Windows loads DLLs from the
//   game folder first, so ours is loaded instead of the system one. Every real
//   export is forwarded to the genuine System32 DLL via tiny jump stubs, so the
//   game behaves exactly as before.
//
// WHAT IT DOES
//   * Logging        - CPU instruction sets, GPUs/drivers, D3D11 device and
//                      swapchain creation, failed shaders (saved as .cso).
//   * Crash reports  - vectored + unhandled exception handlers: faulting
//                      module+offset, registers, call stack, ISA hint, minidump.
//   * Hang reports   - startup hang, frames stopped / stuck in Present, window
//                      not responding: every thread's location + minidump.
//   * GPU loss       - DXGI device removed/hung reason + minidump.
//   * Compatibility  - [Environment] ini section: per-process environment
//                      variables (VK_LOADER_DRIVERS_DISABLE=*igvk* fixes the
//                      DXVK + old Intel Vulkan driver startup deadlock).
//                    - [BlockDlls] ini section: make DLLs look missing
//                      (nvapi64.dll, like Proton) so the game uses its plain
//                      D3D11 path, which renders correctly under DXVK.
//
// FILES IT WRITES (next to the DLL):  CrashLogger\log_<date>.txt, *.dmp,
//   CrashLogger\shaders\*.cso, GBFRCrashLogger.ini (default created once).
//
// Build: build.sh (mingw-w64 cross compiler) - see README.md.
// ============================================================================

#define __USE_MINGW_ANSI_STDIO 1
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <cpuid.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>

#define LOGGER_VERSION "1.0.6"

// ---------------------------------------------------------------------------
// Proxy forwarding
// ---------------------------------------------------------------------------
extern "C" void* g_fwd[32];
void* g_fwd[32];

#if defined(PROXY_DINPUT8)
#define PROXY_DLL_NAME L"dinput8.dll"
#define PROXY_EXPORTS(X) \
    X(0, DirectInput8Create) X(1, DllCanUnloadNow) X(2, DllGetClassObject) \
    X(3, DllRegisterServer) X(4, DllUnregisterServer) X(5, GetdfDIJoystick)
#else
#define PROXY_DLL_NAME L"version.dll"
#define PROXY_EXPORTS(X) \
    X(0, GetFileVersionInfoA) X(1, GetFileVersionInfoByHandle) X(2, GetFileVersionInfoExA) \
    X(3, GetFileVersionInfoExW) X(4, GetFileVersionInfoSizeA) X(5, GetFileVersionInfoSizeExA) \
    X(6, GetFileVersionInfoSizeExW) X(7, GetFileVersionInfoSizeW) X(8, GetFileVersionInfoW) \
    X(9, VerFindFileA) X(10, VerFindFileW) X(11, VerInstallFileA) X(12, VerInstallFileW) \
    X(13, VerLanguageNameA) X(14, VerLanguageNameW) X(15, VerQueryValueA) X(16, VerQueryValueW)
#endif

// For each export X(i, Name) this emits an assembly stub "fw_Name: jmp [g_fwd+8*i]".
// The .def file exports Name -> fw_Name, so a call from the game jumps straight
// into the real System32 function with all registers/stack untouched (no need to
// know each function's signature). g_fwd is filled by InitForwarding().
#define STUB(i, name) asm(".text\n.globl fw_" #name "\nfw_" #name ":\n\tjmp *(g_fwd+8*" #i ")(%rip)\n");
PROXY_EXPORTS(STUB)
#define NAME(i, name) #name,
static const char* g_fwdNames[] = { PROXY_EXPORTS(NAME) };
static const int g_fwdCount = sizeof(g_fwdNames) / sizeof(g_fwdNames[0]);
static HMODULE g_realDll;
// ----------------------------------------------------------------------------
// Fallback target for a proxied export the real system DLL doesn't have.
// Returns 0 with ERROR_PROC_NOT_FOUND instead of jumping to a null pointer.
// ----------------------------------------------------------------------------
static uintptr_t MissingExport() { SetLastError(ERROR_PROC_NOT_FOUND); return 0; }

// ---------------------------------------------------------------------------
// Globals / config
// ---------------------------------------------------------------------------
// Settings from the [Logger] section of GBFRCrashLogger.ini (see LoadConfig).
struct Config {
    int debugLayer = 0;          // add D3D11_CREATE_DEVICE_DEBUG (needs Windows "Graphics Tools")
    int logWarnings = 0;         // also log debug-layer warnings
    int dumpShaders = 0;         // write every created shader to shaders\ folder
    int logShaderCreates = 0;    // log every shader creation (noisy)
    int hangSeconds = 30;        // no Present / window not responding for N s => hang report
    int fullDump = 0;            // full-memory minidumps (large)
    int firstChanceAVReport = 0; // full report on first-chance access violations
    int maxFirstChance = 100;    // max first-chance lines in log
    int heartbeatSeconds = 60;   // periodic "alive" line
    int perfSeconds = 10;        // performance summary interval (fps, 1% low, stutters, VRAM); 0 = off
    int stutterMs = 100;         // frames longer than this are logged individually (max 100 lines)
    int probeD3D = 1;            // create a tiny hidden device to hook vtables early
    int startupHangSeconds = 25; // no game D3D11 device after N s => startup hang report
};
static Config g_cfg;

static HMODULE g_self;
static wchar_t g_dir[MAX_PATH];      // folder of this DLL
static wchar_t g_outDir[MAX_PATH];   // CrashLogger folder
static wchar_t g_stamp[64];
static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logCs;
static CRITICAL_SECTION g_hookCs;
static CRITICAL_SECTION g_symCs;
static volatile LONG g_inReport;
static volatile LONG g_reportCount;
static DWORD g_workerTid;

static struct CpuInfo {
    char vendor[13], brand[49];
    unsigned family, model, stepping;
    bool sse3, ssse3, sse41, sse42, popcnt, aes, avx, fma, f16c, avx2, bmi1, bmi2,
         avx512f, avx512dq, avx512bw, avx512vl, sha, osxsave, osAvx, osAvx512;
} g_cpu;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// Writes raw bytes to the log file and flushes immediately so nothing is lost if the
// game dies a moment later. Uses a *try*-lock: inside a crash handler another thread may
// hold the lock forever, so after ~200 ms it writes without it rather than deadlocking.
// ----------------------------------------------------------------------------
static void LogWrite(const char* s, DWORD n) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    bool locked = false;
    for (int i = 0; i < 200 && !(locked = TryEnterCriticalSection(&g_logCs)); i++) Sleep(1);
    DWORD w; WriteFile(g_log, s, n, &w, nullptr);
    FlushFileBuffers(g_log);
    if (locked) LeaveCriticalSection(&g_logCs);
}

// ----------------------------------------------------------------------------
// printf-style logging. Every line gets a [hh:mm:ss.mmm Tthread] prefix and CRLF ending.
// ----------------------------------------------------------------------------
static void Log(const char* fmt, ...) {
    char buf[4096];
    SYSTEMTIME t; GetLocalTime(&t);
    int n = snprintf(buf, sizeof(buf), "[%02u:%02u:%02u.%03u T%5lu] ", t.wHour, t.wMinute, t.wSecond,
                     t.wMilliseconds, GetCurrentThreadId());
    va_list ap; va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof(buf) - n - 3, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    n += (m < (int)sizeof(buf) - n - 3) ? m : (int)sizeof(buf) - n - 3;
    buf[n++] = '\r'; buf[n++] = '\n';
    LogWrite(buf, n);
}

// ----------------------------------------------------------------------------
// Converts a UTF-16 (Windows wide) string to UTF-8 into a caller buffer. Returns out.
// ----------------------------------------------------------------------------
static const char* W2U(const wchar_t* w, char* out, int cap) {
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, nullptr, nullptr)) out[0] = 0;
    return out;
}

// ---------------------------------------------------------------------------
// Safe memory helpers & module table (lock free, usable in crash handler)
// ---------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// Reads memory from our own process via ReadProcessMemory. Unlike a normal pointer
// dereference this simply fails on bad addresses instead of raising a new exception,
// so it is safe to use while handling a crash.
// ----------------------------------------------------------------------------
static bool SafeRead(const void* addr, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), addr, out, n, &got) && got == n;
}

struct ModEntry { uintptr_t base, size; char name[64]; };
static ModEntry g_mods[1024];
static int g_modCount;

// ----------------------------------------------------------------------------
// Snapshots every loaded module (name, base address, size) into g_mods.
// Uses EnumProcessModules/GetModuleInformation, which read the PEB without taking the
// loader lock, so it is usable from crash and hang handlers.
// ----------------------------------------------------------------------------
static void BuildModuleTable() {
    HMODULE hm[1024]; DWORD need = 0;
    g_modCount = 0;
    if (!EnumProcessModules(GetCurrentProcess(), hm, sizeof(hm), &need)) return;
    int n = (int)(need / sizeof(HMODULE)); if (n > 1024) n = 1024;
    for (int i = 0; i < n; i++) {
        MODULEINFO mi{};
        if (!GetModuleInformation(GetCurrentProcess(), hm[i], &mi, sizeof(mi))) continue;
        wchar_t path[MAX_PATH]; char u[MAX_PATH * 3];
        if (!GetModuleFileNameExW(GetCurrentProcess(), hm[i], path, MAX_PATH)) path[0] = 0;
        const wchar_t* fn = wcsrchr(path, L'\\'); fn = fn ? fn + 1 : path;
        ModEntry& e = g_mods[g_modCount++];
        e.base = (uintptr_t)mi.lpBaseOfDll; e.size = mi.SizeOfImage;
        W2U(fn, u, sizeof(u)); strncpy(e.name, u, sizeof(e.name) - 1); e.name[sizeof(e.name) - 1] = 0;
    }
}

// ----------------------------------------------------------------------------
// Returns the module that contains address a, or nullptr (JIT code, heap, garbage).
// ----------------------------------------------------------------------------
static const ModEntry* FindMod(uintptr_t a) {
    for (int i = 0; i < g_modCount; i++)
        if (a >= g_mods[i].base && a < g_mods[i].base + g_mods[i].size) return &g_mods[i];
    return nullptr;
}

// ----------------------------------------------------------------------------
// Formats an address as "module.dll+0xOFFSET" - offsets stay the same between runs,
// unlike raw addresses, so they can be compared and looked up in a disassembler.
// ----------------------------------------------------------------------------
static void FmtAddr(uintptr_t a, char* out, size_t cap) {
    const ModEntry* m = FindMod(a);
    if (m) snprintf(out, cap, "%s+0x%llx", m->name, (unsigned long long)(a - m->base));
    else snprintf(out, cap, "0x%016llx (no module)", (unsigned long long)a);
}

// dbghelp (preloaded by worker so the crash handler never calls LoadLibrary)
typedef BOOL(WINAPI* PFN_SymInitializeW)(HANDLE, PCWSTR, BOOL);
typedef DWORD(WINAPI* PFN_SymSetOptions)(DWORD);
typedef BOOL(WINAPI* PFN_SymFromAddr)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
typedef BOOL(WINAPI* PFN_SymRefreshModuleList)(HANDLE);
typedef BOOL(WINAPI* PFN_MiniDumpWriteDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                            PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
static PFN_SymFromAddr pSymFromAddr;
static PFN_SymRefreshModuleList pSymRefresh;
static PFN_MiniDumpWriteDump pMiniDump;
static bool g_symOk;

// ----------------------------------------------------------------------------
// Adds a " [Function+0xNN]" symbol name via dbghelp when one is available (exports of
// system DLLs, or PDBs if present). Silently does nothing if dbghelp is busy/unavailable.
// ----------------------------------------------------------------------------
static void SymName(uintptr_t a, char* out, size_t cap) {
    out[0] = 0;
    if (!g_symOk || !pSymFromAddr) return;
    if (!TryEnterCriticalSection(&g_symCs)) return;
    alignas(SYMBOL_INFO) char sb[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* si = (SYMBOL_INFO*)sb; memset(sb, 0, sizeof(sb));
    si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (pSymFromAddr(GetCurrentProcess(), a, &disp, si))
        snprintf(out, cap, " [%s+0x%llx]", si->Name, (unsigned long long)disp);
    LeaveCriticalSection(&g_symCs);
}

// ---------------------------------------------------------------------------
// Breadcrumbs: recent D3D operations, so a crash/hang shows what was going on
// ---------------------------------------------------------------------------
struct Crumb { volatile LONG seq; DWORD tid; ULONGLONG tick; char op[16]; char info[48]; volatile LONG done; LONG hr; };
static Crumb g_crumbs[256];
static volatile LONG g_crumbSeq;

// ----------------------------------------------------------------------------
// Breadcrumbs: records the start of a D3D operation (shader creation, device creation,
// ResizeBuffers...) in a 256-entry ring buffer. Returns a sequence id for CrumbEnd.
// A crash/hang report prints the recent crumbs, and any that never finished.
// ----------------------------------------------------------------------------
static int CrumbBegin(const char* op, const char* info) {
    LONG s = InterlockedIncrement(&g_crumbSeq);
    Crumb& c = g_crumbs[s & 255];
    c.done = 0; c.tid = GetCurrentThreadId(); c.tick = GetTickCount64(); c.hr = 0;
    strncpy(c.op, op, 15); c.op[15] = 0; strncpy(c.info, info, 47); c.info[47] = 0;
    c.seq = s;
    return s;
}
// ----------------------------------------------------------------------------
// Marks breadcrumb s as finished and stores its HRESULT.
// ----------------------------------------------------------------------------
static void CrumbEnd(int s, HRESULT hr) {
    Crumb& c = g_crumbs[s & 255];
    if (c.seq == s) { c.hr = hr; c.done = 1; }
}

// Present / swapchain stats
static volatile LONG64 g_presentCount;
// Frame-time ring buffer written by the render thread in PresentEnd, read by the Watchdog.
static float g_frameMs[8192];
static volatile LONG64 g_frameIdx;          // total frames timed (index = g_frameIdx & 8191)
static LARGE_INTEGER g_qpcFreq, g_lastPresentQpc;
static IDXGIAdapter3* volatile g_adapter3;  // for VRAM usage (QueryVideoMemoryInfo)
static volatile LONG g_stutterLogged;
static volatile ULONGLONG g_lastPresentTick;
static volatile ULONGLONG g_presentBeginTick;
static volatile DWORD g_presentTid;
static volatile LONG g_shaderCount, g_shaderFails;
static volatile LONG g_removedReported;
static volatile LONG g_gameDeviceSeen;
static ID3D11InfoQueue* volatile g_infoQueue;
static volatile LONG g_infoMsgs;
static ULONGLONG g_startTick;

// ----------------------------------------------------------------------------
// Prints the last 40 breadcrumbs. Entries of the faulting thread that are still
// "IN PROGRESS" are marked with >> - that is the D3D call the thread died inside.
// ----------------------------------------------------------------------------
static void PrintCrumbs(DWORD highlightTid) {
    LONG last = g_crumbSeq;
    if (last == 0) { Log("  (no D3D operations recorded)"); return; }
    Log("  Recent D3D operations (oldest first):");
    LONG first = last - 39; if (first < 1) first = 1;
    ULONGLONG now = GetTickCount64();
    for (LONG s = first; s <= last; s++) {
        Crumb& c = g_crumbs[s & 255];
        if (c.seq != s) continue;
        Log("   %s T%5lu %-14s %-40s %s hr=0x%08lx (%llums ago)",
            (c.tid == highlightTid && !c.done) ? ">>" : "  ", c.tid, c.op, c.info,
            c.done ? "done" : "IN PROGRESS", (unsigned long)c.hr, (unsigned long long)(now - c.tick));
    }
    for (LONG s = last; s >= first; s--) {
        Crumb& c = g_crumbs[s & 255];
        if (c.seq == s && !c.done && c.tid == highlightTid) {
            Log("  !! The faulting thread was INSIDE %s (%s) when it failed.", c.op, c.info);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// Human-readable name for a Windows exception / NTSTATUS code.
// ----------------------------------------------------------------------------
static const char* ExcName(DWORD c) {
    switch (c) {
    case 0xC0000005: return "ACCESS_VIOLATION";
    case 0xC000001D: return "ILLEGAL_INSTRUCTION (CPU doesn't support an instruction - e.g. AVX2/AVX-512)";
    case 0xC0000096: return "PRIVILEGED_INSTRUCTION";
    case 0xC00000FD: return "STACK_OVERFLOW";
    case 0xC0000094: return "INTEGER_DIVIDE_BY_ZERO";
    case 0xC0000095: return "INTEGER_OVERFLOW";
    case 0xC000008C: return "ARRAY_BOUNDS_EXCEEDED";
    case 0xC0000374: return "HEAP_CORRUPTION";
    case 0xC0000409: return "STACK_BUFFER_OVERRUN / FAST_FAIL";
    case 0xC0000006: return "IN_PAGE_ERROR";
    case 0x80000002: return "DATATYPE_MISALIGNMENT";
    case 0xC0000025: return "NONCONTINUABLE_EXCEPTION";
    case 0xC0000026: return "INVALID_DISPOSITION";
    case 0xC000041D: return "FATAL_USER_CALLBACK_EXCEPTION";
    case 0xC0000417: return "INVALID_CRUNTIME_PARAMETER";
    case 0xE06D7363: return "C++ EXCEPTION (unhandled throw)";
    case 0x80000003: return "BREAKPOINT";
    default: return "UNKNOWN";
    }
}

// ----------------------------------------------------------------------------
// True for exception codes that normally mean a real crash (access violation, illegal
// instruction, stack overflow, heap corruption...). Benign codes such as C++ throws that
// the game catches, or debugger messages, are ignored by the first-chance handler.
// ----------------------------------------------------------------------------
static bool IsFatalCode(DWORD c) {
    switch (c) {
    case 0xC0000005: case 0xC000001D: case 0xC0000096: case 0xC00000FD: case 0xC0000094:
    case 0xC0000095: case 0xC000008C: case 0xC0000374: case 0xC0000409: case 0xC0000006:
    case 0x80000002: case 0xC0000025: case 0xC0000026: case 0xC000041D: case 0xC0000417:
        return true;
    }
    return false;
}

// ----------------------------------------------------------------------------
// Human-readable name for common DXGI/D3D HRESULTs (device removed/hung, etc.).
// ----------------------------------------------------------------------------
static const char* HrName(HRESULT hr) {
    switch ((unsigned long)hr) {
    case 0x00000000: return "S_OK";
    case 0x887A0001: return "DXGI_ERROR_INVALID_CALL";
    case 0x887A0005: return "DXGI_ERROR_DEVICE_REMOVED";
    case 0x887A0006: return "DXGI_ERROR_DEVICE_HUNG (GPU timed out - often a shader that runs too long / driver hang)";
    case 0x887A0007: return "DXGI_ERROR_DEVICE_RESET";
    case 0x887A0020: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    case 0x887A002D: return "DXGI_ERROR_SDK_COMPONENT_MISSING (debug layer not installed)";
    case 0x887A0004: return "DXGI_ERROR_UNSUPPORTED";
    case 0x8007000E: return "E_OUTOFMEMORY";
    case 0x80070057: return "E_INVALIDARG";
    case 0x80004005: return "E_FAIL";
    case 0x087A0001: return "DXGI_STATUS_OCCLUDED";
    default: return "";
    }
}

// ---------------------------------------------------------------------------
// CPU / system info
// ---------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// Reads XCR0 (the xgetbv instruction): tells whether the OS has enabled saving of
// AVX / AVX-512 registers. A CPU can support AVX but still fault if the OS hasn't.
// ----------------------------------------------------------------------------
static unsigned long long XGetBV0() {
    unsigned eax, edx;
    asm volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(eax), "=d"(edx) : "c"(0));
    return ((unsigned long long)edx << 32) | eax;
}

// ----------------------------------------------------------------------------
// Fills g_cpu with vendor, brand string, family/model/stepping and the instruction-set
// flags games commonly require (SSE4.x, AVX, AVX2, FMA3, F16C, BMI, AVX-512...).
// ----------------------------------------------------------------------------
static void DetectCpu() {
    unsigned a, b, c, d, maxLeaf;
    __cpuid(0, maxLeaf, b, c, d);
    memcpy(g_cpu.vendor, &b, 4); memcpy(g_cpu.vendor + 4, &d, 4); memcpy(g_cpu.vendor + 8, &c, 4);
    g_cpu.vendor[12] = 0;
    __cpuid(1, a, b, c, d);
    unsigned fam = (a >> 8) & 0xF, mod = (a >> 4) & 0xF;
    if (fam == 0xF) fam += (a >> 20) & 0xFF;
    if (fam == 0x6 || fam >= 0xF) mod |= ((a >> 16) & 0xF) << 4;
    g_cpu.family = fam; g_cpu.model = mod; g_cpu.stepping = a & 0xF;
    g_cpu.sse3 = c & 1; g_cpu.ssse3 = c & (1 << 9); g_cpu.fma = c & (1 << 12); g_cpu.sse41 = c & (1 << 19);
    g_cpu.sse42 = c & (1 << 20); g_cpu.popcnt = c & (1 << 23); g_cpu.aes = c & (1 << 25);
    g_cpu.osxsave = c & (1 << 27); g_cpu.avx = c & (1 << 28); g_cpu.f16c = c & (1 << 29);
    if (g_cpu.osxsave) {
        unsigned long long x = XGetBV0();
        g_cpu.osAvx = (x & 6) == 6; g_cpu.osAvx512 = (x & 0xE6) == 0xE6;
    }
    if (maxLeaf >= 7) {
        __cpuid_count(7, 0, a, b, c, d);
        g_cpu.bmi1 = b & (1 << 3); g_cpu.avx2 = b & (1 << 5); g_cpu.bmi2 = b & (1 << 8);
        g_cpu.avx512f = b & (1 << 16); g_cpu.avx512dq = b & (1 << 17); g_cpu.sha = b & (1 << 29);
        g_cpu.avx512bw = b & (1 << 30); g_cpu.avx512vl = b & (1u << 31);
    }
    unsigned maxExt; __cpuid(0x80000000, maxExt, b, c, d);
    g_cpu.brand[0] = 0;
    if (maxExt >= 0x80000004) {
        unsigned* p = (unsigned*)g_cpu.brand;
        for (unsigned i = 0; i < 3; i++) __cpuid(0x80000002 + i, p[i * 4], p[i * 4 + 1], p[i * 4 + 2], p[i * 4 + 3]);
        g_cpu.brand[48] = 0;
    }
}

#define YN(x) ((x) ? "yes" : "NO")
// ----------------------------------------------------------------------------
// Writes the log header: exe path, PID, command line, Windows build (and whether we
// are under Wine/Proton), CPU model + instruction sets, logical cores and RAM.
// ----------------------------------------------------------------------------
static void LogSystemInfo() {
    char u[MAX_PATH * 3]; wchar_t w[MAX_PATH];
    GetModuleFileNameW(nullptr, w, MAX_PATH);
    Log("Game exe : %s", W2U(w, u, sizeof(u)));
    Log("PID      : %lu", GetCurrentProcessId());
    Log("Cmdline  : %s", W2U(GetCommandLineW(), u, sizeof(u)));

    typedef LONG(WINAPI * PFN_RtlGetVersion)(OSVERSIONINFOEXW*);
    auto rgv = (PFN_RtlGetVersion)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOEXW vi{}; vi.dwOSVersionInfoSize = sizeof(vi);
    if (rgv && rgv(&vi) == 0)
        Log("Windows  : %lu.%lu build %lu (%s)", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber,
            vi.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10 or older");
    const char* wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") ? " (running under Wine/Proton)" : "";
    if (*wine) Log("Note     :%s", wine);

    Log("CPU      : %s | %s family %u model 0x%X stepping %u", g_cpu.brand, g_cpu.vendor, g_cpu.family, g_cpu.model, g_cpu.stepping);
    Log("CPU ISA  : SSE3=%s SSSE3=%s SSE4.1=%s SSE4.2=%s POPCNT=%s AES=%s", YN(g_cpu.sse3), YN(g_cpu.ssse3),
        YN(g_cpu.sse41), YN(g_cpu.sse42), YN(g_cpu.popcnt), YN(g_cpu.aes));
    Log("CPU ISA  : AVX=%s(OS:%s) FMA3=%s F16C=%s AVX2=%s BMI1=%s BMI2=%s SHA=%s", YN(g_cpu.avx), YN(g_cpu.osAvx),
        YN(g_cpu.fma), YN(g_cpu.f16c), YN(g_cpu.avx2), YN(g_cpu.bmi1), YN(g_cpu.bmi2), YN(g_cpu.sha));
    Log("CPU ISA  : AVX512F=%s DQ=%s BW=%s VL=%s (OS:%s)", YN(g_cpu.avx512f), YN(g_cpu.avx512dq), YN(g_cpu.avx512bw),
        YN(g_cpu.avx512vl), YN(g_cpu.osAvx512));
    SYSTEM_INFO si; GetNativeSystemInfo(&si);
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms); GlobalMemoryStatusEx(&ms);
    Log("Threads  : %lu logical | RAM %llu MB total, %llu MB free", si.dwNumberOfProcessors,
        (unsigned long long)(ms.ullTotalPhys >> 20), (unsigned long long)(ms.ullAvailPhys >> 20));
}

// ----------------------------------------------------------------------------
// Logs the game exe's file version (useful to tell demo/patch builds apart).
// Uses the real system version.dll directly, not our proxy exports.
// ----------------------------------------------------------------------------
static void LogExeVersion() {
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH); wcscat(sys, L"\\version.dll");
    HMODULE v = LoadLibraryW(sys);
    if (!v) return;
    typedef DWORD(WINAPI * GSZ)(LPCWSTR, LPDWORD);
    typedef BOOL(WINAPI * GVI)(LPCWSTR, DWORD, DWORD, LPVOID);
    typedef BOOL(WINAPI * VQV)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    auto gsz = (GSZ)GetProcAddress(v, "GetFileVersionInfoSizeW");
    auto gvi = (GVI)GetProcAddress(v, "GetFileVersionInfoW");
    auto vqv = (VQV)GetProcAddress(v, "VerQueryValueW");
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    DWORD h = 0, sz = gsz ? gsz(exe, &h) : 0;
    if (sz && sz < 1 << 20) {
        static BYTE buf[1 << 16];
        if (sz <= sizeof(buf) && gvi(exe, 0, sz, buf)) {
            VS_FIXEDFILEINFO* fi; UINT len;
            if (vqv(buf, L"\\", (LPVOID*)&fi, &len) && fi)
                Log("Exe ver  : %u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                    HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
        }
    }
}

// ---------------------------------------------------------------------------
// Stack walking
// ---------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// True if address a is in committed, executable memory.
// ----------------------------------------------------------------------------
static bool IsExecutable(uintptr_t a) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)a, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD p = mbi.Protect & 0xFF;
    return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

// ----------------------------------------------------------------------------
// Heuristic used when scanning raw stack memory: a value is probably a return address
// if it points into executable code of a known module AND the bytes just before it
// encode a CALL instruction (E8 rel32 or FF /2).
// ----------------------------------------------------------------------------
static bool LooksLikeReturnAddr(uintptr_t a) {
    if (a < 0x10000 || !FindMod(a) || !IsExecutable(a)) return false;
    unsigned char b[8];
    if (!SafeRead((void*)(a - 8), b, 8)) return false;
    if (b[3] == 0xE8) return true;                                // call rel32
    for (int k = 2; k <= 7; k++)                                  // call r/m (FF /2)
        if (b[8 - k] == 0xFF && ((b[8 - k + 1] >> 3) & 7) == 2) return true;
    return false;
}

// ----------------------------------------------------------------------------
// Proper x64 stack walk starting from a CONTEXT, using the unwind tables in each module
// (RtlLookupFunctionEntry + RtlVirtualUnwind). Stops if the stack pointer leaves the
// thread's stack, to avoid reading garbage on a corrupted stack.
// ----------------------------------------------------------------------------
static void PrintUnwind(const CONTEXT* start, int maxFrames) {
    CONTEXT ctx = *start;
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    uintptr_t lo = (uintptr_t)tib->StackLimit, hi = (uintptr_t)tib->StackBase;
    // If this context isn't on our current stack (e.g. stack overflow guard), be lenient.
    if (ctx.Rsp < lo || ctx.Rsp >= hi) { lo = ctx.Rsp - 0x100000; hi = ctx.Rsp + 0x100000; }
    char a[160], s[300];
    for (int i = 0; i < maxFrames; i++) {
        uintptr_t pc = ctx.Rip;
        if (!pc) break;
        FmtAddr(pc, a, sizeof(a)); SymName(pc, s, sizeof(s));
        Log("    #%02d %s%s", i, a, s);
        if (ctx.Rsp < lo || ctx.Rsp >= hi) { Log("    (stack pointer out of range - stopping)"); break; }
        DWORD64 imgBase = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(pc, &imgBase, nullptr);
        if (!fe) {
            uintptr_t ret;
            if (!SafeRead((void*)ctx.Rsp, &ret, 8)) break;
            ctx.Rip = ret; ctx.Rsp += 8;
        } else {
            PVOID hd; DWORD64 est;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imgBase, pc, fe, &ctx, &hd, &est, nullptr);
        }
        if (ctx.Rip == pc && i > 0) break;
    }
}

// ----------------------------------------------------------------------------
// Fallback stack trace: reads up to 8 KB of raw stack from rsp and lists values that
// look like return addresses. Useful when the real unwind breaks (e.g. inside drivers).
// ----------------------------------------------------------------------------
static void PrintStackScan(uintptr_t rsp, int maxHits) {
    static uintptr_t buf[1024];
    size_t got = 0;
    // read as much as is readable, in 4KB pages
    size_t first = 4096 - (rsp & 0xFFF);
    if (SafeRead((void*)rsp, buf, first)) {
        got = first;
        while (got + 4096 <= sizeof(buf) && SafeRead((void*)(rsp + got), (char*)buf + got, 4096)) got += 4096;
    }
    int hits = 0; char a[160], s[300];
    for (size_t i = 0; i < got / 8 && hits < maxHits; i++) {
        if (LooksLikeReturnAddr(buf[i])) {
            FmtAddr(buf[i], a, sizeof(a)); SymName(buf[i], s, sizeof(s));
            Log("    [rsp+0x%04zx] %s%s", i * 8, a, s);
            hits++;
        }
    }
    if (!hits) Log("    (nothing found)");
}

// ----------------------------------------------------------------------------
// Dumps the bytes at the faulting instruction and classifies them (VEX = AVX/AVX2/FMA,
// EVEX = AVX-512, UD2 = deliberate trap...). Next to the CPU's real flags this shows at a
// glance whether an ILLEGAL_INSTRUCTION crash is a missing-CPU-feature problem.
// ----------------------------------------------------------------------------
static void PrintIsaHint(uintptr_t rip) {
    unsigned char b[32];
    if (!SafeRead((void*)rip, b, sizeof(b))) { Log("  Bytes at RIP: <unreadable>"); return; }
    char hex[128]; int n = 0;
    for (int i = 0; i < 24; i++) n += snprintf(hex + n, sizeof(hex) - n, "%02X ", b[i]);
    Log("  Bytes at RIP: %s", hex);
    int i = 0;
    while (i < 8 && (b[i] == 0x66 || b[i] == 0x67 || b[i] == 0xF2 || b[i] == 0xF3 || b[i] == 0x2E ||
                     b[i] == 0x3E || b[i] == 0x26 || b[i] == 0x64 || b[i] == 0x65 || b[i] == 0x36 || b[i] == 0xF0)) i++;
    const char* hint = nullptr;
    if (b[i] == 0xC4 || b[i] == 0xC5) hint = "VEX-encoded instruction (AVX / AVX2 / FMA3 / BMI / F16C)";
    else if (b[i] == 0x62) hint = "EVEX-encoded instruction (AVX-512)";
    else {
        if (b[i] >= 0x40 && b[i] <= 0x4F) i++;
        if (b[i] == 0x0F && b[i + 1] == 0x0B) hint = "UD2 - deliberate trap (game/driver assert)";
        else if (b[i] == 0x0F && (b[i + 1] == 0x38 || b[i + 1] == 0x3A)) hint = "SSSE3/SSE4.x/AES/SHA-class instruction";
        else if (b[i] == 0xF3 && b[i + 1] == 0x0F && b[i + 2] == 0xB8) hint = "POPCNT";
    }
    if (hint) {
        Log("  Instruction class: %s", hint);
        Log("  This CPU: AVX=%s(OS %s) AVX2=%s FMA3=%s F16C=%s BMI2=%s AVX512F=%s SSE4.2=%s", YN(g_cpu.avx),
            YN(g_cpu.osAvx), YN(g_cpu.avx2), YN(g_cpu.fma), YN(g_cpu.f16c), YN(g_cpu.bmi2), YN(g_cpu.avx512f),
            YN(g_cpu.sse42));
    }
}

// ----------------------------------------------------------------------------
// Lists every loaded module with base and size (spot overlays/injectors/wrappers).
// ----------------------------------------------------------------------------
static void PrintModules() {
    Log("  Loaded modules (%d):", g_modCount);
    for (int i = 0; i < g_modCount; i++)
        Log("    %016llx %08llx %s", (unsigned long long)g_mods[i].base, (unsigned long long)g_mods[i].size, g_mods[i].name);
}

// ----------------------------------------------------------------------------
// Gets a thread's description (SetThreadDescription name) if the game set one.
// ----------------------------------------------------------------------------
static void ThreadName(DWORD tid, char* out, size_t cap) {
    out[0] = 0;
    typedef HRESULT(WINAPI * PFN_GTD)(HANDLE, PWSTR*);
    static PFN_GTD gtd = (PFN_GTD)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "GetThreadDescription");
    if (!gtd) return;
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!h) return;
    PWSTR d = nullptr;
    if (SUCCEEDED(gtd(h, &d)) && d) { if (*d) W2U(d, out, (int)cap); LocalFree(d); }
    CloseHandle(h);
}

// ---------------------------------------------------------------------------
// Minidump (written from a helper thread)
// ---------------------------------------------------------------------------
struct DumpReq { EXCEPTION_POINTERS* ep; DWORD tid; wchar_t path[MAX_PATH]; BOOL ok; };
// ----------------------------------------------------------------------------
// Thread body that actually writes the .dmp file with MiniDumpWriteDump. Dumps are
// written from a separate thread because writing a dump of the crashing thread from
// itself is unreliable. Small dump by default, full memory if FullDump=1.
// ----------------------------------------------------------------------------
static DWORD WINAPI DumpThread(LPVOID p) {
    DumpReq* r = (DumpReq*)p;
    if (!pMiniDump) return 0;
    HANDLE f = CreateFileW(r->path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    MINIDUMP_EXCEPTION_INFORMATION mei{ r->tid, r->ep, FALSE };
    DWORD type = MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                 MiniDumpWithHandleData | MiniDumpWithProcessThreadData | MiniDumpWithDataSegs;
    if (g_cfg.fullDump) type |= MiniDumpWithFullMemory | MiniDumpWithFullMemoryInfo;
    r->ok = pMiniDump(GetCurrentProcess(), GetCurrentProcessId(), f, (MINIDUMP_TYPE)type, r->ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(f);
    return 0;
}

static CRITICAL_SECTION g_dumpCs;
static volatile LONG g_hangDumps;
// ----------------------------------------------------------------------------
// Requests a minidump (crash / hang / device_removed) and waits up to 60 s for it.
// Serialised by a lock so a crash dump and a hang dump never collide, and rate-limited
// (max 6 dumps per session, max 2 hang dumps) so a broken session can't fill the disk.
// ----------------------------------------------------------------------------
static void WriteDump(EXCEPTION_POINTERS* ep, const wchar_t* tag) {
    if (!wcscmp(tag, L"hang") && InterlockedIncrement(&g_hangDumps) > 2) { Log("  (hang dump limit reached, text report only)"); return; }
    LONG n = InterlockedIncrement(&g_reportCount);
    if (n > 6) { Log("  (dump limit reached, skipping minidump)"); return; }
    EnterCriticalSection(&g_dumpCs);
    static DumpReq r;
    r.ep = ep; r.tid = GetCurrentThreadId(); r.ok = FALSE;
    swprintf(r.path, MAX_PATH, L"%ls\\%ls_%ls_%ld.dmp", g_outDir, g_stamp, tag, n);
    HANDLE t = CreateThread(nullptr, 0, DumpThread, &r, 0, nullptr);
    if (t) { WaitForSingleObject(t, 60000); CloseHandle(t); }
    char u[MAX_PATH * 3];
    Log("  Minidump: %s (%s)", W2U(r.path, u, sizeof(u)), r.ok ? "written" : "FAILED");
    LeaveCriticalSection(&g_dumpCs);
}

// ---------------------------------------------------------------------------
// Exception reporting
// ---------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// Prints the general-purpose registers of a CPU context.
// ----------------------------------------------------------------------------
static void LogRegisters(const CONTEXT* c) {
    Log("  RAX=%016llx RBX=%016llx RCX=%016llx RDX=%016llx", c->Rax, c->Rbx, c->Rcx, c->Rdx);
    Log("  RSI=%016llx RDI=%016llx RBP=%016llx RSP=%016llx", c->Rsi, c->Rdi, c->Rbp, c->Rsp);
    Log("  R8 =%016llx R9 =%016llx R10=%016llx R11=%016llx", c->R8, c->R9, c->R10, c->R11);
    Log("  R12=%016llx R13=%016llx R14=%016llx R15=%016llx", c->R12, c->R13, c->R14, c->R15);
    Log("  RIP=%016llx EFLAGS=%08lx MXCSR=%08lx", c->Rip, c->EFlags, c->MxCsr);
}

// ----------------------------------------------------------------------------
// Prints frame counter, time since the last Present, shader counts and uptime, and
// whether a thread is currently stuck inside Present().
// ----------------------------------------------------------------------------
static void PresentStats() {
    ULONGLONG now = GetTickCount64();
    Log("  Frames presented: %lld | last Present %llu ms ago | shaders created: %ld (failed %ld) | uptime %llu s",
        (long long)g_presentCount, g_lastPresentTick ? (unsigned long long)(now - g_lastPresentTick) : 0ULL,
        g_shaderCount, g_shaderFails, (unsigned long long)((now - g_startTick) / 1000));
    if (g_presentBeginTick)
        Log("  !! Thread T%lu has been inside Present for %llu ms", g_presentTid, (unsigned long long)(now - g_presentBeginTick));
}

static volatile LONG g_crashed;
// ----------------------------------------------------------------------------
// The full crash report: exception code + meaning, faulting module+offset, what an
// access violation tried to touch, registers, instruction bytes + ISA hint, unwound call
// stack, heuristic stack scan, frame stats, breadcrumbs, minidump and module list.
// ----------------------------------------------------------------------------
static void ReportException(EXCEPTION_POINTERS* ep, const char* kind, bool dump) {
    g_crashed = 1;
    BuildModuleTable();
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    CONTEXT* c = ep->ContextRecord;
    char a[160], s[300], tn[128];
    ThreadName(GetCurrentThreadId(), tn, sizeof(tn));
    Log("==================================================================");
    Log("=== %s", kind);
    Log("==================================================================");
    Log("  Exception: 0x%08lx %s", er->ExceptionCode, ExcName(er->ExceptionCode));
    FmtAddr((uintptr_t)er->ExceptionAddress, a, sizeof(a)); SymName((uintptr_t)er->ExceptionAddress, s, sizeof(s));
    Log("  Address  : %s%s", a, s);
    Log("  Thread   : %lu %s", GetCurrentThreadId(), tn);
    if (er->ExceptionCode == 0xC0000005 && er->NumberParameters >= 2) {
        const char* op = er->ExceptionInformation[0] == 0 ? "READ from" : er->ExceptionInformation[0] == 1 ? "WRITE to" : "EXECUTE (DEP) at";
        Log("  Detail   : tried to %s 0x%016llx%s", op, (unsigned long long)er->ExceptionInformation[1],
            er->ExceptionInformation[1] < 0x10000 ? "  <- NULL pointer (+offset)" : "");
    }
    if (er->ExceptionCode == 0xC0000006 && er->NumberParameters >= 3)
        Log("  Detail   : in-page error NTSTATUS 0x%08llx at 0x%016llx", (unsigned long long)er->ExceptionInformation[2],
            (unsigned long long)er->ExceptionInformation[1]);
    LogRegisters(c);
    PrintIsaHint(c->Rip);
    Log("  Call stack (unwound):");
    PrintUnwind(c, 48);
    Log("  Possible callers found on stack (heuristic):");
    PrintStackScan(c->Rsp, 24);
    PresentStats();
    PrintCrumbs(GetCurrentThreadId());
    if (dump) WriteDump(ep, L"crash");
    PrintModules();
    Log("=== end of report ===");
}

// ---------------------------------------------------------------------------
// Exception handlers
// ---------------------------------------------------------------------------
static volatile LONG g_fcCount;
static uintptr_t g_fcSeen[256];
static volatile LONG g_fcSeenN;
static uintptr_t g_fullReported[16];
static volatile LONG g_fullReportedN;

// ----------------------------------------------------------------------------
// True if a full report was already written for this fault address (avoids writing the
// same report twice: once at first chance, again when it becomes unhandled).
// ----------------------------------------------------------------------------
static bool AlreadyFull(uintptr_t a) {
    for (LONG i = 0; i < g_fullReportedN && i < 16; i++) if (g_fullReported[i] == a) return true;
    return false;
}

// ----------------------------------------------------------------------------
// Vectored exception handler - runs FIRST for every exception in the process, before
// the game's own __try/__except blocks. For fatal codes it either writes a full report
// immediately (illegal instruction, heap corruption, stack overflow, fast-fail, or AV when
// FirstChanceAVReport=1) or a short one-line note plus 6 stack frames (deduplicated by
// address), then always lets the exception continue normally.
// ----------------------------------------------------------------------------
static LONG CALLBACK VehHandler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!IsFatalCode(code)) return EXCEPTION_CONTINUE_SEARCH;
    if (GetCurrentThreadId() == g_workerTid && g_inReport) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t addr = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;

    bool seen = false;
    for (LONG i = 0; i < g_fcSeenN && i < 256; i++) if (g_fcSeen[i] == addr) { seen = true; break; }
    if (!seen) { LONG k = InterlockedIncrement(&g_fcSeenN) - 1; if (k < 256) g_fcSeen[k] = addr; }

    bool fullNow = code == 0xC000001D || code == 0xC0000096 || code == 0xC0000374 || code == 0xC00000FD ||
                   code == 0xC0000409 || (code == 0xC0000005 && g_cfg.firstChanceAVReport);
    if (fullNow && !AlreadyFull(addr) && InterlockedExchange(&g_inReport, 1) == 0) {
        LONG k = InterlockedIncrement(&g_fullReportedN) - 1; if (k < 16) g_fullReported[k] = addr;
        ReportException(ep, "FIRST-CHANCE FATAL EXCEPTION (likely the crash)", true);
        g_inReport = 0;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (!seen && InterlockedIncrement(&g_fcCount) <= g_cfg.maxFirstChance) {
        BuildModuleTable();
        char a[160], s[300];
        FmtAddr(addr, a, sizeof(a)); SymName(addr, s, sizeof(s));
        EXCEPTION_RECORD* er = ep->ExceptionRecord;
        if (code == 0xC0000005 && er->NumberParameters >= 2)
            Log("[first-chance] 0x%08lx %s at %s%s (%s 0x%llx) - may be handled by the game",
                code, ExcName(code), a, s, er->ExceptionInformation[0] == 1 ? "write" : er->ExceptionInformation[0] == 8 ? "exec" : "read",
                (unsigned long long)er->ExceptionInformation[1]);
        else
            Log("[first-chance] 0x%08lx %s at %s%s - may be handled by the game", code, ExcName(code), a, s);
        PrintUnwind(ep->ContextRecord, 6);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LPTOP_LEVEL_EXCEPTION_FILTER g_gameFilter;

// ----------------------------------------------------------------------------
// Top-level (unhandled) exception filter: the game is about to crash. Writes the full
// report + dump, then hands the crash to the game's own crash handler if it set one.
// ----------------------------------------------------------------------------
static LONG WINAPI OurUnhandledFilter(EXCEPTION_POINTERS* ep) {
    if (InterlockedExchange(&g_inReport, 1) == 0) {
        uintptr_t addr = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
        if (AlreadyFull(addr)) {
            Log("=== UNHANDLED EXCEPTION 0x%08lx - same as the first-chance report above; the game is now crashing.",
                ep->ExceptionRecord->ExceptionCode);
        } else {
            ReportException(ep, "UNHANDLED EXCEPTION (GAME CRASH)", true);
        }
        g_inReport = 0;
    }
    if (g_gameFilter) {
        Log("  Passing crash on to the game's own crash handler...");
        return g_gameFilter(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ----------------------------------------------------------------------------
// Replacement for SetUnhandledExceptionFilter in the game's import table. Games (and
// their crash reporters) call it to install their own filter, which would remove ours.
// We store their filter in g_gameFilter, keep ours installed, and chain to theirs.
// ----------------------------------------------------------------------------
static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI HkSetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f) {
    LPTOP_LEVEL_EXCEPTION_FILTER prev = g_gameFilter;
    if (f == OurUnhandledFilter) return prev;
    g_gameFilter = f;
    BuildModuleTable();
    char a[160]; FmtAddr((uintptr_t)f, a, sizeof(a));
    Log("Game installed its own crash filter (%s) - keeping ours on top and chaining to it", f ? a : "NULL");
    return prev;
}

// ---------------------------------------------------------------------------
// IAT / vtable patching
// ---------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// Import Address Table hook: in module `mod`, replaces every import of `func` from DLLs
// whose name starts with dllPrefix by newFn (original optionally returned in origOut).
// Only affects calls made by that module - nothing is patched inside system DLLs.
// ----------------------------------------------------------------------------
static int PatchIat(HMODULE mod, const char* dllPrefix, const char* func, void* newFn, void** origOut) {
    BYTE* base = (BYTE*)mod;
    auto dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    int patched = 0;
    size_t plen = strlen(dllPrefix);
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        const char* n = (const char*)(base + imp->Name);
        if (_strnicmp(n, dllPrefix, plen)) continue;
        auto iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        auto oft = imp->OriginalFirstThunk ? (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk) : nullptr;
        for (size_t i = 0; iat[i].u1.Function; i++) {
            bool match = false;
            if (oft && !IMAGE_SNAP_BY_ORDINAL(oft[i].u1.Ordinal)) {
                auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft[i].u1.AddressOfData);
                match = !strcmp((const char*)ibn->Name, func);
            }
            if (!match) continue;
            DWORD old;
            if (!VirtualProtect(&iat[i], sizeof(void*), PAGE_READWRITE, &old)) continue;
            if (origOut && !*origOut && (void*)iat[i].u1.Function != newFn) *origOut = (void*)iat[i].u1.Function;
            iat[i].u1.Function = (ULONGLONG)newFn;
            VirtualProtect(&iat[i], sizeof(void*), old, &old);
            patched++;
        }
    }
    return patched;
}

struct VtRec { void** vt; void* orig[32]; };
static VtRec g_devVts[8], g_scVts[8], g_facVts[8];
static int g_devVtN, g_scVtN, g_facVtN;

// ----------------------------------------------------------------------------
// Finds (or creates) the bookkeeping record for a COM vtable we have patched.
// ----------------------------------------------------------------------------
static VtRec* GetRec(VtRec* recs, int& n, void** vt, bool create) {
    for (int i = 0; i < n; i++) if (recs[i].vt == vt) return &recs[i];
    if (!create || n >= 8) return nullptr;
    recs[n].vt = vt; memset(recs[n].orig, 0, sizeof(recs[n].orig));
    return &recs[n++];
}

// ----------------------------------------------------------------------------
// Returns the ORIGINAL function pointer for vtable slot idx of object obj, so a hook can
// call the real implementation. Objects of different classes have different vtables,
// hence one record per vtable.
// ----------------------------------------------------------------------------
static void* OrigFor(VtRec* recs, int n, void* obj, int idx) {
    void** vt = *(void***)obj;
    for (int i = 0; i < n; i++) if (recs[i].vt == vt && recs[i].orig[idx]) return recs[i].orig[idx];
    for (int i = 0; i < n; i++) if (recs[i].orig[idx]) return recs[i].orig[idx];  // fallback
    return nullptr;
}

// ----------------------------------------------------------------------------
// Vtable hook: replaces slot idx of a COM vtable with fn and remembers the original.
// Vtables are shared by all objects of a class, so one patch covers every instance.
// ----------------------------------------------------------------------------
static bool PatchVt(VtRec* rec, int idx, void* fn) {
    void** vt = rec->vt;
    if (vt[idx] == fn) return false;
    DWORD old;
    if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old)) return false;
    rec->orig[idx] = vt[idx];
    vt[idx] = fn;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
    return true;
}

// ---------------------------------------------------------------------------
// D3D11 / DXGI hooks
// ---------------------------------------------------------------------------
static volatile LONG g_probing;

// ----------------------------------------------------------------------------
// Identifies a shader blob: for DXBC bytecode, the 128-bit checksum stored in its header
// (stable per shader); otherwise a 64-bit FNV-1a hash of the bytes.
// ----------------------------------------------------------------------------
static void DxbcHash(const void* bc, SIZE_T len, char* out) {
    const unsigned char* p = (const unsigned char*)bc;
    if (p && len >= 20 && !memcmp(p, "DXBC", 4)) {
        for (int i = 0; i < 16; i++) sprintf(out + i * 2, "%02x", p[4 + i]);
    } else {
        unsigned long long h = 1469598103934665603ULL;
        for (SIZE_T i = 0; p && i < len; i++) { h ^= p[i]; h *= 1099511628211ULL; }
        sprintf(out, "fnv%016llx", h);
    }
}

// ----------------------------------------------------------------------------
// Saves shader bytecode to CrashLogger\\shaders\\<type>_<hash>.cso (once per shader).
// ----------------------------------------------------------------------------
static void DumpShaderFile(const char* type, const char* hash, const void* bc, SIZE_T len) {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%ls\\shaders", g_outDir);
    CreateDirectoryW(dir, nullptr);
    swprintf(path, MAX_PATH, L"%ls\\%hs_%hs.cso", dir, type, hash);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(f, bc, (DWORD)len, &w, nullptr); CloseHandle(f);
}

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateShader)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, void**);
// ----------------------------------------------------------------------------
// Maps an ID3D11Device vtable slot to a shader stage name (VS/GS/PS/HS/DS/CS).
// ----------------------------------------------------------------------------
static const char* ShaderType(int idx) {
    switch (idx) { case 12: return "VS"; case 13: return "GS"; case 15: return "PS"; case 16: return "HS";
                   case 17: return "DS"; case 18: return "CS"; }
    return "??";
}

// ----------------------------------------------------------------------------
// Hook for ID3D11Device::Create{Vertex,Geometry,Pixel,Hull,Domain,Compute}Shader
// (vtable slots 12,13,15,16,17,18). Records a breadcrumb, calls the real function and
// logs + saves any shader the driver rejects. This is how we found the two depth-of-field
// compute shaders that need UAV slots u10/u11 (feature level 11_1) and fail on the
// GTX 960M's native 11_0 D3D11 device.
// ----------------------------------------------------------------------------
template <int IDX>
static HRESULT STDMETHODCALLTYPE HkCreateShader(ID3D11Device* d, const void* bc, SIZE_T len, ID3D11ClassLinkage* cl, void** out) {
    auto orig = (PFN_CreateShader)OrigFor(g_devVts, g_devVtN, d, IDX);
    char hash[40]; DxbcHash(bc, len, hash);
    char info[48]; snprintf(info, sizeof(info), "%s %s", ShaderType(IDX), hash);
    int crumb = CrumbBegin("CreateShader", info);
    HRESULT hr = orig(d, bc, len, cl, out);
    CrumbEnd(crumb, hr);
    InterlockedIncrement(&g_shaderCount);
    if (FAILED(hr)) {
        InterlockedIncrement(&g_shaderFails);
        Log("Create%sShader FAILED hr=0x%08lx %s | dxbc=%s size=%zu (saved to shaders\\)", ShaderType(IDX),
            (unsigned long)hr, HrName(hr), hash, (size_t)len);
        DumpShaderFile(ShaderType(IDX), hash, bc, len);
    } else {
        if (g_cfg.logShaderCreates) Log("Create%sShader ok dxbc=%s size=%zu", ShaderType(IDX), hash, (size_t)len);
        if (g_cfg.dumpShaders) DumpShaderFile(ShaderType(IDX), hash, bc, len);
    }
    return hr;
}

// ----------------------------------------------------------------------------
// Copies D3D11 debug-layer messages (errors/corruption, plus warnings if LogWarnings=1)
// into the log and clears the queue. Only active when DebugLayer=1 and the Windows
// 'Graphics Tools' feature is installed.
// ----------------------------------------------------------------------------
static void DrainInfoQueue(bool all) {
    ID3D11InfoQueue* q = g_infoQueue;
    if (!q) return;
    UINT64 n = q->GetNumStoredMessages();
    static char buf[8192];
    for (UINT64 i = 0; i < n; i++) {
        SIZE_T len = 0;
        if (FAILED(q->GetMessage(i, nullptr, &len)) || len > sizeof(buf)) continue;
        D3D11_MESSAGE* m = (D3D11_MESSAGE*)buf;
        if (FAILED(q->GetMessage(i, m, &len))) continue;
        bool show = m->Severity <= D3D11_MESSAGE_SEVERITY_ERROR || (m->Severity == D3D11_MESSAGE_SEVERITY_WARNING && (g_cfg.logWarnings || all));
        if (show && InterlockedIncrement(&g_infoMsgs) <= 3000) {
            const char* sev = m->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION" :
                              m->Severity == D3D11_MESSAGE_SEVERITY_ERROR ? "ERROR" :
                              m->Severity == D3D11_MESSAGE_SEVERITY_WARNING ? "WARNING" : "INFO";
            Log("[D3D11 %s #%d] %.*s", sev, (int)m->ID, (int)m->DescriptionByteLength, m->pDescription);
        }
    }
    q->ClearStoredMessages();
}

static void LogThreadsSnapshot();
// ----------------------------------------------------------------------------
// Called when Present/ResizeBuffers reports the GPU device was removed/hung/reset.
// Logs DXGI's reason (GetDeviceRemovedReason), debug messages, breadcrumbs, every
// thread's location and writes a device_removed minidump. Runs once per session.
// ----------------------------------------------------------------------------
static void OnDeviceRemoved(IUnknown* sc, HRESULT hr, const char* where) {
    if (InterlockedExchange(&g_removedReported, 1)) return;
    BuildModuleTable();
    HRESULT reason = 0;
    ID3D11Device* dev = nullptr;
    IDXGISwapChain* s = (IDXGISwapChain*)sc;
    if (s && SUCCEEDED(s->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) && dev) reason = dev->GetDeviceRemovedReason();
    Log("==================================================================");
    Log("=== GPU DEVICE REMOVED / LOST (from %s)", where);
    Log("==================================================================");
    Log("  %s returned 0x%08lx %s", where, (unsigned long)hr, HrName(hr));
    Log("  GetDeviceRemovedReason = 0x%08lx %s", (unsigned long)reason, HrName(reason));
    Log("  Meaning: the GPU driver reset the device. HUNG = a GPU job (usually a shader) took too long;");
    Log("  REMOVED/INTERNAL_ERROR = driver crash or bad command. The shader being created/used last is a suspect.");
    PresentStats();
    DrainInfoQueue(true);
    PrintCrumbs(0);
    LogThreadsSnapshot();
    WriteDump(nullptr, L"device_removed");
    if (dev) dev->Release();
}

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static void* volatile g_lastSc;
// ----------------------------------------------------------------------------
// Present hook prologue: logs the swapchain description the first time a new swapchain
// presents, and records which thread entered Present and when (for hang detection).
// ----------------------------------------------------------------------------
static void PresentBegin(IDXGISwapChain* sc) {
    if (g_lastSc != sc) {
        g_lastSc = sc;
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(sc->GetDesc(&d)))
            Log("Game swapchain %p: %ux%u fmt=%d buffers=%u windowed=%d swapEffect=%d flags=0x%x", (void*)sc,
                d.BufferDesc.Width, d.BufferDesc.Height, (int)d.BufferDesc.Format, d.BufferCount, d.Windowed,
                (int)d.SwapEffect, d.Flags);
    }
    if (!g_adapter3) {
        IDXGIDevice* dd = nullptr; IDXGIAdapter* ad = nullptr; IDXGIAdapter3* a3 = nullptr;
        if (SUCCEEDED(sc->GetDevice(__uuidof(IDXGIDevice), (void**)&dd)) && dd) {
            if (SUCCEEDED(dd->GetAdapter(&ad)) && ad) {
                if (SUCCEEDED(ad->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&a3)) && a3) g_adapter3 = a3;  // kept for the session
                ad->Release();
            }
            dd->Release();
        }
        if (!g_adapter3) g_adapter3 = (IDXGIAdapter3*)(uintptr_t)1;  // marker: tried, unavailable
    }
    g_presentTid = GetCurrentThreadId();
    g_presentBeginTick = GetTickCount64();
}
// ----------------------------------------------------------------------------
// Present hook epilogue: updates frame counter / last-frame time and routes
// device-removed style HRESULTs to OnDeviceRemoved; other failures are logged (max 20).
// ----------------------------------------------------------------------------
static void PresentEnd(IDXGISwapChain* sc, HRESULT hr, UINT flags, const char* where) {
    g_presentBeginTick = 0;
    g_lastPresentTick = GetTickCount64();
    InterlockedIncrement64(&g_presentCount);
    // frame time = time between consecutive Present() returns
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    static LONG lastShaders;
    if (g_lastPresentQpc.QuadPart) {
        float ms = (float)((now.QuadPart - g_lastPresentQpc.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart);
        LONG64 i = g_frameIdx;
        g_frameMs[i & 8191] = ms;
        g_frameIdx = i + 1;
        LONG sh = g_shaderCount;
        if (g_cfg.stutterMs > 0 && ms >= g_cfg.stutterMs && g_presentCount > 120 && InterlockedIncrement(&g_stutterLogged) <= 100)
            Log("stutter: %.0f ms frame (shaders created during it: %ld)%s", ms, sh - lastShaders,
                sh - lastShaders ? " <- shader compilation" : "");
        lastShaders = sh;
    }
    g_lastPresentQpc = now;
    if (flags & DXGI_PRESENT_TEST) return;
    if (hr == (HRESULT)0x887A0005 || hr == (HRESULT)0x887A0006 || hr == (HRESULT)0x887A0007 || hr == (HRESULT)0x887A0020)
        OnDeviceRemoved(sc, hr, where);
    else if (FAILED(hr)) {
        static volatile LONG n; if (InterlockedIncrement(&n) <= 20) Log("%s failed hr=0x%08lx %s", where, (unsigned long)hr, HrName(hr));
    }
}

// ----------------------------------------------------------------------------
// Hook for IDXGISwapChain::Present (vtable slot 8).
// ----------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE HkPresent(IDXGISwapChain* sc, UINT si, UINT fl) {
    auto orig = (PFN_Present)OrigFor(g_scVts, g_scVtN, sc, 8);
    PresentBegin(sc);
    HRESULT hr = orig(sc, si, fl);
    PresentEnd(sc, hr, fl, "Present");
    return hr;
}
// ----------------------------------------------------------------------------
// Hook for IDXGISwapChain1::Present1 (vtable slot 22).
// ----------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE HkPresent1(IDXGISwapChain1* sc, UINT si, UINT fl, const DXGI_PRESENT_PARAMETERS* pp) {
    auto orig = (PFN_Present1)OrigFor(g_scVts, g_scVtN, sc, 22);
    PresentBegin(sc);
    HRESULT hr = orig(sc, si, fl, pp);
    PresentEnd(sc, hr, fl, "Present1");
    return hr;
}
// ----------------------------------------------------------------------------
// Hook for IDXGISwapChain::ResizeBuffers (slot 13): logs resolution/format changes and
// failures (resolution switches are a common crash point).
// ----------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE HkResizeBuffers(IDXGISwapChain* sc, UINT bc, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    auto orig = (PFN_ResizeBuffers)OrigFor(g_scVts, g_scVtN, sc, 13);
    char info[48]; snprintf(info, sizeof(info), "%ux%u fmt=%d", w, h, (int)f);
    int c = CrumbBegin("ResizeBuffers", info);
    HRESULT hr = orig(sc, bc, w, h, f, fl);
    CrumbEnd(c, hr);
    Log("ResizeBuffers(%u, %ux%u, fmt=%d, flags=0x%x) -> 0x%08lx %s", bc, w, h, (int)f, fl, (unsigned long)hr, HrName(hr));
    if (hr == (HRESULT)0x887A0005 || hr == (HRESULT)0x887A0006) OnDeviceRemoved(sc, hr, "ResizeBuffers");
    return hr;
}

// ----------------------------------------------------------------------------
// Patches a swapchain's vtable: Present, ResizeBuffers and (if supported) Present1.
// ----------------------------------------------------------------------------
static void HookSwapChain(IDXGISwapChain* sc) {
    if (!sc) return;
    EnterCriticalSection(&g_hookCs);
    VtRec* r = GetRec(g_scVts, g_scVtN, *(void***)sc, true);
    if (r) {
        bool a = PatchVt(r, 8, (void*)HkPresent);
        PatchVt(r, 13, (void*)HkResizeBuffers);
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) {
            VtRec* r1 = (*(void***)sc1 == r->vt) ? r : GetRec(g_scVts, g_scVtN, *(void***)sc1, true);
            if (r1) PatchVt(r1, 22, (void*)HkPresent1);
            sc1->Release();
        }
        if (a) Log("Hooked swapchain vtable %p (Present/Present1/ResizeBuffers)", (void*)r->vt);
    }
    LeaveCriticalSection(&g_hookCs);
}

// ----------------------------------------------------------------------------
// Logs which GPU a D3D11 device actually runs on, and its feature level.
// ----------------------------------------------------------------------------
static void LogAdapterOfDevice(ID3D11Device* dev, const char* who) {
    IDXGIDevice* dd = nullptr; IDXGIAdapter* ad = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) && dd) {
        if (SUCCEEDED(dd->GetAdapter(&ad)) && ad) {
            DXGI_ADAPTER_DESC d{}; ad->GetDesc(&d);
            char u[256];
            Log("%s uses adapter: %s (VEN_%04X DEV_%04X) FL=0x%x", who, W2U(d.Description, u, sizeof(u)), d.VendorId,
                d.DeviceId, (unsigned)dev->GetFeatureLevel());
            ad->Release();
        }
        dd->Release();
    }
}

// ----------------------------------------------------------------------------
// Patches an ID3D11Device vtable so every shader creation goes through HkCreateShader.
// For the game's own device it also logs the adapter, marks 'device seen' for the
// startup watchdog and picks up the debug-layer message queue if present.
// ----------------------------------------------------------------------------
static void HookDevice(ID3D11Device* dev, bool fromGame) {
    if (!dev) return;
    EnterCriticalSection(&g_hookCs);
    VtRec* r = GetRec(g_devVts, g_devVtN, *(void***)dev, true);
    bool any = false;
    if (r) {
        any |= PatchVt(r, 12, (void*)HkCreateShader<12>);
        any |= PatchVt(r, 13, (void*)HkCreateShader<13>);
        any |= PatchVt(r, 15, (void*)HkCreateShader<15>);
        any |= PatchVt(r, 16, (void*)HkCreateShader<16>);
        any |= PatchVt(r, 17, (void*)HkCreateShader<17>);
        any |= PatchVt(r, 18, (void*)HkCreateShader<18>);
    }
    LeaveCriticalSection(&g_hookCs);
    if (any) Log("Hooked D3D11 device vtable %p (shader creation)", r ? (void*)r->vt : nullptr);
    if (fromGame) {
        g_gameDeviceSeen = 1;
        LogAdapterOfDevice(dev, "Game D3D11 device");
        if (!g_infoQueue) {
            ID3D11InfoQueue* q = nullptr;
            if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)&q)) && q) {
                q->SetMessageCountLimit(4096);
                g_infoQueue = q;
                Log("D3D11 debug layer is ACTIVE - its errors will be written to this log");
            }
        }
    }
}

// Factory hooks so every swapchain the game creates gets hooked
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);

// ----------------------------------------------------------------------------
// Given the IUnknown device passed to swapchain creation, hooks it as a D3D11 device.
// ----------------------------------------------------------------------------
static void FromDeviceUnknown(IUnknown* u) {
    if (!u || g_probing) return;
    ID3D11Device* d = nullptr;
    if (SUCCEEDED(u->QueryInterface(__uuidof(ID3D11Device), (void**)&d)) && d) { HookDevice(d, true); d->Release(); }
}

// ----------------------------------------------------------------------------
// Hook for IDXGIFactory::CreateSwapChain (slot 10): logs the request and hooks the
// resulting swapchain and its device.
// ----------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE HkCreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    auto orig = (PFN_CreateSwapChain)OrigFor(g_facVts, g_facVtN, f, 10);
    HRESULT hr = orig(f, dev, desc, out);
    if (!g_probing) {
        Log("IDXGIFactory::CreateSwapChain %ux%u swapEffect=%d -> 0x%08lx %s", desc ? desc->BufferDesc.Width : 0,
            desc ? desc->BufferDesc.Height : 0, desc ? (int)desc->SwapEffect : -1, (unsigned long)hr, HrName(hr));
        FromDeviceUnknown(dev);
    }
    if (SUCCEEDED(hr) && out && *out) HookSwapChain(*out);
    return hr;
}
// ----------------------------------------------------------------------------
// Hook for IDXGIFactory2::CreateSwapChainForHwnd (slot 15): same as above for the
// modern (flip-model) creation path the game actually uses.
// ----------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE HkCreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d,
                                                          const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** out) {
    auto orig = (PFN_CreateSwapChainForHwnd)OrigFor(g_facVts, g_facVtN, f, 15);
    HRESULT hr = orig(f, dev, hwnd, d, fs, o, out);
    if (!g_probing) {
        Log("IDXGIFactory2::CreateSwapChainForHwnd %ux%u fmt=%d swapEffect=%d -> 0x%08lx %s", d ? d->Width : 0, d ? d->Height : 0,
            d ? (int)d->Format : -1, d ? (int)d->SwapEffect : -1, (unsigned long)hr, HrName(hr));
        FromDeviceUnknown(dev);
    }
    if (SUCCEEDED(hr) && out && *out) HookSwapChain(*out);
    return hr;
}

// ----------------------------------------------------------------------------
// Patches a DXGI factory vtable (CreateSwapChain / CreateSwapChainForHwnd).
// ----------------------------------------------------------------------------
static void HookFactory(IUnknown* fac) {
    if (!fac) return;
    EnterCriticalSection(&g_hookCs);
    VtRec* r = GetRec(g_facVts, g_facVtN, *(void***)fac, true);
    bool a = false;
    if (r) {
        a |= PatchVt(r, 10, (void*)HkCreateSwapChain);
        IDXGIFactory2* f2 = nullptr;
        if (SUCCEEDED(fac->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2)) && f2) {
            VtRec* r2 = (*(void***)f2 == r->vt) ? r : GetRec(g_facVts, g_facVtN, *(void***)f2, true);
            if (r2) a |= PatchVt(r2, 15, (void*)HkCreateSwapChainForHwnd);
            f2->Release();
        }
    }
    LeaveCriticalSection(&g_hookCs);
    if (a) Log("Hooked DXGI factory vtable %p", r ? (void*)r->vt : nullptr);
}

// D3D11 creation (IAT hooks in the game exe)
typedef HRESULT(WINAPI* PFN_D3D11CreateDevice)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                               ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT(WINAPI* PFN_D3D11CreateDeviceAndSwapChain)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
                                                           UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**,
                                                           D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static PFN_D3D11CreateDevice oD3D11CreateDevice;
static PFN_D3D11CreateDeviceAndSwapChain oD3D11CreateDeviceAndSwapChain;

// ----------------------------------------------------------------------------
// Hook for CreateDXGIFactory2 in the game exe's import table (the only DXGI function the
// exe imports). Hooks the returned factory so the game's swapchain - and its Present -
// get hooked too. This works with DXVK without the logger ever creating DXGI objects
// itself (ProbeD3D stays off), so it can't cause the concurrent-init deadlock.
// ----------------------------------------------------------------------------
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory2)(UINT, REFIID, void**);
static PFN_CreateDXGIFactory2 oCreateDXGIFactory2;
static HRESULT WINAPI HkCreateDXGIFactory2(UINT flags, REFIID riid, void** out) {
    HRESULT hr = oCreateDXGIFactory2(flags, riid, out);
    if (SUCCEEDED(hr) && out && *out) HookFactory((IUnknown*)*out);
    return hr;
}

// ----------------------------------------------------------------------------
// Logs the arguments of a D3D11 device-creation call (adapter, flags, feature levels).
// ----------------------------------------------------------------------------
static void LogCreateArgs(const char* fn, IDXGIAdapter* ad, D3D_DRIVER_TYPE dt, UINT flags, const D3D_FEATURE_LEVEL* fl, UINT n) {
    char lv[128] = ""; int k = 0;
    for (UINT i = 0; fl && i < n && k < 120; i++) k += snprintf(lv + k, sizeof(lv) - k, "%x ", (unsigned)fl[i]);
    char an[256] = "default";
    if (ad) { DXGI_ADAPTER_DESC d{}; if (SUCCEEDED(ad->GetDesc(&d))) W2U(d.Description, an, sizeof(an)); }
    Log("%s(adapter=%s, driverType=%d, flags=0x%x, featureLevels=[%s])", fn, an, (int)dt, flags, fl ? lv : "default");
}

// ----------------------------------------------------------------------------
// Hook for D3D11CreateDevice in the game exe's import table. Logs the call, optionally
// adds the debug-layer flag (DebugLayer=1, falls back if not installed), and hooks the
// returned device. Logged the feature level that exposed the 11_0 vs 11_1 problem.
// ----------------------------------------------------------------------------
static HRESULT WINAPI HkD3D11CreateDevice(IDXGIAdapter* ad, D3D_DRIVER_TYPE dt, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL* fl,
                                          UINT n, UINT sdk, ID3D11Device** dev, D3D_FEATURE_LEVEL* outFl, ID3D11DeviceContext** ctx) {
    LogCreateArgs("D3D11CreateDevice", ad, dt, flags, fl, n);
    UINT f2 = flags | (g_cfg.debugLayer ? D3D11_CREATE_DEVICE_DEBUG : 0);
    int c = CrumbBegin("CreateDevice", "");
    HRESULT hr = oD3D11CreateDevice(ad, dt, sw, f2, fl, n, sdk, dev, outFl, ctx);
    if (FAILED(hr) && f2 != flags) {
        Log("  Debug-layer device failed (0x%08lx %s) - install 'Graphics Tools' optional feature. Retrying without.", (unsigned long)hr, HrName(hr));
        hr = oD3D11CreateDevice(ad, dt, sw, flags, fl, n, sdk, dev, outFl, ctx);
    }
    CrumbEnd(c, hr);
    Log("  -> 0x%08lx %s", (unsigned long)hr, HrName(hr));
    if (SUCCEEDED(hr) && dev && *dev) HookDevice(*dev, true);
    return hr;
}

// ----------------------------------------------------------------------------
// Same as HkD3D11CreateDevice for the combined device+swapchain entry point.
// ----------------------------------------------------------------------------
static HRESULT WINAPI HkD3D11CreateDeviceAndSwapChain(IDXGIAdapter* ad, D3D_DRIVER_TYPE dt, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL* fl,
                                                      UINT n, UINT sdk, const DXGI_SWAP_CHAIN_DESC* scd, IDXGISwapChain** sc,
                                                      ID3D11Device** dev, D3D_FEATURE_LEVEL* outFl, ID3D11DeviceContext** ctx) {
    LogCreateArgs("D3D11CreateDeviceAndSwapChain", ad, dt, flags, fl, n);
    UINT f2 = flags | (g_cfg.debugLayer ? D3D11_CREATE_DEVICE_DEBUG : 0);
    int c = CrumbBegin("CreateDevSwap", "");
    HRESULT hr = oD3D11CreateDeviceAndSwapChain(ad, dt, sw, f2, fl, n, sdk, scd, sc, dev, outFl, ctx);
    if (FAILED(hr) && f2 != flags) {
        Log("  Debug-layer device failed (0x%08lx %s) - retrying without.", (unsigned long)hr, HrName(hr));
        hr = oD3D11CreateDeviceAndSwapChain(ad, dt, sw, flags, fl, n, sdk, scd, sc, dev, outFl, ctx);
    }
    CrumbEnd(c, hr);
    Log("  -> 0x%08lx %s", (unsigned long)hr, HrName(hr));
    if (SUCCEEDED(hr)) {
        if (dev && *dev) HookDevice(*dev, true);
        if (sc && *sc) HookSwapChain(*sc);
    }
    return hr;
}

// Probe: tiny hidden device + swapchains so vtables are hooked no matter how the game creates them.
// ----------------------------------------------------------------------------
// Optional early setup (ProbeD3D=1): lists the system's GPUs and driver versions, then
// creates a tiny hidden device + two swapchains so device/swapchain/factory vtables are
// hooked before the game renders. Automatically DISABLED when d3d11.dll/dxgi.dll sit
// next to the exe (DXVK): initialising DXVK from a second thread at the same time as
// the game is exactly the kind of thing that deadlocked startup earlier.
// ----------------------------------------------------------------------------
static void ProbeD3D() {
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!d3d11 || !dxgi) { Log("Probe: could not load d3d11/dxgi"); return; }
    auto create = (PFN_D3D11CreateDevice)GetProcAddress(d3d11, "D3D11CreateDevice");
    typedef HRESULT(WINAPI * PFN_CF1)(REFIID, void**);
    auto cf1 = (PFN_CF1)GetProcAddress(dxgi, "CreateDXGIFactory1");

    // GPU list
    IDXGIFactory1* fac = nullptr;
    if (cf1 && SUCCEEDED(cf1(__uuidof(IDXGIFactory1), (void**)&fac)) && fac) {
        IDXGIAdapter1* ad = nullptr;
        for (UINT i = 0; fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d{}; ad->GetDesc1(&d);
            LARGE_INTEGER umd{};
            bool hasVer = SUCCEEDED(ad->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd));
            char u[256];
            Log("GPU %u    : %s | VEN_%04X DEV_%04X REV_%02X | VRAM %llu MB | driver %u.%u.%u.%u%s", i, W2U(d.Description, u, sizeof(u)),
                d.VendorId, d.DeviceId, d.Revision, (unsigned long long)(d.DedicatedVideoMemory >> 20),
                hasVer ? HIWORD(umd.HighPart) : 0, hasVer ? LOWORD(umd.HighPart) : 0, hasVer ? HIWORD(umd.LowPart) : 0,
                hasVer ? LOWORD(umd.LowPart) : 0, (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " (software)" : "");
            if (d.VendorId == 0x10DE && hasVer) {
                unsigned v = (LOWORD(umd.LowPart)) + (HIWORD(umd.LowPart) % 10) * 10000;
                Log("           NVIDIA driver version ~ %u.%02u", v / 100, v % 100);
            }
            ad->Release();
        }
        HookFactory(fac);
    }
    if (!g_cfg.probeD3D || !create) { if (fac) fac->Release(); return; }

    InterlockedExchange(&g_probing, 1);
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fls, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fls + 1, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (SUCCEEDED(hr) && dev) {
        Log("Probe device FL=0x%x", (unsigned)dev->GetFeatureLevel());
        HookDevice(dev, false);
        WNDCLASSEXW wc{ sizeof(wc) }; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = g_self; wc.lpszClassName = L"GBFRLoggerProbe";
        RegisterClassExW(&wc);
        HWND w1 = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, g_self, nullptr);
        HWND w2 = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, g_self, nullptr);
        if (fac && w1) {
            DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 1; sd.BufferDesc.Width = 64; sd.BufferDesc.Height = 64;
            sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.OutputWindow = w1; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            IDXGISwapChain* sc = nullptr;
            if (SUCCEEDED(fac->CreateSwapChain(dev, &sd, &sc)) && sc) { HookSwapChain(sc); sc->Release(); }
        }
        IDXGIFactory2* f2 = nullptr;
        if (fac && w2 && SUCCEEDED(fac->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2)) && f2) {
            DXGI_SWAP_CHAIN_DESC1 d1{}; d1.Width = 64; d1.Height = 64; d1.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d1.SampleDesc.Count = 1; d1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d1.BufferCount = 2;
            d1.SwapEffect = (DXGI_SWAP_EFFECT)4 /*FLIP_DISCARD*/;
            IDXGISwapChain1* sc1 = nullptr;
            if (SUCCEEDED(f2->CreateSwapChainForHwnd(dev, w2, &d1, nullptr, nullptr, &sc1)) && sc1) { HookSwapChain(sc1); sc1->Release(); }
            f2->Release();
        }
        if (ctx) { ctx->ClearState(); ctx->Flush(); ctx->Release(); }
        dev->Release();
        if (w1) DestroyWindow(w1);
        if (w2) DestroyWindow(w2);
    } else {
        Log("Probe device creation failed 0x%08lx %s", (unsigned long)hr, HrName(hr));
    }
    InterlockedExchange(&g_probing, 0);
    if (fac) fac->Release();
}

// ---------------------------------------------------------------------------
// Hang reporting
// ---------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// Lists every thread of the process: name, current instruction (RIP) and up to 8 likely
// callers. Each thread is suspended only long enough to copy its registers and 4 KB of
// stack; all analysis happens after it is resumed, so no lock is taken while a thread
// is frozen (avoids deadlocking the game we are diagnosing).
// ----------------------------------------------------------------------------
static void LogThreadsSnapshot() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{ sizeof(te) };
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    static uintptr_t stackCopy[512];
    Log("  Threads (RIP + likely callers from stack):");
    int count = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        if (++count > 160) break;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        CONTEXT c{}; c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        size_t got = 0; bool okc = false;
        if (SuspendThread(h) != (DWORD)-1) {
            okc = GetThreadContext(h, &c);
            if (okc) { for (got = 0; got < sizeof(stackCopy) && SafeRead((char*)c.Rsp + got, (char*)stackCopy + got, 512); got += 512); }
            ResumeThread(h);
        }
        CloseHandle(h);
        if (!okc) continue;
        char a[160], tn[128], line[1400]; int n = 0;
        ThreadName(te.th32ThreadID, tn, sizeof(tn));
        FmtAddr(c.Rip, a, sizeof(a));
        Log("   T%5lu %-24s RIP %s", te.th32ThreadID, tn, a);
        int hits = 0;
        for (size_t i = 0; i < got / 8 && hits < 8; i++) {
            if (LooksLikeReturnAddr(stackCopy[i])) {
                FmtAddr(stackCopy[i], a, sizeof(a));
                n += snprintf(line + n, sizeof(line) - n, "%s%s", hits ? " <- " : "", a);
                hits++;
            }
        }
        if (hits) Log("          %s", line);
    }
    CloseHandle(snap);
}

// ----------------------------------------------------------------------------
// Hang report: frame stats, breadcrumbs, debug messages, all thread locations + a hang
// minidump. This is what pinpointed the DXVK / Intel Vulkan driver deadlock.
// ----------------------------------------------------------------------------
static void ReportHang(const char* why) {
    BuildModuleTable();
    Log("==================================================================");
    Log("=== HANG DETECTED: %s", why);
    Log("==================================================================");
    PresentStats();
    DrainInfoQueue(true);
    PrintCrumbs(g_presentTid);
    LogThreadsSnapshot();
    WriteDump(nullptr, L"hang");
    Log("=== end of hang report (game may still recover) ===");
}

struct FindWnd { DWORD pid; HWND hwnd; };
// ----------------------------------------------------------------------------
// EnumWindows callback: finds the game's main visible top-level window.
// ----------------------------------------------------------------------------
static BOOL CALLBACK EnumWndCb(HWND h, LPARAM lp) {
    FindWnd* f = (FindWnd*)lp; DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != f->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    wchar_t cls[64]; GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"GBFRLoggerProbe")) return TRUE;
    f->hwnd = h; return FALSE;
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// Applies the SetUnhandledExceptionFilter IAT hook to the exe and every non-Windows
// DLL loaded so far (re-run every 5 s to catch DLLs loaded later).
// ----------------------------------------------------------------------------
static void PatchUefInModules(bool all) {
    HMODULE hm[512]; DWORD need = 0;
    wchar_t win[MAX_PATH]; GetWindowsDirectoryW(win, MAX_PATH); size_t wl = wcslen(win);
    if (!EnumProcessModules(GetCurrentProcess(), hm, sizeof(hm), &need)) return;
    int n = (int)(need / sizeof(HMODULE)); if (n > 512) n = 512;
    for (int i = 0; i < n; i++) {
        if (hm[i] == g_self) continue;
        if (i > 0 && !all) break;
        wchar_t p[MAX_PATH]; GetModuleFileNameW(hm[i], p, MAX_PATH);
        if (!_wcsnicmp(p, win, wl)) continue;  // skip system DLLs
        int k = PatchIat(hm[i], "kernel32", "SetUnhandledExceptionFilter", (void*)HkSetUnhandledExceptionFilter, nullptr);
        k += PatchIat(hm[i], "api-ms-win-core-errorhandling", "SetUnhandledExceptionFilter", (void*)HkSetUnhandledExceptionFilter, nullptr);
        (void)k;
    }
}

static DWORD WINAPI Watchdog(LPVOID);
// ----------------------------------------------------------------------------
// Setup thread (started from DllMain, because DllMain must not do heavy work):
// preloads dbghelp for symbols + minidumps, logs the exe version and runs ProbeD3D.
// ----------------------------------------------------------------------------
static DWORD WINAPI Worker(LPVOID) {
    // dbghelp
    wchar_t p[MAX_PATH]; GetSystemDirectoryW(p, MAX_PATH); wcscat(p, L"\\dbghelp.dll");
    HMODULE dbg = LoadLibraryW(p);
    if (dbg) {
        pMiniDump = (PFN_MiniDumpWriteDump)GetProcAddress(dbg, "MiniDumpWriteDump");
        pSymFromAddr = (PFN_SymFromAddr)GetProcAddress(dbg, "SymFromAddr");
        pSymRefresh = (PFN_SymRefreshModuleList)GetProcAddress(dbg, "SymRefreshModuleList");
        auto sso = (PFN_SymSetOptions)GetProcAddress(dbg, "SymSetOptions");
        auto si = (PFN_SymInitializeW)GetProcAddress(dbg, "SymInitializeW");
        if (sso) sso(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_NO_PROMPTS | SYMOPT_FAIL_CRITICAL_ERRORS);
        if (si && si(GetCurrentProcess(), nullptr, TRUE)) g_symOk = true;
    }
    Log("dbghelp: %s, minidumps %s", g_symOk ? "symbols ok" : "no symbols", pMiniDump ? "available" : "UNAVAILABLE");
    LogExeVersion();
    if (g_cfg.probeD3D) ProbeD3D();
    else Log("ProbeD3D=0: not touching dxgi/d3d11 from the logger (GPU list comes from the game's own device)");
    Log("Setup complete - waiting for game. (hang timeout %d s)", g_cfg.hangSeconds);
    return 0;
}

// ----------------------------------------------------------------------------
// Monitoring thread, separate from Worker so it keeps working even if setup blocks.
// Every 250 ms: drains debug messages, refreshes symbols, re-applies the crash-filter
// hook, and detects hangs - startup hang (no D3D11 device after StartupHangSeconds),
// frames stopped / stuck inside Present, or window 'Not Responding'. Writes a heartbeat
// (fps, working set, free RAM) every HeartbeatSeconds.
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// CPU usage sampler for the perf line: whole-process CPU % (of all cores) and the
// three busiest threads (% of ONE core, with their names - DXVK names its threads,
// e.g. dxvk-cs / dxvk-submit / dxvk-shader). A thread near 100% means that thread is
// the bottleneck (CPU/driver-bound); all threads well below 100% while fps is low
// means the GPU is the bottleneck.
// ----------------------------------------------------------------------------
struct ThrSample { DWORD tid; ULONGLONG t; };
static ThrSample g_thrPrev[1024];
static int g_thrPrevN;
static ULONGLONG g_procPrev, g_cpuWallPrev;
static void CpuSample(char* out, size_t cap) {
    out[0] = 0;
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    ULONGLONG wall100ns = (ULONGLONG)(q.QuadPart * 10000000.0 / (double)g_qpcFreq.QuadPart);
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    ULONGLONG proc = ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
    ULONGLONG dWall = wall100ns - g_cpuWallPrev;
    static ThrSample cur[1024]; int curN = 0;
    struct Top { DWORD tid; double pct; } top[3] = {};
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{ sizeof(te) };
        DWORD pid = GetCurrentProcessId();
        for (BOOL ok = Thread32First(snap, &te); ok && curN < 1024; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            if (GetThreadTimes(h, &c, &e, &k, &u)) {
                ULONGLONG t = ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
                cur[curN].tid = te.th32ThreadID; cur[curN].t = t; curN++;
                if (g_cpuWallPrev && dWall) {
                    for (int i = 0; i < g_thrPrevN; i++) if (g_thrPrev[i].tid == te.th32ThreadID) {
                        double pct = 100.0 * (double)(t - g_thrPrev[i].t) / (double)dWall;
                        for (int j = 0; j < 3; j++) if (pct > top[j].pct) {
                            for (int m = 2; m > j; m--) top[m] = top[m - 1];
                            top[j].tid = te.th32ThreadID; top[j].pct = pct; break;
                        }
                        break;
                    }
                }
            }
            CloseHandle(h);
        }
        CloseHandle(snap);
    }
    if (g_cpuWallPrev && dWall) {
        SYSTEM_INFO si; GetSystemInfo(&si);
        int n = snprintf(out, cap, " | CPU %.0f%% of %lu cores, busiest:", 100.0 * (double)(proc - g_procPrev) / (double)dWall / si.dwNumberOfProcessors,
                         si.dwNumberOfProcessors);
        for (int j = 0; j < 3 && top[j].tid && n < (int)cap - 1; j++) {
            char tn[64]; ThreadName(top[j].tid, tn, sizeof(tn));
            n += snprintf(out + n, cap - n, " %s(T%lu) %.0f%%", tn[0] ? tn : "thread", top[j].tid, top[j].pct);
        }
    }
    memcpy(g_thrPrev, cur, sizeof(ThrSample) * curN); g_thrPrevN = curN;
    g_procPrev = proc; g_cpuWallPrev = wall100ns;
}

static DWORD WINAPI Watchdog(LPVOID) {
    g_workerTid = GetCurrentThreadId();

    ULONGLONG lastBeat = GetTickCount64(), lastRepatch = 0, hungSince = 0, presentWarned = 0;
    LONG64 lastBeatFrames = 0;
    bool stallReported = false, windowReported = false, noPresentWarned = false;
    int symRefreshCountdown = 20;
    for (;;) {
        Sleep(250);
        ULONGLONG now = GetTickCount64();
        DrainInfoQueue(false);
        if (--symRefreshCountdown <= 0) {
            symRefreshCountdown = 40;
            if (g_symOk && pSymRefresh && TryEnterCriticalSection(&g_symCs)) { pSymRefresh(GetCurrentProcess()); LeaveCriticalSection(&g_symCs); }
        }
        if (now - lastRepatch > 5000) { lastRepatch = now; PatchUefInModules(true); }

        FindWnd fw{ GetCurrentProcessId(), nullptr };
        EnumWindows(EnumWndCb, (LPARAM)&fw);
        HWND gameWnd = fw.hwnd;
        // Present stall / stuck in Present
        ULONGLONG hangMs = (ULONGLONG)g_cfg.hangSeconds * 1000;
        if (g_presentCount > 0) {
            ULONGLONG begin = g_presentBeginTick;
            bool stuck = begin && now - begin > hangMs;
            bool stalled = now - g_lastPresentTick > hangMs;
            if ((stuck || stalled) && !stallReported && !g_crashed && !(gameWnd && IsIconic(gameWnd))) {
                stallReported = true;
                if (InterlockedCompareExchange(&g_inReport, 1, 0) == 0) {
                    ReportHang(stuck ? "a thread is stuck inside Present() (GPU/driver hang?)"
                                     : "no frames presented for a while (game/render thread stalled - ignore if you paused or alt-tabbed)");
                    g_inReport = 0;
                }
            } else if (!stuck && !stalled && stallReported) {
                stallReported = false;
                Log("Frames resumed.");
            }
        }
        // startup hang: game never got to creating its D3D11 device
        static bool startupReported = false;
        if (!startupReported && !g_gameDeviceSeen && !g_crashed && g_cfg.startupHangSeconds > 0 &&
            now - g_startTick > (ULONGLONG)g_cfg.startupHangSeconds * 1000) {
            startupReported = true;
            if (InterlockedCompareExchange(&g_inReport, 1, 0) == 0) {
                char why[160];
                snprintf(why, sizeof(why), "no D3D11 device created %d s after launch%s (stuck during startup)",
                         g_cfg.startupHangSeconds, gameWnd ? "" : ", no game window yet");
                ReportHang(why);
                g_inReport = 0;
            }
        }
        // window not responding
        if (fw.hwnd && IsHungAppWindow(fw.hwnd)) {
            if (!hungSince) hungSince = now;
            if (!windowReported && now - hungSince > hangMs && !g_crashed) {
                windowReported = true;
                if (InterlockedCompareExchange(&g_inReport, 1, 0) == 0) {
                    ReportHang("game window is 'Not Responding'");
                    g_inReport = 0;
                }
            }
        } else {
            if (windowReported) Log("Game window responding again.");
            hungSince = 0; windowReported = false;
        }
        if (!noPresentWarned && fw.hwnd && g_presentCount == 0 && now - g_startTick > 60000) {
            noPresentWarned = true;
            Log("Note: game window exists but the Present hook hasn't fired yet - hang detection via frames is inactive.");
        }
        (void)presentWarned;
        // performance summary: fps, 1% low, worst frame, stutter counts, VRAM
        static ULONGLONG lastPerf = 0; static LONG64 lastPerfIdx = 0;
        if (g_cfg.perfSeconds > 0 && now - lastPerf >= (ULONGLONG)g_cfg.perfSeconds * 1000) {
            LONG64 end = g_frameIdx, beg = lastPerfIdx;
            if (end - beg > 8192) beg = end - 8192;
            int n = (int)(end - beg);
            if (n >= 10 && lastPerf) {
                static float tmp[8192];
                double sum = 0; int s50 = 0, s100 = 0;
                for (int i = 0; i < n; i++) { float v = g_frameMs[(beg + i) & 8191]; tmp[i] = v; sum += v; s50 += v >= 50; s100 += v >= 100; }
                std::sort(tmp, tmp + n);
                float p99 = tmp[(int)(n * 0.99)], mx = tmp[n - 1], med = tmp[n / 2];
                char vram[160] = "";
                IDXGIAdapter3* a3 = g_adapter3;
                if (a3 && (uintptr_t)a3 != 1) {
                    DXGI_QUERY_VIDEO_MEMORY_INFO lo{}, nl{};
                    if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &lo)) &&
                        SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nl)))
                        snprintf(vram, sizeof(vram), " | VRAM %llu/%llu MB (%.0f%%) sysmem-for-GPU %llu MB",
                                 (unsigned long long)(lo.CurrentUsage >> 20), (unsigned long long)(lo.Budget >> 20),
                                 lo.Budget ? 100.0 * lo.CurrentUsage / lo.Budget : 0.0, (unsigned long long)(nl.CurrentUsage >> 20));
                }
                MEMORYSTATUSEX ms2{}; ms2.dwLength = sizeof(ms2); GlobalMemoryStatusEx(&ms2);
                char cpu[400]; CpuSample(cpu, sizeof(cpu));
                Log("perf: %.1f fps avg | median %.1f ms | 1%% low %.1f fps | worst %.0f ms | stutters >50ms: %d, >100ms: %d | shaders %ld%s | RAM free %llu MB%s",
                    n * 1000.0 / sum, med, p99 > 0 ? 1000.0 / p99 : 0.0, mx, s50, s100, g_shaderCount, vram,
                    (unsigned long long)(ms2.ullAvailPhys >> 20), cpu);
            }
            lastPerf = now; lastPerfIdx = end;
        }
        if (g_cfg.heartbeatSeconds > 0 && now - lastBeat >= (ULONGLONG)g_cfg.heartbeatSeconds * 1000) {
            LONG64 f = g_presentCount;
            double fps = (double)(f - lastBeatFrames) * 1000.0 / (double)(now - lastBeat);
            MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms); GlobalMemoryStatusEx(&ms);
            PROCESS_MEMORY_COUNTERS pmc{}; GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
            Log("alive: frames=%lld (~%.1f fps) shaders=%ld workingSet=%llu MB sysFree=%llu MB", (long long)f, fps, g_shaderCount,
                (unsigned long long)(pmc.WorkingSetSize >> 20), (unsigned long long)(ms.ullAvailPhys >> 20));
            lastBeat = now; lastBeatFrames = f;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Config / init
// ---------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// Reads [Logger] settings from GBFRCrashLogger.ini next to the DLL, creating a commented
// default ini on first run.
// ----------------------------------------------------------------------------
static void LoadConfig() {
    wchar_t ini[MAX_PATH];
    swprintf(ini, MAX_PATH, L"%ls\\GBFRCrashLogger.ini", g_dir);
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) {
        const char* def =
            "; GBFR Crash Logger settings (0 = off, 1 = on)\r\n"
            "[Logger]\r\n"
            "; Turn on the D3D11 debug layer (needs Windows Settings > Optional features > 'Graphics Tools'). Slower, but explains GPU errors.\r\n"
            "DebugLayer=0\r\n"
            "; Also log debug-layer warnings (noisy)\r\n"
            "LogWarnings=0\r\n"
            "; Save every shader the game creates to CrashLogger\\shaders (failed ones are always saved)\r\n"
            "DumpShaders=0\r\n"
            "; Log every shader creation\r\n"
            "LogShaderCreates=0\r\n"
            "; Seconds without a frame (or window Not Responding) before a hang report is written\r\n"
            "HangSeconds=30\r\n"
            "; Full-memory minidumps (several GB) instead of small ones\r\n"
            "FullDump=0\r\n"
            "; Write a full report on the first access violation even if the game might handle it\r\n"
            "FirstChanceAVReport=0\r\n"
            "MaxFirstChanceLines=100\r\n"
            "HeartbeatSeconds=60\r\n"
            "; Performance summary every N seconds (fps, 1% low, stutters, VRAM). 0 = off\r\n"
            "PerfSeconds=10\r\n"
            "; Log each frame that takes at least this many ms\r\n"
            "StutterMs=100\r\n"
            "; Create a hidden test device at startup so hooks are in place before the game's renderer starts\r\n"
            "ProbeD3D=1\r\n"
            "; Seconds after launch without a D3D11 device before a startup-hang report (0 = off)\r\n"
            "StartupHangSeconds=25\r\n"
            "\r\n"
            "; Environment variables set for the game process only, before DXVK/Vulkan start.\r\n"
            "[Environment]\r\n"
            "; Old Intel iGPU Vulkan driver re-enters DXVK's dxgi.dll during startup and deadlocks it.\r\n"
            "VK_LOADER_DRIVERS_DISABLE=*igvk*\r\n"
            "\r\n"
            "; DLLs the game may not load (appear missing, like under Proton). name=0 to disable an entry.\r\n"
            "[BlockDlls]\r\n"
            "nvapi64.dll=1\r\n";
        HANDLE f = CreateFileW(ini, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, def, (DWORD)strlen(def), &w, nullptr); CloseHandle(f); }
    }
    auto I = [&](const wchar_t* k, int d) { return (int)GetPrivateProfileIntW(L"Logger", k, d, ini); };
    g_cfg.debugLayer = I(L"DebugLayer", 0);
    g_cfg.logWarnings = I(L"LogWarnings", 0);
    g_cfg.dumpShaders = I(L"DumpShaders", 0);
    g_cfg.logShaderCreates = I(L"LogShaderCreates", 0);
    g_cfg.hangSeconds = I(L"HangSeconds", 30); if (g_cfg.hangSeconds < 3) g_cfg.hangSeconds = 3;
    g_cfg.fullDump = I(L"FullDump", 0);
    g_cfg.firstChanceAVReport = I(L"FirstChanceAVReport", 0);
    g_cfg.maxFirstChance = I(L"MaxFirstChanceLines", 100);
    g_cfg.heartbeatSeconds = I(L"HeartbeatSeconds", 60);
    g_cfg.perfSeconds = I(L"PerfSeconds", 10);
    g_cfg.stutterMs = I(L"StutterMs", 100);
    g_cfg.probeD3D = I(L"ProbeD3D", 1);
    g_cfg.startupHangSeconds = I(L"StartupHangSeconds", 25);
}

// [Environment] section: KEY=VALUE pairs set for this process only (before DXVK/Vulkan start)
static wchar_t g_envApplied[4096];
// ----------------------------------------------------------------------------
// Reads the [Environment] ini section and sets each KEY=VALUE as an environment variable
// for this process only, before DXVK / Vulkan start. Used to set
// VK_LOADER_DRIVERS_DISABLE=*igvk* so the Vulkan loader skips the old Intel HD 530
// driver, whose startup re-entered DXVK's dxgi.dll and deadlocked the game.
// ----------------------------------------------------------------------------
static void ApplyEnvironment() {
    wchar_t ini[MAX_PATH]; swprintf(ini, MAX_PATH, L"%ls\\GBFRCrashLogger.ini", g_dir);
    static wchar_t sec[16384];
    DWORD n = GetPrivateProfileSectionW(L"Environment", sec, 16384, ini);
    g_envApplied[0] = 0;
    for (wchar_t* p = sec; n && *p; p += wcslen(p) + 1) {
        if (*p == L';' || *p == L'#') continue;
        wchar_t* eq = wcschr(p, L'=');
        if (!eq) continue;
        *eq = 0;
        SetEnvironmentVariableW(p, eq + 1);
        size_t used = wcslen(g_envApplied);
        swprintf(g_envApplied + used, 4096 - used, L"%ls%ls=%ls", used ? L", " : L"", p, eq + 1);
        *eq = L'=';
    }
}

// [BlockDlls] section: DLL names (one per line, value ignored or =1) the game is not allowed to load.
// Used to mimic Proton, where e.g. nvapi64.dll is absent unless explicitly enabled.
static wchar_t g_block[16][64];
static int g_blockN;
// ----------------------------------------------------------------------------
// Reads the [BlockDlls] ini section (e.g. nvapi64.dll=1) into g_block. name=0 disables.
// ----------------------------------------------------------------------------
static void LoadBlockList() {
    wchar_t ini[MAX_PATH]; swprintf(ini, MAX_PATH, L"%ls\\GBFRCrashLogger.ini", g_dir);
    static wchar_t sec[4096];
    DWORD n = GetPrivateProfileSectionW(L"BlockDlls", sec, 4096, ini);
    for (wchar_t* p = sec; n && *p && g_blockN < 16; p += wcslen(p) + 1) {
        if (*p == L';' || *p == L'#') continue;
        wchar_t name[64]; wcsncpy(name, p, 63); name[63] = 0;
        if (wchar_t* eq = wcschr(name, L'=')) {
            if (eq[1] == L'0') continue;   // name=0 disables the entry
            *eq = 0;
        }
        while (*name && name[wcslen(name) - 1] == L' ') name[wcslen(name) - 1] = 0;
        if (*name) wcscpy(g_block[g_blockN++], name);
    }
}
// ----------------------------------------------------------------------------
// True if a DLL path/name (wide) matches an entry of the block list (with or without .dll).
// ----------------------------------------------------------------------------
static bool IsBlockedW(LPCWSTR path) {
    if (!path || !g_blockN) return false;
    const wchar_t* b = path;
    for (const wchar_t* q = path; *q; q++) if (*q == L'\\' || *q == L'/') b = q + 1;
    for (int i = 0; i < g_blockN; i++) {
        size_t l = wcslen(g_block[i]);
        if (!_wcsnicmp(b, g_block[i], l) && (b[l] == 0 || !_wcsicmp(b + l, L".dll"))) return true;
    }
    return false;
}
// ----------------------------------------------------------------------------
// ANSI version of IsBlockedW.
// ----------------------------------------------------------------------------
static bool IsBlockedA(LPCSTR path) {
    if (!path) return false;
    wchar_t w[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, path, -1, w, MAX_PATH); w[MAX_PATH - 1] = 0;
    return IsBlockedW(w);
}
static volatile LONG g_blockLogged;
// ----------------------------------------------------------------------------
// Logs a blocked DLL load (max 20 lines).
// ----------------------------------------------------------------------------
static void LogBlocked(const char* fn, const char* name) {
    if (InterlockedIncrement(&g_blockLogged) <= 20) {
        void* caller = __builtin_return_address(0);
        BuildModuleTable(); char a[160]; FmtAddr((uintptr_t)caller, a, sizeof(a));
        Log("Blocked %s(\"%s\") per [BlockDlls] - the game sees it as missing (like Proton)", fn, name);
    }
}
// ----------------------------------------------------------------------------
// LoadLibraryW hook for the exe: blocked DLLs fail with ERROR_MOD_NOT_FOUND exactly as if
// the file did not exist; everything else goes to the real LoadLibraryW.
// ----------------------------------------------------------------------------
static HMODULE WINAPI HkLoadLibraryW(LPCWSTR n) {
    if (IsBlockedW(n)) { char u[MAX_PATH * 3]; LogBlocked("LoadLibraryW", W2U(n, u, sizeof(u))); SetLastError(ERROR_MOD_NOT_FOUND); return nullptr; }
    return LoadLibraryW(n);
}
// ----------------------------------------------------------------------------
// LoadLibraryExW hook - this is the call the game's built-in NVAPI library uses to load
// nvapi64.dll. Blocking it makes the game fall back to its plain D3D11 path, the same
// as under Linux/Proton, which is what made characters and terrain render under DXVK.
// ----------------------------------------------------------------------------
static HMODULE WINAPI HkLoadLibraryExW(LPCWSTR n, HANDLE f, DWORD fl) {
    if (IsBlockedW(n)) { char u[MAX_PATH * 3]; LogBlocked("LoadLibraryExW", W2U(n, u, sizeof(u))); SetLastError(ERROR_MOD_NOT_FOUND); return nullptr; }
    return LoadLibraryExW(n, f, fl);
}
// ----------------------------------------------------------------------------
// LoadLibraryA hook (see HkLoadLibraryW).
// ----------------------------------------------------------------------------
static HMODULE WINAPI HkLoadLibraryA(LPCSTR n) {
    if (IsBlockedA(n)) { LogBlocked("LoadLibraryA", n); SetLastError(ERROR_MOD_NOT_FOUND); return nullptr; }
    return LoadLibraryA(n);
}
// ----------------------------------------------------------------------------
// LoadLibraryExA hook (see HkLoadLibraryW).
// ----------------------------------------------------------------------------
static HMODULE WINAPI HkLoadLibraryExA(LPCSTR n, HANDLE f, DWORD fl) {
    if (IsBlockedA(n)) { LogBlocked("LoadLibraryExA", n); SetLastError(ERROR_MOD_NOT_FOUND); return nullptr; }
    return LoadLibraryExA(n, f, fl);
}
// ----------------------------------------------------------------------------
// GetModuleHandleW hook: a blocked DLL is also reported as not loaded.
// ----------------------------------------------------------------------------
static HMODULE WINAPI HkGetModuleHandleW(LPCWSTR n) {
    if (IsBlockedW(n)) { SetLastError(ERROR_MOD_NOT_FOUND); return nullptr; }
    return GetModuleHandleW(n);
}
// ----------------------------------------------------------------------------
// GetModuleHandleA hook: a blocked DLL is also reported as not loaded.
// ----------------------------------------------------------------------------
static HMODULE WINAPI HkGetModuleHandleA(LPCSTR n) {
    if (IsBlockedA(n)) { SetLastError(ERROR_MOD_NOT_FOUND); return nullptr; }
    return GetModuleHandleA(n);
}
// ----------------------------------------------------------------------------
// Installs the LoadLibrary*/GetModuleHandle* IAT hooks in the exe if [BlockDlls] lists
// anything, and logs how many imports were hooked.
// ----------------------------------------------------------------------------
static void InstallBlockHooks() {
    if (!g_blockN) return;
    HMODULE exe = GetModuleHandleW(nullptr);
    struct { const char* fn; void* hk; } h[] = {
        { "LoadLibraryW", (void*)HkLoadLibraryW }, { "LoadLibraryExW", (void*)HkLoadLibraryExW },
        { "LoadLibraryA", (void*)HkLoadLibraryA }, { "LoadLibraryExA", (void*)HkLoadLibraryExA },
        { "GetModuleHandleW", (void*)HkGetModuleHandleW }, { "GetModuleHandleA", (void*)HkGetModuleHandleA } };
    int total = 0;
    for (auto& e : h) {
        total += PatchIat(exe, "kernel32", e.fn, e.hk, nullptr);
        total += PatchIat(exe, "api-ms-win-core-libraryloader", e.fn, e.hk, nullptr);
    }
    char list[1024] = ""; int k = 0;
    for (int i = 0; i < g_blockN; i++) { char u[200]; k += snprintf(list + k, sizeof(list) - k, "%s%s", i ? ", " : "", W2U(g_block[i], u, sizeof(u))); }
    Log("BlockDlls: %s (%d loader imports hooked in the exe)", list, total);
}

// ----------------------------------------------------------------------------
// Loads the REAL version.dll / dinput8.dll from System32 and fills g_fwd with its
// export addresses, so the game's normal calls into this DLL keep working. Must happen
// before the game calls any of those exports.
// ----------------------------------------------------------------------------
static void InitForwarding() {
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    wcscat(sys, L"\\"); wcscat(sys, PROXY_DLL_NAME);
    g_realDll = LoadLibraryW(sys);
    for (int i = 0; i < g_fwdCount; i++) {
        void* p = g_realDll ? (void*)GetProcAddress(g_realDll, g_fwdNames[i]) : nullptr;
        g_fwd[i] = p ? p : (void*)MissingExport;
    }
}

// ----------------------------------------------------------------------------
// Creates the CrashLogger folder next to the DLL and opens log_<date_time>.txt.
// latest_log_name.txt always holds the path of the newest log.
// ----------------------------------------------------------------------------
static void OpenLog() {
    swprintf(g_outDir, MAX_PATH, L"%ls\\CrashLogger", g_dir);
    CreateDirectoryW(g_outDir, nullptr);
    SYSTEMTIME t; GetLocalTime(&t);
    swprintf(g_stamp, 64, L"%04u-%02u-%02u_%02u-%02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\log_%ls.txt", g_outDir, g_stamp);
    g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    // Also a fixed-name copy target: latest.txt points to newest via a small text note
    wchar_t latest[MAX_PATH]; swprintf(latest, MAX_PATH, L"%ls\\latest_log_name.txt", g_outDir);
    HANDLE f = CreateFileW(latest, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) { char u[MAX_PATH * 3]; W2U(path, u, sizeof(u)); DWORD w; WriteFile(f, u, (DWORD)strlen(u), &w, nullptr); CloseHandle(f); }
}

// ----------------------------------------------------------------------------
// Entry point. On process attach (runs while the game is still starting, under the
// Windows loader lock, so only light work here):
//   1. set up forwarding to the real system DLL
//   2. read the ini, apply [Environment] and [BlockDlls]
//   3. open the log and write system info
//   4. install the exception handlers and import-table hooks
//   5. start the Worker (setup) and Watchdog (monitoring) threads
// On process detach, logs a normal exit. If a log ends WITHOUT that line, the process
// was killed (TerminateProcess / fast-fail) rather than exiting.
// ----------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_logCs);
        InitializeCriticalSection(&g_hookCs);
        InitializeCriticalSection(&g_symCs);
        InitializeCriticalSection(&g_dumpCs);
        g_startTick = GetTickCount64();
        InitForwarding();
        GetModuleFileNameW(h, g_dir, MAX_PATH);
        if (wchar_t* s = wcsrchr(g_dir, L'\\')) *s = 0;
        LoadConfig();
        ApplyEnvironment();
        LoadBlockList();
        OpenLog();
        {
            wchar_t exeDir[MAX_PATH], p[MAX_PATH]; GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
            if (wchar_t* s2 = wcsrchr(exeDir, L'\\')) *s2 = 0;
            const wchar_t* names[] = { L"d3d11.dll", L"dxgi.dll" };
            for (auto n : names) {
                swprintf(p, MAX_PATH, L"%ls\\%ls", exeDir, n);
                if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES && g_cfg.probeD3D) {
                    g_cfg.probeD3D = 0;
                    Log("Found %ls next to the exe (DXVK/ReShade/wrapper) - ProbeD3D forced off to avoid initialising it concurrently", n);
                }
            }
        }
        DetectCpu();
        Log("GBFR Crash Logger v" LOGGER_VERSION " loaded as %ls (forwarding to system copy: %s)", PROXY_DLL_NAME, g_realDll ? "ok" : "FAILED");
        LogSystemInfo();
        if (g_envApplied[0]) { char u[8192]; Log("Env set  : %s", W2U(g_envApplied, u, sizeof(u))); }
        Log("Config   : DebugLayer=%d LogWarnings=%d DumpShaders=%d HangSeconds=%d FullDump=%d FirstChanceAVReport=%d ProbeD3D=%d",
            g_cfg.debugLayer, g_cfg.logWarnings, g_cfg.dumpShaders, g_cfg.hangSeconds, g_cfg.fullDump, g_cfg.firstChanceAVReport, g_cfg.probeD3D);

        InstallBlockHooks();
        AddVectoredExceptionHandler(1, VehHandler);
        SetUnhandledExceptionFilter(OurUnhandledFilter);
        PatchUefInModules(true);

        HMODULE exe = GetModuleHandleW(nullptr);
        int a = PatchIat(exe, "d3d11.dll", "D3D11CreateDevice", (void*)HkD3D11CreateDevice, (void**)&oD3D11CreateDevice);
        int b = PatchIat(exe, "d3d11.dll", "D3D11CreateDeviceAndSwapChain", (void*)HkD3D11CreateDeviceAndSwapChain, (void**)&oD3D11CreateDeviceAndSwapChain);
        int c = PatchIat(exe, "dxgi.dll", "CreateDXGIFactory2", (void*)HkCreateDXGIFactory2, (void**)&oCreateDXGIFactory2);
        QueryPerformanceFrequency(&g_qpcFreq);
        Log("Hooks    : exception handlers installed; exe imports D3D11CreateDevice=%s D3D11CreateDeviceAndSwapChain=%s CreateDXGIFactory2=%s",
            a ? "hooked" : "not imported", b ? "hooked" : "not imported", c ? "hooked" : "not imported");
        if (g_cfg.debugLayer && !a && !b)
            Log("Warning  : DebugLayer=1 but the exe doesn't import D3D11 creation directly - debug layer can't be forced this way.");

        HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
        t = CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        Log("Process exiting %s. frames=%lld shaders=%ld failedShaders=%ld uptime=%llu s",
            reserved ? "(normal ExitProcess - if the game vanished, it exited by itself after an error)" : "(DLL unloaded)",
            (long long)g_presentCount, g_shaderCount, g_shaderFails, (unsigned long long)((GetTickCount64() - g_startTick) / 1000));
        if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    }
    return TRUE;
}
