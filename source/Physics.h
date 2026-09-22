#pragma once

#include <cstdint>
#include <vector>

/**
    The physics that lives on the CPU: the dispersion relation, the skimmed
    stone, the rain, and the grid.

    The dispersion relation is also written in GLSL, in the evolve pass, and
    the two are marked `//= mirrored` line for line. It is the one formula the
    whole plugin is: every ring, every quiet disc, every wave held back by
    shallow water comes out of it. The CPU copy exists so `mptest` can predict
    what the GPU's rings must do from the same constants -- the group velocity,
    the minimum of it, the Airy scale of the edge -- without asking the GPU.
*/
namespace millpond
{

/// The pond's constants, in SI.
struct Water
{
	double gravity   = 9.81;   ///< m/s^2
	double tension   = 7.28e-5;///< surface tension over density, m^3/s^2
	double depth     = 0.2;    ///< m
	double viscosity = 1e-6;   ///< kinematic, m^2/s
};

/// Angular frequency of a wave of wavenumber k (rad/m), in rad/s:
///
///     omega^2 = ( g k + (sigma/rho) k^3 ) tanh( k h )
///
/// Capillary-gravity waves on water of finite depth. Exact linear theory.
double Omega( double k, const Water& water );

/// Group velocity d(omega)/dk, m/s. Central difference on Omega, with a step
/// small enough that its error is far below anything the harness can measure.
double GroupVelocity( double k, const Water& water );

/// Lamb's viscous damping rate for the AMPLITUDE of a wave: 2 nu k^2, 1/s.
double DampingRate( double k, const Water& water );

/// The minimum group velocity and where it sits. Surface tension makes the
/// group velocity turn back up at short wavelengths, so there is a slowest
/// speed any ripple can carry energy at -- 0.1776 m/s for clean deep water --
/// and nothing can be inside a disc growing at that speed. With no surface
/// tension there is no minimum and this returns k = 0.
struct GroupMinimum
{
	double k        = 0.0;///< rad/m
	double velocity = 0.0;///< m/s
	double third    = 0.0;///< d^3 omega / dk^3 there, m^3/s: the Airy scale
};
GroupMinimum FindGroupMinimum( const Water& water );

//---------------------------------------------------------------------------
// Events: something that puts a crater into the water.
//---------------------------------------------------------------------------
struct Impact
{
	double time      = 0.0; ///< sim seconds
	float x          = 0.0f;///< metres from the frame's left edge
	float y          = 0.0f;///< metres from the frame's bottom edge
	float radius     = 0.01f;///< crater radius a, metres
	float amplitude  = 0.0f; ///< crater depth, metres (signed: positive is a dip)
};

/// A skimmed stone, launched at `start` along `headingDegrees`.
///
/// Each bounce costs the same energy (Bocquet, Am. J. Phys. 71, 2003), so
/// after n bounces V_n^2 = V_0^2 (1 - n / N). Between bounces the stone is a
/// projectile leaving at `angleDegrees`, so a hop covers
/// d_n = 2 V_n^2 sin(b) cos(b) / g in t_n = 2 V_n sin(b) / g. The hops
/// therefore get shorter *linearly* and quicker as a square root, which is the
/// signature of a real skim. The Nth touch has nothing left and is the sink:
/// a full-size plop rather than a skip.
///
/// Touches are a quarter smaller than the stone (it skims, it does not dig in)
/// and their depth follows the vertical speed V_n sin(b), so the rings fade
/// down the line of bounces.
std::vector< Impact > SkimSchedule( double launchTime, float startX, float startY, float headingDegrees,
                                    float speed, float angleDegrees, int bounces, float stoneRadius,
                                    float splash, double gravity );

/// Where a skim along `headingDegrees` that passes through (px, py) enters the
/// frame -- the first touch, a stone's radius or two inside the bank it was
/// thrown from. Frame is `width` x `height` metres.
void SkimStart( float px, float py, float headingDegrees, float width, float height, float inset,
                float& startX, float& startY );

//---------------------------------------------------------------------------
// Randomness. Integer only, the same on every machine.
//---------------------------------------------------------------------------
class Random
{
public:
	explicit Random( uint64_t seed = 0x853c49e6748fea9bULL );

	uint32_t Next();
	double Uniform();///< [0, 1)

	/// A Poisson-distributed count with this mean. Knuth's product method,
	/// which is exact and fast for the small means a frame of rain has; means
	/// past 30 go through a normal approximation, which is a frame with more
	/// drops in it than the event array can take anyway.
	int Poisson( double mean );

private:
	uint64_t state;
};

//---------------------------------------------------------------------------
// The grid.
//---------------------------------------------------------------------------
struct Grid
{
	int nx = 0, ny = 0;     ///< cells, both powers of two
	double lx = 0.0, ly = 0.0;///< the simulated domain, metres: twice the frame
};

/// The grid for a frame of this aspect at this Detail. The domain is the
/// frame mirrored to 2W x 2H; `longCells` goes along its longer side and the
/// other side gets whichever power of two makes the cells closest to square.
Grid ChooseGrid( float frameWidthMetres, float frameHeightMetres, int longCells );

} // namespace millpond
