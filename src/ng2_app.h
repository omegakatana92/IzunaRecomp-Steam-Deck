// ng2 - ReXGlue Recompiled Project

#pragma once

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/cvar.h>
#include <rex/ui/flags.h>

#include "ng2_diagnostics.h"
#include "ng2_disc.h"
#include "ng2_menu.h"
#include "ng2_platform.h"
#include "ng2_saveimport.h"
#include "ng2_autoskip.h"
#include "ng2_perf.h"
#include "ng2_perf_hud.h"
#include "ng2_settings.h"
#include "ng2_tuning.h"
// The hub (the front screen), its in-game menu, the app folder's layout, and
// the texture hook behind the PlayStation button pictures (and the PS5's
// dump) - the PS5's, and Windows' with the PC's own input, sound and folders
// in the same files (ps5_pad.cpp, ps5_sound.cpp, ps5_paths.cpp).
#include "ps5/hub/hub_ui.h"
#include "ps5/hub/ps5_hub.h"
#include "ps5/overlay/ps5_overlay.h"
#include "ps5/ps5_button_prompts.h"
#include "ps5/ps5_pad.h"
#include "ps5/ps5_paths.h"
#include "ps5/ps5_textures.h"
#ifndef NG2_PS5_VERSION
#define NG2_PS5_VERSION "dev"
#endif
#include <rex/system/flags.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xmemory.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/ui/window.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include "native/izuna_gpu.h"  // [IzunaRecomp, M4] the native graphics system (every build)
#if defined(NG2_PS5_HOST_TEST)
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/presenter.h>
#include "native/native_renderer.h"
#endif
#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <optional>
#include <system_error>
#include <thread>
#include <vector>

#include "izuna/izuna_mods.h"
#include "izuna/izuna_pc.h"
#include "izuna/izuna_scripts.h"
#include "izuna/izuna_shaders.h"
#include "izuna/izuna_shader_overlay.h"
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>

// Defined in patch_hooks.cpp: the internal render size patch and the video
// mode, driven from the settings through the cvars the hooks read.
REXCVAR_DECLARE(int32_t, ng2_render_width);
REXCVAR_DECLARE(int32_t, ng2_render_height);
REXCVAR_DECLARE(int32_t, ng2_video_mode);

// Defined in patch_hooks.cpp: guest address of a path that cannot resolve,
// filled in once the runtime exists.
extern uint32_t g_ng2_skip_path_va;

class Ng2App : public rex::ReXApp {
 public:
  Ng2Settings settings_;

  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Ng2App>(new Ng2App(ctx, "ng2",
        PPCImageConfig));
  }

  // The graphics system the kernel's Vd* calls reach: ours (IzunaRecomp, M4) -
  // there is no GPU emulation in the build to fall back to.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    ApplyDisplaySettings();
    ApplyTuning();

    // [IzunaRecomp] The native graphics system: the game's sync, vblanks and
    // swap, with the Kasumi renderer drawing - no GPU plugin, no emulated GPU
    // (the SDK is built without it: REXGLUE_GPU_EMULATION off).
    config.graphics = ng2native::CreateIzunaGraphicsSystem();
    config.gpu_plugin.clear();

    // Treat the title as the full, licensed version so purchased DLC is
    // visible. 0 = nothing licensed, 1 = first license (the full game), -1 =
    // everything. Only applied when the value is still the default, so
    // --license_mask on the command line continues to win.
    if (REXCVAR_GET(license_mask) == 0)
      REXCVAR_SET(license_mask, 1u);
  }

  // Default the game data root to whatever the hub last chose, or game/ beside
  // the executable. --game_data_root still wins when given.
  //
  // This is also where the settings file is read. It has to happen before
  // OnFinalizePaths (which needs the game path) and before OnPreSetup (which
  // needs everything else), and both run later.
  void OnConfigurePaths(rex::PathConfig& paths) override {
#if defined(IZUNA_LINUX_DESKTOP)
    ng2ps5::paths::EnsureFolders();
    paths.config_path = ng2ps5::paths::UserData() / "ng2.toml";
    if (rex::cvar::GetFlagByName("log_file").empty()) {
      const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
      rex::cvar::SetFlagByName("log_file", (ng2ps5::paths::Logs() / fmt::format("ng2_{}.log", stamp)).string());
    }
#endif
    settings_.Load();
    ApplyEnvironmentOverrides();
    // [IzunaRecomp] Logging off (the Developer tab): the log never starts
    // (rex::InitLogging, next, with everything off: no file, no threads). A
    // --log_level given on the command line other than the default still wins.
    if (!settings_.logging && REXCVAR_GET(log_level) == "info") rex::cvar::SetFlagByName("log_level", "off");
#if defined(__PROSPERO__)
    // [ps5 port] The app folder's own folders, the PS5's first-run values
    // over the PC defaults (once), and the shader storage in UserData so it
    // survives updates of the app and is never shipped (CLAUDE.md 5.8).
    ng2ps5::paths::EnsureFolders();
    if (settings_.ps5_defaults_version < ng2ps5::hub::kPs5DefaultsVersion) {
      ng2ps5::hub::UpdatePs5Defaults(settings_);
      settings_.Save();
    }
    // The aspect ratio is always kept on the PS5 (the hub has no toggle for
    // it any more), whatever an older settings file says.
    settings_.letterbox = true;
    rex::cvar::SetFlagByName("cache_root", (ng2ps5::paths::Cache() / "shaders").string());
#else
    // [IzunaRecomp] PC defaults rounds, each once per settings file (then the
    // player's choice stands). Round 1: the keyboard plays alongside any
    // controller (both drive player 1) - before it, a settings rule meant for
    // the PS5 turned it off at every load, so no PC install has chosen "off".
    if (settings_.pc_defaults_version < 4) {
      if (settings_.pc_defaults_version < 1) settings_.keyboard_control = true;
      if (settings_.pc_defaults_version < 2) {
        // Round 2: fullscreen at the screen's size, the picture drawn for it
        // (Render scale Auto, the field's default), a 1080p window when it is
        // one; and the trigger keys the right way round - NG2 blocks on the
        // left trigger and centres the camera on the right (izuna_keys.h; the
        // owner's test10: "shift isn't actually block").
        settings_.fullscreen = true;
        if (settings_.window_width == 1280 && settings_.window_height == 720) {
          settings_.window_width = 1920;
          settings_.window_height = 1080;
        }
        const int lt = izuna::keys::Find("lt"), rt = izuna::keys::Find("rt");
        if (settings_.pc_keys[size_t(rt)] == "Shift" && settings_.pc_keys[size_t(lt)] == "Q")
          std::swap(settings_.pc_keys[size_t(rt)], settings_.pc_keys[size_t(lt)]);
      }
      // Round 3 (test24-25) made Low input latency the default; round 4
      // (test26) puts Standard back - the owner's choice stands from here.
      if (settings_.pc_defaults_version == 3) settings_.pc_low_latency = false;
      settings_.pc_defaults_version = 4;
      settings_.Save();
    }
#endif

    settings_.Clamp();
    ApplyWindowSettings();
    if (paths.game_data_root.empty()) {
      paths.game_data_root = settings_.ResolvedGamePath();
    }
    game_data_root_ = paths.game_data_root;
    UsePortableUserData(paths);
  }

  // The profile, its saves and the installed DLC, beside the executable.
  //
  // The runtime's own default is under Documents, which means a copy of the
  // game moved to another drive arrives with no saves and there is nothing
  // next to the executable worth backing up. `user\` beside ng2.exe makes the
  // whole install one folder.
  //
  // Whatever was already at the old location is MOVED IN the first time rather
  // than orphaned - which matters, because that is where any save imported so
  // far is. The old path is never hard-coded: it is read from what the runtime
  // had already filled in before this hook ran, so this survives the SDK
  // changing its mind about where the default should be.
  void UsePortableUserData(rex::PathConfig& paths) {
#if defined(__PROSPERO__) || defined(IZUNA_LINUX_DESKTOP)
    // [desktop/PS5] Always the app's UserData folder (ps5_paths.h); there is no
    // older location to bring anything across from.
    paths.user_data_root = ng2ps5::paths::UserData();
    paths.cache_root = ng2ps5::paths::Cache() / "shaders";
    return;
#endif
    const auto local = rex::filesystem::GetExecutableFolder() / "user";
    const auto previous = paths.user_data_root;
    if (previous == local)
      return;

    std::error_code ec;
    const bool have_local = std::filesystem::exists(local, ec);
    if (!have_local && !previous.empty() &&
        std::filesystem::is_directory(previous, ec)) {
      std::filesystem::create_directories(local, ec);
      std::filesystem::copy(previous, local,
                            std::filesystem::copy_options::recursive |
                                std::filesystem::copy_options::skip_existing,
                            ec);
      if (ec) {
        REXLOG_WARN("User data: could not bring {} across to {} ({})",
                    previous.string(), local.string(), ec.message());
      } else {
        REXLOG_INFO("User data: brought {} across to {}", previous.string(),
                    local.string());
      }
    }
    std::filesystem::create_directories(local, ec);
    paths.user_data_root = local;
    REXLOG_INFO("User data: {}", local.string());
  }

  // The hub, on the one hook where the window and ImGui are live but the
  // runtime has not been built yet - so the game path START GAME resumes with
  // is the one that actually gets mounted.
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults,
      std::function<void(rex::PathConfig)> resume) override {
#if defined(__PROSPERO__)
    // [ps5 port] The hub, every launch: it is the app's front screen, not a
    // first-run setup, and OPTIONS on it starts the game at once.
    return ShowPs5Hub(defaults, std::move(resume));
#endif
    const bool path_from_cli = !REXCVAR_GET(game_data_root).empty();

    // Remember a game folder given on the command line, so a later launch with
    // no arguments (the relaunch back to the hub after the game's own Quit
    // Game, or a plain double-click) still finds it.
    if (path_from_cli) {
      const std::string cli_root = REXCVAR_GET(game_data_root);
      if (ng2::InspectFolder(std::filesystem::path(cli_root)).Usable() &&
          (!settings_.configured || settings_.game_path != cli_root)) {
        settings_.game_path = cli_root;
        settings_.configured = true;
        settings_.Save();
        REXLOG_INFO("Settings: remembered the command-line game folder {}",
                    cli_root);
      }
    }

    // [IzunaRecomp] Windows: START GAME asks for the game's folder when there
    // is none (a folder given on the command line is remembered above).
    ng2ps5::paths::SetGameData(path_from_cli ? std::filesystem::path(REXCVAR_GET(game_data_root))
                                             : settings_.ResolvedGamePath());
    return ShowPs5Hub(defaults, std::move(resume));
  }

  // The hub: the front screen on every build.
  //
  // This hook runs after OnPreSetup has applied the display settings and the
  // tuning file, but before the runtime - and with it the GPU - is built. So
  // everything the hub changed is applied again on START GAME, and settings
  // the GPU reads once at start-up (the resolution scale, the render target
  // path, the texture cache, the shader mode) take effect without a relaunch.
  std::optional<rex::PathConfig> ShowPs5Hub(const rex::PathConfig& defaults,
                                            std::function<void(rex::PathConfig)> resume) {
    StartUiPump();
    ng2ps5::hub::ShowOptions options;
    options.settings = &settings_;
#if defined(__PROSPERO__)
    options.version = std::string(NG2_PS5_VERSION) + " native dev  (Ninja's Dawn " NG2_VERSION ")";  // [native renderer track]
#else
    options.version = std::string("IzunaRecomp ") + IZUNA_VERSION;
    options.pick_game_folder = [this] {
      return ng2::PickFolder("Choose the folder with your copy of Ninja Gaiden II (default.xex)",
                             settings_.ResolvedGamePath());
    };
#endif
    options.write_diagnostics = [] {
      const auto result = ng2::WriteDiagnostics(Ng2Settings::Path(), ng2ps5::paths::Logs());
      return result.ok ? "Written to logs/" + result.file.filename().string()
                       : "Could not write diagnostics: " + result.error;
    };
    options.start = [this, defaults, resume](std::vector<std::filesystem::path> imports) {
      app_context().CallInUIThreadDeferred([this, defaults, resume, imports] {
        StopUiPump();
        hub_open_.store(false, std::memory_order_release);
        settings_.Clamp();
        settings_.Save();
        if (!imports.empty())
          ng2::QueueSaveImports(imports);
        ApplyDisplaySettings();
        ApplyTuning();
        ng2::ApplyLiveSettings(settings_, window());
        rex::PathConfig paths = defaults;
        paths.game_data_root = REXCVAR_GET(game_data_root).empty() ? settings_.ResolvedGamePath()
                                                                     : std::filesystem::path(REXCVAR_GET(game_data_root));
        resume(paths);
      });
    };
    options.quit = [this] {
      app_context().CallInUIThreadDeferred([this] {
        StopUiPump();
        settings_.Save();
        std::fflush(nullptr);
        rex::FlushLogging();
        ng2::ExitNow(0);
      });
    };
    // NG2_AUTOSTART: straight past the hub, for unattended test runs (the
    // host test build; on a console nothing sets it).
    if (const char* autostart = std::getenv("NG2_AUTOSTART"); autostart && *autostart) {
      REXLOG_INFO("PS5 hub: NG2_AUTOSTART - starting the game without the hub");
      options.start({});
      return std::nullopt;
    }
    hub_open_.store(true, std::memory_order_release);
    ng2ps5::hub::Show(imgui_drawer(), std::move(options));
    return std::nullopt;
  }

