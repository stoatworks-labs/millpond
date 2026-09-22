/**
    mptest -- render Millpond offline, and measure what its water is doing.

    It drives the REAL plugin class, through the same ProcessOpenGL a host
    calls, on a synthetic 60 fps clock. A test that exercises a
    reimplementation tests the reimplementation.

        mptest --out /tmp/frame.png     the card, under water
        mptest --card /tmp/card.png     the card on its own
        mptest --list                   every parameter and its default
        mptest --pipe                   raw frames in, raw frames out
        mptest --film N                 N frames of the card, raw frames out

    `--script` is a plain text file of `frame  Parameter Name  value` lines,
    held before the first key and after the last and linearly interpolated
    between -- the format of old-cathode's octest, tinsel's tinseltest and the
    rest, so one filming script can drive any of the fleet. The three buttons
    (Drop, Skim, Still) are events, and a track is held before its first key
    and interpolated between keys -- so a press is three keys, `29 Drop 0`,
    `30 Drop 1`, `31 Drop 0`, and it lands on frame 30. Leave out the key
    before and the value ramps up from the previous one, crossing 0.5 (the
    press) halfway there; docs/demo.cues is written out in full.

        ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
          | mptest --pipe --size 1920x1080 [--script cues.txt] \
          | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -r 60 -i - out.mov

    The claims, one flag each, in the order the README makes them:

        --fft          the GPU transform and its derivatives against a
                       double-precision DFT
        --modes        each Fourier mode rings at the dispersion relation's
                       frequency and decays at Lamb's rate
        --gravity      Cauchy-Poisson: with no surface tension, the rings at
                       radius r and time t have wavelength 8 pi r^2 / (g t^2)
        --quiet        with surface tension, the disc inside the ring is still,
                       and its edge sits where the Airy function says
        --shallow      in shallow water nothing outruns sqrt( g h )
        --banks        Walls hold the energy and pass no flow; Open lets it go
        --refraction   the bed is displaced by Snell's law, at two rasters
        --fresnel      the reflection is the exact Fresnel reflectance
        --caustics     light is conserved, and focused by the Jacobian
        --still        flat water bends nothing
        --skim         the hops shorten linearly, then the stone sinks
        --rain         the rain is Poisson at the rate asked for
        --audio        silence drops nothing; a beat drops a pebble a beat
        --negative     every check above, against a wrong model, must fail
        --bench        time a frame at 720p through 4K

    ----------------------------------------------------------- rasters

    The waves are simulated on a grid sized in METRES, so the physics checks
    are raster-independent by construction and run at one small raster. The
    optics checks read pixels, so they run at two rasters and fail if either
    is wrong.
*/

#include "Controls.h"
#include "Millpond.h"
#include "Physics.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace millpond;

namespace
{
constexpr double kPi = 3.14159265358979323846;

using Floats = std::vector< float >;
using Bytes  = std::vector< unsigned char >;

//---------------------------------------------------------------------------
// Reporting.
//---------------------------------------------------------------------------
int g_failures = 0;

std::string fmt( const char* format, ... )
{
	char buffer[ 1024 ];
	va_list args;
	va_start( args, format );
	std::vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	return buffer;
}

void Check( bool condition, const std::string& message )
{
	std::printf( "  %s  %s\n", condition ? "ok  " : "FAIL", message.c_str() );
	if( !condition )
		++g_failures;
}

int Verdict()
{
	std::printf( "\n  %s\n", g_failures == 0 ? "PASS" : "FAIL" );
	return g_failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// Deliberate errors, for --negative. Each field, when set, makes one check
// score the plugin against a model that is wrong by an amount the check is
// supposed to be able to see.
//---------------------------------------------------------------------------
struct Perturb
{
	double gravityFactor   = 1.0;///< --gravity expects this times g
	double tensionFactor   = 1.0;///< --quiet expects this times the tension
	double depthFactor     = 1.0;///< --shallow expects this times the depth
	double indexOverride   = 0.0;///< --refraction / --fresnel expect this index
	double derivativeSign  = 1.0;///< --fft expects this sign on the derivative
	double omegaFactor     = 1.0;///< --modes expects this times omega
	double focusDepth      = 1.0;///< --caustics expects this times the depth
	double skimLaw         = 0.0;///< --skim: 1 expects geometric, not linear, hops
	double rainFactor      = 1.0;///< --rain expects this times the rate
	bool banksSwapped      = false;///< --banks expects Open to hold and Walls to lose
	double stillSlack      = 0.0;///< --still adds this to the expected output
	bool audioDeaf         = false;///< --audio expects no pebbles from the beat
};

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( Bytes& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( Bytes& out, const char* type, const Bytes& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

/// `rgba` is floats, row 0 at the BOTTOM (GL's order); the file is written top
/// row first, which is the only place anything here flips.
bool writePng( const std::string& path, int width, int height, const Floats& rgba )
{
	Bytes raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		for( int x = 0; x < width; ++x )
			for( int c = 0; c < 4; ++c )
			{
				const float v = rgba[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ];
				raw.push_back( static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) ) );
			}
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	Bytes compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	Bytes png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	Bytes ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.insert( ihdr.end(), { 8, 6, 0, 0, 0 } );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Integer hashing, for the card. Never fract( sin( x ) * 43758.5453 ): that is
// the driver's answer, and two machines disagree about it.
//---------------------------------------------------------------------------
uint32_t lowbias32( uint32_t x )
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

double hash01( uint32_t a, uint32_t b = 0 )
{
	return static_cast< double >( lowbias32( a ^ lowbias32( b + 0x9e3779b9U ) ) ) / 4294967296.0;
}

//---------------------------------------------------------------------------
// The card: a stream bed. Sand, and pebbles on it, each shaded as a lump lit
// from the top left. Rows are v = 0 first, the way GL stores a texture.
//
// A bed rather than a test pattern because it is what the effect is FOR, and
// because it is busy at every scale: the refraction has something to bend
// everywhere, which is what the dead-control sweep needs.
//---------------------------------------------------------------------------
Floats buildCard( int width, int height )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );

	struct Stone
	{
		double u, v, rx, ry, angle;
		double r, g, b;
	};
	std::vector< Stone > stones;
	const double aspect = static_cast< double >( width ) / height;
	for( uint32_t i = 0; i < 70; ++i )
	{
		Stone s;
		s.u     = hash01( i, 1 ) * aspect;
		s.v     = hash01( i, 2 );
		s.rx    = 0.03 + 0.07 * hash01( i, 3 ) * hash01( i, 33 );
		s.ry    = s.rx * ( 0.55 + 0.4 * hash01( i, 4 ) );
		s.angle = kPi * hash01( i, 5 );
		const double tone = 0.25 + 0.55 * hash01( i, 6 );
		const double warm = hash01( i, 7 );
		s.r = tone * ( 0.85 + 0.3 * warm );
		s.g = tone * ( 0.85 + 0.1 * warm );
		s.b = tone * ( 0.95 - 0.25 * warm );
		stones.push_back( s );
	}

	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double u = ( x + 0.5 ) / height;//square units, so pebbles are round at any aspect
			const double v = ( y + 0.5 ) / height;

			//Sand: warm, with grain at two scales.
			const double grain = 0.06 * ( hash01( static_cast< uint32_t >( x * 7919 + y * 104729 ) ) - 0.5 )
			                     + 0.05 * std::sin( 31.0 * u + 7.0 * std::sin( 5.0 * v ) ) * std::sin( 23.0 * v );
			double r = 0.74 + grain, g = 0.64 + grain, b = 0.46 + grain;

			for( const Stone& s : stones )
			{
				const double dx = u - s.u, dy = v - s.v;
				const double c = std::cos( s.angle ), sn = std::sin( s.angle );
				const double px = ( c * dx + sn * dy ) / s.rx;
				const double py = ( -sn * dx + c * dy ) / s.ry;
				const double q  = px * px + py * py;
				if( q >= 1.0 )
				{
					//A soft contact shadow round the stone.
					if( q < 1.6 )
					{
						const double shade = 1.0 - 0.35 * ( 1.6 - q ) / 0.6;
						r *= shade;
						g *= shade;
						b *= shade;
					}
					continue;
				}
				//A dome, lit from the top left.
				const double z     = std::sqrt( 1.0 - q );
				const double light = std::clamp( 0.35 + 0.65 * ( -0.45 * px + 0.45 * py + 0.77 * z ), 0.0, 1.2 );
				const double speck = 0.05 * ( hash01( static_cast< uint32_t >( x * 31 + y * 17 ), 9 ) - 0.5 );
				r = s.r * light + speck;
				g = s.g * light + speck;
				b = s.b * light + speck;
			}

			const size_t o = ( static_cast< size_t >( y ) * width + x ) * 4;
			card[ o + 0 ] = static_cast< float >( std::clamp( r, 0.0, 1.0 ) );
			card[ o + 1 ] = static_cast< float >( std::clamp( g, 0.0, 1.0 ) );
			card[ o + 2 ] = static_cast< float >( std::clamp( b, 0.0, 1.0 ) );
			card[ o + 3 ] = 1.0f;
		}

	return card;
}

/// A coordinate card: R is the frame's u and G its v, exactly, at pixel
/// centres. Sampled bilinearly anywhere, it returns the coordinates of the
/// point sampled -- so the output IS the displacement map.
Floats coordinateCard( int width, int height )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const size_t o = ( static_cast< size_t >( y ) * width + x ) * 4;
			card[ o + 0 ]  = static_cast< float >( ( x + 0.5 ) / width );
			card[ o + 1 ]  = static_cast< float >( ( y + 0.5 ) / height );
			card[ o + 2 ]  = 0.0f;
			card[ o + 3 ]  = 1.0f;
		}
	return card;
}

