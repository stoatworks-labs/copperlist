#pragma once

#include <cmath>

#include <FFGLSDK.h>

#include "StoatworksAboutParams.h"

/**
	Every parameter, and what its host-side value means.

	- **`FF_TYPE_STANDARD` is 0..1**, always: `SetParamInfo` clamps a STANDARD
	  default into that range before `SetParamRange` could widen it. Each one
	  is mapped to the demo's units by a named function below and nowhere else.
	- **`FF_TYPE_INTEGER` holds a real integer** with a real range: pixels per
	  field, lines, counts, planes. Those are the machine's own units, so they
	  are not dressed up as sliders.
	- **`FF_TYPE_OPTION` holds the element value**, which here is the index.
	  Its range reads back 0..1 whatever the element count (the fleet trap),
	  so `ToOption` in Copperlist.cpp maps by index and `cptest --list`
	  prints the real range for the sweep.
	- **`FF_TYPE_TEXT`**: the scroller's text, and the About line.

	## Order is load-bearing

	The host draws parameters in declaration order and `SetParamGroup`
	collapses runs, so the enum is the inspector, top to bottom, in the spec's
	groups: Text, Bars, Stars, Cube, Bobs, Machine.

	## The switches

	Every scene can be turned off, by the control that already means "how
	many": an empty Text, Bar Count 0, Star Count 0, Bob Count 0, Font Colour
	Cycle 0. The cube, which has no count, has `Cube On`.
*/
namespace copperlist
{
enum ParamId : FFUInt32
{
	// -- Text ----------------------------------------------------------------
	PT_TEXT,
	PT_SCROLL_SPEED,
	PT_WAVE_HEIGHT,
	PT_WAVE_LENGTH,
	PT_FONT_CYCLE,

	// -- Bars ----------------------------------------------------------------
	PT_BAR_COUNT,
	PT_BAR_HEIGHT,
	PT_BAR_SPEED,
	PT_BAR_WAVE,
	PT_BAR_PALETTE,

	// -- Stars ---------------------------------------------------------------
	PT_STAR_COUNT,
	PT_STAR_SPEED,
	PT_LAYERS,

	// -- Cube ----------------------------------------------------------------
	PT_CUBE_ON,
	PT_CUBE_SIZE,
	PT_SPIN_X,
	PT_SPIN_Y,
	PT_FILLED,

	// -- Bobs ----------------------------------------------------------------
	PT_BOB_COUNT,
	PT_BOB_PATH,

	// -- Machine -------------------------------------------------------------
	PT_STANDARD,
	PT_BITPLANES,
	PT_SCALING,
	PT_PIXEL_ASPECT,
	PT_BACKGROUND,

	// -- About ---------------------------------------------------------------
	PT_ABOUT_TEXT,
	PT_ABOUT_BUTTON_1,
	PT_ABOUT_BUTTON_2,
	PT_ABOUT_BUTTON_3,
	PT_ABOUT_BUTTON_4,

	PT_COUNT_
};

enum class Scaling : int
{
	Integer = 0,///< nearest, whole multiples, letterboxed (cropped at x1 if it cannot fit)
	Fit,        ///< nearest, the largest scale that fits
	FitSmooth,  ///< bilinear, the largest scale that fits
	Count
};

enum class Aspect : int
{
	NonSquare = 0,///< the standard's own pixel shape
	Square,
	Count
};

enum class Background : int
{
	Border = 0,///< COLOR00 as the beam left it, like a monitor's border
	Black,
	Transparent,
	Count
};

// Integer ranges ------------------------------------------------------------
constexpr int kScrollSpeedMax = 8; ///< pixels per field
constexpr int kBarCountMax    = 8;
constexpr int kBarHeightMin   = 4; ///< lines
constexpr int kBarHeightMax   = 48;
constexpr int kStarCountMax   = 64;///< per layer
constexpr int kStarSpeedMin   = 1; ///< pixels per field, slowest layer
constexpr int kStarSpeedMax   = 4;
constexpr int kLayersMax      = 3;
constexpr int kBobCountMax    = 16;
constexpr int kBitplanesMax   = 5;

// The 0..1 controls, in the demo's units ------------------------------------

/// Peak of the scroller's wave, 0 to 48 lines.
inline int WaveHeightLines( float v )
{
	return static_cast< int >( std::lround( 48.0f * v ) );
}

/// Columns per cycle of the wave, 40 to 640, logarithmic.
inline int WaveLengthColumns( float v )
{
	return static_cast< int >( std::lround( 40.0f * std::pow( 16.0f, v ) ) );
}

/// Palette steps per field, Q8: 0 is off, 1 is a step every other field.
inline int CycleRateQ8( float v )
{
	return static_cast< int >( std::lround( 128.0f * v ) );
}

/// Bar travel in sine-table steps per field, Q8: 0 to 24 steps of 1024.
inline int BarSpeedQ8( float v )
{
	return static_cast< int >( std::lround( 24.0f * 256.0f * v ) );
}

/// The WAIT's horizontal swing in four-pixel steps, 0 to 16: 0 puts every
/// bar's colour write in horizontal blank, so bars run the full width.
inline int BarWaveSteps( float v )
{
	return static_cast< int >( std::lround( 16.0f * v ) );
}

/// Half an edge of the cube, 10 to 36 pixels.
inline int CubeHalfEdge( float v )
{
	return 10 + static_cast< int >( std::lround( 26.0f * v ) );
}

/// Spin in sine-table steps per field, Q8: -16 to +16, 0.5 is still.
inline int SpinQ8( float v )
{
	return static_cast< int >( std::lround( ( v - 0.5f ) * 2.0f * 16.0f * 256.0f ) );
}

} // namespace copperlist