#if defined(__PROSPERO__)
  // [ps5 port] What the guest is told and how far it is scaled, from the
  // hub's resolution preset (CLAUDE.md 5.7). The guest is told the Xbox
  // 360's own 1280x720 HD mode - the frame the game is built for - and the
  // GPU renders it at an exact 3x, 2x or 1x; the presenter takes that to the
  // console's video output, bars and all if Keep aspect ratio is on.
  void ApplyPs5Display() {
    static constexpr int kPresetScale[] = {3, 2, 1};
    settings_.resolution_scale = settings_.ps5_render_scale > 0
                                     ? settings_.ps5_render_scale
                                     : kPresetScale[std::clamp(settings_.ps5_resolution, 0, 2)];
    REXCVAR_SET(video_mode_width, 1280);
    REXCVAR_SET(video_mode_height, 720);
    if (rex::cvar::GetFlagInfo("video_mode_explicit"))
      rex::cvar::SetFlagByName("video_mode_explicit", "true");
    REXCVAR_SET(video_mode_refresh_rate, double(settings_.fps));
    if (settings_.internal_720p) {
      REXCVAR_SET(ng2_render_width, 1280);
      REXCVAR_SET(ng2_render_height, 720);
    } else {
      REXCVAR_SET(ng2_render_width, 0);
      REXCVAR_SET(ng2_render_height, 0);
    }
    REXCVAR_SET(ng2_video_mode, settings_.video_mode);
    const int base_w = settings_.internal_720p ? 1280 : 1120;
    const int base_h = settings_.internal_720p ? 720 : 584;
    REXLOG_INFO("Display (PS5): preset {}, renders {}x{} ({}x of {}x{}), {} Hz, keep aspect {}",
                settings_.ps5_resolution, base_w * settings_.resolution_scale,
                base_h * settings_.resolution_scale, settings_.resolution_scale, base_w, base_h,
                settings_.fps, settings_.letterbox);
  }
