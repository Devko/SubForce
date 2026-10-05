# Building and testing

- [Requirements](#requirements)
- [Quick start](#quick-start)
- [Make targets](#make-targets)
- [Make variables](#make-variables)
- [Tests](#tests)
- [Benchmarking on the device](#benchmarking-on-the-device)
- [Packaging and installing](#packaging-and-installing)
- [Release builds](#release-builds)
- [Diagnostics on the device](#diagnostics-on-the-device)
- [Binary compatibility](#binary-compatibility)

---

## Requirements

SubForce builds on Linux or WSL; it is developed on Ubuntu 24.04.

| Tool | Needed for |
|---|---|
| `g++` 13 | tests, demos and the x86 bench |
| `arm-linux-gnueabihf-g++` 11 or newer | the device build (libstdc++ is linked dynamically; MPC OS ships it). Release builds come from [CI](#release-builds) |
| GNU make ≥ 4.3 | everything |
| `python3` | generating the parameter list, layout and C++ headers from `surface/surface.py` |
| `gcc` | the skin generator's C renderer |
| Python 3 with Pillow (`PY=`) | the skin, the page previews and the release package |
| `qemu-user` (`qemu-arm`) | `test-arm`, `test-arm-pgo` and the profile-guided device build (`qemu-user-static` works too; `ARM_RUN` says how ARM programs run) |
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
| `PGO` | `auto` (default): profile-guided when ARM programs can run here (`qemu-arm`, or natively); `1`: always; `0`: plain build |
| `ARM_PREFIX` | The device toolchain's prefix (default `arm-linux-gnueabihf-`); empty for a native ARM build |
| `ARM_RUN` | How ARM programs run here (default `qemu-arm -L /usr/arm-linux-gnueabihf`); empty on ARM |
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
| `SF_FIXED_SEED` | Set: every instance the same random numbers (noise, drift, S&H, RANDOM); the tests and demos set it |
| `SF_TRACE_DIR` | Where the [diagnostics](#diagnostics-on-the-device) flag and log are (default `/tmp`) |

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

## Release builds

Releases are built by CI (`.github/workflows/build.yml`) on every push, the way the plugin catalog's
own ports are built: the device build runs in `arm32v7/gcc:11-bullseye` (GCC 11, glibc 2.31) under
QEMU, profile-guided, with the test suite run against the objects the `.so` is linked from; the
sanitizer suite runs on x86. The zip is checked with the catalog's own checker
(`third_party/mpc-vst-plugins/tools/catalog_check.py --catalog`) and kept as the run's artifact.

Pushing a tag `vX.Y.Z` also publishes it as a GitHub release, a prerelease while the version is 0.x
(the catalog's beta channel). The catalog reads the major version as the parameter list's
compatibility: bump X whenever parameter indices change.

The same build outside CI, in an ARM environment: `make ARM_PREFIX= ARM_RUN= PGO=1 plugin-package`
(`ARM_PREFIX` empty: the native compiler; `ARM_RUN` empty: ARM programs run directly).

A local build with a newer distribution's cross compiler (Ubuntu 24.04: glibc 2.39, the C23
`__isoc23_sscanf`) needs glibc 2.38. That loads on the Force and other MPC OS 3.x devices, fine for
testing, but the catalog refuses it.

## Diagnostics on the device

To see what MPC sends when a control is touched, turned or tapped, create the flag file while MPC
runs (no restart):

```sh
ssh root@<ip> touch /tmp/subforce.trace
```

Within a second every SubForce instance appends one line per `setParameter` to `/tmp/subforce.log`:
the time, the instance, the parameter, the value MPC sent, the value it had read back before and the
plugin's value and text after. Remove the flag file to stop. The log stops growing at 2 MB; `/tmp`
is cleared when the device restarts.

## Binary compatibility

- The `.so` exports only `VSTPluginMain` (a linker version script; the build counts every defined
  dynamic symbol and fails otherwise) and links with `--no-undefined`: an unresolved symbol would
  otherwise only show as MPC crashing on load. `-fno-gnu-unique` keeps it unloadable.
- The [release build](#release-builds) needs glibc 2.31 or less, so it loads on MPC OS 2.x and 3.x.
  Built with a newer toolchain it needs that toolchain's glibc (Ubuntu 24.04: 2.38). The device build
  prints the highest glibc version it needs, and `plugin-package` warns when it is over the
  catalog's 2.32.
- libstdc++ is linked dynamically. GCC 11's (the release build's) needs `GLIBCXX_3.4.29` (the
  floating-point `from_chars` the saved state is parsed with): MPC OS 3.x ships GCC 13's. The
  catalog's checker reads only the glibc version.
