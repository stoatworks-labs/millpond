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

	void Capture()
	{
		glGetIntegerv( GL_VIEWPORT, viewport );
		blend = glIsEnabled( GL_BLEND );
		glGetIntegerv( GL_BLEND_SRC_RGB, &blendSrcRGB );
		glGetIntegerv( GL_BLEND_DST_RGB, &blendDstRGB );
		glGetIntegerv( GL_BLEND_SRC_ALPHA, &blendSrcAlpha );
		glGetIntegerv( GL_BLEND_DST_ALPHA, &blendDstAlpha );
		programPointSize = glIsEnabled( GL_PROGRAM_POINT_SIZE );
	}

	void Restore() const
	{
		glViewport( viewport[ 0 ], viewport[ 1 ], viewport[ 2 ], viewport[ 3 ] );
		glBlendFuncSeparate( blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha );
		if( blend )
			glEnable( GL_BLEND );
		else
			glDisable( GL_BLEND );
		if( programPointSize )
			glEnable( GL_PROGRAM_POINT_SIZE );
		else
			glDisable( GL_PROGRAM_POINT_SIZE );
		glBindVertexArray( 0 );
	}
};

/// Capture on the way in, restore on the way out, whichever way out it is.
struct ScopedGLState
{
	SavedGLState saved;

	ScopedGLState()
	{
		saved.Capture();
	}
	~ScopedGLState()
	{
		saved.Restore();
	}

	ScopedGLState( const ScopedGLState& ) = delete;
	ScopedGLState& operator=( const ScopedGLState& ) = delete;
};

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
