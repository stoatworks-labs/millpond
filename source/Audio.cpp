#include "Audio.h"

#include <algorithm>
#include <cmath>

namespace millpond::audio
{
namespace
{
/// A one-pole coefficient for a time constant, given the frame length. Returns
/// 1 -- snap straight to the target -- for a time constant of zero.
float coefficient( float dt, float tau )
{
	if( tau <= 1e-4f || dt <= 0.0f )
		return 1.0f;

	return 1.0f - std::exp( -dt / tau );
}

/// The floor under the normalising peak. Without it, silence divides a level of
/// zero by a peak of zero, and the first faint sound after it reads as full
/// scale -- so the pond would erupt in the gap between tracks.
constexpr float kPeakFloor = 0.02f;

/// The peak's decay. Long enough that a bar of quiet does not re-normalise the
/// track, short enough that plugging in a different source is not a minute of
/// nothing happening.
constexpr float kPeakTau = 3.0f;

/// An onset cannot be followed by another within this. 80 ms is 750 bpm in
/// straight quavers, so it costs nothing musically and it stops a single hit
/// with a ragged envelope from firing three times.
constexpr float kRefractory = 0.08f;

/// The absolute floor on spectral flux. The adaptive threshold alone divides
/// noise by noise during silence and finds onsets in it.
constexpr float kFluxFloor = 0.004f;

float clamp01( float v )
{
	return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
}
} // namespace

void Analyser::Reset()
{
	binLevel.fill( 0.0f );
	binPrevious.fill( 0.0f );
	level = peak = flux = fluxMean = held = refractory = 0.0f;
	fired  = false;
	primed = false;
}

void Analyser::Update( const float* bins, int count, float dt, const Settings& settings )
{
	const int n = std::clamp( count, 0, kBins );

	const float attack   = coefficient( dt, std::max( 0.0f, settings.attackSeconds ) );
	const float release  = coefficient( dt, std::max( 0.0f, settings.releaseSeconds ) );
	const float peakFall = coefficient( dt, kPeakTau );
	const float fluxFall = coefficient( dt, 1.0f );
	const float holdFall = coefficient( dt, std::max( 0.0f, settings.holdSeconds ) );

	float sum      = 0.0f;
	float riseSum  = 0.0f;
	for( int i = 0; i < n; ++i )
	{
		//sqrt because bin magnitudes bunch hard against zero: a spectrum used
		//raw responds to the kick drum and to nothing else in the mix.
		const float raw = std::sqrt( std::max( 0.0f, bins ? bins[ i ] : 0.0f ) );

		//Flux is measured between two RAW frames, never against the envelope.
		//An envelope is a low-pass, and low-passing a signal before asking
		//where its corners are is asking the wrong signal.
		riseSum += std::max( 0.0f, raw - binPrevious[ i ] );
		binPrevious[ i ] = raw;

		const float coeff = raw >= binLevel[ i ] ? attack : release;
		binLevel[ i ] += ( raw - binLevel[ i ] ) * coeff;
		sum += binLevel[ i ];
	}

	//A host that hands over fewer bins than it declared leaves the rest where
	//they were, which would freeze part of the spectrum at whatever was playing
	//when it stopped.
	for( int i = n; i < kBins; ++i )
	{
		binLevel[ i ] *= ( 1.0f - release );
		binPrevious[ i ] = 0.0f;
	}

	const float span = static_cast< float >( std::max( n, 1 ) );
	level = sum / span;
	flux  = riseSum / span;

	//---------------------------------------------------------------------
	// The cold start.
	//
	// On the very first frame there is no previous frame. `binPrevious` is a
	// buffer of zeroes, so every bin reads as having risen from silence and
	// the flux comes out enormous -- and because `dt` is zero on that frame
	// every one-pole below snaps rather than filters, so `fluxMean` takes
	// that whole number as its adaptive floor. The bar is then several times
	// any real onset and decays with a one-second time constant, which
	// leaves the detector DEAF for about a second and a half.
	//
	// It is not a harness artefact: it happens in the host every time audio
	// starts, a clip is triggered, or the analyser is reset -- exactly the
	// moments an operator is watching for the first kick. Measured on the
	// harness's own feed before the fix: a bar of 1.44 against hits of 0.48,
	// and 1 onset detected in the first two seconds instead of 3.
	//
	// So the first frame establishes what "previous" means and reports no
	// flux at all. Nothing rose; there was nothing for it to rise from.
	//---------------------------------------------------------------------
	if( !primed )
	{
		primed = true;
		flux   = 0.0f;
	}

	peak = std::max( level, peak - ( peak - kPeakFloor ) * peakFall );
	peak = std::max( peak, kPeakFloor );

	fluxMean += ( flux - fluxMean ) * fluxFall;

	//Higher sensitivity, lower bar. The span is wide because the useful
	//setting depends enormously on the material: a compressed master has
	//almost no flux and a live drum kit has nothing but.
	const float fluxMargin = 2.5f - 2.35f * clamp01( settings.sensitivity );

	fired      = false;
	refractory = std::max( 0.0f, refractory - dt );

	const float bar = std::max( kFluxFloor, fluxMean * ( 1.0f + fluxMargin ) );
	if( refractory <= 0.0f && flux > bar )
	{
		fired      = true;
		refractory = kRefractory;
		++onsets;
	}

	const float normalised = clamp01( level / peak );
	if( fired )
	{
		//Latch the level the hit arrived at, so a soft hit kicks a little and
		//a hard one kicks a lot. Latching a constant 1 instead would make
		//every kick identical, which is a metronome rather than a response.
		held = std::max( held, normalised );
	}
	else
	{
		held -= held * holdFall;
	}
}

float Analyser::Level() const
{
	return clamp01( level / std::max( kPeakFloor, peak ) );
}

float Analyser::Kick() const
{
	return clamp01( held );
}

} // namespace millpond::audio
