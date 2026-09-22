#include "Shaders.h"

namespace millpond
{

const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;

	//Straight through, in 0..1 picture space. MaxUV is applied where the
	//host's texture is actually read, in the composite, and nowhere else:
	//every other buffer here is one we allocated to the exact size.
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// Pass 1: inject. Last frame's state, the sponge, and this frame's craters.
//---------------------------------------------------------------------------
const char* const kInjectShader = R"(#version 410 core

uniform sampler2D StateTexture;//(eta, eta_t / RateScale, eta_x, eta_y), read texel for texel
uniform ivec2 GridSize;
uniform vec2 Domain;           //the simulated domain, metres: twice the frame
uniform vec2 Frame;            //the frame, metres
uniform int Walls;             //1: every crater gets its mirror images, no sponge
uniform float SpongeRate;      //1/s, at the far edge of the margin
uniform float Dt;              //seconds of water this frame
uniform int DropCount;
uniform vec4 Drops[ 64 ];      //x, y, radius, depth -- metres, frame coordinates

out vec4 fragColor;

//A crater with no net volume: the water it pushes down comes up as a rim.
//Its spectrum is k^2 exp( -k^2 a^2 / 4 ), which peaks at k = 2/a -- so the
//pebble's size is what picks the part of the dispersion curve it rings.
float crater( vec2 d, float radius, float depth )
{
	float q = dot( d, d ) / ( radius * radius );
	if( q > 30.0 )
		return 0.0;
	return -depth * ( 1.0 - q ) * exp( -q );
}

//The shortest way from one point to another on a torus.
vec2 wrapped( vec2 d )
{
	return d - Domain * floor( d / Domain + 0.5 );
}

void main()
{
	ivec2 cell = ivec2( gl_FragCoord.xy );
	vec4 state = texelFetch( StateTexture, cell, 0 );

	//Grid point i sits at (i + 1/2) cells, which is where GL puts texel i's
	//centre -- so the surface is read back at exactly the positions it was
	//computed at, and mirroring about x = 0 maps the grid onto itself.
	vec2 p = ( vec2( cell ) + 0.5 ) * Domain / vec2( GridSize );

	float eta  = state.x;
	float rate = state.y;

	if( Walls == 0 )
	{
		//Open water. The frame is the lower-left quadrant; the other three are
		//a margin that surrounds it on every side, because the domain wraps.
		//Distance into the margin, per axis, is to the nearer frame edge:
		//past W on the right, or past 2W (which is 0) on the left.
		vec2 outside = max( p - Frame, vec2( 0.0 ) );
		vec2 into    = min( outside, Domain - p );
		vec2 ramp    = clamp( into / ( 0.5 * Frame ), 0.0, 1.0 );
		float r      = max( ramp.x, ramp.y );

		//Quadratic, so the absorber has no edge for a wave to reflect off.
		float keep = exp( -SpongeRate * r * r * Dt );
		eta *= keep;
		rate *= keep;
	}

	for( int i = 0; i < DropCount; ++i )
	{
		vec4 drop = Drops[ i ];
		eta += crater( wrapped( p - drop.xy ), drop.z, drop.w );

		if( Walls != 0 )
		{
			//The method of images. On a domain that is the frame mirrored to
			//2W x 2H, a state that is even about x = 0 is also even about
			//x = W: both walls have no flow through them, to every order of
			//reflection, for as long as the pond runs.
			eta += crater( wrapped( p - vec2( -drop.x, drop.y ) ), drop.z, drop.w );
			eta += crater( wrapped( p - vec2( drop.x, -drop.y ) ), drop.z, drop.w );
			eta += crater( wrapped( p - vec2( -drop.x, -drop.y ) ), drop.z, drop.w );
		}
	}

	fragColor = vec4( eta, rate, 0.0, 0.0 );
}
)";

//---------------------------------------------------------------------------
// Passes 2 and 4: one Stockham radix-2 stage, along one axis, of TWO complex
// fields at once (.xy and .zw). Natural order in, natural order out, no bit
// reversal anywhere.
//
// Stage `Span` builds transforms of length Span out of pairs of length Span/2:
// output j takes its even half from j's own sub-transform and its odd half
// from the one Length/2 further on, twiddled by exp( +-2 pi i (j mod Span) /
// Span ). Written out for N = 4 in AGENTS.md; checked against a
// double-precision DFT by `mptest --fft`.
//---------------------------------------------------------------------------
const char* const kFFTShader = R"(#version 410 core

