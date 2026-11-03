// Read guest image pages without faulting on inaccessible mapping holes.
#include "izuna/izuna_shaders.h"
#include <algorithm>
#include <cstring>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <fstream>
#include <cstdio>
#include <unistd.h>
#include <cerrno>
#endif
namespace izuna::shaders {
std::vector<uint8_t> CopyReadable(const uint8_t* src, size_t size) {
  std::vector<uint8_t> out(size);
  if (!src) return out;
#if defined(_WIN32)
  for (size_t at = 0; at < size;) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(src + at, &mbi, sizeof(mbi))) break;
    const size_t region = std::min<size_t>(
        size - at, size_t(static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize - (src + at)));
    const DWORD p = mbi.Protect & 0xFF;
    if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && p != PAGE_NOACCESS && p != 0)
      std::memcpy(out.data() + at, src + at, region);
    at += region;
  }
#elif defined(__linux__)
  // Consult permissions first: /proc/self/mem can read PROT_NONE pages,
  // which must stay zero here. pread fails safely if a mapping disappears.
  const int fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return out;
  std::ifstream maps("/proc/self/maps");
  const uintptr_t begin = reinterpret_cast<uintptr_t>(src);
  if (size > UINTPTR_MAX - begin) { close(fd); return out; }
  const uintptr_t end = begin + size;
  for (std::string line; std::getline(maps, line);) {
    unsigned long lo{}, hi{};
    char permission{};
    if (std::sscanf(line.c_str(), "%lx-%lx %c", &lo, &hi, &permission) != 3 || permission != 'r') continue;
    uintptr_t at = std::max(begin, uintptr_t(lo));
    const uintptr_t stop = std::min(end, uintptr_t(hi));
    while (at < stop) {
      const ssize_t got = pread(fd, out.data() + (at - begin), stop - at, off_t(at));
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) break;
      at += size_t(got);
    }
  }
  close(fd);
#else
  std::memcpy(out.data(), src, size);
#endif
  return out;
}

}  // namespace izuna::shaders
