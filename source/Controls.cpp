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
	return linear( value, 0.0f, 30.0f );
}

} // namespace millpond
