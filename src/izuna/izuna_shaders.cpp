// [IzunaRecomp] The game's shaders from the player's own copy (izuna_shaders.h).
//
// The scan, the translation and the compiler arguments are those of the
// fork's command-line tool (XenosRecomp/main.cpp and dxc_compiler.cpp,
// NG2_RECOMP: the base shader only, SPIR-V only), so a table built here is the
// one tools/native/build_shaders.sh would write for the same image.

#include "pch.h"  // (third_party/xenosrecomp: Windows.h / dxcapi.h, fmt, smolv, xxhash; first, as in the fork)

#include "izuna/izuna_shaders.h"

#include "shader.h"
#include "shader_recompiler.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#endif

#include <rex/logging.h>


extern const char kIzunaShaderCommon[];       // third_party/xenosrecomp/shader_common.h (CMake)
extern const unsigned long kIzunaShaderCommonSize;

namespace izuna::shaders {
namespace {

// Bump when the translation changes in a way shader_common.h does not show
// (shader_recompiler.cpp): every saved table is then built again.
constexpr const char* kBuilderVersion = "izuna-shaders 4 (reeot-XenosRecomp ng2 + scene fit, coverage, per-slot scale, exact interpolant components where compared)";
constexpr uint32_t kMagic = 0x48535A49;  // 'IZSH'
constexpr uint32_t kFormat = 1;

Table* g_active = nullptr;

DxcCreateInstanceProc LoadDxc(const std::filesystem::path& dll, std::string* error) {
#if defined(_WIN32)
  HMODULE m = LoadLibraryW(dll.wstring().c_str());
  if (!m) {
    *error = fmt::format("{} did not load (error {})", dll.string(), GetLastError());
    return nullptr;
  }
  auto proc = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(m, "DxcCreateInstance"));
#else
  void* m = dlopen(dll.string().c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!m) {
    *error = fmt::format("{} did not load ({})", dll.string(), dlerror());
    return nullptr;
  }
  auto proc = reinterpret_cast<DxcCreateInstanceProc>(dlsym(m, "DxcCreateInstance"));
#endif
  if (!proc) *error = "DxcCreateInstance not found in " + dll.string();
  return proc;
}

// The fork's DxcCompiler::compile for SPIR-V (compileLibrary false).
std::vector<uint8_t> CompileSpirv(IDxcCompiler3* dxc, const std::string& source, bool pixel, std::string* errors) {
  DxcBuffer buffer{};
  buffer.Ptr = source.c_str();
  buffer.Size = source.size();
  const wchar_t* args[16]{};
  uint32_t n = 0;
  args[n++] = pixel ? L"-T ps_6_0" : L"-T vs_6_0";
  args[n++] = L"-E shaderMain";
  args[n++] = L"-HV 2021";
  args[n++] = L"-all-resources-bound";
  args[n++] = L"-spirv";
  args[n++] = L"-fvk-use-dx-layout";
  if (!pixel) args[n++] = L"-fvk-invert-y";
  args[n++] = L"-Qstrip_debug";
  args[n++] = L"-DREEOT_RECOMP";
  IDxcResult* result = nullptr;
  std::vector<uint8_t> out;
  if (FAILED(dxc->Compile(&buffer, args, n, nullptr, IID_PPV_ARGS(&result))) || !result) return out;
  HRESULT status = E_FAIL;
  result->GetStatus(&status);
  if (FAILED(status)) {
    IDxcBlobUtf8* e = nullptr;
    if (result->HasOutput(DXC_OUT_ERRORS) && SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&e), nullptr)) &&
        e) {
      *errors = std::string(e->GetStringPointer(), e->GetStringLength()).substr(0, 400);
      e->Release();
    }
  } else {
    IDxcBlob* object = nullptr;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) && object) {
      const uint8_t* p = static_cast<const uint8_t*>(object->GetBufferPointer());
      out.assign(p, p + object->GetBufferSize());
      object->Release();
    }
  }
  result->Release();
  return out;
}

// Runs `count` jobs on every core; DXC recurses deeply, so 16 MB stacks.
void RunParallel(size_t count, const std::function<void(size_t)>& job) {
  struct Shared {
    std::atomic<size_t> next{0};
    size_t count;
    const std::function<void(size_t)>* job;
  } shared;
  shared.count = count;
  shared.job = &job;
  const unsigned threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
#if defined(_WIN32)
  auto run = [](void* p) -> unsigned {
    auto& s = *static_cast<Shared*>(p);
    for (size_t i; (i = s.next.fetch_add(1)) < s.count;) (*s.job)(i);
    return 0;
  };
  std::vector<HANDLE> handles;
  for (unsigned t = 0; t < threads; ++t) {
    const uintptr_t h = _beginthreadex(nullptr, 16u << 20, run, &shared, 0, nullptr);
    if (h) handles.push_back(reinterpret_cast<HANDLE>(h));
  }
  if (handles.empty()) run(&shared);
  for (HANDLE h : handles) {
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
  }
#else
  auto run = [](void* p) -> void* {
    auto& s = *static_cast<Shared*>(p);
    for (size_t i; (i = s.next.fetch_add(1)) < s.count;) (*s.job)(i);
    return nullptr;
  };
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 16u << 20);
  std::vector<pthread_t> ids(threads);
  for (auto& id : ids) pthread_create(&id, &attr, run, &shared);
  for (auto& id : ids) pthread_join(id, nullptr);
  pthread_attr_destroy(&attr);
