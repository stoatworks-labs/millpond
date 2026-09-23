#pragma once

#include "Audio.h"
#include "Controls.h"
#include "PassBuffer.h"
#include "Physics.h"
#include "Shaders.h"

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

#include <deque>
#include <vector>

namespace millpond
{
/**
    The plugin.

    Two models in sequence: the water (Shaders.h passes 1-4, the state kept
    between frames) and the light (passes 5-7, a pure function of the surface
    and the picture). See AGENTS.md for why each is built the way it is.
*/
class MillpondPlugin : public CFFGLPlugin
{
public:
	MillpondPlugin();

	FFResult InitGL( const FFGLViewportStruct* viewport ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* input ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters on
	/// a fresh instance and deletes the instance if one fails, and
	/// CFFGLPlugin's SetTextParameter is a stub that returns exactly that
	/// failure -- so without this override no real host can load the plugin,
	/// while every offline harness here carries on passing.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	FFResult SetTime( double time ) override;

	//-------------------------------------------------------------------
	// For the harness. Nothing in the plugin's own operation calls these;
	// they exist so `mptest` can measure the surface the shipping passes
	// produced, rather than a copy of it computed for the test.
	//-------------------------------------------------------------------

	/// The offline harness DECLARES its clock unit (seconds) rather than
	/// letting the plugin vote on it against the wall clock -- a harness
	/// renders hundreds of frames a second, which the vote would read as
	/// milliseconds.
	void SetClockScaleForTest( double scale );

	/// (eta, eta_t / kRateScale, eta_x, eta_y) on the grid, metres. 0 before
	/// the first frame.
	GLuint SurfaceTextureID() const;

	/// The blurred caustic buffer, bed coordinates. 0 if caustics are off.
	GLuint CausticTextureID() const;
	int CausticWidth() const
	{
		return causticWidth;
	}
	int CausticHeight() const
	{
		return causticHeight;
	}

	/// The grid the last frame ran on.
	const Grid& CurrentGrid() const
	{
		return grid;
	}

	/// The water the current parameters describe.
	Water CurrentWater() const;

	/// The frame, in metres, as of the last frame.
	float FrameWidthMetres() const
	{
		return frameWidth;
	}
	float FrameHeightMetres() const
	{
		return frameHeight;
	}

	/// Seconds of water since the plugin started.
	double SimTime() const
	{
		return simTime;
	}

	/// Replace the surface with (eta, eta_t / kRateScale, eta_x, eta_y) RGBA floats, one
	/// per grid cell, row 0 first. With Speed at 0 and nothing dropping it
	/// then stays exactly as given, so the optics can be measured against a
	/// surface whose every slope is known.
	void LoadSurfaceForTest( const std::vector< float >& rgba );

	/// Run one step of the water on whatever is in it, with no impacts --
	/// exactly the chain a frame runs. With dt = 0 it is the transform's
	/// round trip, which is what `--fft` measures.
	void StepForTest( double dt )
	{
		Simulate( {}, dt );
	}

	/// Every impact injected so far, in order. For `--skim` and `--rain`.
	const std::vector< Impact >& InjectedForTest() const
	{
		return injectedLog;
	}
	void KeepInjectedLog( bool keep )
	{
		logInjected = keep;
	}

	/// Where the last caustic mesh sampled, and how finely -- for --bench.
	int MeshColumns() const
	{
		return meshColumns;
	}
	int MeshRows() const
	{
		return meshRows;
	}

private:
	void UpdateClock();
	void UpdateAudio( double dt );

	/// Everything that falls into the water this frame, in frame metres.
	std::vector< Impact > CollectImpacts( double dt );
	void RandomPoint( float centreX, float centreY, float scatter, float& x, float& y );

	/// Allocate every buffer for this grid and raster. Called once per frame
	/// before anything binds a texture, and a no-op unless something changed.
	/// All of it up front: every `ffglex::Scoped*` binding clears to 0 on
	/// scope exit rather than restoring, and `FFGLFBO::Initialise` binds
	/// under one -- so allocating mid-chain silently unbinds the texture the
	/// current pass is reading, on exactly the frame that allocates.
	bool ensureBuffers( const Grid& wanted, GLsizei width, GLsizei height );
	bool ensureMesh( int columns, int rows );

	void Simulate( const std::vector< Impact >& impacts, double dt );
	void Transform( PassBuffer ( &buffers )[ 2 ], int& current, float direction );
	static GLuint MakeTwiddles( int length );
	void Caustics( float depth, const float sunTravel[ 3 ], float sunRadiusRadians );

	float params[ PT_COUNT ] = {};

	ffglex::FFGLShader injectShader;
	ffglex::FFGLShader fftShader;
	ffglex::FFGLShader evolveShader;
	ffglex::FFGLShader causticShader;
	ffglex::FFGLShader blurShader;
	ffglex::FFGLShader compositeShader;

	ffglex::FFGLScreenQuad quad;

	PassBuffer spectrum[ 2 ];///< RG32F: the forward transform's ping-pong
	PassBuffer surface[ 2 ]; ///< RGBA32F: the inverse transform's, and the state
	PassBuffer caustic[ 2 ]; ///< R32F: the mesh's deposit, and the blur's other half
	GLuint twiddleX = 0;     ///< RG32F, Nx x 1: exp( -2 pi i m / Nx ), from doubles
	GLuint twiddleY = 0;     ///< RG32F, Ny x 1
	int surfaceIndex  = 0;
	int causticIndex  = 0;
	bool haveCaustics = false;

	Grid grid;
	int bufferBanks   = -1;
	int causticWidth  = 0;
	int causticHeight = 0;

	GLuint meshVAO      = 0;///< empty: the mesh is generated from gl_VertexID
	int meshColumns     = 0;
	int meshRows        = 0;
	GLsizei meshIndices = 0;

	float frameWidth  = 1.0f;
	float frameHeight = 1.0f;

	//-------------------------------------------------------------------
	// Time. See rosette: the host's clock unit is voted on against the wall
	// clock, because Resolume has sent both seconds and milliseconds.
	//-------------------------------------------------------------------
	double hostTime     = -1.0;
	double lastRawTime  = -1.0;
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	double clockScale   = 0.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	int clockFrames     = 0;
	double now          = 0.0;
	double lastNow      = -1.0;
	double simTime      = 0.0;
	bool settledJump    = false;///< the unit vote settled this frame: no dt across it

	//-------------------------------------------------------------------
	// Events.
	//-------------------------------------------------------------------
	int dropPresses  = 0;
	int skimPresses  = 0;
	bool stillWanted = false;
	bool dropHeld    = false;
	bool skimHeld    = false;
	bool stillHeld   = false;

	std::deque< Impact > pending;///< a skim's later touches, in time order
	Random random;
	audio::Analyser analyser;

	std::vector< Impact > injectedLog;
	bool logInjected = false;
};

} // namespace millpond
