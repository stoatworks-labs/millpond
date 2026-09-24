# millpond — for agents

The why behind the code. `CLAUDE.md` has the commands; this file has the
reasoning, the traps that were actually hit, and what is and is not known.
Built 2026-09-22/23 in one session, from Allan's one-line request: "ripples and
distortions, like dropping or skipping a pebble on water; scrunched paper as a
stretch goal". The spec is `~/Projects/resolume/specs/SPEC-millpond.md`.

## The one idea

The picture is the bed of a pond, and we look straight down at it through
water. Two models run in sequence, and nothing is drawn by hand:

1. **The water.** This is linear water-wave theory, solved exactly for each
   Fourier mode. It holds everything the operator sees move.
2. **The light.** Snell's law for the view ray and for the sun. The sun's light
   is splatted onto the bed as a conserving mesh, and the sky is reflected by
   the exact Fresnel term.

The one control shared by both is **Depth**. The same h goes into tanh(kh) in
the dispersion relation and into the ray's path to the bed. It is one pond.

## The water, and why it is spectral

A ripple shader is usually a finite-difference wave equation. That has one
wave speed, a CFL limit, and grid dispersion that nobody chose. Real water
ripples are dispersive, and the look of a dropped stone *is* the dispersion:

- the still disc inside the ring;
- long waves racing ahead of short ones;
- the rings getting closer together towards the middle.

So the propagator is exact instead. Each frame:

1. **inject** — last frame's (η, η_t/40), times the sponge, plus this frame's
   craters (and their mirror images for Walls). RG32F.
2. **forward FFT** — Stockham radix-2, log₂Nx + log₂Ny passes, the complex
   field η + i·η_t/40.
3. **evolve** — unpack the two real spectra with the Hermitian trick. Advance
   each mode by Δt as the damped oscillator y″ + 2γy′ + ω²y = 0, with
   ω² = (gk + σk³/ρ) tanh kh and γ = 2νk², using the closed form on whichever
   side of critical damping the mode is on. Pack (η + i·η_t/40) and
   (∂ₓη + i·∂ᵧη) for the way back.
4. **inverse FFT** — out comes (η, η_t/40, ∂ₓη, ∂ᵧη) in one RGBA32F texture.
   That texture is both this frame's surface and next frame's state.

There is no time step. A 30 fps host and a 240 fps host ring the same pond,
and a mode's phase is right however large ωΔt is (up to float, below).

### Stockham indexing, written out for N = 4

Stage `Span` = 2, then 4. Output j takes even = x[(j/Span)·Span/2 + j mod
Span/2] and odd = x[that + N/2], twiddled by exp(∓2πi (j mod Span)/Span).
With Span 2, y = (x0+x2, x0−x2, x1+x3, x1−x3). With Span 4 this gives
X0 = y0+y2, X1 = y1 + w·y3 with w = e^{−iπ/2}, and so on. Forward is −. The
sign was fixed by writing N = 4 out by hand; `--fft` checks every size
against an O(N²) DFT.

### Banks

The domain is the frame mirrored to 2W × 2H and it wraps. Grid point i sits
at (i + ½) cells, which is where GL puts texel i's centre. So the mirror of
cell i about x = 0 is cell Nx−1−i, and reading the surface with `texture()`
returns exactly the computed points.

- **Walls:** add each crater with its three images. The state is even about
  x = 0, so by periodicity it is even about x = W as well. That is a
  reflecting rectangular basin to every order: a DCT built out of an FFT.
- **Open:** no images. The three non-frame quadrants are a sponge,
  exp(−σ r² Δt), quadratic in the distance into the margin, with σ sized to
  give the fastest wave the pond holds six e-foldings on the way in (see the
  traps). Because the domain wraps, the frame is surrounded by margin on
  all four sides.

Changing Banks or Detail resets the pond. A walled pond carrying an open
pond's asymmetric state would keep the margin's waves bouncing in the
reflections.

### The crater

The crater is a Mexican hat, −A(1 − r²/a²) e^{−r²/a²}. It has zero net volume,
so the water pushed down comes up as a rim and the mean level never drifts.
Its Hankel transform is A k² a⁴/8 · e^{−k²a²/4}. Worked: H[e^{−pr²}] =
e^{−k²/4p}/2p; differentiate in p for the r² term; the constant parts cancel.
That transform is what makes the exact check possible (below), and its peak
at k = 2/a is why Pebble Size chooses between capillary and gravity rings.

## The light

- **View ray.** Straight down, `refract` at the surface normal of z = η, on
  to z = −h. The picture is read where the ray lands. Flat water gives
  exactly the identity, and `--still` checks that to 3e-7.
