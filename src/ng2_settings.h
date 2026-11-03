// Persistent PC settings for the NG2 recompilation.
//
// Stored as a flat key=value file next to the executable so it can be edited by
// hand or written by the settings menu. Everything here is applied before the
// guest launches; the fields marked "restart" only take effect on the next run
// because the window and the guest video mode are created during startup.
//
// This file owns the *player's* choices. Two other config files exist and are
// deliberately separate:
//
//   cache/ng2_tuning.toml  derived, rewritten every launch from these values
//                          plus the fixed correctness settings (ng2_tuning.h).
//                          Never edit it; it is the transport into the GPU
//                          plugin's cvars, not a place to keep anything.
//   ng2.toml               the SDK's own cvar config, written by the raw cvar
//                          browser on F4. Anything set there is applied before
//                          this file is even read.

#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include <rex/filesystem.h>
#include <rex/logging.h>

#include "ps5/ps5_paths.h"
#include "izuna/izuna_keys.h"

struct Ng2Settings {
  // --- Display (restart) -------------------------------------------------
  int window_width = 1280;
  int window_height = 720;
  bool fullscreen = false;

  // Which display to open on, 0 = the primary. The runtime has always had a
  // `monitor` cvar and nothing drove it, so the window went wherever Windows
  // put it - which on a two-screen setup is not where the size was chosen for.
  int monitor = 0;
  // 30..60, drives the guest video mode. The field still parses a higher number
  // from an old settings file so those files load, but Clamp() brings it down to
  // 60: this title paces its logic off the reported refresh rate, so above 60 it
  // runs fast rather than smooth, and a player hit a crash at 144.
  int fps = 60;
  bool vsync = true;

  // --- Graphics ----------------------------------------------------------
  // True internal render scale: the guest's own framebuffer is rendered at
  // this multiple and downsampled, i.e. supersampling rather than upscaling.
  // 1..6. The plugin accepts up to 7 and says so if asked for more; the menu
  // stopped at 3 for no reason anyone measured, which on a card that can
  // afford 4x is image quality left on the table.
  int resolution_scale = 1;

  // The Xenia community render-size patch: the title renders internally at
  // 1120x584 and scales up. Raising it to 1280x720 removes that scale.
  // Implemented as midasm hooks in patch_hooks.cpp, not a memory patch, since
  // the values are immediates the recompiler has already turned into C++
  // constants.
  bool internal_720p = false;

  // The community Chapter 12 crash workaround (Gliniak, via Xenia's patch file
  // for this title). Off by default: its author is explicit that it causes
  // problems outside that chapter, so it is a "turn on when you get stuck"
  // setting rather than something to leave enabled.
  bool chapter12_workaround = false;

  // How the pre-rendered videos are handled. 1, decoding them ourselves, is
  // simply what the port does now, so the menu does not ask: the alternatives
  // are the corrupted picture (0) and no picture at all (2). It stays here as
  // a way out for anyone who wants one, and a video with no prepared file
  // falls back to the game's own playback on its own.
  // 0 = the game decodes its own videos, which is what this port does now.
  //
  // The v0.5.0 codegen fix (shared_vector_registers) repaired the guest's
  // decoder, which is why v0.5.2 deleted the converted videos and the install
  // step that made them: playback is native and there is nothing to replace.
  //
  // This briefly defaulted to 1 on the strength of a run that black-screened at
  // the intro. That was misdiagnosed - the same failure appears with mode 1,
  // and its actual trigger is dumping and the texture pack running TOGETHER,
  // which puts a file write and a file stat on the GPU thread for every texture
  // the decoder creates and starves the ring buffer. Mode 0 plays the intro and
  // reaches the title screen normally.
  //
  // 0 lets the game play them, 2 skips them entirely. 1 used to replace the
  // picture from files converted at install time, which existed only to work
  // around the broken guest decoder; that decoder is fixed, so 1 is gone and
  // an old config carrying it is folded back to 0 by Apply().
  int video_mode = 0;

  // Textures dumped as the game loads them (the PS5's developer option; the
  // hub's texture hook, src/ps5/ps5_textures.cpp).
  bool texture_dump = false;

  // -1 = leave the game's own sampler settings alone, 0..4 = force 1x/2x/4x/
  // 8x/16x anisotropic filtering.
  int anisotropic = -1;

