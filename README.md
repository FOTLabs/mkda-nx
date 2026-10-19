# Mortal Kombat: Deadly Alliance for Nintendo Switch

An unofficial native Nintendo Switch port of *Mortal Kombat: Deadly Alliance* (2002, original Xbox), built with [XboxRecomp](https://github.com/sp00nznet/xboxrecomp) static recompilation and the NV2A OpenGL renderer.

It runs at 960×720 with 2× MSAA and 4× anisotropic filtering, targeting the original 60 Hz gameplay speed. Movies with synchronised subtitles, audio, saves, left-stick movement and HD Rumble are supported. Menus, matches and the opening Konquest lessons have been tested on hardware; occasional visual glitches may remain.

> [!IMPORTANT]
> You need your own legally obtained **European Xbox release (English, French, German, Spanish, Italian)**. Other revisions, including USA, are not supported by this build. This repository contains no game files.

## How to install

1. Open **https://fotlabs.github.io/mkda-nx/** in desktop Chrome or Edge.
2. Select your extracted game folder (containing `default.xbe`) or Xbox ISO. The installer validates the executable, downloads and verifies the release NRO, and creates `mkda-nx.zip` locally. Your game files are never uploaded.
3. Extract the ZIP to the root of your SD card.
4. Launch `mkdax.nro` from the Homebrew Menu using title takeover (hold R while opening a game). Album applet mode does not provide enough memory.

```text
/switch/mkdax/
  mkdax.nro
  LICENSES.txt
  mkdax_env.txt        tested default settings
  game/               your extracted disc (default.xbe, artxbox/…)
  save/               created on first run
```

For manual installation, download `mkda-nx-v1.0.1-switch.zip` from [Releases](https://github.com/FOTLabs/mkda-nx/releases), extract it to your SD root, and copy your extracted disc into `/switch/mkdax/game/`. Keep existing `save/` files when updating. The installer validates the executable revision; use a complete, unmodified disc extraction.

## Controls

Each Switch face button maps to the Xbox button with the same letter.

| Switch | Xbox |
| --- | --- |
| A / B / X / Y | A / B / X / Y |
| L / R | White / Black |
| ZL / ZR | Left / Right trigger |
| + / − | Start / Back |
| D-pad / left stick | Movement |
| Right stick | Right stick |

Set `RECOMP_PAD_LAYOUT=position` to match Xbox face-button positions instead of letters. Backward dashes use two quick taps away from the opponent, on either D-pad or left stick.

## Settings

Settings live in `/switch/mkdax/mkdax_env.txt`, one `KEY=VALUE` per line (`#` starts a comment). The installer supplies the tested profile.

| Setting | Effect |
| --- | --- |
| `MKDA_RUMBLE_STRENGTH=0.55` | HD Rumble strength, from `0` to `1` |
| `RECOMP_GL_SCALE=1.5` | Internal resolution scale (`1` for native resolution) |
| `RECOMP_GL_MSAA=2` | Multisampling (`0` disables it) |
| `RECOMP_GL_ANISO=4` | Anisotropic texture filtering |
| `MKDA_LOG=1` | Write diagnostic logs; off in release defaults |

Keep the supplied audio and movie settings for the tested playback behaviour. The first boot can take longer while shaders are prepared. To report a problem, enable logging, reproduce it and attach `mkdax_log.txt` to an issue.

## Build

Linux/WSL is the verified development path. Switch builds require devkitPro (devkitA64, libnx, Switch SDL2 and Mesa), CMake, Ninja, Python and your game. See [docs/building.md](docs/building.md).

| Folder | Contents |
| --- | --- |
| `src/` | Launcher, title fixes and Switch platform code |
| `sdk/` | Vendored XboxRecomp runtime and code generator |
| `tools/` | Preparation, generation, build, test and release scripts |
| `installer/` | Browser installer; no game-file uploads |
| `tests/` | Host and guest regression tests |
| `docs/` | Build guide, project scope and licence texts |

## Credits

- **Midway Games** and everyone who made the original game.
- [**sp00nznet and XboxRecomp contributors**](https://github.com/sp00nznet/xboxrecomp): the static recompilation toolkit and runtime.
- [**xemu contributors**](https://github.com/xemu-project/xemu), including espes, Jannik Vogel and Matt Borgerson: MCPX APU and NV2A definitions, and fixed-function lighting references.
- **Mesa contributors**, [**devkitPro**](https://devkitpro.org/) and [**switchbrew/libnx**](https://github.com/switchbrew/libnx): graphics, toolchain and libraries.
- [**FOTLabs/buffy-nx**](https://github.com/FOTLabs/buffy-nx): release, browser-installer and HD Rumble reference.
- [**StevensND/nfsmw-nx**](https://github.com/StevensND/nfsmw-nx): inspiration for the browser installer.

## Legal

This is an unofficial fan project, unaffiliated with the Mortal Kombat rights holders, Midway, Microsoft or Nintendo. All trademarks and characters belong to their respective owners. No retail assets, executables or generated game source are included; you must provide your own game.

Project code is [MIT licensed](LICENSE). XboxRecomp is MIT; the xemu-derived files in `sdk/` remain LGPL-2.1-or-later. Binary releases include matching source and relink materials. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and [docs/legal.md](docs/legal.md). These licences grant no rights to the original game.
