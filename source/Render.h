#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Controls.h"

#include <FFGLSDK.h>

/**
	The GPU's whole job: one texture, one triangle, into the host's FBO.

	## Layout is decided here, on the CPU

	`ComputeLayout` turns the output raster, the scaling mode and the pixel
	aspect into an origin and a scale in output pixels. The shader does
	nothing but apply them, and `cptest` calls the same function to know
	where each playfield pixel landed, so a check never transcribes the
	layout.

	**Integer** is the largest whole multiple that fits, centred at a whole
	pixel. When not even x1 fits -- 320 x 180, where 256 lines are taller
	than the raster -- it stays at x1, centred, and the top and bottom of the
	field are cropped the way a monitor's overscan crops them. Integer ignores
	`Pixel Aspect`: a whole multiple cannot be 1.04 wide.

	**Fit** and **Fit Smooth** take the largest scale that fits, with the
	standard's pixel aspect if `Pixel Aspect` is Non-square. The aspect is
	derived from the pixel clock, not remembered: a low-resolution pixel is
	two colour clocks' worth of line (HRM 2: 3,546,895 Hz PAL, 3,579,545 Hz
	NTSC), and a square pixel on a 625- or 525-line raster is 14.75 MHz or
	12 3/11 MHz of it (the broadcast square-pixel rates). A non-interlaced line
	is two frame lines tall, so the width over the height is
	`square / ( 2 x lores )`: **1.0397 for PAL, 0.8571 for NTSC**.
*/
namespace copperlist
{
struct Layout
{
	float originX = 0.0f, originY = 0.0f;///< output pixels, y down
	float scaleX = 1.0f, scaleY = 1.0f;  ///< output pixels per playfield pixel
	int   width = 0, height = 0;         ///< the output raster
	int   playW = 320, playH = 256;
	bool  smooth     = false;
	int   background = 0;
};

/// Pixel aspect (width / height) of a low-resolution pixel.
double PixelAspect( bool ntsc );

Layout ComputeLayout( int width, int height, int playW, int playH, Scaling scaling, Aspect aspect,
					  bool ntsc, Background background );

class Renderer
{
public:
	bool InitGL();
	void DeInitGL();

	/// Upload the chipset's picture: (playW + 2) x (playH + 2) RGBA8.
	void Upload( const std::vector< uint8_t >& rgba, int width, int height );

	void Draw( const Layout& layout, GLuint hostFBO );

	const std::string& Note() const { return mNote; }

private:
	ffglex::FFGLShader mProgram;
	GLuint             mVao     = 0;
	GLuint             mTexture = 0;
	int                mTexW = 0, mTexH = 0;
	bool               mReady = false;
	std::string        mNote;
};

} // namespace copperlist
