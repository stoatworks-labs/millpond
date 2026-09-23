# Attributions

millpond is built on other people's work. This file lists what that work is,
who did it, and what it is doing here.

> **Provisional.** Across the fleet this file is generated from master lists in
> `stoatworks-backend` by `scripts/sync-attributions.py`. millpond is in that
> script's `names.json` but not in its component lists, so this copy is still
> hand-written; v0.1.0 shipped that way. Finishing the registration and re-running
> the sync is the fix — and note that the script's `--only` flag truncates the
> file rather than filtering it.

## Third-party code this project uses

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>
Licence: BSD-3-Clause
Copyright: FreeFrame

Vendored as a git submodule at `external/ffgl`, pinned to `b1afaf9`.

The plugin ABI itself. An FFGL effect is defined by this SDK's headers — there
is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Windows only, from vcpkg, statically linked. The SDK's headers pull it in for
the OpenGL function pointers; macOS uses the system OpenGL framework instead.

### zlib

<https://zlib.net>
Licence: zlib
Copyright: Jean-loup Gailly and Mark Adler

Ships with macOS. The offline harness links it to deflate its PNG output.
Nothing in the shipped plugin uses it.

## Work from elsewhere in the fleet

### rosette — the audio analyser and the clock

<https://github.com/stoatworks-labs/rosette>
Licence: MIT
Copyright: Stoatworks Labs

`source/Audio.{h,cpp}` is rosette's analyser (itself from macroblock's), with
its primed first frame. That frame is why `mptest --audio` expects 19 pebbles
from 20 hits and not 20. The host-clock unit vote in `UpdateClock` is rosette's,
unchanged.

### vectrix, tinsel, intaglio

<https://github.com/stoatworks-labs/vectrix>
Licence: MIT
Copyright: Stoatworks Labs

`GLState.h` (put the host's blend state back however the frame ends) is
vectrix's, which took it from resolume-scopes. `PassBuffer` (`FFGLFBO` with the
SDK's colour-texture leak fixed; a wrap mode added here for the periodic
surface), `Diag`, the CMake shape, the harness shape, the `--pipe`/`--script`
format, `tools/sweep.py` and `tools/verify.sh` come from tinsel and intaglio.

## Method

Textbook physics, described in books and papers rather than copied from
anyone's source:

- Linear water waves with gravity, surface tension and finite depth, and
  Lamb's viscous damping rate 2νk² — Lamb, *Hydrodynamics*.
- The Cauchy–Poisson initial-value problem, which `mptest` evaluates exactly as
  a Hankel integral to check the rings against; the Airy structure at the
  minimum group velocity.
- The method of images for a rectangular basin with reflecting walls.
- Snell's law and the unpolarised Fresnel equations.
- The equal-energy-per-bounce model of a skimmed stone — L. Bocquet, "The
  physics of stone skipping", *Am. J. Phys.* 71, 150 (2003),
  [doi:10.1119/1.1519232](https://doi.org/10.1119/1.1519232).
- J0 in the harness is its power series and Hankel's asymptotic expansion,
  written out from the definitions (checked against SciPy: 5e-13 and 3e-11).

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or
you would rather not be listed — open an issue and it will be fixed.
