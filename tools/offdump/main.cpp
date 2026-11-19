/**
    offdump -- render a fixed set of scenes and write every float the plugin
    produced, so two BUILDS can be compared bit for bit.

        offdump OUTDIR WxH

    `tools/offcompare.sh` builds this twice -- against this tree and against
    the last commit before Faraday -- and compares the files with cmp. It is
    how "with Shake at 0 the plugin is exactly v0.1.0" is proved rather than
    asserted: mptest's --faraday-off compares this build with itself, which
    shows the new controls are inert at Shake 0 but says nothing about
    whether the code they live in changed the old path.

    So this file must compile against BOTH trees: it uses only what v0.1.0's
    plugin already had (the parameter ids up to PT_MIX, SurfaceTextureID,
    CausticTextureID, SetClockScaleForTest). Anything newer goes behind
    PT_COUNT, a compile-time constant that differs between the two.

    OFFDUMP_SHAKE=x sets Shake to x in the far variant: offcompare.sh's
    negative control, which must then find differences.

    Per scene it writes the output picture (RGBA float), the surface
    (eta, eta_t / 40, eta_x, eta_y per grid cell) and the caustic buffer, and
    once the stateful part of the parameter table v0.1.0 declared: ids 0-35,
    every control and the About line, with names, kinds and defaults, which a
    saved composition depends on. v0.1.0's three About buttons (36-38) are
    left out: they are events, which hold no state, and v0.2.0's generated
    block put its new "User guide" button first, so their labels moved down
    one (Controls.h).

    A newer build also renders every scene a second time with the five other
    Faraday controls at their far ends and Shake at 0 ("far"); both of its
    variants are compared with the old build's one.

    GPU only, and one GPU: two processes on the same renderer are
    deterministic, two renderers are not, and neither is Apple's software
    renderer against itself across builds that schedule differently.
*/

#include "Audio.h"
#include "Controls.h"
#include "Millpond.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace millpond;

