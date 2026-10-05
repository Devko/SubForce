# Performance and sound quality

- [The budget](#the-budget)
- [Measurements](#measurements)
- [Why it is cheap](#why-it-is-cheap)
- [Measured sound quality](#measured-sound-quality)
- [Considered and left out](#considered-and-left-out)

---

## The budget

MPC renders 128-frame blocks: **2902 µs per block**, per plugin instance. A plugin passes at
**p99 ≤ 15%** of the block (warns up to 35%), the rule PolyForce uses.

SubForce is one voice, so its cost hardly depends on the patch: a single sine and everything at
full (both oscillators, sub, noise, feedback, sync, full Multidrive, Duo) run the same loop.

## Measurements

`make bench`, x86 (only to prove the bench works; the Force is far slower per sample):

| Case | avg | p99 | max |
|---|---|---|---|
| idle (no note) | 0.05% | 0.05% | 0.2% |
| Init, one held note | 0.5% | 0.8% | 1.4% |
| heavy patch, Duo | 0.5% | 0.7% | 0.7% |
| heavy, retrig + glide every 50 ms | 0.5% | 0.7% | 1.2% |

The profiling build puts about 15 µs per block in the voice and 1.5 µs in the control steps.

**On the device: to be measured** (`make bench-device`). The estimate from PolyForce's numbers (one
PolyForce voice, about 45 k ARM instructions per block, measured about 1% of a block) is a few
percent for SubForce's one voice at 2x.

## Why it is cheap

- **One voice.** Everything PolyForce spends on voice management and vectorising across voices
  isn't needed.
- **Idle costs nothing:** when the amp EG has finished and the output died away, the engine stops
  rendering until the next note.
- **Control rate:** modulation and glide every 8 samples, gliding in between; only the envelopes and
  the cutoff they drive are computed per sample (that is what makes a 1 ms filter EG snap).
- **Short polynomials** instead of libm for `exp2`, `tan` and `tanh(x)/x`; a polyphase IIR
  decimator (8 multiplies per output sample) instead of a long FIR.
- **Profile-guided** device build, trained under `qemu-arm` on a spread of patches and the factory
  presets.

## Measured sound quality

From the test suite (`make test` prints these):

| What | Measured |
|---|---|
| Worst alias, any wave shape, C4..C7 | −53 dB against the strongest partial (triangle −93 dB, saw −64..−76 dB, narrowest pulse the worst) |
| Worst alias, hard sync | −65 dB |
| Decimator | passband flat to 20 kHz within 0.001 dB, stopband from 24.2 kHz at −85 dB |
| Self-oscillation | from about 90% resonance; tracks the cutoff 15–50 cents flat (110 Hz–7 kHz), as a ladder does |
| Slopes | 5.4 / 10.9 / 16.3 / 21.7 dB per octave measured an octave above a 400 Hz cutoff (6/12/18/24 nominal) |
| Bass loss | 85% resonance: the passband drops by more than 8 dB, the ladder's 1 / (1 + r) |
| Multidrive | 0 → 100%: +4 dB louder, much denser |

## Considered and left out

- **4x oversampling.** 2x with polyBLEP oscillators already puts aliasing far below the signal;
  4x would double the voice's cost for the ladder's own nonlinear harmonics, which the halfband
  removes above 24 kHz anyway. A quality switch can come later if the device bench leaves room.
- **Audio-rate modulation** (oscillator 2 or noise as a bus source for FM): needs the cutoff and
  pitch computed per high-rate sample. Possible, at a cost; not in Phase 0.
- **Compensating the bass loss.** The ladder's thinning with resonance is kept, as on the
  hardware; Multidrive and the mixer make up for it.