#endif

  // The app's dialogs (each lives for the whole run, drawing only when it has
  // something to show) and its keys. F4 is the SDK's own cvar browser.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    // The window title names the port's version, not the SDK's.
    if (auto* w = window()) {
#if defined(IZUNA_VERSION)
      w->SetTitle(std::string("Ninja Gaiden II  -  IzunaRecomp ") + IZUNA_VERSION);
#else
      w->SetTitle(std::string("Ninja Gaiden II  -  v") + NG2_VERSION);
#endif
    }

    // [IzunaRecomp] What the script mods draw (izuna_scripts.h).
    script_overlay_ = std::make_unique<izuna::scripts::Overlay>(drawer, immediate_drawer());

    // The readouts, and the sampler behind them (it ticks twice a second, so
    // a readout switched on starts from a current figure).
    ng2::SetHudSettings(&settings_);
    ng2::StartPerfMonitor();
    perf_hud_ = std::make_unique<ng2::PerfHudOverlay>(drawer);

    // The in-game menu. Created now, enabled in OnPostSetup: the hub has the
    // controller until the game starts.
    {
      ng2ps5::overlay::Callbacks callbacks;
      callbacks.apply_live = [this] {
        ng2::ApplyLiveSettings(settings_, window());
        ng2::SetAutoSkipEnabled(false);  // (cinematics skip removed from the PS5 app: see below)
#if defined(__PROSPERO__)
        rex::cvar::SetFlagByName("native_vblank_display_locked",
                                 settings_.ps5_display_locked_vblank ? "true" : "false");
#endif
        rex::cvar::SetFlagByName("audio_resync", settings_.ps5_audio_resync ? "true" : "false");
        rex::cvar::SetFlagByName("gpu_profile_command_thread", settings_.ps5_profile_gpu ? "true" : "false");
        rex::cvar::SetFlagByName("gpu_profile_thread_name",
                                 settings_.ps5_profile_target == 1 ? "Main XThread" : "GPU Commands");
      };
      callbacks.return_to_hub = [this] {
        app_context().CallInUIThreadDeferred([this] { ReturnToMenu(); });
      };
      callbacks.quit_home = [this] {
        app_context().CallInUIThreadDeferred([this] {
          ShutdownCleanlyForRelaunch();
          std::fflush(nullptr);
          rex::FlushLogging();
          ng2::ExitNow(0);
        });
      };
      ps5_overlay_ = std::make_unique<ng2ps5::overlay::Overlay>(drawer, &settings_, std::move(callbacks));
    }

    // F8 shows or hides the readouts (which ones stays as chosen).
    rex::ui::RegisterBind(
        "bind_ng2_hud", "F8", "Show or hide the on-screen readouts", [this] {
          settings_.hud_enabled = !settings_.hud_enabled;
          REXLOG_INFO("F8: on-screen readouts {}",
                      settings_.hud_enabled ? "on" : "off");
          settings_.Save();
        });

    // Escape: the in-game menu in the game (QuitFromEscape).
    rex::ui::RegisterBind("bind_ng2_quit", "Escape", "Quit the game",
                          [this] { QuitFromEscape(); });

    // [IzunaRecomp] Alt+Enter: fullscreen or a window, saved as the setting.
    rex::ui::RegisterBind("bind_ng2_fullscreen", "Alt+Return", "Toggle fullscreen", [this] {
      settings_.fullscreen = !settings_.fullscreen;
      settings_.Save();
      ng2::ApplyLiveSettings(settings_, window());
      REXLOG_INFO("Alt+Enter: {}", settings_.fullscreen ? "fullscreen" : "windowed");
    });

    // F10: the in-game menu (from the game's start; the hub has its own keys).
    rex::ui::RegisterBind("bind_ng2_settings", "F10", "Open or close the in-game menu", [this] {
      if (ps5_overlay_ && ps5_overlay_->enabled()) ps5_overlay_->Toggle();
    });
  }

  // The menu's own font and theme. OnConfigureFonts runs while the ImGui atlas
  // is still open; OnConfigureStyle right after the SDK has applied its own
  // defaults, so this is a replacement rather than a patch.
  void OnConfigureFonts(ImFontAtlas* atlas) override {
#if defined(__PROSPERO__)
    ng2ps5::hub::ConfigureFonts(atlas);  // [ps5 port] built into the app
    ng2::LoadMenuFonts(atlas);
#else
    // [IzunaRecomp] The UI fonts first: the first one added is ImGui's default
    // (the readouts, the scripts' text); the hub names its own.
    ng2::LoadMenuFonts(atlas);
    ng2ps5::hub::ConfigureFonts(atlas);
#endif
  }

  void OnConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) override {
    ng2::ApplyMenuStyle(imgui_style);
    (void)ui_style;
  }

  // Everything this port started, stopped before the process goes.
  //
  // Not in OnShutdown: Escape and the close button both end in the SDK's
  // "Title terminated; hard-exiting process", and OnShutdown does not run on
  // that path (measured). The settings are written here, so a change made in
  // the in-game menu and never explicitly saved survives quitting.
  void ShutdownCleanly() {
    if (shutting_down_)
      return;
    shutting_down_ = true;
    StopTitleWatcher();
    ng2::StopPerfMonitor();
    settings_.Clamp();
    settings_.Save();
    REXLOG_INFO("Shutdown: settings saved");
    StartExitWatchdog();
  }

  // Escape's quit: release what we own, then take the close button's path.
  //
  // It used to ask the runtime to quit gracefully (QuitFromUIThread), and
  // measured, that never comes back: our own teardown ran to completion and
  // logged it, and the process sat there until the watchdog below killed it
  // three seconds later, window black the whole time. The guest's threads are
  // fibers and the graceful path waits on something that never finishes. The
  // close button never showed it, because the SDK's close path terminates the
  // title and hard-exits ("Title terminated; hard-exiting process."). So
  // Escape now asks the window to close, which is exactly that path, with the
  // watchdog kept behind it in case the request is ever swallowed. Fable 2
  // made the same change first and measured it: 0.4 s where the watchdog
  // path took 3.3 s.
  //
  // Reachable from a test seam as well as the key: NG2_QUIT_AFTER=<seconds>
  // fires this from a timer, so whether the quit really exits, and how fast,
  // is something a script measures rather than something anyone believes.
  void QuitFromEscape() {
    // [IzunaRecomp] On the hub Escape is Back (ps5_pad.cpp), not a way out.
    if (hub_open_.load(std::memory_order_acquire))
      return;
    // [IzunaRecomp] In the game Escape opens (and closes) the in-game menu,
    // whose Quit to desktop asks first - a stray press no longer ends the game.
    if (ps5_overlay_ && ps5_overlay_->enabled() && !shutting_down_) {
      ps5_overlay_->Toggle();
      return;
    }
    REXLOG_INFO("Escape: quitting");
    ShutdownCleanly();
    if (auto* w = window()) {
      w->RequestClose();
    } else {
      app_context().QuitFromUIThread();
    }
  }

  void ArmQuitSeam() {
    const int after = EnvInt("NG2_QUIT_AFTER", 0);
    if (after <= 0)
      return;
    REXLOG_INFO("Quit seam: Escape's path fires in {} s", after);
    std::thread([this, after] {
      std::this_thread::sleep_for(std::chrono::seconds(after));
      app_context().CallInUIThreadDeferred([this] { QuitFromEscape(); });
    }).detach();
  }

  // Make sure the process actually goes. Our own teardown is done by the time
  // this starts - the settings are written, our threads are joined - so what
  // remains is the runtime's, and it is not entitled to hang. On the close
  // request path this never fires; it is the backstop.
  void StartExitWatchdog() {
    std::thread([] {
      std::this_thread::sleep_for(std::chrono::seconds(3));
      REXLOG_WARN("Shutdown: the runtime did not finish in 3 s; exiting now");
      std::fflush(nullptr);
      rex::FlushLogging();
      ng2::ExitNow(0);  // [ps5 port] _Exit, or the PS5 shell's close
    }).detach();
  }

  // [IzunaRecomp] Windows: the game's shaders from the player's own copy,
  // built (or loaded from the last start's build) before the game's first
  // instruction runs (izuna/izuna_shaders.h) - on a thread of its own, the
  // progress on screen and in the title, then the game started. ng2.exe has
  // no shaders of its own. (The PS5 and the Linux host test, with no DXC,
  // draw with the table built in at build time.)
  void OnBeforeModuleResume(std::function<void()> start) override {
#if defined(_WIN32) || defined(IZUNA_LINUX_DESKTOP)
    auto* state = REX_KERNEL_STATE();
    auto module = state ? state->GetExecutableModule() : rex::system::object_ref<rex::system::UserModule>();
    auto* xex = module ? module->xex_module() : nullptr;
    if (!xex || !xex->base_address()) {
      start();  // (no image to read: the renderer says so and draws nothing)
      return;
    }
    const uint32_t base = xex->base_address(), size = xex->image_size();
    const auto exe_dir = rex::filesystem::GetExecutableFolder();
    std::filesystem::path cache = exe_dir / "cache" / "shaders";
    #if defined(_WIN32)
    if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA"); local && *local)
      cache = std::filesystem::path(local) / L"IzunaRecomp" / L"shaders";
    #else
    cache = ng2ps5::paths::Cache() / "shaders";
    #endif
    REXLOG_INFO("[izuna] shaders: preparing from the game image ({:08X}, {} MB)", base, size >> 20);
    // The panel in the window (empty until the first shader is built: a start
    // that loads the saved build never shows it).
    shader_progress_.done.store(0);
    shader_progress_.total.store(0);
    if (!shader_overlay_) shader_overlay_ = std::make_unique<izuna::shaders::ProgressOverlay>(imgui_drawer(), &shader_progress_);
    StartUiPump();  // (stopped when the hub's START closed it: the window repaints for the panel)
    std::thread([this, start = std::move(start), base, size, exe_dir, cache]() mutable {
      // The image copied out of guest memory, page by page where readable (a
      // page the loader left without access reads as zeros, never faults).
      const std::vector<uint8_t> image =
          izuna::shaders::CopyReadable(REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(base), size);
      static izuna::shaders::Table table;
      auto last = std::make_shared<std::atomic<int>>(-1);
      const auto report = izuna::shaders::Prepare(
          image.data(), image.size(), cache,
#if defined(_WIN32)
          exe_dir / "dxcompiler.dll",
#else
          exe_dir / "libdxcompiler.so",
#endif
          table,
          [this, last](size_t done, size_t total) {
            shader_progress_.total.store(int(total), std::memory_order_relaxed);
            shader_progress_.done.store(int(done), std::memory_order_relaxed);
            const int pct = int(done * 100 / std::max<size_t>(total, 1));
            if (last->exchange(pct) == pct) return;
            app_context().CallInUIThreadDeferred([this, pct] {
              if (auto* w = window())
                w->SetTitle(fmt::format("Ninja Gaiden II  -  IzunaRecomp  -  preparing shaders from your game: {}%",
                                        pct));
            });
          });
      if (!report.ok) {
        // ng2.exe carries no shaders of its own: without these the game
        // cannot be drawn, so the player is told why rather than shown black.
        izuna::pc::ShowError(fmt::format(
            "The game's shaders could not be prepared from your copy of Ninja Gaiden II ({}).\n\n"
            "Check that the platform DXC library is beside the executable, and that the game folder is a complete copy of the disc.",
            report.error));
        ng2::ExitNow(1);
      }
      izuna::shaders::SetActive(&table);
      REXLOG_INFO("[izuna] shaders: {} {} from the game in {:.1f} s ({} found); cache {}", report.built,
                  report.from_cache ? "loaded (built at an earlier start)" : "built", report.seconds, report.found,
                  cache.string());
      app_context().CallInUIThreadDeferred([this, start = std::move(start)]() mutable {
        if (auto* w = window()) w->SetTitle(std::string("Ninja Gaiden II  -  IzunaRecomp ") + IZUNA_VERSION);
        StopUiPump();
        shader_overlay_.reset();
        start();
      });
    }).detach();
#else
    start();
#endif
  }

  void OnPostLaunchModule(rex::system::XThread* thread) override {
    if (thread)
      main_guest_thread_id_ = thread->thread_id();
    StartTitleWatcher();
#if defined(NG2_PS5_HOST_TEST)
    StartCaptures();
#endif
  }