  // Post-process antialiasing, applied by the GPU plugin to the swap image.
  // "none", "fxaa" or "fxaa_extreme" - the list the `swap_post_effect` cvar
  // itself declares. This is the only real AA knob the runtime has; the
  // presenter's `present_effect` offers bilinear and nothing else, so there is
  // no upscaling filter to choose and the menu does not pretend otherwise.
  std::string antialias = "none";

  // The presenter's output filter: "bilinear", "cas", "fsr", "fsr2", "fsr3".
  //
  // CAS is FidelityFX Contrast Adaptive Sharpening - the only real sharpening
  // the runtime has. It was long believed absent: this menu carried a comment
  // saying present_effect "declares exactly one value, bilinear", which was
  // true of the STOCK plugin. The SDK's own build gates the spatial CAS/FSR
  // shaders (committed, prebuilt, needing no download) behind the same option
  // as the temporal FSR2/3 path, and that option is off in the shipped build.
  // A source-built runtime declares all five and CAS works.
  //
  // The row is driven by what the running build actually declares, so on a
  // stock plugin it shows one value and says why rather than offering a filter
  // that would do nothing.
  std::string present_effect = "bilinear";

  // Extra CAS sharpening, 0..1, on top of what the filter does by itself.
  float cas_sharpness = 0.0f;

  // The on-screen readouts, top right (ng2_perf_hud.h): the master switch,
  // on F8, apart from which readouts are chosen.
  bool hud_enabled = false;

  bool hud_fps = true;
  bool hud_gpu = true;
  bool hud_vram = true;

  // Process CPU across all cores, and a small bar under each readout (frame
  // rate / CPU / GPU / video memory) showing its instantaneous level, in the
  // Fable II style. Both only ever show when the readouts are on (hud_enabled /
  // F8), so an update that adds them changes nothing until the player turns the
  // HUD on.
  bool hud_cpu = true;
  bool hud_ram = true;   // process system-memory (RAM) readout, under CPU
  bool hud_bars = true;

  bool present_dither = false;
  bool letterbox = true;

  // --- Comfort ------------------------------------------------------------
  // Seconds of mouse stillness over the window before the pointer hides.
  // 0 = never hide. Borrowed from re:Blue, which does the same thing.
  int cursor_hide_seconds = 5;

  // Drive the guest pad from the keyboard and mouse. The runtime has the
  // driver; without this it is off and only a real controller works, which on
  // a PC port is a surprising default.
  bool keyboard_control = false;

  // --- Content -----------------------------------------------------------
  // The last disc image chosen in the installer. Kept only so the field is not
  // blank next launch: an empty box beside a 7 GB file that is still sitting
  // there reads as "the ISO is gone".
  std::string iso_path;

  std::string game_path;  // folder containing default.xex; empty = game/
  std::string dlc_path;   // folder of STFS packages; empty = dlc/

  // Set once the user has pressed Play at least once, so the setup screen only
  // interrupts the first run. Hold Shift at launch to get it back.
  bool configured = false;

