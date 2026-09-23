#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

/**
	The part of the Amiga's chipset that makes a cracktro look like one.

	Not an emulator of the machine: there is no 68000, no Paula, no disk, no
	interrupts and no DMA slot arbitration. What IS here is the set of things
	whose constraints are visible on screen, emulated at the level the
	*Amiga Hardware Reference Manual* (3rd ed.) describes them, so that the
	demo code in `demo/` has to live with them the way the original coders did:

	  - **chip RAM**, 512 KB of it, which the copper list, the bitplanes, the
	    font and the bob all live in, addressed in bytes like the real thing;
	  - **the copper**, reading real two-word instructions out of chip RAM --
	    MOVE, WAIT and SKIP, bit for bit as HRM ch. 2 encodes them -- with the
	    manual's timing: a MOVE is four colour clocks, a WAIT six, and a WAIT
	    compares the beam with the low bit of the horizontal position ignored;
	  - **Denise's half of the display**: 32 colour registers of 12 bits, up to
	    five low-resolution bitplanes selected by `BPLCON0`, and eight 16-pixel
	    sprites whose registers the copper may rewrite on any line;
	  - **the blitter**, area mode with its shifts, masks, minterms, descending
	    mode and fill, and line mode with the manual's octant table.

	## Time

	One colour clock (CCK) is one horizontal count: 227 per line (HRM 2, "The
	display sees all these lines as 227 1/2 colour clocks long"; this model
	uses 227 in both standards), two low-resolution pixels per CCK. The beam's
	low-resolution position within a line is `p = 2 * h`, and the display
	window starts at `p = kDiwHStart` ($81, the manual's standard DIWSTRT), so
	playfield pixel `x` is beam position `p = x + $81`.

	A register write lands at the CCK the MOVE completes, and takes effect for
	every pixel at or after `p = 2 * cck`. That is a modelling choice where the
	manual is silent: real Denise has a pipeline delay of a few pixels between
	a register write and the first pixel it colours, and the HRM does not give
	it. See AGENTS.md, "The manual, and where it is ambiguous".

	## The field

	`RunField` runs the copper over one whole field from the top of its list
	and composes the picture as the beam would have painted it. The blitter
	is instantaneous: every blit the demo issues completes before the field
	is displayed (a real demo double-buffers for the same effect). Registers
	and chip RAM persist from field to field exactly as on the machine, which
	is what `cptest --replay` exists to keep honest.
*/
namespace copperlist::chip
{
constexpr uint32_t kChipBytes = 512u * 1024u;

constexpr int kCckPerLine  = 227;
constexpr int kLoresPerLine = 2 * kCckPerLine;
constexpr int kDiwHStart   = 0x81;///< DIWSTRT HSTART, low-res beam position of x = 0
constexpr int kDiwVStart   = 0x2C;///< DIWSTRT VSTART, beam line of y = 0
constexpr int kWidth       = 320;
constexpr int kRowBytes    = kWidth / 8;
constexpr int kMaxPlanes   = 5;
constexpr int kSprites     = 8;

/// The copper's costs, in colour clocks (HRM 2, "What is a Copper
/// Instruction?"): a MOVE is two memory cycles on the odd slots only, so four
/// cycle times; a WAIT is three, so six; the extra one is the wake-up.
constexpr int kMoveCck     = 4;
constexpr int kWaitFetchCck = 4;
constexpr int kWaitWakeCck = 2;

/// A video standard, as the manual counts it.
struct Standard
{
	int lines;  ///< beam lines per field (non-interlaced)
	int visible;///< playfield lines (HRM 3, "Height and Width of the Playfield")
	int fieldHz;///< nominal field rate
};
constexpr Standard kPal{ 312, 256, 50 };
constexpr Standard kNtsc{ 262, 200, 60 };

/// Custom register offsets from $DFF000, the ones this model implements.
enum Reg : uint16_t
{
	BLTCON0  = 0x040,
	BLTCON1  = 0x042,
	BLTAFWM  = 0x044,
	BLTALWM  = 0x046,
	BLTCPTH  = 0x048,
	BLTCPTL  = 0x04A,
	BLTBPTH  = 0x04C,
	BLTBPTL  = 0x04E,
	BLTAPTH  = 0x050,
	BLTAPTL  = 0x052,
	BLTDPTH  = 0x054,
	BLTDPTL  = 0x056,
	BLTSIZE  = 0x058,
	BLTCMOD  = 0x060,
	BLTBMOD  = 0x062,
	BLTAMOD  = 0x064,
	BLTDMOD  = 0x066,
	BLTCDAT  = 0x070,
	BLTBDAT  = 0x072,
	BLTADAT  = 0x074,
	BPL1PTH  = 0x0E0,///< BPLxPTH/L at $E0 + 4 * (x - 1)
	BPLCON0  = 0x100,
	BPLCON1  = 0x102,
	BPLCON2  = 0x104,
	SPR0POS  = 0x140,///< SPRxPOS/CTL/DATA/DATB at $140 + 8 * x
	SPR0CTL  = 0x142,
	SPR0DATA = 0x144,
	SPR0DATB = 0x146,
	COLOR00  = 0x180,
};

/// Perturbations of the model. Every one of these makes the machine wrong in
/// one specific way; each exists so that a check in `cptest` can be shown to
/// FAIL against it. None is reachable from the plugin's parameters.
struct Debug
{
	int  moveCck      = kMoveCck;   ///< a copper whose MOVE is cheaper than the manual's
	int  waitWakeCck  = kWaitWakeCck;///< an odd wake-up puts writes off the 4-pixel grid
	bool ignoreBpu    = false;      ///< display every plane regardless of BPLCON0
	int  lineErrBias  = 0;          ///< added to the line-mode error term at BLTSIZE
	/// 24-bit colour, keyed by the chip address of the MOVE that carries it:
	/// the "8-bit colour" negative control. A MOVE found here stores this
	/// value in the register instead of its own 12 bits.
	std::unordered_map< uint32_t, uint32_t > wideColour;
};

class Chipset
{
public:
	Chipset();