Floats flatCard( int width, int height, float value )
{
	Floats card( static_cast< size_t >( width ) * height * 4, value );
	for( size_t i = 3; i < card.size(); i += 4 )
		card[ i ] = 1.0f;
	return card;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere without
	//a GPU, where it will at least prove the shaders compile.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
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

//---------------------------------------------------------------------------
// The audio the harness feeds, as rosette's harness feeds it: written into
// the Audio buffer's elements the way the host writes them.
//---------------------------------------------------------------------------
enum class AudioFeed
{
	Silence,
	Pulses///< a bass-heavy spectrum with a hit every half second
};

void feedAudio( MillpondPlugin& plugin, double seconds, AudioFeed feed )
{
	const double beat  = std::fmod( seconds, 0.5 );
	const float strike = feed == AudioFeed::Pulses ? static_cast< float >( 0.15 + 1.5 * std::exp( -beat / 0.06 ) ) : 0.0f;
	for( int bin = 0; bin < audio::kBins; ++bin )
	{
		const float across = static_cast< float >( bin ) / static_cast< float >( audio::kBins - 1 );
		const float shape  = 0.7f * ( 1.0f - across ) * ( 1.0f - across ) + 0.2f * ( 0.5f + 0.5f * std::sin( 25.0f * across ) );
		plugin.SetParamElementValue( PT_AUDIO, static_cast< unsigned int >( bin ), shape * strike );
	}
}

//---------------------------------------------------------------------------
// A rig: the real plugin, a float input texture and a float output
// framebuffer, at one size, on a synthetic 60 fps clock.
//---------------------------------------------------------------------------
struct Rig
{
	MillpondPlugin plugin;
	int width = 0, height = 0;
	GLuint sourceTexture = 0, outputTexture = 0, outputFBO = 0;
	int frame            = 0;
	double fps           = 60.0;
	AudioFeed feed       = AudioFeed::Silence;

	ProcessOpenGLStruct process    = {};
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };

	~Rig()
	{
		plugin.DeInitGL();
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
	}

	bool Init( int w, int h, const Floats* picture = nullptr )
	{
		width  = w;
		height = h;

		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		plugin.SetClockScaleForTest( 1.0 );

		const Floats card = picture ? *picture : buildCard( width, height );
		sourceTexture     = makeTexture( width, height, card.data() );
		outputTexture     = makeTexture( width, height, nullptr );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
		{
			std::fprintf( stderr, "the harness's own output framebuffer is not complete\n" );
			return false;
		}

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
		return true;
	}

	void Upload( const Floats& picture )
	{
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, picture.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void Set( unsigned int id, float value )
	{
		plugin.SetFloatParameter( id, value );
	}

	/// An event parameter, pressed and released, as a host sends one.
	void Press( unsigned int id )
	{
		plugin.SetFloatParameter( id, 1.0f );
		plugin.SetFloatParameter( id, 0.0f );
	}

	/// Still water, nothing falling on it: the base every physics check
	/// builds from, so that the only thing in the pond is what it drops.
	void Calm()
	{
		Set( PT_RAIN, 0.0f );
		Set( PT_AUDIO_PEBBLES, 0.0f );
		Set( PT_AUDIO_RAIN, 0.0f );
		Set( PT_SCATTER, 0.0f );
	}

	bool Render( int frames = 1 )
	{
		for( int i = 0; i < frames; ++i )
		{
			const double seconds = static_cast< double >( frame ) / fps;
			plugin.SetTime( seconds );
			feedAudio( plugin, seconds, feed );
			++frame;

			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );

			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "ProcessOpenGL failed\n" );
				return false;
			}
		}
		return true;
	}

	/// The output, RGBA floats, row 0 at the bottom.
	Floats Output() const
	{
		Floats pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}

	/// The surface, (eta, eta_t, eta_x, eta_y) per grid cell, row 0 first.
	Floats Surface() const
	{
		const Grid& grid = plugin.CurrentGrid();
		Floats data( static_cast< size_t >( grid.nx ) * grid.ny * 4 );
		glBindTexture( GL_TEXTURE_2D, plugin.SurfaceTextureID() );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, data.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return data;
	}

	/// The caustic buffer, one float per pixel, bed coordinates.
	Floats CausticMap() const
	{
		Floats data( static_cast< size_t >( plugin.CausticWidth() ) * plugin.CausticHeight() );
		const GLuint id = plugin.CausticTextureID();
		if( id == 0 )
			return {};
		glBindTexture( GL_TEXTURE_2D, id );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glGetTexImage( GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, data.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return data;
	}
};

//---------------------------------------------------------------------------
// Parameters in physical units. The inverses of Controls.cpp, so a check can
// say "2 metres" rather than "0.5654".
//---------------------------------------------------------------------------
float geometricParam( double value, double low, double high )
{
	return static_cast< float >( std::log( value / low ) / std::log( high / low ) );
}
float pondParam( double metres )
{
	return geometricParam( metres, 0.1, 20.0 );
}
float depthParam( double metres )
{
	return geometricParam( metres, 0.002, 2.0 );
}
float pebbleParam( double metres )
{
	return geometricParam( metres, 0.002, 0.10 );
}
float viscosityParam( double nu )
{
	return geometricParam( nu, 1e-7, 1e-2 );
}
float tensionParam( double tension )
{
	return static_cast< float >( tension / ( 2.0 * kWaterTension ) );
}
float detailParam( int cells )
{
	for( int i = 0; i < kDetailCount; ++i )
		if( kDetailCells[ i ] == cells )
			return static_cast< float >( i );
	return 2.0f;
}

//---------------------------------------------------------------------------
// Reading the surface. Grid point i sits at (i + 1/2) cells, periodic.
//---------------------------------------------------------------------------
double sampleSurface( const Floats& s, const Grid& g, double x, double y, int channel )
{
	const double fx = x / ( g.lx / g.nx ) - 0.5;
	const double fy = y / ( g.ly / g.ny ) - 0.5;
	const double x0 = std::floor( fx ), y0 = std::floor( fy );
	const double tx = fx - x0, ty = fy - y0;

	auto at = [ & ]( int i, int j ) {
		i = ( ( i % g.nx ) + g.nx ) % g.nx;
		j = ( ( j % g.ny ) + g.ny ) % g.ny;
		return static_cast< double >( s[ ( static_cast< size_t >( j ) * g.nx + i ) * 4 + channel ] );
	};
	const int i = static_cast< int >( x0 ), j = static_cast< int >( y0 );
	return ( 1 - ty ) * ( ( 1 - tx ) * at( i, j ) + tx * at( i + 1, j ) )
	       + ty * ( ( 1 - tx ) * at( i, j + 1 ) + tx * at( i + 1, j + 1 ) );
}

/// eta along a ray from (cx, cy) at `angle`, every `step` metres out to `reach`.
std::vector< double > radialProfile( const Floats& s, const Grid& g, double cx, double cy, double angle, double step,
                                     double reach )
{
	std::vector< double > out;
	for( double r = 0.0; r <= reach; r += step )
		out.push_back( sampleSurface( s, g, cx + r * std::cos( angle ), cy + r * std::sin( angle ), 0 ) );
	return out;
}

/// Zero crossings of a sampled profile, by linear interpolation, in metres.
std::vector< double > zeroCrossings( const std::vector< double >& p, double step, double from, double to )
{
	std::vector< double > out;
	for( size_t i = 0; i + 1 < p.size(); ++i )
	{
		const double r = static_cast< double >( i ) * step;
		if( r < from || r > to )
			continue;
		if( ( p[ i ] < 0.0 ) != ( p[ i + 1 ] < 0.0 ) && p[ i ] != p[ i + 1 ] )
			out.push_back( r + step * p[ i ] / ( p[ i ] - p[ i + 1 ] ) );
	}
	return out;
}

/// Set up a pond for a physics check, drop one pebble at the frame's centre,
/// and run it for `seconds` of water. The raster is small: the grid is in
/// metres, so the raster only decides how the result would be LOOKED at.
bool ringPond( Rig& rig, double pondMetres, double depth, double tension, double pebble, int cells, double seconds,
               double viscosity = 1e-7 )
{
	if( !rig.Init( 256, 144 ) )
		return false;
	rig.Calm();
	rig.Set( PT_POND_SIZE, pondParam( pondMetres ) );
	rig.Set( PT_DEPTH, depthParam( depth ) );
	rig.Set( PT_TENSION, tensionParam( tension ) );
	rig.Set( PT_VISCOSITY, viscosityParam( viscosity ) );
	rig.Set( PT_DETAIL, detailParam( cells ) );
	rig.Set( PT_PEBBLE_SIZE, pebbleParam( pebble ) );
	rig.Set( PT_PEBBLE_X, 0.5f );
	rig.Set( PT_PEBBLE_Y, 0.5f );
	rig.Set( PT_CAUSTICS, 0.0f );

	//Frame 0 has no previous frame, so no time passes on it: it allocates.
	if( !rig.Render( 1 ) )
		return false;

	//The press lands on the next frame, which injects the crater and then
	//advances the water one frame: after n frames the ring is n/60 s old.
	rig.Press( PT_DROP );
	const int frames = static_cast< int >( std::lround( seconds * rig.fps ) );
	return rig.Render( frames );
}

//---------------------------------------------------------------------------
// A double-precision DFT, O(N^2) per line on purpose: nothing in it shares an
// idea with a Stockham transform, so the two cannot agree by sharing a bug.
//---------------------------------------------------------------------------
using Complex = std::pair< double, double >;

void dft( std::vector< Complex >& line, double sign )
{
	const size_t n = line.size();
	std::vector< Complex > out( n );
	for( size_t k = 0; k < n; ++k )
	{
		double re = 0.0, im = 0.0;
		for( size_t j = 0; j < n; ++j )
		{
			const double a = sign * 2.0 * kPi * static_cast< double >( ( k * j ) % n ) / static_cast< double >( n );
			re += line[ j ].first * std::cos( a ) - line[ j ].second * std::sin( a );
			im += line[ j ].first * std::sin( a ) + line[ j ].second * std::cos( a );
		}
		out[ k ] = { re, im };
	}
	line = out;
}

void dft2( std::vector< Complex >& field, int nx, int ny, double sign )
{
	std::vector< Complex > line( nx );
	for( int y = 0; y < ny; ++y )
	{
		for( int x = 0; x < nx; ++x )
			line[ x ] = field[ static_cast< size_t >( y ) * nx + x ];
		dft( line, sign );
		for( int x = 0; x < nx; ++x )
			field[ static_cast< size_t >( y ) * nx + x ] = line[ x ];
	}
	line.resize( ny );
	for( int x = 0; x < nx; ++x )
	{
		for( int y = 0; y < ny; ++y )
			line[ y ] = field[ static_cast< size_t >( y ) * nx + x ];
		dft( line, sign );
		for( int y = 0; y < ny; ++y )
			field[ static_cast< size_t >( y ) * nx + x ] = line[ y ];
	}
}

