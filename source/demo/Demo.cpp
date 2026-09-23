#include "Demo.h"

#include <algorithm>
#include <cstdlib>

#include "Font.h"
#include "Tables.h"

namespace copperlist::demo
{
using namespace chip;

namespace
{
constexpr uint16_t kBackground = 0x000;

// Twelve-bit bar bases, eight a palette: $RGB, four bits a gun.
constexpr uint16_t kBarBases[ static_cast< int >( BarPalette::Count ) ][ 8 ] = {
	{ 0xF00, 0xF80, 0xFF0, 0x0F0, 0x0FF, 0x08F, 0x00F, 0xF0F },// Rainbow
	{ 0xF00, 0xF40, 0xF80, 0xFC0, 0xFF0, 0xF60, 0xF20, 0xFA0 },// Fire
	{ 0x00F, 0x06F, 0x0AF, 0x0FF, 0x8FF, 0x48F, 0x26F, 0x0CF },// Ice
	{ 0xFFF, 0xCCC, 0x999, 0xEEE, 0xBBB, 0xDDD, 0xAAA, 0x888 },// Mono
};

// The font's three colours: body, highlight, shadow.
constexpr uint16_t kFont[ 4 ] = { 0x000, 0xFB2, 0xFFE, 0xA51 };
constexpr uint16_t kBob       = 0xE44;
constexpr uint16_t kFace[ 4 ] = { 0x000, 0x8E6, 0x4B4, 0x283 };// lit, mid, shade
constexpr uint16_t kStar[ 3 ] = { 0xFFF, 0xAAA, 0x666 };
constexpr uint16_t kStarData[ 3 ] = { 0xC000, 0x8000, 0x8000 };

/// The sprite each star layer uses: 0, 2 and 4, so each has its own colour
/// registers and the fastest layer, sprite 0, is in front.
int StarSprite( int layer )
{
	return layer * 2;
}

/// The copper list, as words in chip RAM. The two WAIT words and the MOVE
/// words are HRM 2's encodings exactly.
struct CopperList
{
	Chipset& chip;
	uint32_t pc;
	bool     wrapped = false;

