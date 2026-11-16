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
    source/Shaders.*     the seven passes, the four Faraday ones; kRateScale
    source/Millpond.*    the plugin: clock, events, buffers, the passes
    source/Audio.*       rosette's analyser
    source/PassBuffer.*  FFGLFBO with the leak fixed, plus wrap modes
    source/GLState.h     put the host's state back
    source/Faraday.*     the shaken tank's CPU half: k_c, the threshold, the
                         drive table in double, the stroboscope, the bass
    tools/mptest/        the harness: checks, --pipe/--film/--script, --bench
    tools/sweep.py       no control is silently dead
    tools/offdump/       dumps ten scenes' floats, for offcompare.sh
    tools/offcompare.sh  Shake 0 against the build before Faraday, bit for bit
    tools/mutate.py      one-character mutants of the Faraday shaders
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

## Faraday waves: the shaken tank

Added 2026-11-16 on `claude/faraday`, from the IDEAS entry "millpond: Faraday
waves". The spec, with every source and what was read of it, is
`~/Projects/resolume/specs/SPEC-millpond-faraday.md`. **Shake 0 is off**: none
of this runs, and the frame is v0.1.0's bit for bit (`tools/offcompare.sh`).

**The one idea.** The bass shakes the tank up and down. In the tank's frame
gravity becomes g − a cos Ωt, and every mode millpond already rings as a
damped oscillator becomes a damped Mathieu oscillator (Benjamin & Ursell
1954, eq. 2.12):

    η̈ + 2γη̇ + [ ω0²(k) − a k tanh(kh) cos Ωt ] η = 0,    γ = 2νk² + Damping

Above the first tongue the modes with ω0 ≈ Ω/2 grow, at exactly half the
drive. Nothing about the lattice is drawn: its spacing is millpond's own
ω(k) at Ω/2, its onset is the tongue, and the bed is seen through it by the
existing optics.

### A shaken frame

The CPU (`Faraday.cpp`, `PlanShake`, all in double) works out ω_c = Ω/2, k_c
from the dispersion relation, γ_c, the leading-order threshold
a_c = 4γ_c ω_c/(k_c tanh k_c h), the acceleration Shake × bass × a_c, the
saturation rate β = 150 γ_c k_c², the drive's phase, the stroboscope's water
step and the cosines at every substep's two Gauss points. Then:

1. **a. envelope** — Ã² = η² + (η_t/ω_c)², R32F.
2. **b–d. kernel** — FFT of Ã², times the Pattern kernel K̂(|k|), inverse.
   Skipped for Stripes (K̂ = 1).
3. **e. saturate** — η, η_t × exp(−β max(K∗Ã², 0) Δt). The inject pass reads
   this instead of last frame's surface.
4. **inject → FFT → shaken evolve → iFFT** — v0.1.0's chain, with the evolve
   swapped for a fourth-order Magnus integrator (32 substeps a drive period,
   at most 1024 a pass) and the seed.

A frame longer than 1/30 s of water (a stroboscope's jump, a slow host, a
high Speed) runs as equal chunks of passes a–4, at most 32 of them.

### Why each choice

- **Magnus, not RK4.** The 2×2 exponential has a closed form (the evolve
  pass's own cos/sin or cosh/sinh), so a substep is exact for the frozen
  coefficients, and with no shake one substep is exactly v0.1.0's step. A
  short wave rotates through many radians a substep without a stability
  limit. Truncation at 32 substeps a period is 4e-6 to 1.4e-5 relative over
  one to two seconds for the lattice's modes (`--mathieu` prints it every
  case).
- **The drive's phase lives in double on the CPU.** It is reduced mod 2π
  every frame, and the shader only gets cosines in a texture and times
  relative to the frame. That is the answer to the clock-overflow trap:
  Resolume's clock reaches 5×10⁸ ms, which a float holds only to 32 ms, a
  third of a drive period at 10 Hz.
  `--clock` runs the pond at a host time of 4.99×10⁸ ms and gets the water
  it gets at 0.
- **Frame-rate independence** comes from three places: a fixed number of
  substeps per drive period, however long the frame; a seed that is a Wiener
  increment (√Δt); and a saturation that is exact for the frame's Δt on a
  uniform level. `--rate`, `--saturation` and `--strobe` measure each.
