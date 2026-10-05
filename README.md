# SubForce

**An analog-style monosynth that runs natively inside MPC on the Akai Force.**

SubForce is a VST2 instrument for MPC OS's built-in plugin host, with its own touchscreen pages
and Q-Link sets. It tips its hat to a classic 37-key American paraphonic analog monosynth (*the
original* in these docs), built on the same plugin groundwork as its sibling
[PolyForce](https://github.com/Devko/PolyForce). The name, DSP and presets are all SubForce's own.

> [!NOTE]
> **Preview (0.0.x).** The engine, plugin, touchscreen pages and factory presets are complete and
> pass the full test suite on x86 and under ARM emulation. On a Force (MPC OS 3.9) the release build
> installs, loads and benches at about 3% of a block; not every page has been played on the device
> yet. The parameter list may still change before v0.1: sounds are saved by name and survive that,
> but recorded automation (stored by parameter index) could then move a different control.

![SubForce's FILTER page](docs/img/filter.png)

*The FILTER page, rendered offline from the skin (on the device MPC fills in the values).*

## Highlights

- **Two oscillators** with a continuously variable wave (triangle → saw → square → narrow pulse),
  32' to 2', hard sync, a square **sub oscillator** (−1 or −2 octaves) and **noise** (white, pink
  or dark)
- **Mixer with feedback** — the mixer's output back into it, as on the original: thicker, then
  gritty, then the chaos of an overdriven loop — and the classic habit of overdriving the filter
  when the levels are high
- **4-pole transistor ladder filter**: 6, 12, 18 or 24 dB, resonance self-oscillating past 70% of
  the knob, the ladder's bass loss, **Multidrive** (asymmetric, tube-like warmth to hard clipping),
  keyboard tracking up to 200%
- **Two DAHDSR envelopes** (filter, amp) with a linear attack, velocity, keyboard tracking, reset,
  and a loop that runs through the release as the original's does
- **Two mod busses**: triangle, square, saw, ramp, S&H, smooth random or the filter EG, free or
  locked to MPC's tempo, to pitch, cutoff and one more destination, scaled by the mod wheel,
  pressure or velocity
- **Mono or Duo** (each oscillator its own key), note priority, single or multi trigger,
  **glide** (rate, time or exponential)
- **Band-limited and oversampled**: polyBLEP oscillators and the whole voice at 2× (88.2 kHz) with a
  halfband decimator; worst aliasing −53 dB up to C7, hard sync −65 dB
- **57 factory presets** in 7 categories, level-matched, many of them for
  [melodic techno](docs/USER_GUIDE.md#melodic-techno) (rolling basslines, resonant sequences,
  big leads); user presets, favorites, a browser, Init and Randomize
- **Light on the CPU**: one voice, about 2.8% of a block on the Force (3.6% with everything on),
  with NEON where the work is parallel

Effects are deliberately left out: use MPC's insert effects on the track.

## Listen

From source, on Linux or WSL: `make demos` renders every factory preset and a filter sweep through
the plugin's own entry points into `build/demos-out/` (WAV, and all of them back to back as
`tour.wav`), the way MPC plays it — no device needed.

## Documentation

| Document | What's in it |
|---|---|
| [User guide](docs/USER_GUIDE.md) | The pages, the sound engine, presets, melodic techno, MIDI |
| [Building](docs/BUILDING.md) | Toolchain, make targets, tests, device bench, packaging, release builds |
| [Architecture](docs/ARCHITECTURE.md) | Source layout, the signal path, threads and real-time rules, saved state |
| [Performance](docs/PERFORMANCE.md) | CPU budget, measurements, the DSP's measured quality |
| [Roadmap](docs/ROADMAP.md) | What's done, what's next, decisions |
| [Changelog](CHANGELOG.md) | What changed in each release |

## Requirements

- An **Akai Force**. Other first-generation (32-bit ARM) MPC OS devices may work but are untested.
- **Root SSH access** to the device (for example through MockbaMod). Stock MPC OS has no way to
  install third-party plugins.
- **MPC OS 3.x** (tested on 3.9). Release builds use glibc symbols up to 2.27 only, but they also
  need GCC 11's libstdc++ (`GLIBCXX_3.4.29`), and MPC OS 2.x doesn't draw third-party plugin pages:
  2.x is untested. A local build with a newer cross toolchain needs glibc 2.38 (3.x only; see
  [Building](docs/BUILDING.md#release-builds)).

## Installation

Download a release package (`SubForce-<version>-mpc-armv7.zip`) from
[Releases](https://github.com/Devko/SubForce/releases), or build one with `make plugin-package`
(see [Building](docs/BUILDING.md)). Unzip it and follow the `INSTALL.md` inside. In short:

```sh
scp -r SubForce-<version> root@<device-ip>:/tmp/
ssh -t root@<device-ip> sh /tmp/SubForce-<version>/install.sh
```

The installer asks for confirmation (`-y` skips it), **stops MPC** (save your project first),
copies the plugin to `/sdcard/Synths/Devko - VST - SubForce/`, backs up and edits `MPC.settings`,
and starts MPC again. Running it again upgrades in place and keeps your own presets (in
`Presets/User/` inside that folder) and favorites. Then add **SubForce** to a track from MPC's
instrument plugins.

To uninstall, run the package's `uninstall.sh` the same way: it stops MPC, removes the plugin and
its `MPC.settings` entry (after a backup) and starts MPC again; your own presets and favorites stay
in the plugin folder (delete it to remove them too).

## Building from source

On Linux or WSL (developed on Ubuntu 24.04):

```sh
make test            # the full test suite under ASan/UBSan
make arm-plugin      # build/arm/subforce.so for the device
make plugin-package  # dist/SubForce-<version>-mpc-armv7.zip
```

Release packages come from CI (glibc 2.31, profile-guided, checked with the plugin catalog's own
checker): see [Building](docs/BUILDING.md#release-builds).

## Status

| Stage | |
|---|---|
| Phases 0 and 1 — engine, plugin, pages, 57 presets, tests, package, the original's behaviour | ✅ |
| Release build in CI (glibc 2.31, profile-guided, catalog-checked) | ✅ |
| On the device: installs, loads and benches (`make bench-device`) | ✅ |
| 0.0.1 — first public preview (the plugin catalog's beta channel) | 🔜 |
| On the device: play every page | 🔜 |
| v0.1 — parameter list frozen (append-only from then on) | ⬜ |

Details in the [roadmap](docs/ROADMAP.md).

## License

SubForce is released under the [MIT License](LICENSE). Third-party components keep their own
licenses (below).

## Credits

- Plugin groundwork (VST2 glue, touchscreen logic, preset library, build and bench tooling):
  [PolyForce](https://github.com/Devko/PolyForce), MIT.
- The ladder's cheap nonlinear zero-delay solution: Teemu "mystran" Voipio's
  "cheap non-linear zero-delay filters" (KVR forum, 2012). The decimator's structure and
  coefficient formulas: Laurent de Soras's HIIR (WTFPL). Pink noise: Paul Kellet's "economy"
  filter.
- Skin generator, previews and installer:
  [sd88me/mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins) (MIT), vendored in
  `third_party/mpc-vst-plugins` with a few small, marked patches.
- Interface font: [Titillium Web](https://fonts.google.com/specimen/Titillium+Web), SIL Open
  Font License 1.1 (`surface/fonts/OFL.txt`).

SubForce is an independent project, not affiliated with or endorsed by Moog Music, Akai
Professional / inMusic or Steinberg, nor by any artist named in its documentation. Akai, Force and
MPC are trademarks of inMusic Brands; VST is a trademark of Steinberg Media Technologies GmbH.
