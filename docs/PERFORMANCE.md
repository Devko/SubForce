# Performance and sound quality

- [The budget](#the-budget)
- [Measurements](#measurements)
- [Why it is cheap](#why-it-is-cheap)
- [Measured sound quality](#measured-sound-quality)
- [Considered and left out](#considered-and-left-out)

---

## The budget

MPC renders 128-frame blocks: **2902 µs per block**, per plugin instance. A plugin passes at
**p99 ≤ 15%** and **max ≤ 50%** of the block (warns up to 35% / 80%), the rule PolyForce uses.

SubForce is one voice, so its cost depends little on the patch: Init (one saw) and everything at
full differ by under 1% of the block; only noise and the feedback loop add work, and only while
they are up.

## Measurements

`make bench-device` on the Force (Cortex-A17 at 1.8 GHz, MPC OS 3.9, MPC running; 2026-10-05),
percent of the 2902 µs block, the profile-guided build:

| Case | Phase 0 avg / p99 | 0.0.1 avg / p99 |
|---|---|---|
| idle (no note) | 0.38% / 0.62% | 0.17% / 0.36% |
| Init, one held note | 3.64% / 4.09% | 2.79% / 3.31% |
| heavy patch, Duo (noise and feedback on) | 3.70% / 4.19% | 3.55% / 4.07% |
| heavy, retrig + glide every 50 ms | 3.75% / 4.28% | 3.60% / 4.15% |

The heavy patch now runs the original's feedback loop (the mixer's output back into it), which is
serial by nature; Init leaves noise and feedback off and pays for neither.

The profiling build puts about 73 µs per block in the voice (Init) and 8–17 µs in the control
steps (it reads the clock between stages, so it reads higher than the plain build).

`make bench` (x86) only proves the bench works; the Force is far slower per sample.

## Why it is cheap

- **One voice.** Everything PolyForce spends on voice management and vectorising across voices
  isn't needed.
- **No divisions waiting on each other.** VFP's divider takes ~18 cycles and accepts one every
  ~14 (measured on the device); the Phase 0 voice queued 14 of them per sample. Now:
  - **The ladder** (`dsp/ladder.h`, 162 → 103 ns a tick): the four stages' nonlinear gains and
    their 1 / (1 + f t) are four NEON lanes with reciprocal estimates refined by Newton-Raphson; the
    loop is solved as an affine chain (each stage's output a + b · y3) and closed with one
    division; inputs, outputs and integrators are one vector step each.
  - **The oscillators** take each stretch's length instead of dividing for it; the two divisions
    left (at a wrap, at a pulse edge) are reciprocals.
  - **The cutoff coefficients** (`exp2` and `tan`) are worked out four at a time for each control
    run, ahead of the audio.
  - Measured on this core: a NEON q-register op issues every 2 cycles (a 64-bit datapath), so the
    serial parts stay in VFP; an all-NEON ladder (a prefix scan) was slower.
- **Idle costs next to nothing:** when the amp EG has finished and the output died away, the engine
  stops rendering, and the control grid only keeps time (glides, busses, drift) until a note wakes
  it; block sizes still never change the sound.
- **Control rate:** modulation and glide every 8 samples, gliding in between; no libm calls
  (`log2Fast`, `exp2Fast`, the cutoff note and the fixed coefficients worked out ahead),
  multiplications instead of divisions.
- **Pay for what is on:** the noise filters (pink, dark, the 30 Hz high-pass) and the feedback loop
  only run while the noise or feedback level is up.
- **Short polynomials** instead of libm for `exp2`, `log2`, `tan` and `tanh(x)/x`; a polyphase IIR
  decimator (8 multiplies per output sample) instead of a long FIR.
- **Profile-guided** device build, trained under `qemu-arm` on a spread of patches and the factory
  presets.

## Measured sound quality

From the test suite (`make test` prints these):

| What | Measured |
|---|---|
| Worst alias, any wave shape, C4..C7 | −53 dB against the strongest partial (the narrowest pulse the worst) |
| Pulse width swept by a bus | every edge corrected: the largest step between two samples is 1.5 (an uncorrected edge is 2) |
| Worst alias, hard sync | −65 dB |
| Decimator | passband flat to 20 kHz within 0.001 dB, stopband from 24.2 kHz at −85 dB |
| Self-oscillation | past 70% of the knob, as the original's (65%: none; 76%: rms 0.07); tracks the cutoff 15–50 cents flat (110 Hz–7 kHz), as a ladder does |
| Slopes | 5.4 / 10.9 / 16.3 / 21.7 dB per octave between 880 Hz and 1.76 kHz with a 400 Hz cutoff (6/12/18/24 nominal, reached further up) |
| Bass loss | 65% resonance (just under the edge): the passband drops by 13 dB, the ladder's 1 / (1 + r) |
| Multidrive | 0 → 100%: +4 dB louder, much denser; at 50% a triangle's 2nd harmonic at −20 dB (asymmetric, tube-like), none clean |
| Feedback | 0 → 100%: +6 dB, the 9th harmonic +9 dB against the fundamental; no motorboating (an octave under the note: −101 dB) |
| Noise colour | white, pink (−10 dB a decade, Paul Kellet's filter), dark: within 0.3 dB of each other through the open filter |

## Considered and left out

- **4x oversampling.** 2x with polyBLEP oscillators already puts aliasing far below the signal;
  4x would double the voice's cost for the ladder's own nonlinear harmonics, which the halfband
  removes above 24 kHz anyway. The device bench leaves room (3.6% of a block with everything on);
  a quality switch can come if listening shows a need.
- **Audio-rate modulation** (oscillator 2 or noise as a bus source for FM): needs the cutoff and
  pitch computed per high-rate sample. Possible, at a cost; not in 0.0.1.
- **Compensating the bass loss.** The ladder's thinning with resonance is kept, as on the
  original; Multidrive and the mixer make up for it.
- **Hand-written assembly for the ladder.** GCC moves some vector lanes through core registers; a
  hand-scheduled ladder might save another ~30 ns a tick (~8% of the voice). Not worth giving up
  the one C++ source the x86 tests also run.
