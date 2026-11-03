// [IzunaRecomp] The Windows build's steadiness (izuna_pc.h).

#include "izuna/izuna_pc.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <string>
#include <thread>

#include <rex/logging.h>

// A laptop's switchable graphics: the drivers run a program that exports
// these on the discrete GPU (NVIDIA Optimus, AMD PowerXpress). The Vulkan
// device is chosen by IzunaRecomp itself (the discrete one first); these also
// put the drivers' own profile for the process on it.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace izuna::pc {
namespace {

std::wstring g_dump_dir;
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
std::atomic<bool> g_dumping{false};

LONG WINAPI WriteDump(EXCEPTION_POINTERS* info) {
  if (!g_dumping.exchange(true)) {
    wchar_t name[96];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    wcsftime(name, 96, L"crash-%Y%m%d-%H%M%S.dmp", &tm);
    CreateDirectoryW(g_dump_dir.c_str(), nullptr);  // (made only now: with logging off nothing else makes it)
    const std::wstring path = g_dump_dir + L"\\" + name;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      MINIDUMP_EXCEPTION_INFORMATION exception = {GetCurrentThreadId(), info, FALSE};
      // The stacks, the threads, and the memory the stacks point at: enough to
      // see where and why, without the whole guest memory (gigabytes).
      MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                        MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                      MiniDumpWithUnloadedModules),
                        info ? &exception : nullptr, nullptr, nullptr);
      CloseHandle(file);
      REXLOG_ERROR("Crash: a minidump was written to {} (send it with the log)",
                   std::string(path.begin(), path.end()));
      rex::FlushLogging();
    }
  }
  return g_previous_filter ? g_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void TuneProcess() {
  static std::atomic<bool> done{false};
  if (done.exchange(true)) return;
  // Power throttling (EcoQoS) off: execution speed, and the timer resolution
  // a throttled process has ignored.
  PROCESS_POWER_THROTTLING_STATE power = {};
  power.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  power.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  power.StateMask = 0;
  const bool unthrottled =
      SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &power, sizeof(power)) != 0;
  timeBeginPeriod(1);
  SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
  REXLOG_INFO("Windows: {}1 ms timers, above-normal priority", unthrottled ? "power throttling off, " : "");
  {
    // Which CPU, and which build of IzunaRecomp runs on it (izuna_cpu_redirect.cpp).
    int r[4];
    char brand[49] = {};
    __cpuid(r, int(0x80000000u));
    if (unsigned(r[0]) >= 0x80000004u) {
      for (int i = 0; i < 3; ++i) {
        __cpuid(r, int(0x80000002u) + i);
        std::memcpy(brand + i * 16, r, 16);
      }
    }
    const char* b = brand;
    while (*b == ' ') ++b;
#if defined(IZUNA_CPU_LEVEL) && IZUNA_CPU_LEVEL == 3
    const char* build = "ng2.exe, x86-64-v3 (AVX2, FMA)";
#elif defined(IZUNA_CPU_LEVEL)
    const char* build = "ng2-compat.exe, x86-64-v2 (for processors without AVX2)";
#else
    const char* build = "?";
#endif
    REXLOG_INFO("CPU: {} ({} hardware threads); this build: {}", *b ? b : "unknown",
                std::thread::hardware_concurrency(), build);
  }
}

void EnsureSingleInstance() {
  static HANDLE mutex = nullptr;
  if (mutex) return;
  mutex = CreateMutexW(nullptr, FALSE, L"Local\\IzunaRecomp-NinjaGaidenII");
  if (!mutex) return;  // (no guard rather than no game)
  const DWORD got = WaitForSingleObject(mutex, 5000);
  if (got == WAIT_OBJECT_0 || got == WAIT_ABANDONED) return;  // (held for the process's life)
  REXLOG_ERROR("Another copy of IzunaRecomp is running: quitting");
  rex::FlushLogging();
  MessageBoxW(nullptr, L"IzunaRecomp is already running.", L"IzunaRecomp", MB_OK | MB_ICONINFORMATION);
  ExitProcess(0);
}

void ShowError(const std::string& text) {
  REXLOG_ERROR("{}", text);
  rex::FlushLogging();
  const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  std::wstring wide(size_t(n > 0 ? n : 1), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), n);
  MessageBoxW(nullptr, wide.c_str(), L"IzunaRecomp", MB_OK | MB_ICONERROR);
}

bool ScreenSize(int* width, int* height) {
  DEVMODEW mode = {};
  mode.dmSize = sizeof(mode);
  if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) || !mode.dmPelsWidth || !mode.dmPelsHeight)
    return false;
  *width = int(mode.dmPelsWidth);
  *height = int(mode.dmPelsHeight);
  return true;
}

void RaiseThreadPriority(int level) {
  SetThreadPriority(GetCurrentThread(), level >= 1 ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_ABOVE_NORMAL);
}

void SleepNs(int64_t ns) {
  if (ns <= 0) return;
  // (never closed: one per timing thread, which lives as long as the process)
  thread_local HANDLE timer =
      CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (timer) {
    LARGE_INTEGER due;
    due.QuadPart = -std::max<int64_t>(1, ns / 100);  // (100 ns units, relative)
    if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
      WaitForSingleObject(timer, DWORD(ns / 1'000'000 + 100));
      return;
    }
  }
  std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
}

void InstallCrashDumps(const std::filesystem::path& logs) {
  g_dump_dir = logs.wstring();
  g_previous_filter = SetUnhandledExceptionFilter(WriteDump);
  REXLOG_INFO("Crashes write a minidump to {}", logs.string());
}

}  // namespace izuna::pc

#else

#include <chrono>
#include <thread>

#include <rex/logging.h>
#if defined(IZUNA_LINUX_DESKTOP)
#include <SDL3/SDL.h>
#endif

namespace izuna::pc {
void TuneProcess() {}
void EnsureSingleInstance() {}
void ShowError(const std::string& text) { REXLOG_ERROR("{}", text); }
bool ScreenSize(int* width, int* height) {
#if defined(IZUNA_LINUX_DESKTOP)
  SDL_Window* window = SDL_GetKeyboardFocus();
  const SDL_DisplayID display = window ? SDL_GetDisplayForWindow(window) : SDL_GetPrimaryDisplay();
  const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
  if (!mode) return false;
  if (width) *width = mode->w;
  if (height) *height = mode->h;
  return mode->w > 0 && mode->h > 0;
#else
  return false;
#endif
}
void RaiseThreadPriority(int) {}
void SleepNs(int64_t ns) {
  if (ns > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
}
void InstallCrashDumps(const std::filesystem::path&) {}
}  // namespace izuna::pc

#endif