#if defined(NG2_PS5_HOST_TEST)
  // [native renderer track, host test] NG2_CAPTURE="12,40,75": at each of
  // those seconds after launch, the frame shown is saved as
  // native_frames/cap_<s>.rgba - the native renderer's next frame, or with
  // NG2_CAPTURE_PRESENTER the presenter's picture. Two runs with the same
  // pad script give pairs to compare (references stay out of the repo).
  void StartCaptures() {
    const char* list = std::getenv("NG2_CAPTURE");
    if (!list || !*list)
      return;
    // ("+N": N seconds after native_watch_tex's texture first showed)
    std::vector<int> times;
    std::vector<bool> relative;
    for (const char* p = list; *p;) {
      relative.push_back(*p == '+');
      times.push_back(std::atoi(*p == '+' ? p + 1 : p));
      while (*p && *p != ',') ++p;
      if (*p) ++p;
    }
    std::thread([this, times, relative] {
      const auto start = std::chrono::steady_clock::now();
      std::chrono::steady_clock::time_point seen{};
      const auto dir = rex::filesystem::GetExecutableFolder() / "native_frames";
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      for (size_t k = 0; k < times.size(); ++k) {
        const int t = times[k];
        if (relative[k]) {
          while (seen == std::chrono::steady_clock::time_point{}) {
            if (ng2native::WatchedTextureSeen()) seen = std::chrono::steady_clock::now();
            else std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
          std::this_thread::sleep_until(seen + std::chrono::seconds(t));
        } else {
          std::this_thread::sleep_until(start + std::chrono::seconds(t));
        }
        const std::string name = (relative[k] ? "cap_rel" : "cap_") + std::to_string(t);
        // NG2_DUMP_MEM="addr:size" (hex, physical): that guest memory too (the
        // native renderer keeps its resolves on the GPU: what the CPU sees)
        if (const char* dm = std::getenv("NG2_DUMP_MEM"); dm && *dm) {
          const uint32_t addr = uint32_t(std::strtoul(dm, nullptr, 16));
          const char* colon = std::strchr(dm, ':');
          const uint32_t size = colon ? uint32_t(std::strtoul(colon + 1, nullptr, 16)) : 0;
          auto* memory = REX_KERNEL_MEMORY();
          if (memory && size && size_t(addr) + size <= 0x20000000u) {
            std::ofstream mf(dir / fmt::format("mem_{}_{:08X}.bin", name, addr), std::ios::binary);
            mf.write(memory->TranslatePhysical<const char*>(addr), std::streamsize(size));
          }
        }
        // (NG2_CAPTURE_PRESENTER=1: the presenter's picture even with the native
        // renderer - what reaches the screen, after the swap's gamma pass)
        static const bool presenter = std::getenv("NG2_CAPTURE_PRESENTER") != nullptr;
        if (ng2native::RequestNamedDump(name.c_str()) && !presenter) {
          REXLOG_INFO("[capture] {} s: the next native frame -> native_frames/{}.rgba", t, name);
          continue;
        }
        auto* rt = runtime();
        auto* gs = rt ? rt->graphics_system() : nullptr;
        rex::ui::RawImage image;
        if (!gs || !gs->presenter() || !gs->presenter()->CaptureGuestOutput(image)) {
          REXLOG_WARN("[capture] {} s: no frame to capture", t);
          continue;
        }
        std::vector<uint8_t> rgba(size_t(image.width) * image.height * 4);
        for (uint32_t y = 0; y < image.height; ++y) {
          for (uint32_t x = 0; x < image.width; ++x) {
            const uint8_t* src = image.data.data() + y * image.stride + x * 4;
            uint8_t* dst = rgba.data() + (size_t(y) * image.width + x) * 4;
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = 255;
          }
        }
        std::ofstream f(dir / fmt::format("{}_{}x{}.rgba", name, image.width, image.height), std::ios::binary);
        f.write(reinterpret_cast<const char*>(rgba.data()), std::streamsize(rgba.size()));
        REXLOG_INFO("[capture] {} s: presenter frame {}x{} saved", t, image.width, image.height);
      }
    }).detach();
  }
#endif

  // The game's own "Quit Game" leaves a black screen, and this is why.
  //
  // That menu entry calls XamLoaderTerminateTitle, which reaches
  // KernelState::TerminateTitle. That marks every guest thread, wakes the ones
  // blocked in a kernel wait, gives them 200ms to exit, and then DELIBERATELY
  // LEAVES THE STRAGGLERS RUNNING - force-killing a thread orphans whatever
  // host lock it holds.
  //
  // NG2's tasks are fibers, so its main guest thread spends essentially all of
  // its time inside SwitchToFiber rather than in a kernel wait. It never
  // reaches XThread::CheckTitleTermination, so it is always a straggler. The
  // SDK quits the app from a thread that Wait()s on exactly that thread
  // (ReXApp::LaunchModule), so the wait never returns, the app never quits,
  // and the window sits there with nothing left to draw into it.
  //
  // That also means OnGuestThreadExit could never have helped: it is called
  // from that same wait, for the main thread only, and the main thread is the
  // one that does not exit.
  //
  // What IS reliable is that TerminateTitle erases every guest thread from the
  // kernel's map whether or not it actually stopped. So the main thread's id
  // going missing is a stable, permanent signal that the title has terminated
  // - not a race against the 200ms drain, and not dependent on the
  // terminating_title_ flag, which TerminateTitle clears again before the
  // calling thread exits.
  void StartTitleWatcher() {
    if (main_guest_thread_id_ == 0)
      return;
    title_watcher_ = std::thread([this] {
      // The thread has to be SEEN in the kernel's map before its absence means
      // anything. This hook runs before main_thread->Resume(), and treating
      // "not there yet" as "the title ended" would relaunch the app in a loop
      // that only a task manager could stop - the single worst way this could
      // fail, so it is not left to timing.
      bool seen = false;
      while (!title_watcher_stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        auto* kernel = REX_KERNEL_STATE();
        if (!kernel)
          continue;
        if (kernel->GetThreadByID(main_guest_thread_id_)) {
          seen = true;
          continue;
        }
        if (!seen)
          continue;

        // Closing the window ALSO terminates the title, and this must not turn
        // that into a relaunch. ReXApp::OnClosing calls TerminateTitle and then
        // _Exit(0) a few milliseconds later, so a poll landing inside that gap
        // would see exactly what a guest quit looks like - about a 2% chance per
        // close at this interval, which is far too likely for "closing the app
        // reopened it".
        //
        // Settling for longer than that gap is what tells them apart: on any
        // host quit path the process is gone before this returns, and on a
        // guest quit nothing else is happening and the state is permanent.
        std::this_thread::sleep_for(std::chrono::milliseconds(750));
        if (title_watcher_stop_.load(std::memory_order_acquire) || shutting_down_)
          return;
        REXLOG_INFO("The guest terminated the title - returning to the menu");
        ReturnToMenu();
        return;
      }
    });
  }

  // Detaches rather than joins. The watcher can be inside its settle sleep
  // while a host quit runs, and joining would block the quit for up to a
  // second; the stop flag is checked after that sleep, so a detached watcher
  // that wakes into a shutdown does nothing and exits.
  void StopTitleWatcher() {
    title_watcher_stop_.store(true, std::memory_order_release);
    if (title_watcher_.joinable())
      title_watcher_.detach();
  }

  // Quitting the game (its own Quit Game, or the in-game menu's Return to
  // the hub) puts the player back on the hub.
  //
  // It gets there by relaunching rather than by tearing the runtime down and
  // rebuilding it: the guest's stragglers are still running inside a runtime
  // whose memory would be going away underneath them, which is the same fault
  // the window-close path avoids by hard-exiting. A fresh process has none of
  // that, and it is fast enough that it reads as a screen change. (The hub
  // opens at every launch; --ng2_setup is the PC port's flag for its setup
  // screen, still passed so the relaunch reads as one in the log.)
  void ReturnToMenu() {
    if (shutting_down_)
      return;
    ShutdownCleanlyForRelaunch();
#if defined(__PROSPERO__)
    // [ps5 port] The app reloaded by the system, back into its hub.
    const std::filesystem::path exe = "/app0/eboot.bin";
#else
    // [IzunaRecomp] This very executable (ng2.exe, or ng2-compat.exe on an
    // older CPU), not the name.
    const std::filesystem::path exe = rex::filesystem::GetExecutablePath();
#endif
    std::error_code ec;
    if (std::filesystem::exists(exe, ec)) {
      ng2::LaunchDetached(exe, "--ng2_setup");
    } else {
      REXLOG_WARN("Could not find {} to relaunch; quitting instead", exe.string());
    }
    std::fflush(nullptr);
    rex::FlushLogging();
    ng2::ExitNow(0);  // [ps5 port] _Exit, or the PS5 shell's close
  }

  // ShutdownCleanly without the exit watchdog: the caller exits by itself and
  // a 3s _Exit racing a relaunch would be a second process starting while this
  // one is still holding the window.
  void ShutdownCleanlyForRelaunch() {
    if (shutting_down_)
      return;
    shutting_down_ = true;
    settings_.Clamp();
    settings_.Save();
    REXLOG_INFO("Shutdown for relaunch: settings saved");
  }

  // Called when the window's close button is pressed, before the SDK's own
  // close handling terminates the title. Marks the watcher off so the
  // termination that is about to happen is not mistaken for a guest quit.
  bool OnWindowCloseRequested() override {
    title_watcher_stop_.store(true, std::memory_order_release);
    return true;
  }

  void OnShutdown() override {
    ShutdownCleanly();

    // Dialogs hold a raw pointer to the drawer, which the SDK tears down after
    // this hook: drop them now.
    StopUiPump();
    ps5_overlay_.reset();
    shader_overlay_.reset();
    script_overlay_.reset();
    perf_hud_.reset();

    // Every bind this port registered: a bind left behind holds a lambda
    // capturing `this`.
    for (const char* bind : {"bind_ng2_hud", "bind_ng2_quit", "bind_ng2_fullscreen", "bind_ng2_settings"})
      rex::ui::UnregisterBind(bind);
    REXLOG_INFO("Shutdown: settings saved, dialogs released");
  }

  // PC display settings.
  //
  // The host window and the guest video mode are separate: window_width/height
  // size the desktop window, while video_mode_* is what the title is told the
  // display is, which is what actually changes the rendered resolution and the
  // rate it targets. Passing these on the command line does nothing - the
  // launcher's parser does not reach these cvars - so they are set here, and
  // read from the environment so they stay configurable without a rebuild.
  //
  //   NG2_WIDTH / NG2_HEIGHT        window size (default 1280x720)
  //   NG2_FULLSCREEN=1              borderless fullscreen
  //   NG2_FPS                       guest refresh rate, 30-60 (default 60). A
  //                                 higher value is clamped to 60 by Clamp()
  //                                 below - above 60 this title runs fast, not
  //                                 smooth, so there is no test lever for it.
  //   NG2_RENDER_WIDTH / _HEIGHT    guest video mode, defaults to window size
  static int EnvInt(const char* name, int fallback) {
    const char* e = std::getenv(name);
    if (!e || !*e) return fallback;
    const int v = std::atoi(e);
    return v > 0 ? v : fallback;
  }

  // Environment overrides win over the saved file, for scripted testing. Read
  // once, in OnConfigurePaths, so every later consumer sees the same values.
  void ApplyEnvironmentOverrides() {
    settings_.window_width = EnvInt("NG2_WIDTH", settings_.window_width);
    settings_.window_height = EnvInt("NG2_HEIGHT", settings_.window_height);
    settings_.fps = EnvInt("NG2_FPS", settings_.fps);
    settings_.resolution_scale = EnvInt("NG2_SCALE", settings_.resolution_scale);
    if (std::getenv("NG2_FULLSCREEN"))
      settings_.fullscreen = EnvInt("NG2_FULLSCREEN", 0) != 0;
    if (const char* game = std::getenv("NG2_GAME"); game && *game)
      settings_.game_path = game;
    if (const char* dlc = std::getenv("NG2_DLC"); dlc && *dlc)
      settings_.dlc_path = dlc;
    // The same import the hub queues, without the file dialog (a single save
    // or a folder of them), so a save import can be scripted.
    if (const char* save = std::getenv("NG2_IMPORT_SAVE"); save && *save)
      ng2::QueueSaveImports(ng2::FindSavePackages(save));
  }

  // The three settings the window itself is built from. They have to be in
  // place before SetupPresentation creates it, which is BEFORE OnPreSetup - so
  // this runs from OnConfigurePaths, inside SetupEnvironment.
  //
  // Setting them in OnPreSetup looked like it worked, because the window ends
  // up the right size anyway once the guest sets its video mode. Fullscreen
  // has no such second chance: saved as on, it came back windowed every time,
  // and the only reason that was caught was photographing the window and
  // finding the capture still 1602x939 instead of the full screen.
  // A window bigger than the monitor is a window with its bottom edge off the
  // screen (4K on a 1440p display read as "the menu went full screen and some
  // options are gone", when they were simply below the desk).
  //
  // Only the WINDOW is clamped. The internal render scale is a different
  // setting and is what supersampling uses, so asking for 3x on a 1440p screen
  // still renders at 3x - the picture just arrives somewhere it can be seen.
  // The window the player asked for, in the units the runtime actually wants.
  //
  // window_width/height in the settings mean PHYSICAL pixels, because that is
  // what "4K" means to a person. The runtime's cvars mean LOGICAL pixels, which
  // Windows then multiplies by the display's scaling. On a 150% 4K screen those
  // differ by half again: filling it needs 2560 logical, and asking for 3840
  // produces a 5760-pixel window that cannot fit on any monitor here.
  //
  // That was the whole of "the app will not set up 4K". An earlier version of
  // this function made it worse rather than better - it clamped a logical
  // request against a physical work area, so a window that "fitted" was then
  // scaled up past the edge of the screen anyway.
  //
  // So: clamp in physical against the monitor this window will open on, then
  // convert once, on the way out.
  void ApplyWindowSizeForMonitor() {
    int screen_w = 0, screen_h = 0;
    float scale = 1.0f;
    if (!ng2::MonitorWorkArea(settings_.monitor, screen_w, screen_h, scale)) {
      // Could not tell; do not invent a limit or a scale.
      REXCVAR_SET(window_width, settings_.window_width);
      REXCVAR_SET(window_height, settings_.window_height);
      return;
    }
    // Clamp against the monitor it will open on, but convert with the scale
    // Windows will actually apply - which is the PRIMARY display's, not this
    // one's. Measured: with the 4K monitor's own 150% the window came out
    // 3218 pixels wide on a 3840 screen, because the conversion assumed a
    // scaling that was never applied.
    const float applied = ng2::SystemScale();
    const int physical_w = std::min(settings_.window_width, screen_w);
    const int physical_h = std::min(settings_.window_height, screen_h);
    const int logical_w = int(float(physical_w) / applied + 0.5f);
    const int logical_h = int(float(physical_h) / applied + 0.5f);

    REXLOG_INFO("Display: monitor {} is {}x{} at {:.0f}%; windows are scaled "
                "{:.0f}%, so {}x{} physical is {}x{} logical",
                settings_.monitor + 1, screen_w, screen_h, scale * 100.0f,
                applied * 100.0f, physical_w, physical_h, logical_w, logical_h);

    REXCVAR_SET(window_width, logical_w);
    REXCVAR_SET(window_height, logical_h);
  }

  void ApplyWindowSettings() {
    // The runtime has always had this and nothing drove it, so the window went
    // wherever Windows last put it - which on three screens is not necessarily
    // the one the size was chosen for.
    // The runtime's display numbers are ONE-based, with 0 meaning "wherever
    // Windows likes". Tested: 1 was the primary, 2 and 3 the other two, and 4
    // fell back to the primary. The setting is a 0-based index into the list
    // the menu shows, so it is shifted here rather than in the menu.
    rex::cvar::SetFlagByName("monitor",
                             std::to_string(settings_.monitor + 1));
    ApplyWindowSizeForMonitor();
    REXCVAR_SET(fullscreen, settings_.fullscreen);
  }

  // [IzunaRecomp] A PC's Render scale (pc_render_scale, Auto by the screen):
  // 1x, 1.5x (1920x1080 - a true 1080p), 2x (1440p) or 3x (4K). Auto takes
  // the smallest that covers the screen's height: 1.5x for 1080p, 2x for
  // 1440p, 3x for 4K (test11 drew 2x for a 1080p screen: 78% more pixels than
  // the screen has, and its GPU-heavy scenes missed refreshes).
  void ResolvePcRenderScale() {
#if !defined(__PROSPERO__)
    // The 16:9 resolutions offered, by height, and their widths: the scale is
    // width / 1280 (1366 x 768: 1.0671875, the height 768.4 rounding to 768).
    static const int kSizes[][2] = {{720, 1280},  {768, 1366},  {900, 1600}, {1080, 1920},
                                    {1440, 2560}, {1800, 3200}, {2160, 3840}};
    int width = 0;
    for (const auto& s : kSizes)
      if (s[0] == settings_.pc_resolution) width = s[1];
    int sw = 0, sh = 0;
    const bool screen = izuna::pc::ScreenSize(&sw, &sh);
    if (!width) {
      // Auto: the largest 16:9 picture that fits the screen (fullscreen) or the
      // window, drawn at exactly that size - a 1920x1200 screen gets 1920x1080
      // with bars, a 3440x1440 one 2560x1440, each shown 1:1.
      const int aw = settings_.fullscreen && screen ? sw : settings_.window_width;
      const int ah = settings_.fullscreen && screen ? sh : settings_.window_height;
      // A listed size that fits wins (1366x768 is not exactly 16:9: the exact
      // fit of that screen is 1365x767); else the exact fit (1440x900: 1440x810).
      width = int(std::floor(std::min(aw / 1280.0, ah / 720.0) * 1280.0 + 1e-6));
      for (const auto& s : kSizes)
        if (s[1] <= aw && s[0] <= ah && s[1] >= width - 2) width = s[1];
    }
    width = std::clamp(width, 1280, 3840);
    const float k = float(width / 1280.0);
    if (settings_.pc_scale_resolved != k)
      REXLOG_INFO("Resolution: {}x{} (scale {:.4f}; {}; screen {}x{}, {})", width, int(std::floor(720.0 * k + 1e-6)),
                  k, settings_.pc_resolution ? "chosen" : "Auto", sw, sh,
                  settings_.fullscreen ? "fullscreen" : "windowed");
    settings_.pc_scale_resolved = k;
    settings_.resolution_scale = int(std::ceil(k));  // (the whole-number readers: the display log)
#endif
  }

  void ApplyDisplaySettings() {
    ResolvePcRenderScale();
#if defined(__PROSPERO__)
    ApplyPs5Display();  // [ps5 port] no window to fit: the resolution presets
    return;
#endif
    // The guest is told a 16:9 display, whatever shape the window is.
    //
    // It used to be told the window size. Ninja Gaiden II renders 16:9 (the
    // internal render size below) and the presenter pillarboxes only when the
    // guest's display aspect differs from the window's - so a 3840x1600 window
    // made the guest report a 2.4:1 display, the 16:9 frame "matched" it, and
    // Keep aspect ratio never got a say: the whole game stretched. It went
    // unseen for as long as every window was 1280x720.
    //
    // The largest 16:9 box that fits the window: a 16:9 window is told its own
    // size as before, an ultrawide is told a display of its own height.
    int guest_w = settings_.window_width;
    int guest_h = settings_.window_height;
    // (Reporting a wide display instead only stretched the 2D and NG2's
    // fixed-16:9 3D.)
    if (guest_w * 9 > guest_h * 16) {
      guest_w = guest_h * 16 / 9;
    } else if (guest_w * 9 < guest_h * 16) {
      guest_h = guest_w * 9 / 16;
    }
    guest_w &= ~1;
    guest_h &= ~1;
    REXCVAR_SET(video_mode_width, guest_w);
    REXCVAR_SET(video_mode_height, guest_h);
    // The runtime treats a video mode equal to its default (1280x720) as unset
    // and substitutes the window size - the stretch again for any window that
    // is not itself 1280x720, and on a 125% display even that one is told
    // 1024x576, which is not high definition. This asks it to take the value
    // as set. By name, so a runtime without the option simply ignores it.
    if (rex::cvar::GetFlagInfo("video_mode_explicit"))
      rex::cvar::SetFlagByName("video_mode_explicit", "true");

    // The title paces itself off the reported display refresh rate, so this is
    // the frame-rate control - it sets game SPEED as much as smoothness. Clamped
    // to 30-60: above 60 the game runs fast rather than smooth, which is a crash
    // a player actually hit at 144, so the menu no longer offers it and Clamp()
    // brings an old settings file down to 60.
    REXCVAR_SET(video_mode_refresh_rate, double(settings_.fps));

    // The internal render size patch. These are our own cvars, read by the
    // midasm hooks in patch_hooks.cpp; 0 means "leave the game's 1120x584
    // alone", which is why they are only written when the patch is on.
    if (settings_.internal_720p) {
      REXCVAR_SET(ng2_render_width, 1280);
      REXCVAR_SET(ng2_render_height, 720);
    }
    REXCVAR_SET(ng2_video_mode, settings_.video_mode);

    // vsync, the render scale and the rest of the renderer's settings are
    // applied by ApplyTuning() through the config loader (ng2_tuning.h).

    REXLOG_INFO(
        "Display: window {}x{}, guest display {}x{}, {}x internal scale, {} Hz, "
        "fullscreen={}, vsync={}",
        settings_.window_width, settings_.window_height,
        REXCVAR_GET(video_mode_width), REXCVAR_GET(video_mode_height),
        settings_.resolution_scale, settings_.fps, settings_.fullscreen,
        settings_.vsync);
  }

  // Install downloadable content.
  //
  // NG2's DLC ships as LIVE-signed STFS packages under
  //   DLC/<title id>/<content type>/<40-hex filename>
  // The SDK's ContentManager::InstallContent extracts a package into its own
  // content root and writes the .header file XAM enumerates, which is what
  // makes the extra characters and Mission Mode visible to the game's menus.
  //
  // Packages are matched by their STFS magic rather than by extension - the
  // files have no extension at all. Installing is idempotent, so re-running is
  // harmless; a package that fails is logged and skipped rather than aborting
  // the boot.
  void InstallDlc() {
    auto* kernel = REX_KERNEL_STATE();
    if (!kernel) return;
    auto* content = kernel->content_manager();
    if (!content) return;

    // Whatever the settings menu chose first, then the two conventional
    // spellings beside the executable - the disc rip and its DLC usually live
    // together.
    std::vector<std::filesystem::path> roots{
        settings_.ResolvedDlcPath(),
        rex::filesystem::GetExecutableFolder() / "dlc",
        rex::filesystem::GetExecutableFolder() / "DLC",
    };
    std::error_code ec;
    for (const auto& root : roots) {
      if (!std::filesystem::is_directory(root, ec)) continue;
      std::vector<std::filesystem::path> packages;
      for (auto& e : std::filesystem::recursive_directory_iterator(root, ec))
        if (ng2::IsStfsPackage(e.path())) packages.push_back(e.path());
      std::sort(packages.begin(), packages.end());
      REXLOG_INFO("DLC: {} package(s) under {}", packages.size(),
                  root.string());
      for (const auto& pkg : packages) {
        const auto result = content->InstallContent(pkg);
        if (result == 0)
          REXLOG_INFO("DLC: installed {}", pkg.filename().string());
        else
          REXLOG_WARN("DLC: failed to install {} (0x{:08X})",
                      pkg.filename().string(), uint32_t(result));
      }
      return;  // first directory that exists wins
    }
    REXLOG_INFO("DLC: no dlc/ folder beside the executable; skipping");
  }

  // Every controller drives guest user 0.
  //
  // The runtime's default is "device ordinal N feeds guest user N", so a pad
  // that does not enumerate first lands on guest user 1 - and Ninja Gaiden II
  // is single-player: it polls user 0 and nothing else. That one policy
  // produced both of the symptoms that looked unrelated: a pad that moved
  // nothing in game, and a sign-in prompt that could not be satisfied, because
  // the profile the game was asking about belonged to a user index that does
  // not exist.
  //
  // SharedAssignment is the SDK's own answer for this case - "every device
  // feeds guest user 0 ... for single-player titles that only ever poll user
  // 0" - and it must be in place before the guest starts polling.
  void UseOneGuestUser() {
    auto* runtime_ptr = runtime();
    if (runtime_ptr == nullptr)
      return;
    // SetDeviceAssignment is on the concrete input system, not the two-method
    // interface the runtime hands back, so this asks rather than assumes: a
    // different backend simply keeps the default instead of crashing.
    auto* input =
        dynamic_cast<rex::input::InputSystem*>(runtime_ptr->input_system());
    if (input == nullptr) {
      REXLOG_WARN("Input: not the SDL input system; controllers stay on the "
                  "default per-slot assignment");
      return;
    }
    input->SetDeviceAssignment(std::make_unique<rex::input::SharedAssignment>());

    // The synthetic pad that dismisses chapter-opening cinematics. Added after
    // the assignment so SharedAssignment picks it up with everything else; it
    // reports nothing at all unless a chapter load has armed it.
    // [ps5 port] Never armed here: "Skip chapter cinematics" is gone from the
    // hub (the owner removed it), and a settings file that still has it on
    // must not bring it back. The driver stays for NG2_PAD_SCRIPT testing.
    ng2::SetAutoSkipEnabled(false);
    input->AddDriver(ng2::MakeAutoSkipDriver());
    REXLOG_INFO("Input: every controller drives guest user 0");
  }

  // Keystrokes only reach the game while one of our windows has focus.
  //
  // Checked by process rather than window handle - the SDK does not expose the
  // HWND, and "is the foreground window one of ours" answers the same question.
  //
  // This callback also used to hold input back while a replacement video was
  // drawn over the game. That mode is gone: the game plays its own videos now,
  // so it is the one taking the skip press, which is what should happen.
  void GateInputToForeground() {
    auto* runtime_ptr = runtime();
    auto* input = runtime_ptr ? dynamic_cast<rex::input::InputSystem*>(
                                    runtime_ptr->input_system())
                              : nullptr;
    if (input == nullptr)
      return;
    input->SetActiveCallback([] {
      // (The game is held while the in-game menu is open.)
      if (ng2ps5::overlay::Overlay::IsOpen())
        return false;
      return ng2::ThisProcessIsForeground();
    });
    REXLOG_INFO("Input: held while the in-game menu is open or we are not the foreground window");
  }

  // A save chosen on the hub.
  //
  // It cannot be imported there: the hub runs before the runtime is built,
  // so there is no ContentManager and no signed-in profile to import into. It
  // is queued instead and carried out here, which is after both exist and
  // still before the guest starts looking for saves.
  void ImportQueuedSaves() {
    const auto packages = ng2::TakeQueuedSaveImports();
    if (packages.empty()) return;
    size_t done = 0;
    for (const auto& package : packages) {
      std::string message;
      if (ng2::ImportSave(package, message)) {
        ++done;
        REXLOG_INFO("Save: imported {} - {}", package.filename().string(),
                    message);
      } else {
        // One bad file does not stop the rest: a folder of saves is exactly
        // where a stray or corrupt one turns up.
        REXLOG_WARN("Save: could not import {} - {}",
                    package.filename().string(), message);
      }
    }
    REXLOG_INFO("Save: {} of {} imported", done, packages.size());
  }

  // The renderer's, audio's and input's settings, plus the title-specific
  // correctness settings, applied through the cvar config loader so a value
  // waits for a cvar registered later. ng2_tuning.h documents each one.
  void ApplyTuning() {
#if defined(__PROSPERO__)
    // [ps5 port] Derived every launch, so with the cache rather than beside
    // the (read-mostly) package files.
    {
      auto entries = Ng2Tuning::Fixed();
      for (auto& e : Ng2Tuning::FromSettings(settings_, std::string()))
        entries.push_back(std::move(e));
      Ng2Tuning::Apply(ng2ps5::paths::Cache() / "ng2_tuning.toml", entries);
      return;
    }
#endif
    ResolvePcRenderScale();
    auto entries = Ng2Tuning::Fixed();
    const auto mappings =
        rex::filesystem::GetExecutableFolder() / "gamecontrollerdb.txt";
    std::error_code ec;
    const bool have_mappings = std::filesystem::exists(mappings, ec);
    for (auto& e : Ng2Tuning::FromSettings(
             settings_, have_mappings ? mappings.string() : std::string())) {
      entries.push_back(std::move(e));
    }
    Ng2Tuning::Apply(
#if defined(IZUNA_LINUX_DESKTOP)
        ng2ps5::paths::Cache() / "ng2_tuning.toml",
#else
        rex::filesystem::GetExecutableFolder() / "cache" / "ng2_tuning.toml",
#endif
        entries);
  }

  // NG2_DUMP_CVARS=<path> writes every registered cvar - name, category, type,
  // current value, default, and any declared allowed values - once the GPU
  // plugin is up and its own cvars exist.
  //
  // This is the only honest way to know what a build can actually do. The
  // plugin does not export accessors for its flags, so they cannot be listed
  // from outside; guessing from the SDK headers is how the settings menu ended
  // up offering FSR and CAS, which this presenter does not implement.
  static void DumpCvars(const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
      REXLOG_WARN("Cvar dump: cannot write {}", path.string());
      return;
    }
    auto names = rex::cvar::ListFlags();
    std::sort(names.begin(), names.end());
    out << "# " << names.size() << " registered cvars\n";
    for (const auto& name : names) {
      const auto* info = rex::cvar::GetFlagInfo(name);
      if (info == nullptr)
        continue;
      out << name << "\n  category  " << info->category
          << "\n  value     " << rex::cvar::GetFlagByName(name)
          << "\n  default   " << info->default_value << "\n";
      if (!info->constraints.allowed_values.empty()) {
        out << "  allowed  ";
        for (const auto& v : info->constraints.allowed_values)
          out << " " << v;
        out << "\n";
      }
      if (info->constraints.HasRangeConstraint()) {
        out << "  range     "
            << (info->constraints.min ? std::to_string(*info->constraints.min) : "-")
            << " .. "
            << (info->constraints.max ? std::to_string(*info->constraints.max) : "-")
            << "\n";
      }
    }
    REXLOG_INFO("Cvar dump: {} cvars -> {}", names.size(), path.string());
  }

  // Logging is up by here.
  void OnPostInitLogging() override {
    // [IzunaRecomp] Windows: crash minidumps in logs\, power throttling off,
    // 1 ms timers (izuna_pc.h). Nothing elsewhere.
    izuna::pc::InstallCrashDumps(rex::filesystem::GetExecutableFolder() / "logs");
    izuna::pc::EnsureSingleInstance();
    izuna::pc::TuneProcess();
  }

  // The same bundle the settings button writes, from the environment.
  //
  // The button is in a menu, and the reports worth having most are from people
  // whose game never reaches one. This runs during startup instead, so a
  // launch that dies later still leaves something to send.
  void MaybeWriteDiagnostics() {
    const char* want = std::getenv("NG2_DIAGNOSTICS");
    if (!want || !*want || *want == '0')
      return;
    const auto result = ng2::WriteDiagnostics(
        Ng2Settings::Path(), rex::filesystem::GetExecutableFolder() / "logs");
    if (result.ok)
      REXLOG_INFO("Diagnostics: NG2_DIAGNOSTICS wrote {}", result.file.string());
    else
      REXLOG_ERROR("Diagnostics: {}", result.error);
  }

  void OnPostSetup() override {
    MaybeWriteDiagnostics();
    ArmQuitSeam();
#if defined(__PROSPERO__)
    // [ps5 port] The texture hook: the PlayStation button pictures and the
    // dump. Before LaunchModule, so the first texture the game loads goes
    // through it. Then the overlay gets the controller.
    {
      ng2ps5::prompts::Init(ng2ps5::paths::App());
      ng2ps5::textures::Config tc;
      tc.dump = settings_.texture_dump;
      tc.dump_dir = ng2ps5::paths::TexturePacks() / "PS5 Dump" / "dump";
      tc.prompts = settings_.ps5_playstation_prompts;
      tc.dump_hud_pictures = settings_.ps5_dump_hud_pictures;
      ng2ps5::textures::Configure(tc);
      if (ps5_overlay_)
        ps5_overlay_->SetEnabled(true);
    }
#else
    // [IzunaRecomp] Windows: the PlayStation button pictures as on the PS5
    // (the texture hook, before the game loads its first texture) - Auto
    // takes them when the hub last saw a PlayStation controller.
    {
      const bool playstation =
          settings_.pc_prompts == 2 ||
          (settings_.pc_prompts == 0 && ng2ps5::LastPromptStyle() == ng2ps5::PromptStyle::kPlayStation);
      ng2ps5::prompts::Init(ng2ps5::paths::App());
      ng2ps5::textures::Config tc;
      tc.prompts = playstation;
      ng2ps5::textures::Configure(tc);
      REXLOG_INFO("Button prompts: {}", playstation ? "PlayStation" : "the game's own");
    }
    // [IzunaRecomp] Windows: the in-game menu, from the game's start.
    if (ps5_overlay_)
      ps5_overlay_->SetEnabled(true);
#endif

    if (const char* dump = std::getenv("NG2_DUMP_CVARS"); dump && *dump)
      DumpCvars(dump);

    // Read back what the renderer took: the check that the deferred config
    // reached it.
    REXLOG_INFO("Kasumi renderer: internal scale {}x, anti-aliasing {}",
                rex::cvar::GetFlagByName("native_resolution_scale"),
                rex::cvar::GetFlagByName("native_antialias"));
    ng2::ApplySound(settings_);  // [IzunaRecomp]
    // The window exists by now, so the comfort settings can go straight on it.
    if (auto* w = window()) {
      // Auto-hide only takes effect in the kAutoHidden visibility mode, so arm
      // the mode here too, not just the delay (see ApplyLiveSettings).
      if (settings_.cursor_hide_seconds > 0) {
        w->SetCursorAutoHideDelayMs(
            static_cast<uint32_t>(settings_.cursor_hide_seconds) * 1000u);
        w->SetCursorVisibility(rex::ui::Window::CursorVisibility::kAutoHidden);
      } else {
        w->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
      }
    }

    InstallDlc();
    ImportQueuedSaves();
    // [IzunaRecomp] The enabled mods' files over the game's (the executable
    // is in memory by now and the game has not run).
    izuna::mods::Activate(game_data_root_);

    UseOneGuestUser();
    GateInputToForeground();

    if (auto* memory = REX_KERNEL_MEMORY()) {
      AllocateSkipPath(memory);
      CommitLowGuestPages(memory);
    }
  }

 private:
  // Commit the low guest pages as readable zeroes.
  //
  // The game keeps 160 slot records at 0x84C23C70 whose data pointers are null
  // until something populates them, and 275+ sites still index off them as
  // `buffer[idTable[type]]`. On hardware those reads land in low memory and
  // come back zero, which every consumer treats as "nothing here"; here the
  // pages are uncommitted, so the same reads fault at guest 0x59BF / 0x4F60.
  //
  // Page 0 is included (the game's null-based reads land there too); the
  // range is mapped read-only, so a stray *write* through a null pointer is
  // still caught.
  void CommitLowGuestPages(rex::memory::Memory* memory) {
    constexpr uint32_t kLowBase = 0x00000000;
    constexpr uint32_t kLowSize = 0x00040000;  // through 0x3FFFF

    auto* heap = memory->LookupHeap(kLowBase);
    if (!heap) {
      REXLOG_WARN("No heap covers guest 0x{:08X}; low-memory reads will fault",
                  kLowBase);
      return;
    }
    // Commit read+write first: Zero() writes through the guest mapping, so a
    // read-only range faults on our own memset before the game ever runs.
    const bool ok = heap->AllocFixed(
        kLowBase, kLowSize, 0,
        rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
    if (ok) {
      memory->Zero(kLowBase, kLowSize);
      uint32_t old_protect = 0;
      heap->Protect(kLowBase, kLowSize, rex::memory::kMemoryProtectRead,
                    &old_protect);
      REXLOG_INFO("Committed guest 0x{:08X}-0x{:08X} as read-only zeroes",
                  kLowBase, kLowBase + kLowSize - 1);
    } else {
      REXLOG_WARN("AllocFixed failed for guest 0x{:08X}+0x{:X}", kLowBase,
                  kLowSize);
    }
  }

  // One guest page holding a path that will never resolve, for the skip-videos
  // patch to point the file wrapper at. Allocated rather than borrowed from
  // some existing string: a real string in .rdata could one day name a real
  // file, and this has to be guaranteed to fail.
  void AllocateSkipPath(rex::memory::Memory* memory) {
    constexpr uint32_t kBase = 0x00040000;  // just above the low-memory commit
    constexpr uint32_t kSize = 0x1000;
    auto* heap = memory->LookupHeap(kBase);
    if (!heap)
      return;
    if (!heap->AllocFixed(kBase, kSize, 0,
                          rex::memory::kMemoryAllocationReserve |
                              rex::memory::kMemoryAllocationCommit,
                          rex::memory::kMemoryProtectRead |
                              rex::memory::kMemoryProtectWrite)) {
      REXLOG_WARN("Skip-videos: could not allocate the placeholder path");
      return;
    }
    memory->Zero(kBase, kSize);
    static constexpr char kPath[] = "game:\\__video_skipped__.wmv";  // [ps5 port] the mirror lost the backslash
    auto* dest = memory->TranslateVirtual<char*>(kBase);
    if (dest == nullptr)
      return;
    std::memcpy(dest, kPath, sizeof(kPath));
    g_ng2_skip_path_va = kBase;
    REXLOG_INFO("Skip-videos: placeholder path at guest 0x{:08X}", kBase);
  }

  // Repaint the UI while the hub (or the startup shader build's panel) is up.
  //
  // Nothing else is driving the presenter at this point: the guest has not
  // been created, so no frame is ever presented, and the SDK only paints the
  // overlay when something asks it to. Without this the hub paints once and
  // then freezes - it looks fine in a screenshot and is completely
  // dead to the mouse, because ImGui only consumes its queued input inside a
  // draw. re:Blue needs the same pump for the same reason.
  //
  // The ticks have to come from a second thread: a UI-thread tick that
  // re-enqueues itself never lets the pending-function queue drain, which
  // starves SDL's own event handling and kills the window's close button.
  void StartUiPump() {
    if (ui_pump_thread_.joinable())
      return;
    ui_pump_stop_.store(false, std::memory_order_release);
    ui_pump_pending_.store(false, std::memory_order_release);
    ui_pump_thread_ = std::thread([this] {
      while (!ui_pump_stop_.load(std::memory_order_acquire)) {
        // One repaint in flight at a time. A modal file dialog blocks the UI
        // thread for as long as it is open, and without this the whole time
        // would come back as a queue full of stale repaint requests.
        if (!ui_pump_pending_.exchange(true, std::memory_order_acq_rel)) {
          app_context().CallInUIThreadDeferred([this] {
            if (auto* w = window())
              w->RequestPresenterUIPaintFromUIThread();
            ui_pump_pending_.store(false, std::memory_order_release);
          });
        }
        // [IzunaRecomp] The next paint as soon as the last is done: V-Sync
        // paces them, at the screen's rate (a fixed 16 ms sleep between
        // requests painted the hub at an uneven 50-odd frames a second).
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }

  void StopUiPump() {
    ui_pump_stop_.store(true, std::memory_order_release);
    if (ui_pump_thread_.joinable())
      ui_pump_thread_.join();
  }

  std::thread ui_pump_thread_;
  std::atomic<bool> ui_pump_stop_{false};
  std::atomic<bool> ui_pump_pending_{false};
  std::atomic<bool> hub_open_{false};  // [IzunaRecomp] (Escape is the hub's Back while it is shown)

  // The dialogs. Each is owned here and closed by destroying it:
  // ImGuiDialog::Close() does `delete this`, so a dialog must never be both
  // unique_ptr-owned and Close()d.
  std::unique_ptr<izuna::scripts::Overlay> script_overlay_;
  izuna::shaders::Progress shader_progress_;  // (the startup shader build's count, for its panel)
  std::unique_ptr<izuna::shaders::ProgressOverlay> shader_overlay_;
  std::unique_ptr<ng2::PerfHudOverlay> perf_hud_;
  std::unique_ptr<ng2ps5::overlay::Overlay> ps5_overlay_;  // the in-game menu

  std::filesystem::path game_data_root_;  // (the game's folder: [replace] mods read its archives)
  bool shutting_down_ = false;

  // Watches for the guest terminating the title. See OnPostLaunchModule.
  std::thread title_watcher_;
  std::atomic<bool> title_watcher_stop_{false};
  uint32_t main_guest_thread_id_ = 0;
};