- **The saturation is on the envelope.** η alone swings with the phase, so a
  damping on η² would depend on which instant of the cycle a frame lands on.
  Ã is the same at every phase for anything oscillating at ω_c. β ∝ γ_c k_c²
  is Milner's νk⁴ scaling when the damping is viscous; 150 is chosen, not
  derived (see the steepness below).
- **The seed is banded.** A still pond has nothing to grow from. The seed is
  a Wiener increment in η only (so the first seeded frame leaves η_t at 0),
  Hermitian, a Gaussian in |k| about k_c with a width of 0.25 k_c, m = 0
  excluded, and summed over the four mirror images for Walls. White to the
  grid's scale it was up to 19 of 255 levels of caustic sparkle in silence;
  banded, it is invisible and only the modes the shake can amplify get it.
- **Pattern is a kernel.** The cubic coupling between standing waves is
  what picks the pattern (no quadratic terms: the response flips sign each
  drive period). The kernel's notches shape g(θ) (derived below), so each
  Pattern is the minimum of the Lyapunov function, not a drawn lattice.
- **Slow Motion is a stroboscope.** Each frame the water advances to the
  latest moment, no later than the nominal clock, at which its phase ω_c t
  equals a slowed phase. So the water keeps real time on average (the
  lattice stands up and lies down with the music) while the oscillation is
  seen S times slower. This is schlieren's cheat (a slowed camera, beats in
  real time) adapted: slowing the water's own clock, as schlieren does, would
  slow the lattice's growth and collapse with it, and millpond already has
  that as Speed. The price: anything not at ω_c (rain, pebbles) is seen in
  jumps of one response period. **The default is 8×**: unslowed, a 10 Hz
  lattice reverses ten times a second, a high-contrast pattern reversal in
  the photosensitive band; at 8× it is 1.25 a second.
- **Shake is in units of the threshold**, 0 to 4. The threshold moves from
  0.21 g to 0.63 g across Drive with Damping 2 (and 0.0003 g to 0.37 g with
  Damping 0), so a Shake in g would make the lattice vanish whenever someone
  touched Drive. Not past 4: the fastest-growing wavenumber moves off k_c as
  the shake grows. At the defaults the lattice's wavenumber measured 1.8%
  above k_c at 1.5×, 2.6% at 2× and 7.3% at 4× (`--wavelength`'s measure,
  run once by hand at 10 Hz on the 1.5 m pond); the leading-order growth
  rate σ(k) = √((q_k/4ω_c)² − δ²) − γ puts the fastest-growing one 29% above
  at 4× and 60% at 8×, so saturation and pattern competition hold the
  lattice nearer k_c than that estimate.
- **The bass is the host's FFT.** The mean of √bin over the lowest Bass
  Bins bins (the √ is rosette's assumption), a 5 ms attack and 80 ms
  release, normalised by its own recent peak (3 s decay, floor 0.02), on the
  host's clock, not the water's. Nobody has measured Resolume's bins, so
  Bass Bins (1–16, default 2) exposes the frequency mapping instead of
  assuming it. No live audio device (earshot): an amplitude envelope at frame
  rate is all the model needs; the drive frequency is a control.

### g(θ), worked

Standing waves η = Σ a_j cos(k_j·x) cos(ω_c t), all |k_j| = k_c. Their envelope
is Ã² = (Σ a_j cos k_j·x)², the same at every phase. The saturation is
dη/dt = −β (K∗Ã²) η, and K∗ multiplies each Fourier component cos(q·x) by
K̂(|q|), with K̂(0) = 1. Project onto cos(k_i·x) (twice the spatial mean of the
product):

- **Self:** Ã² = a²/2 (1 + cos 2k·x), so K∗Ã² = a²/2 (1 + K̂(2k_c) cos 2k·x).
  Times a cos k·x, the cos k·x part is a³ (1/2 + K̂(2k_c)/4):
  **g0 = 1/2 + K̂(2k_c)/4**.
- **Cross, wave j at angle θ:** two terms carry a_j² a_i. a_j² cos² k_j·x
  gives a_j²/2 (its K̂(2k_c) part projects to nothing for θ ≠ 0, π). The
  cross term 2a_i a_j cos k_i·x cos k_j·x = a_i a_j [cos (k_i−k_j)·x +
  cos (k_i+k_j)·x] picks up K̂ at |k_i ∓ k_j| = 2k_c sin θ/2 and
  2k_c cos θ/2, and times a_j cos k_j·x each projects with ½:
  **g(θ) = 1/2 + [K̂(2k_c sin θ/2) + K̂(2k_c cos θ/2)]/2**.

For N equal waves at equal angles the steady amplitude is
A² = α/(g0 + Σ_{m≠1} g(θ_m)), and the Lyapunov function is lowest for the
largest N/(g0 + Σ g) (Chen & Viñals (4.1); Edwards & Fauve (9)). With the
plugin's Gaussian notches (width 0.25 k_c, so g at the notch is not exactly
1/2 elsewhere):