	void Raw( uint16_t a, uint16_t b )
	{
		chip.WriteWord( pc, a );
		chip.WriteWord( pc + 2, b );
		pc += 4;
	}
	uint32_t Move( uint16_t reg, uint16_t value )
	{
		const uint32_t at = pc;
		Raw( static_cast< uint16_t >( reg & 0x01FE ), value );
		return at;
	}
	/// WAIT for a beam line and horizontal position. VP is eight bits, so a
	/// PAL window reaching past line 255 needs the manual's trick: wait for
	/// the end of line 255 first, after which the comparison sees the line
	/// counter wrapped to 0 (HRM 2, "Vertical Beam Position").
	void Wait( int line, int hp )
	{
		if( line >= 256 && !wrapped )
		{
			Raw( 0xFFDF, 0xFFFE );
			wrapped = true;
		}
		Raw( static_cast< uint16_t >( ( ( line & 0xFF ) << 8 ) | ( hp & 0xFE ) | 1 ), 0xFFFE );
	}
	/// "This event will never occur, so the Copper stops until the next
	/// vertical blanking interval begins."
	void End() { Raw( 0xFFFF, 0xFFFE ); }
};

uint16_t BarColour( int palette, int bar, int line, int height, uint32_t* wide )
{
	const uint16_t base = kBarBases[ palette ][ bar % 8 ];
	const int      dist = std::abs( 2 * line + 1 - height );
	const int      lev  = 15 - ( dist * 15 ) / height;

	uint16_t out = 0;
	uint32_t w   = 0;
	for( int gun = 0; gun < 3; ++gun )
	{
		const int shift = 8 - gun * 4;
		const int b     = ( base >> shift ) & 15;
		int       g     = ( b * lev + 7 ) / 15;
		if( lev == 15 )
			g += ( 15 - g ) / 2;
		out = static_cast< uint16_t >( out | ( g << shift ) );

		// What a machine with eight bits a gun would have drawn instead.
		int g8 = ( b * 17 * ( height - dist ) ) / height;
		if( lev == 15 )
			g8 += ( 255 - g8 ) / 2;
		w |= static_cast< uint32_t >( g8 ) << ( 16 - gun * 8 );
	}
	if( wide != nullptr )
		*wide = w;
	return out;
}

uint16_t PaletteEntry( int i, int cycleStep )
{
	if( i == 17 || i == 18 || i == 19 )
		return kStar[ 0 ];
	if( i == 21 || i == 22 || i == 23 )
		return kStar[ 1 ];
	if( i == 25 || i == 26 || i == 27 )
		return kStar[ 2 ];
	if( i == 0 )
		return kBackground;
	if( i & 3 )
		return kFont[ 1 + ( ( i & 3 ) - 1 + cycleStep ) % 3 ];
	if( i & 4 )
		return kBob;
	return kFace[ ( i >> 3 ) & 3 ];
}

uint32_t Plane( int i )
{
	return map::kPlanes + static_cast< uint32_t >( i ) * map::kPlaneSize;
}
} // namespace

void BlitLine( Chipset& chip, uint32_t plane, int x1, int y1, int x2, int y2, bool exclusive, bool singleDot )
{
	const int  dxs     = x2 - x1;
	const int  dys     = y2 - y1;
	const int  adx     = std::abs( dxs );
	const int  ady     = std::abs( dys );
	const bool shallow = adx >= ady;
	const int  dx      = std::max( adx, ady );
	const int  dy      = std::min( adx, ady );

	// Octant bits, from BLTCON1's SUD/SUL/AUL table (HRM Appendix A).
	int sud, sul, aul;
	if( shallow )
	{
		sud = 1;
		sul = dys < 0 ? 1 : 0;
		aul = dxs < 0 ? 1 : 0;
	}
	else
	{
		sud = 0;
		sul = dxs < 0 ? 1 : 0;
		aul = dys < 0 ? 1 : 0;
	}

	const int  apt  = 4 * dy - 2 * dx;
	const bool sign = apt < 0;

	// "BLTCON0 bits 15-12 = x1 modulo 15" says the register summary; the
	// field is four bits and the bit within the word is x1 modulo 16, which
	// is what every working line routine loads. See AGENTS.md.
	chip.Poke( BLTCON0, static_cast< uint16_t >( ( ( x1 & 15 ) << 12 ) | 0x0B00 | ( exclusive ? 0x4A : 0xCA ) ) );
	chip.Poke( BLTCON1, static_cast< uint16_t >( ( sign ? 0x40 : 0 ) | ( sud << 4 ) | ( sul << 3 ) | ( aul << 2 ) |
											   ( singleDot ? 2 : 0 ) | 1 ) );
	chip.Poke( BLTADAT, 0x8000 );
	chip.Poke( BLTBDAT, 0xFFFF );
	chip.Poke( BLTAFWM, 0xFFFF );
	chip.Poke( BLTALWM, 0xFFFF );
	chip.Poke( BLTAMOD, static_cast< uint16_t >( 4 * ( dy - dx ) ) );
	chip.Poke( BLTBMOD, static_cast< uint16_t >( 4 * dy ) );
	chip.Poke( BLTCMOD, kRowBytes );
	chip.Poke( BLTDMOD, kRowBytes );
	chip.Poke( BLTAPTH, 0 );
	chip.Poke( BLTAPTL, static_cast< uint16_t >( apt ) );
	const uint32_t first = plane + static_cast< uint32_t >( y1 * kRowBytes + ( x1 >> 4 ) * 2 );
	chip.Poke( BLTCPTH, static_cast< uint16_t >( first >> 16 ) );
	chip.Poke( BLTCPTL, static_cast< uint16_t >( first ) );
	chip.Poke( BLTDPTH, static_cast< uint16_t >( first >> 16 ) );
	chip.Poke( BLTDPTL, static_cast< uint16_t >( first ) );
	chip.Poke( BLTSIZE, static_cast< uint16_t >( ( ( dx + 1 ) << 6 ) | 2 ) );
}

void Demo::LoadAssets( Chipset& chip ) const
{
	// Written every field rather than once: it is 8 KB, and it means nothing
	// the display reads depends on what an earlier field left in chip RAM.
	for( int g = 0; g < font::GlyphCount(); ++g )
		for( int p = 0; p < 2; ++p )
			for( int r = 0; r < 16; ++r )
			{
				uint16_t word = 0;
				for( int x = 0; x < 16; ++x )
					if( ( font::Pixel( g, x, r ) >> p ) & 1 )
						word = static_cast< uint16_t >( word | ( 0x8000u >> x ) );
				const uint32_t at = map::kFont + static_cast< uint32_t >( ( ( g * 2 + p ) * 16 + r ) * 4 );
				chip.WriteWord( at, word );
				chip.WriteWord( at + 2, 0 );// the extra word a shifted blit spills into
			}

	// The bob: a disc of radius 7.5 with a highlight knocked out of its upper
	// left, so where two overlap the cookie-cut shows which was blitted last.
	for( int y = 0; y < 16; ++y )
	{
		uint16_t data = 0, mask = 0;
		for( int x = 0; x < 16; ++x )
		{
			const int dx = 2 * x + 1 - 16, dy = 2 * y + 1 - 16;
			if( dx * dx + dy * dy > 225 )
				continue;
			mask = static_cast< uint16_t >( mask | ( 0x8000u >> x ) );
			const int hx = 2 * x + 1 - 11, hy = 2 * y + 1 - 11;
			if( hx * hx + hy * hy > 16 )
				data = static_cast< uint16_t >( data | ( 0x8000u >> x ) );
		}
		chip.WriteWord( map::kBall + static_cast< uint32_t >( y * 4 ), data );
		chip.WriteWord( map::kBall + static_cast< uint32_t >( y * 4 + 2 ), 0 );
		chip.WriteWord( map::kBallMask + static_cast< uint32_t >( y * 4 ), mask );
		chip.WriteWord( map::kBallMask + static_cast< uint32_t >( y * 4 + 2 ), 0 );
	}
}

void Demo::Clear( Chipset& chip, int lines ) const
{
	// D only, minterm 0: the blitter's cheapest job.
	for( int p = 0; p < kMaxPlanes; ++p )
	{
		chip.Poke( BLTCON0, 0x0100 );
		chip.Poke( BLTCON1, 0x0000 );
		chip.Poke( BLTDMOD, 0 );
		chip.Poke( BLTDPTH, static_cast< uint16_t >( Plane( p ) >> 16 ) );
		chip.Poke( BLTDPTL, static_cast< uint16_t >( Plane( p ) ) );
		chip.Poke( BLTSIZE, static_cast< uint16_t >( ( lines << 6 ) | ( kRowBytes / 2 ) ) );
	}
}

void Demo::Scroller( Chipset& chip, const Scene& s, int64_t scroll, int64_t field, int lines )
{
	mScrollerTop  = lines * 5 / 8 - 8;
	mScrollOffset = 0;
	if( s.text.empty() )
		return;

	const int64_t total = 16 * static_cast< int64_t >( s.text.size() );
	const int     off   = static_cast< int >( Wrap( scroll, total ) );
	const int     k0    = off / 16;
	const int     sub   = off % 16;
	mScrollOffset       = off;

	// Clear the flat buffer, both planes at once.
	chip.Poke( BLTCON0, 0x0100 );
	chip.Poke( BLTCON1, 0x0000 );
	chip.Poke( BLTDMOD, 0 );
	chip.Poke( BLTDPTH, static_cast< uint16_t >( map::kScroll >> 16 ) );
	chip.Poke( BLTDPTL, static_cast< uint16_t >( map::kScroll ) );
	chip.Poke( BLTSIZE, static_cast< uint16_t >( ( 32 << 6 ) | ( map::kScrollRow / 2 ) ) );

	// The glyphs, at a whole-pixel x: a shifted B, ORed into C.
	for( int k = 0; k < 22; ++k )
	{
		const int g  = font::GlyphFor( s.text[ static_cast< size_t >( ( k0 + k ) % static_cast< int >( s.text.size() ) ) ] );
		const int xb = 16 + 16 * k - sub;
		if( xb + 32 > static_cast< int >( map::kScrollRow ) * 8 )
			break;
		for( int p = 0; p < 2; ++p )
		{
			const uint32_t src = map::kFont + static_cast< uint32_t >( ( g * 2 + p ) * 16 * 4 );
			const uint32_t dst = map::kScroll + static_cast< uint32_t >( p ) * map::kScrollPlane +
								 static_cast< uint32_t >( ( xb >> 4 ) * 2 );
			chip.Poke( BLTCON0, static_cast< uint16_t >( ( ( xb & 15 ) << 12 ) | 0x0700 | 0xEE ) );
			chip.Poke( BLTCON1, static_cast< uint16_t >( ( xb & 15 ) << 12 ) );
			chip.Poke( BLTBMOD, 0 );
			chip.Poke( BLTCMOD, static_cast< uint16_t >( map::kScrollRow - 4 ) );
			chip.Poke( BLTDMOD, static_cast< uint16_t >( map::kScrollRow - 4 ) );
			chip.Poke( BLTBPTH, static_cast< uint16_t >( src >> 16 ) );
			chip.Poke( BLTBPTL, static_cast< uint16_t >( src ) );
			chip.Poke( BLTCPTH, static_cast< uint16_t >( dst >> 16 ) );
			chip.Poke( BLTCPTL, static_cast< uint16_t >( dst ) );
			chip.Poke( BLTDPTH, static_cast< uint16_t >( dst >> 16 ) );
			chip.Poke( BLTDPTL, static_cast< uint16_t >( dst ) );
			chip.Poke( BLTSIZE, static_cast< uint16_t >( ( 16 << 6 ) | 2 ) );
		}
	}

	// The wave: every column copied to the screen at its own whole-line
	// offset, one blit per column per plane. A is a constant $FFFF whose
	// first/last-word masks select the column; minterm $CA is the manual's
	// cookie-cut, B where A, C elsewhere.
	const int amp = std::max( 0, std::min( { s.waveHeight, mScrollerTop, lines - 16 - mScrollerTop } ) );
	const int len = std::max( 1, s.waveLength );
	for( int c = 0; c < kWidth; ++c )
	{
		const int      y    = mScrollerTop + MulQ14( amp, Sin( static_cast< int64_t >( c ) * kSineSteps / len + field * 6 ) );
		const uint16_t mask = static_cast< uint16_t >( 0x8000u >> ( c & 15 ) );
		for( int p = 0; p < 2; ++p )
		{
			const uint32_t src = map::kScroll + static_cast< uint32_t >( p ) * map::kScrollPlane +
								 static_cast< uint32_t >( ( ( c + 16 ) >> 4 ) * 2 );
			const uint32_t dst = Plane( p ) + static_cast< uint32_t >( y * kRowBytes + ( c >> 4 ) * 2 );
			chip.Poke( BLTCON0, 0x07CA );
			chip.Poke( BLTCON1, 0x0000 );
			chip.Poke( BLTADAT, 0xFFFF );
			chip.Poke( BLTAFWM, mask );
			chip.Poke( BLTALWM, mask );
			chip.Poke( BLTBMOD, static_cast< uint16_t >( map::kScrollRow - 2 ) );
			chip.Poke( BLTCMOD, kRowBytes - 2 );
			chip.Poke( BLTDMOD, kRowBytes - 2 );
			chip.Poke( BLTBPTH, static_cast< uint16_t >( src >> 16 ) );
			chip.Poke( BLTBPTL, static_cast< uint16_t >( src ) );
			chip.Poke( BLTCPTH, static_cast< uint16_t >( dst >> 16 ) );
			chip.Poke( BLTCPTL, static_cast< uint16_t >( dst ) );
			chip.Poke( BLTDPTH, static_cast< uint16_t >( dst >> 16 ) );
			chip.Poke( BLTDPTL, static_cast< uint16_t >( dst ) );
			chip.Poke( BLTSIZE, static_cast< uint16_t >( ( 16 << 6 ) | 1 ) );
		}
	}
}

void Demo::Bobs( Chipset& chip, const Scene& s, int64_t field, int lines ) const
{
	const int mid = lines / 2 - 8;
	const int ay  = lines * 3 / 8;
	for( int i = s.bobCount - 1; i >= 0; --i )
	{
		const int64_t t = field * 3 - i * 40;
		int           x = 0, y = 0;
		switch( static_cast< BobPath >( s.bobPath ) )
		{
		case BobPath::Lissajous:
			x = 152 + MulQ14( 140, Sin( t ) );
			y = mid + MulQ14( ay, Sin( 2 * t + 256 ) );
			break;
		case BobPath::Circle:
			x = 152 + MulQ14( 110, Cos( t ) );
			y = mid + MulQ14( ay * 3 / 4, Sin( t ) );
			break;
		case BobPath::Wave:
			x = static_cast< int >( Wrap( i * 24 + field * 2, 304 ) );
			y = mid + MulQ14( ay / 2, Sin( x * 6 + field * 5 ) );
			break;
		default:
			x = 152 + MulQ14( 130, Sin( t ) );
			y = mid + MulQ14( ay * 2 / 3, Sin( 2 * t ) );
			break;
		}
		x = std::min( std::max( x, 0 ), kWidth - 16 );
		y = std::min( std::max( y, 0 ), lines - 16 );

		const uint32_t dst = Plane( 2 ) + static_cast< uint32_t >( y * kRowBytes + ( x >> 4 ) * 2 );
		chip.Poke( BLTCON0, static_cast< uint16_t >( ( ( x & 15 ) << 12 ) | 0x0FCA ) );
		chip.Poke( BLTCON1, static_cast< uint16_t >( ( x & 15 ) << 12 ) );
		chip.Poke( BLTAFWM, 0xFFFF );
		chip.Poke( BLTALWM, 0xFFFF );
		chip.Poke( BLTAMOD, 0 );
		chip.Poke( BLTBMOD, 0 );
		chip.Poke( BLTCMOD, kRowBytes - 4 );
		chip.Poke( BLTDMOD, kRowBytes - 4 );
		chip.Poke( BLTAPTH, static_cast< uint16_t >( map::kBallMask >> 16 ) );
		chip.Poke( BLTAPTL, static_cast< uint16_t >( map::kBallMask ) );
		chip.Poke( BLTBPTH, static_cast< uint16_t >( map::kBall >> 16 ) );
		chip.Poke( BLTBPTL, static_cast< uint16_t >( map::kBall ) );
		chip.Poke( BLTCPTH, static_cast< uint16_t >( dst >> 16 ) );
		chip.Poke( BLTCPTL, static_cast< uint16_t >( dst ) );
		chip.Poke( BLTDPTH, static_cast< uint16_t >( dst >> 16 ) );
		chip.Poke( BLTDPTL, static_cast< uint16_t >( dst ) );
		chip.Poke( BLTSIZE, static_cast< uint16_t >( ( 16 << 6 ) | 2 ) );
	}
}

void Demo::Cube( Chipset& chip, const Scene& s, const Clocks& c, int lines )
{
	mCubeLines.clear();
	if( !s.cubeOn )
		return;
	(void)lines;

	const int S    = s.cubeSize;
	const int zoff = 6 * S;
	const int cx   = kWidth / 2;
	const int cy   = 96;
	const int64_t ax = c.spinX >> 8;
	const int64_t ay = c.spinY >> 8;

	int rx[ 8 ], ry[ 8 ], rz[ 8 ], px[ 8 ], py[ 8 ];
	for( int i = 0; i < 8; ++i )
	{
		const int x = ( i & 1 ) ? S : -S;
		const int y = ( i & 2 ) ? S : -S;
		const int z = ( i & 4 ) ? S : -S;
		const int x1 = MulQ14( x, Cos( ay ) ) + MulQ14( z, Sin( ay ) );
		const int z1 = -MulQ14( x, Sin( ay ) ) + MulQ14( z, Cos( ay ) );
		const int y1 = MulQ14( y, Cos( ax ) ) - MulQ14( z1, Sin( ax ) );
		const int z2 = MulQ14( y, Sin( ax ) ) + MulQ14( z1, Cos( ax ) );
		rx[ i ] = x1;
		ry[ i ] = y1;
		rz[ i ] = z2;
		px[ i ] = cx + ( x1 * zoff ) / ( z2 + zoff );
		py[ i ] = cy + ( y1 * zoff ) / ( z2 + zoff );
	}

	if( !s.filled )
	{
		static const int kEdges[ 12 ][ 2 ] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 },
											   { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
		for( const auto& e : kEdges )
		{
			const Line l{ px[ e[ 0 ] ], py[ e[ 0 ] ], px[ e[ 1 ] ], py[ e[ 1 ] ] };
			BlitLine( chip, Plane( 3 ), l.x1, l.y1, l.x2, l.y2, false, false );
			mCubeLines.push_back( l );
		}
		return;
	}

	// Filled: each visible face outlined with one dot a row, exclusive-or, in
	// the planes its colour has bits in, then area-filled by the blitter.
	struct Face
	{
		int v[ 4 ];
		int colour;
	};
	static const Face kFaces[ 6 ] = {
		{ { 0, 2, 6, 4 }, 1 }, { { 1, 3, 7, 5 }, 1 }, { { 0, 1, 5, 4 }, 2 },
		{ { 2, 3, 7, 6 }, 2 }, { { 0, 1, 3, 2 }, 3 }, { { 4, 5, 7, 6 }, 3 },
	};

	int ymin = lines, ymax = -1;
	for( int i = 0; i < 8; ++i )
	{
		ymin = std::min( ymin, py[ i ] );
		ymax = std::max( ymax, py[ i ] );
	}

	for( int bit = 0; bit < 2; ++bit )
	{
		const uint32_t plane = Plane( 3 + bit );
		for( const Face& f : kFaces )
		{
			if( ( ( f.colour >> bit ) & 1 ) == 0 )
				continue;
			// Facing the viewer, who sits at z = -zoff: the outward normal of
			// a face on a cube centred at the origin is its own centre.
			int64_t sx = 0, sy = 0, sz = 0;
			for( int k = 0; k < 4; ++k )
			{
				sx += rx[ f.v[ k ] ];
				sy += ry[ f.v[ k ] ];
				sz += rz[ f.v[ k ] ];
			}
			const int64_t dot = -( sx * sx + sy * sy + sz * sz ) - sz * 4 * zoff;
			if( dot <= 0 )
				continue;

			for( int k = 0; k < 4; ++k )
			{
				int x1 = px[ f.v[ k ] ], y1 = py[ f.v[ k ] ];
				int x2 = px[ f.v[ ( k + 1 ) & 3 ] ], y2 = py[ f.v[ ( k + 1 ) & 3 ] ];
				if( y1 == y2 )
					continue;// a horizontal edge crosses no row
				if( y1 > y2 )
				{
					std::swap( x1, x2 );
					std::swap( y1, y2 );
				}
				// The demo coder's trick: pre-toggle the first dot so the line
				// cancels it, and each edge owns the rows (y1, y2]. Two edges
				// meeting at a vertex then put exactly one dot on its row.
				const uint32_t at = plane + static_cast< uint32_t >( y1 * kRowBytes + ( x1 >> 4 ) * 2 );
				chip.WriteWord( at, static_cast< uint16_t >( chip.ReadWord( at ) ^ ( 0x8000u >> ( x1 & 15 ) ) ) );
				BlitLine( chip, plane, x1, y1, x2, y2, true, true );
			}
		}

		if( ymax < ymin )
			continue;
		// Fill: descending, so the pointers start at the last word.
		const uint32_t last = plane + static_cast< uint32_t >( ymax * kRowBytes + kRowBytes - 2 );
		chip.Poke( BLTCON0, 0x03AA );
		chip.Poke( BLTCON1, 0x0012 );
		chip.Poke( BLTCMOD, 0 );
		chip.Poke( BLTDMOD, 0 );
		chip.Poke( BLTCPTH, static_cast< uint16_t >( last >> 16 ) );
		chip.Poke( BLTCPTL, static_cast< uint16_t >( last ) );
		chip.Poke( BLTDPTH, static_cast< uint16_t >( last >> 16 ) );
		chip.Poke( BLTDPTL, static_cast< uint16_t >( last ) );
		chip.Poke( BLTSIZE, static_cast< uint16_t >( ( ( ymax - ymin + 1 ) << 6 ) | ( kRowBytes / 2 ) ) );
	}
}

void Demo::Copper( Chipset& chip, const Scene& s, const Clocks& c, int64_t field, int lines ) const
{
	chip.debug.wideColour.clear();
	CopperList cl{ chip, map::kCopper };

	// -- the top of the field: every register the display uses ---------------
	cl.Move( BPLCON0, static_cast< uint16_t >( ( s.bitplanes << 12 ) | 0x0200 ) );
	cl.Move( BPLCON1, 0 );
	cl.Move( BPLCON2, 0 );// playfield in front of every sprite pair
	for( int i = 0; i < kMaxPlanes; ++i )
	{
		cl.Move( static_cast< uint16_t >( BPL1PTH + 4 * i ), static_cast< uint16_t >( Plane( i ) >> 16 ) );
		cl.Move( static_cast< uint16_t >( BPL1PTH + 4 * i + 2 ), static_cast< uint16_t >( Plane( i ) ) );
	}
	const int cycleStep = static_cast< int >( Wrap( c.cycle >> 8, 3 ) );
	for( int i = 0; i < 32; ++i )
		cl.Move( static_cast< uint16_t >( COLOR00 + 2 * i ), PaletteEntry( i, cycleStep ) );
	for( int i = 0; i < kSprites; ++i )
	{
		cl.Move( static_cast< uint16_t >( SPR0CTL + 8 * i ), 0 );
		cl.Move( static_cast< uint16_t >( SPR0DATB + 8 * i ), 0 );
	}

	// -- bars ----------------------------------------------------------------
	const int count  = s.barCount;
	const int height = std::max( 2, s.barHeight );
	std::vector< int > top( static_cast< size_t >( count ) );
	const int amp = std::max( 0, lines / 2 - height / 2 - 2 );
	for( int b = 0; b < count; ++b )
		top[ static_cast< size_t >( b ) ] =
			lines / 2 + MulQ14( amp, Sin( ( c.barPhase >> 8 ) + b * kSineSteps / count ) ) - height / 2;

	// -- stars: which star, if any, each layer shows on each line -----------
	const int layers = std::min( std::max( s.layers, 0 ), 3 );
	const int stars  = std::min( std::max( s.starCount, 0 ), lines );
	std::vector< int > starAt( static_cast< size_t >( 3 * lines ), -1 );
	for( int L = 0; L < layers; ++L )
		for( int k = 0; k < stars; ++k )
			starAt[ static_cast< size_t >( L * lines + ( k * lines / stars + L * 5 ) % lines ) ] = k;

	for( int y = 0; y < lines; ++y )
	{
		const int line = kDiwVStart + y;
		cl.Wait( line, 0x00 );

		// Bars in list order: where two cover one line, the later MOVE is the
		// one on screen, because a colour register holds one value.
		int covering[ 16 ];
		int n = 0;
		for( int b = 0; b < count && n < 16; ++b )
			if( y >= top[ static_cast< size_t >( b ) ] && y < top[ static_cast< size_t >( b ) ] + height )
				covering[ n++ ] = b;

		auto moveBar = [ & ]( int b ) {
			uint32_t       wide = 0;
			const uint16_t col  = BarColour( s.barPalette, b, y - top[ static_cast< size_t >( b ) ], height, &wide );
			const uint32_t at   = cl.Move( COLOR00, col );
			if( debug.wideColour )
				chip.debug.wideColour[ at ] = wide;
		};

		if( s.barWave == 0 && n > 0 )
			for( int i = 0; i < n; ++i )
				moveBar( covering[ i ] );
		else
			cl.Move( COLOR00, kBackground );

		// Stars: one sprite per layer, re-armed at a new x on every line it
		// has a star, disarmed on the line after -- sprite multiplexing by
		// the copper (HRM 4, "Manual Mode").
		for( int L = 0; L < layers; ++L )
		{
			const int spr = StarSprite( L );
			const int k   = starAt[ static_cast< size_t >( L * lines + y ) ];
			if( k >= 0 )
			{
				const int x = static_cast< int >(
					Wrap( static_cast< int64_t >( Hash( static_cast< uint32_t >( k * 3 + L + 1 ) ) % kWidth ) -
							  c.stars * ( 3 - L ),
						  kWidth ) );
				const int h = x + kDiwHStart;
				cl.Move( static_cast< uint16_t >( SPR0CTL + 8 * spr ), static_cast< uint16_t >( h & 1 ) );
				cl.Move( static_cast< uint16_t >( SPR0POS + 8 * spr ), static_cast< uint16_t >( ( h >> 1 ) & 0xFF ) );
				cl.Move( static_cast< uint16_t >( SPR0DATA + 8 * spr ), kStarData[ L ] );
			}
			else if( y > 0 && starAt[ static_cast< size_t >( L * lines + y - 1 ) ] >= 0 )
				cl.Move( static_cast< uint16_t >( SPR0CTL + 8 * spr ), 0 );
		}

		// With Bar Wave on, each bar starts where a WAIT says, on the
		// copper's four-pixel grid.
		// A WAIT for a position the beam has already passed costs six clocks
		// and does nothing, so a bar that starts left of the one before it
		// gets no WAIT at all: its MOVE follows the previous one directly,
		// one MOVE -- eight pixels -- later. That is where two colour changes
		// sit as close as the copper allows.
		if( s.barWave > 0 )
		{
			int lastHp = -1;
			for( int i = 0; i < n; ++i )
			{
				const int b  = covering[ i ];
				const int hp = 58 + 2 * ( ( s.barWave * ( Sin( y * 12 + field * 4 + b * 97 ) + kSineOne ) ) >> 15 );
				if( hp > lastHp )
				{
					cl.Wait( line, hp );
					lastHp = hp;
				}
				moveBar( b );
			}
		}
	}

	cl.Wait( kDiwVStart + lines, 0x00 );
	cl.Move( COLOR00, kBackground );
	for( int L = 0; L < 3; ++L )
		cl.Move( static_cast< uint16_t >( SPR0CTL + 8 * StarSprite( L ) ), 0 );
	cl.End();
}

void Demo::Field( Chipset& chip, const Scene& scene, const Clocks& clocks, int64_t field )
{
	const Standard& standard = scene.ntsc ? kNtsc : kPal;
	if( chip.GetStandard().visible != standard.visible )
		chip.SetStandard( standard );
	const int lines = standard.visible;

	if( debug.accumulateScroll )
		mAccumulated += scene.scrollSpeed;

	LoadAssets( chip );
	Clear( chip, lines );
	Scroller( chip, scene, debug.accumulateScroll ? mAccumulated : clocks.scroll, field, lines );
	Bobs( chip, scene, field, lines );
	Cube( chip, scene, clocks, lines );
	Copper( chip, scene, clocks, field, lines );
	chip.RunField( map::kCopper );
}

} // namespace copperlist::demo
