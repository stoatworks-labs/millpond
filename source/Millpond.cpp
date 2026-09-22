#include "Millpond.h"

#include "Diag.h"
#include "GLState.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name. The symptom without it is an
//unknown-type error on ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;

namespace millpond
{
namespace
{
constexpr float kPi = 3.14159265358979324f;

/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be
/// the thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

const char* const kBanksNames[]  = { "Open", "Walls" };
const char* const kDetailNames[] = { "256", "512", "1024", "2048" };
const char* const kViewNames[]   = { "Picture", "Height", "Slope", "Caustics" };

/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// Seconds of host time a single frame may advance by. The host's clock jumps
/// when the composition is scrubbed or the machine sleeps; the propagator is
/// exact for any step, but a pond that runs ten minutes in one frame has
/// simply lost everything that was in it.
constexpr double kMaxFrameDelta = 0.25;

/// How far past the frame the caustic mesh reaches, as a fraction of it. Light
/// that enters the water just outside the frame can land just inside it; this
/// is enough for a slope of 1 at 1/5 of the frame's depth.
constexpr float kMeshMargin = 0.1f;

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

int optionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

void setInts( const FFGLShader& shader, const char* name, int a, int b )
{
	glUniform2i( glGetUniformLocation( shader.GetGLID(), name ), a, b );
}

int log2Exact( int value )
{
	int bits = 0;
	while( ( 1 << bits ) < value )
		++bits;
	return bits;
}
} // namespace

//---------------------------------------------------------------------------
// The buttons are declared one per link, so the run in the enum and the run the
// block actually has must agree. They diverge the day somebody writes a user
// guide, and this is what says so.
static_assert( PT_COUNT - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
               "the About run no longer matches StoatworksAbout.h -- "
               "add or remove a PT_ABOUT_BUTTON_n to match" );

//---------------------------------------------------------------------------
MillpondPlugin::MillpondPlugin()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The water runs on the host's clock: its rings travel at so many metres a
	//second, so the plugin has to know what a second is.
	SetTimeSupported( true );

	//-------------------------------------------------------------------
	// Defaults. A garden pond from a step-ladder, with a light rain on it, so
	// dropping the effect on a layer shows water straight away rather than
	// a picture that looks untouched until somebody finds the Drop button.
	//-------------------------------------------------------------------
	params[ PT_POND_SIZE ] = 0.511f;//1.5 m of frame height
	params[ PT_DEPTH ]     = 0.725f;//0.3 m
	params[ PT_TENSION ]   = 0.5f;  //clean water
	params[ PT_VISCOSITY ] = 0.2f;  //water, 1e-6 m^2/s
	params[ PT_SPEED ]     = ParamFromSpeed( 1.0f );
	params[ PT_BANKS ]     = static_cast< float >( Banks::Open );
	params[ PT_DETAIL ]    = 2.0f;  //1024 along the domain's long side
	params[ PT_STILL ]     = 0.0f;

	params[ PT_DROP ]        = 0.0f;
	params[ PT_PEBBLE_X ]    = 0.5f;
	params[ PT_PEBBLE_Y ]    = 0.5f;
	params[ PT_PEBBLE_SIZE ] = 0.73f;//3.5 cm: gravity rings leading, capillary ripples inside
	params[ PT_SPLASH ]      = 0.8f; //crater half its radius deep: a thrown stone, not a dropped one
	params[ PT_SCATTER ]     = 0.0f;

	params[ PT_SKIM ]        = 0.0f;
	params[ PT_HEADING ]     = 0.0f;  //left to right
	params[ PT_THROW_SPEED ] = 0.39f; //2.1 m/s: the whole skim fits a 1.5 m frame
	params[ PT_SKIM_ANGLE ]  = 0.27f; //13 degrees
	params[ PT_BOUNCES ]     = 0.23f; //10 touches, then the sink

	params[ PT_RAIN ]      = 0.27f;//about 1.6 drops a second
	params[ PT_RAIN_SIZE ] = 0.46f;//4 mm

	params[ PT_AUDIO_PEBBLES ] = 0.0f;
	params[ PT_AUDIO_RAIN ]    = 0.0f;

	params[ PT_CAUSTICS ]      = 0.5f;  //physical
	params[ PT_SUN_ELEVATION ] = 0.9375f;//85 degrees: see AGENTS.md, a straight-down camera only sees glints under a high sun
	params[ PT_SUN_AZIMUTH ]   = 0.35f; //126 degrees
	params[ PT_SUN_SIZE ]      = 0.39f; //0.6 degrees: a bright, slightly hazy sun
	params[ PT_REFLECTION ]    = 1.0f / 3.0f;//physical
	params[ PT_SKY_R ]         = 0.55f;
	params[ PT_SKY_G ]         = 0.65f;
	params[ PT_SKY_B ]         = 0.75f;
	params[ PT_GLINT ]         = 0.3f;  //135x the sky: specks past white

	params[ PT_VIEW ] = static_cast< float >( View::Picture );
	params[ PT_MIX ]  = 1.0f;