//===========================================================================
// --fft
//===========================================================================
int runFFT( const Perturb& perturb )
{
	std::printf( "\n=== fft: the GPU transform, both ways, against a double-precision DFT\n" );

	Rig rig;
	if( !rig.Init( 128, 72 ) )
		return 1;
	rig.Calm();
	rig.Set( PT_SPEED, 0.0f );
	rig.Set( PT_DETAIL, detailParam( 256 ) );
	rig.Set( PT_CAUSTICS, 0.0f );
	if( !rig.Render( 1 ) )
		return 1;

	const Grid g = rig.plugin.CurrentGrid();
	const int nx = g.nx, ny = g.ny;
	const size_t cells = static_cast< size_t >( nx ) * ny;

	//White noise in both fields: every mode the grid has, at once.
	Floats state( cells * 4, 0.0f );
	for( size_t i = 0; i < cells; ++i )
	{
		state[ i * 4 + 0 ] = static_cast< float >( hash01( static_cast< uint32_t >( i ), 11 ) - 0.5 );
		state[ i * 4 + 1 ] = static_cast< float >( hash01( static_cast< uint32_t >( i ), 12 ) - 0.5 );
	}
	rig.plugin.LoadSurfaceForTest( state );
	rig.plugin.StepForTest( 0.0 );
	const Floats back = rig.Surface();

	//The expectation: each field transformed, its Nyquist row and column
	//removed (the plugin drops them, and says why), and for eta the two
	//spectral derivatives, i kx and i ky.
	auto spectrumOf = [ & ]( int channel ) {
		std::vector< Complex > f( cells );
		for( size_t i = 0; i < cells; ++i )
			f[ i ] = { state[ i * 4 + channel ], 0.0 };
		dft2( f, nx, ny, -1.0 );
		for( int y = 0; y < ny; ++y )
			for( int x = 0; x < nx; ++x )
				if( x == nx / 2 || y == ny / 2 )
					f[ static_cast< size_t >( y ) * nx + x ] = { 0.0, 0.0 };
		return f;
	};
	auto inverse = [ & ]( std::vector< Complex > f ) {
		dft2( f, nx, ny, 1.0 );
		std::vector< double > out( cells );
		for( size_t i = 0; i < cells; ++i )
			out[ i ] = f[ i ].first / static_cast< double >( cells );
		return out;
	};

	const std::vector< Complex > etaHat  = spectrumOf( 0 );
	const std::vector< Complex > rateHat = spectrumOf( 1 );

	std::vector< Complex > dxHat( cells ), dyHat( cells );
	for( int y = 0; y < ny; ++y )
		for( int x = 0; x < nx; ++x )
		{
			const size_t i  = static_cast< size_t >( y ) * nx + x;
			const int mx    = x < nx / 2 ? x : x - nx;
			const int my    = y < ny / 2 ? y : y - ny;
			const double kx = perturb.derivativeSign * 2.0 * kPi * mx / g.lx;
			const double ky = perturb.derivativeSign * 2.0 * kPi * my / g.ly;
			//i k F
			dxHat[ i ] = { -kx * etaHat[ i ].second, kx * etaHat[ i ].first };
			dyHat[ i ] = { -ky * etaHat[ i ].second, ky * etaHat[ i ].first };
		}

	const std::vector< double > expected[ 4 ] = { inverse( etaHat ), inverse( rateHat ), inverse( dxHat ),
		                                          inverse( dyHat ) };
	const char* names[ 4 ] = { "eta", "eta_t/40", "d eta/dx", "d eta/dy" };

	for( int c = 0; c < 4; ++c )
	{
		double errorSq = 0.0, signalSq = 0.0, worst = 0.0;
		for( size_t i = 0; i < cells; ++i )
		{
			const double e = back[ i * 4 + c ] - expected[ c ][ i ];
			errorSq += e * e;
			signalSq += expected[ c ][ i ] * expected[ c ][ i ];
			worst = std::max( worst, std::fabs( e ) );
		}
		const double relative = std::sqrt( errorSq / std::max( signalSq, 1e-300 ) );
		//Float32 through 8 + 7 radix-2 stages each way: rounding grows as
		//sqrt( stages ) times the 6e-8 unit roundoff, so a few 1e-7 is the
		//honest floor. 1e-5 is two orders above it and six below a sign error.
		Check( relative < 1e-5, fmt( "%-9s  relative rms error %.2e over %dx%d (bound 1e-5), worst %.2e", names[ c ],
		                             relative, nx, ny, worst ) );
	}

	return Verdict();
}

//===========================================================================
// --modes
//===========================================================================
int runModes( const Perturb& perturb )
{
	std::printf( "\n=== modes: each wave rings at omega(k) and decays at 2 nu k^2\n" );

	struct Case
	{
		const char* what;
		double tension, depth, viscosity;
		int mx, my;
		double dt;
	};
	const Case cases[] = {
		{ "gravity, deep", 0.0, 2.0, 1e-7, 3, 0, 0.37 },
		{ "capillary-gravity", kWaterTension, 2.0, 1e-6, 40, 9, 0.113 },
		{ "shallow", kWaterTension, 0.004, 1e-6, 5, 2, 0.5 },
		{ "syrup (overdamped)", kWaterTension, 0.2, 5e-3, 30, 0, 0.05 },
	};

	for( const Case& c : cases )
	{
		Rig rig;
		if( !rig.Init( 128, 72 ) )
			return 1;
		rig.Calm();
		rig.Set( PT_SPEED, 0.0f );
		//Walls: the wave fills the whole periodic domain, three quarters of
		//which is the Open sponge. This check is about the propagator.
		rig.Set( PT_BANKS, static_cast< float >( Banks::Walls ) );
		rig.Set( PT_POND_SIZE, pondParam( 1.0 ) );
		rig.Set( PT_DETAIL, detailParam( 256 ) );
		rig.Set( PT_TENSION, tensionParam( c.tension ) );
		rig.Set( PT_DEPTH, depthParam( c.depth ) );
		rig.Set( PT_VISCOSITY, viscosityParam( c.viscosity ) );
		rig.Set( PT_CAUSTICS, 0.0f );
		if( !rig.Render( 1 ) )
			return 1;

		const Grid g         = rig.plugin.CurrentGrid();
		const Water water    = rig.plugin.CurrentWater();
		const double kx      = 2.0 * kPi * c.mx / g.lx;
		const double ky      = 2.0 * kPi * c.my / g.ly;
		const double k       = std::hypot( kx, ky );
		const double omega   = Omega( k, water ) * perturb.omegaFactor;
		const double gamma   = DampingRate( k, water );
		const double amp     = 1e-3;

		//A standing wave, eta = A cos( k . x ), released from rest.
		Floats state( static_cast< size_t >( g.nx ) * g.ny * 4, 0.0f );
		for( int y = 0; y < g.ny; ++y )
			for( int x = 0; x < g.nx; ++x )
			{
				const double px = ( x + 0.5 ) * g.lx / g.nx, py = ( y + 0.5 ) * g.ly / g.ny;
				state[ ( static_cast< size_t >( y ) * g.nx + x ) * 4 ] =
					static_cast< float >( amp * std::cos( kx * px + ky * py ) );
			}
		rig.plugin.LoadSurfaceForTest( state );
		rig.plugin.StepForTest( c.dt );
		const Floats back = rig.Surface();

		//The textbook damped oscillator, released from rest:
		//eta(t) = A e^{-gamma t} ( cos wd t + gamma/wd sin wd t ) on either side
		//of critical damping (sin/sinh as appropriate).
		double factor, rateFactor;
		const double disc = omega * omega - gamma * gamma;
		if( disc > 0.0 )
		{
			const double wd = std::sqrt( disc );
			factor          = std::exp( -gamma * c.dt ) * ( std::cos( wd * c.dt ) + gamma / wd * std::sin( wd * c.dt ) );
			rateFactor      = -std::exp( -gamma * c.dt ) * omega * omega / wd * std::sin( wd * c.dt );
		}
		else
		{
			const double wd = std::sqrt( -disc );
			factor = std::exp( -gamma * c.dt ) * ( std::cosh( wd * c.dt ) + gamma / wd * std::sinh( wd * c.dt ) );
			rateFactor = -std::exp( -gamma * c.dt ) * omega * omega / wd * std::sinh( wd * c.dt );
		}

		//Project the result onto the mode: the amplitude it came back with.
		double projEta = 0.0, projRate = 0.0, norm = 0.0;
		for( int y = 0; y < g.ny; ++y )
			for( int x = 0; x < g.nx; ++x )
			{
				const double px = ( x + 0.5 ) * g.lx / g.nx, py = ( y + 0.5 ) * g.ly / g.ny;
				const double basis = std::cos( kx * px + ky * py );
				const size_t i     = static_cast< size_t >( y ) * g.nx + x;
				projEta += back[ i * 4 + 0 ] * basis;
				projRate += back[ i * 4 + 1 ] * basis;
				norm += basis * basis;
			}
		const double gotEta  = projEta / norm / amp;
		const double gotRate = projRate / norm / amp * kRateScale;

		//Float phase: omega t reaches a few hundred radians here and a float
		//holds that to about 3e-5 of a radian. 2e-4 of the unit amplitude is
		//several times that and a small fraction of a percent of omega.
		const double tolerance = 2e-4 + 1e-6 * omega * c.dt;
		Check( std::fabs( gotEta - factor ) < tolerance && std::fabs( gotRate - rateFactor ) < tolerance * std::max( omega, 1.0 ),
		       fmt( "%-19s k=%7.1f omega=%8.3f gamma=%.2e: eta %+.5f (expect %+.5f), eta_t %+.4f (expect %+.4f)", c.what,
		            k, omega, gamma, gotEta, factor, gotRate, rateFactor ) );
	}

	return Verdict();
}

//===========================================================================
// The exact linear solution for one crater on open water, in double.
//
// The crater is eta0( r ) = -A ( 1 - r^2/a^2 ) exp( -r^2/a^2 ), released from
// rest. Its Hankel transform is A k^2 a^4 / 8 exp( -k^2 a^2 / 4 ) -- worked in
// AGENTS.md -- so linear theory gives, for all time, the Cauchy-Poisson
// integral
//
//     eta( r, t ) = -A a^4/8  integral k^3 exp( -k^2 a^2/4 ) J0( k r ) cos( omega(k) t ) dk
//
// with omega from the dispersion relation (and each mode's cos( omega t )
// damped at Lamb's rate, which at the harness's 1e-7 m^2/s is still two
// percent by five seconds). Nothing in it is asymptotic: the
// near field, the Airy edges and the long waves out front are all in it.
//===========================================================================

/// J0 in double, from its definitions: the power series below x = 12, where
/// the largest term is about 4e3 and double keeps 1e-13 of the sum, and the
/// Hankel asymptotic expansion above, whose first omitted term at x = 12 is
/// under 1e-10. Written out here rather than borrowed.
double besselJ0( double x )
{
	const double ax = std::fabs( x );
	if( ax < 12.0 )
	{
		const double q = 0.25 * ax * ax;
		double term = 1.0, sum = 1.0;
		for( int k = 1; k < 60; ++k )
		{
			term *= -q / ( static_cast< double >( k ) * k );
			sum += term;
			if( std::fabs( term ) < 1e-18 )
				break;
		}
		return sum;
	}

	//J0(x) = sqrt( 2 / (pi x) ) ( P cos( x - pi/4 ) - Q sin( x - pi/4 ) ),
	//P and Q the standard series in t_k = 1^2 3^2 .. (2k-1)^2 / ( k! (8x)^k ):
	//P = 1 - t_2 + t_4 - ..., Q = -t_1 + t_3 - ...
	const double z = 1.0 / ( 8.0 * ax );
	double p = 1.0, q = 0.0, term = 1.0;
	for( int k = 1; k <= 12; ++k )
	{
		const double odd = 2.0 * k - 1.0;
		term *= odd * odd * z / k;
		if( k % 2 == 1 )
			q += ( ( k / 2 ) % 2 == 0 ? -1.0 : 1.0 ) * term;
		else
			p += ( ( k / 2 ) % 2 == 0 ? 1.0 : -1.0 ) * term;
	}
	const double phase = ax - 0.25 * kPi;
	return std::sqrt( 2.0 / ( kPi * ax ) ) * ( p * std::cos( phase ) - q * std::sin( phase ) );
}

std::vector< double > cauchyPoisson( const Water& water, double a, double depthOfCrater, double t,
                                     const std::vector< double >& radii )
{
	//The integrand's phase runs at up to r + c_g t per unit k; a step of a
	//fiftieth of pi over that resolves it with room to spare.
	const double kMax  = 10.0 / a;
	double fastest     = 0.0;
	for( double k = 0.5; k < kMax; k += 0.5 )
		fastest = std::max( fastest, GroupVelocity( k, water ) );
	const double reach = radii.empty() ? 1.0 : *std::max_element( radii.begin(), radii.end() );
	const double dk    = std::min( 0.02 * kPi / ( reach + fastest * t ), 0.05 );

	std::vector< double > weight, phase, ks;
	for( double k = 0.5 * dk; k < kMax; k += dk )
	{
		ks.push_back( k );
		//Each mode released from rest, as a damped oscillator at Lamb's rate.
		const double omega = Omega( k, water ), gamma = DampingRate( k, water );
		const double wd    = std::sqrt( std::max( omega * omega - gamma * gamma, 1e-30 ) );
		const double swing = std::exp( -gamma * t ) * ( std::cos( wd * t ) + gamma / wd * std::sin( wd * t ) );
		weight.push_back( k * k * k * std::exp( -k * k * a * a / 4.0 ) * swing * dk );
	}

	std::vector< double > out;
	out.reserve( radii.size() );
	for( double r : radii )
	{
		double sum = 0.0;
		for( size_t i = 0; i < ks.size(); ++i )
			sum += weight[ i ] * besselJ0( ks[ i ] * r );
		out.push_back( -depthOfCrater * std::pow( a, 4.0 ) / 8.0 * sum );
	}
	return out;
}