uniform sampler2D Source;
uniform sampler2D Twiddles;//exp( -2 pi i m / Length ), m = 0 .. Length-1, from doubles
uniform int Length;      //points along this axis
uniform int Span;        //the sub-transform length this stage produces: 2, 4 .. Length
uniform int Horizontal;  //1: along x
uniform float Direction; //-1 forward, +1 inverse

out vec4 fragColor;

vec2 times( vec2 a, vec2 b )
{
	return vec2( a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x );
}

void main()
{
	ivec2 cell = ivec2( gl_FragCoord.xy );
	int j      = Horizontal != 0 ? cell.x : cell.y;

	int halfSpan  = Span / 2;
	int evenIndex = ( j / Span ) * halfSpan + ( j % halfSpan );
	int oddIndex  = evenIndex + Length / 2;

	ivec2 evenCell = cell;
	ivec2 oddCell  = cell;
	if( Horizontal != 0 )
	{
		evenCell.x = evenIndex;
		oddCell.x  = oddIndex;
	}
	else
	{
		evenCell.y = evenIndex;
		oddCell.y  = oddIndex;
	}

	vec4 even = texelFetch( Source, evenCell, 0 );
	vec4 odd  = texelFetch( Source, oddCell, 0 );

	//The twiddle, from a table the CPU filled in double precision. Computing
	//it here with cos and sin costs a few units in the last place, and the
	//SAME few units every frame -- a systematic error, so it accumulates
	//coherently in a state that is fed back sixty times a second. Measured
	//on the walled pond: 7e-4 of mirror asymmetry after ten seconds with
	//cos and sin, against the table's figure in AGENTS.md.
	vec2 twiddle = texelFetch( Twiddles, ivec2( ( j % Span ) * ( Length / Span ), 0 ), 0 ).xy;
	if( Direction > 0.0 )
		twiddle.y = -twiddle.y;

	fragColor = even + vec4( times( odd.xy, twiddle ), times( odd.zw, twiddle ) );
}
)";

//---------------------------------------------------------------------------
// Pass 3: evolve. Every mode, advanced exactly.
//---------------------------------------------------------------------------
const char* const kEvolveShader = R"(#version 410 core

uniform sampler2D Spectrum;//Z = FFT( eta + i eta_t )
uniform ivec2 GridSize;
uniform vec2 Domain;
uniform float Gravity;     //m/s^2
uniform float Tension;     //surface tension over density, m^3/s^2
uniform float Depth;       //m
uniform float Viscosity;   //m^2/s
uniform float Dt;          //s
uniform float Norm;        //1 / ( Nx Ny ), the inverse transform's scale, applied here once
uniform float RateScale;   //eta_t travels as eta_t / RateScale -- see Shaders.h

out vec4 fragColor;