	//-------------------------------------------------------------------
	// Declaration. Every numeric parameter is a plain 0..1 float: the ranges
	// live in Controls.cpp and nowhere else.
	//-------------------------------------------------------------------
	SetParamInfof( PT_POND_SIZE, "Pond Size", FF_TYPE_STANDARD );
	SetParamInfof( PT_DEPTH, "Depth", FF_TYPE_STANDARD );
	SetParamInfof( PT_TENSION, "Surface Tension", FF_TYPE_STANDARD );
	SetParamInfof( PT_VISCOSITY, "Viscosity", FF_TYPE_STANDARD );
	SetParamInfof( PT_SPEED, "Speed", FF_TYPE_STANDARD );
	SetOptionParamInfo( PT_BANKS, "Banks", static_cast< int >( Banks::Count ), params[ PT_BANKS ] );
	for( int i = 0; i < static_cast< int >( Banks::Count ); ++i )
		SetParamElementInfo( PT_BANKS, i, kBanksNames[ i ], static_cast< float >( i ) );
	SetOptionParamInfo( PT_DETAIL, "Detail", kDetailCount, params[ PT_DETAIL ] );
	for( int i = 0; i < kDetailCount; ++i )
		SetParamElementInfo( PT_DETAIL, i, kDetailNames[ i ], static_cast< float >( i ) );
	SetParamInfo( PT_STILL, "Still", FF_TYPE_EVENT, false );

	SetParamInfo( PT_DROP, "Drop", FF_TYPE_EVENT, false );
	SetParamInfof( PT_PEBBLE_X, "Pebble X", FF_TYPE_XPOS );
	SetParamInfof( PT_PEBBLE_Y, "Pebble Y", FF_TYPE_YPOS );
	SetParamInfof( PT_PEBBLE_SIZE, "Pebble Size", FF_TYPE_STANDARD );
	SetParamInfof( PT_SPLASH, "Splash", FF_TYPE_STANDARD );
	SetParamInfof( PT_SCATTER, "Scatter", FF_TYPE_STANDARD );

	SetParamInfo( PT_SKIM, "Skim", FF_TYPE_EVENT, false );
	SetParamInfof( PT_HEADING, "Heading", FF_TYPE_STANDARD );
	SetParamInfof( PT_THROW_SPEED, "Throw Speed", FF_TYPE_STANDARD );
	SetParamInfof( PT_SKIM_ANGLE, "Skim Angle", FF_TYPE_STANDARD );
	SetParamInfof( PT_BOUNCES, "Bounces", FF_TYPE_STANDARD );

	SetParamInfof( PT_RAIN, "Rain", FF_TYPE_STANDARD );
	SetParamInfof( PT_RAIN_SIZE, "Rain Size", FF_TYPE_STANDARD );

	//An FFT buffer: Resolume shows it as an audio-source picker and writes the
	//spectrum into it every frame. With no audio routed the two amounts below
	//do nothing, rather than the pond reacting to silence.
	SetBufferParamInfo( PT_AUDIO, "Audio", audio::kBins, FF_USAGE_FFT );
	for( int i = 0; i < audio::kBins; ++i )
		SetParamElementInfo( PT_AUDIO, i, "", 0.0f );
	SetParamInfof( PT_AUDIO_PEBBLES, "Audio Pebbles", FF_TYPE_STANDARD );
	SetParamInfof( PT_AUDIO_RAIN, "Audio Rain", FF_TYPE_STANDARD );