- **Caustics.** A mesh over the frame plus a 10% margin. Each corner is moved
  to where refracted sunlight lands, minus where flat water would land it, so
  that flat water maps the bed onto itself. Each triangle deposits (area
  before ÷ area after) at every pixel centre it covers, with additive
  blending, into R32F. Then a normalised Gaussian blur of σ = h·tan(sun
  radius). A blur moves light and never makes any.
- **Reflection.** The unpolarised Fresnel term, 0.02037 looking straight down,
  times a uniform sky, plus the sun's disc where the reflected ray finds it.

## The traps

Ordered by how much time they cost.

**Screen-space derivatives lose light where the water folds.** The first
caustic pass took |det(d origin/d screen)| from `dFdx`/`dFdy` in the fragment
shader. That is textbook and exact on paper. It lost up to 5% of the light
wherever the water folds: the middle layer of a fold lands as slivers a tenth
of a pixel wide, and a derivative across a sliver is taken from helper
invocations a whole pixel away. Found by histogramming the exact landing map
of a plane wave into pixel bins: the GPU's row sum was 1226 against 1280.

**A geometry shader makes vertex texture fetches return zero** on this Mac's
GL (4.1 Metal, M4 Max). The fix for the trap above was a geometry shader that
computed each triangle's area ratio from its three corners. With one attached,
`textureLod` in the vertex shader returned 0 everywhere. Nothing moved and
every triangle's ratio was exactly 1. The uniforms and program were fine;
dropping the geometry stage brought the displacement back. Do not reintroduce
one.

**So the mesh has no vertex data at all.** `gl_VertexID` gives the cell, the
triangle and the corner. Every vertex computes all three corners of its own
triangle and the ratio, in the same order, so all three agree bit for bit.
It costs three surface reads per vertex and six vertices per cell (about
1.1 M at the default) and it is still 1 ms a frame.

**A regular mesh whose pitch is a rational multiple of the pixel pitch is
biased.** Each triangle's deposit is right *on average over where it sits
against the pixel grid*. At exactly 1.5 px a cell every triangle sits at one
of a few phases, so the average is never taken: the rain case came up 1.8%
short at 640×360 with Detail 2048, and 0.02% short at a pitch of 1.876 px. The
corners are jittered by up to 0.3 cell with an integer hash. The outer ring of
corners stays fixed, and shared corners jitter identically, so the mesh stays
watertight. Now every raster and Detail tried is within 0.1%. An axis-aligned
plane wave is the worst case even so, because every row is the same row;
`--caustics` compares each column's mean over 360 rows (the row mean takes out
which jittered triangle covers which pixel), with an allowance derived from
the triangle size.

**GPUs divide as reciprocal × multiply, and this one stores half floats by
truncating.** Still water's area over itself came out 0.99999994. In R16F that
truncated to 0.99951, a visible step dark on 6% of the pixels, and it biased
every deposit down by half a step. The buffer is R32F. Float blending works
here and on llvmpipe in win-lab's Arena; it is also standard on DX11-class
Windows GPUs, but untested on one.

**η and η_t share a transform, and η_t is ω times bigger.** A transform's
rounding is relative to the larger thing it carries. Unscaled, η_t drowned η:
the walled pond's mirror symmetry drifted 7e-4 (L2) in ten seconds. Carrying
η_t/40 (40 rad/s is ω at the minimum group velocity) brought that to 7e-5.
The surface texture's `.g` is η_t/40 everywhere, harness included.

**Twiddles from `cos`/`sin` in the shader are a systematic error.** The same
few ulp land every frame on a state that is fed back sixty times a second, so
they accumulate coherently. The twiddles come from a double-precision table
(an RG32F texture per axis). That halved the round-trip error to 2.2e-7.

**The Nyquist row and column must be zeroed.** At Nyquist, i·k·A is not the
conjugate of itself, so a spectral derivative there is not Hermitian. It leaks
into the imaginary half of the packed pair as a grid-scale checkerboard.

**The Open sponge ate the `--modes` test wave.** A standing wave that fills
the whole periodic domain is three quarters in the sponge. `--modes` runs with
Walls. This was a test bug, but it cost a round of looking for a propagator
bug.

