#include "izuna/izuna_shaders.h"
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
int main() {
  const size_t page = size_t(sysconf(_SC_PAGESIZE));
  auto* p = static_cast<uint8_t*>(mmap(nullptr, page * 3, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (p == MAP_FAILED) return 1;
  std::memset(p, 0x31, page);
  std::memset(p + page * 2, 0x73, page);
  if (mprotect(p + page, page, PROT_NONE) != 0) return 2;
  const auto data = izuna::shaders::CopyReadable(p + 17, page * 3 - 34);
  const auto all = [&](size_t begin, size_t end, uint8_t value) {
    return std::all_of(data.begin() + begin, data.begin() + end, [=](uint8_t v) { return v == value; });
  };
  const bool ok = all(0, page - 17, 0x31) && all(page - 17, page * 2 - 17, 0) &&
                  all(page * 2 - 17, data.size(), 0x73) &&
                  izuna::shaders::CopyReadable(nullptr, 16) == std::vector<uint8_t>(16);
  munmap(p, page * 3);
  std::puts(ok ? "PASS: unreadable page zeroed; both readable pages preserved" : "FAIL");
  return ok ? 0 : 3;
}