	SetParamInfof( PT_CAUSTICS, "Caustics", FF_TYPE_STANDARD );
	SetParamInfof( PT_SUN_ELEVATION, "Sun Elevation", FF_TYPE_STANDARD );
	SetParamInfof( PT_SUN_AZIMUTH, "Sun Azimuth", FF_TYPE_STANDARD );
	SetParamInfof( PT_SUN_SIZE, "Sun Size", FF_TYPE_STANDARD );
	SetParamInfof( PT_REFLECTION, "Reflection", FF_TYPE_STANDARD );
	//Consecutive red/green/blue parameters are what a host needs to show a
	//swatch rather than three sliders.
	SetParamInfof( PT_SKY_R, "Sky", FF_TYPE_RED );
	SetParamInfof( PT_SKY_G, "Sky_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_SKY_B, "Sky_Blue", FF_TYPE_BLUE );
	SetParamInfof( PT_GLINT, "Glint", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_VIEW, "View", static_cast< int >( View::Count ), params[ PT_VIEW ] );
	for( int i = 0; i < static_cast< int >( View::Count ); ++i )
		SetParamElementInfo( PT_VIEW, i, kViewNames[ i ], static_cast< float >( i ) );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	//-------------------------------------------------------------------
	// Groups. SetParamGroup collapses runs of consecutive same-group ids, so
	// this depends entirely on the id order in Controls.h.
	//-------------------------------------------------------------------
	for( unsigned int id = PT_POND_SIZE; id <= PT_STILL; ++id )
		SetParamGroup( id, "Pond" );
	for( unsigned int id = PT_DROP; id <= PT_SCATTER; ++id )
		SetParamGroup( id, "Pebble" );
	for( unsigned int id = PT_SKIM; id <= PT_BOUNCES; ++id )
		SetParamGroup( id, "Skim" );
	for( unsigned int id = PT_RAIN; id <= PT_RAIN_SIZE; ++id )
		SetParamGroup( id, "Rain" );
	for( unsigned int id = PT_AUDIO; id <= PT_AUDIO_RAIN; ++id )
		SetParamGroup( id, "Audio" );
	for( unsigned int id = PT_CAUSTICS; id <= PT_GLINT; ++id )
		SetParamGroup( id, "Light" );
	for( unsigned int id = PT_VIEW; id <= PT_MIX; ++id )
		SetParamGroup( id, "Output" );

	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin and nothing outside the class
	// can call it.
	SetParamInfo( PT_ABOUT_TEXT, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_TEXT + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( unsigned int id = PT_ABOUT_TEXT; id < PT_COUNT; ++id )
		SetParamGroup( id, "About" );
}

//---------------------------------------------------------------------------
FFResult MillpondPlugin::InitGL( const FFGLViewportStruct* vp )
{
	diag::init();

	//The GL strings first, and unconditionally: when a shader will not compile
	//it is almost always the driver or the GL version, and knowing which
	//machine reported what is most of the diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct Stage
	{
		FFGLShader* shader;
		const char* vertex;
		const char* fragment;
		const char* name;
	};
	const Stage stages[] = {
		{ &injectShader, kVertexShader, kInjectShader, "inject" },
		{ &fftShader, kVertexShader, kFFTShader, "fft" },
		{ &evolveShader, kVertexShader, kEvolveShader, "evolve" },
		{ &causticShader, kCausticVertexShader, kCausticFragmentShader, "caustic" },
		{ &blurShader, kVertexShader, kBlurShader, "blur" },
		{ &compositeShader, kVertexShader, kCompositeShader, "composite" },
	};

	for( const Stage& stage : stages )
	{
		const bool compiled = stage.shader->Compile( stage.vertex, stage.fragment );
		if( compiled )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the plugin
		//simply does nothing in Resolume, with no message anywhere. These two
		//lines are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name
		             + " shader failed to compile - the plugin will do nothing" );
		FFGLLog::LogToHost( "Millpond: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised" );

	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
bool MillpondPlugin::ensureBuffers( const Grid& wanted, GLsizei width, GLsizei height )
{
	const int banks = optionIndex( params[ PT_BANKS ], static_cast< int >( Banks::Count ) );

	//A different grid is a different pond. So is a change of banks: an open
	//pond's state is not mirror-symmetric, and carrying it into a walled one
	//would leave the margin's waves bouncing about in the reflections.
	const bool reset = wanted.nx != grid.nx || wanted.ny != grid.ny || banks != bufferBanks;
	if( reset )
	{
		for( PassBuffer& buffer : spectrum )
			buffer.Destroy();
		for( PassBuffer& buffer : surface )
			buffer.Destroy();
		surfaceIndex = 0;
		pending.clear();

		if( twiddleX != 0 )
			glDeleteTextures( 1, &twiddleX );
		if( twiddleY != 0 )
			glDeleteTextures( 1, &twiddleY );
		twiddleX = MakeTwiddles( wanted.nx );
		twiddleY = MakeTwiddles( wanted.ny );
		if( twiddleX == 0 || twiddleY == 0 )
			return false;
	}

	//The spectrum is 32-bit because it is a sum over a million cells: a half
	//float's eleven bits would bury every ripple under the rounding of the
	//biggest one. The surface is 32-bit because it is also the state, fed
	//back every frame for as long as the pond runs.
	bool ok = true;
	for( PassBuffer& buffer : spectrum )
		ok = ok && buffer.Ensure( wanted.nx, wanted.ny, GL_RG32F, PassBuffer::Sampling::Nearest );
	for( PassBuffer& buffer : surface )
		ok = ok
		     && buffer.Ensure( wanted.nx, wanted.ny, GL_RGBA32F, PassBuffer::Sampling::Linear,
		                       PassBuffer::Wrap::Repeat );

	//The caustics are a deposit of light in bed coordinates, at the picture's
	//own resolution. 32-bit, although the quantity is about 1: a GPU divides
	//as a reciprocal times a multiply, so still water's area over itself
	//comes out 0.99999994, and this GPU stores to half floats by truncating
	//-- which made still water a half-float step dark on 6% of its pixels,
	//and would bias every deposit down by half a step besides.
	for( PassBuffer& buffer : caustic )
		ok = ok && buffer.Ensure( width, height, GL_R32F, PassBuffer::Sampling::Linear );

	if( !ok )
		return false;

	grid          = wanted;
	bufferBanks   = banks;
	causticWidth  = width;
	causticHeight = height;
	return true;
}

//---------------------------------------------------------------------------
GLuint MillpondPlugin::MakeTwiddles( int length )
{
	std::vector< float > table( static_cast< size_t >( length ) * 2 );
	for( int m = 0; m < length; ++m )
	{
		const double angle   = -2.0 * 3.14159265358979323846 * static_cast< double >( m ) / length;
		table[ m * 2 + 0 ] = static_cast< float >( std::cos( angle ) );
		table[ m * 2 + 1 ] = static_cast< float >( std::sin( angle ) );
	}

	GLuint texture = 0;
	glGenTextures( 1, &texture );
	//Bound and unbound by hand rather than through a Scoped binding: this runs
	//from ensureBuffers, before anything is bound, and must leave nothing.
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RG32F, length, 1, 0, GL_RG, GL_FLOAT, table.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

//---------------------------------------------------------------------------
bool MillpondPlugin::ensureMesh( int columns, int rows )
{
	//The mesh has no vertex data: gl_VertexID is all the caustic pass reads.
	//Core profile still insists on a vertex array object being bound to draw.
	if( meshVAO == 0 )
		glGenVertexArrays( 1, &meshVAO );
	if( meshVAO == 0 )
		return false;

	meshColumns = columns;
	meshRows    = rows;
	meshIndices = static_cast< GLsizei >( columns ) * rows * 6;
	return true;
}

//---------------------------------------------------------------------------
void MillpondPlugin::UpdateClock()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	const double raw = hostTime;

	//Resolume has been seen sending both seconds and milliseconds through
	//SetTime, and nothing in the call says which. Vote on it against the wall
	//clock for a few frames, then stop asking. (Rosette's code, unchanged.)
	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		// A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}

	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;

	// Until the unit is settled -- and for a host that never calls SetTime --
	// run on the real clock: wrong in origin but right in rate.
	now = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale : wallNow - wallStart;

	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( raw ) + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now ) );
}

//---------------------------------------------------------------------------
void MillpondPlugin::UpdateAudio( double dt )
{
	float bins[ audio::kBins ] = {};
	int binCount              = 0;
	if( const ParamInfo* info = FindParamInfo( PT_AUDIO ) )
	{
		binCount = static_cast< int >( std::min< size_t >( info->elements.size(), audio::kBins ) );
		for( int i = 0; i < binCount; ++i )
			bins[ i ] = info->elements[ static_cast< size_t >( i ) ].value;
	}

	audio::Settings settings;
	//Audio Pebbles is the detector's sensitivity as well as its switch: the
	//further up, the lower the bar an onset has to clear.
	settings.sensitivity = std::clamp( params[ PT_AUDIO_PEBBLES ], 0.0f, 1.0f );
	analyser.Update( bins, binCount, static_cast< float >( dt ), settings );
}

//---------------------------------------------------------------------------
void MillpondPlugin::RandomPoint( float centreX, float centreY, float scatter, float& x, float& y )
{
	x = centreX;
	y = centreY;
	if( scatter <= 0.0f )
		return;

	//Uniform over the disc, not over the radius: sqrt of a uniform radius, or
	//the pebbles bunch up in the middle.
	const double r     = scatter * std::sqrt( random.Uniform() );
	const double angle = 2.0 * kPi * random.Uniform();
	x += static_cast< float >( r * std::cos( angle ) );
	y += static_cast< float >( r * std::sin( angle ) );
}

//---------------------------------------------------------------------------
std::vector< Impact > MillpondPlugin::CollectImpacts( double dt )
{
	std::vector< Impact > due;

	const float pebble  = PebbleSizeFromParam( params[ PT_PEBBLE_SIZE ] );
	const float splash  = SplashFromParam( params[ PT_SPLASH ] );
	const float scatter = ScatterFromParam( params[ PT_SCATTER ] ) * frameHeight;
	const float aimX    = std::clamp( params[ PT_PEBBLE_X ], 0.0f, 1.0f ) * frameWidth;
	const float aimY    = std::clamp( params[ PT_PEBBLE_Y ], 0.0f, 1.0f ) * frameHeight;

	auto dropAt = [ & ]( float strength ) {
		Impact drop;
		drop.time      = simTime;
		drop.radius    = pebble;
		drop.amplitude = splash * pebble * strength;
		RandomPoint( aimX, aimY, scatter, drop.x, drop.y );
		due.push_back( drop );
	};

	for( ; dropPresses > 0; --dropPresses )
		dropAt( 1.0f );

	//An onset drops a pebble whose splash follows how hard the hit was. The
	//floor keeps a soft hit visible: a ring too shallow to refract anything
	//reads as the detector having missed it.
	if( params[ PT_AUDIO_PEBBLES ] > 0.0f && analyser.Fired() )
		dropAt( 0.35f + 0.65f * analyser.Kick() );

	for( ; skimPresses > 0; --skimPresses )
	{
		const float heading = HeadingFromParam( params[ PT_HEADING ] );
		float startX = 0.0f, startY = 0.0f;
		SkimStart( aimX, aimY, heading, frameWidth, frameHeight, 2.0f * pebble, startX, startY );

		const std::vector< Impact > skim =
			SkimSchedule( simTime, startX, startY, heading, ThrowSpeedFromParam( params[ PT_THROW_SPEED ] ),
		                  SkimAngleFromParam( params[ PT_SKIM_ANGLE ] ), BouncesFromParam( params[ PT_BOUNCES ] ),
		                  pebble, splash, kGravity );

		//Merge into the queue in time order. Two skims in flight at once is
		//an ordinary thing to do.
		for( const Impact& touch : skim )
		{
			auto at = std::upper_bound( pending.begin(), pending.end(), touch,
			                            []( const Impact& a, const Impact& b ) { return a.time < b.time; } );
			pending.insert( at, touch );
		}
	}

	while( !pending.empty() && pending.front().time <= simTime )
	{
		due.push_back( pending.front() );
		pending.pop_front();
	}

	//Rain: a Poisson process in water-time, so Speed speeds the rain with the
	//waves and a frozen pond has no rain falling on it.
	const float rainRate =
		RainFromParam( params[ PT_RAIN ] ) + RainFromParam( params[ PT_AUDIO_RAIN ] ) * analyser.Level();
	const int drops = random.Poisson( static_cast< double >( rainRate ) * dt );
	const float rainSize = RainSizeFromParam( params[ PT_RAIN_SIZE ] );
	for( int i = 0; i < drops; ++i )
	{
		Impact drop;
		drop.time      = simTime;
		drop.x         = static_cast< float >( random.Uniform() * frameWidth );
		drop.y         = static_cast< float >( random.Uniform() * frameHeight );
		drop.radius    = rainSize * static_cast< float >( 0.7 + 0.6 * random.Uniform() );
		drop.amplitude = splash * drop.radius;
		due.push_back( drop );
	}

	if( static_cast< int >( due.size() ) > kMaxDropsPerFrame )
		due.resize( kMaxDropsPerFrame );

	return due;
}

//---------------------------------------------------------------------------
void MillpondPlugin::Transform( PassBuffer ( &buffers )[ 2 ], int& current, float direction )
{
	ScopedShaderBinding shader( fftShader.GetGLID() );
	fftShader.Set( "Source", 0 );
	fftShader.Set( "Twiddles", 1 );
	fftShader.Set( "Direction", direction );

	struct Axis
	{
		int horizontal, length;
		GLuint twiddles;
	};
	const Axis axes[ 2 ] = { { 1, grid.nx, twiddleX }, { 0, grid.ny, twiddleY } };
	for( const Axis& axis : axes )
	{
		fftShader.Set( "Horizontal", axis.horizontal );
		fftShader.Set( "Length", axis.length );
		const int stages = log2Exact( axis.length );
		for( int s = 1; s <= stages; ++s )
		{
			PassBuffer& target = buffers[ 1 - current ];
			ScopedFBOBinding fbo( target.GetGLID(), ScopedFBOBinding::RB_REVERT );
			glViewport( 0, 0, grid.nx, grid.ny );

			//Interleaved, activate then bind, per unit. Every Scoped* binding
			//clears to 0 on exit rather than restoring, on whichever unit is
			//active THEN -- so a binding held across the loop is released on
			//the wrong unit and leaks the other into the host's state.
			ScopedSamplerActivation sampler0( 0 );
			Scoped2DTextureBinding texture( buffers[ current ].TextureID() );
			ScopedSamplerActivation sampler1( 1 );
			Scoped2DTextureBinding table( axis.twiddles );

			fftShader.Set( "Span", 1 << s );
			quad.Draw();
			current = 1 - current;
		}
	}
}

//---------------------------------------------------------------------------
void MillpondPlugin::Simulate( const std::vector< Impact >& impacts, double dt )
{
	const Water water = CurrentWater();
	const int walls   = bufferBanks == static_cast< int >( Banks::Walls ) ? 1 : 0;

	//The smallest crater the grid can carry. A Mexican hat narrower than a
	//cell and a half is aliased into a cross; widening it keeps the ring round
	//at the cost of the smallest raindrops at the lowest Detail.
	const float minRadius =
		1.5f * static_cast< float >( std::max( grid.lx / grid.nx, grid.ly / grid.ny ) );

	std::vector< float > drops( static_cast< size_t >( kMaxDropsPerFrame ) * 4, 0.0f );
	const int count = std::min( static_cast< int >( impacts.size() ), kMaxDropsPerFrame );
	for( int i = 0; i < count; ++i )
	{
		drops[ i * 4 + 0 ] = impacts[ i ].x;
		drops[ i * 4 + 1 ] = impacts[ i ].y;
		drops[ i * 4 + 2 ] = std::max( impacts[ i ].radius, minRadius );
		drops[ i * 4 + 3 ] = impacts[ i ].amplitude;
	}

	//The absorber's strength. It has to take out a wave crossing a margin
	//half a frame wide, and the slowest energy is the ripples at the minimum
	//group velocity, about 0.18 m/s: a peak rate of 9 over the margin in
	//metres integrates to several e-foldings across the quadratic ramp for
	//anything from 0.1 m to 20 m of pond. `mptest --banks` measures it.
	const float margin      = 0.5f * std::min( frameWidth, frameHeight );
	const float spongeRate  = 9.0f / std::max( margin, 0.01f );

	//-------------------------------------------------------------------
	// 1. Inject: state times sponge, plus craters, into spectrum[ 0 ].
	//-------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( spectrum[ 0 ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		glViewport( 0, 0, grid.nx, grid.ny );
		ScopedShaderBinding shader( injectShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( surface[ surfaceIndex ].TextureID() );

		injectShader.Set( "StateTexture", 0 );
		setInts( injectShader, "GridSize", grid.nx, grid.ny );
		injectShader.Set( "Domain", static_cast< float >( grid.lx ), static_cast< float >( grid.ly ) );
		injectShader.Set( "Frame", frameWidth, frameHeight );
		injectShader.Set( "Walls", walls );
		injectShader.Set( "SpongeRate", spongeRate );
		injectShader.Set( "Dt", static_cast< float >( dt ) );
		injectShader.Set( "DropCount", count );
		glUniform4fv( glGetUniformLocation( injectShader.GetGLID(), "Drops" ), kMaxDropsPerFrame, drops.data() );
		quad.Draw();
	}

	//-------------------------------------------------------------------
	// 2. Forward.
	//-------------------------------------------------------------------
	int current = 0;
	Transform( spectrum, current, -1.0f );

	//-------------------------------------------------------------------
	// 3. Evolve, into surface[ 0 ]. Last frame's surface has been read by
	// the inject pass and nothing needs it again.
	//-------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( surface[ 0 ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		glViewport( 0, 0, grid.nx, grid.ny );
		ScopedShaderBinding shader( evolveShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( spectrum[ current ].TextureID() );

		evolveShader.Set( "Spectrum", 0 );
		setInts( evolveShader, "GridSize", grid.nx, grid.ny );
		evolveShader.Set( "Domain", static_cast< float >( grid.lx ), static_cast< float >( grid.ly ) );
		evolveShader.Set( "Gravity", static_cast< float >( water.gravity ) );
		evolveShader.Set( "Tension", static_cast< float >( water.tension ) );
		evolveShader.Set( "Depth", static_cast< float >( water.depth ) );
		evolveShader.Set( "Viscosity", static_cast< float >( water.viscosity ) );
		evolveShader.Set( "Dt", static_cast< float >( dt ) );
		evolveShader.Set( "Norm", 1.0f / ( static_cast< float >( grid.nx ) * static_cast< float >( grid.ny ) ) );
		evolveShader.Set( "RateScale", kRateScale );
		quad.Draw();
	}

	//-------------------------------------------------------------------
	// 4. Inverse. The result is the surface and next frame's state.
	//-------------------------------------------------------------------
	int landed = 0;
	Transform( surface, landed, 1.0f );
	surfaceIndex = landed;

	if( logInjected )
		injectedLog.insert( injectedLog.end(), impacts.begin(), impacts.begin() + count );
}

//---------------------------------------------------------------------------
void MillpondPlugin::Caustics( float depth, const float sunTravel[ 3 ], float sunRadius )
{
	//The mesh samples the surface about once per grid cell -- finer buys
	//nothing, because between grid points the surface is only interpolated --
	//but never finer than a pixel and a half of the buffer it is drawn into,
	//or its triangles fall between pixel centres and the light sparkles.
	const float span     = 1.0f + 2.0f * kMeshMargin;
	const int columns    = std::max( 8, static_cast< int >( std::min( span * 0.5f * grid.nx, span * causticWidth / 1.5f ) ) );
	const int rows       = std::max( 8, static_cast< int >( std::min( span * 0.5f * grid.ny, span * causticHeight / 1.5f ) ) );
	if( !ensureMesh( columns, rows ) )
	{
		haveCaustics = false;
		return;
	}

	//Where flat water puts the light: the refracted sun's sideways travel over
	//the depth. Subtracted so that flat water maps the bed onto itself -- a
	//uniform shift of uniform light is no light at all.
	const float horizontal = std::sqrt( sunTravel[ 0 ] * sunTravel[ 0 ] + sunTravel[ 1 ] * sunTravel[ 1 ] );
	const float sinT       = horizontal / kWaterIndex;
	const float cosT       = std::sqrt( std::max( 1.0f - sinT * sinT, 1e-6f ) );
	float shiftX = 0.0f, shiftY = 0.0f;
	if( horizontal > 1e-6f )
	{
		shiftX = sunTravel[ 0 ] / horizontal * depth * sinT / cosT;
		shiftY = sunTravel[ 1 ] / horizontal * depth * sinT / cosT;
	}

	{
		ScopedFBOBinding fbo( caustic[ 0 ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		glViewport( 0, 0, causticWidth, causticHeight );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );

		setAdditiveBlend();

		ScopedShaderBinding shader( causticShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( surface[ surfaceIndex ].TextureID() );

		causticShader.Set( "SurfaceTexture", 0 );
		causticShader.Set( "FrameToTexture", static_cast< float >( frameWidth / grid.lx ),
		                   static_cast< float >( frameHeight / grid.ly ) );
		causticShader.Set( "Frame", frameWidth, frameHeight );
		causticShader.Set( "Depth", depth );
		causticShader.Set( "Eta", 1.0f / kWaterIndex );
		causticShader.Set( "SunTravel", sunTravel[ 0 ], sunTravel[ 1 ], sunTravel[ 2 ] );
		causticShader.Set( "FlatShift", shiftX, shiftY );
		causticShader.Set( "BufferSize", static_cast< float >( causticWidth ), static_cast< float >( causticHeight ) );
		setInts( causticShader, "Cells", meshColumns, meshRows );
		causticShader.Set( "Margin", kMeshMargin );

		glBindVertexArray( meshVAO );
		glDrawArrays( GL_TRIANGLES, 0, meshIndices );
		glBindVertexArray( 0 );

		glDisable( GL_BLEND );
	}
	causticIndex = 0;
	haveCaustics = true;

	//-------------------------------------------------------------------
	// The sun's size. A point on the bed sees the sun as a disc of this
	// angular radius, so every caustic is the sharp one smeared by
	// depth * tan( radius ). A Gaussian of that sigma stands in for the
	// disc; it moves light and never makes any.
	//-------------------------------------------------------------------
	const float pixelsPerMetre = static_cast< float >( causticHeight ) / frameHeight;
	const float sigma          = depth * std::tan( sunRadius ) * pixelsPerMetre;
	if( sigma < 0.35f )
		return;

	const int taps   = std::clamp( static_cast< int >( std::ceil( 3.0f * sigma ) ), 1, kMaxBlurTaps );
	const float step = 3.0f * sigma / static_cast< float >( taps );

	struct Axis
	{
		int from, to;
		float dx, dy;
	};
	const Axis axes[] = {
		{ 0, 1, 1.0f / static_cast< float >( causticWidth ), 0.0f },
		{ 1, 0, 0.0f, 1.0f / static_cast< float >( causticHeight ) },
	};
	for( const Axis& axis : axes )
	{
		ScopedFBOBinding fbo( caustic[ axis.to ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		glViewport( 0, 0, causticWidth, causticHeight );
		ScopedShaderBinding shader( blurShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( caustic[ axis.from ].TextureID() );

		blurShader.Set( "Source", 0 );
		blurShader.Set( "Direction", axis.dx, axis.dy );
		blurShader.Set( "Sigma", sigma );
		blurShader.Set( "Step", std::max( step, 1.0f ) );
		blurShader.Set( "Taps", taps );
		quad.Draw();
	}
}

//---------------------------------------------------------------------------
FFResult MillpondPlugin::ProcessOpenGL( ProcessOpenGLStruct* pgl )
{
	if( pgl == nullptr || pgl->numInputTextures < 1 || pgl->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& source = *( pgl->inputTextures[ 0 ] );
	const GLsizei width             = static_cast< GLsizei >( source.Width );
	const GLsizei height            = static_cast< GLsizei >( source.Height );
	if( width <= 0 || height <= 0 )
		return FF_FAIL;

	//The host's viewport and blend state, put back however this returns.
	//ScopedFBOBinding restores the framebuffer and nothing else (SDK b1afaf9).
	ScopedGLState restore;
	const GLint* hostViewport = restore.saved.viewport;
	glDisable( GL_BLEND );

	//-------------------------------------------------------------------
	// Time.
	//-------------------------------------------------------------------
	UpdateClock();
	const double hostDt = lastNow >= 0.0 ? std::clamp( now - lastNow, 0.0, kMaxFrameDelta ) : 0.0;
	lastNow             = now;
	UpdateAudio( hostDt );

	const double dt = hostDt * SpeedFromParam( params[ PT_SPEED ] );
	simTime += dt;

	//-------------------------------------------------------------------
	// The pond, in metres.
	//-------------------------------------------------------------------
	frameHeight = PondSizeFromParam( params[ PT_POND_SIZE ] );
	frameWidth  = frameHeight * static_cast< float >( width ) / static_cast< float >( height );

	const int detail  = optionIndex( params[ PT_DETAIL ], kDetailCount );
	const Grid wanted = ChooseGrid( frameWidth, frameHeight, kDetailCells[ detail ] );

	//Every allocation up front. See ensureBuffers.
	if( !ensureBuffers( wanted, width, height ) )
	{
		diag::error( "could not allocate the pass buffers" );
		return FF_FAIL;
	}
	//The frame's metres may have moved without the cell counts doing so.
	grid.lx = wanted.lx;
	grid.ly = wanted.ly;

	if( stillWanted )
	{
		for( PassBuffer& buffer : surface )
			buffer.Clear();
		pending.clear();
		stillWanted = false;
	}

	const std::vector< Impact > impacts = CollectImpacts( dt );
	if( dt > 0.0 || !impacts.empty() )
		Simulate( impacts, dt );

	//-------------------------------------------------------------------
	// The light.
	//-------------------------------------------------------------------
	const float depth         = DepthFromParam( params[ PT_DEPTH ] );
	const float elevation     = SunElevationFromParam( params[ PT_SUN_ELEVATION ] ) * kPi / 180.0f;
	const float azimuth       = SunAzimuthFromParam( params[ PT_SUN_AZIMUTH ] ) * kPi / 180.0f;
	const float sunRadius     = SunSizeFromParam( params[ PT_SUN_SIZE ] ) * kPi / 180.0f;
	const float sunTravel[ 3 ] = { -std::cos( elevation ) * std::cos( azimuth ),
		                           -std::cos( elevation ) * std::sin( azimuth ), -std::sin( elevation ) };

	const int view            = optionIndex( params[ PT_VIEW ], static_cast< int >( View::Count ) );
	const float causticAmount = CausticsFromParam( params[ PT_CAUSTICS ] );
	const bool wantCaustics   = causticAmount > 0.0f || view == static_cast< int >( View::Caustics );

	haveCaustics = false;
	if( wantCaustics )
		Caustics( depth, sunTravel, sunRadius );

	//-------------------------------------------------------------------
	// Composite, back into the host's framebuffer and viewport.
	//-------------------------------------------------------------------
	glBindFramebuffer( GL_FRAMEBUFFER, pgl->HostFBO );
	glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
	{
		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( source );

		ScopedShaderBinding shader( compositeShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding inputTexture( source.Handle );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding surfaceTexture( surface[ surfaceIndex ].TextureID() );
		ScopedSamplerActivation sampler2( 2 );
		//Always a real texture, even when UseCaustics says not to read it: a
		//sampler bound to texture 0 is legal, and some drivers log about it on
		//every frame.
		Scoped2DTextureBinding causticTexture( caustic[ causticIndex ].TextureID() );

		compositeShader.Set( "InputTexture", 0 );
		compositeShader.Set( "SurfaceTexture", 1 );
		compositeShader.Set( "CausticTexture", 2 );
		compositeShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		compositeShader.Set( "HalfTexel", 0.5f / static_cast< float >( width ), 0.5f / static_cast< float >( height ) );
		compositeShader.Set( "FrameToTexture", static_cast< float >( frameWidth / grid.lx ),
		                     static_cast< float >( frameHeight / grid.ly ) );
		compositeShader.Set( "Frame", frameWidth, frameHeight );
		compositeShader.Set( "Depth", depth );
		compositeShader.Set( "Eta", 1.0f / kWaterIndex );
		compositeShader.Set( "UseCaustics", haveCaustics ? 1 : 0 );
		compositeShader.Set( "CausticAmount", causticAmount );
		compositeShader.Set( "Reflection", ReflectionFromParam( params[ PT_REFLECTION ] ) );
		compositeShader.Set( "Sky", params[ PT_SKY_R ], params[ PT_SKY_G ], params[ PT_SKY_B ] );
		compositeShader.Set( "SunTravel", sunTravel[ 0 ], sunTravel[ 1 ], sunTravel[ 2 ] );
		compositeShader.Set( "Glint", GlintFromParam( params[ PT_GLINT ] ) );
		compositeShader.Set( "SunRadius", sunRadius );
		compositeShader.Set( "View", view );
		//Height view: full scale at a twentieth of the pebble's crater depth,
		//which is about the height of its ring a second or two out. The crater
		//itself clips, and should: it is the ring that is worth looking at.
		const float crater = SplashFromParam( params[ PT_SPLASH ] ) * PebbleSizeFromParam( params[ PT_PEBBLE_SIZE ] );
		compositeShader.Set( "HeightGain", 0.5f / std::max( 0.05f * crater, 1e-6f ) );
		compositeShader.Set( "MixAmount", params[ PT_MIX ] );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult MillpondPlugin::DeInitGL()
{
	injectShader.FreeGLResources();
	fftShader.FreeGLResources();
	evolveShader.FreeGLResources();
	causticShader.FreeGLResources();
	blurShader.FreeGLResources();
	compositeShader.FreeGLResources();
	quad.Release();

	for( PassBuffer& buffer : spectrum )
		buffer.Destroy();
	for( PassBuffer& buffer : surface )
		buffer.Destroy();
	for( PassBuffer& buffer : caustic )
		buffer.Destroy();

	if( twiddleX != 0 )
		glDeleteTextures( 1, &twiddleX );
	if( twiddleY != 0 )
		glDeleteTextures( 1, &twiddleY );
	twiddleX = twiddleY = 0;

	if( meshVAO != 0 )
		glDeleteVertexArrays( 1, &meshVAO );
	meshVAO = 0;
	meshColumns = meshRows = 0;
	meshIndices            = 0;

	grid         = Grid {};
	bufferBanks  = -1;
	surfaceIndex = 0;

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
Water MillpondPlugin::CurrentWater() const
{
	Water water;
	water.gravity   = kGravity;
	water.tension   = TensionFromParam( params[ PT_TENSION ] );
	water.depth     = DepthFromParam( params[ PT_DEPTH ] );
	water.viscosity = ViscosityFromParam( params[ PT_VISCOSITY ] );
	return water;
}

GLuint MillpondPlugin::SurfaceTextureID() const
{
	return surface[ surfaceIndex ].TextureID();
}

GLuint MillpondPlugin::CausticTextureID() const
{
	return haveCaustics ? caustic[ causticIndex ].TextureID() : 0;
}

void MillpondPlugin::LoadSurfaceForTest( const std::vector< float >& rgba )
{
	if( grid.nx <= 0 || rgba.size() != static_cast< size_t >( grid.nx ) * grid.ny * 4 )
		return;
	glBindTexture( GL_TEXTURE_2D, surface[ surfaceIndex ].TextureID() );
	glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, grid.nx, grid.ny, GL_RGBA, GL_FLOAT, rgba.data() );
	glBindTexture( GL_TEXTURE_2D, 0 );
}

void MillpondPlugin::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}

//---------------------------------------------------------------------------
FFResult MillpondPlugin::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

char* MillpondPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_TEXT )
	{
		// Function-local rather than a member: the line is built from
		// compile-time facts, so it is the same for every instance, and the
		// host only needs the pointer to outlive the call.
		static const std::string text = stoatworks::about::textParam( 0 );
		return const_cast< char* >( text.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult MillpondPlugin::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_TEXT )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult MillpondPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_TEXT )
		return stoatworks::about::handleParam( index - PT_ABOUT_TEXT, value ) ? FF_SUCCESS : FF_FAIL;

	//The three buttons act on the press, once. An event parameter arrives as
	//1 on press and 0 on release, and a host is free to restate either --
	//so it is the rising edge that counts, never the level.
	const bool down = value >= 0.5f;
	if( index == PT_DROP )
	{
		if( down && !dropHeld )
			++dropPresses;
		dropHeld = down;
	}
	else if( index == PT_SKIM )
	{
		if( down && !skimHeld )
			++skimPresses;
		skimHeld = down;
	}
	else if( index == PT_STILL )
	{
		if( down && !stillHeld )
			stillWanted = true;
		stillHeld = down;
	}

	params[ index ] = value;
	return FF_SUCCESS;
}

float MillpondPlugin::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

} // namespace millpond
