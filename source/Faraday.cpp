#include "Faraday.h"

#include <algorithm>
#include <cmath>

namespace millpond::faraday
{
namespace
{
constexpr double kTwoPi = 6.28318530717958647692;

constexpr float kAttack   = 0.005f;
constexpr float kRelease  = 0.080f;
constexpr float kPeakTau  = 3.0f;
constexpr float kPeakFloor = 0.02f;

/// A one-pole coefficient, exact for the frame's length; 1 (snap) when no time
/// passed, which is what primes the envelope on the first frame.
float coefficient( double dt, float tau )
{
	if( dt <= 0.0 )
		return 1.0f;
	return 1.0f - static_cast< float >( std::exp( -dt / tau ) );
}
} // namespace

double Wrap( double phase )
{
	const double wrapped = std::fmod( phase, kTwoPi );
	return wrapped < 0.0 ? wrapped + kTwoPi : wrapped;
}

double CriticalWavenumber( double omega, const Water& water )
{
	if( !( omega > 0.0 ) )
		return 0.0;

	//Omega is monotonic in k (g k + tension k^3 and tanh( k h ) both rise), so
	//bisect in log k across every wavenumber a pond could hold.
	double lo = 1e-4, hi = 1e7;
	for( int i = 0; i < 200; ++i )
	{
		const double mid = std::sqrt( lo * hi );
		if( Omega( mid, water ) < omega )
			lo = mid;
		else
			hi = mid;
	}
	return std::sqrt( lo * hi );
}

double ThresholdAcceleration( double k, double omega0, double gamma, double depth )
{
	const double forcing = k * std::tanh( std::min( k * depth, 20.0 ) );
	if( forcing <= 0.0 )
		return 0.0;
	return 4.0 * gamma * omega0 / forcing;
}

double ShakenDamping( double k, double viscosity, double walls )
{
	return 2.0 * viscosity * k * k + walls;//= mirrored
}

//---------------------------------------------------------------------------
void Bass::Reset()
{
	envelope = peak = 0.0f;
}

void Bass::Update( const float* bins, int count, int bassBins, double dt )
{
	const int n = std::clamp( std::min( bassBins, count ), 0, 64 );
	float sum   = 0.0f;
	for( int i = 0; i < n; ++i )
		sum += std::sqrt( std::max( 0.0f, bins ? bins[ i ] : 0.0f ) );
	const float raw = n > 0 ? sum / static_cast< float >( n ) : 0.0f;

	const float coeff = coefficient( dt, raw >= envelope ? kAttack : kRelease );
	envelope += ( raw - envelope ) * coeff;

	const float fall = coefficient( dt, kPeakTau );
	peak             = std::max( envelope, peak - ( peak - kPeakFloor ) * fall );
	peak             = std::max( peak, kPeakFloor );
}

float Bass::Level() const
{
	return std::clamp( envelope / std::max( peak, kPeakFloor ), 0.0f, 1.0f );
}

//---------------------------------------------------------------------------
void Strobe::Reset()
{
	waterPhase = seenPhase = lag = 0.0;
}

double Strobe::Advance( double nominal, double omegaC, double slow )
{
	if( !( omegaC > 0.0 ) || nominal <= 0.0 )
		return std::max( nominal, 0.0 );

	if( slow <= 1.0 + 1e-9 )
	{
		//No stroboscope: the water keeps the nominal clock.
		waterPhase = Wrap( waterPhase + omegaC * nominal );
		seenPhase  = waterPhase;
		lag        = 0.0;
		return nominal;
	}

	//The phase the camera should see, S times slower than the water's.
	seenPhase = Wrap( seenPhase + omegaC * nominal / slow );

	//The smallest forward step that lands the water on that phase, plus as
	//many whole response periods as keep it from falling behind the nominal
	//clock. `lag` stays in [0, period): the water is never ahead of the
	//nominal clock and never more than one response period behind it.
	const double period = kTwoPi / omegaC;
	const double base   = Wrap( seenPhase - waterPhase ) / omegaC;
	const double target = lag + nominal;
	const double whole  = std::max( 0.0, std::floor( ( target - base ) / period ) );
	const double step   = base + whole * period;

	lag        = target - step;
	waterPhase = seenPhase;
	return step;
}

//---------------------------------------------------------------------------
int DriveTable( double theta0, double omega, double dt, std::vector< float >& table )
{
	const double periods = dt * omega / kTwoPi;
	const int steps      = std::clamp( static_cast< int >( std::ceil( periods * kStepsPerPeriod ) ), 1, kMaxSteps );
	const double h       = dt / steps;

	//Gauss-Legendre, two points: the step's middle plus or minus h / ( 2 sqrt 3 ).
	const double offset = 0.5 / std::sqrt( 3.0 );
	table.resize( static_cast< size_t >( steps ) * 2 );
	for( int i = 0; i < steps; ++i )
	{
		const double middle = ( static_cast< double >( i ) + 0.5 ) * h;
		table[ static_cast< size_t >( i ) * 2 + 0 ] =
			static_cast< float >( std::cos( theta0 + omega * ( middle - offset * h ) ) );
		table[ static_cast< size_t >( i ) * 2 + 1 ] =
			static_cast< float >( std::cos( theta0 + omega * ( middle + offset * h ) ) );
	}
	return steps;
}

} // namespace millpond::faraday