| Pattern | K̂ | g0 | g(90°) | g(60°) | stripes 1/g0 | squares 2/(g0+g90) | hexagons 3/(g0+2g60) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Stripes | 1 | 0.750 | 1.500 | 1.500 | **1.333** | 0.889 | 0.800 |
| Squares | notch at √2 k_c | 0.734 | 0.500 | 1.150 | 1.363 | **1.621** | 0.989 |
| Hexagons | notches at k_c and √3 k_c | 0.609 | 0.914 | 0.500 | 1.641 | 1.313 | **1.864** |

### The Faraday traps

Ordered by how much time they cost.

**Apple's software renderer's transcendentals.** Its cos, sin and exp are good
to about 2e-4 (exp to about 1e-4), and hundreds of substeps a frame compound
that. 700 substeps of built-in cos/sin missed the Mathieu solution by 15%;
180 substeps put a short wave 17% wrong; the Walls seed, mirrored with
cos/sin phases, was its own mirror image only to 1.3e-3. So: C and S are
series in ψ² for |ψ²| < 1 (every mode near the lattice); exp(−z) is a series
below 0.1 (`expNeg`, in the evolve and the state pass); past ψ = 1, cos and
sin come from `cosSin`, a Cody–Waite reduction by π/2 in two parts and
series on |r| ≤ π/4; and the seed's mirror phases come from the
double-precision twiddle textures the FFT already has. The overdamped
branch keeps exp, which that renderer gets right to the measurement's
precision (`--mathieu`'s syrup case). The GPU never needed any of this.

**η measured at one phase is not a height.** η's RMS swings with the phase
of the cycle a frame lands on. Read that way, the saturated height seemed
to depend on the frame rate; it was the measurement. Every height in the
harness is Ã's RMS. Even Ã swings with phase at large shake (anharmonic
Floquet solutions), which is why `--saturation`'s bound is that swing.

**A stroboscope's jump is a long pass.** The saturation is applied once a
pass, ahead of the evolve: a first-order splitting. The stroboscope advances
the water 0.2 s at a time at 10 Hz, and in one such pass a lattice at 4×
threshold grew unchecked. At the defaults under Slow Motion 8 it stood
between 0.97 and 1.68 mm RMS, where the unstrobed pond stood between 1.15
and 1.37 (and an every-half-second sample of the unstrobed pond, aliased to
one phase, had looked rock steady). Long frames now run in 1/30 s chunks;
`--strobe` holds the two envelopes together.

**The white seed was visible.** See *why each choice*: up to 19 of 255 levels
of caustic sparkle in silence at full Shake.

**The notch must be wider than the lattice's drift.** At 0.1 k_c, a 2× shake
at the old defaults formed its lattice 12% high, the notch missed it, and
every Pattern gave the same labyrinth. 0.25 k_c catches it.

**Off resonance the Floquet multipliers are complex.** Below the tongue and
detuned, a mode beats instead of decaying steadily, so two samples of η do
not measure a growth rate. `--onset` uses the mode nearest k_c driven at
exactly twice its own frequency, samples a whole number of drive periods
apart, and waits out the decaying Floquet solution first.

**`--mathieu` must reach every quadrant of `cosSin`.** Its one short-wave case
sat in quadrant 2, and a sign flip in quadrant 1 survived every check. Three
cases now pick the mode whose phase a substep is 1.7, 4.7 and 6.3 rad.

**First frame, dt = 0.** The first shaken frame covers no water, so a check
that renders one frame and asks whether it ran shaken sees nothing. Render
two.

**The harness's own RK4 must not be the perturbed one.** `--mathieu`'s budget
is the scheme's truncation, Magnus in double against RK4. Its negative
control first perturbed the RK4 that set the budget too, and passed.

