# User guide

- [The idea](#the-idea)
- [The screen](#the-screen)
- [The sound engine](#the-sound-engine)
- [Keys, glide and Duo](#keys-glide-and-duo)
- [The mod busses](#the-mod-busses)
- [Presets](#presets)
- [MIDI](#midi)

---

## The idea

One voice, played like an analog monosynth: two oscillators and a sub into a mixer, a 4-pole
transistor ladder, two envelopes and two mod busses, with the panel's habits kept — push the mixer
and the filter overdrives, turn the resonance up and the bass thins out, take it past 90% and the
filter sings on its own.

## The screen

Six tabs. Every tab has the status line at the top: `VOICES 1   CPU 4%   PEAK 6%` (the CPU the
plugin used in the last half second, of what one track may use).

| Tab | What's on it |
|---|---|
| **OSC** | Oscillator 1 (octave, wave, sub octave), oscillator 2 (octave, frequency, wave, hard sync), the mixer (osc 1, sub, osc 2, noise, feedback, noise colour, drift, keyboard reset) |
| **FILTER** | Cutoff, resonance, Multidrive, filter EG amount, key track, the filter EG's velocity and key tracking, slope; the filter EG |
| **AMP** | The amp EG; velocity, key tracking and volume |
| **MOD** | Mod 1 and mod 2: source, sync, sync rate, control, trigger; rate, pitch amount and where it goes, filter amount, the third destination and its amount |
| **BROWSE** | Preset categories and presets; favorite, random, save, init |
| **KEYS** | Mono / Duo, note priority, trigger; bend ranges; glide; the preset stepper, save, init, randomize |

Every tab has a Q-Link set named after what it controls (`OSC + MIX`, `FILTER`, `AMP`, `MOD 1+2`,
`BROWSE`, `KEYS`). Stepped controls (octaves, slopes, modes, the preset stepper) move exactly one
step per Q-Link detent or data-wheel click.

## The sound engine

### Oscillators

- **Octave**: 32', 16', 8', 4', 2' (8' plays the key's pitch).
- **Wave**: a continuous sweep from triangle (0) through saw, square, down to a 6% pulse. In between
  the shapes cross-fade; past square the pulse narrows. The value shows the shape: `Saw`,
  `Saw-Sqr 40%`, `Pulse 23%`.
- **Osc 2 Freq**: ±7 semitones against oscillator 1, for beating, intervals, or sync sweeps.
- **Hard Sync**: oscillator 2 restarts its cycle with oscillator 1's; move osc 2's frequency (by
  hand, or with a bus) for the tearing sync sound. Osc 1 can be silent in the mixer and still lead.
- **Sub Octave**: the sub oscillator is a square one or two octaves under oscillator 1.
- **KB Reset**: On, every new note starts the oscillators at the beginning of their cycle (the same
  punch every time); Off, they run free, as analog oscillators do.
- **Drift**: slow random pitch movement of each oscillator (up to ±6 cents) and of the cutoff,
  plus a small offset per note. 0 is perfectly stable.

All oscillators are band-limited (polyBLEP) and run at twice the sample rate.

### Mixer

Osc 1, Sub, Osc 2, Noise and **Feedback** levels. The knobs have an audio taper. Several sources
up high drive the filter harder — on purpose. **Feedback** sends the synth's own output (after the
VCA) back into the mixer: a little thickens and growls, a lot with resonance howls. **Noise
Colour** darkens the noise from white.

### Filter

A 4-pole transistor ladder, modelled with its nonlinearities.

- **Cutoff** 20 Hz–20 kHz. **Resonance**: from about 90% the filter oscillates by itself, a sine at
  the cutoff (play it with Key Track at 100%; it tracks the keyboard, a little flat at the top of the
  resonance, like the circuit). Resonance also thins the bass, as on the hardware.
- **Multidrive**: drives the ladder and a second stage after it; from warm to fuzzy. Louder too,
  but by a few dB, not a jump.
- **Slope**: 6, 12, 18 or 24 dB per octave (the outputs of the ladder's four stages).
- **EG Amount**: the filter EG's sweep, up to ±10 octaves (the value shows octaves).
- **Key Track**: 0–200%; 100% makes the cutoff follow the keyboard exactly (around middle C).
- **FEG Velocity**: how much velocity scales the filter EG (and so the sweep).
- **FEG Key Track**: higher notes, shorter filter EG times.

### Envelopes

Both are **DAHDSR**: Delay, Attack, Hold, Decay, Sustain, Release (attack to 10 s, the others to
10 s, delay and hold from 0). The curves are an analog EG's: attack charges like a capacitor aiming
past the top (toward 1.2) and stops at 1, the convex rise of a real attack; decay and release fall
exponentially (the times are to −60 dB).

- **Loop**: while the key is held the envelope cycles delay → attack → hold → decay.
- **Reset**: a new note's attack starts from 0. Off, it starts from wherever the envelope is (smooth
  legato, the analog way).
- **Velocity** and **Key Track** as above; the amp EG's are on the AMP tab.

## Keys, glide and Duo

- **Mode**: *Mono*, or *Duo*: oscillator 1 plays one key and oscillator 2 another, through the one
  filter and VCA (paraphonic). With one key down, both play it.
- **Priority**: which key sounds when several are down — *Last*, *Low* or *High*. Releasing a key
  returns to the next one by the same rule. In Duo, oscillator 1 takes the first by priority,
  oscillator 2 the second.
- **Trigger**: *Multi* restarts the envelopes on every key struck, a sounding key struck again too
  (held by the pedal, or repeated); releasing a key hands the oscillators back to one still held
  without a new attack. *Single* only when the gate was closed (legato phrases glide on one
  envelope).
- **Glide**: *Off*, *Always*, or *Legato* (only between overlapping keys). **Type**: *Rate* (the
  time per octave: big leaps take longer), *Time* (every glide takes the same time), *Exp*
  (exponential, fast then slow, like an RC). **Osc**: which oscillators glide.
- **Bend Up / Down**: the pitch bend range, 0–24 semitones each way.
- The sustain pedal keeps the last note sounding until it lifts. While it holds the gate open, the
  next key plays legato, as on the hardware: Single doesn't restart the envelopes and Legato glide
  glides.

## The mod busses

Two identical busses. Each takes one **source** and sends it to three places at once:

| | |
|---|---|
| **Source** | Triangle, Square, Saw (falling), Ramp (rising), S&H (a new random value each cycle), Smooth (gliding random), Filter EG |
| **Rate** / **Sync** / **Sync Rate** | 0.05–100 Hz free, or a note value of MPC's tempo (8 bars to 1/32T) |
| **Trigger** | *Free*: runs on its own (a synced bus locks to the bar while MPC plays, unless the other bus is modulating its rate); *Retrig*: restarts at every new note |
| **Pitch** + **Pitch To** | pitch amount (up to ±24 st, fine near 0) for osc 1+2, osc 1 or osc 2 |
| **Filter** | cutoff amount (up to ±5 octaves) |
| **Destination** + **Amount** | one more target: Wave 1+2, Wave 1, Wave 2, Resonance, Multidrive, Sub, Noise, Feedback, Volume, or Other Rate (the other bus's rate, ×1/16..×16) |
| **Control** | what sets the depth: *Always*, the *Mod Wheel*, *Aftertouch* (channel pressure) or *Velocity* |

Mod 1 starts on the mod wheel, so its pitch amount gives vibrato under the wheel; mod 2 starts on
Always.

Recipes: **sync sweep** — source Filter EG, pitch to Osc 2, Hard Sync on. **Laser** — Filter EG to
pitch at a large amount. **Wobble** — a synced triangle on the filter, Retrig. **PWM** — a slow
triangle to Wave 1 with the wave near square.

## Presets

27 factory presets in five categories (Templates, Bass, Lead, Keys, FX), all level-matched (−18 LUFS
on their demo phrase). Pick them on the BROWSE tab or step through them on KEYS.

- **SAVE** writes `User NNN.sfp` to `<plugin folder>/Presets/User/` (there is no text entry on the
  device, so presets are numbered; a number is never used twice). Rename them on a computer; the
  name shows in the browser. Files added, renamed or deleted while MPC runs show up when you browse
  (or in a new instance).
- **INIT** loads Init: one saw through a half-open filter.
- **RANDOM** moves the sound toward a random one by **Rand Amount**: oscillators, mixer, filter and
  the envelopes' main stages. Volume, the keyboard, glide and the busses stay. Rand Amount itself is
  a setting of the page, not part of a sound: presets don't save or change it.
- Presets on the SSD: `/media/AkaiForce/SubForce Presets/` (any folders inside become categories).
- A preset file is plain text (`subforce 1` and `key=value` lines of real values), the same as an MPC
  project stores.

## MIDI

| Message | Does |
|---|---|
| Note on / off | play (velocity per the EG velocity settings) |
| Pitch bend | ± the bend ranges |
| CC 1 (mod wheel) | a bus's depth, if its control is Mod Wheel |
| Channel pressure | a bus's depth, if its control is Aftertouch |
| Poly aftertouch | the same, from the sounding key's own pressure (what MPC's pads send) |
| CC 64 | sustain pedal (re-striking a held key retriggers in Multi) |
| | A key is down or up: two note-ons for the same key and then one note-off end it, as on a keyboard |
| CC 120 / 121 / 123 | all sound off / reset all controllers (bend, wheel, pressure, pedal) / all notes off |
