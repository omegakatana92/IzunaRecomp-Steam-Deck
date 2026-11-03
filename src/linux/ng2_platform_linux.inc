// Native desktop services. Included only from ng2_platform.cpp's Linux branch.
#include <SDL3/SDL.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <sstream>
extern char** environ;

namespace ng2 {
std::optional<std::filesystem::path> PickFolder(const std::string& title,
                                               const std::filesystem::path& start_in) {
  // Use argv, never a shell: folder names and titles are untrusted data.
  // zenity works with both Wayland and X11, including Deck Desktop Mode.
  int fd[2];
  if (pipe(fd) != 0) return std::nullopt;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, fd[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, fd[0]);
  posix_spawn_file_actions_addclose(&actions, fd[1]);
  std::string caption = "--title=" + title;
  std::string initial = "--filename=" + start_in.string() + "/";
  char* argv[] = {const_cast<char*>("zenity"), const_cast<char*>("--file-selection"),
                 const_cast<char*>("--directory"), caption.data(), initial.data(), nullptr};
  pid_t pid{};
  const int rc = posix_spawnp(&pid, "zenity", &actions, nullptr, argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  close(fd[1]);
  if (rc != 0) {
    close(fd[0]);
    REXLOG_WARN("Folder picker requires zenity: spawn error {}", rc);
    return std::nullopt;
  }
  std::string value;
  char buffer[4096];
  ssize_t count;
  while ((count = read(fd[0], buffer, sizeof(buffer))) != 0) {
    if (count < 0) { if (errno == EINTR) continue; break; }
    value.append(buffer, size_t(count));
  }
  close(fd[0]);
  int status{};
  while (waitpid(pid, &status, 0) < 0) { if (errno != EINTR) return std::nullopt; }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || value.empty()) return std::nullopt;
  if (value.back() == '\n') value.pop_back();
  return value.empty() ? std::nullopt : std::optional<std::filesystem::path>(value);
}
bool ThisProcessIsForeground() { return SDL_GetKeyboardFocus() != nullptr; }
std::vector<MonitorInfo> Monitors() {
  std::vector<MonitorInfo> result;
  int count{};
  SDL_DisplayID* displays = SDL_GetDisplays(&count);
  if (!displays) return result;
  for (int i = 0; i < count; ++i) {
    SDL_Rect full{}, usable{};
    if (!SDL_GetDisplayBounds(displays[i], &full)) continue;
    if (!SDL_GetDisplayUsableBounds(displays[i], &usable)) usable = full;
    MonitorInfo m;
    m.index = i;
    m.full_width = full.w; m.full_height = full.h;
    m.width = usable.w; m.height = usable.h;
    m.scale = SDL_GetDisplayContentScale(displays[i]);
    if (m.scale <= 0) m.scale = 1;
    m.primary = displays[i] == SDL_GetPrimaryDisplay();
    result.push_back(m);
  }
  SDL_free(displays);
  return result;
}
float SystemScale() {
  for (const auto& m : Monitors()) if (m.primary) return m.scale;
  return 1;
}
bool MonitorWorkArea(int index, int& width, int& height, float& scale) {
  const auto all = Monitors();
  if (all.empty()) return false;
  const MonitorInfo* selected = &all.front();
  for (const auto& m : all) if (m.index == index) { selected = &m; break; }
  width = selected->width; height = selected->height; scale = selected->scale;
  return width > 0 && height > 0;
}
bool LaunchDetached(const std::filesystem::path& exe, const std::string& args) {
  // Current callers pass whitespace-separated flags. No shell expansion.
  std::filesystem::path launch = exe;
  // Relaunch the outer AppImage so its runtime owns a fresh mount; the old
  // mount may be removed as soon as this process exits.
  if (const char* image = std::getenv("APPIMAGE"); image && *image) {
    const std::filesystem::path candidate(image);
    std::error_code ec;
    if (candidate.is_absolute() && std::filesystem::is_regular_file(candidate, ec)) launch = candidate;
  }
  std::vector<std::string> words{launch.string()};
  std::istringstream input(args);
  for (std::string word; input >> word;) words.push_back(std::move(word));
  std::vector<char*> argv;
  for (auto& word : words) argv.push_back(word.data());
  argv.push_back(nullptr);
  pid_t pid{};
  return posix_spawn(&pid, launch.c_str(), nullptr, nullptr, argv.data(), environ) == 0;
}
void ExitNow(int code) { std::_Exit(code); }
}  // namespace ng2
