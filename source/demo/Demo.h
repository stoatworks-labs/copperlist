#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "chip/Chipset.h"

/**
	The cracktro: C++ that does, once a field, what the 68000 did in a
	vertical-blank interrupt -- clear the screen with the blitter, blit the
	scroller, the bobs and the cube, write a copper list -- and then lets the
	chipset display it.

	Nothing here draws a pixel. Every pixel on screen is either a bit the
	blitter set in a bitplane or a colour the copper wrote into a register at a
	beam position, and `chip::Chipset` decides what those look like. That is
	the whole idea of the plugin: the constraints live in the machine, so the
	look cannot cheat.

	## The bitplanes, and why the palette looks like that

	    plane 0, 1   the scroller -- three colours, the font's body, highlight
	                 and shadow (indices 1..3)
	    plane 2      the bobs (index 4)
	    plane 3      the cube; with Filled on, faces with colour bit 0
	    plane 4      Filled only: faces with colour bit 1

	A pixel's index is the OR of whatever is in all five, so where the
	scroller crosses a bob the index is 5..7. The demo makes the scroller win
	there the way demo coders did, **by palette**: every index whose low two
	bits are set is given a font colour. The bob wins over the cube the same
	way. Indices 17..19, 21..23, 25..27 and 29..31 are also the sprites'
	colour registers (HRM 4, Figure 4-6) -- the machine shares them -- so the
	stars' colours win there, and text crossing the second and third faces of
	a filled cube shows star colours. That is the hardware, not a bug.

	## Pure functions of the field

	Every position is computed from the field number and the `Clocks` the
	plugin hands in, and every register the display uses is rewritten at the
	top of every field's copper list. So a field reached by running from zero
	and the same field reached by a jump are the same field, byte for byte.
	`Debug::accumulateScroll` is the classic way to break that, kept as the
	negative control for `cptest --replay`.
*/
namespace copperlist::demo
{
/// Chip RAM layout, in bytes. Word aligned, fixed, nothing allocated.
namespace map
{
constexpr uint32_t kCopper    = 0x00000;///< the copper list, up to 64 KB
constexpr uint32_t kPlanes    = 0x10000;///< five planes of 320 x 256
constexpr uint32_t kPlaneSize = 40u * 256u;
constexpr uint32_t kScroll    = 0x20000;///< the flat scroll buffer, two planes
constexpr uint32_t kScrollRow = 48;     ///< 384 pixels: 16 margin + 320 + 48
constexpr uint32_t kScrollPlane = kScrollRow * 16u;
constexpr uint32_t kFont      = 0x21000;///< glyphs, 2 planes, 2 words a row
constexpr uint32_t kBall      = 0x24000;///< the bob, 2 words a row
constexpr uint32_t kBallMask  = 0x24080;///< and its mask
} // namespace map

/// What the operator asked for, in the demo's own units.
struct Scene
{
	std::string text        = "";
	int         scrollSpeed = 2; ///< pixels per field
	int         waveHeight  = 0; ///< lines, peak
	int         waveLength  = 160;///< columns per cycle
	int         barCount    = 0;
	int         barHeight   = 16;///< lines
	int         barWave     = 0; ///< 0 = full width; else the WAIT's swing, in 4-pixel steps
	int         barPalette  = 0;
	int         starCount   = 0; ///< per layer
	int         layers      = 3;
	bool        cubeOn      = false;
	int         cubeSize    = 40;///< half an edge, pixels
	bool        filled      = false;
	int         bobCount    = 0;
	int         bobPath     = 0;
	bool        ntsc        = false;
	int         bitplanes   = 5;
};

/// Positions that the plugin accumulates, so a speed can change without the
/// thing it moves jumping. At constant settings each is `rate * field`.
struct Clocks
{
	int64_t scroll   = 0;///< pixels
	int64_t barPhase = 0;///< Q8 sine steps
	int64_t spinX    = 0;///< Q8 sine steps
	int64_t spinY    = 0;
	int64_t stars    = 0;///< pixels of the slowest layer
	int64_t cycle    = 0;///< Q8 palette steps
};

enum class BobPath : int
{
	Lissajous = 0,
	Circle,
	Wave,
	FigureEight,
	Count
};

enum class BarPalette : int
{
	Rainbow = 0,
	Fire,
	Ice,
	Mono,
	Count
};

/// A line the blitter drew, as the demo asked for it: endpoints in playfield
/// pixels, in the order it drew them.
struct Line
{
	int x1, y1, x2, y2;
};

struct Debug
{
	/// Keep the scroller's position in the demo and add the speed to it once
	/// per field, the way most real scrollers were written. A jump then lands
	/// the text somewhere else. The negative control for `--replay`.
	bool accumulateScroll = false;
	/// Compute the bar gradients in 8-bit and carry them past the 12-bit
	/// registers (see `chip::Debug::wideColour`). The negative control for
	/// `--palette`.
	bool wideColour = false;
};

class Demo
{
public:
	/// One field of the demo, then the display of it.
	void Field( chip::Chipset& chip, const Scene& scene, const Clocks& clocks, int64_t field );

	/// The cube's edges as drawn in the last field (wireframe only; empty
	/// when the cube is off or filled).
	const std::vector< Line >& CubeLines() const { return mCubeLines; }

	/// Where the scroller's baseline sat, and its wave, in the last field.
	int ScrollerTop() const { return mScrollerTop; }
	int ScrollOffset() const { return mScrollOffset; }

	Debug debug;

private:
	void LoadAssets( chip::Chipset& chip ) const;
	void Clear( chip::Chipset& chip, int lines ) const;
	void Scroller( chip::Chipset& chip, const Scene& s, int64_t scroll, int64_t field, int lines );
	void Bobs( chip::Chipset& chip, const Scene& s, int64_t field, int lines ) const;
	void Cube( chip::Chipset& chip, const Scene& s, const Clocks& c, int lines );
	void Copper( chip::Chipset& chip, const Scene& s, const Clocks& c, int64_t field, int lines ) const;

	std::vector< Line > mCubeLines;
	int64_t             mAccumulated  = 0;
	int                 mScrollerTop  = 0;
	int                 mScrollOffset = 0;
};

/// The blitter's line mode, set up as HRM 6's register summary says. Public
/// because the harness draws with it too.
void BlitLine( chip::Chipset& chip, uint32_t plane, int x1, int y1, int x2, int y2, bool exclusive,
			   bool singleDot );

} // namespace copperlist::demo
