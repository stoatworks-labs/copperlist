#include "Render.h"

#include <algorithm>
#include <cmath>

#include "Shaders.h"

namespace copperlist
{
namespace
{
constexpr double kPalColourClock  = 3546895.0;
constexpr double kNtscColourClock = 3579545.0;
constexpr double kPalSquareRate   = 14750000.0;
constexpr double kNtscSquareRate  = 135.0e6 / 11.0;

int FloorDiv( int a, int b )
{
	return ( a >= 0 ) ? a / b : -( ( -a + b - 1 ) / b );
}
} // namespace

double PixelAspect( bool ntsc )
{
	const double lores = 2.0 * ( ntsc ? kNtscColourClock : kPalColourClock );
	return ( ntsc ? kNtscSquareRate : kPalSquareRate ) / ( 2.0 * lores );
}

Layout ComputeLayout( int width, int height, int playW, int playH, Scaling scaling, Aspect aspect, bool ntsc,
					  Background background )
{
	Layout l;
	l.width      = width;
	l.height     = height;
	l.playW      = playW;
	l.playH      = playH;
	l.background = static_cast< int >( background );
	l.smooth     = scaling == Scaling::FitSmooth;

	if( scaling == Scaling::Integer )
	{
		const int k = std::max( 1, std::min( width / playW, height / playH ) );
		l.scaleX = l.scaleY = static_cast< float >( k );
		l.originX = static_cast< float >( FloorDiv( width - k * playW, 2 ) );
		l.originY = static_cast< float >( FloorDiv( height - k * playH, 2 ) );
		return l;
	}

	const double par = ( aspect == Aspect::NonSquare ) ? PixelAspect( ntsc ) : 1.0;
	const double s   = std::min( width / ( playW * par ), static_cast< double >( height ) / playH );
	l.scaleX  = static_cast< float >( s * par );
	l.scaleY  = static_cast< float >( s );
	l.originX = static_cast< float >( ( width - playW * s * par ) * 0.5 );
	l.originY = static_cast< float >( ( height - playH * s ) * 0.5 );
	return l;
}

bool Renderer::InitGL()
{
	if( mReady )
		return true;
	mNote.clear();
	if( !mProgram.Compile( kVertexShader, kFragmentShader ) )
	{
		mNote = "the scaling shader would not compile";
		return false;
	}
	glGenVertexArrays( 1, &mVao );
	glGenTextures( 1, &mTexture );
	mReady = true;
	return true;
}

void Renderer::DeInitGL()
{
	if( mTexture != 0 )
	{
		glDeleteTextures( 1, &mTexture );
		mTexture = 0;
	}
	if( mVao != 0 )
	{
		glDeleteVertexArrays( 1, &mVao );
		mVao = 0;
	}
	mProgram.FreeGLResources();
	mTexW = mTexH = 0;
	mReady        = false;
}

void Renderer::Upload( const std::vector< uint8_t >& rgba, int width, int height )
{
	if( !mReady || rgba.size() < static_cast< size_t >( width ) * static_cast< size_t >( height ) * 4u )
		return;
	glBindTexture( GL_TEXTURE_2D, mTexture );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	if( width != mTexW || height != mTexH )
	{
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data() );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		mTexW = width;
		mTexH = height;
	}
	else
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data() );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
	glBindTexture( GL_TEXTURE_2D, 0 );
}

void Renderer::Draw( const Layout& l, GLuint hostFBO )
{
	if( !mReady || mTexW == 0 || l.width <= 0 || l.height <= 0 )
		return;

	glBindFramebuffer( GL_FRAMEBUFFER, hostFBO );
	glViewport( 0, 0, l.width, l.height );
	glDisable( GL_DEPTH_TEST );
	// A source owns its layer: the result is written, never blended with
	// whatever the host left in the buffer.
	glDisable( GL_BLEND );

	const GLuint prog = mProgram.GetGLID();
	glUseProgram( prog );
	glUniform1i( glGetUniformLocation( prog, "uField" ), 0 );
	glUniform2i( glGetUniformLocation( prog, "uPlayfield" ), l.playW, l.playH );
	glUniform2f( glGetUniformLocation( prog, "uOutput" ), static_cast< float >( l.width ), static_cast< float >( l.height ) );
	glUniform2f( glGetUniformLocation( prog, "uOrigin" ), l.originX, l.originY );
	glUniform2f( glGetUniformLocation( prog, "uScale" ), l.scaleX, l.scaleY );
	glUniform1i( glGetUniformLocation( prog, "uSmooth" ), l.smooth ? 1 : 0 );
	glUniform1i( glGetUniformLocation( prog, "uBackground" ), l.background );

	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, mTexture );
	glBindVertexArray( mVao );
	glDrawArrays( GL_TRIANGLES, 0, 3 );

	// Hand the context back: the scoped bindings clear rather than restore,
	// so this is done by hand.
	glBindVertexArray( 0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glUseProgram( 0 );
}

} // namespace copperlist
