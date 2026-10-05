# Building and testing

- [Requirements](#requirements)
- [Quick start](#quick-start)
- [Make targets](#make-targets)
- [Make variables](#make-variables)
- [Tests](#tests)
- [Benchmarking on the device](#benchmarking-on-the-device)
- [Packaging and installing](#packaging-and-installing)
- [Binary compatibility](#binary-compatibility)

---

## Requirements

SubForce builds on Linux or WSL; it is developed on Ubuntu 24.04.

| Tool | Needed for |
|---|---|
| `g++` 13 | tests, demos and the x86 bench |
| `arm-linux-gnueabihf-g++` 13 | the device build (the Force ships GCC 13's libstdc++, linked dynamically) |
| GNU make ≥ 4.3 | everything |
| `python3` | generating the parameter list, layout and C++ headers from `surface/surface.py` |
| `gcc` | the skin generator's C renderer |
| Python 3 with Pillow (`PY=`) | the skin, the page previews and the release package |
| `qemu-user` (`qemu-arm`) | `test-arm`, `test-arm-pgo` and the profile-guided device build |
| `ssh`, `scp` | `bench-device`, `plugin-install` |

On Ubuntu 24.04, for example:

```sh
sudo apt install g++ g++-arm-linux-gnueabihf make python3 python3-pil qemu-user
```

`PY` must be the Python that Pillow is installed for (with Ubuntu's `python3-pil`, `/usr/bin/python3.12`).

## Quick start

```sh
make test                      # the full suite under ASan/UBSan
make arm-plugin                # build/arm/subforce.so
make demos                     # every factory preset as a WAV, in build/demos-out/
make skin preview              # the skin, and every page as surface/build/page_*.png
make plugin-package            # dist/SubForce-<version>-mpc-armv7.zip
```

`surface/surface.py` is the single source of the parameter list and the touchscreen pages. Every
build regenerates `params.json`, `layout.conf`, `vst.json` and the C++ headers from it when it
changes; it checks the layout and every factory preset before writing anything.

## Make targets

| Target | What it does |
|---|---|
| `surface` | Regenerate parameters, layout and C++ headers from `surface/surface.py` (automatic) |
| `skin` | Build the skin (`TUI.json` + PNGs) with the vendored generator, then polish it with `surface/skin_polish.py` |
| `preview` | Render every page as `surface/build/page_*.png` |
| `test` | The ASan/UBSan suite |
| `test-arm` | The same suite built for the Force's CPU, run under `qemu-arm` |
| `test-arm-pgo` | The suite linked against the profile-guided objects the shipped `.so` is made of |
| `demos` | Render every factory preset (a phrase per category) and a filter sweep to `build/demos-out/*.wav` (stereo, L = R, as the plugin plays) |
| `preset-levels` | Set every factory preset's volume for `PRESET_LUFS` (default −18) on its demo phrase |
| `bench` | x86 bench: only proves the bench and the profiling build work |
| `arm-plugin` | `build/arm/subforce.so`; profile-guided when `qemu-arm` is installed |
| `arm-bench-stages` | `build/arm/subforce_stages.so`, the profiling build (never shipped) |
| `bench-device` | Run the CPU bench on a device (see [below](#benchmarking-on-the-device)) |
| `plugin-package` | `dist/SubForce-<version>-mpc-armv7.zip` with the installer |
| `plugin-install` | Package, copy to the device and install (stops and restarts MPC) |
| `clean` | Remove `build/` and `surface/build/` |

`make` on its own runs the tests and builds the device `.so` and the x86 profiling build.

## Make variables

| Variable | Meaning |
|---|---|
| `FORCE` | The device's SSH address, `root@<ip>`; required by `bench-device` and `plugin-install` |
| `SSH_KEY` | Private key for the device's root login (default: ssh's own keys and config) |
| `PY` | Python 3 with Pillow, for `skin`, `preview` and `plugin-package` (default `python3`) |
| `PGO` | `auto` (default): profile-guided when `qemu-arm` is installed; `1`: always; `0`: plain build |
| `PLUGIN_VERSION` | Version in the package name |
| `BENCH_ARGS` | `sfbench` arguments for `bench-device` (default `-s 3`) |
| `PRESET_LUFS` | The loudness `preset-levels` matches the factory presets to |

Pass variables on the command line, or keep your own in `local.mk` next to the Makefile (git
ignores it):

```make
FORCE   = root@192.168.0.10
SSH_KEY = $(HOME)/.ssh/force
PY      = /usr/bin/python3.12
```

## Tests

`make test` measures the engine directly and drives the whole plugin through its VST2 entry points
against a fake MPC host (`test/host.h`), under AddressSanitizer and UndefinedBehaviorSanitizer (any
undefined behaviour fails the run). The tests give the surface a clock that moves a second per host
event, so stepping never depends on the machine's speed:

| File | Covers |
|---|---|
| `test/engine_test.cpp` | The math helpers' error bounds; the decimator's passband and stopband; every wave shape's aliasing, pitch and DC; a swept pulse width; sync, the sub (and its octave switch), the keyboard reset; the ladder's self-oscillation, slopes, bass loss, key tracking and drive; envelope timing, loop, reset, velocity; noise colour loudness; idling; stability with everything at full |
| `test/keys_test.cpp` | Note priority, multi and single trigger, re-striking a sounding key, the pedal, more keys than remembered, Duo, mode changes with keys down, glide (Rate, Time, Exp; Always, Legato; which oscillators; from the note's own sample) |
| `test/mod_test.cpp` | The busses: every source, depth, rate, sync (free and locked to the bar), mod wheel / velocity / pressure, the filter EG as a source, every destination, Other Rate (on a locked bus too) |
| `test/preset_test.cpp` | Saved state round trips and bad input, presets (init, save, step, the ends, after RANDOM, missing files), user numbering, files appearing and renamed while running, the browser, favorites, stepping and the values pushed back, randomize, every factory preset playing |
| `test/plugin_test.cpp` | The VST2 basics, MIDI timing and mapping (pedal, mod wheel, pressure, bend both ways), pitch, octaves, CC 120 / 123, suspend, `process()` against `processReplacing`, floods of events and random patches |

`make test-arm` runs the same suite cross-compiled for the Force's CPU under `qemu-arm` (no
sanitizers): it catches 32-bit and ARM-only paths.

### Environment overrides

| Variable | Replaces |
|---|---|
| `SF_PRESET_ROOTS` | The preset roots (colon-separated list) |
| `SF_DATA_DIR` | Where favorites and recent lists are kept (empty: nothing is saved) |

## Benchmarking on the device

```sh
make bench-device FORCE=root@<ip>
```

Copies the plugin, its profiling build and the bench (`sfbench`) to `/tmp` on the device, runs
pinned to core 1 while MPC keeps running (MPC's audio workers own cores 2–3), then deletes them.
The bench `dlopen()`s the `.so` like MPC and times every block. It reads no user folders, saves
nothing, and fails if any case fails. Cases: idle, the Init patch on a held note, a heavy patch
(everything on, Duo), and the heavy patch retriggered every 50 ms with glide.

## Packaging and installing

```sh
make plugin-package
```

Builds `dist/SubForce-<version>-mpc-armv7.zip`: the plugin and its skin as one folder, the installer
and uninstaller, a generated `INSTALL.md` and checksums. Shipped scripts run under BusyBox on the
device, so the build refuses CRLF line endings in them.

```sh
make plugin-install FORCE=root@<ip>
```

Packages, copies the package to the device and runs its installer: it **stops MPC** (save your
project first), backs up and edits `MPC.settings`, and starts MPC again. A reinstall keeps the
user's presets and favorites/recent lists.

## Binary compatibility

- The `.so` exports only `VSTPluginMain` (a linker version script; the build counts every defined
  dynamic symbol and fails otherwise) and links with `--no-undefined`: an unresolved symbol would
  otherwise only show as MPC crashing on load. `-fno-gnu-unique` keeps it unloadable.
- It needs glibc 2.38 (`__isoc23_sscanf`, from the C library headers of the toolchain), which is
  too new for devices still on MPC OS 2.x. The device build prints the highest glibc version it
  needs.
