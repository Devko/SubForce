# Roadmap

Status: ✅ done · 🔜 next · ⬜ planned · 💤 deferred

- [What's next](#whats-next)
- [Phase 0](#phase-0)
- [Planned](#planned)
- [Deferred and not planned](#deferred-and-not-planned)
- [Decisions](#decisions)

---

## What's next

- 🔜 **On the device:** install (`make plugin-install`), play every page, check the Q-Link sets and
  the preset browser; `make bench-device`, and add the numbers to
  [Performance](PERFORMANCE.md#measurements).
- 🔜 **Automation on the device:** whether MPC plays recorded automation of the stepped controls
  (octaves, slopes, modes) back through `setParameter`, and from which thread; the stepping logic
  treats events under 300 ms apart as one turn.
- 🔜 **Listening pass** on real speakers: every factory preset, the filter's drive and resonance
  range, the feedback's character, glide feel; tune voicing constants (`dsp/synth.cpp`: input gain,
  drive span, feedback gain, output gain) and the presets from it.
- ⬜ **v0.1**, the first release: parameter list frozen (append-only from then on).

## Phase 0

✅ (2026-10-05)

- ✅ Engine: two morphing polyBLEP oscillators (triangle → saw → square → 6% pulse), 32'–2', hard sync,
  square sub (−1 / −2 oct), noise with colour, keyboard reset, drift
- ✅ Mixer with feedback (post-VCA, DC-blocked) and the hot-mixer overdrive
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
- ✅ 27 factory presets in 5 categories, level-matched at −18 LUFS
- ✅ Test suite (360 checks, ASan/UBSan, and under `qemu-arm`), bench, profile-guided device build,
  release package, demo renders
- ✅ Review (DSP, plugin, build / tests / docs, in parallel): about 40 confirmed findings fixed, each
  with a check — among them pulse edges a moving width swept past, a sounding key struck again,
  glides and bus retriggers starting late inside a control step, the preset list never re-read,
  PREV/NEXT reloading at the ends, Rand Amount saved with sounds, user preset numbers reused, a
  stale value pushed back to MPC during a preset load, UBSan never failing the build, template
  symbols exported from the `.so`

## Planned

- ⬜ **Audio-rate bus sources**: oscillator 2 and noise as mod sources (FM to pitch and cutoff).
- ⬜ **Quality switch**: 4x oversampling, if the device bench leaves room.
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
- 2026-10-05 — **The ladder's bass loss kept** (no compensation), as on the hardware.
- 2026-10-05 — **Factory presets at −18 LUFS** on their demo phrase, peaks under −3 dBFS.
- Moog, Subsequent and other product names are not used in the plugin, its presets or its pages.