/// The GPU's rings against the exact integral, at every grid point whose
/// distance from the crater is in [from, to]. At grid points, not between
/// them: reading a surface between its samples is linear interpolation, which
/// is off by (k dx)^2 / 8 of the amplitude -- six percent for the shortest
/// ring here -- and that would be measuring the harness.
///
/// Returns the worst difference as a fraction of the largest |eta| the theory
/// has in that annulus.
double ringsAgainstTheory( const Floats& s, const Grid& g, double cx, double cy, const Water& theory, double a,
                           double depthOfCrater, double t, double from, double to, double& largest )
{
	//The theory on a fine radial table, then read by linear interpolation --
	//whose error at 0.25 mm is (k dr)^2/8, under 1e-4 of the amplitude.
	const double dr = 0.00025;
	std::vector< double > radii;
	for( double r = from - dr; r <= to + dr; r += dr )
		radii.push_back( r );
	const std::vector< double > table = cauchyPoisson( theory, a, depthOfCrater, t, radii );

	largest = 0.0;
	for( double v : table )
		largest = std::max( largest, std::fabs( v ) );

	double worst = 0.0;
	const double dx = g.lx / g.nx, dy = g.ly / g.ny;
	for( int j = 0; j < g.ny; ++j )
		for( int i = 0; i < g.nx; ++i )
		{
			const double r = std::hypot( ( i + 0.5 ) * dx - cx, ( j + 0.5 ) * dy - cy );
			if( r < from || r > to )
				continue;
			const double f    = ( r - radii.front() ) / dr;
			const size_t k    = static_cast< size_t >( f );
			const double w    = f - static_cast< double >( k );
			const double want = ( 1.0 - w ) * table[ k ] + w * table[ k + 1 ];
			const double got  = s[ ( static_cast< size_t >( j ) * g.nx + i ) * 4 ];
			worst             = std::max( worst, std::fabs( got - want ) );
		}
	return worst / std::max( largest, 1e-30 );
}

//===========================================================================
// --gravity
//===========================================================================
int runGravity( const Perturb& perturb )
{
	std::printf( "\n=== gravity: no surface tension, deep water -- the rings against the Cauchy-Poisson integral\n" );

	const double pond = 3.0, pebble = 0.03;
	for( double t : { 1.0, 2.5 } )
	{
		Rig rig;
		if( !ringPond( rig, pond, 2.0, 0.0, pebble, 2048, t ) )
			return 1;

		const Floats s   = rig.Surface();
		const Grid& g    = rig.plugin.CurrentGrid();
		const double cx  = 0.5 * rig.plugin.FrameWidthMetres();
		const double cy  = 0.5 * rig.plugin.FrameHeightMetres();
		const double age = std::lround( t * 60.0 ) / 60.0;

		Water theory     = rig.plugin.CurrentWater();
		theory.gravity   = kGravity * perturb.gravityFactor;
		const double amp = SplashFromParam( rig.plugin.GetFloatParameter( PT_SPLASH ) ) * pebble;

		double largest     = 0.0;
		const double worst = ringsAgainstTheory( s, g, cx, cy, theory, pebble, amp, age, 2.0 * pebble, 1.2, largest );

		//What the grid can carry: the crater is sampled at 5 mm, a sixth of
		//its radius, and the spectrum past Nyquist (k a = 18) is exp( -80 )
		//of the peak. What is left is float rounding, fed back 150 times:
		//measured 1e-4 of the ring here. The bound is 0.2%, twenty times that
		//for another GPU's rounding; the negative control shows a 1% error in
		//g breaks it by far more.
		Check( worst < 0.002, fmt( "t=%.2f s: out to 1.2 m, within %.3f%% of the exact integral's largest ring (%.2f mm; "
		                          "bound 0.2%%)",
		                          age, 100.0 * worst, 1000.0 * largest ) );

		//And the classic reading of it: at radius r the wavelength is
		//8 pi r^2 / (g t^2). Printed, not checked: it is the leading term of
		//an asymptotic series, off by O( 1/kr ) near the crater, and the
		//integral above is the exact statement.
		const double r = 0.45 * std::sqrt( age );
		std::printf( "        (at r = %.2f m the leading-order wavelength is %.1f cm)\n", r,
		             100.0 * 8.0 * kPi * r * r / ( kGravity * age * age ) );
	}

	return Verdict();
}

//===========================================================================
// --quiet
//===========================================================================
int runQuiet( const Perturb& perturb )
{
	std::printf( "\n=== quiet: the still disc inside the ring, and the Airy edge of it\n" );

	Water water;
	water.tension   = kWaterTension * perturb.tensionFactor;
	water.depth     = 2.0;
	water.viscosity = 1e-7;
	const GroupMinimum minimum = FindGroupMinimum( water );
	std::printf( "  minimum group velocity %.5f m/s at k = %.1f rad/m (wavelength %.2f cm), omega''' = %.3e\n",
	             minimum.velocity, minimum.k, 200.0 * kPi / minimum.k, minimum.third );

	const double pebble = 0.015;
	for( double t : { 3.0, 5.0 } )
	{
		Rig rig;
		if( !ringPond( rig, 2.0, 2.0, kWaterTension, pebble, 2048, t ) )
			return 1;

		const Floats s  = rig.Surface();
		const Grid& g   = rig.plugin.CurrentGrid();
		const double cx = 0.5 * rig.plugin.FrameWidthMetres();
		const double cy = 0.5 * rig.plugin.FrameHeightMetres();
		const double age = std::lround( t * 60.0 ) / 60.0;

		//The first maximum of Ai( -z ) is at z = 1.0188, and the ring's edge
		//is an Airy function of ( r - c t ) / ( omega''' t / 2 )^(1/3).
		const double scale     = std::cbrt( minimum.third * age / 2.0 );
		const double predicted = minimum.velocity * age + 1.0188 * scale;
		const double lambda    = 2.0 * kPi / minimum.k;

		double worstEdge = 0.0, worstQuiet = 0.0;
		for( double angle : { 0.3, 1.4, 2.6, 4.0, 5.3 } )
		{
			const double step = 0.0005;
			const double reach = std::min( 0.95, minimum.velocity * age + 0.3 );
			const std::vector< double > p = radialProfile( s, g, cx, cy, angle, step, reach );

			//Envelope: the largest |eta| within half a carrier wavelength.
			const int window = static_cast< int >( 0.5 * lambda / step );
			std::vector< double > envelope( p.size(), 0.0 );
			for( size_t i = 0; i < p.size(); ++i )
				for( int j = -window; j <= window; ++j )
				{
					const long k = static_cast< long >( i ) + j;
					if( k >= 0 && k < static_cast< long >( p.size() ) )
						envelope[ i ] = std::max( envelope[ i ], std::fabs( p[ static_cast< size_t >( k ) ] ) );
				}

			//The ring's peak, looking outward from half the edge radius: the
			//crater's own remains at the centre are not the ring.
			size_t best = 0;
			double top  = 0.0;
			for( size_t i = 0; i < p.size(); ++i )
			{
				const double r = static_cast< double >( i ) * step;
				if( r > 0.5 * minimum.velocity * age && envelope[ i ] > top )
				{
					top  = envelope[ i ];
					best = i;
				}
			}
			//Smooth the plateau: the envelope is flat across a window, so take
			//the centre of the run at the maximum.
			size_t last = best;
			while( last + 1 < envelope.size() && envelope[ last + 1 ] >= top * 0.999 )
				++last;
			const double edge = 0.5 * static_cast< double >( best + last ) * step;
			worstEdge         = std::max( worstEdge, std::fabs( edge - predicted ) );

			double inside = 0.0;
			for( size_t i = 0; i < p.size(); ++i )
			{
				const double r = static_cast< double >( i ) * step;
				if( r > 3.0 * pebble && r < 0.85 * minimum.velocity * age )
					inside = std::max( inside, std::fabs( p[ i ] ) );
			}
			worstQuiet = std::max( worstQuiet, inside / std::max( top, 1e-30 ) );
		}

		//The carrier's phase decides which crest of the envelope's top the
		//window lands on: a quarter wavelength either way, plus one grid cell.
		const double bound = 0.25 * lambda + g.lx / g.nx;
		Check( worstEdge < bound, fmt( "t=%.2f s: ring peak within %.2f cm of c t + 1.019 (w''' t/2)^1/3 = %.3f m "
		                               "(bound %.2f cm)",
		                               age, 100.0 * worstEdge, predicted, 100.0 * bound ) );
		Check( worstQuiet < 0.03, fmt( "t=%.2f s: inside 0.85 c t the water is still: %.2f%% of the ring's height "
		                               "(bound 3%%)",
		                               age, 100.0 * worstQuiet ) );

		//And the whole of it, not only the edge: the exact integral, with
		//surface tension, at every grid point out to beyond the ring.
		const double amp   = SplashFromParam( rig.plugin.GetFloatParameter( PT_SPLASH ) ) * pebble;
		double largest     = 0.0;
		const double exact = ringsAgainstTheory( s, g, cx, cy, water, pebble, amp, age, 2.0 * pebble,
		                                         std::min( 0.95, minimum.velocity * age + 0.25 ), largest );
		Check( exact < 0.002, fmt( "t=%.2f s: within %.3f%% of the exact Cauchy-Poisson integral with surface tension "
		                          "(bound 0.2%%)",
		                          age, 100.0 * exact ) );
	}

	return Verdict();
}

//===========================================================================
// --shallow
//===========================================================================
int runShallow( const Perturb& perturb )
{
	std::printf( "\n=== shallow: in 1 cm of water nothing outruns sqrt( g h )\n" );

	const double depth  = 0.01;
	const double pebble = 0.02;
	const double h      = depth * perturb.depthFactor;
	const double c0     = std::sqrt( kGravity * h );

	for( double t : { 1.0, 2.0 } )
	{
		Rig rig;
		if( !ringPond( rig, 2.0, depth, 0.0, pebble, 2048, t ) )
			return 1;

		const Floats s   = rig.Surface();
		const Grid& g    = rig.plugin.CurrentGrid();
		const double cx  = 0.5 * rig.plugin.FrameWidthMetres();
		const double cy  = 0.5 * rig.plugin.FrameHeightMetres();
		const double age = std::lround( t * 60.0 ) / 60.0;

		//The leading edge is itself an Airy front: omega = c0 k - c0 h^2 k^3/6
		//near k = 0, so it spreads over (c0 h^2 t / 2)^(1/3) ahead of c0 t.
		const double spread = std::cbrt( c0 * h * h * age / 2.0 );

		double outermost = 0.0, peak = 0.0;
		std::vector< std::vector< double > > profiles;
		const double step = 0.0005;
		for( double angle : { 0.0, 1.1, 2.3, 3.9, 5.0 } )
			profiles.push_back( radialProfile( s, g, cx, cy, angle, step, 0.95 ) );
		for( const auto& p : profiles )
			for( double v : p )
				peak = std::max( peak, std::fabs( v ) );
		for( const auto& p : profiles )
			for( size_t i = 0; i < p.size(); ++i )
				if( std::fabs( p[ i ] ) > 0.02 * peak )
					outermost = std::max( outermost, static_cast< double >( i ) * step );

		//Beyond the front the Airy tail is exp( -2/3 z^1.5 ): 2% of the peak
		//is z = 3 or so. The crater's own radius rides on top.
		const double ceiling = c0 * age + 3.0 * spread + 2.0 * pebble;
		const double floor   = c0 * age - 3.0 * spread - 2.0 * pebble;
		Check( outermost <= ceiling && outermost >= floor,
		       fmt( "t=%.2f s: front at %.3f m; sqrt(gh) t = %.3f m, allowed %.3f..%.3f", age, outermost, c0 * age,
		            floor, ceiling ) );
	}

	return Verdict();
}

