# Changelog

Releases are built by CI from a `vX.Y.Z` tag (see [Building](docs/BUILDING.md#release-builds)); the
section for the tag's version becomes the release's notes. While the version is 0.x the parameter list
may still change between releases.

## 0.0.7

**Multidrive** calibrated against recordings of the original's factory presets: decoded knob
positions played on the recordings' notes, and the engine's constants fitted to them. At full the
original is grittier than SubForce was: the second stage (after the ladder) now clips harder (its
gain at full 2.0, was 1.5) and the drive into the ladder goes to 9×, was 8×. About half of what the
fit asked for, as only two sounds settled it. Below about a third of the knob the change is slight.
The factory presets are re-levelled (−16 LUFS, most within ±0.3 dB).

For comparisons like it, `tools/demos` renders preset files from anywhere (`--files`), a preset on a
note list (`--notes`) and jobs on stdin (`--serve`); `build/demos_tune` makes the engine's tuning
constants settable for fits (see [Building](docs/BUILDING.md#make-targets)).

## 0.0.6

Fixes from a review of 0.0.3–0.0.5 (engine, plugin, tests and docs):

- A bus with **Velocity** (or another note-time source) on **Glide Time** or **EG Time** used the
  previous note's value; now the note's own (Key: the key played, not the one it glides from).
- **EG Sync**: a sequenced note a hair before a sync unit no longer restarts a few samples in (a
  click with Reset on); MPC starting or stopping re-bases the count instead of restarting.
- MPC's position ran a few samples ahead after every block whose start wasn't on the 8-sample control
  grid (bar-locked busses and EG Sync); now exact.
- A **latched** envelope now lets go when MPC stops and on all notes off (CC 123), not only on all
  sound off.
- **Duo with a droning osc 2** plays as Mono: a second key no longer retriggers.
- **Pitch Bend** is a bus source: with Bend To *Off* the wheel does what a bus makes of it (the guide
  promised that; there was no such source).
- The bus **rate text** updates at once when its rate mode changes (Hi shows ×10).
- **Units**: a duplicated track no longer shares its original's unit (no two live instances are the
  same unit); a project saved before units is unit 1, the same on every load; the unit reaches the
  engine together with the values it came with.
- The programmable destinations stay within their documented ranges with two busses or the Key
  source on them; per-note Analog offsets follow the knob (0 is ideal at once, not from the next
  note); EG Time moving every step no longer calls libm; a zero rate in a hand-built patch can't make
  NaNs; reset() between control steps no longer counts samples twice.
- DEPTH has its own Control selector (MOD's list left open showed open there too).
- The device trace marks state saves and loads (`getChunk`, `setChunk`), to see whether MPC restores
  parameters after a project's state.
- Analog at 0 is 0.0.4's sound bit for bit on the x86 build and within float rounding (−120 dB) on
  the device's fast-math build (0.0.5's notes said "sample for sample" for both).
- Tests: 640 (from 596), tightened where a check could pass with its feature broken.

## 0.0.5

- **Analog** (the knob that was Drift): besides the drift, now everything that makes an analog unit
  sound like one, all of it scaled by the knob and kept small — cycle-to-cycle **jitter**, a faster
  drift, slightly **imperfect shapes** (the saw's bow, the square a little off 50%, soft edges, the
  mixer's AC coupling), **per-note** variation of shape, cutoff and resonance (as many "analog"
  preset packs do), and the **unit**: every instance its own tuning error, square width, cutoff,
  resonance and envelope times, kept by a project. At 0 the sound is 0.0.4's, sample for sample.
- Factory presets re-levelled (49 of them by 0.1–0.3 dB).
- CPU on the Force: +0.2% of a block with Analog up (Init 2.85%, the heavy patch 3.7%).

## 0.0.4

The rest of the original's manual:

- **Key Track for the busses** (0–200%, the original's LFO KBTRACK): the rate follows the key. In Hi
  range at 100% the FM follows the keys too and stays harmonic.
- **Envelopes**: an **exponential attack** (the original's EXP ATTACK, an analog RC curve), **Latch**
  (the envelope stays as if the key were held: drones, held filter sweeps) and **Sync** (restarts
  every note value of MPC's tempo while held: gates, rhythmic plucks).
- **Osc 2 Keys** (the original's KB CTRL): in Duo, oscillator 2 on the highest or the lowest key;
  Mono or Duo, a **drone** that follows no key (its Freq knob then ±3 octaves).
- **Gated glide** (Glide: *Gated*, *Legato Gated*): the glide moves only while a key is held.
- **Bend To**: which oscillators the pitch bend moves (both, osc 1, osc 2, off).
- **3 presets** that show them: Drone Lead, Harmonic FM, Trance Gate (74 in all).
- Key tracking still pivots on MIDI 60 (MPC's C3); the original's is MIDI 48 (its C3). Moving it
  would brighten every saved sound by its Key Track times an octave, for no other change.
- CPU on the Force: unchanged (Init 2.7%, the heavy patch 3.5% of a block on average).

## 0.0.3

Closer to the original's modulation, from its manual, and 14 more presets:

- **Beat Freq** (OSC tab): oscillator 2 detuned by up to ±3.5 Hz, so it beats at the same rate on
  every note, as the original's BEAT FREQ.
- **Hi range** for the mod busses (the Rate Mode is now Free, Sync or **Hi**): 0.5–1000 Hz, worked
  out every sample on pitch, cutoff, wave and volume — audio-rate FM, filter FM and ring modulation,
  as the original's HI RANGE.
- **Seven more sources**: Sine, Noise (random, the rate its speed), Amp EG, Velocity, Aftertouch, Key
  and Constant (the original's programmable sources).
- **Nine more destinations**: EG Amount, Key Track, Osc 1 Level, Osc 2 Level, Beat Freq, EG Time
  (both envelopes), FEG Time, AEG Time, Glide Time.
- **DEPTH tab**: how much the mod wheel, velocity and pressure add to each bus's depth (−100%..+100%,
  the original's MOD WHEEL / VELOCITY / AFTERTOUCH amounts), and a fifth Control, *None*.
- **3 presets** that show them: Beating Bass, Velocity Growl, Ring Lead (71 in all).
- Block sizes no longer change the free-running oscillators' phase across a silence (it differed by
  about 10⁻⁵ of a cycle, inaudible): the sound is now the same sample for sample however MPC splits
  the audio.
- The new parameters are added at the end of the list and the new options at the end of theirs, so
  saved sounds and projects keep their settings and their Q-Link assignments.
- CPU on the Force: unchanged for the existing sounds; both busses in Hi range on top of the heavy
  patch, 4.6% of a block (p99).

- **11 more factory presets** in the direction of KVNDRA's melodic house and ambient
  sound: Deep House Bass, Wah Bass, Dark Pulse Bass, Warm Glow Lead, Rising Lead, Pulsing Lead,
  Melancholy Arp, Slow Analog Arp, Endless Steps, Desert Drone, Cinematic Swell.

## 0.0.2

The first regular release: the plugin catalog lists it with a download button and its installers offer
it (0.0.1 was a prerelease, its beta channel, which neither does by default).

- **2 dB louder**, level with MPC's own instruments and with PolyForce: the factory presets play at
  −16 LUFS on their demo phrases (was −18), no peak over −1 dBFS (the highest −1.8 dBFS); the
  engine's output is 2 dB up, Init and saved sounds too.
- Release builds: a plain `vX.Y.Z` tag publishes a regular release, a suffixed one (`v0.1.0-beta`) a
  prerelease.

## 0.0.1

The first public preview.

- **Engine:** two morphing oscillators (triangle → saw → square → narrow pulse), 32' to 2', hard sync,
  a square sub (−1 or −2 octaves), noise from white through pink to dark, analog drift. A mixer with
  its own feedback loop (thicker, then gritty, then the chaos of an overdriven loop). A nonlinear
  4-pole transistor ladder (6/12/18/24 dB) that self-oscillates past 70% of the resonance knob and
  thins the bass as it rises, with Multidrive (asymmetric, tube-like warmth to hard clipping). Two
  DAHDSR envelopes with a linear attack, velocity, key tracking, reset and a loop through the
  release. Two mod busses (7 sources, free or locked to MPC's tempo; pitch, cutoff and one of 10
  more destinations; mod wheel, aftertouch or velocity). Mono or Duo, note priority, multi or
  single trigger, glide (rate, time or exponential). The whole voice at 2× with a halfband
  decimator.
- **57 factory presets** in seven categories (Templates, Bass, Lead, Keys, FX, Sequence, Pad), many
  for melodic techno; all level-matched. User presets, favorites, a browser, Init and Randomize.
- **Touchscreen pages** (OSC, FILTER, AMP, MOD, BROWSE, KEYS) with a Q-Link set each.
- **On the Force:** about 2.8% of a block for one held note (3.6% with everything on), measured on
  the device; the ladder runs on NEON.
- **Builds:** armhf against glibc 2.31 (loads on MPC OS 2.x and 3.x; the pages need 3.x),
  profile-guided, checked with the plugin catalog's `catalog_check.py`.