void main()
{
	ivec2 cell = ivec2( gl_FragCoord.xy );

	//The Nyquist row and column. A real field's spectrum is Hermitian, and at
	//Nyquist that forces a derivative to be zero -- i k A at k = -k is not the
	//conjugate of itself. Left in, it leaks into the imaginary half of the
	//packed pair and the gradients come back with a grid-scale checkerboard.
	//Nothing physical lives on a two-cell wave anyway.
	if( cell.x == GridSize.x / 2 || cell.y == GridSize.y / 2 )
	{
		fragColor = vec4( 0.0 );
		return;
	}

	//Unpack the two real spectra from the one complex one:
	//A = ( Z(k) + conj Z(-k) ) / 2 is eta's, B = ( Z(k) - conj Z(-k) ) / 2i is eta_t's.
	ivec2 opposite = ivec2( ( GridSize.x - cell.x ) % GridSize.x, ( GridSize.y - cell.y ) % GridSize.y );
	vec2 z  = texelFetch( Spectrum, cell, 0 ).xy;
	vec2 zm = texelFetch( Spectrum, opposite, 0 ).xy;
	zm.y    = -zm.y;

	vec2 A    = 0.5 * ( z + zm );
	vec2 diff = 0.5 * ( z - zm );
	vec2 B    = RateScale * vec2( diff.y, -diff.x );

	ivec2 m = ivec2( cell.x < GridSize.x / 2 ? cell.x : cell.x - GridSize.x,
	                 cell.y < GridSize.y / 2 ? cell.y : cell.y - GridSize.y );
	vec2 kv = 6.28318530717958648 * vec2( m ) / Domain;
	float k = length( kv );

	float restoring   = Gravity * k + Tension * k * k * k;//= mirrored
	float depthFactor = tanh( min( k * Depth, 20.0 ) );   //= mirrored
	float omega       = sqrt( max( restoring * depthFactor, 0.0 ) );//= mirrored
	float gamma       = 2.0 * Viscosity * k * k;          //= mirrored

	//The exact step of  y'' + 2 gamma y' + omega^2 y = 0  over Dt:
	//    y(Dt)  = C y + S ( y' + gamma y )
	//    y'(Dt) = C y' - S ( omega^2 y + gamma y' )
	//with C, S from whichever side of critical damping the mode is on. Syrup
	//really is overdamped at short wavelengths; water never is.
	float C;
	float S;
	float disc = omega * omega - gamma * gamma;
	if( disc > 0.0 )
	{
		float wd    = sqrt( disc );
		float decay = exp( -gamma * Dt );
		C           = decay * cos( wd * Dt );
		S           = decay * sin( wd * Dt ) / wd;
	}
	else
	{
		float wd = sqrt( -disc );
		if( wd * Dt < 1e-4 )
		{
			float decay = exp( -gamma * Dt );
			C           = decay;
			S           = decay * Dt;
		}
		else
		{
			//cosh and sinh times the decay, written so that neither half
			//overflows before the other can cancel it.
			float slow = exp( ( wd - gamma ) * Dt );
			float fast = exp( -( wd + gamma ) * Dt );
			C          = 0.5 * ( slow + fast );
			S          = 0.5 * ( slow - fast ) / wd;
		}
	}

	vec2 A2 = C * A + S * ( B + gamma * A );
	vec2 B2 = ( C * B - S * ( omega * omega * A + gamma * B ) ) / RateScale;

	//Pack for the way back. ( A2 + i B2 ) returns ( eta, eta_t ); the
	//derivatives are i kx A2 and i ky A2, packed as ( i kx A2 ) + i ( i ky A2 ).
	vec2 state = vec2( A2.x - B2.y, A2.y + B2.x );
	vec2 slope = vec2( -kv.x * A2.y, kv.x * A2.x ) - kv.y * A2;

	fragColor = Norm * vec4( state, slope );
}
)";

//---------------------------------------------------------------------------
// Pass 5: the caustic mesh.
//---------------------------------------------------------------------------
const char* const kCausticVertexShader = R"(#version 410 core

//No vertex attributes at all. The mesh is a grid of Columns x Rows cells over
//the frame and a margin round it, two triangles a cell, three vertices a
//triangle, none shared -- and gl_VertexID says which cell, which triangle and
//which corner. Every vertex works out all three corners of its own triangle,
//because the light it carries is a property of the triangle, not the vertex.
//
//Not shared vertices with a screen-space derivative in the fragment shader:
//that lost up to 5% of the light where the water folds, because a fold's
//middle layer lands as slivers a tenth of a pixel wide and a derivative taken
//across a sliver comes from helper invocations a whole pixel away. And not a
//geometry shader: on this machine's GL the vertex stage's texture fetches
//return zero when one is attached. Both are in AGENTS.md.

uniform sampler2D SurfaceTexture;
uniform vec2 FrameToTexture;//frame 0..1 to surface texture coordinates
uniform vec2 Frame;         //metres
uniform float Depth;        //metres
uniform float Eta;          //1 / n
uniform vec3 SunTravel;     //the direction sunlight travels, in air, unit
uniform vec2 FlatShift;     //how far flat water moves the light, metres
uniform vec2 BufferSize;    //the caustic buffer, pixels
uniform ivec2 Cells;        //the mesh: columns, rows
uniform float Margin;       //how far past the frame it reaches, as a fraction of it

flat out float light;

//Where the light entering at frame point `at` lands, as a shift in the
//buffer's pixels. On still water it is exactly zero.
vec2 shiftAt( vec2 at )
{
	vec4 surface = textureLod( SurfaceTexture, at * FrameToTexture, 0.0 );

	//The surface normal of z = eta( x, y ).
	vec3 normal = normalize( vec3( -surface.z, -surface.w, 1.0 ) );
	vec3 inside = refract( SunTravel, normal, Eta );

	//Down to the bed, which is Depth below the MEAN surface -- so a crest
	//adds to the path and a trough takes from it.
	float travel = max( Depth + surface.x, 1e-5 ) / max( -inside.z, 1e-3 );
	return ( inside.xy * travel - FlatShift ) / Frame * BufferSize;
}

