# Building

## Dependencies

- Linux/WSL, Python 3.11+ with `capstone`, CMake 3.20+, Ninja and a C11 compiler.
- Linux: `build-essential cmake ninja-build python3-venv libsdl2-dev libgl-dev libegl-dev zlib1g-dev`. Install Capstone in your Python environment.
- Switch: devkitPro `switch-dev`, `switch-sdl2`, `switch-mesa` and their dependencies. The release uses the OpenGL renderer; NVK and a separate glslang build are not required.
- Optional tests: Node.js 20+ for the installer, AArch64 Linux GCC and QEMU for ARM regressions.

## Prepare your game

```sh
python3 tools/prepare_assets.py /path/to/MK-Deadly-Alliance-Europe.iso
python3 tools/analyze.py
python3 tools/lift.py
```

Or copy your complete extracted disc into `game/`, with the supported `default.xbe` at its root, then run the last two commands. The executable hash is in `config/project.json` and `installer/game.json`. Other revisions require new analysis. Generated sources and per-game analysis output are ignored by Git. Allow at least 10 GiB free for the build.

## Linux build

```sh
cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/linux -j4
build/linux/mkda_recomp game saves
```

Linux is the development and regression-test baseline. The tested release target is Nintendo Switch.

## Switch build

```sh
export DEVKITPRO=/opt/devkitpro
python3 tools/build_switch.py
python3 tools/package_switch.py
```

The NRO is `build/switch/mkdax.nro`. Copy it to `/switch/mkdax/`, with `installer/mkdax_env.txt` and your disc files under `/switch/mkdax/game/`. Launch through full-memory homebrew/title takeover.

## Tests

```sh
node --test installer/tests/core.test.mjs
node installer/tests/real-disc.mjs /path/to/your/MK-Europe.iso
python3 tools/test_linux.py
python3 tools/test_aarch64.py
python3 tools/test_sse_aarch64.py
```

Guest tests require generated code. Renderer fixture tests use local captures and are optional; retail captures are not distributed. The pacing regression checks 600 fast frames, stalls and context changes without controller replay.

To test Kano's actual arcade ending without playing arcade mode, run
`python3 tools/test_ending_preview.py` after building Linux. It enters the retail
ending loader after normal startup, uses `build/ending-preview/saves`, and compares
the complete scene with and without 60 Hz pacing. Logs and periodic frame images
are written under `logs/`. This explicit diagnostic is Linux-only and supplies no
controller input. The preview exits after the ending; it does not run in the
Switch build or normal launches.

## Releases

```sh
python3 tools/package_release.py --build build/switch --output build/release --version 1.0.2
```

The script exports the matching committed source, NRO, SD layout, checksums, licence notices and relink materials. It runs the supplied relink command and requires a byte-identical NRO. No retail data is copied into the release package. `tools/relink.py` supports replacing the LGPL APU archive with a modified one.

The installer is published on the repository's `gh-pages` branch. Its `release/` directory mirrors the NRO on the same origin to support browser downloads, and `release.json` supplies the verified hash.
