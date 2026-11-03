#include "ng2_hwdetect.h"

#if defined(_WIN32)
#include <windows.h>

#include <dxgi1_4.h>
#include <wrl/client.h>
#endif

#include <algorithm>
#include <cstdio>

#include <rex/logging.h>

namespace ng2 {
namespace {

#if defined(_WIN32)
std::string Utf8(const wchar_t* w) {
  if (!w)
    return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1)
    return {};
  std::string out(size_t(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
  return out;
}
#endif

}  // namespace


#if defined(IZUNA_LINUX_DESKTOP)
GpuInfo DetectGpu() { return {}; }  // No DXGI on Linux; renderer logs the selected Vulkan device.
#elif defined(__PROSPERO__)
// [ps5 port] There is one machine to detect. The PS5's 16 GB is shared by the
// CPU and the GPU, so there is no "dedicated" figure to size from: the
// settings are chosen in RecommendSettings from what the console port
// measured instead.
GpuInfo DetectGpu() {
  GpuInfo info;
  info.valid = true;
  info.name = "PlayStation 5 GPU (AMD RDNA 2)";
  return info;
}
#else
GpuInfo DetectGpu() {
  GpuInfo info;
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    return info;

  // The adapter with the most dedicated memory, which on a machine with an
  // integrated GPU beside a discrete one is the one the game will run on.
  Microsoft::WRL::ComPtr<IDXGIAdapter1> best;
  DXGI_ADAPTER_DESC1 best_desc = {};
  for (UINT i = 0;; ++i) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
      break;
    DXGI_ADAPTER_DESC1 desc = {};
    if (FAILED(adapter->GetDesc1(&desc)))
      continue;
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
      continue;
    if (!best || desc.DedicatedVideoMemory > best_desc.DedicatedVideoMemory) {
      best = adapter;
      best_desc = desc;
    }
  }
  if (!best)
    return info;

  info.valid = true;
  info.name = Utf8(best_desc.Description);
  info.dedicated_bytes = uint64_t(best_desc.DedicatedVideoMemory);

  // Budget and current usage are read for the LOG only - they are useful when
  // diagnosing a stutter ("was the card already full?") but nothing is decided
  // from them; see RecommendSettings for why.
  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
  if (SUCCEEDED(best.As(&adapter3))) {
    DXGI_QUERY_VIDEO_MEMORY_INFO vm = {};
    if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm))) {
      info.budget_bytes = vm.Budget;
      info.used_bytes = vm.CurrentUsage;
    }
  }
  if (info.budget_bytes == 0)
    info.budget_bytes = info.dedicated_bytes;
  return info;
}
#endif

}  // namespace ng2
