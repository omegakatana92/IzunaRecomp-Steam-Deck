// [IzunaRecomp] Mods (izuna_mods.h; docs/MODDING.md).
//
// How the game loads a file, and where a mod's file goes in:
//
// - Every game file has an id and a path, the game's own names for them: a
//   table of 5099 path strings in the executable (0x83AEBC98, id -> "char\
//   ryu\ryu.gmd"), its sizes in load_img.bin, and where each sits in the
//   .ng2 archives in pack_img.bin (archive, offset, size).
// - The loader thread loads one by id in sub_823D5EE0. Each request carries
//   a byte that, when set, loads the file loose from D:\databin\<its path>
//   instead of from the archives (sub_823D66A0: open, size, read, close) -
//   the developers' own way to try a file without rebuilding an archive.
// - So a mod's file is loaded by the game itself: the request for its id is
//   set to load loose (izunaModFileRequest), and the open of D:\databin\<path>
//   goes to the mod's file instead (izunaModOpenPath), on a read-only device
//   of the Mods folder - the game's own file calls, so its reads are the
//   kernel's (they handle memory the renderer watches). The game asks a
//   file's size before it makes room for it (sub_823D6128): a mod's file
//   larger than the game's gets the larger size there (izunaModFileSize) and
//   in load_img.bin's table once it is loaded (izunaModFileTable).
// - [replace] loads another game file in place of one, with nothing of the
//   game's in the mod: that file is copied once out of its archive into the
//   cache, then goes in as a mod's file would.
//
// Hooks: config/hooks/mods.toml. Nothing here runs unless a mod is on.

#include "izuna/izuna_mods.h"
#include "izuna/izuna_scripts.h"
#include "native/native_draw.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#if defined(_WIN32)
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif
#include <fstream>
#include <map>
#include <memory>
#include <unordered_map>

#include <toml++/toml.hpp>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#if defined(__PROSPERO__) || defined(IZUNA_LINUX_DESKTOP)
#include "ps5/ps5_paths.h"
#endif

namespace fs = std::filesystem;

