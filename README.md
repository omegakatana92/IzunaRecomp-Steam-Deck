# IzunaRecomp — The Izuna Project

<img width="1280" height="720" alt="97089_dfcfff5534107b77" src="https://github.com/user-attachments/assets/aa900ec6-11f6-4c41-bf0e-e76c549b5be8" />


A static recompilation of **Ninja Gaiden II** (Xbox 360, title `544307D5`) for
Windows, drawn natively by the **Kasumi Renderer** (Vulkan).

The game's PowerPC code is translated ahead of time into native x86-64, and
its Direct3D calls are answered by the Kasumi Renderer, which draws every frame
itself through Vulkan from the game's own shaders, recompiled for your GPU.
There is no emulator in it: no GPU emulation, no interpreter, no JIT.

> **AI disclosure.** The code, documents and tooling in this repository were
> written by an AI coding agent (Anthropic's Claude, through Claude Code),
> directed by the maintainer, who also tested every build on real hardware.
> Treat it like any other early-stage community project: expect bugs, read the
> code before relying on it, and report problems through issues.

IzunaRecomp started as a fork of
[Ninja's Dawn](https://github.com/FermataRest/Ninja-s-Dawn-A-Ninja-Gaiden-2-Recompilation-Project),
the PC recompilation of Ninja Gaiden II, and grew into its own project: the
emulated GPU was replaced by a native renderer, and the app, its settings,
its menus and its mod support were rebuilt around it. Gameplay and game logic
stay 1:1 with the Xbox 360 game.

---

## What you must bring

**This project ships no game data of any kind** — no textures, videos, sound,
saves, DLC or executable image from the disc. You provide all of it from your
own copy.

| | What | |
|---|---|---|
| **1** | **Your own copy of Ninja Gaiden II for Xbox 360**, extracted to a folder containing `default.xex` | Nothing works without it. |
| **2** | **Windows 10 or 11, 64-bit** | |
| **3** | **A GPU with Vulkan** — NVIDIA GTX 900 / AMD RX 400 / Intel Arc or newer, with an up-to-date driver | |

Nothing else needs installing (no Visual C++ Redistributable).

## Getting started

1. Download `IzunaRecomp-<version>-windows.zip` from [Releases](../../releases)
   and unzip it anywhere.
2. Run `ng2.exe`. The hub asks for your game folder (the one with
   `default.xex`); it is remembered.
3. **START GAME.**

The first start takes longer: IzunaRecomp reads the game's shaders from your
copy and compiles them for your GPU (a progress bar shows how far along it
is). They are kept in `%LOCALAPPDATA%\IzunaRecomp\shaders`, so later starts
skip this.

`ng2.exe` is built for processors with AVX2 (Intel since 2013, every AMD
Ryzen); `ng2-compat.exe` is the same game for older processors, and `ng2.exe`
starts it by itself when needed. Keep both in the folder. The `README.txt` in
the release has the controls and the settings in detail.

## What's in it

- **The Kasumi Renderer** — native Vulkan rendering of the whole game, with
  the game's shaders recompiled from your copy at first start; any resolution
  (Auto: your screen's, or 720p to 2160p, each drawn at that true resolution),
  anti-aliasing, anisotropic filtering, output filters.
- **Frame pacing** — V-Sync Auto follows your monitor (60, 120, 144 Hz…,
  G-Sync / FreeSync), or locks the game's clock to 60 / 59.94 Hz; a Low input
  latency mode.
- **A full app around the game** — a hub with settings, an in-game menu
  (Esc / F10, or hold View / Share), performance readouts, saves import, DLC.
- **Keyboard and controller** — play with either or both; every key can be
  rebound; PlayStation or Xbox button prompts.
- **Sound** — full-level stereo with a limiter, the 5.1 mix's bass brought
  into stereo, and a cutscene audio fix that keeps sound in step with the
  picture.
- **Mods** — a `Mods` folder and a MODS page in the hub: replace the game's
  files (models, motions, effects, stages, text, sound banks), replace
  textures with pictures up to 2048 × 2048, hide draws by their shader, Lua
  script mods, and hooks on any of the game's functions. See
  [docs/MODDING.md](docs/MODDING.md); examples are in [examples/mods](examples/mods).
- **Steadiness** — threaded command recording, pipelines linked from
  per-shader parts, asynchronous file reads off the game's main thread, crash
  minidumps, and a Logging switch (Settings › Developer) that turns all
  logging off.

## Known issues

- Parts of some models (Sonia's back) can be missing.
- Glass reflections and glows in places (the tower in chapter 1) look off.
- There's occasional hitches and stutters, these will be improved upon.
- Windows only for now. The tree also builds for the PS5, but that build is
  not released.

Report anything else through issues, with the `logs` folder from beside
`ng2.exe` (the newest `ng2_*.log`, and a `crash-*.dmp` if there is one).

## Experimental Linux / Steam Deck development

This fork adds an opt-in native Linux desktop build and packaging scripts.
Compilation and gameplay status, prerequisites and remaining acceptance checks
are tracked in [docs/LINUX_STEAM_DECK.md](docs/LINUX_STEAM_DECK.md).
Use `tools/linux/build.sh` after SDK setup and supplying your own `assets/default.xex`.
This is not a claim of validated Steam Deck compatibility.

## Building from source

See [docs/BUILDING.md](docs/BUILDING.md). In short: the ReXGlue SDK is fetched
and patched by `tools/setup_sdk.sh`, the game's code is recompiled from **your
own** `default.xex` (never committed — `generated/` is reproducible from it),
and the Windows build is cross-compiled from Linux with Clang against the
headers and libraries of your own Build Tools for Visual Studio.

## Credits

- **Ninja's Dawn** by TheSaltTrader — the PC recompilation IzunaRecomp forks.
- **ReXGlue SDK** by Tom Clay, derived from **Xenia** — the recompiler and
  runtime, as carried by the svr2011-ps5 project's `rexglue-ps5` branch.
- **XenosRecomp** (hedge-dev), through goliathret's reeot-XenosRecomp fork —
  the shader recompiler behind the Kasumi Renderer's shader translation.
- Lua, Zstandard, smol-v, SDL, Dear ImGui, the Vulkan headers and Memory
  Allocator, DirectX Shader Compiler, and the Inter typeface — see
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Licence

IzunaRecomp's own source is under the MIT licence ([LICENSE](LICENSE)), as is
Ninja's Dawn, which it forks. Third-party components keep their own licences
([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). No game material is covered
by any of these licences or included in this repository; the executable in
each release contains the game's code in translated form and is useful only
with your own copy of the game.

Ninja Gaiden is a trademark of Koei Tecmo. IzunaRecomp is not affiliated with
Koei Tecmo or Microsoft.

<img width="1400" height="1000" alt="HWMupABXkAAt5eC" src="https://github.com/user-attachments/assets/79e3aff2-4e30-4cb7-a698-91b840723441" />

