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
        --state        the GL state the host hands over is the state it gets back
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
#include <csignal>
#include <cstdlib>
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

	// Faraday, for --faraday-negative.
	double mathieuForcing  = 1.0;  ///< --mathieu expects this times the shake the plugin used
	double onsetDamping    = 1.0;  ///< --onset expects the tongue of this times the damping
	bool halfHarmonic      = false;///< --half expects the response at f, not f/2
	double wavelengthOmega = 1.0;  ///< --wavelength expects the lattice of this times the drive
	bool patternShift      = false;///< --pattern scores each run as the next pattern
	bool strobeAsClock     = false;///< --strobe expects Slow Motion to be a slower clock
	bool floatClock        = false;///< --clock: the plugin takes its phase from a float clock
	bool resizeStill       = false;///< --resize presses Still on the resize frame
	bool offShake          = false;///< --faraday-off sets Shake a hair above 0
	int rateShift          = 0;    ///< --rate compares against the 60 fps water this many frames early
	bool stateLeak         = false;///< --state-shaken leaves a texture bound on unit 2 after the frame
	double seedBand        = 1.0;  ///< --seed expects the band around this times k_c
	int seedMirror         = 1;    ///< --seed expects Walls' mirror at N - seedMirror - i (1 is the half cell)
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
bool g_software = false;

CGLContextObj createContext()
{
	//MPTEST_RENDERER=software asks for Apple's software renderer by id, as
	//schlieren's sztest does: the second rasteriser every Faraday check is run
	//on (honeydew: it gives different floats, so a cross-context bit-exact
	//comparison is GPU-only). No fallback: a run that asked for it gets it.
	const char* renderer = std::getenv( "MPTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		const CGLPixelFormatAttribute generic[] = {
			kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
			kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
			kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
			kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
			static_cast< CGLPixelFormatAttribute >( 0 )
		};
		CGLPixelFormatObj format = nullptr;
		GLint formatCount        = 0;
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		CGLContextObj context = nullptr;
		const CGLError error  = CGLCreateContext( format, nullptr, &context );
		CGLDestroyPixelFormat( format );
		if( error != kCGLNoError )
			return nullptr;
		CGLSetCurrentContext( context );
		g_software = true;
		std::fprintf( stderr, "mptest: MPTEST_RENDERER=software, Apple's software renderer\n" );
		return context;
	}

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

/// The host's texture can be bigger than its picture (HardwareWidth > Width),
/// with the picture in the corner and MaxUV saying how much of it is real.
/// A padded copy of `picture`: magenta everywhere outside it.
GLuint paddedTexture( const Floats& picture, int width, int height, int hw, int hh )
{
	Floats padded( static_cast< size_t >( hw ) * hh * 4 );
	for( int y = 0; y < hh; ++y )
		for( int x = 0; x < hw; ++x )
		{
			float* o = &padded[ ( static_cast< size_t >( y ) * hw + x ) * 4 ];
			if( x < width && y < height )
				std::memcpy( o, &picture[ ( static_cast< size_t >( y ) * width + x ) * 4 ], 4 * sizeof( float ) );
			else
			{
				o[ 0 ] = 1.0f;
				o[ 1 ] = 0.0f;
				o[ 2 ] = 1.0f;
				o[ 3 ] = 1.0f;
			}
		}
	return makeTexture( hw, hh, padded.data() );
}

//---------------------------------------------------------------------------
// The audio the harness feeds, as rosette's harness feeds it: written into
// the Audio buffer's elements the way the host writes them.
//---------------------------------------------------------------------------
enum class AudioFeed
{
	Silence,
	Pulses,///< a bass-heavy spectrum with a hit every half second
	Drone, ///< a steady bass: the low bins at 1, the rest at 0.05, every frame the same
	Kick   ///< a kick in the lowest two bins every half second, over a steady pad in the rest
};