**Asymptotic checks are the wrong checks.** The first `--gravity` counted zero
crossings against the stationary-phase law 8πr²/(gt²). It missed by 11% near
the crater and 8% far out. Those are genuine O(1/kr) corrections and the
amplitude-slope terms of the next order, not plugin errors. The exact
Cauchy–Poisson integral, evaluated in double at every grid point, replaced it,
and agrees to 0.006%. The first attempt at that read the surface *between*
grid points by bilinear interpolation and blamed the plugin for the harness's
own (k·dx)²/8 error, 6% for the shortest ring. Compare at grid points. The
0.02% left with surface tension was viscosity missing from the theory: at
1e-7 m²/s it is still 2% by five seconds for k ≈ 300.

**Glints need a high sun, and a range that can reach white.** Three separate
reasons there were no glints at all:

- The camera looks straight down, so the sun appears in a facet tilted by half
  its zenith angle. Measured ripples here never tilt more than 4°, so a 70°
  sun (a 10° facet) never glinted. The default is 85°.
- The Glint range topped out at 30× the sky. After the 2% Fresnel weight that
  can never reach white; the real sun is 10⁴–10⁵× the sky. It is now 5000·v³.
- A 0.6° disc seen in rippled water is a line thinner than a pixel that
  pixel centres almost never land on. The disc is widened to the pixel's own
  spread of reflected directions (`fwidth`) and dimmed by the same area
  ratio, which keeps each glint's energy.

**Script tracks interpolate.** In the fleet's cue format a track is held
before its first key and interpolated between keys. A press keyed as
`300 Skim 1 / 301 Skim 0` is therefore held at 1 from frame 0, and a later
press keyed without the key before it ramps up and crosses 0.5 halfway there.
The first reel's skim fired on frame 0, before anyone was watching. A press is
three keys.

**Three texture units cannot be unwound by the scoped bindings.** An
independent review found this. Every `Scoped2DTextureBinding` clears to 0 on
whichever unit is active when it exits, and every `ScopedSamplerActivation`
sets unit 0 on exit. Unwinding three interleaved pairs therefore clears unit
2, sets unit 0, and then "clears unit 1" on unit 0. The composite (three
units) left the surface texture bound on unit 1 in the host's context every
frame. Two pairs happen to unwind correctly, so the FFT's pattern looked
safe. `releaseTextureUnits( 3 )` after the draw fixes it. `--state` would
have caught it; checked by removing the fix.

**The host's clear colour is state too.** Our passes clear with their own and
`SavedGLState` did not put it back. The harness could not see it, because it
sets its own before every frame. Depth test, culling and scissor are now also
forced off for our passes and restored: a host that left any of them on would
reject overlapping caustic triangles, drop the folded ones, or clip a pass.

**The grid is periodic, so anything past the frame comes back in.** A skim
hard enough to cross the pond kept touching past the far bank, and those
touches wrapped round into the frame at random. With Walls, every one also
mirrored into the pool. The stone now stops at the bank, and scattered
pebbles are clamped into the frame.

**The clock-unit vote settles with a jump.** Wall time since the first frame
becomes host time, which can be hours in. The frame that settled the vote
advanced the water by the whole 0.25 s clamp: a lurch and a burst of rain. The
settling frame now takes no time.

**The sponge is sized from the fastest wave the pond holds.** A wave crossing
the margin loses Rate × margin / (3v) e-foldings, so a fixed rate protects
slow ripples well and a big deep pond's long waves hardly at all. The review
had it right and my README had it backwards: the weak case is the LARGEST
ponds. The rate now gives the fastest wave six e-foldings on the way in.

**`getenv` debug switches.** Several were added to the harness while hunting
the above and are all gone again. If you add more, grep for `getenv` before
you commit.

## Every numeric check, and where its tolerance comes from

Asked of each: would it still hold on another rasteriser, at another raster?

