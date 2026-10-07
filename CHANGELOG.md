# Changelog

Releases are built by CI from a `vX.Y.Z` tag (see [Building](docs/BUILDING.md#release-builds)); the
section for the tag's version becomes the release's notes. While the version is 0.x the parameter list
may still change between releases.

## Unreleased

- **11 more factory presets** (68 in all), in the direction of KVNDRA's melodic house and ambient
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
