#pragma once

/**
	The GLSL. There is very little of it on purpose.

	The chipset has already decided every pixel of the field, in 12-bit colour,
	on the CPU. What is left for the GPU is to put that 322 x 258 texture (the
	playfield plus a one-texel ring of border colour) on the output at the
	scale the operator chose:

	  - **Integer and Fit** fetch one texel with `texelFetch` at
	    `floor( u ) + 1`. No filtering, no blending: every output pixel is
	    exactly a texel, which is what lets `cptest --palette` demand
	    multiples of 17 and `--scaling` demand the field replicated bit for bit.
	  - **Fit Smooth** blends four texels by hand, so the weights are the
	    shader's and not the sampler's.

	Outside the playfield the fetch clamps into the ring, which is the border
	colour as the beam left it on that line -- so a letterbox shows copper
	bars running on into the border, as a monitor would.

	`#version 410 core`. No reserved words as identifiers (`patch`, `sample`,
	`input`, `output`, `filter`, `common`, `active`, `half`, `layout`, `flat`).
*/
namespace copperlist
{
extern const char* const kVertexShader;
extern const char* const kFragmentShader;

} // namespace copperlist
