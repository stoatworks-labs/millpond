/**
 * Millpond — browser demo.
 *
 * The eight shaders below are copied unedited from `source/Shaders.cpp`:
 * the crater injection, the Stockham FFT stage (the same shader both ways and
 * along both axes), the exact per-mode propagator with the capillary-gravity
 * dispersion relation and Lamb's damping, the caustic mesh and its blur, and
 * the composite with Snell, Fresnel and the glints. So the water on this page
 * is the plugin's water: every frame the GPU transforms the surface, advances
 * every mode as a damped oscillator, and transforms back.
 * `demo/tools/check_shaders.py` proves the text character for character and
 * `tools/verify.sh` runs it.
 *
 * What is a hand port, and is checked by nothing but a reader: `Controls.cpp`
 * (every 0..1 conversion), the CPU half of `Physics.cpp` (`Omega` and
 * `GroupVelocity` for the sponge's rate, `SkimSchedule`, `SkimStart`, the PCG
 * `Random` and its Poisson draw, `ChooseGrid`), `CollectImpacts`, `Simulate`,
 * `Transform`, `Caustics` and the frame sequence of `ProcessOpenGL`. The
 * parameter declarations come from the constructor in `Millpond.cpp`.
 *
 * **Drop, Skim and Still are buttons.** The plugin declares them as
 * FF_TYPE_EVENT and acts on the rising edge. The kit's parameter model has no
 * event type, so — as readout's Fire does — each is a boolean here that the
 * renderer takes and releases in the same frame, which is what a host does
 * with an event parameter anyway. That is why the button blinks.
 *
 * **Audio is absent.** Audio, Audio Pebbles and Audio Rain read Resolume's FFT
 * buffer; a browser has none, and asking for a microphone to demonstrate a
 * video effect is not a trade worth making. They are left off the panel rather
 * than shown dead, and the removal is exact: with no spectrum the plugin's
 * analyser reports a level of 0 and never fires, so the page drops the same
 * pebbles and the same rain the plugin would.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, GLError, bindTexture } from './vendor/gl.js';

//===========================================================================
// The shaders. Copied from source/Shaders.cpp. Do not edit here.
//===========================================================================

const VERTEX = `#version 410 core

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
`;

const INJECT = `#version 410 core

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
`;

const FFT = `#version 410 core

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
`;

const EVOLVE = `#version 410 core

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
`;

const CAUSTIC_VERTEX = `#version 410 core

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

//Where the light entering at frame point \`at\` lands, as a shift in the
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
`;

const CAUSTIC_FRAGMENT = `#version 410 core

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
`;

const BLUR = `#version 410 core

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
`;

const COMPOSITE = `#version 410 core

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
uniform float SunRadius;   //radians

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
	float toSun    = acos( clamp( dot( reflected, -SunTravel ), -1.0, 1.0 ) );

	//The sun's disc, seen in the water. A pixel does not reflect one
	//direction but the spread of them its patch of surface faces -- fwidth of
	//the reflected ray -- and a disc smaller than that spread would be hit by
	//pixel centres only by luck: the glints were a line too thin to land on
	//any. So the disc is widened to the pixel's spread and dimmed by the same
	//area ratio, which keeps each glint's energy what the sun would give it.
	float spread = length( fwidth( reflected ) );
	float seen   = sqrt( SunRadius * SunRadius + spread * spread );
	float disc   = ( 1.0 - smoothstep( 0.6 * seen, 1.4 * seen, toSun ) ) * ( SunRadius * SunRadius ) / ( seen * seen );
	vec3 above   = ( Sky + vec3( Glint * disc ) ) * bed.a;

	vec4 result = vec4( ( 1.0 - R ) * bed.rgb * lit + R * above, bed.a );

	if( View == 1 )
		result = vec4( vec3( 0.5 + HeightGain * surface.x ), 1.0 );
	else if( View == 2 )
		result = vec4( 0.5 + 0.5 * normal, 1.0 );
	else if( View == 3 )
		result = vec4( vec3( 0.5 * light ), 1.0 );

	fragColor = mix( original, result, MixAmount );
}
`;

//===========================================================================
// Controls.h / Controls.cpp, ported.
//===========================================================================

const f32 = Math.fround;
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const geometric = (v, lo, hi) => f32(lo * Math.pow(hi / lo, clamp(v, 0, 1)));
const linear = (v, lo, hi) => f32(lo + (hi - lo) * clamp(v, 0, 1));

const K_SPEED_LOW = 0.05;
const K_SPEED_HIGH = 4.0;

const pondSizeFromParam = (v) => geometric(v, 0.1, 20.0);
const depthFromParam = (v) => geometric(v, 0.002, 2.0);
const K_WATER_TENSION = f32(7.28e-5);
const tensionFromParam = (v) => linear(v, 0.0, 2.0 * K_WATER_TENSION);
const viscosityFromParam = (v) => geometric(v, 1e-7, 1e-2);
const speedFromParam = (v) => (v <= 0 ? 0 : geometric(v, K_SPEED_LOW, K_SPEED_HIGH));
function paramFromSpeed(speed) {
  if (speed <= 0) return 0;
  const clamped = clamp(speed, K_SPEED_LOW, K_SPEED_HIGH);
  return f32(Math.log(clamped / K_SPEED_LOW) / Math.log(K_SPEED_HIGH / K_SPEED_LOW));
}
const pebbleSizeFromParam = (v) => geometric(v, 0.002, 0.10);
const splashFromParam = (v) => linear(v, 0.0, 0.6);
const scatterFromParam = (v) => linear(v, 0.0, 0.5);
const headingFromParam = (v) => linear(v, 0.0, 360.0);
const throwSpeedFromParam = (v) => geometric(v, 0.5, 20.0);
const skimAngleFromParam = (v) => linear(v, 3.0, 40.0);
const bouncesFromParam = (v) => clamp(Math.round(linear(v, 1.0, 40.0)), 1, 40);
const rainFromParam = (v) => { const c = clamp(v, 0, 1); return f32(80.0 * c * c * c); };
const rainSizeFromParam = (v) => geometric(v, 0.001, 0.02);
const causticsFromParam = (v) => linear(v, 0.0, 2.0);
const sunElevationFromParam = (v) => linear(v, 10.0, 90.0);
const sunAzimuthFromParam = (v) => linear(v, 0.0, 360.0);
const sunSizeFromParam = (v) => geometric(v, 0.1, 10.0);
const reflectionFromParam = (v) => linear(v, 0.0, 3.0);
const glintFromParam = (v) => { const c = clamp(v, 0, 1); return f32(5000.0 * c * c * c); };

const K_GRAVITY = f32(9.81);
const K_RAIN_SPLASH = f32(0.3);
const K_WATER_INDEX = f32(1.333);

/// Shaders.h.
const K_RATE_SCALE = 40.0;
const K_MAX_DROPS_PER_FRAME = 64;
const K_MAX_BLUR_TAPS = 24;

/// Controls.h: the grid along the long side of the mirrored domain.
const DETAIL_CELLS = [256, 512, 1024, 2048];
const BANKS_WALLS = 1;
const VIEW_CAUSTICS = 3;

/// Millpond.cpp's constants.
const K_MAX_FRAME_DELTA = 0.25;
const K_MESH_MARGIN = f32(0.1);
const K_PI = Math.PI;

//===========================================================================
// Physics.cpp, ported: the CPU half.
//===========================================================================

function omega(k, water) {
  const kk = Math.max(k, 0);
  const restoring = water.gravity * kk + water.tension * kk * kk * kk;
  const depthFactor = Math.tanh(Math.min(kk * water.depth, 20.0));
  return Math.sqrt(Math.max(restoring * depthFactor, 0));
}

function groupVelocity(k, water) {
  const step = Math.max(1e-6 * k, 1e-6);
  return (omega(k + step, water) - omega(Math.max(k - step, 0), water))
    / (k + step - Math.max(k - step, 0));
}

/// `SkimSchedule`: each bounce costs the same energy, so the hops shorten
/// linearly and quicken as a square root; the Nth touch is the sink.
function skimSchedule(launchTime, startX, startY, headingDegrees, speed, angleDegrees, bounces, stoneRadius, splash, gravity) {
  const impacts = [];
  const n = Math.max(bounces, 1);
  const heading = headingDegrees * K_PI / 180;
  const dx = Math.cos(heading);
  const dy = Math.sin(heading);
  const beta = clamp(angleDegrees, 0.5, 89.0) * K_PI / 180;
  const v0 = Math.max(speed, 0);

  let t = launchTime;
  let x = startX;
  let y = startY;
  for (let i = 0; i <= n; i += 1) {
    const left = Math.max(1 - i / n, 0);
    const vi = v0 * Math.sqrt(left);
    const sink = i === n;
    const radius = sink ? stoneRadius : f32(0.75 * stoneRadius);
    impacts.push({
      time: t,
      x: f32(x),
      y: f32(y),
      radius,
      amplitude: f32(splash * radius * (sink ? 1 : Math.sqrt(left))),
    });
    if (sink) break;
    const hop = 2 * vi * vi * Math.sin(beta) * Math.cos(beta) / gravity;
    const flight = 2 * vi * Math.sin(beta) / gravity;
    x += hop * dx;
    y += hop * dy;
    t += flight;
  }
  return impacts;
}

/// `SkimStart`: walk back from the aim point along the heading to the bank.
function skimStart(px, py, headingDegrees, width, height, inset) {
  const heading = headingDegrees * K_PI / 180;
  const dx = Math.cos(heading);
  const dy = Math.sin(heading);
  let back = 1e30;
  if (dx > 1e-6) back = Math.min(back, (px - inset) / dx);
  if (dx < -1e-6) back = Math.min(back, (width - inset - px) / -dx);
  if (dy > 1e-6) back = Math.min(back, (py - inset) / dy);
  if (dy < -1e-6) back = Math.min(back, (height - inset - py) / -dy);
  back = Math.max(back, 0);
  return [f32(px - back * dx), f32(py - back * dy)];
}

/// `Random`: PCG-XSH-RR on a 64-bit state, in BigInt so the rain falls where
/// the plugin's would from the same seed.
const MASK64 = (1n << 64n) - 1n;
class Random {
  constructor(seed = 0x853c49e6748fea9bn) {
    this.state = seed & MASK64;
  }

  next() {
    const old = this.state;
    this.state = (old * 6364136223846793005n + 1442695040888963407n) & MASK64;
    const xorshifted = Number((((old >> 18n) ^ old) >> 27n) & 0xffffffffn);
    const rot = Number(old >> 59n);
    return ((xorshifted >>> rot) | (xorshifted << ((-rot) & 31))) >>> 0;
  }

  uniform() {
    return this.next() / 4294967296;
  }

  poisson(mean) {
    if (mean <= 0) return 0;
    if (mean > 30) {
      const u1 = Math.max(this.uniform(), 1e-12);
      const u2 = this.uniform();
      const z = Math.sqrt(-2 * Math.log(u1)) * Math.cos(2 * K_PI * u2);
      return Math.max(0, Math.round(mean + Math.sqrt(mean) * z));
    }
    const limit = Math.exp(-mean);
    let product = this.uniform();
    let count = 0;
    while (product > limit) {
      count += 1;
      product *= this.uniform();
    }
    return count;
  }
}

/// `ChooseGrid`: the frame mirrored to 2W x 2H, `longCells` along its longer
/// side, and the power of two that makes the cells closest to square.
function chooseGrid(frameWidth, frameHeight, longCells) {
  const lx = 2 * Math.max(frameWidth, f32(1e-4));
  const ly = 2 * Math.max(frameHeight, f32(1e-4));
  const wide = lx >= ly;
  const ratio = wide ? ly / lx : lx / ly;
  const shortCells = Math.max(8, Math.round(Math.pow(2, Math.round(Math.log2(longCells * ratio)))));
  return { nx: wide ? longCells : shortCells, ny: wide ? shortCells : longCells, lx, ly };
}

//===========================================================================
// The plugin's frame, in the order ProcessOpenGL runs it.
//===========================================================================

class MillpondRenderer {
  constructor(gl, quad) {
    this.gl = gl;
    this.quad = quad;

    // The surface is RGBA32F read with a LINEAR filter (the composite and the
    // caustic mesh both interpolate it), and so is the R32F caustic deposit.
    // In WebGL2 a float texture with a linear filter and no
    // OES_texture_float_linear is incomplete and samples as black: the water
    // would be flat and the light would be gone, which is a plausible picture
    // of a still pond rather than an error. So its absence stops the page.
    if (!gl.getExtension('OES_texture_float_linear')) {
      throw new GLError('OES_texture_float_linear is missing. The water surface and the caustics are 32-bit float textures read with a linear filter, as in the plugin; without the extension they would sample as black and the page would show a still pond instead of an error.');
    }

    this.inject = new Program(gl, VERTEX, INJECT, 'inject');
    this.fft = new Program(gl, VERTEX, FFT, 'fft');
    this.evolve = new Program(gl, VERTEX, EVOLVE, 'evolve');
    // No vertex attributes: the mesh comes from gl_VertexID alone.
    this.caustic = new Program(gl, CAUSTIC_VERTEX, CAUSTIC_FRAGMENT, 'caustic', { attribs: {} });
    this.blur = new Program(gl, VERTEX, BLUR, 'blur');
    this.composite = new Program(gl, VERTEX, COMPOSITE, 'composite');

    this.spectrum = [new PassBuffer(gl, { filter: 'nearest' }), new PassBuffer(gl, { filter: 'nearest' })];
    this.surface = [new PassBuffer(gl, { filter: 'linear' }), new PassBuffer(gl, { filter: 'linear' })];
    this.causticBuffer = [new PassBuffer(gl, { filter: 'linear' }), new PassBuffer(gl, { filter: 'linear' })];
    this.twiddleX = null;
    this.twiddleY = null;
    this.meshVAO = gl.createVertexArray();

    this.surfaceIndex = 0;
    this.causticIndex = 0;
    this.haveCaustics = false;
    this.grid = { nx: 0, ny: 0, lx: 0, ly: 0 };
    this.bufferBanks = -1;
    this.causticWidth = 0;
    this.causticHeight = 0;
    this.meshColumns = 0;
    this.meshRows = 0;

    this.frameWidth = 1;
    this.frameHeight = 1;

    this.lastNow = -1;
    this.simTime = 0;

    this.dropPresses = 0;
    this.skimPresses = 0;
    this.stillWanted = false;
    this.pending = [];
    this.random = new Random();
  }

  //-------------------------------------------------------------------------
  // The events. The plugin counts rising edges in SetFloatParameter; here the
  // button is a boolean, taken and released on the frame it is seen.
  //-------------------------------------------------------------------------
  takeEvents(params) {
    if (params.get('drop') > 0.5) { this.dropPresses += 1; params.set('drop', 0); }
    if (params.get('skim') > 0.5) { this.skimPresses += 1; params.set('skim', 0); }
    if (params.get('still') > 0.5) { this.stillWanted = true; params.set('still', 0); }
  }

  makeTwiddles(length) {
    const gl = this.gl;
    const table = new Float32Array(length * 2);
    for (let m = 0; m < length; m += 1) {
      const angle = -2 * K_PI * m / length;
      table[m * 2] = Math.cos(angle);
      table[m * 2 + 1] = Math.sin(angle);
    }
    const texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RG32F, length, 1, 0, gl.RG, gl.FLOAT, table);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.bindTexture(gl.TEXTURE_2D, null);
    return texture;
  }

  /// `ensureBuffers()`. A different grid, or a change of banks, is a different
  /// pond: its state is thrown away.
  ensureBuffers(wanted, banks, width, height) {
    const gl = this.gl;
    const reset = wanted.nx !== this.grid.nx || wanted.ny !== this.grid.ny || banks !== this.bufferBanks;
    if (reset) {
      for (const b of this.spectrum) b.dispose();
      for (const b of this.surface) b.dispose();
      this.surfaceIndex = 0;
      this.pending = [];
      if (this.twiddleX) gl.deleteTexture(this.twiddleX);
      if (this.twiddleY) gl.deleteTexture(this.twiddleY);
      this.twiddleX = this.makeTwiddles(wanted.nx);
      this.twiddleY = this.makeTwiddles(wanted.ny);
    }

    for (const b of this.spectrum) b.ensure(wanted.nx, wanted.ny, gl.RG32F);
    for (const b of this.surface) {
      b.ensure(wanted.nx, wanted.ny, gl.RGBA32F);
      // The surface wraps: the domain is periodic, and the plugin allocates it
      // with Wrap::Repeat. The kit's PassBuffer clamps, so it is set here.
      gl.bindTexture(gl.TEXTURE_2D, b.texture);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.REPEAT);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.REPEAT);
    }
    gl.bindTexture(gl.TEXTURE_2D, null);
    if (reset) for (const b of this.surface) b.clearTo(0, 0, 0, 0);

    for (const b of this.causticBuffer) b.ensure(width, height, gl.R32F);

    this.grid = { ...wanted };
    this.bufferBanks = banks;
    this.causticWidth = width;
    this.causticHeight = height;
  }

  randomPoint(centreX, centreY, scatter) {
    let x = centreX;
    let y = centreY;
    if (scatter <= 0) return [x, y];
    const r = scatter * Math.sqrt(this.random.uniform());
    const angle = 2 * K_PI * this.random.uniform();
    x = f32(x + r * Math.cos(angle));
    y = f32(y + r * Math.sin(angle));
    return [clamp(x, 0, this.frameWidth), clamp(y, 0, this.frameHeight)];
  }

  /// `CollectImpacts()`, with the analyser's silence written in: Level() is 0
  /// and Fired() is false, so there is no audio pebble and no audio rain.
  collectImpacts(params, dt) {
    const due = [];
    const pebble = pebbleSizeFromParam(params.get('pebbleSize'));
    const splash = splashFromParam(params.get('splash'));
    const scatter = f32(scatterFromParam(params.get('scatter')) * this.frameHeight);
    const aimX = f32(clamp(params.get('pebbleX'), 0, 1) * this.frameWidth);
    const aimY = f32(clamp(params.get('pebbleY'), 0, 1) * this.frameHeight);

    const dropAt = (strength) => {
      const [x, y] = this.randomPoint(aimX, aimY, scatter);
      due.push({ time: this.simTime, x, y, radius: pebble, amplitude: f32(splash * pebble * strength) });
    };

    for (; this.dropPresses > 0; this.dropPresses -= 1) dropAt(1.0);

    for (; this.skimPresses > 0; this.skimPresses -= 1) {
      const heading = headingFromParam(params.get('heading'));
      const [startX, startY] = skimStart(aimX, aimY, heading, this.frameWidth, this.frameHeight, 2 * pebble);
      const skim = skimSchedule(this.simTime, startX, startY, heading,
        throwSpeedFromParam(params.get('throwSpeed')), skimAngleFromParam(params.get('skimAngle')),
        bouncesFromParam(params.get('bounces')), pebble, splash, K_GRAVITY);
      for (const touch of skim) {
        // A stone that reaches the far bank stops there; the grid is periodic.
        if (touch.x < -touch.radius || touch.x > this.frameWidth + touch.radius
          || touch.y < -touch.radius || touch.y > this.frameHeight + touch.radius) break;
        let at = this.pending.findIndex((p) => p.time > touch.time);
        if (at < 0) at = this.pending.length;
        this.pending.splice(at, 0, touch);
      }
    }

    while (this.pending.length && this.pending[0].time <= this.simTime) due.push(this.pending.shift());

    // Rain: Poisson in water-time, so Speed speeds it and a frozen pond has none.
    const rainRate = rainFromParam(params.get('rain'));
    const drops = this.random.poisson(rainRate * dt);
    const rainSize = rainSizeFromParam(params.get('rainSize'));
    for (let i = 0; i < drops; i += 1) {
      const x = f32(this.random.uniform() * this.frameWidth);
      const y = f32(this.random.uniform() * this.frameHeight);
      const radius = f32(rainSize * f32(0.7 + 0.6 * this.random.uniform()));
      due.push({ time: this.simTime, x, y, radius, amplitude: f32(K_RAIN_SPLASH * radius) });
    }

    if (due.length > K_MAX_DROPS_PER_FRAME) due.length = K_MAX_DROPS_PER_FRAME;
    return due;
  }

  setInts(program, name, a, b) {
    const loc = program.location(name);
    if (loc !== null) this.gl.uniform2i(loc, a, b);
  }

  /// `Transform()`: log2 Nx stages along x, then log2 Ny along y, ping-ponged.
  transform(buffers, current, direction) {
    const gl = this.gl;
    const grid = this.grid;
    this.fft.use();
    this.fft.setSampler('Source', 0);
    this.fft.setSampler('Twiddles', 1);
    this.fft.set('Direction', direction);
    const axes = [[1, grid.nx, this.twiddleX], [0, grid.ny, this.twiddleY]];
    for (const [horizontal, length, twiddles] of axes) {
      this.fft.setInt('Horizontal', horizontal);
      this.fft.setInt('Length', length);
      const stages = Math.round(Math.log2(length));
      for (let s = 1; s <= stages; s += 1) {
        buffers[1 - current].bind();
        bindTexture(gl, 0, buffers[current].texture);
        bindTexture(gl, 1, twiddles);
        this.fft.setInt('Span', 1 << s);
        this.quad.draw();
        current = 1 - current;
      }
    }
    return current;
  }

  water(params) {
    return {
      gravity: K_GRAVITY,
      tension: tensionFromParam(params.get('tension')),
      depth: depthFromParam(params.get('depth')),
      viscosity: viscosityFromParam(params.get('viscosity')),
    };
  }

  /// `Simulate()`: inject, forward, evolve, inverse.
  simulate(params, impacts, dt) {
    const gl = this.gl;
    const grid = this.grid;
    const water = this.water(params);
    const walls = this.bufferBanks === BANKS_WALLS ? 1 : 0;

    const minRadius = f32(1.5 * Math.max(grid.lx / grid.nx, grid.ly / grid.ny));
    const drops = new Float32Array(K_MAX_DROPS_PER_FRAME * 4);
    const count = Math.min(impacts.length, K_MAX_DROPS_PER_FRAME);
    for (let i = 0; i < count; i += 1) {
      drops[i * 4] = impacts[i].x;
      drops[i * 4 + 1] = impacts[i].y;
      drops[i * 4 + 2] = Math.max(impacts[i].radius, minRadius);
      drops[i * 4 + 3] = impacts[i].amplitude;
    }

    // The absorber, sized from the fastest wave the domain can hold.
    const longest = 2 * 3.14159265358979 / Math.max(grid.lx, grid.ly);
    const fastest = Math.max(groupVelocity(longest, water), 0.18);
    const margin = f32(0.5 * Math.min(this.frameWidth, this.frameHeight));
    const spongeRate = f32(f32(18.0 * fastest) / Math.max(margin, 0.01));

    // 1. Inject.
    this.spectrum[0].bind();
    this.inject.use();
    bindTexture(gl, 0, this.surface[this.surfaceIndex].texture);
    this.inject.setSampler('StateTexture', 0);
    this.setInts(this.inject, 'GridSize', grid.nx, grid.ny);
    this.inject.set('Domain', grid.lx, grid.ly);
    this.inject.set('Frame', this.frameWidth, this.frameHeight);
    this.inject.setInt('Walls', walls);
    this.inject.set('SpongeRate', spongeRate);
    this.inject.set('Dt', dt);
    this.inject.setInt('DropCount', count);
    this.inject.setArray('Drops', drops, 4);
    this.quad.draw();

    // 2. Forward.
    const current = this.transform(this.spectrum, 0, -1.0);

    // 3. Evolve, into surface[0].
    this.surface[0].bind();
    this.evolve.use();
    bindTexture(gl, 0, this.spectrum[current].texture);
    this.evolve.setSampler('Spectrum', 0);
    this.setInts(this.evolve, 'GridSize', grid.nx, grid.ny);
    this.evolve.set('Domain', grid.lx, grid.ly);
    this.evolve.set('Gravity', water.gravity);
    this.evolve.set('Tension', water.tension);
    this.evolve.set('Depth', water.depth);
    this.evolve.set('Viscosity', water.viscosity);
    this.evolve.set('Dt', dt);
    this.evolve.set('Norm', 1 / (grid.nx * grid.ny));
    this.evolve.set('RateScale', K_RATE_SCALE);
    this.quad.draw();

    // 4. Inverse: the surface, and next frame's state.
    this.surfaceIndex = this.transform(this.surface, 0, 1.0);
  }

  /// `Caustics()`: the mesh, additive into R32F, then the sun's size.
  caustics(depth, sunTravel, sunRadius) {
    const gl = this.gl;
    const grid = this.grid;
    const span = 1 + 2 * K_MESH_MARGIN;
    const columns = Math.max(8, Math.trunc(Math.min(span * 0.5 * grid.nx, span * this.causticWidth / 1.5)));
    const rows = Math.max(8, Math.trunc(Math.min(span * 0.5 * grid.ny, span * this.causticHeight / 1.5)));
    this.meshColumns = columns;
    this.meshRows = rows;

    const horizontal = Math.sqrt(sunTravel[0] * sunTravel[0] + sunTravel[1] * sunTravel[1]);
    const sinT = horizontal / K_WATER_INDEX;
    const cosT = Math.sqrt(Math.max(1 - sinT * sinT, 1e-6));
    let shiftX = 0;
    let shiftY = 0;
    if (horizontal > 1e-6) {
      shiftX = sunTravel[0] / horizontal * depth * sinT / cosT;
      shiftY = sunTravel[1] / horizontal * depth * sinT / cosT;
    }

    this.causticBuffer[0].clearTo(0, 0, 0, 0);
    gl.enable(gl.BLEND);
    gl.blendEquation(gl.FUNC_ADD);
    gl.blendFunc(gl.ONE, gl.ONE);

    this.caustic.use();
    bindTexture(gl, 0, this.surface[this.surfaceIndex].texture);
    this.caustic.setSampler('SurfaceTexture', 0);
    this.caustic.set('FrameToTexture', this.frameWidth / grid.lx, this.frameHeight / grid.ly);
    this.caustic.set('Frame', this.frameWidth, this.frameHeight);
    this.caustic.set('Depth', depth);
    this.caustic.set('Eta', 1 / K_WATER_INDEX);
    this.caustic.set('SunTravel', sunTravel[0], sunTravel[1], sunTravel[2]);
    this.caustic.set('FlatShift', shiftX, shiftY);
    this.caustic.set('BufferSize', this.causticWidth, this.causticHeight);
    this.setInts(this.caustic, 'Cells', columns, rows);
    this.caustic.set('Margin', K_MESH_MARGIN);
    gl.bindVertexArray(this.meshVAO);
    gl.drawArrays(gl.TRIANGLES, 0, columns * rows * 6);
    gl.bindVertexArray(null);
    gl.disable(gl.BLEND);

    this.causticIndex = 0;
    this.haveCaustics = true;

    const pixelsPerMetre = this.causticHeight / this.frameHeight;
    const sigma = f32(depth * Math.tan(sunRadius) * pixelsPerMetre);
    if (sigma < 0.35) return;

    const taps = clamp(Math.ceil(3 * sigma), 1, K_MAX_BLUR_TAPS);
    const step = f32(3 * sigma / taps);
    const axes = [[0, 1, 1 / this.causticWidth, 0], [1, 0, 0, 1 / this.causticHeight]];
    for (const [from, to, dx, dy] of axes) {
      this.causticBuffer[to].bind();
      this.blur.use();
      bindTexture(gl, 0, this.causticBuffer[from].texture);
      this.blur.setSampler('Source', 0);
      this.blur.set('Direction', dx, dy);
      this.blur.set('Sigma', sigma);
      this.blur.set('Step', Math.max(step, 1));
      this.blur.setInt('Taps', taps);
      this.quad.draw();
    }
  }

  render({ input, params, width, height, time }) {
    const gl = this.gl;
    const pictureWidth = input.width;
    const pictureHeight = input.height;
    gl.disable(gl.BLEND);

    this.takeEvents(params);

    //-----------------------------------------------------------------------
    // Time. This page's clock is already in seconds, so the plugin's vote on
    // the host's unit has nothing to decide. Restart sends it back to zero,
    // which the page treats as "from the top": the water is stilled.
    //-----------------------------------------------------------------------
    if (this.lastNow >= 0 && time < this.lastNow) {
      this.stillWanted = true;
      this.lastNow = -1;
    }
    const hostDt = this.lastNow >= 0 ? clamp(time - this.lastNow, 0, K_MAX_FRAME_DELTA) : 0;
    this.lastNow = time;
    const dt = hostDt * speedFromParam(params.get('speed'));
    this.simTime += dt;

    //-----------------------------------------------------------------------
    // The pond, in metres.
    //-----------------------------------------------------------------------
    this.frameHeight = pondSizeFromParam(params.get('pondSize'));
    this.frameWidth = f32(this.frameHeight * pictureWidth / pictureHeight);
    const detail = params.option('detail');
    const wanted = chooseGrid(this.frameWidth, this.frameHeight, DETAIL_CELLS[detail]);
    this.ensureBuffers(wanted, params.option('banks'), pictureWidth, pictureHeight);

    if (this.stillWanted) {
      for (const b of this.surface) b.clearTo(0, 0, 0, 0);
      this.pending = [];
      this.stillWanted = false;
    }

    const impacts = this.collectImpacts(params, dt);
    if (dt > 0 || impacts.length) this.simulate(params, impacts, dt);

    //-----------------------------------------------------------------------
    // The light.
    //-----------------------------------------------------------------------
    const depth = depthFromParam(params.get('depth'));
    const elevation = sunElevationFromParam(params.get('sunElevation')) * K_PI / 180;
    const azimuth = sunAzimuthFromParam(params.get('sunAzimuth')) * K_PI / 180;
    const sunRadius = sunSizeFromParam(params.get('sunSize')) * K_PI / 180;
    const sunTravel = [
      -Math.cos(elevation) * Math.cos(azimuth),
      -Math.cos(elevation) * Math.sin(azimuth),
      -Math.sin(elevation),
    ];
    const view = params.option('view');
    const causticAmount = causticsFromParam(params.get('caustics'));
    const wantCaustics = causticAmount > 0 || view === VIEW_CAUSTICS;

    this.haveCaustics = false;
    if (wantCaustics) this.caustics(depth, sunTravel, sunRadius);

    //-----------------------------------------------------------------------
    // Composite, to the canvas.
    //-----------------------------------------------------------------------
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, width, height);
    const c = this.composite;
    c.use();
    bindTexture(gl, 0, input.texture);
    bindTexture(gl, 1, this.surface[this.surfaceIndex].texture);
    bindTexture(gl, 2, this.causticBuffer[this.causticIndex].texture);
    c.setSampler('InputTexture', 0);
    c.setSampler('SurfaceTexture', 1);
    c.setSampler('CausticTexture', 2);
    c.set('MaxUV', 1, 1);
    c.set('HalfTexel', 0.5 / pictureWidth, 0.5 / pictureHeight);
    c.set('FrameToTexture', this.frameWidth / this.grid.lx, this.frameHeight / this.grid.ly);
    c.set('Frame', this.frameWidth, this.frameHeight);
    c.set('Depth', depth);
    c.set('Eta', 1 / K_WATER_INDEX);
    c.setInt('UseCaustics', this.haveCaustics ? 1 : 0);
    c.set('CausticAmount', causticAmount);
    c.set('Reflection', reflectionFromParam(params.get('reflection')));
    c.set('Sky', params.get('skyR'), params.get('skyG'), params.get('skyB'));
    c.set('SunTravel', sunTravel[0], sunTravel[1], sunTravel[2]);
    c.set('Glint', glintFromParam(params.get('glint')));
    c.set('SunRadius', sunRadius);
    c.setInt('View', view);
    const crater = splashFromParam(params.get('splash')) * pebbleSizeFromParam(params.get('pebbleSize'));
    c.set('HeightGain', 0.5 / Math.max(0.05 * crater, 1e-6));
    c.set('MixAmount', params.get('mix'));
    this.quad.draw();

    for (let unit = 2; unit >= 0; unit -= 1) bindTexture(gl, unit, null);
  }
}

//===========================================================================
// The parameters, from the constructor in Millpond.cpp. Audio, Audio Pebbles
// and Audio Rain are left out: see the note at the top.
//===========================================================================

const std = (id, name, def, group, extra = {}) => ({ id, name, type: 'standard', default: def, group, ...extra });
const button = (id, name, group, hint) => ({ id, name, type: 'boolean', default: 0, group, hint });
const unit = (v, digits, suffix) => `${v.toFixed(digits)} ${suffix}`;
const length = (m) => (m < 0.01 ? `${(m * 1000).toFixed(1)} mm` : m < 1 ? `${(m * 100).toFixed(1)} cm` : `${m.toFixed(2)} m`);

const PARAMS = [
  std('pondSize', 'Pond Size', 0.511, 'Pond', {
    display: (v) => `${length(pondSizeFromParam(v))} high`,
    hint: 'The frame’s height in metres, 0.1 to 20. The waves run on a grid sized in metres, so the same settings ring the same pond at any raster.' }),
  std('depth', 'Depth', 0.725, 'Pond', {
    display: (v) => length(depthFromParam(v)),
    hint: 'Used twice, on purpose: in the dispersion relation (shallow water holds its long waves back to √(gh)) and in the optics (the bed is this far below the surface, so a deep pond bends the picture further).' }),
  std('tension', 'Surface Tension', 0.5, 'Pond', {
    display: (v) => `${(tensionFromParam(v) / K_WATER_TENSION).toFixed(2)} × water`,
    hint: 'Zero is pure gravity waves; the middle of the travel is clean water; a drop of soap is about half.' }),
  std('viscosity', 'Viscosity', 0.2, 'Pond', {
    display: (v) => `${viscosityFromParam(v).toExponential(1)} m²/s`,
    hint: 'Water is 1e-6. The top of the range is golden syrup, where the ripples die before they leave the stone — overdamped at short wavelengths, and the propagator knows it.' }),
  std('speed', 'Speed', paramFromSpeed(1.0), 'Pond', {
    display: (v) => (speedFromParam(v) === 0 ? 'frozen' : `${speedFromParam(v).toFixed(2)}×`),
    hint: '0.05× to 4×, and exactly zero at the bottom, which freezes the water rather than running it very slowly. The propagator is exact for any step, so speed does not change what the rings do, only how fast.' }),
  { id: 'banks', name: 'Banks', type: 'option', default: 0, group: 'Pond', elements: ['Open', 'Walls'],
    hint: 'Open is a lake: what leaves the frame is absorbed. Walls is a pool: the edges reflect exactly, to every order, by the method of images. Changing it empties the pond.' },
  { id: 'detail', name: 'Detail', type: 'option', default: 2, group: 'Pond', elements: ['256', '512', '1024', '2048'],
    hint: 'The grid along the mirrored domain’s long side, so 1024 is 512 cells across the frame. Changing it empties the pond. 2048 is a lot of FFT for a browser.' },
  button('still', 'Still', 'Pond', 'Flattens the water. An event button in the plugin; here a toggle the renderer releases on the frame it acts, which is why it blinks.'),

  button('drop', 'Drop', 'Pebble', 'Drops a pebble at Pebble X/Y. An event button in the plugin; here a toggle the renderer releases on the frame it acts, which is why it blinks. Press it again for another.'),
  std('pebbleX', 'Pebble X', 0.5, 'Pebble', {
    display: (v) => `${(v * 100).toFixed(0)}% across`,
    hint: 'Where the pebble lands, and the point a skim passes through.' }),
  std('pebbleY', 'Pebble Y', 0.5, 'Pebble', {
    display: (v) => `${(v * 100).toFixed(0)}% up`,
    hint: 'Where the pebble lands, and the point a skim passes through.' }),
  std('pebbleSize', 'Pebble Size', 0.73, 'Pebble', {
    display: (v) => length(pebbleSizeFromParam(v)),
    hint: 'The crater’s spectrum peaks at k = 2/a, so the size picks which part of the dispersion curve it rings: a small pebble makes capillary ripples, a big one throws gravity rings.' }),
  std('splash', 'Splash', 0.8, 'Pebble', {
    display: (v) => `${(splashFromParam(v) * 100).toFixed(0)}% of radius deep`,
    hint: 'The crater’s depth as a fraction of its radius. Past about 0.3 the surface is steeper than linear theory really covers.' }),
  std('scatter', 'Scatter', 0.0, 'Pebble', {
    display: (v) => `${(scatterFromParam(v) * 100).toFixed(0)}% of height`,
    hint: 'A random offset around the pebble point, uniform over a disc.' }),

  button('skim', 'Skim', 'Skim', 'Skims a stone through Pebble X/Y from the bank behind it. An event button in the plugin; here a toggle the renderer releases on the frame it acts, which is why it blinks.'),
  std('heading', 'Heading', 0.0, 'Skim', {
    display: (v) => unit(headingFromParam(v), 0, '°'),
    hint: '0 runs left to right.' }),
  std('throwSpeed', 'Throw Speed', 0.39, 'Skim', {
    display: (v) => unit(throwSpeedFromParam(v), 1, 'm/s'),
    hint: 'Each bounce costs the same energy, so the hops get shorter linearly and quicker as a square root — the signature of a real skim.' }),
  std('skimAngle', 'Skim Angle', 0.27, 'Skim', {
    display: (v) => unit(skimAngleFromParam(v), 0, '°'),
    hint: 'The stone’s flight angle at each bounce.' }),
  std('bounces', 'Bounces', 0.23, 'Skim', {
    display: (v) => `${bouncesFromParam(v)} touches`,
    hint: 'How many times the stone touches before it sinks; the last touch is a full-size plop.' }),

  std('rain', 'Rain', 0.27, 'Rain', {
    display: (v) => `${rainFromParam(v).toFixed(2)} drops/s`,
    hint: 'A Poisson process in water-time, so Speed speeds the rain with the waves and a frozen pond has none falling on it.' }),
  std('rainSize', 'Rain Size', 0.46, 'Rain', {
    display: (v) => length(rainSizeFromParam(v)),
    hint: 'Raindrops have their own crater depth, 0.3 of their radius; Splash is the pebble’s.' }),

  std('caustics', 'Caustics', 0.5, 'Light', {
    display: (v) => `${causticsFromParam(v).toFixed(2)}${Math.abs(causticsFromParam(v) - 1) < 1e-3 ? ' (physical)' : ''}`,
    hint: 'The light net on the bed, from a mesh whose triangles each carry their own light, so light is conserved. 1 is physical.' }),
  std('sunElevation', 'Sun Elevation', 0.9375, 'Light', {
    display: (v) => unit(sunElevationFromParam(v), 1, '°'),
    hint: 'A camera looking straight down only sees the sun in a facet tilted by half the sun’s distance from overhead — so glints need a high sun.' }),
  std('sunAzimuth', 'Sun Azimuth', 0.35, 'Light', {
    display: (v) => unit(sunAzimuthFromParam(v), 0, '°') }),
  std('sunSize', 'Sun Size', 0.39, 'Light', {
    display: (v) => unit(sunSizeFromParam(v), 2, '°'),
    hint: 'The sun’s angular radius: 0.27° is the real sun, several degrees is haze. It blurs the caustics by depth × tan(radius) and widens the glints.' }),
  std('reflection', 'Reflection', 1 / 3, 'Light', {
    display: (v) => `${reflectionFromParam(v).toFixed(2)} × Fresnel`,
    hint: '1 is the exact Fresnel term: 2% looking straight down, all of it at grazing.' }),
  { id: 'skyR', name: 'Sky', type: 'colour', default: 0.55, group: 'Light' },
  { id: 'skyG', name: 'Sky_Green', type: 'colour', default: 0.65, group: 'Light' },
  { id: 'skyB', name: 'Sky_Blue', type: 'colour', default: 0.75, group: 'Light' },
  std('glint', 'Glint', 0.3, 'Light', {
    display: (v) => `${glintFromParam(v).toFixed(0)} × sky`,
    hint: 'The sun’s radiance relative to the sky’s. It has to be big: the real sun is 10⁴ to 10⁵ times the sky, and 2% of that is still far past white.' }),

  { id: 'view', name: 'View', type: 'option', default: 0, group: 'Output', elements: ['Picture', 'Height', 'Slope', 'Caustics'],
    hint: 'Picture is the effect. Height, Slope and Caustics each show one model on its own.' },
  std('mix', 'Mix', 1.0, 'Output', {
    display: (v) => `${(v * 100).toFixed(0)}%`,
    hint: 'The water against the clip.' }),
];

mountDemo({
  name: 'Millpond',
  pluginId: 'MP01',
  tagline: 'Ripples. The clip is a pond bed seen straight down through water that obeys linear water-wave theory exactly: every frame an FFT, every mode advanced as a damped oscillator, and back — then Snell, a light-conserving caustic net, Fresnel sky and the sun’s glints.',
  repo: 'https://github.com/stoatworks-labs/millpond',
  page: 'https://stoatworks-labs.com/software/millpond/',
  video: 'https://www.youtube.com/watch?v=oAbXjVXFwYA',

  // RG32F spectrum, RGBA32F surface, R32F caustics; the caustic mesh adds
  // into the R32F buffer by blending. OES_texture_float_linear is required in
  // createRenderer.
  needFloat: true,
  needFloatBlend: true,

  // Water hides on a busy clip (the plugin's own video uses only calm ones),
  // so the geometry card first: its straight lines show every bend.
  sources: ['grid', 'bars', 'ramp', 'scene', 'spot', 'detail'],

  params: PARAMS,

  differences: [
    'The eight shaders are the plugin’s own text, and demo/tools/check_shaders.py proves it. Everything the CPU does is a hand port to JavaScript: the control conversions, the dispersion relation and group velocity used to size the absorber, the skim schedule, the PCG random numbers and the Poisson rain, the grid choice, and the frame sequence — inject, forward FFT, evolve, inverse FFT, caustic mesh, blur, composite. Nothing checks that port but a reader.',
    'Drop, Skim and Still are FF_TYPE_EVENT buttons in the plugin. The kit has no event type, so each is a toggle here that the renderer takes and releases on the frame it acts — which is what a host does with an event anyway, and why the button blinks. In Resolume the plugin also takes several presses in one frame; here a press is one frame long.',
    'The audio side is not here. The plugin reads Resolume’s FFT buffer: Audio Pebbles drops a pebble on each onset and Audio Rain thickens the rain with the level. A browser has no equivalent and asking for a microphone to demonstrate a video effect is not a trade worth making, so the three controls are absent rather than present and dead. The removal is exact: with no spectrum the plugin’s analyser never fires and reports no level.',
    'Float render targets and filtering. The water needs EXT_color_buffer_float (the spectrum, the surface and the caustics are 32-bit float targets), EXT_float_blend (the caustic mesh adds into a 32-bit float buffer) and OES_texture_float_linear (the surface and the caustics are read with a linear filter). All three are core in the plugin’s GL 4.1 and optional in WebGL2; the page stops with a message if one is missing rather than render a still pond.',
    'The clock is this page’s, in seconds, so the plugin’s vote on whether the host sends seconds or milliseconds has nothing to decide. Each frame is still clamped to a quarter of a second, as in the plugin. Restart stills the water; the plugin has no such control.',
    'The About block is not here; the links are in the header.',
  ],

  createRenderer: (gl, quad) => new MillpondRenderer(gl, quad),
});
