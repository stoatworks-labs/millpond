# millpond

Ripples, caustics and skimmed stones for Resolume Arena/Avenue, as an FFGL
effect. The clip is the bed of a pond, seen from above through water that
obeys the linear water-wave equations exactly. C++/GLSL, CMake MODULE →
universal `.bundle` (macOS) + Windows `.dll`. MIT. ID `MP01`, display name
`SW Millpond`.

Read `AGENTS.md` before touching the transform, the evolve pass, the caustic
mesh or anything that decides what is packed with what. Its "Faraday waves"
section is the reasoning and the traps for the shaken tank (Shake, Drive,
Damping, Slow Motion, Pattern, Bass Bins); the spec is
`~/Projects/resolume/specs/SPEC-millpond-faraday.md`.

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
- Everything: `tools/verify.sh` (fresh universal build, every check, the
  negative controls, the Faraday checks at two rasters and on Apple's software
  renderer, the bit-exact comparison with the build before Faraday, the pipe
  checks, the sweep, lipo, plist, ad-hoc signature, `oxbow probe`, the bench).
  `SKIP_SOFTWARE=1` leaves out the software renderer, its slowest part;
  `MUTANTS=1` adds the shader mutants. Set `OXBOW=` to the oxbow binary when
  running from a worktree whose sibling has no oxbow build.
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
- No dead controls: `python3 tools/sweep.py` (37 live; the context table
  gives the ones that need a drop, a skim, a beat or a shaken pond).
- **Faraday**: `--mathieu --onset --half --wavelength --seed --saturation
  --pattern --strobe --clock --resize --faraday-off --rate --state-shaken`,
  each with `--size WxH`, and `MPTEST_RENDERER=software` for Apple's software
  renderer. `--faraday-negative`: all 18 against a wrong model.
  `tools/offcompare.sh [--negative]`: Shake 0 bit for bit against 64f9690's
  build. `python3 tools/mutate.py [--only V28,...]`: one-character mutants of
  the four Faraday shaders, on a temporary copy of the tree.
- Render cost: `./build/mptest --bench` (Faraday rows included).

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
- **Faraday: Shake 0 must stay v0.1.0 bit for bit.** Nothing shaken may run
  at Shake 0; `offcompare.sh` is the proof. New parameters go after the last
  one, never between: compositions store them by index.
- **Faraday shaders use no built-in cos/sin, and exp only where measured
  safe**: series for small arguments, `cosSin` past them, twiddle textures for
  phases. Apple's software renderer's transcendentals broke three checks
  (AGENTS.md). The drive's phase is double on the CPU, never the host clock in
  a float.
- **Heights are Ã, not η**: η's RMS depends on the phase a frame lands on.
- **A shaken frame longer than 1/30 s runs in chunks** (`kMaxShakenStep`), or
  the saturation's splitting lets a stroboscope jump grow the lattice.
- Public at github.com/stoatworks-labs/millpond; registered in the fleet's
  tables in stoatworks-backend and on the website.

## Not done yet
- Released v0.2.0 (Faraday waves, the user guide) on 2026-11-19, after
  v0.1.0 on 2026-09-23. In Resolume only briefly: Arena 7.27.1 on win-lab
  (llvmpipe, no GPU, no sound device) and Arena 7.29.0 on this Mac, where
  Resolume's FFT drove the lattice from a `.wav` (the AV folder's `.mov`
  clips have no sound track). v0.2.0's release DLL passed win-lab's gate
  except its control sweep, which read six controls that change the rain
  rather than the picture as dead (Damping, measured directly there, acts). See
  the README's Status. Never in a show. No OFX port or factory presets.
- Faraday's open questions (the default Pattern, Slow Motion on by default,
  silence still calming the pond with Shake on, the fixed saturation) are in
  the README's Status and the guide's Known limits, for Allan.
- The browser demo's Faraday runs on a generated test bass, not Resolume's
  FFT. Its CPU half is a hand port, compared with the C++ and `mptest` once,
  by hand.
- `docs/USER-GUIDE.md` is the guide; the website's `build_guides.py` builds
  it into the site page and `docs/USER-GUIDE.pdf`. Rebuild the PDF when the
  guide changes.
- `StoatworksAbout.h` is generated by `sync-about.py` in stoatworks-backend;
  `ATTRIBUTIONS.md` is still a provisional hand copy (see its note).

## Browser demo

`demo/` is the page at **millpond-demo.stoatworks-labs.com**. It has the
plugin's twelve shaders (the four Faraday ones included) copied across
unedited. On top of those is a hand port of `Controls.cpp`, the CPU half of
`Physics.cpp`, all of `Faraday.cpp`, `PlanShake`, and the frame sequence with
its shaken chunks.

- Run `python3 demo/tools/check_shaders.py` after touching any shader.
  verify.sh fails on drift.
- Copy the C++ across rather than editing `plugin.js` by hand. A change to
  `Physics.cpp`, `Faraday.cpp`, `CollectImpacts`, `PlanShake` or a control's
  conversion has to be made in `plugin.js` too.
- Shake runs on a generated test bass (drone, on/off, kick, silence) that the
  page writes into the 64 FFT bins. It never asks for a microphone, and the
  page says that it is not Resolume's FFT.
built on the shared kit in `infrastructure/stoatworks-backend/resolume-demo/`, vendored into `demo/vendor/` by its `sync.sh` — fix a kit bug THERE, never here. There is no build step: `cf-run npx wrangler deploy` from the repo root uploads `demo/` as it stands, and the page is verified by content (its `<title>`), never by status code. `AGENTS.md` has what the page leaves out and why.

## Diagnostics

`source/Diag.{h,cpp}` is a log file only, with no crash handler (this runs
inside Resolume). It records which of the shaders failed to compile and
the GL vendor/renderer, and the host clock's unit once it has been voted on.

    ~/Library/Logs/millpond/millpond.YYYY-MM-DD.log