| check | bound | why that number |
| --- | --- | --- |
| `--fft` | 1e-5 relative RMS | float32 rounding through 15 stages each way is a few 1e-7 (measured 2.2e-7); a sign or index error is O(1) |
| `--modes` | 2e-4 + 1e-6·ωt of the amplitude | ωt reaches hundreds of radians and float holds that to ~3e-5 rad |
| `--gravity`, `--quiet` exact | 0.2% of the largest ring | measured 0.006–0.019% after 150–300 frames of fed-back float rounding; 10× headroom for another GPU. A 1% error in g gives 14–37% |
| `--quiet` Airy edge | λ₀/4 + one cell | the carrier's phase decides which crest of the envelope's top is found |
| `--quiet` still disc | 3% of the ring | the Airy tail inside c·t decays as exp(−⅔z^{3/2}); 0.85·c·t is a few z in |
| `--shallow` | c₀t ± (3 Airy widths + 2a) | the front is itself an Airy front of width (c₀h²t/2)^{1/3} |
| `--banks` | Open < 2%, Walls > 50%, mirror 1e-3 | measured 0.0000 / 0.84 / 3.6e-5; a missing image is O(1) |
| `--refraction` | 1.5 × (k·dx)²/8 of the displacement + 1e-3 px | linear interpolation of the loaded slope between grid points |
| `--fresnel` | 1e-3 absolute | dR/dslope ≤ 0.1 times the interpolation error in slope; a wrong index moves R by 1e-2 |
| `--caustics` still | 1e-6 | division as reciprocal × multiply |
| `--caustics` rain | mean 1 ± 0.005 | edge crossings (balanced) plus coverage luck (unbiased, jittered); measured ≤ 0.1% |
| `--caustics` plane | exact variation over ±0.8 mesh cell + 0.01, on each column's mean over 360 rows | a triangle carries its mean focus and reaches 0.8 cell once jittered; the row mean takes out which triangle covers which pixel, and the oblique suns failed per pixel on exactly that luck |
| `--state` | exact | every piece of GL state a host could care about, after three frames |
| `--still` | 1e-5 | float; one 8-bit code value is 4e-3 |
| `--skim-check` | 1e-5 m, 1e-4 rad | positions stored as float metres |
| `--rain` | 4√N | Poisson; a false alarm one run in 16,000 |
| `--audio` | exactly 19 | deterministic feed; the frame-0 hit cannot be an onset |

The physics checks run on a grid sized in metres, so their raster is
irrelevant. The optics checks run at 480×270 and 1280×720. Every check has a
negative control in `--negative`, and all 13 fail against their wrong model.

## Shape of the code

    source/Physics.*     dispersion (mirrored), group velocity, the Airy scale,
                         the skim schedule, PCG rain, the grid choice
    source/Controls.*    0..1 host parameters to physical units
    source/Shaders.*     the seven passes; kRateScale
    source/Millpond.*    the plugin: clock, events, buffers, the passes
    source/Audio.*       rosette's analyser
    source/PassBuffer.*  FFGLFBO with the leak fixed, plus wrap modes
    source/GLState.h     put the host's state back
    tools/mptest/        the harness: checks, --pipe/--film/--script, --bench
    tools/sweep.py       no control is silently dead
    tools/verify.sh      all of it

## Decisions taken without asking

- **Name and id.** `millpond` / `MP01` / `SW Millpond`, after the proverbial
  still water. `MP` was checked free against every repo's source.
- **One plugin; crumpled paper deferred.** Paper is an opaque folded sheet
  under a lamp, not a transparent moving surface, so it is a different
  mechanism and would be a different repo (`CR01` reserved in the spec).
- **Spectral, not finite-difference.** It is the only way to get the
  dispersion exactly. The cost is a fixed ~1 ms at the default Detail whatever
  is happening, and a power-of-two grid.
- **Displacement craters, not impulses.** A crater released from rest is the
  simplest source with a closed-form transform, which is what made the exact
  check possible.
- **Events land on whole frames.** Sub-frame injection would need per-drop
  phase correction in the spectrum. At 60 fps the error is a tenth of the
  skim's quickest hop.
- **The skim starts at the bank** and passes through Pebble X/Y, so one aim
  point serves both the pebble and the skim.
- **Defaults are chosen to be seen:** light rain on, a 3.5 cm stone, a crater
  half its radius deep, 0.3 m of water, an 85° sun. A water effect that does
  nothing until someone finds the Drop button reads as broken.
- **About is generated** by `sync-about.py` (with `guide = ""`: there is no
  user guide). `ATTRIBUTIONS.md` is still a provisional hand copy.

## What is genuinely verified, and what is assumed

Verified, on this machine (M4 Max, macOS 26.4.1): everything in the README's
Status table, `tools/verify.sh` green (universal bundle; `lipo` shows both
slices; `oxbow probe` reads `SW Millpond` / `MP01` / effect), and `oxbow
selftest` instantiates the plugin through the host path and renders 120
frames.

Assumed, or not yet done:

- **Loaded into Resolume only on Windows, on llvmpipe.** In win-lab's Arena
  7.27.1 (no GPU), `plugin-bench/arena/arenaprobe.py` found it registered and
  listed, with all 40 controls declared correctly, and saw it render with Arena
  logging no errors. The plugin voted Arena's clock to be milliseconds. The
  probe's control sweep never presses Drop or Skim, so it reports Surface
  Tension, Banks, Pebble Size, Heading and Throw Speed as dead: that is its
  blind spot, not a defect. Still unknown: anything on macOS Arena, anything
  on a GPU in Resolume, whether the XPOS/YPOS pair shows as a position pad, and
  how the event parameters behave under a real click.
