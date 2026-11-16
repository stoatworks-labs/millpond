#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace millpond
{
namespace
{
/// value^0 = low, value^1 = high, with every octave the same width of travel.
float geometric( float value, float low, float high )
{
	return low * std::pow( high / low, std::clamp( value, 0.0f, 1.0f ) );
}

float linear( float value, float low, float high )
{
	return low + ( high - low ) * std::clamp( value, 0.0f, 1.0f );
}

constexpr float kSpeedLow  = 0.05f;
constexpr float kSpeedHigh = 4.0f;
} // namespace

float PondSizeFromParam( float value )
{
	return geometric( value, 0.1f, 20.0f );
}

float DepthFromParam( float value )
{
	return geometric( value, 0.002f, 2.0f );
}

float TensionFromParam( float value )
{
	return linear( value, 0.0f, 2.0f * kWaterTension );
}

float ViscosityFromParam( float value )
{
	return geometric( value, 1e-7f, 1e-2f );
}

float SpeedFromParam( float value )
{
	//Exactly zero at the bottom: a frozen pond is a thing an operator wants,
	//and "very slow" is not the same thing.
	if( value <= 0.0f )
		return 0.0f;
	return geometric( value, kSpeedLow, kSpeedHigh );
}

float ParamFromSpeed( float speed )
{
	if( speed <= 0.0f )
		return 0.0f;
	const float clamped = std::clamp( speed, kSpeedLow, kSpeedHigh );
	return std::log( clamped / kSpeedLow ) / std::log( kSpeedHigh / kSpeedLow );
}

float PebbleSizeFromParam( float value )
{
	return geometric( value, 0.002f, 0.10f );
}

float SplashFromParam( float value )
{
	return linear( value, 0.0f, 0.6f );
}

float ScatterFromParam( float value )
{
	return linear( value, 0.0f, 0.5f );
}

float HeadingFromParam( float value )
{
	return linear( value, 0.0f, 360.0f );
}

float ThrowSpeedFromParam( float value )
{
	return geometric( value, 0.5f, 20.0f );
}

float SkimAngleFromParam( float value )
{
	return linear( value, 3.0f, 40.0f );
}

int BouncesFromParam( float value )
{
	return std::clamp( static_cast< int >( std::lround( linear( value, 1.0f, 40.0f ) ) ), 1, 40 );
}

float RainFromParam( float value )
{
	const float v = std::clamp( value, 0.0f, 1.0f );
	return 80.0f * v * v * v;
}

float RainSizeFromParam( float value )
{
	return geometric( value, 0.001f, 0.02f );
}

float CausticsFromParam( float value )
{
	return linear( value, 0.0f, 2.0f );
}

float SunElevationFromParam( float value )
{
	return linear( value, 10.0f, 90.0f );
}

float SunAzimuthFromParam( float value )
{
	return linear( value, 0.0f, 360.0f );
}

float SunSizeFromParam( float value )
{
	return geometric( value, 0.1f, 10.0f );
}

float ReflectionFromParam( float value )
{
	return linear( value, 0.0f, 3.0f );
}

float GlintFromParam( float value )
{
	const float v = std::clamp( value, 0.0f, 1.0f );
	return 5000.0f * v * v * v;
}

//---------------------------------------------------------------------------
namespace
{
constexpr float kDriveLow    = 5.0f, kDriveHigh = 120.0f;
constexpr float kDampingLow  = 0.1f, kDampingHigh = 30.0f;
constexpr double kSlowLow    = 1.0, kSlowHigh = 100.0;
constexpr float kMaxOverdrive = 4.0f;
constexpr int kMaxBassBins   = 16;

float inverseGeometric( float value, float low, float high )
{
	const float clamped = std::clamp( value, low, high );
	return std::log( clamped / low ) / std::log( high / low );
}
} // namespace

float ShakeFromParam( float value )
{
	return kMaxOverdrive * std::clamp( value, 0.0f, 1.0f );
}

float DriveFromParam( float value )
{
	return geometric( value, kDriveLow, kDriveHigh );
}

float ParamFromDrive( float hertz )
{
	return inverseGeometric( hertz, kDriveLow, kDriveHigh );
}

float DampingFromParam( float value )
{
	//Exactly zero at the bottom: Lamb's damping alone, a tank with no walls.
	if( value <= 0.0f )
		return 0.0f;
	return geometric( value, kDampingLow, kDampingHigh );
}

float ParamFromDamping( float rate )
{
	if( rate <= 0.0f )
		return 0.0f;
	return inverseGeometric( rate, kDampingLow, kDampingHigh );
}

double SlowMotionFromParam( float value )
{
	return kSlowLow * std::pow( kSlowHigh / kSlowLow, static_cast< double >( std::clamp( value, 0.0f, 1.0f ) ) );
}

float ParamFromSlowMotion( double factor )
{
	const double clamped = std::clamp( factor, kSlowLow, kSlowHigh );
	return static_cast< float >( std::log( clamped / kSlowLow ) / std::log( kSlowHigh / kSlowLow ) );
}

int BassBinsFromParam( float value )
{
	return std::clamp( static_cast< int >( std::lround( linear( value, 1.0f, static_cast< float >( kMaxBassBins ) ) ) ), 1,
	                   kMaxBassBins );
}

float ParamFromBassBins( int bins )
{
	return static_cast< float >( std::clamp( bins, 1, kMaxBassBins ) - 1 ) / static_cast< float >( kMaxBassBins - 1 );
}

} // namespace millpond
