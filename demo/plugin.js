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
 * The four Faraday shaders are copied unedited too: the envelope, the pattern
 * kernel, the saturated state and the shaken evolve (every mode a damped
 * Mathieu oscillator, fourth-order Magnus, the drive's cosines from a table).
 * `demo/tools/check_shaders.py` proves all twelve character for character and
 * `tools/verify.sh` runs it.
 *
 * What is a hand port, and is checked by nothing but a reader: `Controls.cpp`
 * (every 0..1 conversion, the six Faraday ones included), the CPU half of
 * `Physics.cpp` (`Omega` and `GroupVelocity` for the sponge's rate,
 * `SkimSchedule`, `SkimStart`, the PCG `Random` and its Poisson draw,
 * `ChooseGrid`), all of `Faraday.cpp` (`CriticalWavenumber`,
 * `ThresholdAcceleration`, `ShakenDamping`, the `Bass` envelope, the `Strobe`,
 * `DriveTable`, `Wrap`), `CollectImpacts`, `PlanShake`, `ShakeState`,
 * `Simulate` (shaken and not), `Transform`, `Caustics` and the frame sequence
 * of `ProcessOpenGL`, with its 1/30 s chunks for a long shaken frame. Copied
 * from the C++ line by line, not re-derived; `PlanShake` was cross-checked
 * against a C++ build once, by hand (AGENTS.md), and nothing re-runs that. The
 * parameter declarations come from the constructor in `Millpond.cpp`.
 *
 * **Drop, Skim and Still are buttons.** The plugin declares them as
 * FF_TYPE_EVENT and acts on the rising edge. The kit's parameter model has no
 * event type, so — as readout's Fire does — each is a boolean here that the
 * renderer takes and releases in the same frame, which is what a host does
 * with an event parameter anyway. That is why the button blinks.
 *
 * **There is no audio, and the page does not ask for a microphone.** The
 * plugin reads one thing from its host for sound: Resolume's FF_USAGE_FFT
 * buffer, 64 bins, once a frame. Shake needs a bass to act on, so the page
 * writes a GENERATED TEST BASS into 64 bins itself every frame (needle's
 * precedent), chosen from the kit's one transport dropdown, "Test bass": the
 * harness's own `--drone` (the lowest four bins at 1, the rest at 0.05), the
 * same drone 4 s on and 4 s off, the harness's `--kick` (a kick in the lowest
 * two bins twice a second over a pad of 0.5), and silence. The plugin's own
 * bass envelope, ported, reads those bins. It is not Resolume's FFT: nobody
 * has measured what Resolume's bins are, which is why Bass Bins exists.
 *
 * **Audio, Audio Pebbles and Audio Rain are absent**, as before. The onset
 * analyser behind them is not ported. Both amounts default to 0, and at 0 the
 * plugin drops no audio pebble and adds no audio rain whatever the spectrum
 * holds, so with the test bass playing the page still drops exactly the
 * pebbles and rain the plugin would at those defaults.
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
// The Faraday shaders. Copied from source/Shaders.cpp. Do not edit here.
// Only while the tank is shaken: with Shake at 0 not one of them runs.
//===========================================================================

const FARADAY_ENVELOPE = `#version 410 core

uniform sampler2D StateTexture;//(eta, eta_t / RateScale, eta_x, eta_y)
uniform float OmegaC;          //the response frequency, half the drive, rad/s
uniform float RateScale;

out vec4 fragColor;

void main()
{
	vec4 state = texelFetch( StateTexture, ivec2( gl_FragCoord.xy ), 0 );

	//For a wave at omega_c, eta = A cos and eta_t / omega_c = -A sin: this is
	//A^2 at every instant of the cycle, so the damping it sets does not depend
	//on which phase of the oscillation a frame happens to land on.
	float rate = RateScale * state.y / OmegaC;
	fragColor  = vec4( state.x * state.x + rate * rate, 0.0, 0.0, 0.0 );
}
`;

const FARADAY_KERNEL = `#version 410 core

uniform sampler2D Spectrum;//FFT( A~^2 )
uniform ivec2 GridSize;
uniform vec2 Domain;
uniform float Wavenumber;  //k_c, rad/m
uniform float Width;       //the notches' width, as a fraction of k_c
uniform int Pattern;       //0 stripes, 1 squares, 2 hexagons
uniform float Norm;        //1 / ( Nx Ny ): the inverse transform's scale, applied here once

out vec4 fragColor;

float notch( float k, float centre )
{
	float x = ( k - centre ) / ( Width * Wavenumber );
	return 1.0 - exp( -0.5 * x * x );
}

void main()
{
	ivec2 cell = ivec2( gl_FragCoord.xy );
	ivec2 m    = ivec2( cell.x < GridSize.x / 2 ? cell.x : cell.x - GridSize.x,
	                    cell.y < GridSize.y / 2 ? cell.y : cell.y - GridSize.y );
	float k    = length( 6.28318530717958648 * vec2( m ) / Domain );

	float kernel = 1.0;
	if( Pattern == 1 )
		kernel = notch( k, 1.41421356237 * Wavenumber );
	else if( Pattern == 2 )
		kernel = notch( k, Wavenumber ) * notch( k, 1.73205080757 * Wavenumber );

	fragColor = vec4( Norm * kernel * texelFetch( Spectrum, cell, 0 ).xy, 0.0, 0.0 );
}
`;

const FARADAY_STATE = `#version 410 core

uniform sampler2D StateTexture;   //(eta, eta_t / RateScale, eta_x, eta_y)
uniform sampler2D EnvelopeTexture;//K * A~^2, m^2, in .x
uniform float SatRate;            //beta, 1 / ( m^2 s )
uniform float Dt;                 //seconds of water this frame

out vec4 fragColor;

//exp( -z ) for z >= 0. Below 0.1 as its series (the seventh term is 2e-11):
//Apple's software renderer's exp is good to about 1e-4, and a factor applied
//to the state every frame compounds whatever error it has.
float expNeg( float z )
{
	if( z < 0.1 )
		return 1.0 + z * ( -1.0 + z * ( 0.5 + z * ( -1.0 / 6.0 + z * ( 1.0 / 24.0 + z * ( -1.0 / 120.0 + z * ( 1.0 / 720.0 ) ) ) ) ) );
	return exp( -z );
}

void main()
{
	ivec2 cell = ivec2( gl_FragCoord.xy );
	vec4 state = texelFetch( StateTexture, cell, 0 );

	//The saturating damping. The same form as the Open sponge: both halves of
	//the state decay together, so it takes energy and shifts no phase. The
	//envelope can dip below zero where the kernel's notches take out more
	//than the mean; a damping must not become a gain there.
	float envelope = max( texelFetch( EnvelopeTexture, cell, 0 ).x, 0.0 );
	float keep     = expNeg( SatRate * envelope * Dt );
	fragColor      = vec4( state.x * keep, state.y * keep, 0.0, 0.0 );
}
`;

