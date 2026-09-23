#include "Shaders.h"

namespace copperlist
{
const char* const kVertexShader = R"(#version 410 core
// One triangle that covers the viewport; the corners come from gl_VertexID.
void main()
{
	vec2 corner = vec2( ( gl_VertexID == 1 ) ? 3.0 : -1.0, ( gl_VertexID == 2 ) ? 3.0 : -1.0 );
	gl_Position = vec4( corner, 0.0, 1.0 );
}
)";

const char* const kFragmentShader = R"(#version 410 core
out vec4 fragColour;

uniform sampler2D uField;   // (W + 2) x (H + 2) RGBA8, top row first; the ring is the border
uniform ivec2 uPlayfield;   // W, H
uniform vec2  uOutput;      // output size in pixels
uniform vec2  uOrigin;      // playfield's top-left in output pixels, y down
uniform vec2  uScale;       // output pixels per playfield pixel, x and y
uniform int   uSmooth;      // 0 nearest, 1 bilinear
uniform int   uBackground;  // 0 border, 1 black, 2 transparent

vec4 texelAt( ivec2 t )
{
	return texelFetch( uField, clamp( t, ivec2( 0 ), uPlayfield + 1 ), 0 );
}

void main()
{
	// Pixel centre, y down, in the playfield's own pixels.
	vec2 p = vec2( gl_FragCoord.x, uOutput.y - gl_FragCoord.y );
	vec2 u = ( p - uOrigin ) / uScale;

	bool inside = u.x >= 0.0 && u.y >= 0.0 && u.x < float( uPlayfield.x ) && u.y < float( uPlayfield.y );
	if( !inside && uBackground == 1 )
	{
		fragColour = vec4( 0.0, 0.0, 0.0, 1.0 );
		return;
	}
	if( !inside && uBackground == 2 )
	{
		fragColour = vec4( 0.0 );
		return;
	}

	if( uSmooth == 0 )
	{
		fragColour = texelAt( ivec2( floor( u ) ) + 1 );
		return;
	}

	// Bilinear between texel centres, weights computed here.
	vec2  q  = u - 0.5;
	vec2  f0 = floor( q );
	vec2  w  = q - f0;
	ivec2 t  = ivec2( f0 ) + 1;
	vec4  a  = mix( texelAt( t ), texelAt( t + ivec2( 1, 0 ) ), w.x );
	vec4  b  = mix( texelAt( t + ivec2( 0, 1 ) ), texelAt( t + ivec2( 1, 1 ) ), w.x );
	fragColour = mix( a, b, w.y );
}
)";

} // namespace copperlist