//===========================================================================
// --banks
//===========================================================================
int runBanks( const Perturb& perturb )
{
	std::printf( "\n=== banks: walls hold the water's energy and pass no flow; open water lets it go\n" );

	auto energyAfter = [ & ]( Banks banks, double early, double late, double& wallFlux ) {
		Rig rig;
		double result = -1.0;
		if( !rig.Init( 256, 144 ) )
			return result;
		rig.Calm();
		rig.Set( PT_BANKS, static_cast< float >( banks ) );
		rig.Set( PT_POND_SIZE, pondParam( 0.5 ) );
		rig.Set( PT_DEPTH, depthParam( 0.2 ) );
		rig.Set( PT_VISCOSITY, viscosityParam( 1e-7 ) );
		rig.Set( PT_DETAIL, detailParam( 1024 ) );
		rig.Set( PT_PEBBLE_SIZE, pebbleParam( 0.03 ) );
		rig.Set( PT_PEBBLE_X, 0.3f );
		rig.Set( PT_PEBBLE_Y, 0.4f );
		rig.Set( PT_CAUSTICS, 0.0f );
		rig.Render( 1 );
		rig.Press( PT_DROP );

		//Potential energy in the frame, sum of eta^2 over its cells.
		auto energy = [ & ]() {
			const Floats s = rig.Surface();
			const Grid& g  = rig.plugin.CurrentGrid();
			double sum     = 0.0;
			for( int y = 0; y < g.ny / 2; ++y )
				for( int x = 0; x < g.nx / 2; ++x )
				{
					const double e = s[ ( static_cast< size_t >( y ) * g.nx + x ) * 4 ];
					sum += e * e;
				}
			return sum;
		};

		rig.Render( static_cast< int >( early * 60 ) );
		const double first = energy();
		rig.Render( static_cast< int >( ( late - early ) * 60 ) );
		const double second = energy();

		//No flow through a wall is the same statement as the water being a
		//mirror image of itself across it. On this grid the mirror of cell i
		//about x = 0 is cell Nx-1-i, and the same across y = 0; the domain's
		//periodicity makes that the mirror about x = W and y = H as well. In
		//L2, which is what the propagator conserves and what the transforms'
		//rounding is relative to.
		const Floats s = rig.Surface();
		const Grid& g  = rig.plugin.CurrentGrid();
		double norm = 0.0, offX = 0.0, offY = 0.0;
		for( int y = 0; y < g.ny; ++y )
			for( int x = 0; x < g.nx; ++x )
			{
				const double a  = s[ ( static_cast< size_t >( y ) * g.nx + x ) * 4 ];
				const double mx = s[ ( static_cast< size_t >( y ) * g.nx + ( g.nx - 1 - x ) ) * 4 ];
				const double my = s[ ( static_cast< size_t >( g.ny - 1 - y ) * g.nx + x ) * 4 ];
				norm += a * a;
				offX += ( a - mx ) * ( a - mx );
				offY += ( a - my ) * ( a - my );
			}
		wallFlux = std::sqrt( std::max( offX, offY ) / std::max( norm, 1e-300 ) );
		result   = second / std::max( first, 1e-300 );
		return result;
	};

	double openFlux = 0.0, wallFlux = 0.0;
	const double open  = energyAfter( perturb.banksSwapped ? Banks::Walls : Banks::Open, 1.0, 10.0, openFlux );
	const double walls = energyAfter( perturb.banksSwapped ? Banks::Open : Banks::Walls, 1.0, 10.0, wallFlux );

	//Nine seconds at 0.18 m/s or faster carries every ripple across a half
	//metre pond several times. Walls keep it (only viscosity at 1e-7 takes
	//any); open water must have absorbed nearly all of it.
	Check( open >= 0.0 && open < 0.02, fmt( "Open: frame energy at 10 s is %.4f of its value at 1 s (bound < 0.02)", open ) );
	Check( walls > 0.5, fmt( "Walls: frame energy at 10 s is %.3f of its value at 1 s (bound > 0.5)", walls ) );
	//A missing or misplaced image is an O(1) asymmetry. Rounding in 600
	//frames of transforms is about 1e-5 (measured: 2e-6 after one frame,
	//growing as a random walk). 1e-3 sits between the two.
	Check( wallFlux < 1e-3, fmt( "Walls: the water is its own mirror image across both walls to %.1e in L2 (bound "
	                             "1e-3)",
	                             wallFlux ) );

	return Verdict();
}

//===========================================================================
// Optics. A known surface -- a plane wave, eta = A sin( k . x ) -- loaded in,
// the water frozen, and the output read as numbers.
//===========================================================================
struct PlaneWave
{
	double amplitude, kx, ky;

	double eta( double x, double y ) const
	{
		return amplitude * std::sin( kx * x + ky * y );
	}
	double slopeX( double x, double y ) const
	{
		return amplitude * kx * std::cos( kx * x + ky * y );
	}
	double slopeY( double x, double y ) const
	{
		return amplitude * ky * std::cos( kx * x + ky * y );
	}
};

/// A pond at rest, with `wave` loaded as its surface. Speed 0 and nothing
/// falling, so the surface stays exactly as loaded.
bool frozenPond( Rig& rig, int width, int height, const Floats& picture, const PlaneWave& wave, double depth,
                 double pond )
{
	if( !rig.Init( width, height, &picture ) )
		return false;
	rig.Calm();
	rig.Set( PT_SPEED, 0.0f );
	rig.Set( PT_POND_SIZE, pondParam( pond ) );
	rig.Set( PT_DEPTH, depthParam( depth ) );
	rig.Set( PT_DETAIL, detailParam( 1024 ) );
	rig.Set( PT_CAUSTICS, 0.0f );
	rig.Set( PT_REFLECTION, 0.0f );
	rig.Set( PT_GLINT, 0.0f );
	if( !rig.Render( 1 ) )
		return false;

	const Grid g = rig.plugin.CurrentGrid();
	Floats state( static_cast< size_t >( g.nx ) * g.ny * 4 );
	for( int y = 0; y < g.ny; ++y )
		for( int x = 0; x < g.nx; ++x )
		{
			const double px = ( x + 0.5 ) * g.lx / g.nx, py = ( y + 0.5 ) * g.ly / g.ny;
			const size_t i  = ( static_cast< size_t >( y ) * g.nx + x ) * 4;
			state[ i + 0 ]  = static_cast< float >( wave.eta( px, py ) );
			state[ i + 1 ]  = 0.0f;
			state[ i + 2 ]  = static_cast< float >( wave.slopeX( px, py ) );
			state[ i + 3 ]  = static_cast< float >( wave.slopeY( px, py ) );
		}
	rig.plugin.LoadSurfaceForTest( state );
	return rig.Render( 1 );
}

/// A wave that fits the periodic domain exactly: `cycles` whole wavelengths
/// across 2W (or 2H), so the loaded surface has no seam.
PlaneWave fittedWave( const Grid& g, int cyclesX, int cyclesY, double amplitude )
{
	return PlaneWave { amplitude, 2.0 * kPi * cyclesX / g.lx, 2.0 * kPi * cyclesY / g.ly };
}

//===========================================================================
// --refraction
//===========================================================================
int runRefraction( const Perturb& perturb )
{
	std::printf( "\n=== refraction: the bed moves by Snell's law, at two rasters\n" );

	const double n     = perturb.indexOverride > 0.0 ? perturb.indexOverride : kWaterIndex;
	const double depth = 0.15, pond = 1.0;

	struct Raster
	{
		int w, h;
	};
	for( const Raster r : { Raster { 480, 270 }, Raster { 1280, 720 } } )
		for( int axis = 0; axis < 2; ++axis )
		{
			//First pass only to learn the grid; the wave is fitted to it.
			Grid g;
			{
				Rig probe;
				if( !probe.Init( 64, 36 ) )
					return 1;
				probe.Set( PT_POND_SIZE, pondParam( pond ) );
				probe.Set( PT_DETAIL, detailParam( 1024 ) );
				probe.Calm();
				probe.Render( 1 );
				g = probe.plugin.CurrentGrid();
			}

			//About 36 cells a wavelength, slopes up to 0.25.
			const int cycles     = axis == 0 ? g.nx / 36 : g.ny / 36;
			const PlaneWave wave = fittedWave( g, axis == 0 ? cycles : 0, axis == 1 ? cycles : 0, 0.0 );
			const double k       = axis == 0 ? wave.kx : wave.ky;
			PlaneWave loaded     = wave;
			loaded.amplitude     = 0.25 / k;

			Rig rig;
			if( !frozenPond( rig, r.w, r.h, coordinateCard( r.w, r.h ), loaded, depth, pond ) )
				return 1;
			const Floats out = rig.Output();
			const double W = rig.plugin.FrameWidthMetres(), H = rig.plugin.FrameHeightMetres();

			double worst = 0.0, biggest = 0.0, tolerance = 0.0;
			for( int y = r.h / 8; y < r.h * 7 / 8; y += 3 )
				for( int x = r.w / 8; x < r.w * 7 / 8; x += 3 )
				{
					const double px = ( x + 0.5 ) / r.w * W, py = ( y + 0.5 ) / r.h * H;
					const double sx = loaded.slopeX( px, py ), sy = loaded.slopeY( px, py );
					const double slope = std::hypot( sx, sy );

					//Snell, from scratch: incidence atan( slope ) from the
					//normal, refraction asin( sin / n ), and the ray leaves
					//the vertical by the difference, toward the downslope.
					const double incidence = std::atan( slope );
					const double refracted = std::asin( std::sin( incidence ) / n );
					const double lean      = std::tan( incidence - refracted );
					const double travel    = ( depth + loaded.eta( px, py ) );
					const double reach     = travel * lean;
					const double ex = slope > 0 ? reach * sx / slope : 0.0;
					const double ey = slope > 0 ? reach * sy / slope : 0.0;

					const size_t o  = ( static_cast< size_t >( y ) * r.w + x ) * 4;
					const double gx = out[ o + 0 ] * W - px;
					const double gy = out[ o + 1 ] * H - py;

					worst   = std::max( worst, std::hypot( gx - ex, gy - ey ) );
					biggest = std::max( biggest, std::hypot( ex, ey ) );

					//The surface is read between grid points by linear
					//interpolation: its slope is off by at most
					//(k dx)^2 / 8 of its amplitude, and the displacement
					//with it. Plus a thousandth of a pixel of float.
					const double cell = axis == 0 ? g.lx / g.nx : g.ly / g.ny;
					tolerance = std::max( tolerance, reach * ( k * cell ) * ( k * cell ) / 8.0 * 1.5 + 1e-3 * H / r.h );
				}

			Check( worst < tolerance && biggest > 0.005,
			       fmt( "%4dx%-4d wave along %s: displacement up to %.2f cm, worst error %.3f mm (bound %.3f mm)", r.w,
			            r.h, axis == 0 ? "x" : "y", 100.0 * biggest, 1000.0 * worst, 1000.0 * tolerance ) );
		}

	return Verdict();
}