### Every Faraday check, and where its tolerance comes from

Asked of each: **would this hold on another rasteriser, at another raster?**
All of them run at 320×180 and 1280×720 on the GPU and at 320×180 on Apple's
software renderer, and every number below printed the same at both rasters
(the water is a grid in metres; only `--resize` reads the raster, and it
compares bit for bit). The software renderer agreed to the printed digits
except where noted.

| check | measured (GPU) | bound | why that number |
| --- | --- | --- | --- |
| `--mathieu` | 4e-6 to 3.5e-4 of \|y\| for the lattice, capillary, syrup and short waves at ψ up to 4.7; 1.35e-2 at ψ = 6.3 | the scheme's truncation (Magnus in double vs RK4, 2000 steps a frame) + 1e-5 + 16 ulp a substep | rounding a few ulp a substep, plus what the scheme itself gets wrong, measured in the same run. At ψ ≈ 2π the scheme's own truncation is 1.3% over 0.3 s: the check says the GPU is the scheme there, not that the scheme is right |
| `--onset` | onset 2.69101 m/s² vs the exact tongue 2.69081 (+7.3e-5); capillary +5.4e-5 | 5.6e-4, 9.4e-4 | each amplitude to `--mathieu`'s 1e-3, so each rate to 2e-3 over 2 s, through the slope of rate against shake; plus the straight-line interpolation's own bias, computed from the exact rates |
| `--half` | one drive period on, correlation −0.9998; two, +0.9991; twenty, +0.9470 | < −0.9, > +0.9 | 0.9 = cos 0.45 rad: twenty drive periods on, a response off f/2 by more than 0.7% falls below it. Slopes = η's gradient to 9.1e-7 (bound 1e-4: float transforms round at 1e-6) |
| `--wavelength` | lattice 406.69 rad/m vs ω(k) = πf at 406.61 (+0.02%) | inside the growing band, ±2.63% | the lattice can only be made of modes whose exact Floquet exponent is positive at the shake used |
| `--seed` | η_t/ω_c at 1.0e-7 of η; 0.29% / 0.22% of the power beyond 3 sd of k_c; Walls mirror 1.2e-6 | 1e-4; 1%; 1e-4 | a Gaussian leaves 0.27% beyond 3 sd; float transforms round at 1e-6. Software: 1.1e-7 |
| `--saturation` | uniform level kept 0.913931 and 0.548812, errors 2e-8 and 1.3e-7; heights at 60, 40, 24 fps and Speed 4 within ×1.067 | 1e-5; ×1.395 | the level is a sum over every cell; ×1.395 is the envelope's swing in the neutral Floquet solution, squared (a height read at the end is one phase's) |
| `--pattern` | Stripes coherence 0.82; Squares autocorrelation at 90° +0.76; Hexagons at 60° +0.64 | 0.5 | the midpoint between a perfect lattice (1) and none (0) |
| `--strobe` | lag 0 to 0.198 s; zeros 0.8008 s apart (expect 0.8000); strobed = real-time water to 2.4e-5; at 4× the strobed envelope ×1.015 / ×0.985 of the unstrobed | one response period; 2 frames over N−1 gaps; 1e-4; ×1.07 | each crossing placed to a frame; 1e-4 is the Magnus difference between two substep counts; ×1.07 is `--saturation`'s measured frame-step spread rounded up — a machine's measurement, not a derivation |
| `--clock` | RMS 1.4e-6; drive phase 4.188804936 vs 4.188804935 | 1e-4; 1e-6 rad | a double holds 4.99×10⁵ s to 6e-11 s, so the phase to 4e-9 rad; 1e-4 is room for a texel rounding the other way |
| `--resize` | 0 of 131072 surface floats differ | exact | a same-aspect resize keeps the grid, so nothing may change |
| `--faraday-off` | 0 differing floats with the other five controls at their far ends | exact | Shake 0 runs none of it |
| `--rate` | 60 vs 30 fps, RMS 2.07e-5 | 1.5e-3 | each run within `--mathieu`'s budget of the truth, so within twice it of each other |
| `--state-shaken` | 0 wrong | exact | as `--state`, with every Faraday pass run |
| `offcompare.sh` | 122 files at two rasters bit-identical to 64f9690's build | exact | two processes on one GPU are deterministic. GPU only: the software renderer is not deterministic across builds |

