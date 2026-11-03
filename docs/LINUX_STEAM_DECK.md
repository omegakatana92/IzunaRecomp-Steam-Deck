# Linux / Steam Deck port status

Baseline inspected: upstream commit `0bc9638cb4d2956b69b7b4801a02315c335d27be`.

**No native playable Linux build or AppImage has been produced or validated.**
The Vulkan renderer makes a native port a reasonable development target, but
this fork currently inherits Windows and PS5 implementations, not a finished
Linux desktop application. The Linux presets alone are insufficient.

Run `python3 tools/linux/check_prerequisites.py` to inspect local build inputs.
This command does not fetch dependencies or modify game files. Exit status 2
means an input is missing; status 0 only means the listed inputs exist, not
that the port is complete.

## Required private input

Provide your extracted Xbox 360 **Ninja Gaiden II**, title ID `544307D5`.
For code generation place your own executable at `assets/default.xex`; see
[BUILDING.md](BUILDING.md) for SDK setup and code generation. Runtime testing
also needs the complete extracted disc folder, including its `.ng2` archives.
A Windows path on another computer is not accessible to a remote build host.
Keep disc files and `generated/` out of Git commits and source archives.

## Concrete source blockers

| Area | Current behavior | Required Linux work |
| --- | --- | --- |
| Platform services | `src/ng2_platform.cpp` takes the Windows branch outside PS5 | Native folder selection, focus, display enumeration and safe process relaunch |
| Diagnostics | `ng2_diagnostics.cpp`, `ng2_autoskip.cpp`, `ng2_perf.cpp`, `ng2_hwdetect.cpp` contain Windows APIs outside PS5 | POSIX/SDL/Vulkan implementations or explicit unavailable diagnostics; do not report a fictitious PS5 GPU |
| Hub | CMake adds the desktop hub for Windows only | Wire Linux hub, controller input and audio without Sony stubs |
| Shaders | Non-Windows CMake requires a build-time game-derived `shader_cache.cpp` | Enable first-run shader translation with Linux DXC; the existing translator has a `dlopen` path, but CMake does not select it |
| Writable paths | PC paths use executable-relative `user`/`cache`; fallback uses `/data/ng2recomp` | XDG paths for settings, saves, shader cache, logs and mods; an AppImage mount is read-only |
| Runtime | SDK is fetched and patched separately | Compile patched SDK natively and verify guest memory reservation, signals, threads, audio and Vulkan presentation |

`NG2_PS5_HOST_TEST` defines `__PROSPERO__` and links Sony-service stubs. Its
launcher selects offscreen video, dummy audio and software Vulkan. It is a
renderer diagnostic, not a native desktop release or performance result.
Do not publish that binary as a working Steam Deck port.

## Build and packaging sequence

1. Install Clang/LLVM, CMake 3.25+, Ninja and the SDK's Linux development dependencies.
2. Run `tools/setup_sdk.sh` to fetch the pinned SDK and apply the project patches.
3. Build the SDK code generator and generate from the private executable as
   described in BUILDING.md; apply all listed post-codegen crash patches.
4. Implement the native desktop changes above, then configure and link the
   full game with the patched SDK. Resolve failures from actual compiler logs.
5. Stage a Linux x86-64 directory containing the ELF executable, required
   redistributable libraries, project assets and third-party notices. Keep
   game data external. Check all dependencies and the minimum GLIBC/GLIBCXX
   versions on the target SteamOS; building on a newer distribution can make
   the result incompatible even inside an AppImage.
6. Package that validated directory as a `.tar.gz` and an AppDir/AppImage.
   The AppRun launcher must preserve arguments, support paths with spaces,
   and write state outside its read-only mount. Use the host Vulkan driver;
   do not bundle a build machine's GPU driver.
7. Validate extracted AppImage execution and ordinary AppImage launch on
   Steam Deck, including Desktop and Gaming Mode, before marking a release
   playable. Include checksums and exact build revision with both artifacts.

## Acceptance checks on real Steam Deck hardware

- Hub opens at 1280x800 and can select a disc folder, including one on microSD.
- First-run shader compilation completes; subsequent launch reuses its cache.
- Built-in controls, Steam Input, audio and menus work in Gaming Mode.
- Reach gameplay and complete a sustained test; record frame times and audio
  behavior rather than assuming a frame-rate target.
- Save, exit, relaunch and load progress. Settings persist across relaunches.
- Suspend/resume and disconnected audio devices do not crash the application.
- Missing/wrong disc files give actionable errors. Neither release contains
  disc files, personal saves or private paths.

Until these steps pass, any port branch is experimental source work, not a
validated Linux build or playable AppImage.