namespace izuna::mods {
namespace {

// The game's tables (Ninja Gaiden II, the disc's default.xex).
constexpr uint32_t kFileNames = 0x83AEBC98;  // a path string per file id
constexpr uint32_t kFileCount = 5099;        // load_img.bin's count (0x13EB)
constexpr uint32_t kLoadImg = 0x84F3F1F8;    // load_img.bin as loaded: its buffer
constexpr const char* kLoosePrefix = "D:\\databin\\";

// The archives of pack_img.bin's first six slots (the executable's table at
// 0x83AF0D40; slots 6-19 hold the chapters again, one each).
const std::vector<std::vector<std::string>>& SlotArchives() {
  static const std::vector<std::vector<std::string>> slots = [] {
    std::vector<std::vector<std::string>> s = {{"cmn.ng2"}, {"char.ng2"}, {}, {"rtm.ng2"}, {"other.ng2"},
                                               {"other_rtm.ng2"}};
    for (int c = 1; c <= 14; ++c) {
      char name[24];
      std::snprintf(name, sizeof(name), "s_chap_%02d.ng2", c);
      s[2].push_back(name);
    }
    return s;
  }();
  return slots;
}

// A path as the game writes it, for comparing: backslashes, lower case, no
// leading separator.
std::string Key(std::string_view path) {
  std::string k;
  k.reserve(path.size());
  for (char ch : path) {
    if (ch == '/') ch = '\\';
    if (k.empty() && ch == '\\') continue;
    k.push_back(char(std::tolower(static_cast<unsigned char>(ch))));
  }
  return k;
}

std::string Lower(std::string s) {
  for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

std::string Str(const toml::table& t, const char* key) {
  if (auto v = t[key].value<std::string>()) return *v;
  return {};
}

// A mod's mod.toml: its keys at the top, or under [mod].
void ReadManifest(Mod& mod) {
  const fs::path file = mod.dir / "mod.toml";
  std::error_code ec;
  if (!fs::exists(file, ec)) return;
  try {
    const toml::table root = toml::parse_file(file.string());
    const toml::table* t = root["mod"].as_table() ? root["mod"].as_table() : &root;
    mod.manifest = true;
    mod.name = Str(*t, "name");
    mod.author = Str(*t, "author");
    mod.version = Str(*t, "version");
    mod.description = Str(*t, "description");
    mod.script = Str(*t, "script");
    if (const toml::array* hide = (*t)["hide_shaders"].as_array()) {
      for (const toml::node& n : *hide) {
        const std::string h = Lower(n.value_or(std::string()));
        if (h.size() == 16 && h.find_first_not_of("0123456789abcdef") == std::string::npos) {
          mod.hide_shaders.push_back(h);
        } else {
          mod.notes.push_back("hide_shaders: '" + h + "' is not a shader's hash (16 hex digits)");
        }
      }
    }
    if (const toml::table* r = root["replace"].as_table()) {
      for (const auto& [target, source] : *r) {
        if (auto s = source.value<std::string>()) {
          mod.replace.emplace_back(std::string(target.str()), *s);
        } else {
          mod.notes.push_back("[replace] " + std::string(target.str()) + ": the game file to load must be a string");
        }
      }
    }
  } catch (const toml::parse_error& e) {
    mod.notes.push_back(std::string("mod.toml could not be read: ") + std::string(e.description()));
  }
}

// The files under a mod's files/, as paths in the game.
void ReadFiles(Mod& mod) {
  const fs::path files = mod.dir / "files";
  std::error_code ec;
  if (!fs::is_directory(files, ec)) return;
  for (fs::recursive_directory_iterator it(files, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string leaf = it->path().filename().string();
    if (!leaf.empty() && leaf[0] == '.') {  // (.DS_Store and the like)
      if (it->is_directory(ec)) it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(ec)) continue;
    std::string rel = fs::relative(it->path(), files, ec).generic_string();
    std::replace(rel.begin(), rel.end(), '/', '\\');
    mod.files.push_back(rel);
  }
  std::sort(mod.files.begin(), mod.files.end());
}

// The pictures under a mod's textures/: <id>.png (or .jpg, .tga, .bmp).
bool IsTextureId(const std::string& id) {
  // <w>x<h>-<format>-<16 hex digits>
  const size_t x = id.find('x'), d1 = id.find('-'), d2 = id.rfind('-');
  return x != std::string::npos && d1 != std::string::npos && d2 > d1 && x < d1 && id.size() - d2 - 1 == 16 &&
         id.find_first_not_of("0123456789abcdefx-") == std::string::npos;
}
void ReadTextures(Mod& mod) {
  std::error_code ec;
  for (fs::directory_iterator it(mod.dir / "textures", ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::string ext = Lower(it->path().extension().string());
    if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".tga" && ext != ".bmp") continue;
    const std::string id = Lower(it->path().stem().string());
    if (IsTextureId(id)) {
      mod.textures.push_back(id);
      mod.texture_files.push_back(it->path());
    } else {
      mod.notes.push_back("textures/" + it->path().filename().string() +
                          ": not named by a texture's id (<w>x<h>-<format>-<hash>, as the texture dump names them)");
    }
  }
}

uint64_t FileSize(const fs::path& p) {
  std::error_code ec;
  const auto n = fs::file_size(p, ec);
  return ec ? 0 : uint64_t(n);
}

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

std::vector<uint8_t> ReadAll(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Where a file id sits in the archives (pack_img.bin, as sub_823D5B58 reads
// it: per slot, per archive, a count and the ids, offsets and sizes).
struct Located {
  std::string archive;
  uint32_t offset = 0, size = 0;
};
bool Locate(const std::vector<uint8_t>& pack, uint32_t id, Located* out) {
  auto u = [&](uint32_t o, uint32_t* v) {
    if (size_t(o) + 4 > pack.size()) return false;
    *v = Be32(pack.data() + o);
    return true;
  };
  uint32_t table[4];
  for (int i = 0; i < 4; ++i)
    if (!u(uint32_t(i) * 4, &table[i])) return false;
  const auto& slots = SlotArchives();
  for (uint32_t slot = 0; slot < slots.size(); ++slot) {
    for (uint32_t a = 0; a < slots[slot].size(); ++a) {
      uint32_t p[4], count, ids, offsets, sizes;
      for (int i = 0; i < 4; ++i)
        if (!u(table[i] + 4 * slot, &p[i])) return false;
      if (!u(p[1] + 4 * a, &count) || !u(p[0] + 4 * a, &ids) || !u(p[2] + 4 * a, &offsets) ||
          !u(p[3] + 4 * a, &sizes) || count > kFileCount) {
        return false;
      }
      for (uint32_t k = 0; k < count; ++k) {
        uint32_t fid;
        if (!u(ids + 4 * k, &fid)) return false;
        if (fid != id) continue;
        out->archive = slots[slot][a];
        return u(offsets + 4 * k, &out->offset) && u(sizes + 4 * k, &out->size);
      }
    }
  }
  return false;
}

fs::path CacheDir() {
#if defined(__PROSPERO__)
  return ng2ps5::paths::Cache() / "mods";
#else
#if defined(IZUNA_LINUX_DESKTOP)
  return ng2ps5::paths::Cache() / "mods";
#else
  return rex::filesystem::GetExecutableFolder() / "cache" / "mods";
#endif
#endif
}

// What the game sees of the enabled mods (Activate; read-only after it).
struct Entry {
  std::string key;         // the game's path, as Key() makes it
  uint32_t id = 0;
  fs::path host;           // the file
  std::string guest_path;  // the same file to the game's CreateFile
  uint32_t guest_va = 0;
  uint32_t size = 0;
  std::string mod;  // its name, for the log
};
struct Plan {
  std::atomic<bool> active{false};
  bool log_loads = false;
  std::vector<Entry> entries;
  std::vector<int32_t> by_id;  // entry per file id, or -1
  std::unordered_map<std::string, size_t> by_key;
  std::vector<std::string> names;  // the game's path per file id (for the load log)
  Status status;
  std::atomic<uint32_t> loaded{0};
} g;

const char* GuestString(uint32_t va, size_t max, size_t* len) {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory || va < 0x1000) return nullptr;
  const char* s = memory->TranslateVirtual<const char*>(va);
  if (!s) return nullptr;
  size_t n = 0;
  while (n < max && s[n]) ++n;
  if (n == max) return nullptr;
  *len = n;
  return s;
}

// The game's path per file id, from its table in memory.
std::vector<std::string> ReadNames() {
  std::vector<std::string> names;
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return names;
  names.reserve(kFileCount);
  for (uint32_t id = 0; id < kFileCount; ++id) {
    const uint32_t va = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(kFileNames + 4 * id));
    size_t len = 0;
    const char* s = GuestString(va, 256, &len);
    if (!s) {
      REXLOG_WARN("[mods] the game's file table ends early, at id {} - not the disc this was made for?", id);
      break;
    }
    names.emplace_back(s, len);
  }
  return names;
}

bool Mount(const char* device, const char* link, const fs::path& root) {
  auto* vfs = REX_KERNEL_FS();
  if (!vfs) return false;
  auto d = std::make_unique<rex::filesystem::HostPathDevice>(device, root, true);
  if (!d->Initialize() || !vfs->RegisterDevice(std::move(d))) {
    REXLOG_WARN("[mods] could not mount {}", root.string());
    return false;
  }
  vfs->RegisterSymbolicLink(link, device);
  return true;
}

}  // namespace

fs::path Root() {
#if defined(__PROSPERO__)
  return ng2ps5::paths::App() / "Mods";
#else
#if defined(IZUNA_LINUX_DESKTOP)
  return ng2ps5::paths::WritableRoot() / "mods";
#else
  return rex::filesystem::GetExecutableFolder() / "mods";
#endif
#endif
}

fs::path TextureDumpDir() { return Root().parent_path() / "Texture Dump"; }

fs::path StateFile() {
#if defined(__PROSPERO__)
  return ng2ps5::paths::UserData() / "mods.toml";
#else
  return Root() / "mods.toml";
#endif
}

std::vector<Mod> List(Options* options) {
  // The folders there now.
  std::map<std::string, Mod> found;
  std::error_code ec;
  fs::create_directories(Root(), ec);
  for (fs::directory_iterator it(Root(), ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_directory(ec)) continue;
    const std::string folder = it->path().filename().string();
    if (folder.empty() || folder[0] == '.') continue;
    Mod mod;
    mod.folder = folder;
    mod.dir = it->path();
    ReadManifest(mod);
    if (mod.name.empty()) mod.name = folder;
    ReadFiles(mod);
    if (mod.script.empty() && fs::is_regular_file(mod.dir / "scripts" / "main.lua", ec)) mod.script = "scripts/main.lua";
    if (!mod.script.empty() && (mod.script.find("..") != std::string::npos ||
                                !fs::is_regular_file(mod.dir / fs::path(mod.script), ec))) {
      mod.notes.push_back("its script " + mod.script + " is not in its folder");
      mod.script.clear();
    }
    ReadTextures(mod);
    if (mod.files.empty() && mod.replace.empty() && mod.script.empty() && mod.textures.empty() &&
        mod.hide_shaders.empty()) {
      mod.notes.push_back("brings nothing (no files/, [replace], scripts/main.lua, textures/ or hide_shaders)");
    }
    found.emplace(Lower(folder), std::move(mod));
  }

  // The saved order and switches; folders not in it go at the end, on.
  std::vector<Mod> mods;
  Options opts;
  try {
    if (fs::exists(StateFile(), ec)) {
      const toml::table state = toml::parse_file(StateFile().string());
      opts.log_file_loads = state["log_file_loads"].value_or(false);
      opts.dump_textures = state["dump_textures"].value_or(false);
      if (const toml::array* list = state["mod"].as_array()) {
        for (const toml::node& n : *list) {
          const toml::table* t = n.as_table();
          if (!t) continue;
          auto it = found.find(Lower(Str(*t, "folder")));
          if (it == found.end()) continue;  // (deleted since)
          it->second.enabled = (*t)["enabled"].value_or(true);
          mods.push_back(std::move(it->second));
          found.erase(it);
        }
      }
    }
  } catch (const toml::parse_error& e) {
    REXLOG_WARN("[mods] {} could not be read ({}): every mod on, in name order", StateFile().string(),
                std::string(e.description()));
  }
  for (auto& [key, mod] : found) mods.push_back(std::move(mod));

  // The files two enabled mods both bring: the first one has them.
  std::unordered_map<std::string, std::string> taken;
  for (Mod& mod : mods) {
    if (!mod.enabled) continue;
    std::vector<std::string> targets = mod.files;
    for (const auto& [target, source] : mod.replace) targets.push_back(target);
    for (const std::string& f : targets) {
      auto [it, fresh] = taken.emplace(Key(f), mod.name);
      if (!fresh) mod.notes.push_back(f + ": " + it->second + " (above) has it");
    }
    for (const std::string& id : mod.textures) {
      auto [it, fresh] = taken.emplace("texture:" + id, mod.name);
      if (!fresh) mod.notes.push_back("texture " + id + ": " + it->second + " (above) has it");
    }
  }
  if (options) *options = opts;
  return mods;
}

bool Save(const std::vector<Mod>& mods, const Options& options) {
  toml::table state;
  state.insert("log_file_loads", options.log_file_loads);
  state.insert("dump_textures", options.dump_textures);
  toml::array list;
  for (const Mod& mod : mods) list.push_back(toml::table{{"folder", mod.folder}, {"enabled", mod.enabled}});
  state.insert("mod", std::move(list));
  std::error_code ec;
  fs::create_directories(StateFile().parent_path(), ec);
  const fs::path tmp = StateFile().string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    f << "# Which mods are on, in their order (the first one wins a file two mods both bring).\n"
         "# Written by the hub's MODS page.\n\n"
      << state << "\n";
    if (!f) return false;
  }
  fs::rename(tmp, StateFile(), ec);
  return !ec;
}

bool Remove(const Mod& mod, std::string* error) {
  std::error_code ec;
  // Only a folder directly under Mods/.
  if (mod.folder.empty() || mod.dir.parent_path() != Root()) {
    if (error) *error = "not a folder in Mods/";
    return false;
  }
  fs::remove_all(mod.dir, ec);
  if (ec && error) *error = ec.message();
  if (!ec) REXLOG_INFO("[mods] removed {}", mod.dir.string());
  return !ec;
}

void Activate(const fs::path& game_data) {
  Options options;
  std::vector<Mod> mods = List(&options);
  g.log_loads = options.log_file_loads;
  for (const Mod& mod : mods) {
    for (const std::string& note : mod.notes) REXLOG_WARN("[mods] {}: {}", mod.name, note);
  }
  // What they change in the drawing: textures (the first mod's), shaders left out.
  {
    std::unordered_map<std::string, fs::path> textures;
    int hidden = 0;
    for (const Mod& mod : mods) {
      if (!mod.enabled) continue;
      for (size_t i = 0; i < mod.textures.size(); ++i) textures.emplace(mod.textures[i], mod.texture_files[i]);
      for (const std::string& h : mod.hide_shaders) {
        ng2native::HideShader(std::strtoull(h.c_str(), nullptr, 16), true);
        ++hidden;
      }
    }
    if (!textures.empty() || hidden) {
      REXLOG_INFO("[mods] {} texture(s) replaced, {} shader(s) hidden", textures.size(), hidden);
    }
    ng2native::SetTextureReplacements(std::move(textures));
    if (options.dump_textures) {
      ng2native::SetTextureDump(true, TextureDumpDir());
      REXLOG_INFO("[mods] each texture the game draws is saved to {}", TextureDumpDir().string());
    }
  }
  const bool any = std::any_of(mods.begin(), mods.end(), [](const Mod& m) { return m.enabled; });
  if (!any && !g.log_loads) {
    REXLOG_INFO("[mods] {} mod(s) in {}, none on", mods.size(), Root().string());
    return;
  }

  g.names = ReadNames();
  if (g.names.size() != kFileCount) return;
  std::unordered_map<std::string, uint32_t> ids;
  for (uint32_t id = 0; id < g.names.size(); ++id) ids.emplace(Key(g.names[id]), id);
  g.by_id.assign(kFileCount, -1);

  std::vector<uint8_t> pack, load_img;  // (read when a [replace] needs them)
  bool cache_used = false;
  auto add = [&](const Mod& mod, const std::string& target, const fs::path& host, const std::string& guest) {
    const std::string key = Key(target);
    auto id = ids.find(key);
    if (id == ids.end()) {
      REXLOG_WARN("[mods] {}: {} is not one of the game's files - left out", mod.name, target);
      return false;
    }
    if (g.by_id[id->second] >= 0) return false;  // (a mod above has it: noted by List)
    const uint64_t size = FileSize(host);
    if (size == 0 || size > 0x7FFFFFFF) {
      REXLOG_WARN("[mods] {}: {} is empty or too large - left out", mod.name, target);
      return false;
    }
    Entry e;
    e.key = key;
    e.id = id->second;
    e.host = host;
    e.guest_path = guest;
    e.size = uint32_t(size);
    e.mod = mod.name;
    g.by_id[e.id] = int32_t(g.entries.size());
    g.by_key.emplace(key, g.entries.size());
    g.entries.push_back(std::move(e));
    return true;
  };

  for (const Mod& mod : mods) {
    if (!mod.enabled) continue;
    int brought = 0;
    for (const std::string& f : mod.files) {
      std::string host = f;  // (the game's backslashes, as the host's separators)
      std::replace(host.begin(), host.end(), '\\', '/');
      brought += add(mod, f, mod.dir / "files" / fs::path(host), "mods:\\" + mod.folder + "\\files\\" + f) ? 1 : 0;
    }
    for (const auto& [target, source] : mod.replace) {
      auto src = ids.find(Key(source));
      if (src == ids.end()) {
        REXLOG_WARN("[mods] {}: [replace] {}: {} is not one of the game's files", mod.name, target, source);
        continue;
      }
      if (pack.empty()) pack = ReadAll(game_data / "pack_img.bin");
      if (load_img.empty()) load_img = ReadAll(game_data / "load_img.bin");
      Located at;
      if (!Locate(pack, src->second, &at)) {
        REXLOG_WARN("[mods] {}: [replace] {}: {} is in none of the game's archives (DLC files cannot be used)",
                    mod.name, target, source);
        continue;
      }
      // The file's own size (load_img.bin); the archive's is padded to 2 KB.
      // A DLC file has none (the archives keep only a placeholder for it).
      const uint32_t exact =
          load_img.size() >= 16 + 8 * size_t(src->second + 1) ? Be32(load_img.data() + 16 + 8 * src->second) : 0;
      if (exact == 0 || exact > at.size) {
        REXLOG_WARN("[mods] {}: [replace] {}: {} is a DLC file, not one of the disc's - left out", mod.name, target,
                    source);
        continue;
      }
      const uint32_t size = exact;
      char leaf[32];
      std::snprintf(leaf, sizeof(leaf), "%u.bin", src->second);
      const fs::path cached = CacheDir() / leaf;
      std::error_code ec;
      if (FileSize(cached) != size) {
        fs::create_directories(CacheDir(), ec);
        std::ifstream in(game_data / at.archive, std::ios::binary);
        std::vector<char> bytes(size);
        in.seekg(at.offset);
        in.read(bytes.data(), std::streamsize(size));
        if (!in) {
          REXLOG_WARN("[mods] {}: [replace] {}: could not read {} from {}", mod.name, target, source, at.archive);
          continue;
        }
        std::ofstream out(cached, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), std::streamsize(size));
        if (!out) {
          REXLOG_WARN("[mods] {}: [replace] could not write {}", mod.name, cached.string());
          continue;
        }
      }
      cache_used = true;
      if (add(mod, target, cached, std::string("modcache:\\") + leaf)) {
        ++brought;
        REXLOG_INFO("[mods] {}: {} loads {} ({} bytes from {} at {:#x})", mod.name, target, source, size,
                    at.archive, at.offset);
      }
    }
    if (brought) {
      ++g.status.mods;
      REXLOG_INFO("[mods] on: {}{}{} - {} file(s)", mod.name, mod.version.empty() ? "" : " ", mod.version, brought);
    }
  }
  g.status.files = int(g.entries.size());

