#include "Chipset.h"

#include <algorithm>

namespace copperlist::chip
{
namespace
{
/// The blitter's function generator (HRM 6, "Designing the LF Control Byte
/// with Minterms"): bit n of LF is the output for the source combination
/// n = A*4 + B*2 + C, applied to each of the sixteen bit positions at once.
uint16_t Minterm( uint8_t lf, uint16_t a, uint16_t b, uint16_t c )
{
	uint16_t d = 0;
	for( int n = 0; n < 8; ++n )
	{
		if( ( ( lf >> n ) & 1 ) == 0 )
			continue;
		const uint16_t ta = ( n & 4 ) ? a : static_cast< uint16_t >( ~a );
		const uint16_t tb = ( n & 2 ) ? b : static_cast< uint16_t >( ~b );
		const uint16_t tc = ( n & 1 ) ? c : static_cast< uint16_t >( ~c );
		d = static_cast< uint16_t >( d | ( ta & tb & tc ) );
	}
	return d;
}

int16_t Signed( uint16_t v )
{
	return static_cast< int16_t >( v );
}
} // namespace

Chipset::Chipset()
{
	Reset();
}

void Chipset::Reset()
{
	mRam.assign( kChipBytes, 0 );
	mRegs.fill( 0 );
	mColour.fill( 0 );
	mSprite.fill( Sprite{} );
	mWrites.clear();
	SetStandard( mStandard );
}

void Chipset::SetStandard( const Standard& s )
{
	mStandard = s;
	mPicture.assign( static_cast< size_t >( PictureWidth() ) * static_cast< size_t >( PictureHeight() ) * 4u, 0 );
	mIndices.assign( static_cast< size_t >( kWidth ) * static_cast< size_t >( s.visible ), 0 );
}

uint16_t Chipset::ReadWord( uint32_t address ) const
{
	const uint32_t a = address & ( kChipBytes - 1u ) & ~1u;
	return static_cast< uint16_t >( mRam[ a ] << 8 | mRam[ a + 1 ] );
}

void Chipset::WriteWord( uint32_t address, uint16_t value )
{
	const uint32_t a = address & ( kChipBytes - 1u ) & ~1u;
	mRam[ a ]     = static_cast< uint8_t >( value >> 8 );
	mRam[ a + 1 ] = static_cast< uint8_t >( value );
}

uint32_t Chipset::Pointer( uint16_t hi ) const
{
	return ( ( static_cast< uint32_t >( mRegs[ hi >> 1 ] ) & 0x7u ) << 16 |
			 mRegs[ ( hi + 2 ) >> 1 ] ) &
		   ~1u;
}

void Chipset::SetPointer( uint16_t hi, uint32_t value )
{
	mRegs[ hi >> 1 ]         = static_cast< uint16_t >( ( value >> 16 ) & 0x7u );
	mRegs[ ( hi + 2 ) >> 1 ] = static_cast< uint16_t >( value & 0xFFFEu );
}

void Chipset::Poke( uint16_t reg, uint16_t value )
{
	reg &= 0x1FE;
	mRegs[ reg >> 1 ] = value;
	if( reg == BLTSIZE )
	{
		if( mRegs[ BLTCON1 >> 1 ] & 1 )
			BlitLine();
		else
			Blit();
	}
}

//---------------------------------------------------------------------------
// The blitter, area mode (HRM 6: "DMA Channels", "Shifts and Masks",
// "Descending Mode", "Area Fill Mode").
//---------------------------------------------------------------------------
void Chipset::Blit()
{
	const uint16_t con0 = mRegs[ BLTCON0 >> 1 ];
	const uint16_t con1 = mRegs[ BLTCON1 >> 1 ];
	const uint16_t size = mRegs[ BLTSIZE >> 1 ];

	int h = size >> 6;
	int w = size & 63;
	if( h == 0 )
		h = 1024;
	if( w == 0 )
		w = 64;

	const bool desc = ( con1 & 0x0002 ) != 0;
	const int  step = desc ? -2 : 2;
	const int  ash  = con0 >> 12;
	const int  bsh  = con1 >> 12;
	const bool useA = ( con0 & 0x0800 ) != 0;
	const bool useB = ( con0 & 0x0400 ) != 0;
	const bool useC = ( con0 & 0x0200 ) != 0;
	const bool useD = ( con0 & 0x0100 ) != 0;
	const uint8_t lf = static_cast< uint8_t >( con0 & 0xFF );
	const bool efe  = ( con1 & 0x0010 ) != 0;
	const bool ife  = ( con1 & 0x0008 ) != 0;
	const bool fci  = ( con1 & 0x0004 ) != 0;

	// Modulos are signed and, in descending mode, subtracted.
	const int sign = desc ? -1 : 1;
	const int amod = sign * ( Signed( mRegs[ BLTAMOD >> 1 ] ) & ~1 );
	const int bmod = sign * ( Signed( mRegs[ BLTBMOD >> 1 ] ) & ~1 );
	const int cmod = sign * ( Signed( mRegs[ BLTCMOD >> 1 ] ) & ~1 );
	const int dmod = sign * ( Signed( mRegs[ BLTDMOD >> 1 ] ) & ~1 );

	uint32_t apt = Pointer( BLTAPTH ), bpt = Pointer( BLTBPTH );
	uint32_t cpt = Pointer( BLTCPTH ), dpt = Pointer( BLTDPTH );
	const uint16_t afwm = mRegs[ BLTAFWM >> 1 ], alwm = mRegs[ BLTALWM >> 1 ];
	const uint16_t adat = mRegs[ BLTADAT >> 1 ], bdat = mRegs[ BLTBDAT >> 1 ];
	const uint16_t cdat = mRegs[ BLTCDAT >> 1 ];

	// "For the first word of the blit, zeros are shifted in; for each
	// subsequent word of the same blit, the data shifted out from the
	// previous word is shifted in" -- across rows too, which is why a shifted
	// blit wants a masked (or zero) last word.
	uint16_t aPrev = 0, bPrev = 0;

	for( int r = 0; r < h; ++r )
	{
		bool state = fci;
		for( int c = 0; c < w; ++c )
		{
			uint16_t a = useA ? ReadWord( apt ) : adat;
			if( useA )
				apt += static_cast< uint32_t >( step );
			// The masks are ANDed before the shift. In descending mode the
			// first-word mask still masks the first word FETCHED.
			if( c == 0 )
				a &= afwm;
			if( c == w - 1 )
				a &= alwm;

			const uint16_t b = useB ? ReadWord( bpt ) : bdat;
			if( useB )
				bpt += static_cast< uint32_t >( step );

			uint16_t as, bs;
			if( !desc )
			{
				as = static_cast< uint16_t >( ( ( static_cast< uint32_t >( aPrev ) << 16 ) | a ) >> ash );
				bs = static_cast< uint16_t >( ( ( static_cast< uint32_t >( bPrev ) << 16 ) | b ) >> bsh );
			}
			else
			{
				as = static_cast< uint16_t >( ( ( ( static_cast< uint32_t >( a ) << 16 ) | aPrev ) << ash ) >> 16 );
				bs = static_cast< uint16_t >( ( ( ( static_cast< uint32_t >( b ) << 16 ) | bPrev ) << bsh ) >> 16 );
			}
			aPrev = a;
			bPrev = b;

			const uint16_t cv = useC ? ReadWord( cpt ) : cdat;
			if( useC )
				cpt += static_cast< uint32_t >( step );

			uint16_t d = Minterm( lf, as, bs, cv );

			// Area fill: right to left along the row, which is why it needs
			// descending mode. Each 1 flips the fill state.
			if( efe || ife )
			{
				uint16_t out = 0;
				for( int bit = 0; bit < 16; ++bit )
				{
					const bool src = ( ( d >> bit ) & 1 ) != 0;
					if( src )
						state = !state;
					const bool on = efe ? state : ( state || src );
					if( on )
						out = static_cast< uint16_t >( out | ( 1u << bit ) );
				}
				d = out;
			}

			if( useD )
			{
				WriteWord( dpt, d );
				dpt += static_cast< uint32_t >( step );
			}
		}
		if( useA )
			apt += static_cast< uint32_t >( amod );
		if( useB )
			bpt += static_cast< uint32_t >( bmod );
		if( useC )
			cpt += static_cast< uint32_t >( cmod );
		if( useD )
			dpt += static_cast< uint32_t >( dmod );
	}

	SetPointer( BLTAPTH, apt );
	SetPointer( BLTBPTH, bpt );
	SetPointer( BLTCPTH, cpt );
	SetPointer( BLTDPTH, dpt );
}

//---------------------------------------------------------------------------
// The blitter, line mode (HRM 6, "Line Mode" and its register summary).
//
// The manual gives the set-up and the octant table but not the inner loop,
// so the step rule here is stated rather than quoted: BLTAPT is a running
// Bresenham decision term, set to 4dy - 2dx; on every step the MINOR axis
// moves when the sign bit is clear (the term is >= 0) and the term grows by
// BLTAMOD = 4(dy - dx), otherwise it grows by BLTBMOD = 4dy; the MAJOR axis
// moves on every step. That is Bresenham with ties stepping the minor axis,
// and it is the reading every emulator takes of the same registers.
//
// SUD/SUL/AUL decode as their names say (Appendix A, BLTCON1): SUD set means
// the Sometimes axis is Up/Down (a shallow line, x major); SUL, that the
// Sometimes step is Up or Left; AUL, that the Always step is Up or Left.
// SING suppresses every dot after the first on a horizontal row.
//---------------------------------------------------------------------------
void Chipset::BlitLine()
{
	const uint16_t con0 = mRegs[ BLTCON0 >> 1 ];
	const uint16_t con1 = mRegs[ BLTCON1 >> 1 ];
	const uint16_t size = mRegs[ BLTSIZE >> 1 ];

	const uint8_t lf   = static_cast< uint8_t >( con0 & 0xFF );
	const bool    sud  = ( con1 & 0x0010 ) != 0;
	const bool    sul  = ( con1 & 0x0008 ) != 0;
	const bool    aul  = ( con1 & 0x0004 ) != 0;
	const bool    sing = ( con1 & 0x0002 ) != 0;
	bool          sign = ( con1 & 0x0040 ) != 0;

	int       err  = Signed( mRegs[ BLTAPTL >> 1 ] ) + debug.lineErrBias;
	const int amod = Signed( mRegs[ BLTAMOD >> 1 ] );
	const int bmod = Signed( mRegs[ BLTBMOD >> 1 ] );
	const int cmod = Signed( mRegs[ BLTCMOD >> 1 ] );
	if( debug.lineErrBias != 0 )
		sign = err < 0;

	int len = size >> 6;
	if( len == 0 )
		len = 1024;

	uint32_t       cpt  = Pointer( BLTCPTH );
	int            xbit = con0 >> 12;
	const uint16_t tex  = mRegs[ BLTBDAT >> 1 ];
	int            tbit = con1 >> 12;
	bool           dotOnRow = false;

	auto moveX = [ & ]( int dir ) {
		xbit += dir;
		if( xbit == 16 )
		{
			xbit = 0;
			cpt += 2;
		}
		else if( xbit < 0 )
		{
			xbit = 15;
			cpt -= 2;
		}
	};
	auto moveY = [ & ]( int dir ) {
		cpt += static_cast< uint32_t >( dir * cmod );
		dotOnRow = false;
	};

	for( int i = 0; i < len; ++i )
	{
		if( !sing || !dotOnRow )
		{
			const uint16_t a = static_cast< uint16_t >( 0x8000u >> xbit );
			const uint16_t b = ( ( tex >> tbit ) & 1 ) ? 0xFFFF : 0x0000;
			const uint16_t c = ReadWord( cpt );
			WriteWord( cpt, Minterm( lf, a, b, c ) );
			dotOnRow = true;
		}
		tbit = ( tbit + 15 ) & 15;

		const bool minor = !sign;
		err += minor ? amod : bmod;
		if( sud )
		{
			moveX( aul ? -1 : 1 );
			if( minor )
				moveY( sul ? -1 : 1 );
		}
		else
		{
			moveY( aul ? -1 : 1 );
			if( minor )
				moveX( sul ? -1 : 1 );
		}
		sign = err < 0;
	}

	SetPointer( BLTCPTH, cpt );
	SetPointer( BLTDPTH, cpt );
}

//---------------------------------------------------------------------------
// The copper (HRM 2).
//---------------------------------------------------------------------------
int Chipset::WaitSatisfiedAt( int from, uint16_t w1, uint16_t w2 ) const
{
	// "Bits 14-8 vertical position compare enable, bits 7-1 horizontal";
	// V7 has no enable bit and is always compared. The low bit of the
	// horizontal position is never compared: that is the four-pixel grid.
	const uint16_t mask   = static_cast< uint16_t >( 0x8000u | ( w2 & 0x7F00u ) | ( w2 & 0x00FEu ) );
	const uint16_t target = static_cast< uint16_t >( w1 & 0xFFFEu & mask );
	const int      total  = mStandard.lines * kCckPerLine;

	if( mask == 0xFFFE )
	{
		const int tv = target >> 8;
		const int th = target & 0xFE;
		for( int v = from / kCckPerLine; v < mStandard.lines; ++v )
		{
			const int hs = ( v == from / kCckPerLine ) ? from % kCckPerLine : 0;
			const int vv = v & 0xFF;
			if( vv > tv )
				return v * kCckPerLine + hs;
			if( vv == tv && th <= kCckPerLine - 1 )
				return v * kCckPerLine + std::max( hs, th );
		}
		return -1;
	}

	for( int t = from; t < total; ++t )
	{
		const int      v    = t / kCckPerLine;
		const int      h    = t % kCckPerLine;
		const uint16_t beam = static_cast< uint16_t >( ( ( v & 0xFF ) << 8 | ( h & 0xFE ) ) & mask );
		if( beam >= target )
			return t;
	}
	return -1;
}

void Chipset::CopperRun( uint32_t list )
{
	mWrites.clear();
	const int total = mStandard.lines * kCckPerLine;
	uint32_t  pc    = list;
	int       t     = 0;

	// A runaway list cannot hang the host: every instruction costs at least
	// four colour clocks, so this bound is the field.
	while( t < total )
	{
		const uint16_t w1 = ReadWord( pc );
		const uint16_t w2 = ReadWord( pc + 2 );
		const uint32_t at = pc;
		pc += 4;

		if( ( w1 & 1 ) == 0 )
		{
			// MOVE. "The Copper cannot write into any register whose address
			// is lower than $10" -- and $10..$1F needs the danger bit, which
			// this machine never sets. The real copper stops; so does this.
			t += debug.moveCck;
			const uint16_t reg = static_cast< uint16_t >( w1 & 0x01FE );
			if( reg < 0x20 )
				break;
			if( t < total )
				mWrites.push_back( Write{ t, at, reg, w2 } );
		}
		else if( ( w2 & 1 ) == 0 )
		{
			// WAIT: three memory cycles, one of them the wake-up.
			t += kWaitFetchCck;
			const int at2 = WaitSatisfiedAt( t, w1, w2 );
			if( at2 < 0 )
				break;// $FFFF,$FFFE: the end of the list
			t = std::max( t, at2 ) + debug.waitWakeCck;
		}
		else
		{
			// SKIP the next instruction if the beam has reached the position.
			t += kMoveCck;
			const int at2 = WaitSatisfiedAt( t, w1, static_cast< uint16_t >( w2 & 0xFFFE ) );
			if( at2 >= 0 && at2 <= t )
				pc += 4;
		}
	}
}

void Chipset::Apply( const Write& w )
{
	mRegs[ w.reg >> 1 ] = w.value;

	if( w.reg >= COLOR00 && w.reg < COLOR00 + 64 )
	{
		const int  i    = ( w.reg - COLOR00 ) >> 1;
		const auto wide = debug.wideColour.find( w.source );
		mColour[ static_cast< size_t >( i ) ] =
			wide != debug.wideColour.end() ? wide->second : Expand12( w.value & 0x0FFF );
		return;
	}
	if( w.reg >= SPR0POS && w.reg < SPR0POS + 8 * kSprites )
	{
		Sprite& s = mSprite[ static_cast< size_t >( ( w.reg - SPR0POS ) >> 3 ) ];
		switch( ( w.reg - SPR0POS ) & 7 )
		{
		case 0: s.pos = w.value; break;
		// "Writing to the SPRxCTL register disables the sprite."
		case 2:
			s.ctl   = w.value;
			s.armed = false;
			break;
		// "Writing to the A buffer enables (arms) the sprite."
		case 4:
			s.data  = w.value;
			s.armed = true;
			break;
		case 6: s.datb = w.value; break;
		default: break;
		}
	}
}

void Chipset::RunField( uint32_t copperList )
{
	CopperRun( copperList );
	Compose();
}

void Chipset::Compose()
{
	const int visible = mStandard.visible;
	const int pw      = PictureWidth();
	size_t    wi      = 0;

	uint32_t planeBase[ kMaxPlanes ] = {};
	uint32_t topBorder = 0, bottomBorder = 0;

	auto put = [ & ]( int px, int py, uint32_t rgb ) {
		uint8_t* d = mPicture.data() + ( static_cast< size_t >( py ) * static_cast< size_t >( pw ) + static_cast< size_t >( px ) ) * 4u;
		d[ 0 ] = static_cast< uint8_t >( rgb >> 16 );
		d[ 1 ] = static_cast< uint8_t >( rgb >> 8 );
		d[ 2 ] = static_cast< uint8_t >( rgb );
		d[ 3 ] = 255;
	};

	for( int v = 0; v < mStandard.lines; ++v )
	{
		const int lineStart = v * kCckPerLine;
		const int lineEnd   = lineStart + kCckPerLine;
		const int y         = v - kDiwVStart;

		for( Sprite& s : mSprite )
			s.count = 0;

		if( y < 0 || y >= visible )
		{
			while( wi < mWrites.size() && mWrites[ wi ].cck < lineEnd )
				Apply( mWrites[ wi++ ] );
			continue;
		}

		for( int p = 0; p < kLoresPerLine; ++p )
		{
			while( wi < mWrites.size() && mWrites[ wi ].cck < lineEnd &&
				   2 * ( mWrites[ wi ].cck - lineStart ) <= p )
				Apply( mWrites[ wi++ ] );

			if( y == 0 && p == 0 )
			{
				// Bitplane pointers are latched at the top of the window;
				// the copper reloads them every field, as HRM 3 requires.
				for( int i = 0; i < kMaxPlanes; ++i )
					planeBase[ i ] = Pointer( static_cast< uint16_t >( BPL1PTH + 4 * i ) );
				topBorder = mColour[ 0 ];
			}

			// Sprites: the comparator fires when the beam equals HSTART, and
			// the shift registers then output sixteen pixels, MSB first.
			int spriteColour = 0, spriteNumber = -1;
			for( int s = 0; s < kSprites; ++s )
			{
				Sprite& sp = mSprite[ static_cast< size_t >( s ) ];
				const int hstart = ( ( sp.pos & 0xFF ) << 1 ) | ( sp.ctl & 1 );
				if( sp.armed && p == hstart )
				{
					sp.shiftA = sp.data;
					sp.shiftB = sp.datb;
					sp.count  = 16;
				}
				if( sp.count > 0 )
				{
					const int c = ( ( sp.shiftA >> 15 ) & 1 ) | ( ( ( sp.shiftB >> 15 ) & 1 ) << 1 );
					sp.shiftA = ( sp.shiftA << 1 ) & 0xFFFF;
					sp.shiftB = ( sp.shiftB << 1 ) & 0xFFFF;
					--sp.count;
					// The lowest-numbered sprite wins (HRM 4, "Sprite Priority").
					if( c != 0 && spriteNumber < 0 )
					{
						spriteNumber = s;
						spriteColour = c;
					}
				}
			}

			if( p == kDiwHStart - 1 )
				put( 0, y + 1, mColour[ 0 ] );

			const int x = p - kDiwHStart;
			if( x < 0 || x >= kWidth )
				continue;

			const uint16_t con0 = mRegs[ BPLCON0 >> 1 ];
			int            bpu  = std::min( ( con0 >> 12 ) & 7, kMaxPlanes );
			if( debug.ignoreBpu )
				bpu = kMaxPlanes;

			int idx = 0;
			for( int i = 0; i < bpu; ++i )
			{
				const uint32_t addr = planeBase[ i ] + static_cast< uint32_t >( y * kRowBytes + ( x >> 4 ) * 2 );
				if( ( ReadWord( addr ) >> ( 15 - ( x & 15 ) ) ) & 1 )
					idx |= 1 << i;
			}
			mIndices[ static_cast< size_t >( y ) * kWidth + static_cast< size_t >( x ) ] = static_cast< uint8_t >( idx );

			// Single playfield: PF2P (BPLCON2 bits 5-3) places the playfield
			// among the sprite pairs (HRM 7, Table 7-2); a sprite pair whose
			// group number is below it is in front.
			const int pf2p  = ( mRegs[ BPLCON2 >> 1 ] >> 3 ) & 7;
			uint32_t  rgb   = mColour[ static_cast< size_t >( idx ) ];
			if( spriteNumber >= 0 && ( idx == 0 || ( spriteNumber >> 1 ) < pf2p ) )
				rgb = mColour[ static_cast< size_t >( 16 + ( spriteNumber >> 1 ) * 4 + spriteColour ) ];
			put( x + 1, y + 1, rgb );
		}

		while( wi < mWrites.size() && mWrites[ wi ].cck < lineEnd )
			Apply( mWrites[ wi++ ] );
		put( pw - 1, y + 1, mColour[ 0 ] );
		if( y == visible - 1 )
			bottomBorder = mColour[ 0 ];
	}

	while( wi < mWrites.size() )
		Apply( mWrites[ wi++ ] );

	for( int px = 0; px < pw; ++px )
	{
		put( px, 0, topBorder );
		put( px, visible + 1, bottomBorder );
	}
}

} // namespace copperlist::chip