`--faraday-negative` runs 18 deliberately wrong models; all 18 fail.
`offcompare.sh --negative` sets Shake to 0.001 and must find differences
(54 of 122 files). The pipe check reads `PIPESTATUS` and has `yes | head` as
its control (141).

### The shader mutants

`tools/mutate.py` makes one-character mutants of the four Faraday shaders
and runs the twelve Faraday checks against each at 320×180, on a copy of the
tree, 120 s a check (a timeout counts as caught). As of the last run, 52 of
52 caught:

| id | shader | mutation | caught by |
| --- | --- | --- | --- |
| E1 | envelope | `state.y / OmegaC` → `*` | mathieu, half, wavelength, saturation, pattern, onset |
| E2 | envelope | `+ rate` → `- rate` | saturation |
| E3 | envelope | `RateScale * state.y` → `state.z` | mathieu, half, saturation, pattern, onset |
| K1 | kernel | `1.0 - exp` → `+` | pattern |
| K2 | kernel | `( k - centre )` → `+` | pattern |
| K3 | kernel | notch at √2 → 2.41 | pattern |
| K4 | kernel | notch at √3 → 2.73 | pattern |
| K5 | kernel | `Pattern == 1` → `0` | pattern |
| K6 | kernel | `Norm * kernel` → `+` | half, pattern |
| S1 | state | envelope `.x` → `.y` | half, saturation, pattern |
| S2 | state | `SatRate * envelope` → `+` | mathieu, half, wavelength, saturation, strobe, pattern, onset |
| S3 | state | `state.y * keep` → `/` | half, wavelength, saturation, pattern |
| S4 | state | expNeg series `0.5` → `0.6` | saturation |
| V1 | evolve | `+ WallDamping` → `-` | mathieu, saturation, onset |
| V2 | evolve | `Accel * k` → `/` | mathieu, half, wavelength, seed, saturation, strobe, pattern, onset |
| V3 | evolve | Magnus `0.1443` → `0.2443` | mathieu, rate, onset |
| V4 | evolve | `omega2 - q * drive.x` → `+` | mathieu, half, wavelength, saturation, strobe, pattern, onset |
| V5 | evolve | `drive.y` → `drive.x` | mathieu, rate, onset |
| V6 | evolve | `( w1 + w2 )` → `-` | mathieu, half, saturation, rate, strobe, pattern, onset |
| V7 | evolve | `corr * ( w2 - w1 )` → `+` | mathieu, rate, onset |
| V8 | evolve | `- d * d` → `+` | mathieu |
| V9 | evolve | C series `1/2` → `1/3` | mathieu, wavelength, seed, saturation, rate, onset |
| V10 | evolve | S series `1/6` → `1/7` | mathieu, saturation, onset |
| V11 | evolve | series branch `< 1.0` → `< 9.0` | mathieu |
| V12 | evolve | `decay * cs.x` → `/` | mathieu, seed, saturation |
| V13 | evolve | `cs.y / psi` → `*` | mathieu, seed, saturation |
| V14 | evolve | `exp( psi - gamma * h )` → `+` | mathieu |
| V15 | evolve | `( slow + fast )` → `-` | mathieu |
| V16 | evolve | `( d + gamma * h ) * eta` → `-` | mathieu, wavelength, saturation, rate, pattern, onset |
| V17 | evolve | `2.0 * gamma * d` → `3.0` | mathieu |
| V18 | evolve | `( gamma * h + d ) * rate` → `-` | mathieu, rate, onset |
| V19 | evolve | drive table row `0` → `1` | mathieu, half, wavelength, saturation, strobe, pattern, onset |
| V20 | evolve | substep loop from 1 | mathieu, saturation, rate, strobe, onset |
| V21 | evolve | `expNeg( gamma * h )` → `/` | mathieu, half, wavelength, saturation, strobe, pattern, onset |
| V22 | evolve | `rate / RateScale` → `*` | every check but clock, resize and state-shaken |
| V23 | evolve | `( diff.y, -diff.x )` → `+` | mathieu, seed, rate, strobe, pattern, onset |
| V24 | evolve | `A2.x - B2.y` → `+` | mathieu, seed, rate, strobe, pattern, onset |
| V25 | evolve | `- kv.y * A2` → `+` | half |
| V26 | evolve | Nyquist `/ 2` → `/ 3` | half, wavelength, seed, saturation |
| V27 | evolve | cosSin `1/120` → `1/130` | mathieu |
| V28 | evolve | cosSin quadrant 1 sign | mathieu |
| V29 | evolve | cosSin quadrant 2 sign | mathieu |
| V30 | evolve | cosSin quadrant 3 sign | mathieu |
| V31 | evolve | cosSin quadrant 0 `c` → `s` | mathieu |
| D1 | seed | `SeedAmp > 0.0` → `<` | half, wavelength, seed, saturation, pattern |
| D2 | seed | `SeedAmp * sqrt` → `/` | half |
| D3 | seed | band `-0.25` → `-0.05` | seed |
| D4 | seed | TwiddleY conjugate sign | seed |
| D5 | seed | Hermitian partner sign | half, seed |
| D6 | seed | `( k - Wavenumber )` → `+` | half, seed |
| D7 | seed | mirror index `cell.y` → `cell.x` | half, seed |

