#include "Physics.h"

#include <algorithm>
#include <cmath>

namespace millpond
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
} // namespace

double Omega( double k, const Water& water )
{
	const double kk = std::max( k, 0.0 );
	const double restoring = water.gravity * kk + water.tension * kk * kk * kk;//= mirrored
	//tanh( k h ) is 1 to double precision by kh = 20; the clamp keeps the
	//GLSL copy, which may compute it through exp( 2x ), inside a float.
	const double depthFactor = std::tanh( std::min( kk * water.depth, 20.0 ) );//= mirrored
	return std::sqrt( std::max( restoring * depthFactor, 0.0 ) );              //= mirrored
}

double GroupVelocity( double k, const Water& water )
{
	const double step = std::max( 1e-6 * k, 1e-6 );
	return ( Omega( k + step, water ) - Omega( std::max( k - step, 0.0 ), water ) )
	       / ( k + step - std::max( k - step, 0.0 ) );
}

double DampingRate( double k, const Water& water )
{
	return 2.0 * water.viscosity * k * k;//= mirrored
}

GroupMinimum FindGroupMinimum( const Water& water )
{
	GroupMinimum result;
	if( water.tension <= 0.0 )
		return result;

	//Bracket, then golden-section. The minimum of the group velocity sits at
	//about 0.39 of the capillary wavenumber sqrt( g / tension ), so a decade
	//either side of that brackets it for any water this plugin can make.
	const double kc = std::sqrt( water.gravity / water.tension );
	double lo       = 0.05 * kc;
	double hi       = 5.0 * kc;

	const double phi = 0.5 * ( std::sqrt( 5.0 ) - 1.0 );
	double a         = hi - phi * ( hi - lo );
	double b         = lo + phi * ( hi - lo );
	double fa        = GroupVelocity( a, water );
	double fb        = GroupVelocity( b, water );
	for( int i = 0; i < 200; ++i )
	{
		if( fa < fb )
		{
			hi = b;
			b  = a;
			fb = fa;
			a  = hi - phi * ( hi - lo );
			fa = GroupVelocity( a, water );
		}
		else
		{
			lo = a;
			a  = b;
			fa = fb;
			b  = lo + phi * ( hi - lo );
			fb = GroupVelocity( b, water );
		}
	}

	result.k        = 0.5 * ( lo + hi );
	result.velocity = GroupVelocity( result.k, water );

	//The curvature of the group velocity there, which is omega'''. A wide
	//step, because this is a second difference of a first difference and a
	//narrow one is all rounding.
	const double h = 0.02 * result.k;
	result.third   = ( GroupVelocity( result.k + h, water ) - 2.0 * result.velocity
                     + GroupVelocity( result.k - h, water ) )
	               / ( h * h );
	return result;
}

//---------------------------------------------------------------------------
std::vector< Impact > SkimSchedule( double launchTime, float startX, float startY, float headingDegrees,
                                    float speed, float angleDegrees, int bounces, float stoneRadius,
                                    float splash, double gravity )
{
	std::vector< Impact > impacts;
	const int n = std::max( bounces, 1 );

	const double heading = headingDegrees * kPi / 180.0;
	const double dx      = std::cos( heading );
	const double dy      = std::sin( heading );
	const double beta    = std::clamp( static_cast< double >( angleDegrees ), 0.5, 89.0 ) * kPi / 180.0;
	const double v0      = std::max( static_cast< double >( speed ), 0.0 );

	double t = launchTime;
	double x = startX;
	double y = startY;

	for( int i = 0; i <= n; ++i )
	{
		//Energy left after i bounces, as a fraction of the throw's.
		const double left  = std::max( 1.0 - static_cast< double >( i ) / static_cast< double >( n ), 0.0 );
		const double vi    = v0 * std::sqrt( left );
		const bool sink    = ( i == n );

		Impact touch;
		touch.time      = t;
		touch.x         = static_cast< float >( x );
		touch.y         = static_cast< float >( y );
		touch.radius    = sink ? stoneRadius : 0.75f * stoneRadius;
		//A touch digs in as hard as it is falling. The sink is the whole
		//stone going under, which is the full crater whatever the speed was.
		touch.amplitude = static_cast< float >( splash * touch.radius * ( sink ? 1.0 : std::sqrt( left ) ) );
		impacts.push_back( touch );

		if( sink )
			break;

		const double hop    = 2.0 * vi * vi * std::sin( beta ) * std::cos( beta ) / gravity;
		const double flight = 2.0 * vi * std::sin( beta ) / gravity;
		x += hop * dx;
		y += hop * dy;
		t += flight;
	}

	return impacts;
}