const FARADAY_EVOLVE = `#version 410 core

uniform sampler2D Spectrum;  //Z = FFT( eta + i eta_t / RateScale )
uniform sampler2D DriveTable;//RG32F, Steps x 1: cos( drive phase ) at each substep's two Gauss points, from doubles
uniform sampler2D TwiddleX;  //RG32F, Nx x 1: exp( -2 pi i m / Nx ), from doubles (the transform's own)
uniform sampler2D TwiddleY;  //RG32F, Ny x 1
uniform ivec2 GridSize;
uniform vec2 Domain;
uniform float Gravity;       //m/s^2
uniform float Tension;       //surface tension over density, m^3/s^2
uniform float Depth;         //m
uniform float Viscosity;     //m^2/s
uniform float WallDamping;   //s^-1, the same for every mode
uniform float Accel;         //the shake's acceleration amplitude, m/s^2
uniform float Step;          //the substep, s
uniform int Steps;
uniform float Norm;
uniform float RateScale;
uniform float Wavenumber;    //k_c, rad/m: where the seed goes
uniform float SeedWidth;     //the seed's band, one standard deviation, as a fraction of k_c
uniform float SeedAmp;       //this frame's seed at k_c: metres RMS a cell, were it white
uniform int Seed;            //the shaken frame's number
uniform int Walls;

out vec4 fragColor;

//exp( -z ) for z >= 0, as the state pass has it: the series below 0.1.
float expNeg( float z )
{
	if( z < 0.1 )
		return 1.0 + z * ( -1.0 + z * ( 0.5 + z * ( -1.0 / 6.0 + z * ( 1.0 / 24.0 + z * ( -1.0 / 120.0 + z * ( 1.0 / 720.0 ) ) ) ) ) );
	return exp( -z );
}

//( cos x, sin x ) for x >= 0 by quadrant and series, with nothing
//transcendental: Apple's software renderer's cos and sin are far enough off
//that 180 substeps of them put a short wave 17% wrong. x is reduced by the
//nearest multiple of pi / 2 in two parts (Cody and Waite: the float nearest
//pi / 2 is 4.37e-8 too big), leaving |r| <= pi / 4, where the next terms are
//under 3e-8.
vec2 cosSin( float x )
{
	float n  = floor( x * 0.636619772 + 0.5 );
	float r  = ( x - n * 1.57079637 ) + n * 4.37113883e-8;
	float r2 = r * r;
	float c  = 1.0 + r2 * ( -1.0 / 2.0 + r2 * ( 1.0 / 24.0 + r2 * ( -1.0 / 720.0 + r2 * ( 1.0 / 40320.0 ) ) ) );
	float s  = r * ( 1.0 + r2 * ( -1.0 / 6.0 + r2 * ( 1.0 / 120.0 + r2 * ( -1.0 / 5040.0 + r2 * ( 1.0 / 362880.0 ) ) ) ) );
	int q    = int( mod( n, 4.0 ) );
	if( q == 0 )
		return vec2( c, s );
	if( q == 1 )
		return vec2( -s, c );
	if( q == 2 )
		return vec2( -c, -s );
	return vec2( s, -c );
}

uint mixBits( uint x )
{
	//lowbias32, as the caustic mesh's jitter: integer, the same on every machine.
	x ^= x >> 16u;
	x *= 0x7feb352du;
	x ^= x >> 15u;
	x *= 0x846ca68bu;
	x ^= x >> 16u;
	return x;
}

//Uniform with unit variance, from a cell, the frame's number and a salt.
float noiseAt( ivec2 at, uint salt )
{
	uint h  = mixBits( ( uint( at.x ) * 0x9e3779b9u ) ^ mixBits( ( uint( at.y ) + 0x632be5abu ) ^ mixBits( uint( Seed ) * 2u + salt ) ) );
	float u = ( float( h >> 8u ) + 0.5 ) / 16777216.0;
	return 1.73205081 * ( 2.0 * u - 1.0 );
}

//A real field's noise at one cell of the spectrum: one complex number a
//pair, A( -m ) = conj A( m ), taken from whichever of the two cells comes
//first. Unit variance in each half.
vec2 hermitian( ivec2 at )
{
	ivec2 opp  = ivec2( ( GridSize.x - at.x ) % GridSize.x, ( GridSize.y - at.y ) % GridSize.y );
	bool first = at.x < opp.x || ( at.x == opp.x && at.y <= opp.y );
	ivec2 key  = first ? at : opp;
	float sign = at == opp ? 0.0 : ( first ? 1.0 : -1.0 );
	return vec2( noiseAt( key, 0u ), sign * noiseAt( key, 1u ) );
}

vec2 cmul( vec2 a, vec2 b )
{
	return vec2( a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x );
}

void main()
{
	ivec2 cell = ivec2( gl_FragCoord.xy );

	//Nyquist, as in the evolve pass.
	if( cell.x == GridSize.x / 2 || cell.y == GridSize.y / 2 )
	{
		fragColor = vec4( 0.0 );
		return;
	}

	//Unpack the two real spectra, as in the evolve pass.
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
	float omega2      = max( restoring * depthFactor, 0.0 );
	float gamma       = 2.0 * Viscosity * k * k + WallDamping;//= mirrored

	//The shake modulates omega^2 by q cos( theta ): Benjamin & Ursell (2.12),
	//gravity g - a cos theta in the tank's frame.
	float q = Accel * k * depthFactor;

	//Fourth-order Magnus, two Gauss points a substep. For y' = M(t) y with
	//M = [[0, 1], [-w, -2 gamma]] the commutator [M2, M1] is
	//(w2 - w1) [[1, 0], [-2 gamma, -1]], so the step's exponent is
	//    E = [[d, h], [-h wbar - 2 gamma d, -2 gamma h - d]],  d = sqrt(3) h^2 ( w2 - w1 ) / 12
	//with trace -2 gamma h and determinant h^2 wbar - d^2, and
	//    exp( E ) = exp( -gamma h ) [ C I + S ( E + gamma h I ) ]
	//with C = cos( psi ), S = sin( psi ) / psi, psi^2 the determinant less
	//( gamma h )^2 -- cosh and sinh past critical damping. With no shake,
	//exactly the evolve pass's step.
	//
	//Hundreds of substeps a frame compound any error in C, S and the decay,
	//and Apple's software renderer's cos, sin and exp are good to about 2e-4
	//(700 substeps of them missed the Mathieu solution by 15%). So for
	//|psi| < 1 -- every mode near the lattice -- C and S are their series in
	//psi^2, which hold on both sides of critical damping and need nothing
	//transcendental (the sixth term is under 2e-9); past it, cos and sin come
	//from cosSin. The overdamped side keeps exp, which that renderer gets
	//right to the measurement's precision (--mathieu's syrup case).
	float h     = Step;
	float decay = expNeg( gamma * h );
	float corr  = 0.144337567 * h * h;
	vec2 eta    = A;
	vec2 rate   = B;
	for( int i = 0; i < Steps; ++i )
	{
		vec2 drive = texelFetch( DriveTable, ivec2( i, 0 ), 0 ).xy;
		float w1   = omega2 - q * drive.x;
		float w2   = omega2 - q * drive.y;
		float wbar = 0.5 * ( w1 + w2 );
		float d    = corr * ( w2 - w1 );
		float disc = h * h * ( wbar - gamma * gamma ) - d * d;

		float C;
		float S;
		if( abs( disc ) < 1.0 )
		{
			float x = disc;
			C = decay * ( 1.0 + x * ( -1.0 / 2.0 + x * ( 1.0 / 24.0 + x * ( -1.0 / 720.0 + x * ( 1.0 / 40320.0 + x * ( -1.0 / 3628800.0 ) ) ) ) ) );
			S = decay * ( 1.0 + x * ( -1.0 / 6.0 + x * ( 1.0 / 120.0 + x * ( -1.0 / 5040.0 + x * ( 1.0 / 362880.0 + x * ( -1.0 / 39916800.0 ) ) ) ) ) );
		}
		else if( disc > 0.0 )
		{
			float psi = sqrt( disc );
			vec2 cs   = cosSin( psi );
			C         = decay * cs.x;
			S         = decay * cs.y / psi;
		}
		else
		{
			//cosh and sinh times the decay, so neither half overflows
			//before the other can cancel it.
			float psi  = sqrt( -disc );
			float slow = exp( psi - gamma * h );
			float fast = exp( -psi - gamma * h );
			C          = 0.5 * ( slow + fast );
			S          = 0.5 * ( slow - fast ) / psi;
		}

		vec2 etaNext  = C * eta + S * ( ( d + gamma * h ) * eta + h * rate );
		vec2 rateNext = C * rate - S * ( ( h * wbar + 2.0 * gamma * d ) * eta + ( gamma * h + d ) * rate );
		eta           = etaNext;
		rate          = rateNext;
	}

	vec2 A2 = eta;
	vec2 B2 = rate / RateScale;

	//The seed: the ambient disturbance the lattice grows from, a Wiener
	//increment in eta. Only near k_c: the modes the shake can amplify need
	//something to break their symmetry, and a seed white out to the grid's
	//scale was up to 19 levels of caustic sparkle in silence. Each mode gets
	//what a white field of SeedAmp a cell would give it, times a Gaussian band;
	//no orientation is preferred, and the mean level (m = 0) is never moved.
	//
	//eta is real, so the noise is Hermitian (above). With Walls the field must
	//also be its own mirror image about the half cell, eta( N-1-i ) = eta( i ),
	//which in the spectrum is A( m ) = P A( -m ), P = exp( 2 pi i m / N ): so
	//the noise is averaged over the two mirrors, with P from the transform's
	//own twiddles (computed in double) rather than this GPU's cos and sin --
	//Apple's software renderer's, good to 2e-4, broke the mirror by 1e-3.
	if( SeedAmp > 0.0 && ( m.x != 0 || m.y != 0 ) )
	{
		float band = ( k - Wavenumber ) / ( SeedWidth * Wavenumber );
		float amp  = SeedAmp * sqrt( float( GridSize.x ) * float( GridSize.y ) ) * exp( -0.25 * band * band );
		if( Walls != 0 )
		{
			vec2 px = texelFetch( TwiddleX, ivec2( cell.x, 0 ), 0 ).xy * vec2( 1.0, -1.0 );
			vec2 py = texelFetch( TwiddleY, ivec2( cell.y, 0 ), 0 ).xy * vec2( 1.0, -1.0 );
			vec2 n  = hermitian( cell ) + cmul( px, hermitian( ivec2( opposite.x, cell.y ) ) )
			          + cmul( py, hermitian( ivec2( cell.x, opposite.y ) ) ) + cmul( cmul( px, py ), hermitian( opposite ) );
			A2 += amp * 0.35355339 * n;
		}
		else
			A2 += amp * 0.70710678 * hermitian( cell );
	}

	//Pack for the way back, as in the evolve pass.
	vec2 state = vec2( A2.x - B2.y, A2.y + B2.x );
	vec2 slope = vec2( -kv.x * A2.y, kv.x * A2.x ) - kv.y * A2;

	fragColor = Norm * vec4( state, slope );
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

/// Faraday. All of these do nothing while Shake is 0.
const K_DRIVE_LOW = 5.0;
const K_DRIVE_HIGH = 120.0;
const K_DAMPING_LOW = f32(0.1);
const K_DAMPING_HIGH = 30.0;
const K_SLOW_LOW = 1.0;
const K_SLOW_HIGH = 100.0;
const K_MAX_OVERDRIVE = 4.0;
const K_MAX_BASS_BINS = 16;
/// Rounded to float at each of the C++'s float operations -- high / low, the
/// float pow, the multiply -- rather than once at the end as `geometric` above
/// is, which differs from the C++ by one unit in the last place at some
/// settings (AGENTS.md, the cross-check).
const geometricF32 = (v, lo, hi) => f32(lo * f32(Math.pow(f32(hi / lo), clamp(v, 0, 1))));
const inverseGeometric = (value, low, high) => {
  const clamped = clamp(value, low, high);
  return f32(Math.log(clamped / low) / Math.log(high / low));
};
const shakeFromParam = (v) => f32(K_MAX_OVERDRIVE * clamp(v, 0, 1));
const driveFromParam = (v) => geometricF32(v, K_DRIVE_LOW, K_DRIVE_HIGH);
const paramFromDrive = (hertz) => inverseGeometric(hertz, K_DRIVE_LOW, K_DRIVE_HIGH);
/// Exactly zero at the bottom: Lamb's damping alone, a tank with no walls.
const dampingFromParam = (v) => (v <= 0 ? 0 : geometricF32(v, K_DAMPING_LOW, K_DAMPING_HIGH));
const paramFromDamping = (rate) => (rate <= 0 ? 0 : inverseGeometric(rate, K_DAMPING_LOW, K_DAMPING_HIGH));
/// In double, as the C++ has it.
const slowMotionFromParam = (v) => K_SLOW_LOW * Math.pow(K_SLOW_HIGH / K_SLOW_LOW, clamp(v, 0, 1));
const paramFromSlowMotion = (factor) => {
  const clamped = clamp(factor, K_SLOW_LOW, K_SLOW_HIGH);
  return f32(Math.log(clamped / K_SLOW_LOW) / Math.log(K_SLOW_HIGH / K_SLOW_LOW));
};
const bassBinsFromParam = (v) => clamp(Math.round(linear(v, 1.0, K_MAX_BASS_BINS)), 1, K_MAX_BASS_BINS);
const paramFromBassBins = (bins) => f32((clamp(bins, 1, K_MAX_BASS_BINS) - 1) / (K_MAX_BASS_BINS - 1));

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
const PATTERN_STRIPES = 0;
const PATTERN_COUNT = 3;

/// Millpond.cpp's constants.
const K_MAX_FRAME_DELTA = 0.25;
const K_MESH_MARGIN = f32(0.1);
const K_PI = Math.PI;
const K_TWO_PI_D = 6.28318530717958647692;

/// `optionIndex`: the option a host's float holds, clamped to the list.
const optionIndex = (value, count) => clamp(Math.round(value), 0, count - 1);

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

/// `CurrentWater()`: the water the controls describe, as the plugin builds it.
function currentWater(params) {
  return {
    gravity: K_GRAVITY,
    tension: tensionFromParam(params.get('tension')),
    depth: depthFromParam(params.get('depth')),
    viscosity: viscosityFromParam(params.get('viscosity')),
  };
}

//===========================================================================
// Faraday.h / Faraday.cpp, ported: the shaken tank's CPU half. All in double,
// as the C++ is, except where the C++ is float (the bass envelope).
//===========================================================================

const K_STEPS_PER_PERIOD = 32;
const K_MAX_STEPS = 1024;
const K_SATURATION = 150.0;
const K_SEED = 1e-8;
const K_SEED_WIDTH = f32(0.25);
const K_MAX_SHAKEN_STEP = 1.0 / 30.0;
const K_MAX_CHUNKS = 32;
const K_NOTCH_WIDTH = f32(0.25);

const K_ATTACK = f32(0.005);
const K_RELEASE = f32(0.080);
const K_PEAK_TAU = f32(3.0);
const K_PEAK_FLOOR = f32(0.02);

/// `Wrap`: x mod 2 pi, in [0, 2 pi). `%` is fmod: it keeps the dividend's sign.
function wrap(phase) {
  const wrapped = phase % K_TWO_PI_D;
  return wrapped < 0 ? wrapped + K_TWO_PI_D : wrapped;
}

/// `CriticalWavenumber`: the root of Omega( k ) = omega, by bisection in log k.
function criticalWavenumber(omegaTarget, water) {
  if (!(omegaTarget > 0)) return 0;
  let lo = 1e-4;
  let hi = 1e7;
  for (let i = 0; i < 200; i += 1) {
    const mid = Math.sqrt(lo * hi);
    if (omega(mid, water) < omegaTarget) lo = mid;
    else hi = mid;
  }
  return Math.sqrt(lo * hi);
}

/// `ThresholdAcceleration`: the first tongue's a_c at exact resonance, to
/// leading order in the damping: 4 gamma omega0 / ( k tanh( k h ) ).
function thresholdAcceleration(k, omega0, gamma, depth) {
  const forcing = k * Math.tanh(Math.min(k * depth, 20.0));
  if (forcing <= 0) return 0;
  return 4.0 * gamma * omega0 / forcing;
}

/// `ShakenDamping`: Lamb's 2 nu k^2 plus the walls' rate.
function shakenDamping(k, viscosity, walls) {
  return 2.0 * viscosity * k * k + walls;
}

/// A one-pole coefficient, exact for the frame's length; 1 (snap) when no time
/// passed, which is what primes the envelope on the first frame.
function coefficient(dt, tau) {
  if (dt <= 0) return 1;
  return f32(1 - f32(Math.exp(-dt / tau)));
}

/// `Bass`: the mean of sqrt( bin ) over the lowest `bassBins` bins, a 5 ms
/// attack and 80 ms release, normalised against its own recent peak (3 s
/// decay, floor 0.02). Float, as the C++ is.
class Bass {
  constructor() {
    this.envelope = 0;
    this.peak = 0;
  }

  reset() {
    this.envelope = 0;
    this.peak = 0;
  }

  update(bins, count, bassBins, dt) {
    const n = clamp(Math.min(bassBins, count), 0, 64);
    let sum = 0;
    for (let i = 0; i < n; i += 1) sum = f32(sum + f32(Math.sqrt(Math.max(0, bins ? bins[i] : 0))));
    const raw = n > 0 ? f32(sum / n) : 0;

    const coeff = coefficient(dt, raw >= this.envelope ? K_ATTACK : K_RELEASE);
    this.envelope = f32(this.envelope + f32(f32(raw - this.envelope) * coeff));

    const fall = coefficient(dt, K_PEAK_TAU);
    this.peak = Math.max(this.envelope, f32(this.peak - f32(f32(this.peak - K_PEAK_FLOOR) * fall)));
    this.peak = Math.max(this.peak, K_PEAK_FLOOR);
  }

  level() {
    return clamp(f32(this.envelope / Math.max(this.peak, K_PEAK_FLOOR)), 0, 1);
  }
}

/// `Strobe`: Slow Motion as a stroboscope. Each frame the water advances to
/// the latest moment, no later than the nominal clock, at which its phase
/// omega_c t matches a phase slowed S times. Both phases in double, reduced
/// mod 2 pi every frame.
class Strobe {
  constructor() {
    this.reset();
  }

  reset() {
    this.waterPhase = 0;
    this.seenPhase = 0;
    this.lag = 0;
  }

  advance(nominal, omegaC, slow) {
    if (!(omegaC > 0) || nominal <= 0) return Math.max(nominal, 0);

    if (slow <= 1.0 + 1e-9) {
      // No stroboscope: the water keeps the nominal clock.
      this.waterPhase = wrap(this.waterPhase + omegaC * nominal);
      this.seenPhase = this.waterPhase;
      this.lag = 0;
      return nominal;
    }

    this.seenPhase = wrap(this.seenPhase + omegaC * nominal / slow);

    const period = K_TWO_PI_D / omegaC;
    const base = wrap(this.seenPhase - this.waterPhase) / omegaC;
    const target = this.lag + nominal;
    const whole = Math.max(0, Math.floor((target - base) / period));
    const step = base + whole * period;

    this.lag = target - step;
    this.waterPhase = this.seenPhase;
    return step;
  }
}

/// `DriveTable`: the drive's cosine at the two Gauss points of each substep,
/// interleaved, computed in double and stored as float. Returns the substeps.
function driveTable(theta0, omegaDrive, dt, table) {
  const periods = dt * omegaDrive / K_TWO_PI_D;
  const steps = clamp(Math.ceil(periods * K_STEPS_PER_PERIOD), 1, K_MAX_STEPS);
  const h = dt / steps;

  // Gauss-Legendre, two points: the step's middle plus or minus h / ( 2 sqrt 3 ).
  const offset = 0.5 / Math.sqrt(3.0);
  for (let i = 0; i < steps; i += 1) {
    const middle = (i + 0.5) * h;
    table[i * 2] = Math.cos(theta0 + omegaDrive * (middle - offset * h));
    table[i * 2 + 1] = Math.cos(theta0 + omegaDrive * (middle + offset * h));
  }
  return steps;
}

/// `MillpondPlugin::PlanShake`: everything a shaken frame needs from the
/// controls and the clocks, and the water time it covers. Advances the
/// stroboscope. `table` (a Float32Array of 2 x K_MAX_STEPS) gets the drive's
/// cosines. A free function here, rather than a method, so that it can be
/// lifted out of the page and run under node against the C++ (AGENTS.md).
function planShake(params, bass, strobe, nominal, table) {
  const shake = { active: true };

  const water = currentWater(params);
  shake.driveHz = driveFromParam(params.get('drive'));
  const omegaDrive = K_TWO_PI_D * shake.driveHz;

  // The response is at half the drive, at the wavenumber millpond's own
  // dispersion relation gives that frequency.
  shake.omegaC = 0.5 * omegaDrive;
  shake.wavenumber = criticalWavenumber(shake.omegaC, water);
  shake.walls = dampingFromParam(params.get('damping'));
  shake.gammaC = shakenDamping(shake.wavenumber, water.viscosity, shake.walls);
  shake.threshold = thresholdAcceleration(shake.wavenumber, shake.omegaC, shake.gammaC, water.depth);
  shake.accel = f32(shakeFromParam(params.get('shake')) * bass.level()) * shake.threshold;
  shake.satRate = K_SATURATION * shake.gammaC * shake.wavenumber * shake.wavenumber;
  shake.pattern = optionIndex(params.get('pattern'), PATTERN_COUNT);

  // The drive's phase where this frame's water starts: twice the response's,
  // both kept in double and reduced mod 2 pi every frame. The page's clock is
  // never in it, as the host's is never in the plugin's.
  shake.theta0 = wrap(2.0 * strobe.waterPhase);

  shake.waterStep = strobe.advance(nominal, shake.omegaC, slowMotionFromParam(params.get('slowMotion')));
  shake.steps = shake.waterStep > 0 ? driveTable(shake.theta0, omegaDrive, shake.waterStep, table) : 0;
  shake.chunks = 1;
  return shake;
}

//===========================================================================
// The GENERATED TEST BASS. This is the page's, not the plugin's: a browser has
// no Resolume and this page asks for no microphone, so it writes the 64 bins
// the plugin reads from Resolume's FFT buffer itself, once a frame, from the
// page's clock. Not Resolume's FFT — whose bins nobody has measured — but the
// three feeds the plugin's own harness uses (tools/mptest's AudioFeed), plus
// one switched on and off, so a frame here can be held against mptest's.
//===========================================================================

const AUDIO_BINS = 64;

/// mptest's AudioFeed::Drone: the low bins at 1, the rest at 0.05.
function writeDrone(bins) {
  for (let i = 0; i < AUDIO_BINS; i += 1) bins[i] = i < 4 ? 1.0 : f32(0.05);
}

const TEST_BASS = [
  {
    id: 'drone',
    name: 'Drone: a steady bass',
    hint: 'The plugin harness’s --drone: the lowest four bins at 1 and the other sixty at 0.05, the same every frame. With Shake up, the lattice stands up within about two seconds and stays.',
    write: (bins) => writeDrone(bins),
  },
  {
    id: 'comesAndGoes',
    name: 'Drone, 4 s on and 4 s off',
    hint: 'The same drone for four seconds, then four seconds of silence (every bin 0). The lattice stands up, then lies down at the Damping’s rate when the bass stops.',
    write: (bins, t) => {
      if (t % 8 < 4) writeDrone(bins);
      else bins.fill(0);
    },
  },
  {
    id: 'kick',
    name: 'Kick twice a second over a pad',
    hint: 'The plugin harness’s --kick: a kick decaying in 60 ms in the lowest two bins every half second, over a steady 0.5 in the rest. At Bass Bins 1 or 2 the kick is all the bass there is; at 16 the pad’s level dominates and the bass barely moves.',
    write: (bins, t) => {
      const kick = f32(Math.exp(-(t % 0.5) / 0.06));
      for (let i = 0; i < AUDIO_BINS; i += 1) bins[i] = i < 2 ? kick : 0.5;
    },
  },
  {
    id: 'silence',
    name: 'Silence',
    hint: 'Every bin 0: what the plugin sees on a layer with no audio routed to it. Shake has nothing to act on.',
    write: (bins) => bins.fill(0),
  },
];

/// `?bass=<id>` picks the test bass on load, for a link or a headless check.
const TEST_BASS_DEFAULT = (() => {
  const wanted = new URLSearchParams(window.location.search).get('bass');
  return TEST_BASS.some((b) => b.id === wanted) ? wanted : 'drone';
})();

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

    // The Faraday passes, and only those, may fail on their own, as in the
    // plugin's InitGL: a driver that refuses one leaves Shake inert and the
    // pond exactly as v0.1.0 drew it. Here the line under the picture also
    // says so, with the compiler's message.
    this.faradayError = null;
    try {
      this.envelopeProgram = new Program(gl, VERTEX, FARADAY_ENVELOPE, 'faraday envelope');
      this.kernelProgram = new Program(gl, VERTEX, FARADAY_KERNEL, 'faraday kernel');
      this.shakeStateProgram = new Program(gl, VERTEX, FARADAY_STATE, 'faraday state');
      this.shakenEvolveProgram = new Program(gl, VERTEX, FARADAY_EVOLVE, 'faraday evolve');
    } catch (error) {
      this.faradayError = error.message;
      for (const name of ['envelopeProgram', 'kernelProgram', 'shakeStateProgram', 'shakenEvolveProgram']) {
        this[name]?.dispose();
        this[name] = null;
      }
      // eslint-disable-next-line no-console
      console.error(error);
    }
    this.faradayCompiled = this.faradayError === null;
    this.envelope = [new PassBuffer(gl, { filter: 'nearest' }), new PassBuffer(gl, { filter: 'nearest' })];
    this.driveTexture = null;
    this.driveTableData = new Float32Array(K_MAX_STEPS * 2);

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

    // Faraday.
    this.bins = new Float32Array(AUDIO_BINS);
    this.bass = new Bass();
    this.strobe = new Strobe();
    this.lastShake = null;
    this.wasShaken = false;
    this.shakeFrame = 0; // numbers the seed
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

  /// `ensureFaraday()`. The envelope and its transforms, RG32F on the grid;
  /// nothing in them outlives the frame. The drive table at its largest, once.
  ensureFaraday() {
    const gl = this.gl;
    for (const b of this.envelope) b.ensure(this.grid.nx, this.grid.ny, gl.RG32F);
    if (this.driveTexture === null) {
      this.driveTexture = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, this.driveTexture);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RG32F, K_MAX_STEPS, 1, 0, gl.RG, gl.FLOAT, new Float32Array(K_MAX_STEPS * 2));
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      gl.bindTexture(gl.TEXTURE_2D, null);
    }
  }

  uploadDriveTable(steps) {
    const gl = this.gl;
    gl.bindTexture(gl.TEXTURE_2D, this.driveTexture);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, steps, 1, gl.RG, gl.FLOAT, this.driveTableData.subarray(0, steps * 2));
    gl.bindTexture(gl.TEXTURE_2D, null);
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
    return currentWater(params);
  }

  /// `ShakeState()`: the envelope and the saturated state, passes a to e.
  /// Returns the texture the inject pass should read as its state.
  shakeState(shaken, dt) {
    const gl = this.gl;
    const grid = this.grid;
    const state = this.surface[this.surfaceIndex].texture;

    // a. The envelope, into envelope[0].
    this.envelope[0].bind();
    this.envelopeProgram.use();
    bindTexture(gl, 0, state);
    this.envelopeProgram.setSampler('StateTexture', 0);
    this.envelopeProgram.set('OmegaC', shaken.omegaC);
    this.envelopeProgram.set('RateScale', K_RATE_SCALE);
    this.quad.draw();
    let current = 0;

    // b to d. The kernel, in Fourier space. Stripes' kernel is 1, and the
    // round trip would only add its rounding: it is skipped.
    if (shaken.pattern !== PATTERN_STRIPES) {
      current = this.transform(this.envelope, current, -1.0);
      this.envelope[1 - current].bind();
      this.kernelProgram.use();
      bindTexture(gl, 0, this.envelope[current].texture);
      this.kernelProgram.setSampler('Spectrum', 0);
      this.setInts(this.kernelProgram, 'GridSize', grid.nx, grid.ny);
      this.kernelProgram.set('Domain', grid.lx, grid.ly);
      this.kernelProgram.set('Wavenumber', shaken.wavenumber);
      this.kernelProgram.set('Width', K_NOTCH_WIDTH);
      this.kernelProgram.setInt('Pattern', shaken.pattern);
      this.kernelProgram.set('Norm', 1 / (grid.nx * grid.ny));
      this.quad.draw();
      current = 1 - current;
      current = this.transform(this.envelope, current, 1.0);
    }

    // e. Saturated, into the other envelope buffer.
    this.envelope[1 - current].bind();
    this.shakeStateProgram.use();
    bindTexture(gl, 0, state);
    bindTexture(gl, 1, this.envelope[current].texture);
    this.shakeStateProgram.setSampler('StateTexture', 0);
    this.shakeStateProgram.setSampler('EnvelopeTexture', 1);
    this.shakeStateProgram.set('SatRate', shaken.satRate);
    this.shakeStateProgram.set('Dt', dt);
    this.quad.draw();
    bindTexture(gl, 1, null);
    return this.envelope[1 - current].texture;
  }

  /// `Simulate()`: inject, forward, evolve, inverse. A shaken frame first
  /// saturates the state (passes a to e), and evolves every mode as a damped
  /// Mathieu oscillator instead.
  simulate(params, impacts, dt, shaken = null) {
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

    // A shaken frame first saturates the state (passes a to e); the inject
    // pass then reads that instead of last frame's surface.
    const state = shaken ? this.shakeState(shaken, dt) : this.surface[this.surfaceIndex].texture;

    // 1. Inject.
    this.spectrum[0].bind();
    this.inject.use();
    bindTexture(gl, 0, state);
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

    // 3. Evolve, into surface[0]. Last frame's surface has been read by the
    // inject pass and nothing needs it again.
    if (shaken) {
      // 3'. The shaken evolve.
      this.surface[0].bind();
      const e = this.shakenEvolveProgram;
      e.use();
      bindTexture(gl, 0, this.spectrum[current].texture);
      bindTexture(gl, 1, this.driveTexture);
      bindTexture(gl, 2, this.twiddleX);
      bindTexture(gl, 3, this.twiddleY);

      // The seed, as a white field's RMS a cell: a Wiener process in time
      // normalised by the cell's area.
      const cellArea = (grid.lx / grid.nx) * (grid.ly / grid.ny);
      const seedAmp = dt > 0 ? K_SEED * Math.sqrt(dt / cellArea) : 0;

      const steps = shaken.steps;
      e.setSampler('Spectrum', 0);
      e.setSampler('DriveTable', 1);
      e.setSampler('TwiddleX', 2);
      e.setSampler('TwiddleY', 3);
      this.setInts(e, 'GridSize', grid.nx, grid.ny);
      e.set('Domain', grid.lx, grid.ly);
      e.set('Gravity', water.gravity);
      e.set('Tension', water.tension);
      e.set('Depth', water.depth);
      e.set('Viscosity', water.viscosity);
      e.set('WallDamping', shaken.walls);
      e.set('Accel', shaken.accel);
      e.set('Step', steps > 0 ? dt / steps : 0);
      e.setInt('Steps', steps);
      e.set('Norm', 1 / (grid.nx * grid.ny));
      e.set('RateScale', K_RATE_SCALE);
      e.set('Wavenumber', shaken.wavenumber);
      e.set('SeedWidth', K_SEED_WIDTH);
      e.set('SeedAmp', seedAmp);
      e.setInt('Seed', this.shakeFrame & 0x7fffffff);
      this.shakeFrame = (this.shakeFrame + 1) >>> 0;
      e.setInt('Walls', walls);
      this.quad.draw();
      for (let unit = 3; unit >= 1; unit -= 1) bindTexture(gl, unit, null);

      // 4. Inverse: the surface, and next frame's state.
      this.surfaceIndex = this.transform(this.surface, 0, 1.0);
      return;
    }

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

  render({ input, params, width, height, time, variant }) {
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

    // `UpdateAudio()`, the bass half: the generated test bass stands in for
    // the host's FFT buffer (see the note at the top), and the plugin's
    // envelope reads it every frame whether shaken or not, on the host's
    // clock rather than the water's.
    (TEST_BASS.find((b) => b.id === variant) ?? TEST_BASS[0]).write(this.bins, time);
    this.bass.update(this.bins, AUDIO_BINS, bassBinsFromParam(params.get('bassBins')), hostDt);

    // The pond's own clock. Unshaken, the water covers exactly this.
    const nominal = hostDt * speedFromParam(params.get('speed'));

    // Faraday. At Shake 0 nothing below runs and the frame is v0.1.0's.
    // Shaken, the stroboscope decides how much water time the frame covers.
    let shake = null;
    const shaking = this.faradayCompiled && shakeFromParam(params.get('shake')) > 0;
    if (shaking && !this.wasShaken) this.strobe.reset();
    if (shaking) shake = planShake(params, this.bass, this.strobe, nominal, this.driveTableData);
    this.wasShaken = shaking;
    this.lastShake = shake;

    const dt = shaking ? shake.waterStep : nominal;
    this.simTime += dt;

    //-----------------------------------------------------------------------
    // The pond, in metres.
    //-----------------------------------------------------------------------
    this.frameHeight = pondSizeFromParam(params.get('pondSize'));
    this.frameWidth = f32(this.frameHeight * pictureWidth / pictureHeight);
    const detail = params.option('detail');
    const wanted = chooseGrid(this.frameWidth, this.frameHeight, DETAIL_CELLS[detail]);
    this.ensureBuffers(wanted, params.option('banks'), pictureWidth, pictureHeight);

    // The shaken frame's buffers and its drive table.
    if (shaking) {
      this.ensureFaraday();
      if (shake.steps > 0) this.uploadDriveTable(shake.steps);
    }

    if (this.stillWanted) {
      for (const b of this.surface) b.clearTo(0, 0, 0, 0);
      this.pending = [];
      this.stillWanted = false;
    }

    const impacts = this.collectImpacts(params, dt);
    if (shaking && dt > K_MAX_SHAKEN_STEP) {
      // A long shaken frame -- a stroboscope's jump, a slow frame, a high
      // Speed -- in equal chunks, each saturated and evolved on its own. The
      // drive's phase runs on in double from chunk to chunk; the frame's
      // craters go into the first.
      const chunks = Math.min(Math.ceil(dt / K_MAX_SHAKEN_STEP), K_MAX_CHUNKS);
      const h = dt / chunks;
      const omegaDrive = K_TWO_PI_D * shake.driveHz;
      const chunk = { ...shake, waterStep: h };
      let steps = 0;
      for (let i = 0; i < chunks; i += 1) {
        chunk.theta0 = wrap(shake.theta0 + omegaDrive * h * i);
        chunk.steps = driveTable(chunk.theta0, omegaDrive, h, this.driveTableData);
        this.uploadDriveTable(chunk.steps);
        this.simulate(params, i === 0 ? impacts : [], h, chunk);
        steps += chunk.steps;
      }
      this.lastShake = { ...shake, steps, chunks };
    } else if (dt > 0 || impacts.length) {
      this.simulate(params, impacts, dt, shake);
    }

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

    reportFrame(this, params);
  }
}

//===========================================================================
// The line under the picture: what the test bass is putting in, and what the
// plugin's own plan made of it this frame. The page's, not the plugin's.
//===========================================================================

let readoutNode = null;
let readoutAt = 0;

function reportFrame(renderer, params) {
  if (!readoutNode) return;
  const now = performance.now();
  if (now - readoutAt < 150) return;
  readoutAt = now;

  const level = renderer.bass.level();
  const bass = `Bass envelope ${level.toFixed(2)} (Bass Bins ${bassBinsFromParam(params.get('bassBins'))})`;
  if (!renderer.faradayCompiled) {
    readoutNode.textContent = `${bass} · Shake is disabled: a Faraday shader did not compile in this browser, so the pond is drawn as v0.1.0 drew it, as the plugin does when a driver refuses one. ${renderer.faradayError}`;
    return;
  }
  const shake = renderer.lastShake;
  if (!shake) {
    readoutNode.textContent = `${bass} · Shake 0: none of the Faraday passes runs.`;
    return;
  }
  const lambda = shake.wavenumber > 0 ? 2 * K_PI / shake.wavenumber : 0;
  const ratio = shake.threshold > 0 ? shake.accel / shake.threshold : 0;
  readoutNode.textContent =
    `${bass} · shaken at ${ratio.toFixed(2)} × threshold, ${shake.accel.toFixed(2)} of ${shake.threshold.toFixed(2)} m/s²`
    + ` · lattice at ${(shake.driveHz / 2).toFixed(2)} Hz, λ ${length(lambda)}`
    + ` · ${(shake.waterStep * 1000).toFixed(1)} ms of water this frame in ${shake.chunks} ${shake.chunks === 1 ? 'pass' : 'passes'}, ${shake.steps} substeps`;
}

//===========================================================================
// The parameters, from the constructor in Millpond.cpp. Audio, Audio Pebbles
// and Audio Rain are left out, and so is the About block: see the note at the
// top. Faraday comes last, as in the plugin, where it follows About.
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

  std('shake', 'Shake', 0.0, 'Faraday', {
    display: (v) => (shakeFromParam(v) > 0 ? `${shakeFromParam(v).toFixed(2)} × threshold` : 'off'),
    hint: 'The bass shakes the tank up and down, and above the first Mathieu tongue’s threshold a lattice of standing waves grows at half the drive frequency. This is how far past that threshold the loudest bass drives it, 0 to 4 times; 0 is off, and none of the Faraday passes runs. In units of the threshold because the threshold moves with Drive, Damping and the water. Here the bass is the generated test bass under the picture, not music.' }),
  std('drive', 'Drive', paramFromDrive(10.0), 'Faraday', {
    display: (v) => `${driveFromParam(v).toFixed(1)} Hz`,
    hint: 'The tank’s shaking frequency, 5 to 120 Hz. The lattice answers at half of it, with the wavelength millpond’s own dispersion relation gives at that frequency: a higher drive, a finer lattice.' }),
  std('damping', 'Damping', paramFromDamping(2.0), 'Faraday', {
    display: (v) => (dampingFromParam(v) > 0 ? `${dampingFromParam(v).toFixed(2)} /s` : 'none (Lamb’s only)'),
    hint: 'The walls’ extra damping, added to Lamb’s 2νk² for every mode: exactly 0 at the bottom, then 0.1 to 30 per second. It sets how quickly the lattice lies down when the bass stops. The default 2 is a little under Benjamin & Ursell’s measured tube.' }),
  std('slowMotion', 'Slow Motion', paramFromSlowMotion(8.0), 'Faraday', {
    display: (v) => (slowMotionFromParam(v) > 1 + 1e-9 ? `${slowMotionFromParam(v).toFixed(1)}×` : 'off'),
    hint: 'A stroboscope, 1 to 100 times. The lattice’s oscillation is seen this many times slower while the water keeps real time on average, so the lattice still stands up and lies down with the bass; rain and pebbles are seen in jumps. It acts whenever Shake is above 0. Unslowed, a 10 Hz drive’s lattice turns inside out ten times a second; at the default 8× it is 1.25 a second.' }),
  { id: 'pattern', name: 'Pattern', type: 'option', default: 1, group: 'Faraday', elements: ['Stripes', 'Squares', 'Hexagons'],
    hint: 'Which lattice the shaken pond settles into. Each is a kernel on the saturating damping, notched where that pattern’s pairs of waves beat, and the dynamics then pick the pattern; nothing is drawn. The kernel is the plugin’s choice, not the fluid’s.' },
  std('bassBins', 'Bass Bins', paramFromBassBins(2), 'Faraday', {
    display: (v) => `${bassBinsFromParam(v)} ${bassBinsFromParam(v) === 1 ? 'bin' : 'bins'}`,
    hint: 'How many of the host’s lowest FFT bins count as the bass, 1 to 16. Nobody has measured what frequencies Resolume’s bins are, so the plugin exposes the assumption instead of making it. Here those bins hold the page’s generated test bass.' }),
];

const mounted = mountDemo({
  name: 'Millpond',
  pluginId: 'MP01',
  tagline: 'Ripples. The clip is a pond bed seen straight down through water that obeys linear water-wave theory exactly: every frame an FFT, every mode advanced as a damped oscillator, and back — then Snell, a light-conserving caustic net, Fresnel sky and the sun’s glints. Shake the tank with the bass and Faraday’s lattice stands up at half the drive frequency.',
  repo: 'https://github.com/stoatworks-labs/millpond',
  page: 'https://stoatworks-labs.com/software/millpond/',
  video: 'https://www.youtube.com/watch?v=oAbXjVXFwYA',

  // The stock middle clause, plus the one thing it would leave a visitor to
  // assume: that Shake is listening to something.
  blurb: 'It is Millpond’s own GLSL, ported from the repository to WebGL2 and running on generated clips in this page — same parameters, same maths, no install. There is no audio here: Shake is driven by a generated test bass that this page writes into the plugin’s 64-bin spectrum itself. That is not Resolume’s FFT, whose bins nobody has measured.',

  // The test bass, in the kit's one transport dropdown (needle's precedent):
  // which spectrum stands in for the host's FFT buffer. Not a parameter.
  variants: {
    label: 'Test bass',
    default: TEST_BASS_DEFAULT,
    options: TEST_BASS.map(({ id, name, hint }) => ({ id, name, hint })),
  },

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
    'The twelve shaders are the plugin’s own text — the eight that move and light the pond, and the four Faraday passes (the envelope, the pattern kernel, the saturated state and the shaken evolve) — and demo/tools/check_shaders.py proves it. Everything the CPU does is a hand port to JavaScript: the control conversions, the dispersion relation and group velocity used to size the absorber, the skim schedule, the PCG random numbers and the Poisson rain, the grid choice, the shaken frame’s plan (the critical wavenumber, the threshold, the bass envelope, the stroboscope and the drive’s cosines, in double), and the frame sequence — saturate, inject, forward FFT, evolve, inverse FFT, in 1/30 s chunks when a shaken frame is longer, then caustic mesh, blur, composite. Nothing checks that port but a reader; the shaken plan was held against the C++ once, by hand.',
    'Drop, Skim and Still are FF_TYPE_EVENT buttons in the plugin. The kit has no event type, so each is a toggle here that the renderer takes and releases on the frame it acts — which is what a host does with an event anyway, and why the button blinks. In Resolume the plugin also takes several presses in one frame; here a press is one frame long.',
    'There is no audio. The plugin reads Resolume’s 64-bin FFT buffer once a frame; a browser has no Resolume, and this page does not ask for a microphone. Instead it writes a generated test bass into those 64 bins itself, chosen under the picture as “Test bass”: a steady drone (the plugin harness’s own feed: the lowest four bins at 1, the rest at 0.05), the same drone 4 s on and 4 s off, a kick twice a second over a pad (the harness’s other feed), or silence. Shake reads it through the plugin’s own bass envelope, ported: the mean of √bin over Bass Bins bins, a 5 ms attack and 80 ms release, normalised to its own peak over the last few seconds. It is not Resolume’s FFT, and nobody has measured what Resolume’s bins are.',
    'Audio, Audio Pebbles and Audio Rain are absent rather than present and dead. The plugin’s onset analyser behind them is not ported. Both amounts default to 0, and at 0 the plugin drops no pebble and adds no rain whatever the spectrum holds, so with the test bass playing the page still drops exactly the pebbles and rain the plugin would at those defaults.',
    'Faraday has never been in a host. The plugin’s shaken tank is checked by its harness on one Mac’s GPU and on Apple’s software renderer; it has never been loaded in Arena, built for Windows, or run on any other GPU. This page is evidence about none of those. If a Faraday shader will not compile in this browser, Shake does nothing and the line under the picture says so — as the plugin does when a driver refuses one.',
    'Float render targets and filtering. The water needs EXT_color_buffer_float (the spectrum, the surface, the shaken envelope and the caustics are 32-bit float targets), EXT_float_blend (the caustic mesh adds into a 32-bit float buffer) and OES_texture_float_linear (the surface and the caustics are read with a linear filter). All three are core in the plugin’s GL 4.1 and optional in WebGL2; the page stops with a message if one is missing rather than render a still pond.',
    'The clock is this page’s, in seconds, so the plugin’s vote on whether the host sends seconds or milliseconds has nothing to decide. Each frame is still clamped to a quarter of a second, as in the plugin. Restart stills the water and starts the test bass from the top; the plugin has no such control.',
    'The About block is not here; the links are in the header.',
  ],

  createRenderer: (gl, quad) => new MillpondRenderer(gl, quad),
});

//---------------------------------------------------------------------------
// Under the picture: a standing note that the bass is generated, and a line
// that says, every frame, what the plugin's own plan made of it. Not in embed
// mode, which has no transport and no reader.
//---------------------------------------------------------------------------
const transportBar = document.querySelector('.transport');
if (transportBar && mounted) {
  const note = document.createElement('p');
  note.className = 'stage__status';
  note.textContent =
    'Test bass, not audio. Shake is driven by the spectrum chosen above, which this page writes into the plugin’s 64 FFT bins itself. That is not Resolume’s FFT: the plugin’s own bass envelope runs on it, but what Resolume’s bins hold has never been measured.';
  readoutNode = document.createElement('p');
  readoutNode.className = 'stage__status stage__readout';
  transportBar.after(note, readoutNode);
}