#endif
}

uint64_t TranslatorHash() {
  XXH3_state_t* st = XXH3_createState();
  XXH3_64bits_reset(st);
  XXH3_64bits_update(st, kIzunaShaderCommon, kIzunaShaderCommonSize);
  XXH3_64bits_update(st, kBuilderVersion, std::strlen(kBuilderVersion));
  const uint64_t h = XXH3_64bits_digest(st);
  XXH3_freeState(st);
  return h;
}

struct FileHeader {
  uint32_t magic, format;
  uint64_t image_hash, translator_hash;
  uint64_t entries, layouts, spirv;
};

bool Load(const std::filesystem::path& path, uint64_t image_hash, uint64_t translator_hash, Table& out) {
  std::ifstream f(path, std::ios::binary);
  FileHeader h{};
  if (!f || !f.read(reinterpret_cast<char*>(&h), sizeof(h))) return false;
  if (h.magic != kMagic || h.format != kFormat || h.image_hash != image_hash || h.translator_hash != translator_hash ||
      h.entries > 100000 || h.layouts > (64u << 20) || h.spirv > (1u << 30)) {
    return false;
  }
  out.entries.resize(h.entries);
  out.layouts.resize(h.layouts);
  out.spirv.resize(h.spirv);
  return f.read(reinterpret_cast<char*>(out.entries.data()), std::streamsize(h.entries * sizeof(ShaderCacheEntry))) &&
         f.read(reinterpret_cast<char*>(out.layouts.data()), std::streamsize(h.layouts * sizeof(uint32_t))) &&
         f.read(reinterpret_cast<char*>(out.spirv.data()), std::streamsize(h.spirv));
}

void Save(const std::filesystem::path& path, uint64_t translator_hash, const Table& t) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  const auto tmp = path.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    const FileHeader h{kMagic, kFormat, t.image_hash, translator_hash, t.entries.size(), t.layouts.size(), t.spirv.size()};
    f.write(reinterpret_cast<const char*>(&h), sizeof(h));
    f.write(reinterpret_cast<const char*>(t.entries.data()), std::streamsize(t.entries.size() * sizeof(ShaderCacheEntry)));
    f.write(reinterpret_cast<const char*>(t.layouts.data()), std::streamsize(t.layouts.size() * sizeof(uint32_t)));
    f.write(reinterpret_cast<const char*>(t.spirv.data()), std::streamsize(t.spirv.size()));
    f.close();
    if (f.fail()) {
      std::filesystem::remove(tmp, ec);
      return;
    }
  }
  std::filesystem::rename(tmp, path, ec);
}

}  // namespace


void SetActive(Table* table) { g_active = table; }
const Table* Active() { return g_active; }