void SkimStart( float px, float py, float headingDegrees, float width, float height, float inset, float& startX,
                float& startY )
{
	const double heading = headingDegrees * kPi / 180.0;
	const double dx      = std::cos( heading );
	const double dy      = std::sin( heading );

	//Walk BACK from the aim point along the heading until the first frame
	//edge. The line leaves the frame through whichever edge it reaches first.
	double back = 1e30;
	if( dx > 1e-6 )
		back = std::min( back, ( px - inset ) / dx );
	if( dx < -1e-6 )
		back = std::min( back, ( width - inset - px ) / -dx );
	if( dy > 1e-6 )
		back = std::min( back, ( py - inset ) / dy );
	if( dy < -1e-6 )
		back = std::min( back, ( height - inset - py ) / -dy );
	back = std::max( back, 0.0 );

	startX = static_cast< float >( px - back * dx );
	startY = static_cast< float >( py - back * dy );
}

//---------------------------------------------------------------------------
Random::Random( uint64_t seed ) : state( seed )
{
}

uint32_t Random::Next()
{
	//PCG-XSH-RR. Integer throughout, so every machine draws the same rain.
	const uint64_t old = state;
	state              = old * 6364136223846793005ULL + 1442695040888963407ULL;
	const uint32_t xorshifted = static_cast< uint32_t >( ( ( old >> 18u ) ^ old ) >> 27u );
	const uint32_t rot        = static_cast< uint32_t >( old >> 59u );
	return ( xorshifted >> rot ) | ( xorshifted << ( ( -static_cast< int32_t >( rot ) ) & 31 ) );
}

double Random::Uniform()
{
	return static_cast< double >( Next() ) / 4294967296.0;
}

int Random::Poisson( double mean )
{
	if( mean <= 0.0 )
		return 0;

	if( mean > 30.0 )
	{
		//Box-Muller, rounded. Only reachable with the rain at full and the
		//speed at 4x, a frame that saturates the event array regardless.
		const double u1 = std::max( Uniform(), 1e-12 );
		const double u2 = Uniform();
		const double z  = std::sqrt( -2.0 * std::log( u1 ) ) * std::cos( 2.0 * kPi * u2 );
		return std::max( 0, static_cast< int >( std::lround( mean + std::sqrt( mean ) * z ) ) );
	}

	const double limit = std::exp( -mean );
	double product     = Uniform();
	int count          = 0;
	while( product > limit )
	{
		++count;
		product *= Uniform();
	}
	return count;
}

//---------------------------------------------------------------------------
Grid ChooseGrid( float frameWidthMetres, float frameHeightMetres, int longCells )
{
	Grid grid;
	grid.lx = 2.0 * std::max( frameWidthMetres, 1e-4f );
	grid.ly = 2.0 * std::max( frameHeightMetres, 1e-4f );

	const bool wide    = grid.lx >= grid.ly;
	const double ratio = wide ? grid.ly / grid.lx : grid.lx / grid.ly;

	//Closest to square in log terms, so a 16:9 frame at 1024 gets 1024x512
	//(cells 1.125:1) rather than 1024x1024 (1:1.78).
	const int shortCells = std::max(
		8, static_cast< int >( std::lround( std::pow( 2.0, std::round( std::log2( longCells * ratio ) ) ) ) ) );

	grid.nx = wide ? longCells : shortCells;
	grid.ny = wide ? shortCells : longCells;
	return grid;
}

} // namespace millpond