  // --- PS5 (restart) - [ps5 port] -----------------------------------------
  // The resolution presets (CLAUDE.md 5.7): 0 = 4K, 1 = 1440p, 2 = 720p. Each
  // is an exact integer scale of the 1280x720 base (3x, 2x, 1x), which is the
  // only kind of scale the Xenos backend renders; 1080p would be 1.5x and is
  // not offered for that reason.
  int ps5_resolution = 1;  // 1440p: the tested default (hub_settings.cpp)
  // 0 = the scale the resolution implies; 1..3 forces one (Advanced).
  int ps5_render_scale = 0;
  // The one asset change: the PlayStation button pictures (5.9).
  bool ps5_playstation_prompts = true;
  // Developer: write every small picture the game loads to logs/hud_pictures,
  // which is how the Xbox button pictures are found (M7).
  bool ps5_dump_hud_pictures = false;
  // Frame pacing: guest vblanks on the display's refresh (smooth) or the
  // runtime's own 60 Hz timer.
  bool ps5_display_locked_vblank = true;
  // [IzunaRecomp] V-Sync on a PC: 0 Auto (the screen's own refresh when it is
  // steady and 60 fps divides it, else 60 Hz), 1 a 60 Hz clock, 2 a 59.94 Hz
  // clock (the Xbox 360's NTSC rate). Either clock is what a variable-refresh
  // screen (G-Sync / FreeSync) shows evenly.
  int pc_vsync = 0;
  // [IzunaRecomp] Button prompts on a PC: 0 Auto (PlayStation pictures when
  // the hub last saw a PlayStation controller), 1 the game's own, 2 PlayStation.
  int pc_prompts = 0;
  // [IzunaRecomp] Input latency on a PC: Low paints a picture only once the one
  // before it is on the screen (VK_KHR_present_wait), so at most one waits for
  // the screen (the PS5's Low uses VideoOut). Standard by default (test24-25
  // made Low the default; the owner found those builds stutterier - the
  // measured cause was elsewhere, but Standard is the setting they played).
  bool pc_low_latency = false;
  // [IzunaRecomp] The session log in logs/ (Developer tab). Off: no log at
  // all - no file, no log threads - and the renderer skips the timing it
  // measures only for the log (ng2_app.h's OnConfigurePaths: log_level off).
  bool logging = true;
  // [IzunaRecomp] Resolution on a PC: the picture's height - 720, 768, 900,
  // 1080, 1440, 1800 or 2160 (the 16:9 sizes 1280x720 ... 3840x2160), each a
  // true resolution (the game drawn at it, shown 1:1) - or 0 Auto: the exact
  // fit of the screen (fullscreen) or window. Resolved at startup (ng2_app.h,
  // ResolvePcRenderScale) into pc_scale_resolved.
  int pc_resolution = 0;
  float pc_scale_resolved = 0.0f;  // (not saved: the scale the renderer gets)
  // [IzunaRecomp] The keyboard's layout on a PC (izuna/izuna_keys.h): one key
  // per pad input, SETTINGS > Controls.
  izuna::keys::Keys pc_keys = izuna::keys::Defaults();
  // [IzunaRecomp] Which round of PC defaults this file has had (OnConfigurePaths).
  int pc_defaults_version = 0;
  // [IzunaRecomp] Sound (both platforms; rex/audio/downmix.h, output_stage.h):
  // the master volume in percent; the stereo fold at full level with the
  // limiter catching the peaks (true) or the SDK's quieter normalized fold;
  // the 5.1 mix's LFE (bass) folded into a stereo output too.
  int audio_volume = 100;
  bool audio_full_stereo = true;
  bool audio_stereo_bass = false;
  // Leave out the white screen flash of the Flying Swallow (and any move
  // that flashes the screen the same way). The original has it; on by
  // default for comfort, a toggle away.
  bool ps5_remove_screen_flash = false;  // (coming soon: not applied yet)
  // Input latency Low [IzunaRecomp]: the guest vblank waits for the display's
  // pending flip, so each frame shows at the next refresh (native_vblank_wait_flips).
  // Standard lets two finished frames queue ahead of the screen. (In the main
  // port Low was two swapchain images, and felt uneven in 16.13 testing.)
  bool ps5_low_latency = true;
  // Diagnostics: sample the GPU command thread and log where it spends time.
  bool ps5_profile_gpu = false;
  // What Profile GPU thread samples: 0 the GPU command thread, 1 the game's
  // main thread.
  int ps5_profile_target = 0;
  // Cutscene audio fix (audio_resync): drop the audio backlog after the game
  // was late, so sound stays level with the picture.
  bool ps5_audio_resync = true;
  // [native renderer track] Developer options for the native renderer (the
  // hub's Developer tab; each becomes a native_* cvar, ng2_tuning.h).
#if defined(__PROSPERO__)  // (the console's test builds keep them; a PC player's do not)
  int ps5_native_save_frames = 600;        // save a native frame every N frames (0 off; -N: every N, keep all)
  int ps5_native_log_draws = 1;            // 0 off, 1 the first heavy 3D frames, 2 a heavy frame a minute
#else
  int ps5_native_save_frames = 0;
  int ps5_native_log_draws = 0;
#endif
  bool ps5_native_save_textures = false;   // save each newly decoded texture once
  bool ps5_native_save_resolves = false;   // save every resolve of each logged frame
  bool ps5_native_threaded = true;         // the game thread records, a worker draws
  bool ps5_native_pipeline_libraries = true;  // pipelines linked from per-shader parts
  bool ps5_native_fresh_pipelines = false; // ignore the saved pipeline cache
  // Developer, diagnostic: capture a frame report this many seconds after
  // the first frame (0 off) - every rendered image of two frames, written to
  // logs/frame_reports - not saved on.
  int ps5_frame_report_after = 0;
  // The hub's menu music from hub/music, off by default.
  bool ps5_menu_music = true;
  // The PS5's own first-run values have been applied over the PC defaults.
  bool ps5_defaults_applied = false;
  // Which round of PS5 defaults this file has had (kPs5DefaultsVersion in
  // hub_settings.h): a later round moves existing installs once.
  int ps5_defaults_version = 0;