Report Prepare(const uint8_t* image, size_t size, const std::filesystem::path& cache_dir,
               const std::filesystem::path& dxc_dll, Table& out,
               const std::function<void(size_t, size_t)>& progress) {
  const auto t0 = std::chrono::steady_clock::now();
  Report r;
  if (!image || size < sizeof(ShaderContainer) + 8) {
    r.error = "no game image in memory";
    return r;
  }
  out = Table{};
  out.image_hash = XXH3_64bits(image, size);
  const uint64_t translator = TranslatorHash();
  const auto path = cache_dir / fmt::format("ng2_{:016x}_{:016x}.izsh", out.image_hash, translator);
  if (Load(path, out.image_hash, translator, out)) {
    r.ok = r.from_cache = true;
    r.found = r.built = out.entries.size();
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
  }

  // The containers (XenosRecomp/main.cpp's scan): a header whose flags are
  // 0x102A11xx, its sizes inside the image, two zero fields.
  std::map<XXH64_hash_t, const uint8_t*> found;
  for (size_t i = 0; i + sizeof(ShaderContainer) + 1 < size;) {
    auto c = reinterpret_cast<const ShaderContainer*>(image + i);
    const size_t data = size_t(uint32_t(c->virtualSize)) + uint32_t(c->physicalSize);
    if ((uint32_t(c->flags) & 0xFFFFFF00u) == 0x102A1100u && data <= size - i && uint32_t(c->field1C) == 0 &&
        uint32_t(c->field20) == 0) {
      found.try_emplace(XXH3_64bits(c, data), image + i);
      i += data;
    } else {
      i += sizeof(uint32_t);
    }
  }
  r.found = found.size();
  if (found.empty()) {
    r.error = "no shader containers in the game image";
    return r;
  }
  std::string dll_error;
  const DxcCreateInstanceProc create = LoadDxc(dxc_dll, &dll_error);
  if (!create) {
    r.error = dll_error;
    return r;
  }

  struct Built {
    XXH64_hash_t hash;
    const uint8_t* data;
    bool ok = false;
    std::string error;
    std::vector<uint8_t> smolv;
    ShaderCacheEntry entry{};
    std::vector<VertexFetchLayoutRecord> layout;
  };
  std::vector<Built> shaders;
  shaders.reserve(found.size());
  for (const auto& [hash, data] : found) shaders.push_back({hash, data});
  const std::string_view include(kIzunaShaderCommon, kIzunaShaderCommonSize);
  std::atomic<size_t> done{0};
  RunParallel(shaders.size(), [&](size_t i) {
    Built& b = shaders[i];
    thread_local ShaderRecompiler recompiler;
    thread_local IDxcCompiler3* dxc = nullptr;
    if (!dxc && FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc)))) dxc = nullptr;
    recompiler = {};
    try {
      recompiler.recompile(b.data, include);
    } catch (...) {
      b.error = "translation failed";
    }
    if (b.error.empty() && recompiler.unsupportedExports) b.error = "export register with no destination";
    if (b.error.empty() && !dxc) b.error = "no DXC instance";
    std::vector<uint8_t> spirv;
    if (b.error.empty()) {
      spirv = CompileSpirv(dxc, recompiler.out, recompiler.isPixelShader, &b.error);
      if (spirv.empty() && b.error.empty()) b.error = "DXC returned nothing";
    }
    if (b.error.empty() &&
        !smolv::Encode(spirv.data(), spirv.size(), b.smolv, smolv::kEncodeFlagStripDebugInfo)) {
      b.error = "smol-v encode failed";
    }
    if (b.error.empty()) {
      ShaderCacheEntry& e = b.entry;
      e.hash = b.hash;
      e.specConstantsMask = recompiler.specConstantsMask;
      e.vfetchCodeOffset = recompiler.physicalCodeOffset;
      e.usesFloatConstants = recompiler.usesFloatConstants ? 1 : 0;
      e.interpolantMask = recompiler.interpolantMask;
      e.floatConstantRegs = recompiler.ng2FloatConstantRegs;
      e.textureSlotMask = recompiler.ng2TextureSlotMask;
      e.pointSlotMask = recompiler.ng2PointSlotMask;
      e.linearSlotMask = recompiler.ng2LinearSlotMask;
      e.mipPointSlotMask = recompiler.ng2MipPointSlotMask;
      e.mipLinearSlotMask = recompiler.ng2MipLinearSlotMask;
      e.mipBaseSlotMask = recompiler.ng2MipBaseSlotMask;
      e.anisoOffSlotMask = recompiler.ng2AnisoOffSlotMask;
      e.memExport = recompiler.usesMemExport ? 1u : 0u;
      b.layout = recompiler.vertexLayout;
      b.ok = true;
    }
    const size_t d = ++done;
    if (progress) progress(d, shaders.size());
  });

  // The table, in hash order (std::map's): each shader's SPIR-V after the last,
  // its layout records after the last (build_shaders.sh's layout).
  for (Built& b : shaders) {
    if (!b.ok) {
      ++r.failed;
      static int logged = 0;
      if (++logged <= 8) REXLOG_WARN("[izuna] shader {:016X} not built: {}", b.hash, b.error);
      continue;
    }
    ShaderCacheEntry e = b.entry;
    e.spirvOffset = uint32_t(out.spirv.size());
    e.spirvSize = uint32_t(b.smolv.size());
    e.dxilOffset = 0;
    e.dxilSize = 0;
    e.fullSpirvOffset = e.posSpirvOffset = e.velSpirvOffset = uint32_t(out.spirv.size() + b.smolv.size());
    e.vertexLayoutOffset = uint32_t(out.layouts.size());
    e.vertexLayoutCount = uint32_t(b.layout.size());
    for (const VertexFetchLayoutRecord& rec : b.layout) {
      out.layouts.push_back(rec.w0);
      out.layouts.push_back(rec.w1);
    }
    out.spirv.insert(out.spirv.end(), b.smolv.begin(), b.smolv.end());
    out.entries.push_back(e);
    ++r.built;
  }
  // All or nothing: a table with holes would leave those draws out, where the
  // built-in table has every shader.
  r.ok = r.built > 0 && r.failed == 0;
  if (r.ok) Save(path, translator, out);
  if (!r.ok && r.error.empty())
    r.error = r.built == 0 ? std::string("no shader built") : fmt::format("{} of {} shaders failed", r.failed, r.found);
  r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return r;
}

}  // namespace izuna::shaders