	/// Power on: chip RAM and every register to zero.
	void Reset();

	void SetStandard( const Standard& s );
	const Standard& GetStandard() const { return mStandard; }

	// -- chip RAM, as the 68000 sees it --------------------------------------
	uint16_t ReadWord( uint32_t address ) const;
	void     WriteWord( uint32_t address, uint16_t value );
	uint8_t* Ram() { return mRam.data(); }
	const uint8_t* Ram() const { return mRam.data(); }

	/// A 68000 write to a custom register. A write to BLTSIZE starts the blit
	/// and, in this model, finishes it.
	void Poke( uint16_t reg, uint16_t value );

	/// Run one field: the copper from `copperList`, and the display.
	void RunField( uint32_t copperList );

	// -- what the beam painted -----------------------------------------------
	/// RGBA8, (320 + 2) x (visible + 2), top row first. The outer ring is the
	/// border: COLOR00 as the beam left it beside, above and below the display
	/// window, which is where a real monitor shows it.
	const std::vector< uint8_t >& Picture() const { return mPicture; }
	int PictureWidth() const { return kWidth + 2; }
	int PictureHeight() const { return mStandard.visible + 2; }

	/// The playfield colour index each window pixel was given, before sprites
	/// and priority, after BPLCON0's plane count. 320 x visible.
	const std::vector< uint8_t >& Indices() const { return mIndices; }

	/// The copper's register writes in the last field, in order. For the
	/// harness, which decodes the list itself and compares.
	struct Write
	{
		int      cck;    ///< colour clocks since the top of the field
		uint32_t source; ///< chip address of the MOVE
		uint16_t reg;
		uint16_t value;
	};
	const std::vector< Write >& Writes() const { return mWrites; }

	Debug debug;

private:
	void CopperRun( uint32_t list );
	int  WaitSatisfiedAt( int from, uint16_t w1, uint16_t w2 ) const;
	void Apply( const Write& w );
	void Compose();
	void Blit();
	void BlitLine();
	uint32_t Pointer( uint16_t hi ) const;
	void     SetPointer( uint16_t hi, uint32_t value );

	Standard mStandard = kPal;

	std::vector< uint8_t >  mRam;
	std::array< uint16_t, 0x100 > mRegs{};///< custom registers, word-indexed

	// Denise's live state while a field is composed.
	std::array< uint32_t, 32 > mColour{};///< RGB888, already expanded
	struct Sprite
	{
		uint16_t pos = 0, ctl = 0, data = 0, datb = 0;
		bool     armed = false;
		int      shiftA = 0, shiftB = 0, count = 0;
	};
	std::array< Sprite, kSprites > mSprite{};

	std::vector< Write >   mWrites;
	std::vector< uint8_t > mPicture;
	std::vector< uint8_t > mIndices;
};

/// Expand a 12-bit colour register to RGB888, as a DAC with four bits a gun
/// shows it: 0..15 to 0..255 in steps of 17.
inline uint32_t Expand12( uint16_t c )
{
	const uint32_t r = ( c >> 8 ) & 15u, g = ( c >> 4 ) & 15u, b = c & 15u;
	return ( r * 17u ) << 16 | ( g * 17u ) << 8 | ( b * 17u );
}

} // namespace copperlist::chip