//===========================================================================
// --fresnel
//===========================================================================
double fresnelReflectance( double cosIncident, double n )
{
	//Textbook, s and p averaged, air to water.
	const double ci = std::clamp( cosIncident, 0.0, 1.0 );
	const double si = std::sqrt( 1.0 - ci * ci );
	const double st = si / n;
	const double ct = std::sqrt( 1.0 - st * st );
	const double rs = ( ci - n * ct ) / ( ci + n * ct );
	const double rp = ( n * ci - ct ) / ( n * ci + ct );
	return 0.5 * ( rs * rs + rp * rp );
}

int runFresnel( const Perturb& perturb )
{
	std::printf( "\n=== fresnel: a black bed under a white sky reflects exactly R( theta )\n" );

	const double n = perturb.indexOverride > 0.0 ? perturb.indexOverride : kWaterIndex;
	std::printf( "  normal incidence: ((n-1)/(n+1))^2 = %.5f\n", std::pow( ( n - 1 ) / ( n + 1 ), 2.0 ) );

	for( int steep = 0; steep < 2; ++steep )
	{
		Grid g;
		{
			Rig probe;
			if( !probe.Init( 64, 36 ) )
				return 1;
			probe.Set( PT_POND_SIZE, pondParam( 1.0 ) );
			probe.Set( PT_DETAIL, detailParam( 1024 ) );
			probe.Calm();
			probe.Render( 1 );
			g = probe.plugin.CurrentGrid();
		}
		PlaneWave wave = fittedWave( g, g.nx / 40, g.ny / 60, 0.0 );
		const double k = std::hypot( wave.kx, wave.ky );
		//Still water, then slopes up to 1.2 -- 50 degrees, where R has risen
		//from 2% to about 6%.
		wave.amplitude = steep ? 1.2 / k : 0.0;

		Rig rig;
		if( !frozenPond( rig, 640, 360, flatCard( 640, 360, 0.0f ), wave, 0.2, 1.0 ) )
			return 1;
		rig.Set( PT_REFLECTION, 1.0f / 3.0f );
		rig.Set( PT_SKY_R, 1.0f );
		rig.Set( PT_SKY_G, 1.0f );
		rig.Set( PT_SKY_B, 1.0f );
		rig.Set( PT_GLINT, 0.0f );
		rig.Render( 1 );
		const Floats out = rig.Output();
		const double W = rig.plugin.FrameWidthMetres(), H = rig.plugin.FrameHeightMetres();

		double worst = 0.0, lo = 1.0, hi = 0.0;
		for( int y = 20; y < 340; y += 3 )
			for( int x = 20; x < 620; x += 3 )
			{
				const double px = ( x + 0.5 ) / 640 * W, py = ( y + 0.5 ) / 360 * H;
				const double sx = wave.slopeX( px, py ), sy = wave.slopeY( px, py );
				const double expected = fresnelReflectance( 1.0 / std::sqrt( 1.0 + sx * sx + sy * sy ), n );
				const double got      = out[ ( static_cast< size_t >( y ) * 640 + x ) * 4 ];
				worst                 = std::max( worst, std::fabs( got - expected ) );
				lo                    = std::min( lo, got );
				hi                    = std::max( hi, got );
			}
		//dR/dslope is at most about 0.1 over this range, and the slope is
		//interpolated to (k dx)^2/8 of 1.2 -- about 4e-4 of reflectance at
		//worst. 1e-3 covers it with room; a wrong index moves R by 1e-2.
		Check( worst < 1e-3, fmt( "%s: R from %.4f to %.4f, worst error %.2e (bound 1e-3)",
		                           steep ? "slopes to 1.2" : "still water", lo, hi, worst ) );
	}

	return Verdict();
}

//===========================================================================
// --caustics
//===========================================================================
int runCaustics( const Perturb& perturb )
{
	std::printf( "\n=== caustics: light is conserved, and focused by the Jacobian of the landing map\n" );

	//1. Still water: every pixel receives exactly its own light.
	{
		Rig rig;
		if( !rig.Init( 480, 270 ) )
			return 1;
		rig.Calm();
		rig.Set( PT_SUN_SIZE, 0.0f );
		rig.Render( 5 );
		const Floats c = rig.CausticMap();
		double worst   = 0.0;
		size_t at      = 0;
		for( size_t i = 0; i < c.size(); ++i )
			if( std::fabs( c[ i ] - 1.0 ) > worst )
			{
				worst = std::fabs( c[ i ] - 1.0 );
				at    = i;
			}
		//Area over itself, divided the way a GPU divides: a unit or two in
		//the last place.
		Check( !c.empty() && worst < 1e-6, fmt( "still water: every pixel's light is 1 to within %.1e (bound 1e-6)", worst ) );
	}

	//2. Heavy rain, the sun low and sharp: the net is violent, and the light
	//in it still adds up. Only two things can make the frame's total differ
	//from 1: light crossing the frame's edge (balanced, in and out) and the
	//luck of which pixel centres the triangles happen to cover (unbiased,
	//because the mesh is jittered). Measured within 0.1% at every raster and
	//Detail tried; the bound is 0.5%. Unjittered, at 1.5 px a cell, it was
	//1.8% short.
	for( int blurred = 0; blurred < 2; ++blurred )
	{
		Rig rig;
		if( !rig.Init( 640, 360 ) )
			return 1;
		rig.Calm();
		rig.Set( PT_RAIN, 0.7f );
		rig.Set( PT_SPLASH, 0.8f );
		rig.Set( PT_SUN_ELEVATION, 0.5f );
		rig.Set( PT_SUN_SIZE, blurred ? 0.6f : 0.0f );
		rig.Render( 150 );
		const Floats c = rig.CausticMap();
		double sum = 0.0, peak = 0.0;
		for( float v : c )
		{
			sum += v;
			peak = std::max( peak, static_cast< double >( v ) );
		}
		const double mean = sum / std::max< size_t >( c.size(), 1 );
		Check( std::fabs( mean - 1.0 ) < 0.005 && peak > 2.0,
		       fmt( "rain, sun %s: mean light %.4f (bound 1 +- 0.005), brightest %.1fx", blurred ? "hazy" : "sharp",
		            mean, peak ) );
	}

	//3. A known surface: a plane wave, sun overhead, no folds. The bed point
	//x' = x + (h + eta) tan( lean( slope ) ) receives 1 / (dx'/dx) of light.
	{
		Grid g;
		{
			Rig probe;
			if( !probe.Init( 64, 36 ) )
				return 1;
			probe.Set( PT_POND_SIZE, pondParam( 1.0 ) );
			probe.Set( PT_DETAIL, detailParam( 1024 ) );
			probe.Calm();
			probe.Render( 1 );
			g = probe.plugin.CurrentGrid();
		}
		PlaneWave wave   = fittedWave( g, g.nx / 48, 0, 0.0 );
		const double depth = 0.1;
		//Weak enough not to fold: dx'/dx stays between about 0.6 and 1.4.
		wave.amplitude = 0.4 / ( depth * ( 1.0 - 1.0 / kWaterIndex ) * wave.kx * wave.kx );

		Rig rig;
		if( !frozenPond( rig, 1280, 720, buildCard( 1280, 720 ), wave, depth, 1.0 ) )
			return 1;
		rig.Set( PT_CAUSTICS, 0.5f );
		rig.Set( PT_SUN_ELEVATION, 1.0f );
		rig.Set( PT_SUN_SIZE, 0.0f );
		rig.Render( 1 );
		const Floats c = rig.CausticMap();
		const double W = rig.plugin.FrameWidthMetres();

		//The landing map, from scratch, and its derivative numerically.
		const double h = depth * perturb.focusDepth;
		auto landing   = [ & ]( double x ) {
            const double s         = wave.slopeX( x, 0.0 );
            const double incidence = std::atan( std::fabs( s ) );
            const double refracted = std::asin( std::sin( incidence ) / kWaterIndex );
            const double lean      = std::tan( incidence - refracted ) * ( s > 0 ? 1.0 : -1.0 );
            return x + ( h + wave.eta( x, 0.0 ) ) * lean;
		};

		//Invert x' -> x by bisection on the monotone map, then 1 / derivative.
		auto expectedAt = [ & ]( double xb ) {
			double lo = xb - 0.2, hi = xb + 0.2;
			for( int i = 0; i < 80; ++i )
			{
				const double mid = 0.5 * ( lo + hi );
				( landing( mid ) < xb ? lo : hi ) = mid;
			}
			const double x = 0.5 * ( lo + hi ), e = 1e-6;
			return 2.0 * e / ( landing( x + e ) - landing( x - e ) );
		};

		const int row  = 360;
		//One mesh cell, in metres: the caustic mesh is 1.2 frames across.
		const double cell = 1.2 * W / std::max( rig.plugin.MeshColumns(), 1 );
		double worst = 0.0, lo = 10.0, hi = 0.0, bound = 0.0;
		for( int x = 128; x < 1152; ++x )
		{
			const double xb       = ( x + 0.5 ) / 1280.0 * W;
			const double expected = expectedAt( xb );
			const double got      = c[ static_cast< size_t >( row ) * 1280 + x ];

			//Each mesh triangle carries its own AVERAGE focus, constant
			//across it, and a triangle reaches up to 0.8 of a mesh cell from
			//any point in it once its corners are jittered: so the check
			//allows how far the exact value moves within 0.8 of a cell of
			//the pixel. Derived from the expectation, not fitted to the result.
			const double spread = std::max( std::fabs( expectedAt( xb + 0.8 * cell ) - expected ),
			                                std::fabs( expectedAt( xb - 0.8 * cell ) - expected ) );
			const double allow  = spread + 0.01;
			if( std::fabs( got - expected ) > allow )
				worst = std::max( worst, std::fabs( got - expected ) - allow );
			bound = std::max( bound, allow );
			lo    = std::min( lo, got );
			hi    = std::max( hi, got );
		}
		Check( worst == 0.0 && hi > 1.25 && lo < 0.8,
		       fmt( "plane wave: light from %.3f to %.3f, every pixel within 0.8 of a mesh cell's variation + 0.01 of "
		            "1/(dx'/dx) (largest allowance %.3f, worst excess %.4f)",
		            lo, hi, bound, worst ) );
	}

	return Verdict();
}