  // The paths the game's CreateFile is given instead, in guest memory.
  if (!g.entries.empty()) {
    auto* memory = REX_KERNEL_MEMORY();
    size_t total = 0;
    for (const Entry& e : g.entries) total += e.guest_path.size() + 1;
    const uint32_t base = memory ? memory->SystemHeapAlloc(uint32_t(total)) : 0;
    if (!base) {
      REXLOG_ERROR("[mods] no guest memory for the paths: mods off");
      g.entries.clear();
      g.by_key.clear();
      g.by_id.assign(kFileCount, -1);
      g.status = {};
      return;
    }
    uint32_t va = base;
    for (Entry& e : g.entries) {
      std::memcpy(memory->TranslateVirtual<char*>(va), e.guest_path.c_str(), e.guest_path.size() + 1);
      e.guest_va = va;
      va += uint32_t(e.guest_path.size() + 1);
    }
    if (!Mount("\\Device\\IzunaMods", "mods:", Root()) ||
        (cache_used && !Mount("\\Device\\IzunaModCache", "modcache:", CacheDir()))) {
      g.entries.clear();
      g.by_key.clear();
      g.by_id.assign(kFileCount, -1);
      g.status = {};
      return;
    }
  }
  REXLOG_INFO("[mods] {} mod(s) on, {} game file(s) replaced{}", g.status.mods, g.status.files,
              g.log_loads ? "; every file the game loads is logged" : "");
  g.active.store(true, std::memory_order_release);
  scripts::Start(mods);
}

Status Active() { return g.status; }

const std::vector<std::string>& FileNames() { return g.names; }

}  // namespace izuna::mods