uint mixBits( uint x )
{
	//lowbias32. Integer, so every machine jitters the mesh the same way.
	x ^= x >> 16u;
	x *= 0x7feb352du;
	x ^= x >> 15u;
	x *= 0x846ca68bu;
	x ^= x >> 16u;
	return x;
}

//A mesh corner, jittered by up to 0.3 of a cell in each axis, the same way for
//every triangle that shares it -- so the mesh stays watertight.
//
//The jitter is what makes the deposit unbiased. A triangle hands its light to
//the pixel centres it covers, which is right ON AVERAGE over where the
//triangle sits against the pixel grid. A regular mesh whose pitch is a
//rational multiple of the pixel pitch sits at the same few phases everywhere,
//so the average is never taken: at 1.5 pixels a cell the rain case lost 1.8%
//of its light, at 1.876 it lost 0.02%. Jitter takes the average at any pitch.
//It changes nothing on still water, where every triangle lands on itself.
vec2 cornerAt( ivec2 corner )
{
	uint h     = mixBits( uint( corner.x ) * 0x9e3779b9u ^ mixBits( uint( corner.y ) + 0x632be5abu ) );
	vec2 jiggle = vec2( float( h & 0xffffu ), float( h >> 16u ) ) / 65535.0 - 0.5;
	//The outer ring of corners stays put, so the mesh covers exactly what
	//it says it covers.
	if( corner.x == 0 || corner.y == 0 || corner.x == Cells.x || corner.y == Cells.y )
		jiggle = vec2( 0.0 );
	return vec2( -Margin ) + ( 1.0 + 2.0 * Margin ) * ( vec2( corner ) + 0.6 * jiggle ) / vec2( Cells );
}

void main()
{
	int cell     = gl_VertexID / 6;
	int local    = gl_VertexID - cell * 6;
	ivec2 base   = ivec2( cell % Cells.x, cell / Cells.x );

	//Two triangles a cell: (0,0) (1,0) (1,1), and (0,0) (1,1) (0,1).
	ivec2 c0 = base;
	ivec2 c1 = local < 3 ? base + ivec2( 1, 0 ) : base + ivec2( 1, 1 );
	ivec2 c2 = local < 3 ? base + ivec2( 1, 1 ) : base + ivec2( 0, 1 );

	vec2 p0 = cornerAt( c0 ), p1 = cornerAt( c1 ), p2 = cornerAt( c2 );
	vec2 s0 = shiftAt( p0 ), s1 = shiftAt( p1 ), s2 = shiftAt( p2 );

	//The triangle's area before and after, from its edges. Built from the
	//corners in the same order in all three vertices, so all three compute
	//the same number. The landed edges are the original edge plus the
	//difference of two small shifts -- exact to far more places than a
	//difference of two landed positions in the thousands of pixels, and on
	//still water bit-for-bit the original.
	vec2 a = ( p1 - p0 ) * BufferSize;
	vec2 b = ( p2 - p0 ) * BufferSize;
	vec2 c = a + ( s1 - s0 );
	vec2 d = b + ( s2 - s0 );
	float before = abs( a.x * b.y - a.y * b.x );
	float after  = abs( c.x * d.y - c.y * d.x );

	//Light in over light out. A triangle folded flat onto a line covers no
	//pixel centre; one that nearly is would put all its light on one pixel,
	//so it is sent off screen instead. The odds it covered a centre were its
	//area, a millionth of a pixel, and that is the light this throws away.
	light = after > 1e-6 * before ? before / after : 0.0;

	int corner = local - ( local < 3 ? 0 : 3 );
	vec2 here  = corner == 0 ? p0 : ( corner == 1 ? p1 : p2 );
	vec2 moved = corner == 0 ? s0 : ( corner == 1 ? s1 : s2 );
	gl_Position = after > 1e-6 * before ? vec4( 2.0 * ( here + moved / BufferSize ) - 1.0, 0.0, 1.0 )
	                                    : vec4( 2.0, 2.0, 2.0, 1.0 );
}
)";

const char* const kCausticFragmentShader = R"(#version 410 core

flat in float light;
out vec4 fragColor;

