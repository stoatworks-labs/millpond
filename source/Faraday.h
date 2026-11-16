#pragma once

#include "Physics.h"

#include <vector>

/**
    The CPU half of the Faraday addition: the tank shaken up and down by the
    bass. The spec is `~/Projects/resolume/specs/SPEC-millpond-faraday.md`;
    AGENTS.md has the reasoning and the traps.

    In the tank's frame gravity is g - a cos( theta ), and every Fourier mode
    becomes a damped Mathieu oscillator (Benjamin & Ursell 1954, eq. 2.12):

        eta'' + 2 gamma eta' + ( omega0^2 - a k tanh( k h ) cos theta ) eta = 0

    The GPU advances that (Shaders.h, the Faraday evolve pass). What lives here
    is what the GPU is handed: the drive's phase, kept in double and reduced
    every frame; the cosines of it at every substep's Gauss points; the
    stroboscope that decides how much water time each frame covers; the bass
    envelope; and the leading-order threshold that the Shake control is
    measured in.
*/
namespace millpond::faraday
{

/// Substeps per drive period for the fourth-order Magnus integrator, and the
/// most one frame may take. 32 a period puts the scheme's truncation far
/// below float rounding (AGENTS.md has the measurement).
constexpr int kStepsPerPeriod = 32;
constexpr int kMaxSteps       = 1024;

/// The saturating damping's strength, in units of gamma_c k_c^2 (Milner's
/// nu k^4 scaling when the damping is viscous). Chosen, not derived: a lattice
/// at full Shake stands at a steepness k A of about 0.2.
constexpr double kSaturation = 150.0;

/// The ambient disturbance the pattern grows from. Each Fourier mode gets what
/// a white field whose RMS a cell, over a frame of dt seconds, is
/// kSeed sqrt( dt / cell area ) would give it -- times a Gaussian band around
/// k_c, kSeedWidth k_c wide. A Wiener process in time, so the noise put in per
/// second does not depend on the frame rate; normalised by the cell's area, so
/// a mode's share does not depend on Detail; banded, because white out to the
/// grid's scale it was visible (up to 19 of 255 levels of caustic sparkle at
/// full Shake in silence) while only the modes near k_c need it.
constexpr double kSeed     = 1e-8;
constexpr float kSeedWidth = 0.25f;

/// The longest stretch of water one shaken pass may cover, seconds, and the
/// most passes a frame may take. The saturation is applied once a pass, ahead
/// of that pass's evolve -- a first-order splitting, whose error grows with
/// the step. The stroboscope advances the water in jumps of whole response
/// periods (0.2 s at 10 Hz), and in a single jump a lattice at 4x threshold
/// grew unchecked: at the defaults, under Slow Motion 8, it stood between
/// 0.97 and 1.68 mm RMS where the unstrobed pond stood between 1.15 and 1.37
/// (`--strobe` holds the two together). So a longer frame is
/// advanced in equal chunks no longer than this: 1/30 s, inside the frame
/// steps `--saturation` holds to one height (1/60 to 1/15 s). A frame that
/// would need more than kMaxChunks takes longer chunks instead.
constexpr double kMaxShakenStep = 1.0 / 30.0;
constexpr int kMaxChunks        = 32;

/// The width (one standard deviation) of the pattern kernel's notches, as a
/// fraction of k_c. Wide enough to keep catching the lattice's pairs when the
/// lattice forms off k_c -- at 0.1 a 2x shake at the old defaults formed 12%
/// high, the notch missed and every Pattern gave the same labyrinth.
constexpr float kNotchWidth = 0.25f;

/// The wavenumber whose free frequency is `omega`: the root of millpond's own
/// dispersion relation, Omega( k ) = omega. Omega rises monotonically with k,
/// so this is a bisection; 0 if omega is not positive.
double CriticalWavenumber( double omega, const Water& water );

/// The first tongue's threshold acceleration at exact resonance, to leading
/// order in the damping: q / omega0^2 = 4 gamma / omega0, so
/// a_c = 4 gamma omega0 / ( k tanh( k h ) ). Kumar & Tuckerman (4.21) with
/// Lamb's damping; Edwards & Fauve's a_c = 8 omega k nu for deep water. Used to
/// scale Shake and nothing else: the harness measures the real onset against
/// the exact Floquet threshold of the same equation.
double ThresholdAcceleration( double k, double omega0, double gamma, double depth );

/// The linear damping the shaken pond's modes have: Lamb's 2 nu k^2 plus the
/// walls' rate. Mirrored in the Faraday evolve pass.
double ShakenDamping( double k, double viscosity, double walls );

//---------------------------------------------------------------------------
// The bass.
//---------------------------------------------------------------------------

/// The bass envelope from the host's FFT buffer: the mean of sqrt( bin ) over
/// the lowest `bassBins` bins (sqrt is the existing analyser's assumption,
/// from rosette), a 5 ms attack and 80 ms release, normalised against its own
/// recent peak (3 s decay, floor 0.02) so the same Shake means the same thing
/// on a quiet stem and a mastered track.
class Bass
{
public:
	void Update( const float* bins, int count, int bassBins, double dt );
	float Level() const;///< 0..1
	void Reset();

private:
	float envelope = 0.0f;
	float peak     = 0.0f;
};

//---------------------------------------------------------------------------
// The stroboscope.
//---------------------------------------------------------------------------

/// Slow Motion, as a stroboscope rather than a slower clock. Each frame the
/// water is advanced to the latest moment, no later than the nominal clock,
/// at which the response phase omega_c t matches a slowed phase
/// omega_c t_nominal / S. So the water runs at real speed on average -- the
/// lattice stands up and lies down at the music's pace -- while the
/// oscillation at omega_c is seen S times slower. Everything else is seen in
/// jumps of one response period, as a stroboscope shows it.
///
/// Both phases are in double and reduced mod 2 pi every frame: the shader is
/// only ever handed a phase in [0, 2 pi) and times relative to the frame.
class Strobe
{
public:
	/// Advance by `nominal` seconds of the pond's own clock (host time times
	/// Speed) at response frequency `omegaC` and slow-down `slow`. Returns the
	/// water time this frame covers. slow <= 1 is no stroboscope at all: the
	/// water advances by exactly `nominal`.
	double Advance( double nominal, double omegaC, double slow );

	/// omega_c t of the water, in [0, 2 pi). The drive's phase is twice it.
	double WaterPhase() const
	{
		return waterPhase;
	}

	/// How far the water is behind the nominal clock, seconds. In [0, 2 pi / omega_c).
	double Lag() const
	{
		return lag;
	}

	void Reset();

private:
	double waterPhase = 0.0;
	double seenPhase  = 0.0;
	double lag        = 0.0;
};

/// The drive's cosine at the two Gauss points of each of the frame's substeps,
/// interleaved (step 0 point 1, step 0 point 2, step 1 point 1 ...), from
/// `theta0` (the drive's phase at the frame's start, radians) at `omega`
/// (rad/s) over `dt` seconds. Computed in double. Returns the substep count;
/// `table` gets twice that many floats.
int DriveTable( double theta0, double omega, double dt, std::vector< float >& table );

/// x mod 2 pi, in [0, 2 pi).
double Wrap( double phase );

} // namespace millpond::faraday
