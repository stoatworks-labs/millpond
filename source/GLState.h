#pragma once

#include <FFGLSDK.h>

namespace millpond
{
/**
    The GL state this plugin changes, captured so it can be put back.

    Lifted from vectrix, which lifted it from `resolume-scopes`' `Scopes.cpp`
    unchanged, because the requirement is unchanged: FFGL requires the context to be returned in a
    default state, and Resolume renders the rest of the composition with
    whatever it finds. A plugin that leaves additive blending on is a plugin
    that makes the *next* effect in the chain look broken, which is where the
    bug report will come from.

    `ScopedGLState` is the only addition. The renderer has several early
    returns -- a buffer that would not allocate, a shader that did not compile,
    a sample count of one -- and every one of them has to put the state back.
    Doing that by hand at each `return` is how one of them ends up missing it.
*/
struct SavedGLState
{
	GLint viewport[ 4 ];
	GLboolean blend;
	GLint blendSrcRGB;
	GLint blendDstRGB;
	GLint blendSrcAlpha;
	GLint blendDstAlpha;
	GLboolean programPointSize;
	GLfloat clearColour[ 4 ];
	GLboolean depthTest;
	GLboolean cullFace;
	GLboolean scissorTest;
	GLint vertexArray;
	GLint activeTexture;

	void Capture()
	{
		glGetIntegerv( GL_VIEWPORT, viewport );
		//The clear colour is state too, and the passes clear their buffers
		//with their own. A host that clears with its colour after we return
		//would otherwise clear to ours -- and an offline harness never sees
		//it, because it sets its own before every frame.
		glGetFloatv( GL_COLOR_CLEAR_VALUE, clearColour );
		//FFGL promises the host hands over default state, but these three
		//would silently eat geometry if one did not: a depth test rejects
		//overlapping triangles at the same depth, culling drops the ones a
		//fold turns over, a scissor clips a pass. Off for our passes, and
		//put back as found.
		depthTest   = glIsEnabled( GL_DEPTH_TEST );
		cullFace    = glIsEnabled( GL_CULL_FACE );
		scissorTest = glIsEnabled( GL_SCISSOR_TEST );
		blend = glIsEnabled( GL_BLEND );
		glGetIntegerv( GL_BLEND_SRC_RGB, &blendSrcRGB );
		glGetIntegerv( GL_BLEND_DST_RGB, &blendDstRGB );
		glGetIntegerv( GL_BLEND_SRC_ALPHA, &blendSrcAlpha );
		glGetIntegerv( GL_BLEND_DST_ALPHA, &blendDstAlpha );
		programPointSize = glIsEnabled( GL_PROGRAM_POINT_SIZE );
		glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &vertexArray );
		glGetIntegerv( GL_ACTIVE_TEXTURE, &activeTexture );
	}

	void Restore() const
	{
		glViewport( viewport[ 0 ], viewport[ 1 ], viewport[ 2 ], viewport[ 3 ] );
		glClearColor( clearColour[ 0 ], clearColour[ 1 ], clearColour[ 2 ], clearColour[ 3 ] );
		if( depthTest )
			glEnable( GL_DEPTH_TEST );
		if( cullFace )
			glEnable( GL_CULL_FACE );
		if( scissorTest )
			glEnable( GL_SCISSOR_TEST );
		glBlendFuncSeparate( blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha );
		if( blend )
			glEnable( GL_BLEND );
		else
			glDisable( GL_BLEND );
		if( programPointSize )
			glEnable( GL_PROGRAM_POINT_SIZE );
		else
			glDisable( GL_PROGRAM_POINT_SIZE );
		//Put back the host's, not 0: a host that keeps a vertex array bound
		//across plugin calls would otherwise find it gone.
		glBindVertexArray( static_cast< GLuint >( vertexArray ) );
		glActiveTexture( static_cast< GLenum >( activeTexture ) );
	}
};

/// Capture on the way in, restore on the way out, whichever way out it is.
struct ScopedGLState
{
	SavedGLState saved;

	ScopedGLState()
	{
		saved.Capture();
		glDisable( GL_DEPTH_TEST );
		glDisable( GL_CULL_FACE );
		glDisable( GL_SCISSOR_TEST );
	}
	~ScopedGLState()
	{
		saved.Restore();
	}

	ScopedGLState( const ScopedGLState& ) = delete;
	ScopedGLState& operator=( const ScopedGLState& ) = delete;
};

/// Unbind texture units 1 .. count-1 by hand, leaving unit 0 active.
///
/// Call it after the draw, inside the scope of a pass that binds THREE or
/// more units. Every `ffglex::Scoped2DTextureBinding` clears to 0 on exit on
/// whichever unit is active THEN, and every `ScopedSamplerActivation` sets
/// unit 0 on exit -- so unwinding three interleaved pairs clears unit 2, sets
/// unit 0, and then "clears unit 1" on unit 0. Unit 1 stays bound into the
/// host's context, and after DeInitGL it is a deleted texture. Two pairs
/// happen to unwind correctly, which is why the pattern looked safe.
inline void releaseTextureUnits( int count )
{
	for( int unit = count - 1; unit >= 1; --unit )
	{
		glActiveTexture( static_cast< GLenum >( GL_TEXTURE0 + unit ) );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
	glActiveTexture( GL_TEXTURE0 );
}

/// Additive. The caustic mesh is drawn with this, so where the water folds the
/// light over on itself the layers add -- which is what a caustic IS.
inline void setAdditiveBlend()
{
	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ONE );
}

/// Premultiplied "over", for putting a finished image onto the output.
inline void setOverBlend()
{
	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
}

} // namespace millpond
