#pragma once

/**
    The passes, as GLSL source.

    ---------------------------------------------------------------- the waves

    The surface lives on an Nx x Ny grid covering the frame mirrored to 2W x 2H
    metres, periodic. One frame of water is:

        1. **inject**   grid, RG32F. Last frame's (eta, eta_t), times the sponge
                        (Open banks), plus this frame's craters (and their
                        mirror images, for Walls).
        2. **fft**      grid, RG32F, log2 Nx + log2 Ny Stockham passes. The
                        complex field eta + i eta_t, forward.
        3. **evolve**   grid, RGBA32F. Unpack the two real spectra, advance
                        every mode by dt EXACTLY as a damped oscillator with the
                        dispersion relation's omega(k) and Lamb's damping, and
                        pack two complex fields for the way back: (eta + i
                        eta_t) and (d/dx eta + i d/dy eta).
        4. **fft**      grid, RGBA32F, the same passes, inverse. Out comes
                        (eta, eta_t, eta_x, eta_y) in real space: this frame's
                        surface AND next frame's state, in one texture.

    The transform is the same shader both ways and for both widths: RG buffers
    simply ignore .ba. There is no time step limit anywhere in this chain --
    the propagator is exact for each mode, so a 30 fps host and a 240 fps host
    ring the same pond.

    --------------------------------------------------------------- the light

        5. **caustic**  caustic buffer, R32F, additive. A mesh laid over the
                        frame whose vertices are moved to where refracted
                        sunlight lands on the bed. Each triangle carries its
                        own area over its landed area, and every pixel centre
                        it covers receives that -- so light is conserved per
                        triangle, and where the water folds the light over,
                        the layers add because the blend adds.
        6. **blur**     the same buffer, twice (H, V). The sun's angular size,
                        seen from the bed: sigma = depth * tan( radius ).
        7. **composite** output size. Refract the view ray to the bed, read the
                        picture and the caustic there, add the Fresnel
                        reflection of the sky and the sun's glints.

    ------------------------------------------------------------------ mirroring

    The dispersion relation and the damping rate are written in C++ as well
    (Physics.cpp) and marked `//= mirrored` on both sides, because the harness
    predicts what the rings must do from them. Nothing else is mirrored: the
    transforms, the optics and the mesh are GPU-only, and the harness checks
    them against the textbook (a double-precision DFT, Snell, Fresnel,
    conservation of light) rather than against a C++ copy of themselves.
*/

#include <string>

namespace millpond
{

extern const char* const kVertexShader;
extern const char* const kInjectShader;
extern const char* const kFFTShader;
extern const char* const kEvolveShader;
extern const char* const kCausticVertexShader;
extern const char* const kCausticFragmentShader;
extern const char* const kBlurShader;
extern const char* const kCompositeShader;

/// eta_t is carried as eta_t / kRateScale everywhere on the GPU -- in the
/// surface texture and through both transforms.
///
/// The two fields share one complex transform, and a transform's rounding is
/// relative to the LARGER of what it carries. eta_t is about omega times eta,
/// and omega runs to hundreds of rad/s at the ripples' end of the spectrum, so
/// unscaled it drowned eta in its own rounding: the walled pond's mirror
/// symmetry drifted 7e-4 in ten seconds. 40 rad/s is the frequency at the
/// minimum group velocity, the middle of what a pebble rings.
constexpr float kRateScale = 40.0f;

/// The most craters one frame can put into the water. Rain at full with the
/// speed at 4x asks for about 5 a frame; this is headroom for an audio onset,
/// a skim's sink and a burst of button presses landing on the same frame.
constexpr int kMaxDropsPerFrame = 64;

/// The widest blur the caustic pass will make to one side, in taps. Past this
/// the step stretches rather than the loop growing.
constexpr int kMaxBlurTaps = 24;

} // namespace millpond