using izuna::mods::g;

// --- The hooks (config/hooks/mods.toml) ---------------------------------------

// sub_823D5EE0, the loader's request for a file id (r3), at the test of its
// load-loose byte (r10): set for a file a mod brings.
void izunaModFileRequest(PPCRegister& r3, PPCRegister& r10) {
  if (!g.active.load(std::memory_order_acquire)) return;
  const uint32_t id = r3.u32 & 0xFFFF;
  const bool modded = id < g.by_id.size() && g.by_id[id] >= 0;
  if (g.log_loads && id < g.names.size()) {
    REXLOG_INFO("[mods] the game loads #{} {}{}", id, g.names[id], modded ? " (a mod's)" : "");
  }
  if (!modded) return;
  r10.u64 = 1;
  const uint32_t n = g.loaded.fetch_add(1, std::memory_order_relaxed);
  if (n < 64 && !g.log_loads) {
    const auto& e = g.entries[size_t(g.by_id[id])];
    REXLOG_INFO("[mods] {} from {}", g.names[id], e.mod);
  }
}

// sub_823D66A0 just before its CreateFile (r3: the path it made): the loose
// path of a mod's file goes to that file.
void izunaModOpenPath(PPCRegister& r3) {
  if (!g.active.load(std::memory_order_acquire) || g.entries.empty()) return;
  size_t len = 0;
  const char* path = izuna::mods::GuestString(r3.u32, 300, &len);
  const size_t prefix = std::strlen(izuna::mods::kLoosePrefix);
  if (!path || len <= prefix || strncasecmp(path, izuna::mods::kLoosePrefix, prefix) != 0) return;
  auto it = g.by_key.find(izuna::mods::Key(std::string_view(path + prefix, len - prefix)));
  if (it == g.by_key.end()) return;
  r3.u64 = g.entries[it->second].guest_va;
}