namespace
{
using Floats = std::vector< float >;

/// The number of parameters v0.1.0 declared: everything up to the About block's
/// last button.
constexpr unsigned int kV010Params = 39;

/// The ones that carry state: every control and the About line (ids 0-35). A
/// newer build must declare these first, unchanged. The About buttons after
/// them are events, and the user guide's button relabelled them in v0.2.0.
constexpr unsigned int kV010Stateful = 36;

/// The six Faraday controls are the last six ids in a build that has them
/// (40-45 since the guide's button; they were 39-44 before v0.2.0).
constexpr unsigned int kFaradayCount = 6;

CGLContextObj createContext()
{
	const CGLPixelFormatAttribute attributes[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	CGLPixelFormatObj format = nullptr;
	GLint count              = 0;
	if( CGLChoosePixelFormat( attributes, &format, &count ) != kCGLNoError || format == nullptr )
		return nullptr;
	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

/// A busy deterministic picture: a gradient with integer-hashed grain, so the
/// refraction has something to bend everywhere.
Floats card( int width, int height )
{
	Floats pixels( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			uint32_t h = static_cast< uint32_t >( x ) * 73856093u ^ static_cast< uint32_t >( y ) * 19349663u;
			h ^= h >> 13;
			h *= 0x5bd1e995u;
			h ^= h >> 15;
			const float grain = static_cast< float >( h & 0xffffu ) / 65535.0f;
			const float u     = ( x + 0.5f ) / width;
			const float v     = ( y + 0.5f ) / height;
			float* p          = &pixels[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			p[ 0 ] = 0.2f + 0.6f * u + 0.1f * grain;
			p[ 1 ] = 0.3f + 0.4f * v + 0.1f * grain;
			p[ 2 ] = 0.5f + 0.3f * std::sin( 20.0f * u * v ) * grain;
			p[ 3 ] = 1.0f;
		}
	return pixels;
}

GLuint makeTexture( int width, int height, const float* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

bool writeFile( const std::string& path, const void* data, size_t bytes )
{
	FILE* file = std::fopen( path.c_str(), "wb" );
	if( !file )
		return false;
	const bool ok = std::fwrite( data, 1, bytes, file ) == bytes;
	return std::fclose( file ) == 0 && ok;
}

/// One scene: what it sets, what it presses when, and how long it runs.
struct Scene
{
	const char* name;
	int frames;
	void ( *setup )( MillpondPlugin& );
	int dropFrame  = -1;
	int skimFrame  = -1;
	int stillFrame = -1;
	bool beat      = false;
};

void set( MillpondPlugin& p, unsigned int id, float value )
{
	p.SetFloatParameter( id, value );
}

const Scene kScenes[] = {
	{ "defaults", 120, []( MillpondPlugin& ) {} },
	{ "events", 150, []( MillpondPlugin& p ) {
		 set( p, PT_BANKS, 1.0f );
		 set( p, PT_SCATTER, 0.4f );
		 set( p, PT_AUDIO_PEBBLES, 0.6f );
		 set( p, PT_AUDIO_RAIN, 0.6f );
	 }, 10, 30, -1, true },
	{ "still", 100, []( MillpondPlugin& p ) { set( p, PT_RAIN, 0.6f ); }, -1, -1, 60 },
	{ "coarse", 90, []( MillpondPlugin& p ) {
		 set( p, PT_DETAIL, 0.0f );
		 set( p, PT_RAIN, 0.7f );
		 set( p, PT_SPEED, 1.0f );
	 } },
	{ "fine", 60, []( MillpondPlugin& p ) {
		 set( p, PT_DETAIL, 3.0f );
		 set( p, PT_POND_SIZE, 0.9f );
		 set( p, PT_DEPTH, 0.2f );
		 set( p, PT_TENSION, 1.0f );
		 set( p, PT_VISCOSITY, 0.9f );
	 }, 5 },
	{ "light", 90, []( MillpondPlugin& p ) {
		 set( p, PT_CAUSTICS, 1.0f );
		 set( p, PT_SUN_ELEVATION, 0.3f );
		 set( p, PT_SUN_AZIMUTH, 0.8f );
		 set( p, PT_SUN_SIZE, 0.9f );
		 set( p, PT_REFLECTION, 0.9f );
		 set( p, PT_GLINT, 1.0f );
		 set( p, PT_MIX, 0.7f );
	 }, 20 },
	{ "view1", 60, []( MillpondPlugin& p ) { set( p, PT_VIEW, 1.0f ); }, 10 },
	{ "view2", 60, []( MillpondPlugin& p ) { set( p, PT_VIEW, 2.0f ); }, 10 },
	{ "view3", 60, []( MillpondPlugin& p ) { set( p, PT_VIEW, 3.0f ); }, 10 },
	{ "frozen", 40, []( MillpondPlugin& p ) { set( p, PT_SPEED, 0.0f ); }, 5 },
};

/// The Faraday controls other than Shake, at the ends furthest from their
/// defaults. Only a build that has them; Shake (the first of them) stays 0.
void farEnds( MillpondPlugin& p )
{
	if constexpr( PT_COUNT > kV010Params + kFaradayCount - 1 )
	{
		constexpr unsigned int shakeId = PT_COUNT - kFaradayCount;
		const float ends[ 5 ] = { 1.0f, 0.0f, 1.0f, 2.0f, 1.0f };//Drive, Damping, Slow Motion, Pattern, Bass Bins
		for( unsigned int i = 0; i < 5; ++i )
			p.SetFloatParameter( shakeId + 1 + i, ends[ i ] );
		if( const char* shake = std::getenv( "OFFDUMP_SHAKE" ) )
			p.SetFloatParameter( shakeId, static_cast< float >( std::atof( shake ) ) );
	}
	else
		(void)p;
}

bool renderScene( const Scene& scene, bool far, int width, int height, const std::string& stem )
{
	MillpondPlugin plugin;
	FFGLViewportStruct viewport = {};
	viewport.width              = static_cast< FFUInt32 >( width );
	viewport.height             = static_cast< FFUInt32 >( height );
	if( plugin.InitGL( &viewport ) != FF_SUCCESS )
	{
		std::fprintf( stderr, "offdump: InitGL failed\n" );
		return false;
	}
	plugin.SetClockScaleForTest( 1.0 );
	scene.setup( plugin );
	if( far )
		farEnds( plugin );

	const Floats picture = card( width, height );
	const GLuint source  = makeTexture( width, height, picture.data() );
	const GLuint output  = makeTexture( width, height, nullptr );
	GLuint fbo           = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output, 0 );

	FFGLTextureStruct input = {};
	input.Width = input.HardwareWidth = static_cast< FFUInt32 >( width );
	input.Height = input.HardwareHeight = static_cast< FFUInt32 >( height );
	input.Handle                        = source;
	FFGLTextureStruct* inputs[ 1 ]      = { &input };
	ProcessOpenGLStruct process         = {};
	process.numInputTextures            = 1;
	process.inputTextures               = inputs;
	process.HostFBO                     = fbo;

	bool ok = true;
	for( int f = 0; f < scene.frames && ok; ++f )
	{
		const double seconds = f / 60.0;
		plugin.SetTime( seconds );
		if( scene.beat )
		{
			const float strike = static_cast< float >( 0.15 + 1.5 * std::exp( -std::fmod( seconds, 0.5 ) / 0.06 ) );
			for( int bin = 0; bin < audio::kBins; ++bin )
				plugin.SetParamElementValue( PT_AUDIO, static_cast< unsigned int >( bin ),
				                             strike * ( 1.0f - static_cast< float >( bin ) / audio::kBins ) );
		}
		auto press = [ & ]( int at, unsigned int id ) {
			if( f == at )
			{
				plugin.SetFloatParameter( id, 1.0f );
				plugin.SetFloatParameter( id, 0.0f );
			}
		};
		press( scene.dropFrame, PT_DROP );
		press( scene.skimFrame, PT_SKIM );
		press( scene.stillFrame, PT_STILL );

		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
	}

	if( ok )
	{
		Floats out( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, out.data() );
		ok = writeFile( stem + ".out", out.data(), out.size() * sizeof( float ) );

		const Grid& grid = plugin.CurrentGrid();
		Floats surface( static_cast< size_t >( grid.nx ) * grid.ny * 4 );
		glBindTexture( GL_TEXTURE_2D, plugin.SurfaceTextureID() );
		glGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, surface.data() );
		ok = ok && writeFile( stem + ".surf", surface.data(), surface.size() * sizeof( float ) );

		Floats caustic;
		if( plugin.CausticTextureID() != 0 )
		{
			caustic.resize( static_cast< size_t >( plugin.CausticWidth() ) * plugin.CausticHeight() );
			glBindTexture( GL_TEXTURE_2D, plugin.CausticTextureID() );
			glGetTexImage( GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, caustic.data() );
		}
		glBindTexture( GL_TEXTURE_2D, 0 );
		ok = ok && writeFile( stem + ".caus", caustic.data(), caustic.size() * sizeof( float ) );
	}

	plugin.DeInitGL();
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	glDeleteFramebuffers( 1, &fbo );
	glDeleteTextures( 1, &output );
	glDeleteTextures( 1, &source );
	return ok;
}

} // namespace

int main( int argc, char** argv )
{
	if( argc != 3 )
	{
		std::fprintf( stderr, "usage: offdump OUTDIR WxH\n" );
		return 2;
	}
	const std::string dir = argv[ 1 ];
	int width = 0, height = 0;
	if( std::sscanf( argv[ 2 ], "%dx%d", &width, &height ) != 2 || width <= 0 || height <= 0 )
	{
		std::fprintf( stderr, "offdump: bad size %s\n", argv[ 2 ] );
		return 2;
	}

	//The parameter table a composition is saved against.
	{
		MillpondPlugin plugin;
		std::string table;
		for( unsigned int i = 0; i < kV010Stateful; ++i )
		{
			const char* name = plugin.GetParamName( i );
			char line[ 256 ];
			std::snprintf( line, sizeof( line ), "%u\t%s\t%u\t%.9g\n", i, name ? name : "?", plugin.GetParamType( i ),
			               plugin.GetFloatParameter( i ) );
			table += line;
		}
		if( !writeFile( dir + "/params.txt", table.data(), table.size() ) )
			return 1;
	}

	if( createContext() == nullptr )
	{
		std::fprintf( stderr, "offdump: no accelerated OpenGL 4.1 context\n" );
		return 1;
	}

	const bool newer = PT_COUNT > kV010Params;
	for( const Scene& scene : kScenes )
	{
		if( !renderScene( scene, false, width, height, dir + "/" + scene.name + ".base" ) )
			return 1;
		if( newer && !renderScene( scene, true, width, height, dir + "/" + scene.name + ".far" ) )
			return 1;
	}
	std::printf( "offdump: %zu scenes at %dx%d into %s%s\n", sizeof( kScenes ) / sizeof( kScenes[ 0 ] ), width, height,
	             dir.c_str(), newer ? " (and the Faraday far ends)" : "" );
	return 0;
}
