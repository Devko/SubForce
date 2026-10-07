# Roadmap

Status: ✅ done · 🔜 next · ⬜ planned · 💤 deferred

- [What's next](#whats-next)
- [Phase 2](#phase-2)
- [Phase 1](#phase-1)
- [Phase 0](#phase-0)
- [Planned](#planned)
- [Deferred and not planned](#deferred-and-not-planned)
- [Decisions](#decisions)

---

## What's next

- 🔜 **The plugin catalog** ([sd88me/mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins)): the
  entry's PR, then every release is picked up by itself.
- 🔜 **On the device:** play every page, check the Q-Link sets and the preset browser (installs,
  loads and benches: [Performance](PERFORMANCE.md#measurements)).
- 🔜 **Automation on the device:** whether MPC plays recorded automation of the stepped controls
  (octaves, slopes, modes) back through `setParameter`, and from which thread; the stepping logic
  treats events under 300 ms apart as one turn.
- 🔜 **Listening pass** on real speakers, against the original if one is at hand: every factory
  preset, the new feedback loop's range, Multidrive's asymmetry, the linear attack; tune voicing
  constants (`dsp/synth.cpp`: input gain, drive span and bias, feedback gain and clip, output gain)
  and the presets from it.
- ⬜ **v0.1**: parameter list frozen (append-only from then on).

## Phase 2

✅ (2026-10-07)

- ✅ 11 more presets in the direction of KVNDRA's melodic house / ambient sound (68 in all)
- ✅ Closer to the original's modulation (0.0.3): Beat Freq; Hi range (audio-rate busses, up to
  1 kHz, every sample on pitch, cutoff, wave and volume); Sine, Noise, Amp EG, Velocity, Aftertouch,
  Key and Constant sources; EG Amount, Key Track, the oscillator levels, Beat Freq, EG / FEG / AEG
  Time and Glide Time destinations; wheel, velocity and pressure depth amounts (a DEPTH tab); 3
  presets that show them (71 in all)
- ✅ The rest of the original's manual (0.0.4): LFO key tracking (0–200%), Duo's KB CTRL (oscillator
  2 on the high or low key, or a drone), gated glide, an exponential attack option, envelope latch
  and tempo sync, the bend per oscillator; 3 presets that show them (74 in all)
- ✅ Analog (0.0.5, was Drift): jitter, a faster drift, imperfect shapes, per-note and per-unit
  variation; 0 = the ideal synth (0.0.4's sound: bit for bit on x86, within float rounding,
  −120 dB, on the device build)
- ✅ Review of 0.0.3–0.0.5 (engine, plugin, tests and docs, in parallel): a note's velocity on
  Glide / EG Time one note late, the song position a few samples ahead after each block, EG Sync
  restarting just after a sequenced note, a latched drone surviving MPC's stop, Duo with a drone
  retriggering, the rate text not refreshing in Hi range, a duplicated track sharing its unit, a
  missing pitch-bend source; and the tests tightened where they could pass with a feature broken

## Phase 1

✅ (2026-10-05)

- ✅ Release build in CI: armhf in `arm32v7/gcc:11-bullseye` (glibc 2.31), profile-guided, the ARM
  suite against the shipped objects, catalog-checked; releases from `vX.Y.Z` tags
- ✅ Performance on the Force: NEON ladder (four stages in vector lanes, the loop as an affine
  chain), no divisions in the oscillators, four-lane cutoff coefficients, cheaper control and idle:
  Init 3.64% → 2.79% of the block, idle 0.38% → 0.17%
- ✅ Fixes: buttons and the preset stepper on the Force (PolyForce's device-run rules), Multi
  trigger on key releases, poly aftertouch, CC 121, flush-to-zero scope, per-instance random seeds,
  device diagnostics (`/tmp/subforce.trace`)
- ✅ The original's behaviour, from its manuals: the mixer's own feedback loop, resonance self-oscillating
  past 70%, asymmetric Multidrive, linear attack, loop through the release, pink noise
- ✅ 30 more factory presets (57 in 7 categories), many for melodic techno
- ✅ Releases: 0.0.1 (a prerelease), 0.0.2 (the first regular one, 2 dB louder: −16 LUFS); Phase 2's
  0.0.3 (the original's modulation, 71 presets), 0.0.4 (the rest of its manual, 74 presets), 0.0.5
  (Analog), 0.0.6 (the review's fixes)

## Phase 0

✅ (2026-10-05)

- ✅ Engine: two morphing polyBLEP oscillators (triangle → saw → square → 6% pulse), 32'–2', hard sync,
  square sub (−1 / −2 oct), noise with colour, keyboard reset, drift
- ✅ Mixer with feedback (post-VCA, DC-blocked; replaced in Phase 1 by the mixer's own loop) and the
  hot-mixer overdrive
- ✅ Nonlinear transistor ladder, zero-delay feedback, 6/12/18/24 dB taps, self-oscillation, bass loss,
  Multidrive (input gain + a second stage), key tracking to 200%
- ✅ 2x oversampled voice with a polyphase IIR halfband decimator
- ✅ Two DAHDSR envelopes: velocity, key tracking, loop, reset
- ✅ Two mod busses: 7 sources, free or synced (locked to the bar), pitch with destination, cutoff,
  one of 10 more destinations, mod wheel / pressure / velocity control, retrigger
- ✅ Mono and Duo, last / low / high priority, multi / single trigger, pedal; glide Rate / Time / Exp,
  Always / Legato, per oscillator
- ✅ Plugin from PolyForce's groundwork: VST2 glue, touchscreen logic and stepping, preset library
  and browser, favorites, user presets, randomize, state text, CPU meter
- ✅ Six touchscreen pages and Q-Link sets, an amber skin
- ✅ 27 factory presets in 5 categories, level-matched at −18 LUFS (57 in 7 since Phase 1)
- ✅ Test suite (360 checks then, 640 now; ASan/UBSan, and under `qemu-arm`), bench, profile-guided device build,
  release package, demo renders
- ✅ Review (DSP, plugin, build / tests / docs, in parallel): about 40 confirmed findings fixed, each
  with a check — among them pulse edges a moving width swept past, a sounding key struck again,
  glides and bus retriggers starting late inside a control step, the preset list never re-read,
  PREV/NEXT reloading at the ends, Rand Amount saved with sounds, user preset numbers reused, a
  stale value pushed back to MPC during a preset load, UBSan never failing the build, template
  symbols exported from the `.so`

## Planned

- ⬜ **Oscillator 2 as a bus source** (true oscillator FM, tracking the keys), if Hi range isn't
  enough.
- ⬜ **Quality switch**: 4x oversampling, if listening shows a need (the device bench leaves room).
- ⬜ **Arpeggiator / sequencer**: low priority — the Force sequences better than any plugin page can.
- ⬜ **More presets** after the listening pass.

## Deferred and not planned

| Feature | Why not (now) |
|---|---|
| Built-in effects | MPC's insert effects do the job |
| Polyphony | That is PolyForce |
| Microtuning | PolyForce has it; little call on a bass/lead monosynth so far |
| MPE | Untested whether MPC passes per-note channels to a VST2 |

## Decisions

- 2026-10-05 — **Own repository**, the plugin groundwork taken from PolyForce, the engine new.
- 2026-10-05 — **2x oversampling** for the whole voice (oscillators to VCA), polyBLEP oscillators.
- 2026-10-05 — **The ladder's bass loss kept** (no compensation), as on the original.
- 2026-10-05 — **Factory presets at −18 LUFS** on their demo phrase, peaks under −3 dBFS.
- 2026-10-05 — **−16 LUFS, peaks under −1 dBFS** (0.0.2): PolyForce at −19 played clearly quieter
  than MPC's own instruments on the Force; SubForce goes to PolyForce's new level. The engine's
  output is 2 dB up (the quietest presets' volume was at the knob's top already).
- 2026-10-05 — **Regular releases** from plain `vX.Y.Z` tags: the catalog hides prereleases (no
  download button) and its installers never offer them.
- 2026-10-07 — **Key tracking stays centred on MIDI 60** (MPC's C3), not the original's MIDI 48 (its
  C3): the knob means the same, an octave apart, and moving it would brighten every saved sound.
- Moog, Subsequent and other product names are not used in the plugin, its presets, its pages or
  its docs (beyond the trademark notice): the docs say *the original*.