// sub_823D6128, a file's size (r11, before its rounding to 2 KB) for an id
// (r31): at least the mod's file's.
void izunaModFileSize(PPCRegister& r11, PPCRegister& r31) {
  if (!g.active.load(std::memory_order_acquire)) return;
  const uint32_t id = r31.u32 & 0xFFFF;
  if (id >= g.by_id.size() || g.by_id[id] < 0) return;
  const uint32_t size = g.entries[size_t(g.by_id[id])].size;
  if (size > r11.u32) r11.u64 = size;
}

// sub_83649798, once load_img.bin is loaded and its offsets made pointers:
// the sizes of the mods' files in its table, for whatever reads it directly.
void izunaModFileTable(PPCRegister& /*r1*/) {
  if (!g.active.load(std::memory_order_acquire) || g.entries.empty()) return;
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return;
  const uint32_t buffer = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(izuna::mods::kLoadImg));
  if (!buffer) return;
  const uint32_t sizes = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(buffer));
  const uint32_t count = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(buffer + 4));
  if (count != izuna::mods::kFileCount || !sizes) {
    REXLOG_WARN("[mods] load_img.bin as loaded is not as expected ({} files): its sizes left as they are", count);
    return;
  }
  int raised = 0;
  for (const auto& e : g.entries) {
    uint8_t* p = memory->TranslateVirtual<uint8_t*>(sizes + 8 * e.id);
    if (e.size > rex::memory::load_and_swap<uint32_t>(p)) {
      rex::memory::store_and_swap<uint32_t>(p, e.size);
      ++raised;
    }
  }
  REXLOG_INFO("[mods] load_img.bin: {} of the mods' files are larger than the game's, their sizes raised", raised);
}