- **Resolume's FFT bins** are assumed to be what rosette assumed; the fleet has
  never measured them.
- **Windows only on llvmpipe.** It builds under MSVC 2022, and R32F additive
  blending and vertex texture fetch work on llvmpipe. They are standard on
  DX11-class GPUs, but untested on one.
- **The GPU-less CI runner** would run the transforms in software. It has not
  been tried; `verify.sh` is ~80 s here, and the sweep and `--negative` are
  the slow parts to gate if it crawls.
- Linear theory only; see the README's Status.

## The browser demo

`demo/` is the page at **millpond-demo.stoatworks-labs.com**, built on the shared kit in `infrastructure/stoatworks-backend/resolume-demo/`, vendored into `demo/vendor/` by its `sync.sh` — fix a kit bug THERE, never here. There is no build step: `cf-run npx wrangler deploy` from the repo root uploads `demo/` as it stands, and the page is verified by content (its `<title>`), never by status code.

**What is the plugin's own code.** All eight shaders — `kVertexShader`,
`kInjectShader`, `kFFTShader`, `kEvolveShader`, `kCausticVertexShader`,
`kCausticFragmentShader`, `kBlurShader`, `kCompositeShader` — copied into
`demo/plugin.js` character for character. `demo/tools/check_shaders.py`
compares them and `tools/verify.sh` runs it as its "Demo shaders" step. So the
water on the page is this water: an FFT of the surface every frame, each mode
advanced exactly, and back.

**What is a port, checked by a reader and nothing else.** `Controls.cpp`, the
CPU half of `Physics.cpp` (`Omega`, `GroupVelocity`, `SkimSchedule`,
`SkimStart`, `Random` with its Poisson draw, `ChooseGrid`), `CollectImpacts`,
`Simulate`, `Transform`, `Caustics`, `MakeTwiddles` and the frame sequence of
`ProcessOpenGL`. Cross-checked by hand once, on 2026-09-24, and nothing re-runs
it: `Random` (PCG-XSH-RR on a BigInt state) gives the C++'s first four draws
bit for bit and the same total over 1000 Poisson draws at a mean of 0.37 (334);
an 11-touch skim schedule agrees to float rounding (1e-8); `ChooseGrid` gives
the same 1024 x 512 grid; `GroupVelocity` agrees to 5e-8 relative, the residue
being the water constants rounded to float in the C++. To redo it, slice the
block from `Controls.h / Controls.cpp, ported` to `The plugin's frame` out of
`plugin.js` into a `new Function` under node, and print the same values from a
one-file C++ program built against `source/Physics.cpp` and
`source/Controls.cpp`.

### The decisions, and why

- **Drop, Skim and Still are booleans the renderer releases**, readout's Fire
  precedent. The kit has no event type; each button is taken and set back to
  Off on the frame it acts, which is what a host does with an event parameter
  and why it blinks. That is also the answer to the Arena gate's blind spot:
  here a visitor presses them. A `?drop=1` in the URL fires one pebble on load,
  which is how the page was verified headlessly.
- **Audio, Audio Pebbles and Audio Rain are absent, not dead.** With no spectrum
  the plugin's analyser never fires and reports a level of 0, so the page drops
  exactly the pebbles and rain the plugin would.
- **Three float extensions, and each one's absence throws.**
  `EXT_color_buffer_float` (RG32F spectrum, RGBA32F surface, R32F caustics),
  `EXT_float_blend` (the caustic mesh adds into R32F) and
  `OES_texture_float_linear` (the surface and the caustics are read LINEAR;
  without it WebGL2 samples them as black, which would show a still pond rather
  than an error).
- **The surface wraps.** The kit's `PassBuffer` clamps; the plugin allocates the
  surface with `Wrap::Repeat` because the domain is periodic, so the page sets
  REPEAT itself after each `ensure()`.
- **The clock is the page's, in seconds**, so the unit vote has nothing to do;
  the quarter-second clamp per frame is kept. **Restart stills the water**, which
  the plugin has no control for.
- **Detail stays at the plugin's default, 1024.** A real GPU runs it in the
  browser without trouble. Headless Chrome on SwiftShader cannot: the default
  caustic mesh is about 1.1 million vertices, and `cdpshot.py`'s CDP call timed
  out behind it — verify headlessly at `detail=0&caustics=0`.
- **The default clip is the geometry card**: water hides on a busy clip (the
  plugin's own video uses only calm ones), and straight lines show every bend.
- **The About block is absent**; its links are in the page header.
