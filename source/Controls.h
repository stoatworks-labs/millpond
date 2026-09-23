#pragma once

/**
    The host's parameters, and what they mean in physical units.

    Every numeric parameter the host sees is a plain 0..1 float, because
    `SetParamInfo` clamps an `FF_TYPE_STANDARD` default into 0..1 *before*
    `SetParamRange` could widen it -- so a control that stands for a depth in
    metres or an angle in degrees cannot declare one as its default. The
    conversions all live in Controls.cpp, one function per control, and the
    shaders are handed the physical value.

    Option, boolean and event parameters are the exception: they hold the
    element value itself.

    ------------------------------------------------------------ lengths

    **Every length is in metres, and the frame height is Pond Size metres.**
    The waves are simulated on a grid sized in metres, not pixels, so the same
    settings ring the same pond at 720p and at 4K -- the raster only decides how
    finely the finished surface is looked at. That is also why every *physics*
    check in `mptest` is raster-independent by construction, and only the
    *optics* checks have to run at two rasters.
*/

namespace millpond
{
/**
    Parameter ids.

    **Append only.** `SetParamGroup` collapses runs of consecutive same-group
    ids, so inserting an id mid-enum silently splits a group in two; and every
    saved composition stores parameters by index, so a renumber rewrites what
    an operator's old project means.
*/
enum ParamId : unsigned int
{
	// -- Pond ---------------------------------------------------------------
	// The water itself: how big, how deep, and the three constants that
	// decide how fast each wavelength travels and how long it lasts.
	PT_POND_SIZE = 0,
	PT_DEPTH,
	PT_TENSION,
	PT_VISCOSITY,
	PT_SPEED,
	PT_BANKS,
	PT_DETAIL,
	PT_STILL,

	// -- Pebble -------------------------------------------------------------
	PT_DROP,
	PT_PEBBLE_X,
	PT_PEBBLE_Y,
	PT_PEBBLE_SIZE,
	PT_SPLASH,
	PT_SCATTER,

	// -- Skim ---------------------------------------------------------------
	PT_SKIM,
	PT_HEADING,
	PT_THROW_SPEED,
	PT_SKIM_ANGLE,
	PT_BOUNCES,

	// -- Rain ---------------------------------------------------------------
	PT_RAIN,
	PT_RAIN_SIZE,

	// -- Audio --------------------------------------------------------------
	PT_AUDIO,
	PT_AUDIO_PEBBLES,
	PT_AUDIO_RAIN,

	// -- Light --------------------------------------------------------------
	PT_CAUSTICS,
	PT_SUN_ELEVATION,
	PT_SUN_AZIMUTH,
	PT_SUN_SIZE,
	PT_REFLECTION,
	PT_SKY_R,
	PT_SKY_G,
	PT_SKY_B,
	PT_GLINT,

	// -- Output -------------------------------------------------------------
	PT_VIEW,
	PT_MIX,

	// -- The Stoatworks About block -----------------------------------------
	//
	// One display-only text line, then one button per link the block carries.
	// Millpond.cpp static_asserts this run against `about::kParamCount`, so a
	// user guide (which adds a link) cannot shift PT_COUNT silently.
	//
	// Last in the enum so no saved composition's parameter ids shift.
	PT_ABOUT_TEXT,
	PT_ABOUT_BUTTON_1,
	PT_ABOUT_BUTTON_2,
	PT_ABOUT_BUTTON_3,
	PT_COUNT
};

/// What happens at the edge of the frame.
enum class Banks
{
	Open = 0,///< a lake: what leaves the frame is absorbed and never returns
	Walls,   ///< a pool: the frame's edges reflect, exactly, to every order

	Count
};

/// What the output shows. Picture is the effect; the others show one of the
/// two models on its own, which is also a source of pure patterns.
enum class View
{
	Picture = 0,
	Height,
	Slope,
	Caustics,