//===========================================================================
// --still
//===========================================================================
int runStill( const Perturb& perturb )
{
	std::printf( "\n=== still: flat water bends nothing, and reflects exactly R0 of the sky\n" );

	for( int reflecting = 0; reflecting < 2; ++reflecting )
	{
		const int w = 640, h = 360;
		const Floats card = buildCard( w, h );
		Rig rig;
		if( !rig.Init( w, h, &card ) )
			return 1;
		rig.Calm();
		rig.Set( PT_REFLECTION, reflecting ? 1.0f / 3.0f : 0.0f );
		//Ten seconds of water with nothing in it: the whole chain runs every
		//frame, and a round trip that did not return zero would show here.
		rig.Render( 600 );
		const Floats out = rig.Output();

		const double r0 = std::pow( ( kWaterIndex - 1.0 ) / ( kWaterIndex + 1.0 ), 2.0 );
		const float sky[ 3 ] = { rig.plugin.GetFloatParameter( PT_SKY_R ), rig.plugin.GetFloatParameter( PT_SKY_G ),
			                     rig.plugin.GetFloatParameter( PT_SKY_B ) };
		double worst = 0.0;
		for( size_t i = 0; i < out.size(); i += 4 )
			for( int c = 0; c < 3; ++c )
			{
				const double expected =
					( reflecting ? ( 1.0 - r0 ) * card[ i + c ] + r0 * sky[ c ] : card[ i + c ] ) + perturb.stillSlack;
				worst = std::max( worst, std::fabs( out[ i + c ] - expected ) );
			}
		Check( worst < 1e-5, fmt( "%s: output = %s to within %.1e (bound 1e-5)",
		                          reflecting ? "Reflection 1" : "Reflection 0",
		                          reflecting ? "(1 - R0) picture + R0 sky, R0 = 0.02037" : "the picture", worst ) );
	}

	return Verdict();
}

//===========================================================================
// --skim
//===========================================================================
int runSkim( const Perturb& perturb )
{
	std::printf( "\n=== skim: equal energy per bounce, so the hops shorten linearly, then the stone sinks\n" );

	Rig rig;
	if( !rig.Init( 320, 180 ) )
		return 1;
	rig.Calm();
	rig.Set( PT_DETAIL, detailParam( 256 ) );
	rig.Set( PT_CAUSTICS, 0.0f );
	rig.Set( PT_HEADING, 30.0f / 360.0f );
	rig.Set( PT_PEBBLE_X, 0.5f );
	rig.Set( PT_PEBBLE_Y, 0.5f );
	rig.plugin.KeepInjectedLog( true );
	rig.Render( 1 );
	rig.Press( PT_SKIM );
	rig.Render( 300 );

	const std::vector< Impact >& log = rig.plugin.InjectedForTest();
	const int bounces = BouncesFromParam( rig.plugin.GetFloatParameter( PT_BOUNCES ) );
	const double v0   = ThrowSpeedFromParam( rig.plugin.GetFloatParameter( PT_THROW_SPEED ) );
	const double beta = SkimAngleFromParam( rig.plugin.GetFloatParameter( PT_SKIM_ANGLE ) ) * kPi / 180.0;
	const double d0   = 2.0 * v0 * v0 * std::sin( beta ) * std::cos( beta ) / kGravity;

	Check( static_cast< int >( log.size() ) == bounces + 1,
	       fmt( "%zu impacts: %d touches and the sink (expected %d)", log.size(), bounces, bounces + 1 ) );

	double worstHop = 0.0, worstTime = 0.0, worstHeading = 0.0;
	for( size_t i = 0; i + 1 < log.size(); ++i )
	{
		const double n    = static_cast< double >( i );
		const double hop  = std::hypot( log[ i + 1 ].x - log[ i ].x, log[ i + 1 ].y - log[ i ].y );
		const double left = perturb.skimLaw > 0.0 ? std::pow( 0.8, n ) : 1.0 - n / bounces;
		worstHop          = std::max( worstHop, std::fabs( hop - d0 * left ) );

		const double flight = log[ i + 1 ].time - log[ i ].time;
		worstTime = std::max( worstTime, std::fabs( flight - 2.0 * v0 * std::sqrt( left ) * std::sin( beta ) / kGravity ) );

		const double heading = std::atan2( log[ i + 1 ].y - log[ i ].y, log[ i + 1 ].x - log[ i ].x );
		worstHeading         = std::max( worstHeading, std::fabs( heading - 30.0 * kPi / 180.0 ) );
	}
	//The positions are stored as floats in metres: a few 1e-7 m.
	Check( worstHop < 1e-5, fmt( "hop n = %.3f m x (1 - n/%d) to within %.1e m (bound 1e-5)", d0, bounces, worstHop ) );
	Check( worstTime < 1e-9, fmt( "flight n = 2 V0 sqrt(1 - n/N) sin b / g to within %.1e s", worstTime ) );
	Check( worstHeading < 1e-4, fmt( "every hop along the 30 degree heading, to %.1e rad", worstHeading ) );

	const bool sinkBiggest = !log.empty() && log.back().radius > log.front().radius;
	Check( sinkBiggest, "the last impact is the sink: the whole stone, larger than a touch" );

	return Verdict();
}

//===========================================================================
// --rain
//===========================================================================
int runRain( const Perturb& perturb )
{
	std::printf( "\n=== rain: a Poisson process at the rate asked for, in water-time\n" );

	for( float speed : { 1.0f, 0.0f } )
	{
		Rig rig;
		if( !rig.Init( 64, 36 ) )
			return 1;
		rig.Calm();
		rig.Set( PT_DETAIL, detailParam( 256 ) );
		rig.Set( PT_CAUSTICS, 0.0f );
		rig.Set( PT_RAIN, 0.4f );
		rig.Set( PT_SPEED, ParamFromSpeed( speed ) );
		rig.plugin.KeepInjectedLog( true );
		rig.Render( 60 * 150 );

		const double rate     = RainFromParam( 0.4f ) * perturb.rainFactor * speed;
		const double seconds  = rig.plugin.SimTime();
		const double expected = rate * seconds;
		const double got      = static_cast< double >( rig.plugin.InjectedForTest().size() );

		//A Poisson count's standard deviation is its square root. Four of
		//them is a false alarm one run in sixteen thousand.
		const double band = 4.0 * std::sqrt( std::max( expected, 1.0 ) );
		Check( std::fabs( got - expected ) <= band,
		       fmt( "speed %.0f: %.0f drops in %.1f s of water, expected %.1f +- %.1f", speed, got, seconds, expected,
		            band ) );
	}

	return Verdict();
}

//===========================================================================
// --audio
//===========================================================================
int runAudio( const Perturb& perturb )
{
	std::printf( "\n=== audio: silence drops nothing; a beat every half second drops a pebble a beat\n" );

	for( int beat = 0; beat < 2; ++beat )
	{
		Rig rig;
		if( !rig.Init( 64, 36 ) )
			return 1;
		rig.Calm();
		rig.Set( PT_DETAIL, detailParam( 256 ) );
		rig.Set( PT_CAUSTICS, 0.0f );
		rig.Set( PT_AUDIO_PEBBLES, 0.6f );
		rig.feed = beat ? AudioFeed::Pulses : AudioFeed::Silence;
		rig.plugin.KeepInjectedLog( true );
		rig.Render( 60 * 10 );

		const size_t got      = rig.plugin.InjectedForTest().size();
		const size_t expected = perturb.audioDeaf ? 0 : ( beat ? 19 : 0 );
		//Twenty hits in ten seconds, at 0, 0.5 .. 9.5 s. The one on frame 0
		//cannot be an onset: there is no earlier frame for it to have risen
		//from, and the analyser is primed there precisely so that it does not
		//read the whole spectrum as a rise from silence (rosette's finding).
		//So nineteen, exactly: each of the others lands on its own frame.
		Check( got == expected, fmt( "%s: %zu pebbles in 10 s (expected %zu)", beat ? "a beat every 0.5 s" : "silence",
		                             got, expected ) );
	}

	return Verdict();
}

//===========================================================================
// --negative
//===========================================================================
int runNegative()
{
	struct Case
	{
		const char* name;
		int ( *check )( const Perturb& );
		Perturb perturb;
		const char* what;
	};

	std::vector< Case > cases;
	{
		Perturb p;
		p.derivativeSign = -1.0;
		cases.push_back( { "fft", runFFT, p, "expect the derivative with the wrong sign of i" } );
	}
	{
		Perturb p;
		p.omegaFactor = 1.01;
		cases.push_back( { "modes", runModes, p, "expect every frequency 1% high" } );
	}
	{
		Perturb p;
		p.gravityFactor = 1.01;
		cases.push_back( { "gravity", runGravity, p, "expect g 1% stronger" } );
	}
	{
		Perturb p;
		p.tensionFactor = 1.05;
		cases.push_back( { "quiet", runQuiet, p, "expect 5% more surface tension" } );
	}
	{
		Perturb p;
		p.depthFactor = 0.5;
		cases.push_back( { "shallow", runShallow, p, "expect the water half as deep" } );
	}
	{
		Perturb p;
		p.banksSwapped = true;
		cases.push_back( { "banks", runBanks, p, "expect Open to hold and Walls to lose" } );
	}
	{
		Perturb p;
		p.indexOverride = 1.25;
		cases.push_back( { "refraction", runRefraction, p, "expect n = 1.25 instead of 1.333" } );
	}
	{
		Perturb p;
		p.indexOverride = 1.5;
		cases.push_back( { "fresnel", runFresnel, p, "expect glass, n = 1.5" } );
	}
	{
		Perturb p;
		p.focusDepth = 1.25;
		cases.push_back( { "caustics", runCaustics, p, "expect the bed 25% deeper" } );
	}
	{
		Perturb p;
		p.stillSlack = 1.0 / 255.0;
		cases.push_back( { "still", runStill, p, "expect one 8-bit code value more" } );
	}
	{
		Perturb p;
		p.skimLaw = 1.0;
		cases.push_back( { "skim", runSkim, p, "expect hops that shrink geometrically" } );
	}
	{
		Perturb p;
		p.rainFactor = 1.25;
		cases.push_back( { "rain", runRain, p, "expect 25% more rain" } );
	}
	{
		Perturb p;
		p.audioDeaf = true;
		cases.push_back( { "audio", runAudio, p, "expect the beat to drop nothing" } );
	}

	int unfalsifiable = 0;
	for( const Case& c : cases )
	{
		std::printf( "\n=== negative control: %s -- %s\n", c.name, c.what );
		const int before = g_failures;
		g_failures       = 0;
		c.check( c.perturb );
		const int observed = g_failures;
		g_failures         = before;

		if( observed > 0 )
			std::printf( "  ok    %s failed %d check%s, as it must\n", c.name, observed, observed == 1 ? "" : "s" );
		else
		{
			std::printf( "  FAIL  %s PASSED against a wrong model -- it cannot fail, so it is not a check\n", c.name );
			++unfalsifiable;
		}
	}

	std::printf( "\nnegative controls: %zu perturbations, %d of them undetected\n", cases.size(), unfalsifiable );
	std::printf( "\n  %s\n", unfalsifiable == 0 ? "PASS" : "FAIL" );
	return unfalsifiable == 0 ? 0 : 1;
}

