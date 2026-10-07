# The demo video

A melodic-techno build in F minor made only with SubForce (bass sequence, pluck arpeggio, gliding
lead, pad), each layer entering clean and then going through its own EffectForce, the knobs moving as
a player would turn them; EffectForce's Glue Comp on the master. The audio is rendered offline through
the built plugins; the video shows their pages with the values they reported.

- `render.cpp`: plays the track through the plugins as MPC does (128-frame blocks, MIDI with sample
  offsets, the transport running), level-matches each layer's effected half to its clean half, masters
  the mix (−14 LUFS, peaks under −1 dBFS). Writes `out/mix.wav`, `out/state.jsonl` (every watched
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