  static std::filesystem::path Path() {
#if defined(__PROSPERO__) || defined(IZUNA_LINUX_DESKTOP)
    // With the rest of what the app writes, in UserData/.
    return ng2ps5::paths::UserData() / "ng2_settings.cfg";
#else
    return rex::filesystem::GetExecutableFolder() / "ng2_settings.cfg";
#endif
  }

  // A path from the settings file, absolute or relative to the executable's
  // folder. Relative is what makes an install portable: a pre-configured
  // folder with game\, dlc\, textures\ and user\ inside it keeps working
  // when it is moved to another drive or PC, with no visit to the setup
  // screen. Before this a relative path was taken against the process's
  // working directory, which is the folder only when launched from it.
  static std::filesystem::path Beside(const std::string& p) {
    std::filesystem::path path(p);
    if (path.is_absolute())
      return path;
    return rex::filesystem::GetExecutableFolder() / path;
  }

  // Where the game data actually comes from: the configured folder, or game/
  // beside the executable.
  std::filesystem::path ResolvedGamePath() const {
#if defined(__PROSPERO__)
    // [ps5 port] The app's Game Data folder: there is no picker, and the
    // player copies the disc there over FTP. [native renderer track] Through
    // paths::GameData(), which falls back to the main app's Game Data (internal
    // drive, then USB) - the same lookup the hub's check uses.
    return ng2ps5::paths::GameData();
#endif
    if (!game_path.empty())
      return Beside(game_path);
    return rex::filesystem::GetExecutableFolder() / "game";
  }

  // The texture folder (dump/ and pack/ inside it). Only meaningful when

  // Prepared videos, kept with the game data rather than beside the
  // executable, so they travel with an install wherever it is put and are
  // found again without anyone being asked where they went.
  std::filesystem::path ResolvedVideoPath() const {
    return ResolvedGamePath() / "video";
  }

  std::filesystem::path ResolvedDlcPath() const {
#if defined(__PROSPERO__)
    return rex::filesystem::GetExecutableFolder() / "DLC";  // [ps5 port]
#endif
    if (!dlc_path.empty())
      return Beside(dlc_path);
#if defined(IZUNA_LINUX_DESKTOP)
    return ng2ps5::paths::Dlc();
#else
    return rex::filesystem::GetExecutableFolder() / "dlc";
#endif
  }

