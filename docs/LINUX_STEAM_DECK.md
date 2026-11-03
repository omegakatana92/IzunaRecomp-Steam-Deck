# Experimental native Linux / Steam Deck build

This fork adds `IZUNA_LINUX_DESKTOP=ON` without defining `__PROSPERO__` or using
Sony-service stubs. It connects the SDL hub, controller/audio code and Vulkan
renderer, enables first-run shader translation with Linux DXC, and adds native
folder selection, display/focus detection, diagnostics and process relaunch.
The AppImage relaunch path uses the original AppImage, preserving its mount
lifetime. Optional GPU/VRAM diagnostic counters remain unavailable on Linux;
the renderer logs the Vulkan device actually selected.

**Build status:** development in progress; no playable Steam Deck validation.
The `NG2_PS5_HOST_TEST` diagnostic is a separate offscreen/software-rendering
path and must not be advertised as this desktop port.

## Private inputs

Use your extracted Xbox 360 **Ninja Gaiden II**, title ID `544307D5`.
Place your own `default.xex` at `assets/default.xex` for code generation.
Runtime testing requires the complete extracted disc directory, including its
`.ng2` archives, which you select in the hub. A Windows path on another machine
is not accessible to a remote build host. Do not commit disc files or
`generated/`. No disc executable or archive is copied by the packaging script.

The supplied test input was identified as title `544307D5`, media `69F555CB`,
version `0.0.0.4`. Its decoded image has 42,504 `.pdata` function entries.

## Build on Linux or WSL2 Ubuntu 24.04

Install Clang/LLVM 19, CMake 3.25+, Ninja, Python 3, Git, pkg-config and NASM,
plus SDL's Linux development dependencies. The build host used:

```sh
sudo apt-get install clang-19 lld-19 cmake ninja-build pkg-config nasm \
  libvulkan-dev libx11-dev libx11-xcb-dev libxrandr-dev libxi-dev \
  libxcursor-dev libxinerama-dev libxss-dev libxtst-dev libxfixes-dev \
  libxext-dev libxrender-dev libxcb1-dev libxcb-keysyms1-dev libxcb-xinput-dev \
  libasound2-dev libpulse-dev libudev-dev libwayland-dev libxkbcommon-dev \
  libdrm-dev libgbm-dev libegl-dev libgl-dev libdecor-0-dev \
  wayland-protocols libpipewire-0.3-dev
```

Build on the Linux filesystem, not under `/mnt/c` when using WSL2.

```sh
bash tools/setup_sdk.sh
mkdir -p assets
cp '/path/to/your/extracted/game/default.xex' assets/default.xex
JOBS=4 bash tools/linux/build.sh
```

The script builds the patched SDK code generator, generates the private game
code, applies the existing crash/correctness patches and hook thunks, then
configures and builds `out/linux-desktop/ng2`. It uses `-march=x86-64-v3`,
which Steam Deck's CPU supports. Other Linux PCs need AVX2 and the other v3
CPU features. `CC`/`CXX` can override the compiler and `JOBS` limits parallelism.

After code generation, the equivalent app preset is
`cmake --preset linux-steam-deck-release`, then
`cmake --build --preset linux-steam-deck-release --parallel 4`.
`python3 tools/linux/check_prerequisites.py` reports local missing inputs;
its success is not evidence of a successful compile or gameplay.

## Shader compiler and packaging

The executable loads **Linux x86-64 `libdxcompiler.so`**, not `dxcompiler.dll`.
The build host uses DXC 1.8.2407's
`linux_dxc_2024_07_31.x86_64.tar.gz` from Microsoft's official
[release](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.8.2407).
Place `libdxcompiler.so` beside `ng2` for an unpackaged launch.

```sh
DXC_SO=/absolute/path/to/libdxcompiler.so \
LINUXDEPLOY=/absolute/path/to/linuxdeploy-x86_64.AppImage \
  bash tools/linux/package.sh
```

The script checks both binaries are x86-64 ELF, stages only selected inputs,
collects dependency licences, and uses linuxdeploy to produce:

- `out/release/IzunaRecomp-<version>-experimental-linux-x86_64.AppImage`
- the corresponding portable `.tar.gz` (extract and run its `AppRun`)
- SHA-256 checksums

It refuses to overwrite an existing staging directory. Use a fresh directory
when rebuilding. Keep the host Vulkan driver; do not bundle the build host's
GPU driver. The AppImage still needs a working Vulkan driver and compatible
system GLIBC/GLIBCXX versions. The hub's folder picker requires `zenity` on
the target system. Alternatively supply an absolute folder using
`--game_data_root='/path/to/extracted/game'`.

## Writable files

| Content | Default Linux location |
| --- | --- |
| Profile and saves | `~/.local/share/IzunaRecomp/user/` |
| Settings | `~/.local/share/IzunaRecomp/user/ng2_settings.cfg` |
| Logs | `~/.local/share/IzunaRecomp/logs/` |
| DLC / save imports / mods | `~/.local/share/IzunaRecomp/` subdirectories |
| Shader and pipeline caches | `~/.cache/IzunaRecomp/` |

Absolute `XDG_DATA_HOME` and `XDG_CACHE_HOME` override those roots. State is
external to the AppImage's read-only mount. The game directory remains where
you select it; it is not copied into these paths.

## Validation

The Linux protected-page test compiles and passes: readable bytes on either
side of a `PROT_NONE` page survive, while the inaccessible page is zeroed.
This exercises the actual image-copy implementation, used to avoid startup
faults when scanning guest memory for shaders.

```sh
mkdir -p out/linux-tests
clang++-19 -std=c++23 -Isrc src/izuna/izuna_readable.cpp \
  tests/linux/readable_pages.cpp -o out/linux-tests/readable-pages
out/linux-tests/readable-pages
```

Before claiming Steam Deck compatibility, check on real hardware:

- Hub at 1280x800, disc selection including microSD, and first-run shaders.
- Built-in controls, Steam Input, audio and menus in Desktop and Gaming Mode.
- Sustained gameplay with measured frame times and stable audio.
- Save, exit, relaunch and load; settings and shader caches persist.
- Return to hub from inside an AppImage; suspend/resume without a crash.
- Missing/wrong disc errors; clean package with no disc files or personal saves.

No frame-rate or playability claim follows from compilation alone.