void feedAudio( MillpondPlugin& plugin, double seconds, AudioFeed feed )
{
	if( feed == AudioFeed::Drone )
	{
		for( int bin = 0; bin < audio::kBins; ++bin )
			plugin.SetParamElementValue( PT_AUDIO, static_cast< unsigned int >( bin ), bin < 4 ? 1.0f : 0.05f );
		return;
	}
	if( feed == AudioFeed::Kick )
	{
		//What Bass Bins is for: one bin and the kick is all there is; sixteen
		//and the pad's steady level dominates, so the bass barely moves.
		const float kick = static_cast< float >( std::exp( -std::fmod( seconds, 0.5 ) / 0.06 ) );
		for( int bin = 0; bin < audio::kBins; ++bin )
			plugin.SetParamElementValue( PT_AUDIO, static_cast< unsigned int >( bin ), bin < 2 ? kick : 0.5f );
		return;
	}
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

	/// Faraday's --clock: a host clock this far in, seconds.
	double clockOffset = 0.0;
	/// Faraday's --strobe: the host time of each frame, when given.
	const std::vector< double >* clock = nullptr;

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

	/// A new raster mid-run, as a host resizing the composition: a new input
	/// and output at the new size, and the same plugin.
	bool Resize( int w, int h )
	{
		glDeleteFramebuffers( 1, &outputFBO );
		glDeleteTextures( 1, &outputTexture );
		glDeleteTextures( 1, &sourceTexture );
		width  = w;
		height = h;
		const Floats card = buildCard( width, height );
		sourceTexture     = makeTexture( width, height, card.data() );
		outputTexture     = makeTexture( width, height, nullptr );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
			return false;
		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		process.HostFBO                                 = outputFBO;
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
			const double seconds = clock && static_cast< size_t >( frame ) < clock->size()
			                           ? ( *clock )[ static_cast< size_t >( frame ) ]
			                           : clockOffset + static_cast< double >( frame ) / fps;
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

	auto energyAfter = [ & ]( Banks banks, double early, double late, double& wallFlux, double pond = 0.5,
	                          double depth = 0.2, double pebble = 0.03, int cells = 1024 ) {
		Rig rig;
		double result = -1.0;
		if( !rig.Init( 256, 144 ) )
			return result;
		rig.Calm();
		rig.Set( PT_BANKS, static_cast< float >( banks ) );
		rig.Set( PT_POND_SIZE, pondParam( pond ) );
		rig.Set( PT_DEPTH, depthParam( depth ) );
		rig.Set( PT_VISCOSITY, viscosityParam( 1e-7 ) );
		rig.Set( PT_DETAIL, detailParam( cells ) );
		rig.Set( PT_PEBBLE_SIZE, pebbleParam( pebble ) );
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

	//The hard case for a sponge: a 20 m pond, 2 m deep, a 10 cm stone. Its
	//longest waves run at metres a second and are the ones a fixed-strength
	//sponge let back in. Ninety seconds carries the slowest ripple here
	//across the frame and out several times over.
	double bigFlux = 0.0;
	const double big = energyAfter( perturb.banksSwapped ? Banks::Walls : Banks::Open, 3.0, 90.0, bigFlux, 20.0, 2.0,
	                                0.1, 256 );
	Check( big >= 0.0 && big < 0.02,
	       fmt( "Open, a 20 m pond 2 m deep: frame energy at 90 s is %.4f of its value at 3 s (bound < 0.02)", big ) );
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
		rig.Set( PT_RAIN_SIZE, 0.75f );//about 9 mm: big drops, a violent net
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

	//3. A known surface: a plane wave, no folds, the sun overhead and then
	//55 degrees up on either side of it (azimuth 0 and 180, in the wave's
	//plane). The bed point x' where the light entering at x lands, less where
	//flat water would land it, receives 1 / (dx'/dx) of light. The oblique
	//suns are what pin the sun's DIRECTION: overhead, and in the mean, a sign
	//error in it or in the flat-water shift would pass.
	struct Sun
	{
		double elevation, azimuth;
		const char* name;
	};
	for( const Sun sun : { Sun { 90.0, 0.0, "overhead" }, Sun { 55.0, 0.0, "55 deg, azimuth 0" },
	                       Sun { 55.0, 180.0, "55 deg, azimuth 180" } } )
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
		//Weak enough not to fold: dx'/dx stays between about 0.6 and 1.4
		//overhead. An oblique sun leans the refracted light further per unit
		//of slope, so the same wave focuses near to a fold; it gets a weaker
		//wave, to test the direction rather than the singularity.
		const double strength = sun.elevation > 89.0 ? 0.4 : 0.2;
		wave.amplitude = strength / ( depth * ( 1.0 - 1.0 / kWaterIndex ) * wave.kx * wave.kx );

		Rig rig;
		if( !frozenPond( rig, 1280, 720, buildCard( 1280, 720 ), wave, depth, 1.0 ) )
			return 1;
		rig.Set( PT_CAUSTICS, 0.5f );
		rig.Set( PT_SUN_ELEVATION, static_cast< float >( ( sun.elevation - 10.0 ) / 80.0 ) );
		rig.Set( PT_SUN_AZIMUTH, static_cast< float >( sun.azimuth / 360.0 ) );
		rig.Set( PT_SUN_SIZE, 0.0f );
		rig.Render( 1 );
		const Floats c = rig.CausticMap();
		const double W = rig.plugin.FrameWidthMetres();

		//The landing map, from scratch -- vector Snell in the x-z plane -- and
		//its derivative numerically.
		const double h  = depth * perturb.focusDepth;
		const double e  = sun.elevation * kPi / 180.0, az = sun.azimuth * kPi / 180.0;
		const double Lx = -std::cos( e ) * std::cos( az ), Lz = -std::sin( e );
		auto refracted  = [ & ]( double nx, double nz, double& tx, double& tz ) {
			const double eta = 1.0 / kWaterIndex;
			const double cosI = -( nx * Lx + nz * Lz );
			const double k    = 1.0 - eta * eta * ( 1.0 - cosI * cosI );
			tx                = eta * Lx + ( eta * cosI - std::sqrt( k ) ) * nx;
			tz                = eta * Lz + ( eta * cosI - std::sqrt( k ) ) * nz;
		};
		double t0x, t0z;
		refracted( 0.0, 1.0, t0x, t0z );
		const double flat = h * t0x / -t0z;
		auto landing      = [ & ]( double x ) {
			const double s = wave.slopeX( x, 0.0 ), norm = std::sqrt( 1.0 + s * s );
			double tx, tz;
			refracted( -s / norm, 1.0 / norm, tx, tz );
			return x + ( h + wave.eta( x, 0.0 ) ) * tx / -tz - flat;
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

		//One mesh cell, in metres: the caustic mesh is 1.2 frames across.
		const double cell = 1.2 * W / std::max( rig.plugin.MeshColumns(), 1 );
		double worst = 0.0, lo = 10.0, hi = 0.0, bound = 0.0;
		for( int x = 128; x < 1152; ++x )
		{
			const double xb       = ( x + 0.5 ) / 1280.0 * W;
			const double expected = expectedAt( xb );
			//The wave runs along x, so every row should read the same: the
			//column's mean over 360 rows takes out the luck of which jittered
			//triangle covers which pixel centre, which is the point of the
			//jitter. What is left is that each triangle carries its own
			//AVERAGE focus, and a triangle reaches up to 0.8 of a mesh cell
			//from any point in it -- so the check allows how far the exact
			//value moves within 0.8 of a cell. Derived from the expectation,
			//not fitted to the result.
			double got = 0.0;
			for( int y = 180; y < 540; ++y )
				got += c[ static_cast< size_t >( y ) * 1280 + x ];
			got /= 360.0;
			const double spread = std::max( std::fabs( expectedAt( xb + 0.8 * cell ) - expected ),
			                                std::fabs( expectedAt( xb - 0.8 * cell ) - expected ) );
			const double allow  = spread + 0.01;
			if( std::fabs( got - expected ) > allow )
			{
				worst = std::max( worst, std::fabs( got - expected ) - allow );
			}
			bound = std::max( bound, allow );
			lo    = std::min( lo, got );
			hi    = std::max( hi, got );
		}
		Check( worst == 0.0 && hi > 1.15 && lo < 0.85,
		       fmt( "plane wave, sun %s: light from %.3f to %.3f, every column (mean of 360 rows) within 0.8 of a mesh cell's variation + 0.01 of "
		            "1/(dx'/dx) (largest allowance %.3f, worst excess %.4f)",
		            sun.name, lo, hi, bound, worst ) );
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
		//Ten seconds of water with nothing in it, the whole chain running
		//every frame. This checks the composite on flat water; it cannot
		//check the transforms, which map zero to zero whatever they do --
		//--fft is the check of those.
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

	//A host texture bigger than its picture: 960x540 in the corner of a
	//1024x1024 texture padded with magenta. Still water must still be the
	//picture, and water full of rain must never show the padding.
	for( int raining = 0; raining < 2; ++raining )
	{
		const int w = 960, h = 540;
		const Floats card = buildCard( w, h );
		Rig rig;
		if( !rig.Init( w, h, &card ) )
			return 1;
		rig.Calm();
		rig.Set( PT_REFLECTION, 0.0f );
		if( raining )
			rig.Set( PT_RAIN, 0.7f );
		const GLuint padded = paddedTexture( card, w, h, 1024, 1024 );
		rig.inputStruct.Handle         = padded;
		rig.inputStruct.HardwareWidth  = 1024;
		rig.inputStruct.HardwareHeight = 1024;
		rig.Render( raining ? 120 : 5 );
		const Floats out = rig.Output();
		double worst = 0.0;
		int magenta  = 0;
		for( size_t i = 0; i < out.size(); i += 4 )
		{
			for( int c = 0; c < 3; ++c )
				worst = std::max( worst, static_cast< double >( std::fabs( out[ i + c ] - card[ i + c ] ) ) );
			magenta += out[ i ] > 0.9f && out[ i + 1 ] < 0.25f && out[ i + 2 ] > 0.9f;
		}
		glDeleteTextures( 1, &padded );
		if( raining )
			Check( magenta == 0, fmt( "padded host texture, heavy rain: %d pixels of the padding show (the card has none)", magenta ) );
		else
			Check( worst < 1e-5, fmt( "padded host texture (960x540 in 1024x1024), still water: the picture to within %.1e", worst ) );
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

	//A throw hard enough to cross the pond: the stone stops at the far bank.
	//The grid is periodic, so a touch past it would wrap round and splash
	//back into the frame somewhere unrelated.
	{
		Rig hard;
		if( !hard.Init( 160, 90 ) )
			return 1;
		hard.Calm();
		hard.Set( PT_DETAIL, detailParam( 256 ) );
		hard.Set( PT_CAUSTICS, 0.0f );
		hard.Set( PT_THROW_SPEED, 0.8f );
		hard.Set( PT_BOUNCES, 1.0f );
		hard.plugin.KeepInjectedLog( true );
		hard.Render( 1 );
		hard.Press( PT_SKIM );
		hard.Render( 600 );
		const std::vector< Impact >& touches = hard.plugin.InjectedForTest();
		const double W = hard.plugin.FrameWidthMetres(), H = hard.plugin.FrameHeightMetres();
		int outside = 0;
		for( const Impact& t : touches )
			outside += t.x < -t.radius || t.x > W + t.radius || t.y < -t.radius || t.y > H + t.radius;
		//At this speed the first hop alone is longer than the pond, so the
		//stone touches once and is gone; the claim is that nothing lands
		//past the bank, where the periodic grid would bring it back in.
		Check( outside == 0 && !touches.empty(),
		       fmt( "a %.1f m/s throw over a %.1f m pond: %zu touch%s before the far bank, %d past it", ThrowSpeedFromParam( 0.8f ),
		            W, touches.size(), touches.size() == 1 ? "" : "es", outside ) );
	}

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
// --state
//===========================================================================
int stateCheck( bool shaken, bool leak )
{
	if( shaken )
		std::printf( "\n=== state-shaken: shaken, what the host hands over is still what it gets back\n" );
	else
		std::printf( "\n=== state: what the host hands over is what it gets back\n" );

	//A host-like context: a vertex array of its own bound, blending on with
	//its own function, a clear colour, a scissor box, texture unit 3 active --
	//none of which the plugin has any business keeping.
	Rig rig;
	if( !rig.Init( 320, 180 ) )
		return 1;
	if( shaken )
	{
		//Squares: every Faraday pass, the kernel's transforms included.
		rig.Set( PT_SHAKE, 0.5f );
		rig.Set( PT_PATTERN, static_cast< float >( Pattern::Squares ) );
	}
	GLuint hostArray = 0;
	GLuint leaked    = 0;
	if( leak )
		leaked = makeTexture( 4, 4, nullptr );
	glGenVertexArrays( 1, &hostArray );
	int problems = 0;
	std::string what;

	for( int frame = 0; frame < 3; ++frame )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, rig.outputFBO );
		glViewport( 7, 5, 300, 170 );
		glBindVertexArray( hostArray );
		glEnable( GL_BLEND );
		glBlendFuncSeparate( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO );
		glClearColor( 0.2f, 0.3f, 0.4f, 0.5f );
		glEnable( GL_SCISSOR_TEST );
		glScissor( 0, 0, 320, 180 );
		glActiveTexture( GL_TEXTURE3 );
		glActiveTexture( GL_TEXTURE0 );
		glUseProgram( 0 );

		rig.plugin.SetTime( frame / 60.0 );
		if( rig.plugin.ProcessOpenGL( &rig.process ) != FF_SUCCESS )
			return 1;
		if( leak )
		{
			glActiveTexture( GL_TEXTURE2 );
			glBindTexture( GL_TEXTURE_2D, leaked );
			glActiveTexture( GL_TEXTURE0 );
		}

		GLint viewport[ 4 ] = {}, array = 0, program = 0, unit = 0, fbo = 0, src = 0, dst = 0;
		GLfloat clear[ 4 ] = {};
		glGetIntegerv( GL_VIEWPORT, viewport );
		glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &array );
		glGetIntegerv( GL_CURRENT_PROGRAM, &program );
		glGetIntegerv( GL_ACTIVE_TEXTURE, &unit );
		glGetIntegerv( GL_FRAMEBUFFER_BINDING, &fbo );
		glGetIntegerv( GL_BLEND_SRC_RGB, &src );
		glGetIntegerv( GL_BLEND_DST_RGB, &dst );
		glGetFloatv( GL_COLOR_CLEAR_VALUE, clear );

		auto expect = [ & ]( bool ok, const char* name ) {
			if( !ok )
			{
				++problems;
				what += std::string( " " ) + name;
			}
		};
		expect( viewport[ 0 ] == 7 && viewport[ 1 ] == 5 && viewport[ 2 ] == 300 && viewport[ 3 ] == 170, "viewport" );
		expect( array == static_cast< GLint >( hostArray ), "vertex-array" );
		expect( program == 0, "program" );
		expect( unit == GL_TEXTURE0, "active-unit" );
		expect( fbo == static_cast< GLint >( rig.outputFBO ), "framebuffer" );
		expect( glIsEnabled( GL_BLEND ) && src == GL_SRC_ALPHA && dst == GL_ONE_MINUS_SRC_ALPHA, "blend" );
		expect( glIsEnabled( GL_SCISSOR_TEST ), "scissor" );
		expect( clear[ 0 ] == 0.2f && clear[ 1 ] == 0.3f && clear[ 2 ] == 0.4f && clear[ 3 ] == 0.5f, "clear-colour" );
		for( int u = 0; u < 8; ++u )
		{
			GLint bound = 0;
			glActiveTexture( static_cast< GLenum >( GL_TEXTURE0 + u ) );
			glGetIntegerv( GL_TEXTURE_BINDING_2D, &bound );
			expect( bound == 0, "texture-unit" );
		}
		glActiveTexture( GL_TEXTURE0 );
	}

	glDisable( GL_SCISSOR_TEST );
	glDisable( GL_BLEND );
	glBindVertexArray( 0 );
	glDeleteVertexArrays( 1, &hostArray );
	if( leaked )
	{
		for( int u = 0; u < 8; ++u )
		{
			glActiveTexture( static_cast< GLenum >( GL_TEXTURE0 + u ) );
			glBindTexture( GL_TEXTURE_2D, 0 );
		}
		glActiveTexture( GL_TEXTURE0 );
		glDeleteTextures( 1, &leaked );
	}
	if( shaken )
		Check( rig.plugin.FaradayActive() && rig.plugin.FaradaySteps() > 0,
		       "the frames ran shaken: every Faraday pass, the kernel's transforms included" );

	Check( problems == 0, fmt( "three frames: viewport, vertex array, program, active unit, framebuffer, blend, scissor, "
	                           "clear colour and eight texture units all as the host left them (%d wrong:%s)",
	                           problems, what.empty() ? " none" : what.c_str() ) );
	return Verdict();
}


//===========================================================================
// --negative
//===========================================================================
int runState( const Perturb& )
{
	return stateCheck( false, false );
}

int runStateShaken( const Perturb& perturb )
{
	return stateCheck( true, perturb.stateLeak );
}

//===========================================================================
// Faraday: the tank shaken up and down by the bass.
//
// The checks drive the real shaken passes through ProcessOpenGL and measure
// the water they leave. Where a check needs the model's equation it writes
// Benjamin & Ursell's (2.12) out again here, in double, with its own RK4 --
// never the plugin's Magnus step, and never faraday::ThresholdAcceleration,
// which is only the unit Shake is measured in.
//
// Every one of them runs at the raster `--size` gives (verify.sh: 320x180 and
// 1280x720, and again on Apple's software renderer). The water lives on a grid
// sized in metres, so the raster changes the caustic and output buffers and
// nothing a check measures; running at two is what proves that.
//===========================================================================
int g_checkWidth  = 1280;
int g_checkHeight = 720;

/// Controls.cpp's kMaxOverdrive: Shake 1 is this many thresholds at full bass.
constexpr double kShakeTop = 4.0;

/// Everything a Faraday check depends on, set every time: a rig default can
/// change what a check measures (honeydew's trap).
struct ShakeSetup
{
	double pond      = 1.5; ///< frame height, m
	int cells        = 512; ///< Detail
	bool walls       = true;///< the whole periodic domain is pond: no sponge
	double driveHz   = 10.0;
	double damping   = 2.0; ///< the walls' rate, s^-1
	double overdrive = 2.0; ///< thresholds at full bass
	double slow      = 1.0;
	int pattern      = static_cast< int >( Pattern::Stripes );
	double speed     = 1.0;
	bool seed        = true;
};

void applyShake( Rig& rig, const ShakeSetup& s )
{
	rig.Calm();
	rig.Set( PT_CAUSTICS, 0.0f );
	rig.Set( PT_POND_SIZE, pondParam( s.pond ) );
	rig.Set( PT_DETAIL, detailParam( s.cells ) );
	rig.Set( PT_BANKS, static_cast< float >( s.walls ? Banks::Walls : Banks::Open ) );
	rig.Set( PT_DEPTH, depthParam( 0.3 ) );
	rig.Set( PT_TENSION, tensionParam( kWaterTension ) );
	rig.Set( PT_VISCOSITY, viscosityParam( 1e-6 ) );
	rig.Set( PT_SPEED, ParamFromSpeed( static_cast< float >( s.speed ) ) );
	rig.Set( PT_SHAKE, static_cast< float >( s.overdrive / kShakeTop ) );
	rig.Set( PT_DRIVE, ParamFromDrive( static_cast< float >( s.driveHz ) ) );
	rig.Set( PT_DAMPING, ParamFromDamping( static_cast< float >( s.damping ) ) );
	rig.Set( PT_SLOW_MOTION, ParamFromSlowMotion( s.slow ) );
	rig.Set( PT_PATTERN, static_cast< float >( s.pattern ) );
	rig.Set( PT_BASS_BINS, ParamFromBassBins( 2 ) );
	rig.plugin.SetSeedForTest( s.seed );
	rig.feed = AudioFeed::Drone;
}

//---------------------------------------------------------------------------
// Benjamin & Ursell (2.12), in double:
//     eta'' + 2 gamma eta' + ( omega0^2 - a k tanh( k h ) cos theta ) eta = 0
// with millpond's dispersion relation and Lamb's damping plus the walls'.
//---------------------------------------------------------------------------
struct MathieuMode
{
	double k       = 0.0;
	double omega2  = 0.0;///< omega0^2
	double gamma   = 0.0;
	double perAccel = 0.0;///< k tanh( k h ): q per unit of shake acceleration
};

MathieuMode mathieuMode( double k, const Water& water, double walls )
{
	MathieuMode m;
	const double t = std::tanh( std::min( k * water.depth, 20.0 ) );
	m.k            = k;
	m.omega2       = ( water.gravity * k + water.tension * k * k * k ) * t;
	m.gamma        = 2.0 * water.viscosity * k * k + walls;
	m.perAccel     = k * t;
	return m;
}

/// The wavenumber whose free frequency is `omega`, by the harness's own
/// bisection on the same dispersion relation.
double dispersionWavenumber( double omega, const Water& water )
{
	double lo = 1e-3, hi = 1e6;
	for( int i = 0; i < 200; ++i )
	{
		const double mid = std::sqrt( lo * hi );
		if( std::sqrt( mathieuMode( mid, water, 0.0 ).omega2 ) < omega )
			lo = mid;
		else
			hi = mid;
	}
	return std::sqrt( lo * hi );
}

/// RK4 over `dt` in `n` steps, from water time t0 with drive phase
/// theta( t ) = theta0 + omegaDrive t.
void mathieuRK4( const MathieuMode& m, double accel, double theta0, double omegaDrive, double t0, double dt, int n,
                 double& eta, double& rate )
{
	const double q = accel * m.perAccel;
	auto f         = [ & ]( double t, double e, double r, double& de, double& dr ) {
        de = r;
        dr = -2.0 * m.gamma * r - ( m.omega2 - q * std::cos( theta0 + omegaDrive * t ) ) * e;
	};
	const double h = dt / n;
	double t       = t0;
	for( int i = 0; i < n; ++i )
	{
		double k1e, k1r, k2e, k2r, k3e, k3r, k4e, k4r;
		f( t, eta, rate, k1e, k1r );
		f( t + 0.5 * h, eta + 0.5 * h * k1e, rate + 0.5 * h * k1r, k2e, k2r );
		f( t + 0.5 * h, eta + 0.5 * h * k2e, rate + 0.5 * h * k2r, k3e, k3r );
		f( t + h, eta + h * k3e, rate + h * k3r, k4e, k4r );
		eta += h / 6.0 * ( k1e + 2.0 * k2e + 2.0 * k3e + k4e );
		rate += h / 6.0 * ( k1r + 2.0 * k2r + 2.0 * k3r + k4r );
		t += h;
	}
}

/// The Floquet exponent (growth rate, 1/s) of the mode at this shake: the
/// monodromy matrix over one drive period, by RK4 at 4000 steps.
double floquetExponent( const MathieuMode& m, double accel, double omegaDrive )
{
	const double period = 2.0 * kPi / omegaDrive;
	double e1 = 1.0, r1 = 0.0, e2 = 0.0, r2 = 1.0;
	mathieuRK4( m, accel, 0.0, omegaDrive, 0.0, period, 4000, e1, r1 );
	mathieuRK4( m, accel, 0.0, omegaDrive, 0.0, period, 4000, e2, r2 );
	const double trace = e1 + r2;
	const double det   = e1 * r2 - e2 * r1;
	const double disc  = trace * trace - 4.0 * det;
	double modulus;
	if( disc >= 0.0 )
		modulus = std::max( std::fabs( 0.5 * ( trace + std::sqrt( disc ) ) ), std::fabs( 0.5 * ( trace - std::sqrt( disc ) ) ) );
	else
		modulus = std::sqrt( det );
	return std::log( modulus ) / period;
}

/// The exact first-tongue threshold for this mode: the shake at which its
/// Floquet exponent crosses zero, by bisection.
double floquetThreshold( const MathieuMode& m, double omegaDrive )
{
	double lo = 0.0, hi = 1.0;
	while( floquetExponent( m, hi, omegaDrive ) < 0.0 && hi < 1e6 )
		hi *= 2.0;
	for( int i = 0; i < 60; ++i )
	{
		const double mid = 0.5 * ( lo + hi );
		if( floquetExponent( m, mid, omegaDrive ) < 0.0 )
			lo = mid;
		else
			hi = mid;
	}
	return 0.5 * ( lo + hi );
}

/// The grid's projection onto one standing mode cos( kx x + ky y ), for one
/// channel of the surface.
double projectMode( const Floats& s, const Grid& g, int mx, int my, int channel )
{
	const double kx = 2.0 * kPi * mx / g.lx, ky = 2.0 * kPi * my / g.ly;
	double sum = 0.0, norm = 0.0;
	for( int y = 0; y < g.ny; ++y )
		for( int x = 0; x < g.nx; ++x )
		{
			const double px = ( x + 0.5 ) * g.lx / g.nx, py = ( y + 0.5 ) * g.ly / g.ny;
			const double basis = std::cos( kx * px + ky * py );
			sum += s[ ( static_cast< size_t >( y ) * g.nx + x ) * 4 + channel ] * basis;
			norm += basis * basis;
		}
	return sum / norm;
}

Floats modeState( const Grid& g, int mx, int my, double amp )
{
	Floats state( static_cast< size_t >( g.nx ) * g.ny * 4, 0.0f );
	const double kx = 2.0 * kPi * mx / g.lx, ky = 2.0 * kPi * my / g.ly;
	for( int y = 0; y < g.ny; ++y )
		for( int x = 0; x < g.nx; ++x )
		{
			const double px = ( x + 0.5 ) * g.lx / g.nx, py = ( y + 0.5 ) * g.ly / g.ny;
			state[ ( static_cast< size_t >( y ) * g.nx + x ) * 4 ] = static_cast< float >( amp * std::cos( kx * px + ky * py ) );
		}
	return state;
}

/// eta's correlation between two surfaces: 1 for the same pattern, -1 for it
/// upside down.
double correlation( const Floats& a, const Floats& b )
{
	double ab = 0.0, aa = 0.0, bb = 0.0;
	for( size_t i = 0; i < a.size(); i += 4 )
	{
		ab += static_cast< double >( a[ i ] ) * b[ i ];
		aa += static_cast< double >( a[ i ] ) * a[ i ];
		bb += static_cast< double >( b[ i ] ) * b[ i ];
	}
	return aa > 0.0 && bb > 0.0 ? ab / std::sqrt( aa * bb ) : 0.0;
}

/// The lattice's height, whatever phase of its oscillation the frame caught:
/// the RMS of eta^2 + ( eta_t / omega_c )^2. eta's own RMS swings from zero
/// to full every half response period (it cost a day: three Speeds gave three
/// "saturated amplitudes" a factor of five apart, all the same lattice).
double rmsAmplitude( const Floats& s, double omegaC )
{
	double sum = 0.0;
	for( size_t i = 0; i < s.size(); i += 4 )
	{
		const double rate = kRateScale * static_cast< double >( s[ i + 1 ] ) / omegaC;
		sum += static_cast< double >( s[ i ] ) * s[ i ] + rate * rate;
	}
	return std::sqrt( sum / static_cast< double >( s.size() / 4 ) );
}

/// The RMS of a - b over eta, relative to the RMS of a.
double relativeDifference( const Floats& a, const Floats& b )
{
	double d = 0.0, n = 0.0;
	for( size_t i = 0; i < a.size(); i += 4 )
	{
		const double e = static_cast< double >( a[ i ] ) - b[ i ];
		d += e * e;
		n += static_cast< double >( a[ i ] ) * a[ i ];
	}
	return n > 0.0 ? std::sqrt( d / n ) : ( d > 0.0 ? 1.0 : 0.0 );
}

bool requireShaken( Rig& rig, const char* what )
{
	//A negative control that makes the system fall back passes vacuously
	//(dice): every measurement asserts that the shaken passes actually ran.
	if( rig.plugin.FaradayActive() && rig.plugin.FaradaySteps() > 0 )
		return true;
	Check( false, fmt( "%s: the last frame did not run shaken (Faraday shaders missing?)", what ) );
	return false;
}

//===========================================================================
// --mathieu
//===========================================================================
int runMathieu( const Perturb& perturb )
{
	std::printf( "\n=== mathieu: one mode on the GPU follows Benjamin & Ursell's damped Mathieu equation\n" );

	struct Case
	{
		const char* what;
		double driveHz, pond, overdrive, detune;
		double seconds;
	};
	const Case cases[] = {
		{ "below threshold", 10.0, 1.5, 0.5, 0.0, 2.0 },
		{ "twice threshold", 10.0, 1.5, 2.0, 0.0, 2.0 },
		{ "4x, detuned 12%", 10.0, 1.5, 4.0, 0.12, 1.5 },
		{ "capillary, 40 Hz", 40.0, 0.2, 2.0, 0.0, 1.0 },
	};

	for( const Case& c : cases )
	{
		Rig rig;
		if( !rig.Init( g_checkWidth, g_checkHeight ) )
			return 1;
		ShakeSetup setup;
		setup.pond      = c.pond;
		setup.cells     = 256;
		setup.driveHz   = c.driveHz;
		setup.overdrive = 0.0;
		setup.seed      = false;
		applyShake( rig, setup );
		if( !rig.Render( 1 ) )
			return 1;

		const Grid g      = rig.plugin.CurrentGrid();
		const Water water = rig.plugin.CurrentWater();
		const double omegaDrive = 2.0 * kPi * c.driveHz;
		const double kc   = dispersionWavenumber( 0.5 * omegaDrive, water );
		const int mx      = static_cast< int >( std::lround( kc * ( 1.0 + c.detune ) * g.lx / ( 2.0 * kPi ) ) );
		const MathieuMode mode = mathieuMode( 2.0 * kPi * mx / g.lx, water, setup.damping );

		//Small enough that the saturating damping is nothing beside the
		//tolerance at the end of the run: start low by the growth expected.
		const double growth = std::max( 0.0, ( c.overdrive - 1.0 ) * mode.gamma );
		const double amp0   = 1e-6 * std::exp( -growth * c.seconds );
		rig.plugin.LoadSurfaceForTest( modeState( g, mx, 0, amp0 ) );

		setup.overdrive = c.overdrive;
		applyShake( rig, setup );

		double eta = amp0, rate = 0.0, t = 0.0, theta0 = 0.0, accel = 0.0, drive = omegaDrive;
		int substeps = 0;
		bool first = true, steady = true;
		const int frames = static_cast< int >( std::lround( c.seconds * rig.fps ) );
		for( int f = 0; f < frames; ++f )
		{
			if( !rig.Render( 1 ) || !requireShaken( rig, c.what ) )
				return 1;
			const double step = rig.plugin.FaradayWaterStep();
			if( first )
			{
				//The drive the plugin was given, which is the control's float
				//mapping of c.driveHz: a 1e-7 difference is 1e-5 rad by the end.
				theta0 = rig.plugin.FaradayDrivePhase();
				accel  = rig.plugin.FaradayAcceleration();
				drive  = 2.0 * kPi * rig.plugin.FaradayDriveHz();
				first  = false;
			}
			steady = steady && rig.plugin.FaradayAcceleration() == accel;
			substeps += rig.plugin.FaradaySteps();
			mathieuRK4( mode, accel * perturb.mathieuForcing, theta0, drive, t, step, 2000, eta, rate );
			t += step;
		}

		const Floats back  = rig.Surface();
		const double gotEta  = projectMode( back, g, mx, 0, 0 );
		const double gotRate = projectMode( back, g, mx, 0, 1 ) * kRateScale;
		const double omega0  = std::sqrt( mode.omega2 );
		const double size    = std::hypot( eta, rate / omega0 );
		const double error   = std::hypot( gotEta - eta, ( gotRate - rate ) / omega0 ) / size;

		//The budget: fourth-order Magnus at 32 substeps a drive period is
		//within 6e-5 of RK4 over these runs (measured in double, AGENTS.md);
		//float rounding is at most a few ulp a substep, 16 x 6e-8 x the
		//substeps. Nothing here is fitted to what this machine printed.
		const double tolerance = 1e-4 + 16.0 * 6e-8 * substeps;
		const double saturation = 150.0 * mode.gamma * mode.k * mode.k * size * size * c.seconds;
		Check( steady && error < tolerance,
		       fmt( "%-17s k=%6.1f (k_c %6.1f) a=%.3f m/s^2: |y| %.3e (expect %.3e), error %.2e of |y| (bound %.1e, "
		            "%d substeps; saturation %.0e)",
		            c.what, mode.k, kc, accel, std::hypot( gotEta, gotRate / omega0 ), size, error, tolerance, substeps,
		            saturation ) );
	}
	return Verdict();
}

//===========================================================================
// --onset
//===========================================================================
int runOnset( const Perturb& perturb )
{
	std::printf( "\n=== onset: the GPU's mode starts to grow where the damped Mathieu tongue says\n" );

	struct Case
	{
		const char* what;
		double driveHz, pond, damping;
	};
	const Case cases[] = {
		{ "gravity, 10 Hz", 10.0, 1.5, 2.0 },
		{ "capillary, 40 Hz", 40.0, 0.2, 0.5 },
	};

	for( const Case& c : cases )
	{
		//The grid's mode nearest k_c, driven at exactly twice its own
		//frequency. Off resonance, below the tongue, the Floquet multipliers
		//are a complex pair and a mode beats instead of decaying steadily, so
		//two samples no longer measure a rate.
		ShakeSetup setup;
		setup.pond      = c.pond;
		setup.cells     = 256;
		setup.damping   = c.damping;
		setup.overdrive = 0.0;
		setup.seed      = false;
		Grid g;
		Water water;
		{
			Rig probe;
			if( !probe.Init( g_checkWidth, g_checkHeight ) )
				return 1;
			setup.driveHz = c.driveHz;
			applyShake( probe, setup );
			if( !probe.Render( 1 ) )
				return 1;
			g     = probe.plugin.CurrentGrid();
			water = probe.plugin.CurrentWater();
		}
		const int mx     = static_cast< int >( std::lround( dispersionWavenumber( kPi * c.driveHz, water ) * g.lx / ( 2.0 * kPi ) ) );
		MathieuMode mode = mathieuMode( 2.0 * kPi * mx / g.lx, water, c.damping );
		setup.driveHz    = std::sqrt( mode.omega2 ) / kPi;
		double omegaDrive = 2.0 * kPi * setup.driveHz;

		//One run at `overdrive` thresholds (the plugin's own unit): the
		//shake it actually used, and the mode's growth rate measured from
		//the surface. Frames three to a drive period, and samples a whole
		//number of drive periods apart, so both see the same phase.
		auto growthAt = [ & ]( double overdrive, double& accelUsed ) -> double {
			Rig rig;
			rig.fps = 3.0 * setup.driveHz;
			if( !rig.Init( g_checkWidth, g_checkHeight ) )
				return std::nan( "" );
			setup.overdrive = 0.0;
			applyShake( rig, setup );
			if( !rig.Render( 1 ) )
				return std::nan( "" );
			rig.plugin.LoadSurfaceForTest( modeState( g, mx, 0, 1e-7 ) );
			setup.overdrive = overdrive;
			applyShake( rig, setup );

			//The decaying Floquet solution goes 2 s0 faster than the growing
			//one, and s0 is 0.9 gamma at the lower shake: 5 / gamma leaves
			//e^-9 of it, a tenth of the --mathieu bound. (3.5 / gamma left
			//e^-6.3, which showed as 6e-4/s on the lower rate.) Then two
			//seconds of growth.
			const int settle  = 6 * static_cast< int >( std::ceil( 5.0 / mode.gamma * rig.fps / 6.0 ) );
			const int measure = 6 * static_cast< int >( std::ceil( 2.0 * rig.fps / 6.0 ) );
			if( !rig.Render( settle ) || !requireShaken( rig, c.what ) )
				return std::nan( "" );
			accelUsed       = rig.plugin.FaradayAcceleration();
			omegaDrive      = 2.0 * kPi * rig.plugin.FaradayDriveHz();
			const double a  = projectMode( rig.Surface(), g, mx, 0, 0 );
			const double t0 = rig.plugin.SimTime();
			if( !rig.Render( measure ) )
				return std::nan( "" );
			const double b = projectMode( rig.Surface(), g, mx, 0, 0 );
			return std::log( std::fabs( b / a ) ) / ( rig.plugin.SimTime() - t0 );
		};

		double aLow = 0.0, aHigh = 0.0;
		const double lowRate  = growthAt( 0.9, aLow );
		const double highRate = growthAt( 1.1, aHigh );
		if( std::isnan( lowRate ) || std::isnan( highRate ) )
			return 1;
		const double measured = aLow + ( aHigh - aLow ) * ( -lowRate ) / ( highRate - lowRate );

		//The exact tongue, from the monodromy matrix in double. The same
		//straight-line interpolation through the exact rates at the same two
		//shakes says how much the interpolation itself moves the onset.
		MathieuMode expected = mode;
		expected.gamma *= perturb.onsetDamping;
		const double floquet   = floquetThreshold( expected, omegaDrive );
		const double lowExact  = floquetExponent( expected, aLow, omegaDrive );
		const double highExact = floquetExponent( expected, aHigh, omegaDrive );
		const double straight  = aLow + ( aHigh - aLow ) * ( -lowExact ) / ( highExact - lowExact );
		const double leading   = 4.0 * mode.gamma * std::sqrt( mode.omega2 ) / mode.perAccel;

		//The bound: each amplitude to the --mathieu budget (1e-3 of itself),
		//so each rate to 2e-3 / 2 s; through the slope of rate against shake
		//that is the onset's; plus the interpolation's own bias.
		const double slope = ( highExact - lowExact ) / ( aHigh - aLow );
		const double bound = ( 2.0 * 1e-3 / 2.0 ) / slope / floquet + std::fabs( straight / floquet - 1.0 );
		const double off   = measured / floquet - 1.0;
		Check( std::fabs( off ) < bound,
		       fmt( "%-16s k=%6.1f gamma=%.3f: growth %+.4f/s (exact %+.4f) at %.4f, %+.4f/s (exact %+.4f) at %.4f m/s^2 "
		            "-> onset %.5f; tongue %.5f (%+.1e, bound %.1e); leading order %.5f",
		            c.what, mode.k, mode.gamma, lowRate, lowExact, aLow, highRate, highExact, aHigh, measured, floquet,
		            off, bound, leading ) );
	}
	return Verdict();
}

//---------------------------------------------------------------------------
// Measuring a lattice. A radix-2 FFT with bit reversal for the analysis only
// (--fft keeps its O(N^2) DFT, which shares no idea with the shader's).
//---------------------------------------------------------------------------
void fftLine( std::vector< Complex >& a, double sign )
{
	const size_t n = a.size();
	for( size_t i = 1, j = 0; i < n; ++i )
	{
		size_t bit = n >> 1;
		for( ; j & bit; bit >>= 1 )
			j ^= bit;
		j ^= bit;
		if( i < j )
			std::swap( a[ i ], a[ j ] );
	}
	for( size_t len = 2; len <= n; len <<= 1 )
	{
		const double angle = sign * 2.0 * kPi / static_cast< double >( len );
		for( size_t i = 0; i < n; i += len )
			for( size_t j = 0; j < len / 2; ++j )
			{
				const double c = std::cos( angle * j ), s = std::sin( angle * j );
				const Complex u = a[ i + j ], v = a[ i + j + len / 2 ];
				const Complex t = { v.first * c - v.second * s, v.first * s + v.second * c };
				a[ i + j ]           = { u.first + t.first, u.second + t.second };
				a[ i + j + len / 2 ] = { u.first - t.first, u.second - t.second };
			}
	}
}

/// |FFT|^2 of one channel (times `scale`) over a power-of-two block of the
/// grid, Hann-windowed when asked. Returns nx * ny powers, natural order.
std::vector< double > powerSpectrum( const Floats& s, const Grid& g, int x0, int y0, int nx, int ny, bool hann,
                                     int channel = 0, double scale = 1.0 )
{
	std::vector< Complex > field( static_cast< size_t >( nx ) * ny );
	for( int y = 0; y < ny; ++y )
		for( int x = 0; x < nx; ++x )
		{
			const double w = hann ? ( 0.5 - 0.5 * std::cos( 2.0 * kPi * ( x + 0.5 ) / nx ) )
			                            * ( 0.5 - 0.5 * std::cos( 2.0 * kPi * ( y + 0.5 ) / ny ) )
			                      : 1.0;
			field[ static_cast< size_t >( y ) * nx + x ] = {
				w * scale * s[ ( static_cast< size_t >( y0 + y ) * g.nx + ( x0 + x ) ) * 4 + channel ], 0.0 };
		}
	std::vector< Complex > line( nx );
	for( int y = 0; y < ny; ++y )
	{
		for( int x = 0; x < nx; ++x )
			line[ x ] = field[ static_cast< size_t >( y ) * nx + x ];
		fftLine( line, -1.0 );
		for( int x = 0; x < nx; ++x )
			field[ static_cast< size_t >( y ) * nx + x ] = line[ x ];
	}
	line.resize( ny );
	for( int x = 0; x < nx; ++x )
	{
		for( int y = 0; y < ny; ++y )
			line[ y ] = field[ static_cast< size_t >( y ) * nx + x ];
		fftLine( line, -1.0 );
		for( int y = 0; y < ny; ++y )
			field[ static_cast< size_t >( y ) * nx + x ] = line[ y ];
	}
	std::vector< double > power( field.size() );
	for( size_t i = 0; i < field.size(); ++i )
		power[ i ] = field[ i ].first * field[ i ].first + field[ i ].second * field[ i ].second;
	return power;
}

/// The power of eta plus that of eta_t / omega_c: for modes that share the
/// lattice's oscillation, the same whatever phase of it the frame caught.
std::vector< double > latticeSpectrum( const Floats& s, const Grid& g, int x0, int y0, int nx, int ny, bool hann,
                                       double omegaC )
{
	std::vector< double > power = powerSpectrum( s, g, x0, y0, nx, ny, hann, 0, 1.0 );
	const std::vector< double > rate = powerSpectrum( s, g, x0, y0, nx, ny, hann, 1, kRateScale / omegaC );
	for( size_t i = 0; i < power.size(); ++i )
		power[ i ] += rate[ i ];
	return power;
}

//===========================================================================
// --half
//===========================================================================
int runHalf( const Perturb& perturb )
{
	std::printf( "\n=== half: the lattice answers at exactly half the drive\n" );

	//Three frames to a drive period: a response at f/2 is upside down three
	//frames on and the same six frames on; one at f would be the same at three.
	const double driveHz = 10.0;
	Rig rig;
	rig.fps = 3.0 * driveHz;
	if( !rig.Init( g_checkWidth, g_checkHeight ) )
		return 1;
	ShakeSetup setup;
	setup.cells     = 512;
	setup.driveHz   = driveHz;
	setup.overdrive = 3.0;
	setup.pattern   = static_cast< int >( Pattern::Squares );
	setup.speed     = 1.0;
	applyShake( rig, setup );
	if( !rig.Render( static_cast< int >( 6.0 * rig.fps ) ) || !requireShaken( rig, "half" ) )
		return 1;

	std::map< int, Floats > at;
	for( int f = 0; f <= 60; ++f )
	{
		if( f == 0 || f == 3 || f == 6 || f == 60 )
			at[ f ] = rig.Surface();
		if( f < 60 && !rig.Render( 1 ) )
			return 1;
	}
	const double rms   = rmsAmplitude( at[ 0 ], kPi * rig.plugin.FaradayDriveHz() );
	const double one   = correlation( at[ 0 ], at[ 3 ] );
	const double two   = correlation( at[ 0 ], at[ 6 ] );
	const double twenty = correlation( at[ 0 ], at[ 60 ] );

	//0.9 is cos( 0.45 rad ): twenty drive periods on, a response off f/2 by
	//more than 0.036 Hz (0.7%) would have slipped that far. It also takes in
	//the lattice's own slow change over two seconds, which is the only reason
	//the numbers are not 1.
	const double sign = perturb.halfHarmonic ? 1.0 : -1.0;
	Check( rms > 1e-4, fmt( "the lattice stood up: RMS height %.2e m (a seed is about 1e-6)", rms ) );
	Check( sign * one > 0.9, fmt( "one drive period on, the lattice is %s: correlation %+.4f (expect %s0.9)",
	                               perturb.halfHarmonic ? "the same" : "upside down", one, perturb.halfHarmonic ? ">+" : "<-" ) );
	Check( two > 0.9, fmt( "two drive periods on, it is back: correlation %+.4f (expect >+0.9)", two ) );
	Check( twenty > 0.9, fmt( "twenty drive periods on, still in step -- f/2 to 0.7%%: correlation %+.4f (expect >+0.9)", twenty ) );
	return Verdict();
}

//===========================================================================
// --wavelength
//===========================================================================
int runWavelength( const Perturb& perturb )
{
	std::printf( "\n=== wavelength: the lattice's spacing is millpond's dispersion relation at f/2\n" );

	const double driveHz = 30.0;
	Rig rig;
	rig.fps = 30.0;
	if( !rig.Init( g_checkWidth, g_checkHeight ) )
		return 1;
	ShakeSetup setup;
	setup.pond      = 0.5;
	setup.cells     = 512;
	setup.walls     = true;
	setup.driveHz   = driveHz;
	setup.overdrive = 1.5;
	setup.pattern   = static_cast< int >( Pattern::Stripes );
	setup.speed     = 2.0;
	applyShake( rig, setup );
	if( !rig.Render( static_cast< int >( 16.0 * rig.fps / setup.speed ) ) || !requireShaken( rig, "wavelength" ) )
		return 1;

	const Floats surface = rig.Surface();
	const Grid g         = rig.plugin.CurrentGrid();
	const Water water    = rig.plugin.CurrentWater();
	const double accel   = rig.plugin.FaradayAcceleration();
	const double omegaDrive = 2.0 * kPi * rig.plugin.FaradayDriveHz() * perturb.wavelengthOmega;
	const double kDisp   = dispersionWavenumber( 0.5 * omegaDrive, water );

	//Walls: the state is the mirrored pond, periodic, so the grid's own
	//modes are exactly the spectrum -- no window. The lattice's wavenumber is
	//the power-weighted mean |k| over the ring.
	const std::vector< double > power = latticeSpectrum( surface, g, 0, 0, g.nx, g.ny, false, kPi * rig.plugin.FaradayDriveHz() );
	double sum = 0.0, weighted = 0.0;
	for( int y = 0; y < g.ny; ++y )
		for( int x = 0; x < g.nx; ++x )
		{
			const int mx = x < g.nx / 2 ? x : x - g.nx, my = y < g.ny / 2 ? y : y - g.ny;
			const double k = std::hypot( 2.0 * kPi * mx / g.lx, 2.0 * kPi * my / g.ly );
			if( k < 0.6 * kDisp || k > 1.4 * kDisp )
				continue;
			const double p = power[ static_cast< size_t >( y ) * g.nx + x ];
			sum += p;
			weighted += p * k;
		}
	const double measured = weighted / sum;

	//The band the lattice can be made of: every wavenumber whose damped
	//Mathieu mode grows at this shake, by the exact Floquet exponent.
	auto grows = [ & ]( double k ) { return floquetExponent( mathieuMode( k, water, setup.damping ), accel, omegaDrive ) > 0.0; };
	auto edge  = [ & ]( double inside, double outside ) {
        for( int i = 0; i < 40; ++i )
        {
            const double mid = 0.5 * ( inside + outside );
            ( grows( mid ) ? inside : outside ) = mid;
        }
        return 0.5 * ( inside + outside );
	};
	double lo = kDisp, hi = kDisp;
	if( grows( kDisp ) )
	{
		lo = edge( kDisp, 0.5 * kDisp );
		hi = edge( kDisp, 1.5 * kDisp );
	}
	Check( measured >= lo && measured <= hi,
	       fmt( "lattice k = %.2f rad/m (wavelength %.2f mm) against the dispersion relation's %.2f at f/2 (%+.2f%%); "
	            "the growing band at %.3f m/s^2 is %.2f to %.2f (+-%.2f%%)",
	            measured, 2000.0 * kPi / measured, kDisp, 100.0 * ( measured / kDisp - 1.0 ), accel, lo, hi,
	            50.0 * ( hi - lo ) / kDisp ) );
	return Verdict();
}

//===========================================================================
// --seed
//===========================================================================
int runSeed( const Perturb& perturb )
{
	std::printf( "\n=== seed: in silence the shaken pond gets only a little noise near k_c, mirrored at Walls\n" );

	for( const bool walls : { false, true } )
	{
		Rig rig;
		if( !rig.Init( g_checkWidth, g_checkHeight ) )
			return 1;
		ShakeSetup setup;
		setup.pond  = 0.5;
		setup.walls = walls;
		applyShake( rig, setup );
		rig.feed = AudioFeed::Silence;//shaken, but nothing in the bass: the seed alone
		if( !rig.Render( 120 ) || !requireShaken( rig, "seed" ) )
			return 1;

		const Floats surface = rig.Surface();
		const Grid g         = rig.plugin.CurrentGrid();
		const double kc      = dispersionWavenumber( kPi * rig.plugin.FaradayDriveHz(), rig.plugin.CurrentWater() );
		const double centre  = perturb.seedBand * kc;
		const double half    = 3.0 * faraday::kSeedWidth * kc;

		//The band: the share of eta's power more than three standard deviations
		//from k_c. A Gaussian band leaves 0.27% out there (erfc( 3 / sqrt 2 ));
		//damping, stronger at short waves, only takes more away. The grid is
		//periodic either way (Open's sponge quadrants included), so no window.
		const std::vector< double > power = powerSpectrum( surface, g, 0, 0, g.nx, g.ny, false );
		double total = 0.0, outside = 0.0;
		for( int y = 0; y < g.ny; ++y )
			for( int x = 0; x < g.nx; ++x )
			{
				const int mx = x < g.nx / 2 ? x : x - g.nx, my = y < g.ny / 2 ? y : y - g.ny;
				if( mx == 0 && my == 0 )
					continue;
				const double k = std::hypot( 2.0 * kPi * mx / g.lx, 2.0 * kPi * my / g.ly );
				const double p = power[ static_cast< size_t >( y ) * g.nx + x ];
				total += p;
				if( std::fabs( k - centre ) > half )
					outside += p;
			}
		const double share = total > 0.0 ? outside / total : 1.0;
		const double rms   = rmsAmplitude( surface, kPi * rig.plugin.FaradayDriveHz() );
		Check( rms > 0.0 && share < 0.01,
		       fmt( "%-5s: %.3f%% of the seed's power is more than 3 sd from %.1f rad/m (bound 1%%: a Gaussian leaves 0.27%%); height %.2e m",
		            walls ? "Walls" : "Open", 100.0 * share, centre, rms ) );

		//The method of images: with Walls the grid is the pond and its mirror
		//images about the half cell, eta( N-1-i ) = eta( i ). The seed is the
		//only thing in the water here, so a seed that broke the mirror would
		//show at its full size. Float FFT round trips leave about 1e-6 a frame,
		//a random walk to about 1e-5 over 120: the bound is ten times that.
		if( walls )
		{
			double mismatch = 0.0, energy = 0.0;
			const int shift = perturb.seedMirror;
			for( int y = 0; y < g.ny; ++y )
				for( int x = 0; x < g.nx; ++x )
				{
					const double e  = surface[ ( static_cast< size_t >( y ) * g.nx + x ) * 4 ];
					const int mxi   = ( ( g.nx - shift - x ) % g.nx + g.nx ) % g.nx;
					const int myi   = ( ( g.ny - shift - y ) % g.ny + g.ny ) % g.ny;
					const double ex = surface[ ( static_cast< size_t >( y ) * g.nx + mxi ) * 4 ];
					const double ey = surface[ ( static_cast< size_t >( myi ) * g.nx + x ) * 4 ];
					mismatch += ( e - ex ) * ( e - ex ) + ( e - ey ) * ( e - ey );
					energy += 2.0 * e * e;
				}
			const double relative = energy > 0.0 ? std::sqrt( mismatch / energy ) : 1.0;
			Check( energy > 0.0 && relative < 1e-4,
			       fmt( "Walls: the seeded pond is its own mirror image to %.1e of its RMS (bound 1e-4)", relative ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --pattern
//===========================================================================
struct LatticeSignature
{
	double single = 0.0;///< the slope's orientation coherence in a window, mean over windows: 1 stripes, 0 a lattice
	double at60   = 0.0;///< the windows' angular autocorrelation at 60 degrees, the isotropic part removed
	double at90   = 0.0;///< and at 90
	double rms    = 0.0;
};

LatticeSignature signature( const Floats& s, const Grid& g, double kc, double omegaC, int window )
{
	//Windows over the frame -- the quadrant x < Nx/2, y < Ny/2, as Open
	//banks leave it -- Hann-weighted, half-overlapping.
	const int kWindow = window;
	constexpr int kBins = 36;
	LatticeSignature out;
	int windows = 0;
	for( int y0 = 0; y0 + kWindow <= g.ny / 2; y0 += kWindow / 2 )
		for( int x0 = 0; x0 + kWindow <= g.nx / 2; x0 += kWindow / 2 )
		{
			const std::vector< double > power = latticeSpectrum( s, g, x0, y0, kWindow, kWindow, true, omegaC );
			const double wx = g.lx / g.nx * kWindow, wy = g.ly / g.ny * kWindow;
			std::vector< double > h( kBins, 0.0 );
			for( int y = 0; y < kWindow; ++y )
				for( int x = 0; x < kWindow; ++x )
				{
					const int mx = x < kWindow / 2 ? x : x - kWindow, my = y < kWindow / 2 ? y : y - kWindow;
					const double kx = 2.0 * kPi * mx / wx, ky = 2.0 * kPi * my / wy;
					const double k  = std::hypot( kx, ky );
					if( k < 0.75 * kc || k > 1.25 * kc )
						continue;
					double angle = std::atan2( ky, kx );
					if( angle < 0.0 )
						angle += kPi;
					const int bin = std::min( kBins - 1, static_cast< int >( angle / kPi * kBins ) );
					h[ bin ] += power[ static_cast< size_t >( y ) * kWindow + x ];
				}
			double total = 0.0;
			int peak     = 0;
			for( int b = 0; b < kBins; ++b )
			{
				total += h[ b ];
				if( h[ b ] > h[ peak ] )
					peak = b;
			}
			if( total <= 0.0 )
				continue;
			//The slope's structure tensor, over each quarter of the window
			//(five wavelengths or so: a labyrinth's stripes bend over ten):
			//stripes have every gradient along one line (coherence 1); a
			//square or hexagonal lattice's point every way equally (0).
			const int half = kWindow / 2;
			double near    = 0.0;
			for( int qy = 0; qy < 2; ++qy )
				for( int qx = 0; qx < 2; ++qx )
				{
					double jxx = 0.0, jyy = 0.0, jxy = 0.0;
					for( int y = 0; y < half; ++y )
						for( int x = 0; x < half; ++x )
						{
							const double w = ( 0.5 - 0.5 * std::cos( 2.0 * kPi * ( x + 0.5 ) / half ) )
							                 * ( 0.5 - 0.5 * std::cos( 2.0 * kPi * ( y + 0.5 ) / half ) );
							const int cx = x0 + qx * half + x, cy = y0 + qy * half + y;
							const size_t i = ( static_cast< size_t >( cy ) * g.nx + cx ) * 4;
							jxx += w * s[ i + 2 ] * s[ i + 2 ];
							jyy += w * s[ i + 3 ] * s[ i + 3 ];
							jxy += w * s[ i + 2 ] * s[ i + 3 ];

							//And eta_t / omega_c's gradient, by central differences,
							//so a frame that caught eta passing through zero still
							//sees the lattice.
							auto rateAt = [ & ]( int px, int py ) {
								px = ( px + g.nx ) % g.nx;
								py = ( py + g.ny ) % g.ny;
								return kRateScale / omegaC * s[ ( static_cast< size_t >( py ) * g.nx + px ) * 4 + 1 ];
							};
							const double rx = ( rateAt( cx + 1, cy ) - rateAt( cx - 1, cy ) ) / ( 2.0 * g.lx / g.nx );
							const double ry = ( rateAt( cx, cy + 1 ) - rateAt( cx, cy - 1 ) ) / ( 2.0 * g.ly / g.ny );
							jxx += w * rx * rx;
							jyy += w * ry * ry;
							jxy += w * rx * ry;
						}
					near += 0.25 * ( jxx + jyy > 0.0 ? std::hypot( jxx - jyy, 2.0 * jxy ) / ( jxx + jyy ) : 0.0 );
				}
			(void)peak;

			const double mean = total / kBins;
			double self = 0.0, c60 = 0.0, c90 = 0.0;
			for( int b = 0; b < kBins; ++b )
			{
				const double a = h[ b ] - mean;
				self += a * a;
				c60 += a * ( h[ ( b + kBins / 3 ) % kBins ] - mean );
				c90 += a * ( h[ ( b + kBins / 2 ) % kBins ] - mean );
			}
			out.single += near;
			out.at60 += self > 0.0 ? c60 / self : 0.0;
			out.at90 += self > 0.0 ? c90 / self : 0.0;
			++windows;
		}
	if( windows > 0 )
	{
		out.single /= windows;
		out.at60 /= windows;
		out.at90 /= windows;
	}
	out.rms = rmsAmplitude( s, omegaC );
	return out;
}

int runPattern( const Perturb& perturb )
{
	std::printf( "\n=== pattern: Stripes, Squares and Hexagons stand up as themselves\n" );

	//Forty seconds of water at twice threshold: a lattice orders slowly
	//(at 14 s every Pattern was still mostly tangle and missed its test), and
	//Speed 4 at 30 fps covers it in 300 frames a pattern. Windows of 64
	//cells are ten wavelengths at 10 Hz on the 1.5 m pond.
	const double driveHz = 10.0, seconds = 40.0, over = 2.0, speed = 4.0, fps = 30.0;
	const int cells = 512, window = 64;

	const char* const names[] = { "Stripes", "Squares", "Hexagons" };
	for( int pattern = 0; pattern < 3; ++pattern )
	{
		Rig rig;
		rig.fps = fps;
		if( !rig.Init( g_checkWidth, g_checkHeight ) )
			return 1;
		ShakeSetup setup;
		setup.cells     = cells;
		setup.walls     = false;//mirror images would pair every stripe with its reflection
		setup.driveHz   = driveHz;
		setup.overdrive = over;
		setup.pattern   = pattern;
		setup.speed     = speed;
		applyShake( rig, setup );
		if( !rig.Render( static_cast< int >( seconds * rig.fps / setup.speed ) ) || !requireShaken( rig, names[ pattern ] ) )
			return 1;

		const double omegaC = kPi * rig.plugin.FaradayDriveHz();
		const double kc     = dispersionWavenumber( omegaC, rig.plugin.CurrentWater() );
		const LatticeSignature sig = signature( rig.Surface(), rig.plugin.CurrentGrid(), kc, omegaC, window );

		//Each test is the midpoint between the perfect lattice and none:
		//stripes' slopes all lie along one line (coherence 1), a square or
		//hexagonal lattice's point every way (0); a perfect square lattice has
		//angular autocorrelation 1 at 90 degrees, a hexagonal one 1 at 60,
		//and a tangle with no order 0 at both.
		const int expect = perturb.patternShift ? ( pattern + 1 ) % 3 : pattern;
		bool ok          = sig.rms > 1e-4;
		if( expect == 0 )
			ok = ok && sig.single > 0.5;
		else if( expect == 1 )
			ok = ok && sig.single < 0.5 && sig.at90 > 0.5 && sig.at90 > sig.at60;
		else
			ok = ok && sig.single < 0.5 && sig.at60 > 0.5 && sig.at60 > sig.at90;
		Check( ok, fmt( "%-8s (scored as %-8s): coherence %.2f, angular autocorrelation at 60 %+.2f, at 90 %+.2f; height %.2e m",
		                names[ pattern ], names[ expect ], sig.single, sig.at60, sig.at90, sig.rms ) );
	}
	return Verdict();
}

//===========================================================================
// --strobe
//===========================================================================
int runStrobe( const Perturb& perturb )
{
	std::printf( "\n=== strobe: Slow Motion shows the real-time water at the moments a stroboscope would\n" );

	const double driveHz = 10.0, slow = 8.0;
	const double omegaC  = kPi * driveHz;
	const double rates[] = { 60.0, 24.0 };
	for( const double fps : rates )
	{
		//B: the stroboscope. Seed off and one mode loaded, so a second run
		//can be held against it exactly.
		ShakeSetup setup;
		setup.cells     = 256;
		setup.driveHz   = driveHz;
		setup.overdrive = 0.0;
		setup.seed      = false;
		setup.slow      = slow;

		//The nominal clock is the host's times Speed -- the control's own
		//float mapping of 1, which is 1 to a few parts in 1e8.
		const double speed = SpeedFromParam( ParamFromSpeed( static_cast< float >( setup.speed ) ) );
		std::vector< double > steps, nominal, lags, projections;
		std::vector< double > feedTimes;
		Floats strobed;
		int mx = 0;
		{
			Rig rig;
			rig.fps = fps;
			if( !rig.Init( g_checkWidth, g_checkHeight ) )
				return 1;
			applyShake( rig, setup );
			if( !rig.Render( 1 ) )
				return 1;
			const Grid g = rig.plugin.CurrentGrid();
			mx = static_cast< int >( std::lround( dispersionWavenumber( omegaC, rig.plugin.CurrentWater() ) * g.lx / ( 2.0 * kPi ) ) );
			rig.plugin.LoadSurfaceForTest( modeState( g, mx, 0, 1e-9 ) );
			setup.overdrive = 2.0;
			applyShake( rig, setup );
			const int frames = static_cast< int >( 6.0 * fps );
			for( int f = 1; f <= frames; ++f )
			{
				if( !rig.Render( 1 ) || !requireShaken( rig, "strobe" ) )
					return 1;
				steps.push_back( rig.plugin.FaradayWaterStep() );
				nominal.push_back( f / fps * speed );
				lags.push_back( rig.plugin.FaradayLag() );
				projections.push_back( projectMode( rig.Surface(), g, mx, 0, 0 ) );
			}
			strobed = rig.Surface();
		}

		//The clock: never ahead of the nominal one, never a response period
		//behind it, and the lag is exactly the difference between the two.
		const double period = 2.0 * kPi / omegaC;
		double water = 0.0, worstBook = 0.0, maxLag = 0.0, minLag = 1e9;
		for( size_t i = 0; i < steps.size(); ++i )
		{
			water += steps[ i ];
			worstBook = std::max( worstBook, std::fabs( nominal[ i ] - water - lags[ i ] ) );
			maxLag    = std::max( maxLag, lags[ i ] );
			minLag    = std::min( minLag, lags[ i ] );
		}
		Check( minLag >= 0.0 && maxLag < period && worstBook < 1e-9,
		       fmt( "%2.0f fps: the water lags the clock by %.4f to %.4f s, inside one response period (%.4f s); "
		            "bookkeeping to %.1e s",
		            fps, minLag, maxLag, period, worstBook ) );

		//What the camera sees: the oscillation, S times slower. The water's
		//zeros are a drive period apart (eta( t + T ) = mu eta( t ), mu < 0),
		//so the strobed picture's are S drive periods apart in nominal time.
		//Each crossing is placed to within a frame; the mean spacing over
		//N - 1 gaps to within two frames over N - 1.
		std::vector< double > crossings;
		for( size_t i = 1; i < projections.size(); ++i )
			if( ( projections[ i ] > 0.0 ) != ( projections[ i - 1 ] > 0.0 ) )
			{
				const double a = projections[ i - 1 ], b = projections[ i ];
				crossings.push_back( nominal[ i - 1 ] + ( nominal[ i ] - nominal[ i - 1 ] ) * a / ( a - b ) );
			}
		const double expected = ( perturb.strobeAsClock ? 1.0 : slow ) * kPi / omegaC;
		const double spacing =
			crossings.size() >= 2 ? ( crossings.back() - crossings.front() ) / static_cast< double >( crossings.size() - 1 ) : 0.0;
		const double bound = crossings.size() >= 2 ? 2.0 / fps / static_cast< double >( crossings.size() - 1 ) : 0.0;
		Check( crossings.size() >= 4 && std::fabs( spacing - expected ) < bound,
		       fmt( "%2.0f fps: the picture's zeros are %.4f s apart over %zu crossings (expect %.4f, S = %.0f drive periods; "
		            "bound %.4f)",
		            fps, spacing, crossings.size(), expected, slow, bound ) );

		//C: no stroboscope, on a host clock that steps exactly as B's water
		//did. If Slow Motion only chooses moments of the same water, C ends
		//on B's surface. (A slower clock -- schlieren's Speed -- would not.)
		feedTimes.push_back( 0.0 );
		double t = 0.0;
		for( size_t i = 0; i < steps.size(); ++i )
		{
			t += perturb.strobeAsClock ? ( nominal[ i ] - ( i ? nominal[ i - 1 ] : 0.0 ) ) / slow : steps[ i ];
			feedTimes.push_back( t );
		}
		Floats plain;
		{
			Rig rig;
			rig.fps   = fps;
			rig.clock = &feedTimes;
			if( !rig.Init( g_checkWidth, g_checkHeight ) )
				return 1;
			setup.overdrive = 0.0;
			setup.slow      = 1.0;
			applyShake( rig, setup );
			if( !rig.Render( 1 ) )
				return 1;
			rig.plugin.LoadSurfaceForTest( modeState( rig.plugin.CurrentGrid(), mx, 0, 1e-9 ) );
			setup.overdrive = 2.0;
			applyShake( rig, setup );
			if( !rig.Render( static_cast< int >( steps.size() ) ) || !requireShaken( rig, "strobe" ) )
				return 1;
			plain = rig.Surface();
		}
		//The same water time in the same steps; the drive phases agree to
		//double rounding. 1e-4 is the Magnus difference between two substep
		//counts (< 6e-5 here) should a ceil() round the other way.
		const double diff = relativeDifference( strobed, plain );
		Check( diff < 1e-4, fmt( "%2.0f fps: the strobed water is the real-time water at the same moments: RMS difference "
		                         "%.1e (bound 1e-4)",
		                         fps, diff ) );
	}
	return Verdict();
}

//===========================================================================
// --clock
//===========================================================================
int runClock( const Perturb& perturb )
{
	std::printf( "\n=== clock: at Resolume's 4.99e8 ms the shaken water is the water at 0\n" );

	auto run = [ & ]( double offset, bool floatClock, Floats& out, double& phase ) {
		Rig rig;
		rig.clockOffset = offset;
		if( !rig.Init( g_checkWidth, g_checkHeight ) )
			return false;
		ShakeSetup setup;
		setup.cells     = 256;
		setup.walls     = false;
		setup.overdrive = 2.0;
		setup.pattern   = static_cast< int >( Pattern::Squares );
		applyShake( rig, setup );
		rig.plugin.SetFloatClockForTest( floatClock );
		if( !rig.Render( 120 ) || !requireShaken( rig, "clock" ) )
			return false;
		out   = rig.Surface();
		phase = rig.plugin.FaradayDrivePhase();
		return true;
	};
	Floats atZero, late;
	double phaseZero = 0.0, phaseLate = 0.0;
	if( !run( 0.0, false, atZero, phaseZero ) || !run( 499000.0, perturb.floatClock, late, phaseLate ) )
		return 1;

	//A double holds 499000 s to 6e-11 s, so each frame's dt is the same to
	//that, and the drive's phase to 4e-9 rad: nothing a float uniform can
	//see. 1e-4 of the RMS is room for a texel whose float rounds the other
	//way; the phase is compared to 1e-6 rad.
	const double diff = relativeDifference( atZero, late );
	Check( diff < 1e-4 && std::fabs( phaseZero - phaseLate ) < 1e-6,
	       fmt( "RMS difference %.1e (bound 1e-4); drive phase %.9f against %.9f rad", diff, phaseLate, phaseZero ) );
	return Verdict();
}

//===========================================================================
// --resize
//===========================================================================
int runResize( const Perturb& perturb )
{
	std::printf( "\n=== resize: a raster change mid-run keeps the shaken water\n" );

	const int w = g_checkWidth, h = g_checkHeight;
	const int w2 = w >= 640 ? w / 2 : w * 2, h2 = w >= 640 ? h / 2 : h * 2;
	auto run = [ & ]( bool resize, Floats& out, double& lag ) {
		Rig rig;
		if( !rig.Init( w, h ) )
			return false;
		ShakeSetup setup;
		setup.cells     = 256;
		setup.walls     = false;
		setup.overdrive = 2.0;
		setup.slow      = 4.0;//so the stroboscope's state is carried across too
		setup.pattern   = static_cast< int >( Pattern::Squares );
		applyShake( rig, setup );
		if( !rig.Render( 60 ) )
			return false;
		if( resize )
		{
			if( perturb.resizeStill )
				rig.Press( PT_STILL );
			if( !rig.Resize( w2, h2 ) )
				return false;
		}
		if( !rig.Render( 60 ) || !requireShaken( rig, "resize" ) )
			return false;
		out = rig.Surface();
		lag = rig.plugin.FaradayLag();
		return true;
	};
	Floats resized, kept;
	double lagResized = 0.0, lagKept = 0.0;
	if( !run( true, resized, lagResized ) || !run( false, kept, lagKept ) )
		return 1;

	//The same context, the same passes, the same inputs: the water never
	//reads the raster, so the two runs must agree bit for bit.
	size_t differ = 0;
	for( size_t i = 0; i < resized.size(); ++i )
		differ += std::memcmp( &resized[ i ], &kept[ i ], sizeof( float ) ) != 0;
	Check( differ == 0 && lagResized == lagKept,
	       fmt( "%dx%d to %dx%d after one second: %zu of %zu surface floats differ from a run that never resized; "
	            "strobe lag %.6f against %.6f",
	            w, h, w2, h2, differ, resized.size(), lagResized, lagKept ) );
	return Verdict();
}

//===========================================================================
// --faraday-off
//===========================================================================
int runFaradayOff( const Perturb& perturb )
{
	std::printf( "\n=== faraday-off: at Shake 0 the other five Faraday controls change nothing\n" );

	auto run = [ & ]( bool tweak, Floats& out, Floats& surface, bool& shaken ) {
		Rig rig;
		if( !rig.Init( g_checkWidth, g_checkHeight ) )
			return false;
		rig.feed = AudioFeed::Pulses;
		rig.Set( PT_RAIN, 0.45f );
		rig.Set( PT_AUDIO_PEBBLES, 0.5f );
		if( tweak )
		{
			rig.Set( PT_DRIVE, 1.0f );
			rig.Set( PT_DAMPING, 0.0f );
			rig.Set( PT_SLOW_MOTION, 1.0f );
			rig.Set( PT_PATTERN, 2.0f );
			rig.Set( PT_BASS_BINS, 1.0f );
			rig.Set( PT_SHAKE, perturb.offShake ? 1e-3f : 0.0f );
		}
		for( int f = 0; f < 90; ++f )
		{
			if( f == 10 )
				rig.Press( PT_DROP );
			if( f == 20 )
				rig.Press( PT_SKIM );
			if( !rig.Render( 1 ) )
				return false;
		}
		out     = rig.Output();
		surface = rig.Surface();
		shaken  = rig.plugin.FaradayActive();
		return true;
	};
	Floats outA, surfA, outB, surfB;
	bool shakenA = false, shakenB = false;
	if( !run( false, outA, surfA, shakenA ) || !run( true, outB, surfB, shakenB ) )
		return 1;

	//Same context: bit for bit, nothing less.
	size_t differ = 0;
	for( size_t i = 0; i < outA.size(); ++i )
		differ += std::memcmp( &outA[ i ], &outB[ i ], sizeof( float ) ) != 0;
	for( size_t i = 0; i < surfA.size(); ++i )
		differ += std::memcmp( &surfA[ i ], &surfB[ i ], sizeof( float ) ) != 0;
	Check( differ == 0 && !shakenA && !shakenB,
	       fmt( "rain, a drop, a skim and audio pebbles for 90 frames with Drive, Damping, Slow Motion, Pattern and Bass "
	            "Bins at their far ends: %zu of %zu output and surface floats differ%s",
	            differ, outA.size() + surfA.size(), shakenB ? " (and a frame ran shaken)" : "" ) );
	return Verdict();
}

//===========================================================================
// --rate
//===========================================================================
int runRate( const Perturb& perturb )
{
	std::printf( "\n=== rate: the shaken water is the same at 60 and 30 fps\n" );

	auto run = [ & ]( double fps, int frames, Floats& out, int& substeps ) {
		Rig rig;
		rig.fps = fps;
		if( !rig.Init( g_checkWidth, g_checkHeight ) )
			return false;
		ShakeSetup setup;
		setup.cells     = 256;
		setup.overdrive = 0.0;
		setup.seed      = false;
		applyShake( rig, setup );
		if( !rig.Render( 1 ) )
			return false;
		const Grid g = rig.plugin.CurrentGrid();
		const int mx = static_cast< int >(
			std::lround( dispersionWavenumber( kPi * setup.driveHz, rig.plugin.CurrentWater() ) * g.lx / ( 2.0 * kPi ) ) );
		Floats state = modeState( g, mx, 0, 1e-9 );
		const Floats other = modeState( g, mx - 4, 3, 6e-10 );
		for( size_t i = 0; i < state.size(); ++i )
			state[ i ] += other[ i ];
		rig.plugin.LoadSurfaceForTest( state );
		setup.overdrive = 2.0;
		applyShake( rig, setup );
		substeps = 0;
		for( int f = 0; f < frames; ++f )
		{
			if( !rig.Render( 1 ) || !requireShaken( rig, "rate" ) )
				return false;
			substeps += rig.plugin.FaradaySteps();
		}
		out = rig.Surface();
		return true;
	};
	Floats sixty, thirty;
	int stepsSixty = 0, stepsThirty = 0;
	if( !run( 60.0, 120 - perturb.rateShift, sixty, stepsSixty ) || !run( 30.0, 60, thirty, stepsThirty ) )
		return 1;

	//Each run is within the --mathieu budget of the true solution, so they
	//are within the sum of the two budgets of each other.
	const double bound = 2.0 * 1e-4 + 16.0 * 6e-8 * ( stepsSixty + stepsThirty );
	const double diff  = relativeDifference( sixty, thirty );
	Check( diff < bound, fmt( "two seconds of two shaken modes: RMS difference %.2e (bound %.1e; %d and %d substeps)", diff,
	                          bound, stepsSixty, stepsThirty ) );
	return Verdict();
}

//===========================================================================
// --faraday-negative: every Faraday check, against a wrong model, must fail.
// Kept apart from --negative so that one's output is v0.1.0's, unchanged.
//===========================================================================
int runFaradayNegative()
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
		p.mathieuForcing = 1.01;
		cases.push_back( { "mathieu", runMathieu, p, "expect the shake 1% stronger than the plugin used" } );
	}
	{
		Perturb p;
		p.onsetDamping = 1.05;
		cases.push_back( { "onset", runOnset, p, "expect the tongue of a mode damped 5% more" } );
	}
	{
		Perturb p;
		p.halfHarmonic = true;
		cases.push_back( { "half", runHalf, p, "expect the response at the drive frequency, not half" } );
	}
	{
		Perturb p;
		p.wavelengthOmega = 1.06;
		cases.push_back( { "wavelength", runWavelength, p, "expect the lattice of a drive 6% faster" } );
	}
	{
		Perturb p;
		p.patternShift = true;
		cases.push_back( { "pattern", runPattern, p, "score Stripes as Squares, Squares as Hexagons, Hexagons as Stripes" } );
	}
	{
		Perturb p;
		p.strobeAsClock = true;
		cases.push_back( { "strobe", runStrobe, p, "expect Slow Motion to be a slower clock, and the picture unslowed" } );
	}
	{
		Perturb p;
		p.floatClock = true;
		cases.push_back( { "clock", runClock, p, "the plugin takes the drive's phase from the host clock as a float" } );
	}
	{
		Perturb p;
		p.resizeStill = true;
		cases.push_back( { "resize", runResize, p, "the resize frame clears the water" } );
	}
	{
		Perturb p;
		p.offShake = true;
		cases.push_back( { "faraday-off", runFaradayOff, p, "Shake a hair above 0" } );
	}
	{
		Perturb p;
		p.rateShift = 1;
		cases.push_back( { "rate", runRate, p, "hold the 30 fps water against the 60 fps water a frame earlier" } );
	}
	{
		Perturb p;
		p.stateLeak = true;
		cases.push_back( { "state-shaken", runStateShaken, p, "the host's view after the frame has a texture left on unit 2" } );
	}
	{
		Perturb p;
		p.seedBand = 1.5;
		cases.push_back( { "seed (band)", runSeed, p, "expect the seed's band at 1.5 k_c" } );
	}
	{
		Perturb p;
		p.seedMirror = 0;
		cases.push_back( { "seed (mirror)", runSeed, p, "expect Walls' mirror about a cell, not the half cell" } );
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

	std::printf( "\nFaraday negative controls: %zu perturbations, %d of them undetected\n", cases.size(), unfalsifiable );
	std::printf( "\n  %s\n", unfalsifiable == 0 ? "PASS" : "FAIL" );
	return unfalsifiable == 0 ? 0 : 1;
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
             const std::vector< std::string >& settings, double fps )
{
	Rig rig;
	if( !rig.Init( width, height ) )
		return 1;
	//The clock the frames are stamped with: a take at 30 fps must run the
	//effect at 30 fps, or everything in it moves at twice the speed it
	//will play back at.
	rig.fps = fps;
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
	std::string surfacePath;
	std::vector< std::string > settings;
	int width  = 1280;
	int height = 720;
	int frames = 120;
	std::vector< int > dropFrames, skimFrames;
	bool beat  = false;
	bool drone = false;
	bool kick  = false;
	bool seed  = true;
	std::string mode;
	std::string scriptPath;
	int filmFrames = -1;
	double fps     = 60.0;

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
				"  --surface PATH    also write the grid: nx, ny (int32) then RGBA float32 a cell\n"
				"  --size WxH        render size (default 1280x720)\n"
				"  --frames N        frames of 60 fps water before reading back (default 120)\n"
				"  --drop N          press Drop on frame N. Repeatable.\n"
				"  --skim N          press Skim on frame N. Repeatable.\n"
				"  --beat            feed a beat every half second into the Audio buffer\n"
				"  --drone           feed a steady bass into the Audio buffer\n"
				"  --kick            feed a kick in the lowest two bins over a steady pad\n"
				"  --no-seed         shaken frames add no seed noise (the harness's hook)\n"
				"  --set \"Name=V\"    set a parameter by its display name. Repeatable.\n"
				"  --list            print every parameter and its default, then exit\n"
				"  --pipe            raw RGBA frames on stdin, raw RGBA frames on stdout\n"
				"  --film N          N frames of the card, raw RGBA frames on stdout\n"
				"  --script PATH     parameter cues for --pipe/--film: 'frame Name value'\n"
				"  --fps N           the clock for --pipe/--film (default 60)\n\n"
				"  --fft --modes --gravity --quiet --shallow --banks --refraction --fresnel\n"
				"  --caustics --still --skim-check --rain --audio --state --negative --bench\n\n"
				"  Faraday, at the raster --size gives (MPTEST_RENDERER=software for Apple's software renderer):\n"
				"  --mathieu --onset --half --wavelength --pattern --strobe --clock --resize --seed\n"
				"  --faraday-off --rate --state-shaken --faraday-negative\n" );
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--card" && hasNext )
			cardPath = argv[ ++i ];
		else if( argument == "--surface" && hasNext )
			surfacePath = argv[ ++i ];
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
		else if( argument == "--drone" )
			drone = true;
		else if( argument == "--kick" )
			kick = true;
		else if( argument == "--no-seed" )
			seed = false;
		else if( argument == "--pipe" )
			mode = "pipe";
		else if( argument == "--film" && hasNext )
		{
			mode       = "pipe";
			filmFrames = std::max( 1, std::atoi( argv[ ++i ] ) );
		}
		else if( argument == "--fps" && hasNext )
			fps = std::max( 1.0, std::atof( argv[ ++i ] ) );
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
		         || argument == "--rain" || argument == "--audio" || argument == "--negative" || argument == "--bench"
		         || argument == "--state" || argument == "--mathieu" || argument == "--onset" || argument == "--half"
		         || argument == "--wavelength" || argument == "--pattern" || argument == "--strobe" || argument == "--seed"
		         || argument == "--clock" || argument == "--resize" || argument == "--faraday-off"
		         || argument == "--rate" || argument == "--state-shaken" || argument == "--faraday-negative" )
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
		{ "audio", runAudio },       { "state", runState },
		{ "mathieu", runMathieu },   { "onset", runOnset },       { "half", runHalf },
		{ "wavelength", runWavelength }, { "pattern", runPattern }, { "strobe", runStrobe }, { "seed", runSeed },
		{ "clock", runClock },       { "resize", runResize },     { "faraday-off", runFaradayOff },
		{ "rate", runRate },         { "state-shaken", runStateShaken },
	};

	//The Faraday checks run at the raster --size gives.
	g_checkWidth  = width;
	g_checkHeight = height;

	bool ran = false;
	for( const auto& check : checks )
		if( mode == check.first )
		{
			result = check.second( none );
			ran    = true;
		}

	if( !ran && mode == "pipe" )
	{
		//A reader that stops early (`| head -c 1`) must end the run with a
		//failed write and exit 1, not kill it with SIGPIPE (141): the second
		//is indistinguishable from a crash to whatever is filming.
		std::signal( SIGPIPE, SIG_IGN );
		result = runPipe( width, height, scriptPath, filmFrames, beat, settings, fps );
	}
	else if( !ran && mode == "negative" )
		result = runNegative();
	else if( !ran && mode == "faraday-negative" )
		result = runFaradayNegative();
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
			if( drone )
				rig.feed = AudioFeed::Drone;
			if( kick )
				rig.feed = AudioFeed::Kick;
			rig.plugin.SetSeedForTest( seed );

			for( int f = 0; f < std::max( frames, 1 ) && result == 0; ++f )
			{
				if( std::find( dropFrames.begin(), dropFrames.end(), f ) != dropFrames.end() )
					rig.Press( PT_DROP );
				if( std::find( skimFrames.begin(), skimFrames.end(), f ) != skimFrames.end() )
					rig.Press( PT_SKIM );
				if( !rig.Render( 1 ) )
					result = 1;
			}

			if( result == 0 && !surfacePath.empty() )
			{
				//The grid's size, two int32s, then (eta, eta_t / kRateScale,
				//eta_x, eta_y) as float32 per cell, row 0 first.
				const Grid& g       = rig.plugin.CurrentGrid();
				const Floats state  = rig.Surface();
				const int32_t dims[ 2 ] = { g.nx, g.ny };
				std::ofstream file( surfacePath, std::ios::binary );
				file.write( reinterpret_cast< const char* >( dims ), sizeof( dims ) );
				file.write( reinterpret_cast< const char* >( state.data() ),
				            static_cast< std::streamsize >( state.size() * sizeof( float ) ) );
				if( !file )
				{
					std::fprintf( stderr, "could not write %s\n", surfacePath.c_str() );
					result = 1;
				}
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
