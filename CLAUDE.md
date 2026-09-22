# millpond

Ripples, caustics and skimmed stones for Resolume Arena/Avenue, as an FFGL
effect. The clip is the bed of a pond, seen from above through water that
obeys the linear water-wave equations exactly. C++/GLSL, CMake MODULE →
universal `.bundle` (macOS) + Windows `.dll`. MIT. ID `MP01`, display name
`SW Millpond`.

Read `AGENTS.md` before touching the transform, the evolve pass, the caustic
mesh or anything that decides what is packed with what.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build`
- Render offline: `./build/mptest --out /tmp/frame.png --drop 10 --skim 60`
- The card on its own: `./build/mptest --card /tmp/card.png`
- List parameters: `./build/mptest --list`
- Set anything by name: `./build/mptest --set "Banks=1" --set "Surface Tension=0"`
- Film: `./build/mptest --film 720 --size 1280x720 --script docs/demo.cues | ffmpeg -f rawvideo -pix_fmt rgba -s 1280x720 -r 60 -i - out.mp4`
- Film a clip through it: `ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - | ./build/mptest --pipe --size WxH [--script cues] | ffmpeg …`

## Verify
- Everything: `tools/verify.sh` (~80 s: fresh universal build, every check, the
  negative controls, the sweep, lipo, plist, ad-hoc signature, `oxbow probe`,
  the bench)
- **The transform**: `--fft`, the round trip and spectral derivatives against an
  O(N²) double DFT. **Each wave**: `--modes`, against the textbook oscillator.
- **The rings**: `--gravity` and `--quiet` compare EVERY grid point with the
  exact Cauchy–Poisson integral (0.01%). `--quiet` also checks the still disc
  and the Airy edge. `--shallow`: nothing outruns √(g h).
- **The banks**: `--banks`. **The optics**: `--refraction` (Snell, two
  rasters), `--fresnel`, `--caustics` (conservation and the Jacobian),
  `--still` (flat water is the identity).
- **The events**: `--skim-check`, `--rain`, `--audio`.
- **The checks can fail**: `--negative`, all 13 against a wrong model.
- No dead controls: `python3 tools/sweep.py` (31 live; ten need the context
  table: a drop, a skim or a beat).
- Render cost: `./build/mptest --bench`.

## Notes
- **The water is exact per mode.** Inject → forward FFT → evolve (damped
  oscillator, ω² = (gk + σk³/ρ) tanh kh, γ = 2νk²) → inverse FFT, every frame.
  No time step limit. The dispersion is `//= mirrored` in `Physics.cpp`
  because the harness predicts the rings from it.
- **η_t is carried divided by `kRateScale` (40 rad/s)** in the surface texture
  and through both transforms. It shares a complex transform with η, and
  unscaled its rounding swamped η's.
- **FFT twiddles come from a double-precision table**, not `cos`/`sin` in the
  shader: the same few ulp every frame is a systematic error in a state that
  is fed back sixty times a second.
- **The Nyquist row and column are zeroed** in the evolve pass. At Nyquist a
  spectral derivative cannot be Hermitian.
- **The grid is the frame mirrored to 2W × 2H.** Walls = craters plus three
  mirror images. Open = a sponge in the three non-frame quadrants. The grid is
  sized in metres, so the physics checks do not depend on the raster.
- **The caustic mesh has no vertex data.** `gl_VertexID` → cell, triangle,
  corner. Each vertex computes its whole triangle's area ratio. Corners are
  jittered, so pixel-centre coverage is unbiased. Do not go back to a
  fragment `dFdx`, a geometry shader, or a regular mesh (AGENTS.md says why
  for each). The buffer is R32F.
- **Glints are widened by `fwidth` of the reflected ray** and dimmed by the
  same area ratio. Without that they are a sub-pixel line nothing samples.
- **Script tracks interpolate.** A button press is three keys: `29 Drop 0`,
  `30 Drop 1`, `31 Drop 0`.
- All host parameters are 0..1 and mapped in `Controls.cpp`; option parameters
  hold the element value; events act on the rising edge.
- GLSL reserved words (`patch sample input output filter common active half
  layout flat`) must not be identifiers; `verify.sh` greps for them.
- Randomness is integer hashing (PCG / lowbias32), never `fract(sin(...))`.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or no
  host can instantiate the plugin.
- `millpond_core` is an OBJECT library: the plugin registers itself from a
  file-scope constructor nothing references.
- The macOS build must be universal. Check with `lipo`, never the build log.
- Local repo only: no GitHub remote, no tag, not registered on the website.

## Not done yet
- Never loaded into Resolume (oxbow selftest only). No OFX port, browser demo
  or factory presets. Never built on Windows.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies.

## Diagnostics

`source/Diag.{h,cpp}` is a log file only, with no crash handler (this runs
inside Resolume). It records which of the six shaders failed to compile and
the GL vendor/renderer, and the host clock's unit once it has been voted on.

    ~/Library/Logs/millpond/millpond.YYYY-MM-DD.log
