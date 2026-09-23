#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
	The scroller's font: 16 x 16 pixels a glyph, three colours, drawn for this
	repo.

	**Not Topaz, and not ripped from any demo.** Every glyph is designed here
	on a 7 x 7 grid of cells, in the chunky square-shouldered shape cracktro
	fonts tended to have, and doubled to 16 x 16 with a one-cell gutter on the
	right and below. See ATTRIBUTIONS.md.

	## Three colours from two bitplanes

	Each lit cell is two by two pixels. Its top row is colour 2 (highlight)
	where nothing lit sits above it, its bottom row colour 3 (shadow) where
	nothing lit sits below it, and everything else is colour 1 (body). So the
	font needs two bitplanes, and a palette range of three registers -- which
	is exactly what `Font Colour Cycle` rotates.
*/
namespace copperlist::demo::font
{
constexpr int kGlyphSize = 16;

/// The characters there are glyphs for, in glyph order. Lower case maps to
/// upper case; anything else becomes '?'.
const std::string& Charset();

/// Glyph index for a character.
int GlyphFor( char c );

/// Colour index (0..3) of one pixel of one glyph.
int Pixel( int glyph, int x, int y );

int GlyphCount();

} // namespace copperlist::demo::font