void main()
{
	//Every pixel centre the landed triangle covers receives the triangle's
	//concentration. Summed over those centres that is, on average, exactly
	//the light the triangle carried: its landed area times its own area over
	//its landed area.
	fragColor = vec4( light, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 6: the sun's size. A normalised Gaussian moves light, never makes it.
//---------------------------------------------------------------------------
const char* const kBlurShader = R"(#version 410 core

uniform sampler2D Source;
uniform vec2 Direction;//one pixel along the axis, in texture coordinates
uniform float Sigma;   //pixels
uniform float Step;    //pixels between taps
uniform int Taps;      //each side

in vec2 uv;
out vec4 fragColor;

void main()
{
	float total  = 0.0;
	float weight = 0.0;
	for( int i = -Taps; i <= Taps; ++i )
	{
		float x = float( i ) * Step;
		float w = exp( -0.5 * x * x / ( Sigma * Sigma ) );
		total += w * texture( Source, uv + Direction * x ).r;
		weight += w;
	}
	fragColor = vec4( total / weight, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 7: composite. Looking straight down through the water.
//---------------------------------------------------------------------------
const char* const kCompositeShader = R"(#version 410 core

uniform sampler2D InputTexture;
uniform vec2 MaxUV;
uniform vec2 HalfTexel;

uniform sampler2D SurfaceTexture;
uniform sampler2D CausticTexture;

uniform vec2 FrameToTexture;
uniform vec2 Frame;
uniform float Depth;
uniform float Eta;

uniform int UseCaustics;
uniform float CausticAmount;
uniform float Reflection;
uniform vec3 Sky;
uniform vec3 SunTravel;
uniform float Glint;
uniform float SunCosInner;
uniform float SunCosOuter;

uniform int View;
uniform float HeightGain;
uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

//Unpolarised Fresnel reflectance, air to water, exact. 0.0204 looking
//straight down; all of it at grazing.
float fresnel( float cosIncident )
{
	float n        = 1.0 / Eta;
	float ci       = clamp( cosIncident, 0.0, 1.0 );
	float sinT2    = ( 1.0 - ci * ci ) * Eta * Eta;
	float ct       = sqrt( max( 1.0 - sinT2, 0.0 ) );
	float rs       = ( ci - n * ct ) / ( ci + n * ct );
	float rp       = ( n * ci - ct ) / ( n * ci + ct );
	return 0.5 * ( rs * rs + rp * rp );
}

vec4 picture( vec2 at )
{
	return texture( InputTexture, clamp( at, HalfTexel, vec2( 1.0 ) - HalfTexel ) * MaxUV );
}

void main()
{
	vec4 surface = texture( SurfaceTexture, uv * FrameToTexture );
	vec3 normal  = normalize( vec3( -surface.z, -surface.w, 1.0 ) );

	//The camera looks straight down. Its ray bends at the surface and runs on
	//to the bed; the picture is whatever it lands on.
	vec3 view    = vec3( 0.0, 0.0, -1.0 );
	vec3 inside  = refract( view, normal, Eta );
	float travel = max( Depth + surface.x, 1e-5 ) / max( -inside.z, 1e-3 );
	vec2 bedAt   = ( uv * Frame + inside.xy * travel ) / Frame;

	vec4 original = picture( uv );
	vec4 bed      = picture( bedAt );

	float light = UseCaustics != 0 ? texture( CausticTexture, bedAt ).r : 1.0;
	float lit   = max( 1.0 + CausticAmount * ( light - 1.0 ), 0.0 );

	//What bounces off the top: the sky, and the sun where the reflected ray
	//finds it. Scaled by the bed's alpha so the effect adds nothing where the
	//picture has nothing.
	float R        = min( fresnel( normal.z ) * Reflection, 1.0 );
	vec3 reflected = reflect( view, normal );
	float toSun    = dot( reflected, -SunTravel );
	float disc     = smoothstep( SunCosOuter, SunCosInner, toSun );
	vec3 above     = ( Sky + vec3( Glint * disc ) ) * bed.a;

	vec4 result = vec4( ( 1.0 - R ) * bed.rgb * lit + R * above, bed.a );

	if( View == 1 )
		result = vec4( vec3( 0.5 + HeightGain * surface.x ), 1.0 );
	else if( View == 2 )
		result = vec4( 0.5 + 0.5 * normal, 1.0 );
	else if( View == 3 )
		result = vec4( vec3( 0.5 * light ), 1.0 );

	fragColor = mix( original, result, MixAmount );
}
)";

} // namespace millpond