	Count
};

/// Grid sizes along the long side of the simulated domain, which is the frame
/// mirrored to twice its width and height -- so 1024 is 512 cells across the
/// frame itself.
constexpr int kDetailCells[] = { 256, 512, 1024, 2048 };
constexpr int kDetailCount   = 4;

//---------------------------------------------------------------------------
// The mappings. Each says its range and its shape.
//---------------------------------------------------------------------------

/// The frame height in metres: 0.1 to 20, geometrically. Two metres is a
/// garden pond seen from a step-ladder.
float PondSizeFromParam( float value );

/// Water depth in metres: 2 mm to 2 m, geometrically. It is used twice, on
/// purpose: in the dispersion relation (tanh kh -- shallow water holds its
/// long waves back to sqrt(g h)) and in the optics (the bed is h below the
/// surface, so a deep pond bends the picture further). One control, because
/// it is one depth.
float DepthFromParam( float value );

/// Surface tension over density, in m^3/s^2: 0 to twice clean water's
/// 7.28e-5, linearly. Zero turns the capillary ripples off and leaves pure
/// gravity waves; a drop of soap is about half.
float TensionFromParam( float value );

/// Kinematic viscosity in m^2/s: 1e-7 to 1e-2, geometrically. Water is 1e-6;
/// the top of the range is golden syrup, where the ripples die before they
/// leave the stone.
float ViscosityFromParam( float value );

/// Time scale: 0.05x to 4x, geometrically, with exactly 0 at the bottom of the
/// travel -- which freezes the water rather than running it very slowly.
float SpeedFromParam( float value );

/// The inverse, so a test can ask for a speed by name.
float ParamFromSpeed( float speed );

/// Pebble radius in metres: 2 mm to 10 cm, geometrically. The crater's
/// spectrum peaks at k = 2/a, so this picks which part of the dispersion
/// curve the pebble rings.
float PebbleSizeFromParam( float value );

/// Crater depth as a fraction of its radius: 0 to 0.6, linearly. Past about
/// 0.3 the surface is steeper than linear theory really covers; it still
/// looks like water, and the harness never goes there.
float SplashFromParam( float value );

/// Random offset around the pebble point, as a fraction of the frame height:
/// 0 to 0.5, linearly.
float ScatterFromParam( float value );

/// The skim's heading, 0 to 360 degrees, linearly. 0 runs left to right.
float HeadingFromParam( float value );

/// Throw speed in m/s: 0.5 to 20, geometrically.
float ThrowSpeedFromParam( float value );

/// The stone's flight angle at each bounce, 3 to 40 degrees, linearly.
float SkimAngleFromParam( float value );

/// How many times the stone touches before it sinks: 1 to 40.
int BouncesFromParam( float value );

/// Raindrops per second over the whole frame: 80 * value^3, so the bottom of
/// the travel is a few drops a minute and zero is exactly zero.
float RainFromParam( float value );

/// Raindrop crater radius in metres: 1 mm to 2 cm, geometrically.
float RainSizeFromParam( float value );

/// Caustic strength: 0 to 2, linearly. 1 is physical -- the net carries all
/// the sun's light and none of the sky's.
float CausticsFromParam( float value );

/// Sun elevation, 10 to 90 degrees, linearly.
float SunElevationFromParam( float value );

/// Sun azimuth, 0 to 360 degrees, linearly.
float SunAzimuthFromParam( float value );

/// The sun's angular RADIUS in degrees: 0.1 to 10, geometrically. The real
/// sun is 0.27; a hazy sky is several. It blurs the caustics by
/// depth * tan( radius ) and widens the glints.
float SunSizeFromParam( float value );

/// Reflection: 0 to 3 times the Fresnel reflectance, linearly. 1 is physical.
float ReflectionFromParam( float value );

/// Glint: the sun's radiance relative to the sky's, 5000 * value^3, so 0 is
/// none and the default 0.3 is 135. It has to be that big: the reflection is
/// Fresnel-weighted, 2% looking down, and the real sun is 10^4 to 10^5 times
/// the sky -- a glint is a point where 2% of that is still far past white.
/// A range that topped out at 30 (the first one did) could never make one.
float GlintFromParam( float value );

/// The skim physics' gravity. Not a control: it is the same g the waves use.
constexpr float kGravity = 9.81f;

/// Clean water's surface tension over density, m^3/s^2.
constexpr float kWaterTension = 7.28e-5f;

/// A raindrop's crater depth as a fraction of its radius.
constexpr float kRainSplash = 0.3f;

/// Water's refractive index.
constexpr float kWaterIndex = 1.333f;

} // namespace millpond