//===========================================================================
// --bench
//===========================================================================
int runBench()
{
	struct Size
	{
		int w, h;
		const char* name;
		int cells;
	};
	const Size sizes[] = { { 1280, 720, "720p", 1024 },   { 1920, 1080, "1080p", 1024 }, { 3840, 2160, "4K", 1024 },
		                   { 1920, 1080, "1080p", 512 },  { 1920, 1080, "1080p", 2048 } };

	for( const Size& size : sizes )
	{
		Rig rig;
		if( !rig.Init( size.w, size.h ) )
			return 1;
		rig.Set( PT_DETAIL, detailParam( size.cells ) );
		//Rain on, so every frame has craters to inject and the caustics have
		//a net to draw: the busy case, not the idle one.
		rig.Set( PT_RAIN, 0.5f );

		//Warm up: the first frames allocate and compile pipeline state.
		if( !rig.Render( 20 ) )
			return 1;
		glFinish();

		constexpr int kTimed = 60;
		const auto start     = std::chrono::steady_clock::now();
		if( !rig.Render( kTimed ) )
			return 1;
		glFinish();
		const auto end = std::chrono::steady_clock::now();

		const double ms =
			std::chrono::duration< double, std::milli >( end - start ).count() / static_cast< double >( kTimed );
		const Grid& g = rig.plugin.CurrentGrid();
		std::printf( "  %-6s Detail %-4d  %6.2f ms/frame  (%4.1f%% of 60 fps; grid %dx%d, mesh %dx%d)\n", size.name,
		             size.cells, ms, 100.0 * ms / ( 1000.0 / 60.0 ), g.nx, g.ny, rig.plugin.MeshColumns(),
		             rig.plugin.MeshRows() );
	}
	return 0;
}

//---------------------------------------------------------------------------
// Parameters by display name, so the automation reads as English.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	float value;
	std::string kind;
};

const char* kindName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_RED: return "red";
	case FF_TYPE_GREEN: return "green";
	case FF_TYPE_BLUE: return "blue";
	case FF_TYPE_XPOS: return "xpos";
	case FF_TYPE_YPOS: return "ypos";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_TEXT: return "text";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( MillpondPlugin& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		list.push_back( NamedParameter { name ? name : "?", i, plugin.GetFloatParameter( i ),
		                                 kindName( plugin.GetParamType( i ) ) } );
	}
	return list;
}

bool applySetting( MillpondPlugin& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}

	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );

	for( const NamedParameter& parameter : listParameters( plugin ) )
	{
		if( parameter.name != name )
			continue;
		plugin.SetFloatParameter( parameter.index, std::strtof( value.c_str(), nullptr ) );
		return true;
	}

	error = "no parameter called '" + name + "'";
	return false;
}
//---------------------------------------------------------------------------
// --script: one 'frame Parameter Name value' per line. Same format as the
// rest of the fleet, so one filming script drives any of them.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;

		//The name is everything up to the last token: parameters have spaces
		//in them ("Pebble Size") and the value never does.
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 0; i + 1 < track.size(); ++i )
	{
		const auto& a = track[ i ];
		const auto& b = track[ i + 1 ];
		if( frame >= a.first && frame <= b.first )
		{
			if( b.first == a.first )
				return b.second;
			const float t = static_cast< float >( frame - a.first ) / static_cast< float >( b.first - a.first );
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

/// --pipe and --film. Raw RGBA, top row first, one frame at a time, on the
/// synthetic 60 fps clock -- so a stall in ffmpeg cannot show up as the water
/// speeding up afterwards.
int runPipe( int width, int height, const std::string& scriptPath, int filmFrames, bool beat,
             const std::vector< std::string >& settings )
{
	Rig rig;
	if( !rig.Init( width, height ) )
		return 1;
	if( beat )
		rig.feed = AudioFeed::Pulses;

	for( const std::string& setting : settings )
	{
		std::string error;
		if( !applySetting( rig.plugin, setting, error ) )
		{
			std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
			return 2;
		}
	}

	//Resolve the script's names once, and refuse a name that is not a
	//parameter: a misspelling that silently did nothing would film a take
	//that looks deliberate and is wrong.
	std::map< unsigned int, Track > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
		if( !error.empty() )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
		const std::vector< NamedParameter > known = listParameters( rig.plugin );
		for( const auto& entry : tracks )
		{
			bool found = false;
			for( const NamedParameter& parameter : known )
				if( parameter.name == entry.first )
				{
					automation[ parameter.index ] = entry.second;
					found                         = true;
				}
			if( !found )
			{
				std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
				return 2;
			}
		}
	}

	const Floats card = buildCard( width, height );
	std::vector< unsigned char > in( static_cast< size_t >( width ) * height * 4 );
	Floats picture( in.size() );

	for( int index = 0; filmFrames < 0 || index < filmFrames; ++index )
	{
		if( filmFrames < 0 )
		{
			size_t filled = 0;
			while( filled < in.size() )
			{
				const ssize_t got = read( STDIN_FILENO, in.data() + filled, in.size() - filled );
				if( got <= 0 )
					break;
				filled += static_cast< size_t >( got );
			}
			if( filled < in.size() )
				break;

			//Top row first on the wire; bottom row first in GL.
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < width * 4; ++x )
					picture[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] =
						in[ static_cast< size_t >( y ) * width * 4 + x ] / 255.0f;
			rig.Upload( picture );
		}
		else if( index == 0 )
			rig.Upload( card );

		for( const auto& track : automation )
			rig.plugin.SetFloatParameter( track.first, valueAt( track.second, index ) );

		if( !rig.Render( 1 ) )
			return 1;

		const Floats out = rig.Output();
		std::vector< unsigned char > bytes( in.size() );
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width * 4; ++x )
				bytes[ static_cast< size_t >( y ) * width * 4 + x ] = static_cast< unsigned char >( std::lround(
					std::clamp( out[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ], 0.0f, 1.0f ) * 255.0f ) );

		size_t written = 0;
		while( written < bytes.size() )
		{
			const ssize_t put = write( STDOUT_FILENO, bytes.data() + written, bytes.size() - written );
			if( put <= 0 )
				return 1;
			written += static_cast< size_t >( put );
		}
	}
	return 0;
}
} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outPath = "/tmp/millpond.png";
	std::string cardPath;
	std::vector< std::string > settings;
	int width  = 1280;
	int height = 720;
	int frames = 120;
	std::vector< int > dropFrames, skimFrames;
	bool beat = false;
	std::string mode;
	std::string scriptPath;
	int filmFrames = -1;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;

		if( argument == "--help" || argument == "-h" )
		{
			std::printf(
				"mptest -- render Millpond offline and measure its water\n\n"
				"  --out PATH        render the card and write it here\n"
				"  --card PATH       write the card itself\n"
				"  --size WxH        render size (default 1280x720)\n"
				"  --frames N        frames of 60 fps water before reading back (default 120)\n"
				"  --drop N          press Drop on frame N. Repeatable.\n"
				"  --skim N          press Skim on frame N. Repeatable.\n"
				"  --beat            feed a beat every half second into the Audio buffer\n"
				"  --set \"Name=V\"    set a parameter by its display name. Repeatable.\n"
				"  --list            print every parameter and its default, then exit\n"
				"  --pipe            raw RGBA frames on stdin, raw RGBA frames on stdout\n"
				"  --film N          N frames of the card, raw RGBA frames on stdout\n"
				"  --script PATH     parameter cues for --pipe/--film: 'frame Name value'\n\n"
				"  --fft --modes --gravity --quiet --shallow --banks --refraction --fresnel\n"
				"  --caustics --still --skim-check --rain --audio --negative --bench\n" );
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--card" && hasNext )
			cardPath = argv[ ++i ];
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--drop" && hasNext )
			dropFrames.push_back( std::atoi( argv[ ++i ] ) );
		else if( argument == "--skim" && hasNext )
			skimFrames.push_back( std::atoi( argv[ ++i ] ) );
		else if( argument == "--beat" )
			beat = true;
		else if( argument == "--pipe" )
			mode = "pipe";
		else if( argument == "--film" && hasNext )
		{
			mode       = "pipe";
			filmFrames = std::max( 1, std::atoi( argv[ ++i ] ) );
		}
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--width" && hasNext )
			width = std::atoi( argv[ ++i ] );
		else if( argument == "--height" && hasNext )
			height = std::atoi( argv[ ++i ] );
		else if( argument == "--list" )
			mode = "list";
		else if( argument == "--skim-check" )
			mode = "skim";
		else if( argument == "--fft" || argument == "--modes" || argument == "--gravity" || argument == "--quiet"
		         || argument == "--shallow" || argument == "--banks" || argument == "--refraction"
		         || argument == "--fresnel" || argument == "--caustics" || argument == "--still"
		         || argument == "--rain" || argument == "--audio" || argument == "--negative" || argument == "--bench" )
			mode = argument.substr( 2 );
		else if( argument == "--size" && hasNext )
		{
			const std::string value = argv[ ++i ];
			const size_t cross      = value.find( 'x' );
			if( cross != std::string::npos )
			{
				width  = std::atoi( value.substr( 0, cross ).c_str() );
				height = std::atoi( value.substr( cross + 1 ).c_str() );
			}
		}
		else
		{
			std::fprintf( stderr, "unknown argument '%s' (try --help)\n", argument.c_str() );
			return 2;
		}
	}

	//--list needs no GL at all: the parameters are declared in the constructor.
	if( mode == "list" )
	{
		MillpondPlugin plugin;
		std::printf( "%-3s %-18s %-9s %s\n", "id", "name", "kind", "default" );
		for( const NamedParameter& parameter : listParameters( plugin ) )
			std::printf( "%-3u %-18s %-9s %.4f\n", parameter.index, parameter.name.c_str(), parameter.kind.c_str(),
			             parameter.value );
		return 0;
	}

	if( !cardPath.empty() )
	{
		if( !writePng( cardPath, width, height, buildCard( width, height ) ) )
		{
			std::fprintf( stderr, "could not write %s\n", cardPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s -- the card, %dx%d\n", cardPath.c_str(), width, height );
		return 0;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	int result = 0;
	const Perturb none;
	const std::pair< const char*, int ( * )( const Perturb& ) > checks[] = {
		{ "fft", runFFT },         { "modes", runModes },       { "gravity", runGravity },
		{ "quiet", runQuiet },     { "shallow", runShallow },   { "banks", runBanks },
		{ "refraction", runRefraction }, { "fresnel", runFresnel }, { "caustics", runCaustics },
		{ "still", runStill },     { "skim", runSkim },         { "rain", runRain },
		{ "audio", runAudio },
	};

	bool ran = false;
	for( const auto& check : checks )
		if( mode == check.first )
		{
			result = check.second( none );
			ran    = true;
		}

	if( !ran && mode == "pipe" )
		result = runPipe( width, height, scriptPath, filmFrames, beat, settings );
	else if( !ran && mode == "negative" )
		result = runNegative();
	else if( !ran && mode == "bench" )
		result = runBench();
	else if( !ran )
	{
		Rig rig;
		if( !rig.Init( width, height ) )
			result = 1;
		else
		{
			for( const std::string& setting : settings )
			{
				std::string error;
				if( !applySetting( rig.plugin, setting, error ) )
				{
					std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
					return 2;
				}
			}
			if( beat )
				rig.feed = AudioFeed::Pulses;

			for( int f = 0; f < std::max( frames, 1 ) && result == 0; ++f )
			{
				if( std::find( dropFrames.begin(), dropFrames.end(), f ) != dropFrames.end() )
					rig.Press( PT_DROP );
				if( std::find( skimFrames.begin(), skimFrames.end(), f ) != skimFrames.end() )
					rig.Press( PT_SKIM );
				if( !rig.Render( 1 ) )
					result = 1;
			}

			if( result == 0 )
			{
				if( writePng( outPath, width, height, rig.Output() ) )
					std::printf( "wrote %s -- %dx%d, %d frames (%.2f s of water)\n", outPath.c_str(), width, height,
					             frames, rig.plugin.SimTime() );
				else
				{
					std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
					result = 1;
				}
			}
		}
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