Survivors along the way, and what caught them: E2 and S4 (the saturation's
envelope and its series) by `--saturation`'s frame-rate and uniform-level
laws; V25 (a slope's sign) by `--half`'s slope check; D5 by `--seed`'s
displacement check; V8, V11, V14, V15 and V17 by `--mathieu`'s syrup and
short-wave cases; V28 by the quadrant cases.

### Measured once by hand, not by a check

Each by a temporary patch to the harness, run and reverted, at the defaults
(10 Hz, Damping 2, Squares, Slow Motion 8, the 1.5 m pond at Detail 1024)
unless stated:

- **Stands up and lies down.** Under a steady drone at full Shake the
  lattice reaches 1.1 mm RMS within 2 s of starting from still water. When
  the bass stops it falls to 0.32 mm in 0.5 s, 0.11 mm in 1 s and 12 µm in
  2 s: the Damping's 2/s.
- **Steepness at saturation** (10 Hz, 0.5 m pond, 20 s): k_c Ã at its
  largest is 0.14 / 0.14 / 0.18 at 2× (Stripes / Squares / Hexagons) and
  0.20 / 0.27 / 0.44 at 4×. Hexagons peak where three waves add.
- **The mean level** is not exactly conserved by the saturation (a
  multiplication by a field that is not uniform): it wanders within 0.8 µm
  under a full lattice and keeps what it had when the lattice lies down.
  Invisible: a uniform level has no slope.
- **Magnus truncation** per substep stays at or under 2.5e-4 relative from
  ψ = 0.09 to ψ = 40 (5 Hz, 0.2 m pond, Detail 2048 for the large ψ); the
  worst is near ψ = 2π.
- **Shake > 0 in silence** still changes the picture (the default scene,
  640×360, after 5 s, Slow Motion 1): with Damping 0 by at most 8 levels and
  a mean of 0.034 on 255 (the seed and the Magnus path are not v0.1.0's);
  with the default Damping 2 by at most 20 and a mean of 1.1, because
  Damping calms every rain ripple too. And Slow Motion acts whenever Shake is
  above 0, so at the default 8× the rain judders in silence.

### What is not verified

- **Never in a host.** Not loaded in Arena on any platform, not built on
  Windows, never run on a GPU that is not this Mac's. The software renderer
  in these checks is Apple's, not win-lab's llvmpipe.
- **The model, not nature.** Damped Mathieu with Lamb's damping is
  quantitatively wrong against the exact viscous Floquet threshold (Kumar &
  Tuckerman §5); `--onset` checks the model's tongue. Damping stands in for
  every wall loss at one rate for every k, which real walls are not.
- **Linear waves at steepness up to 0.44.** No breaking, no droplets, no
  detuning by amplitude (Miles 1984). The saturation constant 150 is chosen.
- **Pattern selection** is the kernel's, not the fluid's (Chen & Viñals
  derive g(θ) from the Navier–Stokes equations; this does not).
- **The bass envelope's time constants** are not checked: the sweep shows
  Bass Bins is live, and the checks feed a drone. Resolume's bins are
  unmeasured.
- **The demo** (`demo/`) has no Faraday: it carries the eight v0.1.0 shaders,
  and the four new ones are not in `demo/plugin.js`.

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
