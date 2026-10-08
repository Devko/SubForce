# The demo video

A melodic-techno build in F minor made only with SubForce (bass sequence, pluck, gliding lead, pad),
each layer entering clean and then going through its own EffectForce; then all together, a breakdown
with a riser, a drop on an impact (both SubForce presets too) and an ending. EffectForce's EQ and glue
compressor sit on the master.

The parts are our own, written with what melodic-techno production tutorials teach:
- the bass on the chord roots, off the beat, with 1-5-octave jumps and velocity accents;
- a sparse arp that the delay completes;
- cutoffs that never stand still, several moves stacked at the drop;
- delays thrown at the ends of phrases.

The audio is rendered offline through the built plugins; the video shows their pages with the values
they reported.

- `render.cpp`: plays the track through the plugins as MPC does (128-frame blocks, MIDI with sample
  offsets, the transport running), level-matches each layer's effected half to its clean half (before
  the master bus), masters the mix (−10 LUFS, peaks under −1 dBFS). Writes `out/mix.wav`, `out/state.jsonl` (every watched
  instance's values and texts per video frame) and `out/cues.jsonl` (sections, pages, captions).
- `compose.py`: draws the frames (pages, captions, clean / through EffectForce) and encodes
  `out/subforce_demo.mp4` with ffmpeg. `--still <seconds>` writes one frame to `out/still.png`.
- `skin.py`: draws a plugin page from its built skin, as MPC does (from EffectForce's showreel).

Needs the x86 builds of SubForce (`make build/subforce.so`), EffectForce and PolyForce next to this
repository (`D:\DEV\EffectForce`, `D:\DEV\PolyForce`), their built skins, Python with Pillow, and
ffmpeg on the PATH.

```sh
sh showreel/build.sh          # in WSL: builds and runs the renderer
python showreel/compose.py    # on Windows: the video
```