  void Load() {
    std::ifstream in(Path());
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') continue;
      const auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      const std::string k = line.substr(0, eq);
      std::string v = line.substr(eq + 1);
      while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
      Apply(k, v);
    }
    REXLOG_INFO("Settings: loaded {}", Path().string());
  }

  void Save() const {
    std::ofstream out(Path(), std::ios::trunc);
    if (!out) {
      REXLOG_WARN("Settings: cannot write {}", Path().string());
      return;
    }
    out << "# ng2 recompilation settings\n"
        << "window_width=" << window_width << "\n"
        << "window_height=" << window_height << "\n"
        << "fullscreen=" << (fullscreen ? 1 : 0) << "\n"
        << "monitor=" << monitor << "\n"
        << "fps=" << fps << "\n"
        << "vsync=" << (vsync ? 1 : 0) << "\n"
        << "resolution_scale=" << resolution_scale << "\n"
        << "internal_720p=" << (internal_720p ? 1 : 0) << "\n"
        << "anisotropic=" << anisotropic << "\n"
        << "texture_dump=" << (texture_dump ? 1 : 0) << "\n"
        // Without this the detection re-runs every launch and overwrites a
        // choice the player made by hand, which is worse than never detecting.
        << "antialias=" << antialias << "\n"
        << "present_dither=" << (present_dither ? 1 : 0) << "\n"
        << "letterbox=" << (letterbox ? 1 : 0) << "\n"
        << "game_path=" << game_path << "\n"
        << "dlc_path=" << dlc_path << "\n"
        // Read by Apply() but never written back until now, so every change
        // to one of these lasted until the next Save() and then vanished.
        // video_mode is the one that matters: it is what "Skip intro videos"
        // sets, and a setting that forgets itself is worse than no setting.
        << "video_mode=" << video_mode << "\n"
        // Same fault as video_mode above, found the same way: these four were
        // parsed on load and never written on save, so every F8 choice lasted
        // until the next Save() and then went back to the defaults.
        << "hud_enabled=" << (hud_enabled ? 1 : 0) << "\n"
        << "hud_fps=" << (hud_fps ? 1 : 0) << "\n"
        << "hud_gpu=" << (hud_gpu ? 1 : 0) << "\n"
        << "hud_vram=" << (hud_vram ? 1 : 0) << "\n"
        << "hud_cpu=" << (hud_cpu ? 1 : 0) << "\n"
        << "hud_ram=" << (hud_ram ? 1 : 0) << "\n"
        << "hud_bars=" << (hud_bars ? 1 : 0) << "\n"
        // Nine more with the same fault, found by the ROUNDTRIP sweep rather
        // than by anyone noticing: each was parsed on load and never written,
        // so it went back to its default at every launch. Between them they
        // cover the AI texture settings, the output filter and its sharpness,
        // both rendering workarounds, the cinematic skip and the remembered
        // disc image - all of them things a player sets once and expects to
        // stay set.
        << "chapter12_workaround=" << (chapter12_workaround ? 1 : 0) << "\n"
        << "present_effect=" << present_effect << "\n"
        << "cas_sharpness=" << cas_sharpness << "\n"
        << "iso_path=" << iso_path << "\n"
        << "keyboard_control=" << (keyboard_control ? 1 : 0) << "\n"
        << "cursor_hide_seconds=" << cursor_hide_seconds << "\n"
        << "configured=" << (configured ? 1 : 0) << "\n";
    // [ps5 port]
    out << "ps5_resolution=" << ps5_resolution << "\n"
        << "ps5_render_scale=" << ps5_render_scale << "\n"
        << "ps5_playstation_prompts=" << (ps5_playstation_prompts ? 1 : 0) << "\n"
        << "ps5_dump_hud_pictures=" << (ps5_dump_hud_pictures ? 1 : 0) << "\n"
        << "ps5_display_locked_vblank=" << (ps5_display_locked_vblank ? 1 : 0) << "\n"
        << "pc_vsync=" << pc_vsync << "\n"
        << "pc_prompts=" << pc_prompts << "\n"
        << "pc_low_latency=" << (pc_low_latency ? 1 : 0) << "\n"
        << "logging=" << (logging ? 1 : 0) << "\n"
        << "pc_defaults_version=" << pc_defaults_version << "\n"
        << "pc_resolution=" << pc_resolution << "\n"
        << "audio_volume=" << audio_volume << "\n"
        << "audio_full_stereo=" << (audio_full_stereo ? 1 : 0) << "\n"
        << "audio_stereo_bass=" << (audio_stereo_bass ? 1 : 0) << "\n";
    for (int i = 0; i < izuna::keys::kCount; ++i)
      out << "key_" << izuna::keys::kActions[i].id << "=" << pc_keys[size_t(i)] << "\n";
    out
        << "ps5_remove_screen_flash=" << (ps5_remove_screen_flash ? 1 : 0) << "\n"
        << "ps5_low_latency=" << (ps5_low_latency ? 1 : 0) << "\n"
        << "ps5_profile_gpu=" << (ps5_profile_gpu ? 1 : 0) << "\n"
        << "ps5_profile_target=" << ps5_profile_target << "\n"
        << "ps5_audio_resync=" << (ps5_audio_resync ? 1 : 0) << "\n"
        << "ps5_native_save_frames=" << ps5_native_save_frames << "\n"
        << "ps5_native_log_draws=" << ps5_native_log_draws << "\n"
        << "ps5_native_save_textures=" << (ps5_native_save_textures ? 1 : 0) << "\n"
        << "ps5_native_save_resolves=" << (ps5_native_save_resolves ? 1 : 0) << "\n"
        << "ps5_native_threaded=" << (ps5_native_threaded ? 1 : 0) << "\n"
        << "ps5_native_pipeline_libraries=" << (ps5_native_pipeline_libraries ? 1 : 0) << "\n"
        << "ps5_native_fresh_pipelines=" << (ps5_native_fresh_pipelines ? 1 : 0) << "\n"

        << "ps5_menu_music=" << (ps5_menu_music ? 1 : 0) << "\n"
        << "ps5_defaults_applied=" << (ps5_defaults_applied ? 1 : 0) << "\n"
        << "ps5_defaults_version=" << ps5_defaults_version << "\n";
    REXLOG_INFO("Settings: saved {}", Path().string());
  }

  void Clamp() {
    window_width = std::clamp(window_width, 640, 7680);
    window_height = std::clamp(window_height, 480, 4320);
    // Upper bound is 60 deliberately, and it is enforced here rather than only in
    // the menu so an old settings file carrying 120 or 144 - or a hand-edited one -
    // loads as 60 instead of speeding the game up.
    fps = std::clamp(fps, 30, 60);
    // Same reason: vsync off raises the guest vblank from 60 Hz to 1000 Hz and
    // this title advances its logic on vblank, so off is a speed-up, not tearing.
    // Forced on whatever an old or hand-edited file says.
    vsync = true;
    monitor = std::clamp(monitor, 0, 16);
    resolution_scale = std::clamp(resolution_scale, 1, 8);
    anisotropic = std::clamp(anisotropic, -1, 4);
    // Only the three the tool and the menu actually offer.
    cas_sharpness = std::clamp(cas_sharpness, 0.0f, 1.0f);
    if (antialias.empty())
      antialias = "none";
    cursor_hide_seconds = std::clamp(cursor_hide_seconds, 0, 60);
    video_mode = std::clamp(video_mode, 0, 2);
    if (video_mode == 1)
      video_mode = 0;  // retired overlay mode, see the field's comment
    // [ps5 port]
    ps5_resolution = std::clamp(ps5_resolution, 0, 2);
    ps5_render_scale = std::clamp(ps5_render_scale, 0, 3);
#if defined(__PROSPERO__)  // [IzunaRecomp] (the console's rules; a PC has a window and a keyboard)
    fullscreen = true;
    keyboard_control = false;
#endif
    audio_volume = std::clamp(audio_volume, 0, 100);
  }

 private:
  static bool Truthy(const std::string& v) {
    return v == "1" || v == "true" || v == "yes";
  }

  void Apply(const std::string& k, const std::string& v) {
    if (k == "window_width") window_width = std::atoi(v.c_str());
    else if (k == "window_height") window_height = std::atoi(v.c_str());
    else if (k == "fullscreen") fullscreen = Truthy(v);
    else if (k == "monitor") monitor = std::atoi(v.c_str());
    else if (k == "fps") fps = std::atoi(v.c_str());
    else if (k == "vsync") vsync = Truthy(v);
    else if (k == "resolution_scale") resolution_scale = std::atoi(v.c_str());
    else if (k == "internal_720p") internal_720p = Truthy(v);
    else if (k == "chapter12_workaround") chapter12_workaround = Truthy(v);
    else if (k == "video_mode") video_mode = std::atoi(v.c_str());
    // Migration: the setting used to be a bool that only meant "skip".
    else if (k == "skip_videos") video_mode = Truthy(v) ? 2 : 0;
    else if (k == "anisotropic") anisotropic = std::atoi(v.c_str());
    else if (k == "texture_dump") texture_dump = Truthy(v);
    else if (k == "antialias") antialias = v;
    else if (k == "hud_enabled") hud_enabled = Truthy(v);
    else if (k == "hud_fps") hud_fps = Truthy(v);
    else if (k == "hud_gpu") hud_gpu = Truthy(v);
    else if (k == "hud_vram") hud_vram = Truthy(v);
    else if (k == "hud_cpu") hud_cpu = Truthy(v);
    else if (k == "hud_ram") hud_ram = Truthy(v);
    else if (k == "hud_bars") hud_bars = Truthy(v);
    else if (k == "hud_graph") hud_bars = Truthy(v);  // migrate the old name
    else if (k == "present_effect") present_effect = v;
    else if (k == "cas_sharpness") cas_sharpness = float(std::atof(v.c_str()));
    else if (k == "present_dither") present_dither = Truthy(v);
    else if (k == "letterbox") letterbox = Truthy(v);
    else if (k == "cursor_hide_seconds") cursor_hide_seconds = std::atoi(v.c_str());
    else if (k == "keyboard_control") keyboard_control = Truthy(v);
    else if (k == "iso_path") iso_path = v;
    else if (k == "game_path") game_path = v;
    else if (k == "dlc_path") dlc_path = v;
    else if (k == "configured") configured = Truthy(v);
    // [ps5 port]
    else if (k == "ps5_resolution") ps5_resolution = std::atoi(v.c_str());
    else if (k == "ps5_render_scale") ps5_render_scale = std::atoi(v.c_str());
    else if (k == "ps5_playstation_prompts") ps5_playstation_prompts = Truthy(v);
    else if (k == "ps5_dump_hud_pictures") ps5_dump_hud_pictures = Truthy(v);
    else if (k == "ps5_display_locked_vblank") ps5_display_locked_vblank = Truthy(v);
    else if (k == "pc_vsync") pc_vsync = std::clamp(std::atoi(v.c_str()), 0, 2);
    else if (k == "pc_prompts") pc_prompts = std::clamp(std::atoi(v.c_str()), 0, 2);
    else if (k == "pc_low_latency") pc_low_latency = Truthy(v);
    else if (k == "logging") logging = Truthy(v);
    else if (k == "pc_defaults_version") pc_defaults_version = std::atoi(v.c_str());
    else if (k == "pc_render_scale") {  // (test11-13's: a multiple, in tenths in test12-13)
      int v10 = std::atoi(v.c_str());
      if (v10 >= 1 && v10 <= 3) v10 *= 10;
      pc_resolution = v10 == 10 ? 720 : v10 == 15 ? 1080 : v10 == 20 ? 1440 : v10 == 30 ? 2160 : 0;
    }
    else if (k == "pc_resolution") {
      const int h = std::atoi(v.c_str());
      pc_resolution = (h == 720 || h == 768 || h == 900 || h == 1080 || h == 1440 || h == 1800 || h == 2160) ? h : 0;
    }
    else if (k == "audio_volume") audio_volume = std::clamp(std::atoi(v.c_str()), 0, 100);
    else if (k == "audio_full_stereo") audio_full_stereo = Truthy(v);
    else if (k == "audio_stereo_bass") audio_stereo_bass = Truthy(v);
    else if (k.rfind("key_", 0) == 0 && izuna::keys::Find(std::string_view(k).substr(4)) >= 0)
      pc_keys[size_t(izuna::keys::Find(std::string_view(k).substr(4)))] = v;
    else if (k == "ps5_remove_screen_flash") ps5_remove_screen_flash = Truthy(v);
    else if (k == "ps5_low_latency") ps5_low_latency = Truthy(v);
    else if (k == "ps5_profile_gpu") ps5_profile_gpu = Truthy(v);
    else if (k == "ps5_profile_target") ps5_profile_target = std::clamp(std::atoi(v.c_str()), 0, 1);
    else if (k == "ps5_audio_resync") ps5_audio_resync = Truthy(v);
    else if (k == "ps5_native_save_frames") ps5_native_save_frames = std::clamp(std::atoi(v.c_str()), -6000, 6000);
    else if (k == "ps5_native_log_draws") ps5_native_log_draws = std::clamp(std::atoi(v.c_str()), 0, 2);
    else if (k == "ps5_native_save_textures") ps5_native_save_textures = Truthy(v);
    else if (k == "ps5_native_save_resolves") ps5_native_save_resolves = Truthy(v);
    else if (k == "ps5_native_threaded") ps5_native_threaded = Truthy(v);
    else if (k == "ps5_native_pipeline_libraries") ps5_native_pipeline_libraries = Truthy(v);
    else if (k == "ps5_native_fresh_pipelines") ps5_native_fresh_pipelines = Truthy(v);

    else if (k == "ps5_menu_music") ps5_menu_music = Truthy(v);
    else if (k == "ps5_defaults_applied") ps5_defaults_applied = Truthy(v);
    else if (k == "ps5_defaults_version") ps5_defaults_version = std::atoi(v.c_str());
  }
};
