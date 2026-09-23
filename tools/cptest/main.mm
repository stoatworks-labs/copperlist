/**
	cptest -- the offline harness.

	It drives **the real code that ships**: `CopperlistPlugin` through its
	parameter list, its `SetTime` and its `ProcessOpenGL`, and the same
	`chip::Chipset` and `demo::Demo` the plugin runs, read back from chip RAM.

	    --copper      COLOR00 changes on the 4-pixel grid, >= 8 px apart, the
	                  later write wins and nothing is a blend
	    --line        the blitter's lines equal an independent Bresenham
	    --bitplanes   n planes never use an index at or above 2^n
	    --fields      field = floor( t x 50 ) exactly, every elapsed field run
	                  once; frames sharing a field are byte-identical
	    --replay      a field run up to equals the same field jumped to
	    --palette     every output channel a multiple of 17
	    --scroll      the scroller moves by exactly an integer, by exact match
	    --scaling     the output IS the chip's picture, Integer and Fit
	    --names       no parameter name over FFGL's 16 characters
	    --list        the fleet's parameter listing, for tools/sweep.py
	    --bench       emulation ms/field, and ms/frame at 720p, 1080p and 4K

	    --out f.png [--size WxH] [--frames N] [--set "Name=value" ...]
	                [--text "..."]
	    --pipe [--size WxH | --width W --height H] [--fps N] [--frames N]
	           [--script cues.txt] [--text "..."] [--set "Name=value" ...]

	Every check carries its own negative control: the model perturbed in one
	named way (chip::Debug, demo::Debug, the plugin's debug flags), and the
	check asserted to FAIL against it. A check that cannot fail is not one.

	## Where the measuring happens

	The copper, the lines, the bitplanes and the field arithmetic are claims
	about the emulated chip, so they are measured in chip RAM and in the
	chip's own picture, with no rasteriser in the room: they give the same
	answer on a GPU-less runner by construction. `--scaling` then proves, at
	1280x720 and at 320x180, that the output under Integer scaling is that
	picture bit for bit -- which is what lets the CPU-side checks speak for
	what is on screen. `--palette`, `--scroll`, `--fields` and `--replay` also
	read the output itself at both rasters.

	## The two rasters

	1280x720 is Integer x2, letterboxed with a whole-pixel origin. 320x180 is
	CI's raster: 256 lines do not fit in 180, so Integer stays at x1 and the
	field is cropped top and bottom, still texel for pixel. Both are pixel
	centres landing strictly inside texels -- (n + 1/2) / k is never within
	1/(2k) of an integer -- so no rasteriser can round one into its
	neighbour. See AGENTS.md, "Would this hold on another rasteriser, at
	another raster?".

	## --pipe

	The fleet's frame format: raw RGBA, top row first, on stdout, until
	`--frames` or until the reader hangs up. `--script` is `frame  Parameter
	Name  value` (or `frame  Name=value`) lines, held before the first key and
	after the last and linear between. Time is the frame counter: frame n is
	`SetTime( n / fps )`, so `--fps` is the clock the fields are counted on.

	    cptest --pipe --size 1920x1080 --fps 50 --script cues.txt \
	      | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -r 50 -i - out.mov
*/
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <fstream>
#include <functional>
#include <sstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Copperlist.h"
#include "demo/Font.h"

#include <unistd.h>

using namespace copperlist;

static int failures = 0;
static void Check( bool ok, const std::string& what )
{
	std::printf( "  %s %s\n", ok ? "ok  " : "FAIL", what.c_str() );
	if( !ok )
		++failures;
}

static std::string F( double v, int places = 3 )
{
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.*f", places, v );
	return buffer;
}

namespace
{
//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS. Carried from graticule via needle.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf                       compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
	std::vector< unsigned char > header;
	putU32( header, static_cast< uint32_t >( width ) );
	putU32( header, static_cast< uint32_t >( height ) );
	header.push_back( 8 );
	header.push_back( 6 );
	header.push_back( 0 );
	header.push_back( 0 );
	header.push_back( 0 );
	putChunk( png, "IHDR", header );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	std::FILE* file = std::fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	CGLPixelFormatAttribute attrs[] = { kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
										kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
										(CGLPixelFormatAttribute)0 };
	CGLPixelFormatObj       pix  = nullptr;
	GLint                   npix = 0;
	if( CGLChoosePixelFormat( attrs, &pix, &npix ) != kCGLNoError || pix == nullptr )
	{
		// No accelerated context (a CI runner): take whatever there is.
		CGLPixelFormatAttribute soft[] = { kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
										   kCGLPFAColorSize, (CGLPixelFormatAttribute)24, (CGLPixelFormatAttribute)0 };
		if( CGLChoosePixelFormat( soft, &pix, &npix ) != kCGLNoError || pix == nullptr )
			return nullptr;
	}
	CGLContextObj ctx = nullptr;
	if( CGLCreateContext( pix, nullptr, &ctx ) != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( ctx );
	return ctx;
}

bool openGL()
{
	static CGLContextObj ctx = nullptr;
	if( ctx == nullptr )
		ctx = createContext();
	if( ctx == nullptr )
		std::printf( "no GL context\n" );
	return ctx != nullptr;
}

struct Target
{
	GLuint fbo = 0, colour = 0;
	int    w = 0, h = 0;

	Target( int width, int height ) : w( width ), h( height )
	{
		glGenTextures( 1, &colour );
		glBindTexture( GL_TEXTURE_2D, colour );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glGenFramebuffers( 1, &fbo );
		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colour, 0 );
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	}
	~Target()
	{
		glDeleteFramebuffers( 1, &fbo );
		glDeleteTextures( 1, &colour );
	}
};

/// Top-down RGBA.
struct Image
{
	int                          w = 0, h = 0;
	std::vector< unsigned char > px;

	const unsigned char* at( int x, int y ) const
	{
		return px.data() + ( static_cast< size_t >( y ) * static_cast< size_t >( w ) + static_cast< size_t >( x ) ) * 4u;
	}
	uint32_t rgb( int x, int y ) const
	{
		const unsigned char* p = at( x, y );
		return static_cast< uint32_t >( p[ 0 ] ) << 16 | static_cast< uint32_t >( p[ 1 ] ) << 8 | p[ 2 ];
	}
	bool operator==( const Image& o ) const { return w == o.w && h == o.h && px == o.px; }
};

Image readBack( const Target& t )
{
	std::vector< unsigned char > raw( static_cast< size_t >( t.w ) * static_cast< size_t >( t.h ) * 4u );
	glBindFramebuffer( GL_FRAMEBUFFER, t.fbo );
	glReadPixels( 0, 0, t.w, t.h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data() );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	Image img;
	img.w = t.w;
	img.h = t.h;
	img.px.resize( raw.size() );
	for( int y = 0; y < t.h; ++y )
		std::memcpy( img.px.data() + static_cast< size_t >( y ) * static_cast< size_t >( t.w ) * 4u,
					 raw.data() + static_cast< size_t >( t.h - 1 - y ) * static_cast< size_t >( t.w ) * 4u,
					 static_cast< size_t >( t.w ) * 4u );
	return img;
}

/// The chip's own picture as an Image: (320 + 2) x (lines + 2), ring included.
Image chipPicture( const chip::Chipset& c )
{
	Image img;
	img.w  = c.PictureWidth();
	img.h  = c.PictureHeight();
	img.px = c.Picture();
	return img;
}

//---------------------------------------------------------------------------
// The plugin, driven the way a host drives it.
//---------------------------------------------------------------------------
unsigned int paramByName( CopperlistPlugin& p, const std::string& name )
{
	for( unsigned int id = 0; id < PT_COUNT_; ++id )
	{
		const char* n = p.GetParamName( id );
		if( n != nullptr && name == n )
			return id;
	}
	return PT_COUNT_;
}

/// A plugin with every scene off: the checks turn on what they measure.
void quiet( CopperlistPlugin& p )
{
	p.SetTextParameter( PT_TEXT, "" );
	p.SetFloatParameter( PT_BAR_COUNT, 0 );
	p.SetFloatParameter( PT_STAR_COUNT, 0 );
	p.SetFloatParameter( PT_CUBE_ON, 0 );
	p.SetFloatParameter( PT_BOB_COUNT, 0 );
	p.SetFloatParameter( PT_FONT_CYCLE, 0 );
}

struct Gl
{
	CopperlistPlugin& plugin;
	Target            target;

	Gl( CopperlistPlugin& p, int w, int h ) : plugin( p ), target( w, h )
	{
		FFGLViewportStruct vp = { 0, 0, static_cast< FFUInt32 >( w ), static_cast< FFUInt32 >( h ) };
		if( plugin.InitGL( &vp ) != FF_SUCCESS )
			std::printf( "  InitGL FAILED\n" );
	}
	~Gl() { plugin.DeInitGL(); }

	/// One host frame at host time `t` (whatever unit the clock was forced to).
	Image frame( double t )
	{
		ProcessOpenGLStruct gl = {};
		gl.HostFBO             = target.fbo;
		glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
		glViewport( 0, 0, target.w, target.h );
		glClearColor( 1.0f, 0.0f, 1.0f, 1.0f );// a sentinel no palette entry uses
		glClear( GL_COLOR_BUFFER_BIT );
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
		plugin.SetTime( t );
		plugin.ProcessOpenGL( &gl );
		return readBack( target );
	}
};

/// The two rasters every GL check runs at: the one this was developed at and
/// the one CI uses. 1280x720 is Integer x2, letterboxed; 320x180 is Integer
/// x1 with the top and bottom of the field cropped.
struct Raster
{
	int w, h;
};
const Raster kRasters[] = { { 1280, 720 }, { 320, 180 } };

/// Where playfield pixel (x, y) of the chip's picture lands in an output made
/// with Integer scaling: the output pixel whose centre maps into it.
bool integerPixel( const Layout& l, int px, int py, int& ox, int& oy )
{
	const int k = static_cast< int >( l.scaleX );
	ox          = static_cast< int >( l.originX ) + px * k;
	oy          = static_cast< int >( l.originY ) + py * k;
	return ox >= 0 && oy >= 0 && ox < l.width && oy < l.height;
}
int FloorDivide( int a, int b )
{
	return a >= 0 ? a / b : -( ( -a + b - 1 ) / b );
}
} // namespace

//---------------------------------------------------------------------------
// --names, --list
//---------------------------------------------------------------------------
int runNames()
{
	CopperlistPlugin plugin;
	std::printf( "names longer than FFGL's 16 characters, and duplicates:\n\n" );
	int                        bad = 0;
	std::vector< std::string > seen;
	for( unsigned int id = 0; id < PT_COUNT_; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name == nullptr )
			continue;
		if( std::strlen( name ) > 16 )
		{
			std::printf( "  %-3u  %-28s %zu characters\n", id, name, std::strlen( name ) );
			++bad;
		}
		if( std::find( seen.begin(), seen.end(), std::string( name ) ) != seen.end() )
		{
			std::printf( "  %-3u  %-28s is a duplicate\n", id, name );
			++bad;
		}
		seen.emplace_back( name );
		for( unsigned int e = 0; e < plugin.GetNumParamElements( id ); ++e )
		{
			const char* el = plugin.GetParamElementName( id, e );
			if( el != nullptr && std::strlen( el ) > 16 )
			{
				std::printf( "  %-3u  %-28s element %u: %s\n", id, name, e, el );
				++bad;
			}
		}
	}
	// The glyphs are the font's; the check is that every character of the
	// default text has one of its own rather than falling back to '?'.
	const std::string text = plugin.GetTextParameter( PT_TEXT );
	for( char c : text )
		if( c != '?' && demo::font::GlyphFor( c ) == demo::font::GlyphFor( '?' ) )
		{
			std::printf( "  the default text uses '%c', which the font has no glyph for\n", c );
			++bad;
		}
	std::printf( "\n  %d problem(s)\n", bad );
	return bad == 0 ? 0 : 1;
}

int runList()
{
	CopperlistPlugin plugin;
	std::printf( "%-4s %-22s %-9s %10s   %-16s\n", "id", "name", "kind", "value", "range" );
	for( unsigned int id = 0; id < PT_COUNT_; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( id >= PT_ABOUT_TEXT )
		{
			std::printf( "%-4u %-22s %-9s %10s   %-16s\n", id, name ? name : "", "about", "-", "-" );
			continue;
		}
		const char* kind = "standard";
		switch( plugin.GetParamType( id ) )
		{
		case FF_TYPE_BOOLEAN: kind = "boolean"; break;
		case FF_TYPE_EVENT: kind = "event"; break;
		case FF_TYPE_INTEGER: kind = "integer"; break;
		case FF_TYPE_OPTION: kind = "option"; break;
		case FF_TYPE_TEXT: kind = "text"; break;
		default: break;
		}
		if( plugin.GetParamType( id ) == FF_TYPE_TEXT )
		{
			std::printf( "%-4u %-22s %-9s %10s   %-16s\n", id, name ? name : "", kind, "-", "-" );
			continue;
		}
		RangeStruct range = plugin.GetParamRange( id );
		// An option's range reads back 0..1 whatever its element count (the
		// fleet trap), so the real one is printed for the sweep.
		if( plugin.GetParamType( id ) == FF_TYPE_OPTION )
		{
			range.min = 0.0f;
			range.max = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( id ) ) ) - 1.0f;
		}
		if( plugin.GetParamType( id ) == FF_TYPE_BOOLEAN )
		{
			range.min = 0.0f;
			range.max = 1.0f;
		}
		char rangeText[ 32 ] = {};
		std::snprintf( rangeText, sizeof( rangeText ), "[%g .. %g]", range.min, range.max );
		std::printf( "%-4u %-22s %-9s %10.4f   %-16s\n", id, name ? name : "", kind, plugin.GetFloatParameter( id ),
					 rangeText );
	}
	return 0;
}

//---------------------------------------------------------------------------
// Shared helpers for the checks.
//---------------------------------------------------------------------------
namespace
{
/// Run a plugin with no GL to host time `seconds` (clock forced to seconds).
void advanceTo( CopperlistPlugin& p, double seconds )
{
	p.SetTime( seconds );
	p.Tick();
}

/// A plugin with the clock forced to seconds and every scene on, the way
/// the checks that want "everything" see it.
void everything( CopperlistPlugin& p )
{
	p.ForceSecondsClock();
	p.SetFloatParameter( PT_BAR_WAVE, 0.5f );
	p.SetFloatParameter( PT_FILLED, 1.0f );
	p.SetFloatParameter( PT_FONT_CYCLE, 0.8f );
}

/// The copper list as the harness reads it: its own decoder over chip RAM,
/// sharing nothing with Chipset::CopperRun but the manual's encoding. For
/// each beam line, the COLOR00 values MOVEd while the copper was last told
/// to wait for that line, in list order.
std::map< int, std::vector< uint16_t > > colour0MovesByLine( const chip::Chipset& c )
{
	std::map< int, std::vector< uint16_t > > out;
	uint32_t                                 pc      = demo::map::kCopper;
	int                                      line    = -1;
	bool                                     wrapped = false;
	for( int guard = 0; guard < 16384; ++guard, pc += 4 )
	{
		const uint16_t w1 = c.ReadWord( pc ), w2 = c.ReadWord( pc + 2 );
		if( w1 == 0xFFFF && w2 == 0xFFFE )
			break;
		if( w1 & 1 )
		{
			if( w1 == 0xFFDF )
			{
				wrapped = true;
				continue;
			}
			line = ( w1 >> 8 ) + ( wrapped ? 256 : 0 );
			continue;
		}
		if( ( w1 & 0x1FE ) == chip::COLOR00 && line >= 0 )
			out[ line ].push_back( static_cast< uint16_t >( w2 & 0x0FFF ) );
	}
	return out;
}

/// Every set pixel of one bitplane, read out of chip RAM.
std::vector< uint8_t > planeBits( const chip::Chipset& c, int plane, int lines )
{
	std::vector< uint8_t > bits( static_cast< size_t >( chip::kWidth * lines ), 0 );
	const uint32_t         base = demo::map::kPlanes + static_cast< uint32_t >( plane ) * demo::map::kPlaneSize;
	for( int y = 0; y < lines; ++y )
		for( int x = 0; x < chip::kWidth; ++x )
			if( ( c.ReadWord( base + static_cast< uint32_t >( y * chip::kRowBytes + ( x >> 4 ) * 2 ) ) >>
				  ( 15 - ( x & 15 ) ) ) &
				1 )
				bits[ static_cast< size_t >( y * chip::kWidth + x ) ] = 1;
	return bits;
}

/// Bresenham, written from the closed form rather than the blitter's loop:
/// at major step i the minor offset is round-half-up( i dy / dx ), i.e.
/// floor( ( 2 i dy + dx ) / ( 2 dx ) ). "Half up" is the blitter's octant
/// rule at a tie: its sign bit is clear at a decision term of exactly zero,
/// so a tie steps the minor axis, in whatever direction the line is drawn.
/// Equal |dx| and |dy| counts as x-major, as the manual's set-up does.
void bresenham( int x1, int y1, int x2, int y2, const std::function< void( int, int ) >& plot )
{
	const int dxs = x2 - x1, dys = y2 - y1;
	const int adx = std::abs( dxs ), ady = std::abs( dys );
	const int sx = dxs < 0 ? -1 : 1, sy = dys < 0 ? -1 : 1;
	if( adx >= ady )
	{
		for( int i = 0; i <= adx; ++i )
		{
			const int m = adx == 0 ? 0 : ( 2 * i * ady + adx ) / ( 2 * adx );
			plot( x1 + sx * i, y1 + sy * m );
		}
	}
	else
	{
		for( int i = 0; i <= ady; ++i )
		{
			const int m = ( 2 * i * adx + ady ) / ( 2 * ady );
			plot( x1 + sx * m, y1 + sy * i );
		}
	}
}

int lines( const CopperlistPlugin& p )
{
	return p.Chip().GetStandard().visible;
}
} // namespace

//---------------------------------------------------------------------------
// --copper
//---------------------------------------------------------------------------
struct CopperResult
{
	int  offGrid = 0, tooClose = 0, notWritten = 0, wrongWinner = 0, outOfOrder = 0;
	int  midLineChanges = 0, crossingLines = 0, linesSeen = 0, atMinimum = 0;
	bool Pass() const
	{
		return offGrid == 0 && tooClose == 0 && notWritten == 0 && wrongWinner == 0 && outOfOrder == 0;
	}
};

/// The copper's claims, measured on the chip's own picture: background
/// pixels only (the playfield is empty), so every colour on screen is
/// COLOR00 as the copper left it at that beam position.
CopperResult measureCopper( CopperlistPlugin& p, const chip::Debug& perturb, int fieldsToRun )
{
	CopperResult r;
	p.ChipForTest().debug.moveCck     = perturb.moveCck;
	p.ChipForTest().debug.waitWakeCck = perturb.waitWakeCck;

	for( int f = 0; f < fieldsToRun; ++f )
	{
		advanceTo( p, ( f * 7 + 0.5 ) / 50.0 );
		const chip::Chipset& c   = p.Chip();
		const Image          img = chipPicture( c );
		const auto           mv  = colour0MovesByLine( c );
		const int            H   = lines( p );

		for( int y = 0; y < H; ++y )
		{
			const auto it = mv.find( chip::kDiwVStart + y );
			if( it == mv.end() )
				continue;
			const std::vector< uint16_t >& writes = it->second;
			std::set< uint32_t >           legal;
			for( uint16_t w : writes )
				legal.insert( chip::Expand12( w ) );
			++r.linesSeen;

			// Walk the line from the left border (x = -1, the ring) to the
			// right border (x = 320).
			std::vector< int >      changes;
			std::vector< uint32_t > sequence;
			uint32_t                prev = img.rgb( 0, y + 1 );
			sequence.push_back( prev );
			for( int x = 0; x <= chip::kWidth; ++x )
			{
				const uint32_t col = img.rgb( x + 1, y + 1 );
				if( !legal.count( col ) )
					++r.notWritten;// a colour no MOVE wrote: a blend, or worse
				if( col != prev )
				{
					if( x < chip::kWidth )
						changes.push_back( x );
					sequence.push_back( col );
				}
				prev = col;
			}

			// The four-pixel grid. A write lands at an even colour clock c,
			// the beam's low-res position 2c, which is x = 2c - $81: every
			// change sits at x + $81 = 0 (mod 4).
			for( int x : changes )
				if( ( x + chip::kDiwHStart ) % 4 != 0 )
					++r.offGrid;
			for( size_t i = 1; i < changes.size(); ++i )
			{
				if( changes[ i ] - changes[ i - 1 ] < 2 * chip::kMoveCck )
					++r.tooClose;
				if( changes[ i ] - changes[ i - 1 ] == 2 * chip::kMoveCck )
					++r.atMinimum;
			}
			r.midLineChanges += static_cast< int >( changes.size() );

			// The later write wins: the colour the line ends on is the last
			// COLOR00 MOVE the list made for it, and the colours along the
			// line appear in list order.
			if( writes.size() >= 2 )
				++r.crossingLines;
			if( chip::Expand12( writes.back() ) != img.rgb( chip::kWidth + 1, y + 1 ) )
				++r.wrongWinner;
			size_t k = 0;
			for( uint32_t col : sequence )
			{
				while( k < writes.size() && chip::Expand12( writes[ k ] ) != col )
					++k;
				if( k == writes.size() )
				{
					++r.outOfOrder;
					break;
				}
			}
		}
	}
	return r;
}

int runCopper()
{
	std::printf( "the copper, measured on the chip's picture\n\n"
				 "  grid: every change at x + $81 = 0 (mod 4)  -- HRM 2: HP's low bit is not compared,\n"
				 "        so a WAIT resolves 4 low-res pixels, and a MOVE is 4 colour clocks = 8 pixels\n"
				 "  spacing: no two changes on a line closer than one MOVE, 8 pixels\n"
				 "  winner: a line ends on the LAST COLOR00 its list wrote; nothing on it is a colour\n"
				 "          no MOVE wrote (so never a blend); colours appear in list order\n\n" );

	struct Case
	{
		const char* name;
		float       wave;
		bool        ntsc;
	};
	const Case cases[] = { { "PAL, bars full width", 0.0f, false },
						   { "PAL, Bar Wave 0.6", 0.6f, false },
						   { "NTSC, Bar Wave 1.0", 1.0f, true } };

	for( const Case& cs : cases )
	{
		CopperlistPlugin p;
		p.ForceSecondsClock();
		quiet( p );
		p.SetFloatParameter( PT_BAR_COUNT, 8 );
		p.SetFloatParameter( PT_BAR_HEIGHT, 40 );
		p.SetFloatParameter( PT_BAR_WAVE, cs.wave );
		p.SetFloatParameter( PT_STANDARD, cs.ntsc ? 1.0f : 0.0f );
		// Stars stay off: they are sprites, whose pixels are not COLOR00,
		// and this check reads every pixel as COLOR00.
		const CopperResult r = measureCopper( p, chip::Debug{}, 12 );
		std::printf( "  %s: %d lines, %d crossing, %d changes inside the line\n", cs.name, r.linesSeen,
					 r.crossingLines, r.midLineChanges );
		Check( r.offGrid == 0, std::string( cs.name ) + ": every change on the 4-pixel grid (" +
								   std::to_string( r.offGrid ) + " off)" );
		Check( r.tooClose == 0, std::string( cs.name ) + ": no two changes closer than 8 pixels (" +
									std::to_string( r.tooClose ) + " closer)" );
		Check( r.notWritten == 0, std::string( cs.name ) + ": no colour that no MOVE wrote (" +
									  std::to_string( r.notWritten ) + " pixels)" );
		Check( r.wrongWinner == 0 && r.outOfOrder == 0,
			   std::string( cs.name ) + ": the later write wins, colours in list order (" +
				   std::to_string( r.wrongWinner ) + " lines wrong, " + std::to_string( r.outOfOrder ) +
				   " out of order)" );
		Check( r.crossingLines > 0, std::string( cs.name ) + ": bars actually cross (not vacuous)" );
		if( cs.wave > 0.0f )
			Check( r.atMinimum > 0, std::string( cs.name ) + ": " + std::to_string( r.atMinimum ) +
										" pairs of changes sit exactly one MOVE (8 px) apart (not vacuous)" );
		if( cs.wave > 0.0f )
			Check( r.midLineChanges > 0, std::string( cs.name ) + ": colour changes inside the line exist" );
		else
			Check( r.midLineChanges == 0,
				   std::string( cs.name ) + ": every write in horizontal blank, one colour a line" );
	}

	std::printf( "\n  negative controls -- each must FAIL the check above:\n" );
	{
		CopperlistPlugin p;
		p.ForceSecondsClock();
		quiet( p );
		p.SetFloatParameter( PT_BAR_COUNT, 8 );
		p.SetFloatParameter( PT_BAR_HEIGHT, 40 );
		p.SetFloatParameter( PT_BAR_WAVE, 0.6f );
		chip::Debug d;
		d.moveCck                = 1;
		const CopperResult r     = measureCopper( p, d, 12 );
		Check( !r.Pass(), "a one-clock MOVE is rejected (" + std::to_string( r.tooClose ) + " too close, " +
							  std::to_string( r.offGrid ) + " off grid)" );
	}
	{
		CopperlistPlugin p;
		p.ForceSecondsClock();
		quiet( p );
		p.SetFloatParameter( PT_BAR_COUNT, 8 );
		p.SetFloatParameter( PT_BAR_HEIGHT, 40 );
		p.SetFloatParameter( PT_BAR_WAVE, 0.6f );
		chip::Debug d;
		d.waitWakeCck        = 1;
		const CopperResult r = measureCopper( p, d, 12 );
		Check( r.offGrid > 0, "an odd wake-up is rejected (" + std::to_string( r.offGrid ) + " changes off grid)" );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --line
//---------------------------------------------------------------------------
/// Blitter lines against the closed form. Returns the number of pixels that
/// differ over every line tried.
long measureLines( int bias, int& linesTried, long& pixelsLit )
{
	long diff  = 0;
	linesTried = 0;
	pixelsLit  = 0;

	// 1. Every octant, every slope class: from the centre of a fresh plane to
	//    points on a ring, including the ties (dx = 2 dy) and the diagonals.
	{
		chip::Chipset c;
		c.debug.lineErrBias = bias;
		const uint32_t plane = demo::map::kPlanes;
		for( int k = 0; k < 96; ++k )
		{
			const double a  = 2.0 * 3.14159265358979323846 * k / 96.0;
			const int    r  = 20 + ( k * 7 ) % 90;
			const int    x1 = 160, y1 = 128;
			int          x2 = x1 + static_cast< int >( std::lround( r * std::cos( a ) ) );
			int          y2 = y1 + static_cast< int >( std::lround( r * std::sin( a ) ) );
			if( k % 8 == 3 )
			{
				x2 = x1 + 2 * ( k % 24 + 3 ) * ( k % 16 < 8 ? 1 : -1 );// a tie: dx = 2 dy
				y2 = y1 + ( k % 24 + 3 ) * ( k % 32 < 16 ? 1 : -1 );
			}
			std::memset( c.Ram() + plane, 0, demo::map::kPlaneSize );
			demo::BlitLine( c, plane, x1, y1, x2, y2, false, false );
			const std::vector< uint8_t > got = planeBits( c, 0, 256 );
			std::vector< uint8_t >       want( got.size(), 0 );
			bresenham( x1, y1, x2, y2,
					   [ & ]( int x, int y ) { want[ static_cast< size_t >( y * chip::kWidth + x ) ] = 1; } );
			for( size_t i = 0; i < got.size(); ++i )
			{
				diff += got[ i ] != want[ i ];
				pixelsLit += got[ i ];
			}
			++linesTried;
		}
	}

	// 2. The cube as the demo draws it, over many fields and sizes.
	{
		CopperlistPlugin p;
		p.ForceSecondsClock();
		quiet( p );
		p.SetFloatParameter( PT_CUBE_ON, 1 );
		p.ChipForTest().debug.lineErrBias = bias;
		for( int f = 0; f < 40; ++f )
		{
			p.SetFloatParameter( PT_CUBE_SIZE, static_cast< float >( f % 5 ) / 4.0f );
			advanceTo( p, ( f * 13 + 0.5 ) / 50.0 );
			const std::vector< uint8_t > got = planeBits( p.Chip(), 3, lines( p ) );
			std::vector< uint8_t >       want( got.size(), 0 );
			for( const demo::Line& l : p.DemoState().CubeLines() )
			{
				bresenham( l.x1, l.y1, l.x2, l.y2,
						   [ & ]( int x, int y ) { want[ static_cast< size_t >( y * chip::kWidth + x ) ] = 1; } );
				++linesTried;
			}
			for( size_t i = 0; i < got.size(); ++i )
			{
				diff += got[ i ] != want[ i ];
				pixelsLit += got[ i ];
			}
		}
	}
	return diff;
}

int runLine()
{
	std::printf( "the blitter's line mode against an independent Bresenham\n\n"
				 "  set-up from HRM 6's register summary; octant bits from Appendix A (SUD/SUL/AUL);\n"
				 "  the reference is the closed form round-half-up( i dy / dx ), ties step the minor axis\n\n" );
	int        tried = 0;
	long       lit   = 0;
	const long diff  = measureLines( 0, tried, lit );
	Check( diff == 0, "every pixel of " + std::to_string( tried ) + " lines equals the reference (" +
						  std::to_string( diff ) + " differ, " + std::to_string( lit ) + " lit)" );
	Check( lit > 5000, "the lines are there to compare (not vacuous)" );

	std::printf( "\n  negative control -- must FAIL:\n" );
	const long bad = measureLines( 2, tried, lit );
	Check( bad > 0, "a decision term off by 2 (half a step) is rejected (" + std::to_string( bad ) + " pixels differ)" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --bitplanes
//---------------------------------------------------------------------------
int maxIndexUsed( CopperlistPlugin& p, int planes, bool ignoreBpu, int& used )
{
	p.SetFloatParameter( PT_BITPLANES, static_cast< float >( planes ) );
	p.ChipForTest().debug.ignoreBpu = ignoreBpu;
	int top = 0;
	std::set< int > distinct;
	for( int f = 0; f < 8; ++f )
	{
		advanceTo( p, ( f * 11 + planes * 3 + 0.5 ) / 50.0 );
		for( uint8_t i : p.Chip().Indices() )
		{
			top = std::max( top, static_cast< int >( i ) );
			distinct.insert( i );
		}
	}
	used = static_cast< int >( distinct.size() );
	return top;
}

int runBitplanes()
{
	std::printf( "with n bitplanes the playfield uses no index at or above 2^n (HRM 3, Table 3-5)\n\n" );
	CopperlistPlugin p;
	everything( p );
	for( int n = 1; n <= chip::kMaxPlanes; ++n )
	{
		int       used = 0;
		const int top  = maxIndexUsed( p, n, false, used );
		Check( top < ( 1 << n ) && top >= ( 1 << ( n - 1 ) ),
			   std::to_string( n ) + " plane(s): highest index " + std::to_string( top ) + " < " +
				   std::to_string( 1 << n ) + ", and plane " + std::to_string( n ) + " is in use (" +
				   std::to_string( used ) + " indices)" );
	}
	std::printf( "\n  negative control -- must FAIL:\n" );
	int       used = 0;
	const int top  = maxIndexUsed( p, 2, true, used );
	Check( top >= 4, "a display that ignores BPLCON0 is rejected at 2 planes (index " + std::to_string( top ) + ")" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --fields
//---------------------------------------------------------------------------
/// Frames f = 0..n-1 of a host at `fps` whose clock starts at frame `epoch`,
/// sent in milliseconds as Resolume sends them. Returns the number of frames
/// whose field was not floor( rate x frame / fps ) in exact integers; counts
/// in `badRuns` the frames that ran other than the number of fields elapsed.
int measureFields( bool floatClock, int64_t epoch, int n, bool ntsc, int fps, int& badRuns, bool exactFloor = false )
{
	CopperlistPlugin p;
	quiet( p );
	p.ForceMillisecondsClock();
	p.debugFloatClock = floatClock;
	p.debugExactFloor = exactFloor;
	p.SetFloatParameter( PT_STANDARD, ntsc ? 1.0f : 0.0f );
	const int64_t rate  = ntsc ? 60 : 50;
	int           wrong = 0;
	int64_t       last  = -1;
	badRuns             = 0;
	for( int f = 0; f < n; ++f )
	{
		const int64_t frame = epoch + f;
		p.SetTime( static_cast< double >( frame ) * 1000.0 / fps );
		p.Tick();
		if( p.LastField() != ( rate * frame ) / fps )
			++wrong;
		if( f > 0 && p.FieldsRunLastAdvance() != static_cast< int >( p.LastField() - last ) )
			++badRuns;
		last = p.LastField();
	}
	return wrong;
}

int runFields()
{
	std::printf( "the field number, from the host clock in milliseconds\n\n"
				 "  expected floor( t x 50 ) = floor( 50 f / fps ) at frame f, in exact integers (60 for NTSC);\n"
				 "  every frame runs exactly the fields that elapsed since the last\n\n" );
	// Resolume's clock was measured at ~499 million ms: frame 29,940,000.
	const int64_t epochs[] = { 0, 29940000, 600000000 };
	for( int64_t e : epochs )
	{
		for( bool ntsc : { false, true } )
			for( int fps : { 60, 30, 24 } )
			{
				int       runs  = 0;
				const int wrong = measureFields( false, e, 600, ntsc, fps, runs );
				Check( wrong == 0 && runs == 0, std::string( ntsc ? "NTSC" : "PAL " ) + " at " + std::to_string( fps ) +
													" fps from frame " + std::to_string( e ) + ": 600 frames, " +
													std::to_string( wrong ) + " wrong fields, " + std::to_string( runs ) +
													" frames that ran other than the fields elapsed" );
			}
	}

	// Frames that share a field are byte-identical, at both rasters; frames
	// that do not, differ (with the scroller moving).
	if( !openGL() )
		return 1;
	for( const Raster& r : kRasters )
	{
		CopperlistPlugin p;
		p.ForceSecondsClock();
		Gl        gl( p, r.w, r.h );
		Image     prev;
		int64_t   prevField = -1;
		int       sameBad = 0, diffBad = 0, pairs = 0;
		for( int f = 0; f < 36; ++f )
		{
			const Image img = gl.frame( f / 60.0 );
			if( prevField >= 0 )
			{
				++pairs;
				if( p.LastField() == prevField && !( img == prev ) )
					++sameBad;
				if( p.LastField() != prevField && img == prev )
					++diffBad;
			}
			prev      = img;
			prevField = p.LastField();
		}
		Check( sameBad == 0 && diffBad == 0,
			   std::to_string( r.w ) + "x" + std::to_string( r.h ) + ": " + std::to_string( pairs ) +
				   " frame pairs; shared field -> identical bytes, new field -> different (" +
				   std::to_string( sameBad ) + "/" + std::to_string( diffBad ) + " wrong)" );
	}

	std::printf( "\n  negative control -- must FAIL:\n" );
	int       runs  = 0;
	const int wrong = measureFields( true, 29940000, 600, false, 60, runs );
	Check( wrong > 0, "a float clock is rejected at Resolume's epoch (" + std::to_string( wrong ) + " of 600 frames wrong)" );
	int       exactWrong = 0;
	for( bool ntsc : { false, true } )
		exactWrong += measureFields( false, 0, 600, ntsc, 60, runs, true );
	Check( exactWrong > 0, "a bare floor( t x rate ), with no allowance for 1/60 not being a double, is rejected (" +
							   std::to_string( exactWrong ) + " frames a field early)" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --replay
//---------------------------------------------------------------------------
struct ReplayOutcome
{
	bool output = false, picture = false;
};

/// Run from frame `from` to `to` at 60 Hz, then compare with a fresh plugin
/// that jumps straight to `to`.
ReplayOutcome replayOnce( const Raster& r, int64_t from, int64_t to, bool accumulate )
{
	ReplayOutcome o;
	Image         runOut, jumpOut, runPic, jumpPic;
	{
		CopperlistPlugin p;
		everything( p );
		p.DemoForTest().debug.accumulateScroll = accumulate;
		Gl gl( p, r.w, r.h );
		for( int64_t f = from; f <= to; ++f )
			runOut = gl.frame( static_cast< double >( f ) / 60.0 );
		runPic = chipPicture( p.Chip() );
	}
	{
		CopperlistPlugin p;
		everything( p );
		p.DemoForTest().debug.accumulateScroll = accumulate;
		Gl gl( p, r.w, r.h );
		jumpOut = gl.frame( static_cast< double >( to ) / 60.0 );
		jumpPic = chipPicture( p.Chip() );
	}
	o.output  = runOut == jumpOut;
	o.picture = runPic == jumpPic;
	return o;
}

int runReplay()
{
	std::printf( "a field reached by running is byte-identical to the same field after a jump\n\n" );
	if( !openGL() )
		return 1;
	for( const Raster& r : kRasters )
	{
		const std::string at = std::to_string( r.w ) + "x" + std::to_string( r.h );
		{
			const ReplayOutcome o = replayOnce( r, 0, 400, false );
			Check( o.output && o.picture, at + ": frames 0..400 run vs a jump to 400: output " +
											  ( o.output ? "identical" : "DIFFERS" ) + ", chip picture " +
											  ( o.picture ? "identical" : "DIFFERS" ) );
		}
		{
			const ReplayOutcome o = replayOnce( r, 29940000, 29940240, false );
			Check( o.output && o.picture, at + ": at Resolume's epoch, 240 frames run vs a jump: " +
											  ( o.output && o.picture ? "identical" : "DIFFER" ) );
		}
	}

	// Resize mid-run: the field carried across a change of raster is the one
	// a fresh plugin draws at the new raster.
	{
		CopperlistPlugin p;
		everything( p );
		Image after;
		{
			Gl gl( p, 1280, 720 );
			for( int f = 0; f < 90; ++f )
				gl.frame( f / 60.0 );
		}
		{
			Gl gl( p, 320, 180 );
			for( int f = 90; f < 120; ++f )
				after = gl.frame( f / 60.0 );
		}
		CopperlistPlugin fresh;
		everything( fresh );
		Gl          gl( fresh, 320, 180 );
		const Image want = gl.frame( 119 / 60.0 );
		Check( after == want, "resize 1280x720 -> 320x180 mid-run: the next field equals a fresh render" );
	}

	std::printf( "\n  negative control -- must FAIL:\n" );
	const ReplayOutcome o = replayOnce( kRasters[ 1 ], 0, 400, true );
	Check( !o.output && !o.picture, "a scroller that accumulates its own position is rejected" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --palette
//---------------------------------------------------------------------------
struct PaletteResult
{
	long off = 0, pixels = 0;
	int  distinct = 0;
};

PaletteResult measurePalette( const Raster& r, int scaling, bool wide )
{
	PaletteResult    res;
	CopperlistPlugin p;
	everything( p );
	p.SetFloatParameter( PT_BAR_WAVE, 0.0f );// bars run into the border too
	p.SetFloatParameter( PT_SCALING, static_cast< float >( scaling ) );
	p.DemoForTest().debug.wideColour = wide;
	Gl                   gl( p, r.w, r.h );
	std::set< uint32_t > seen;
	for( int f = 0; f < 60; f += 7 )
	{
		const Image img = gl.frame( f / 60.0 );
		for( int y = 0; y < img.h; ++y )
			for( int x = 0; x < img.w; ++x )
			{
				const unsigned char* px = img.at( x, y );
				++res.pixels;
				if( px[ 0 ] % 17 || px[ 1 ] % 17 || px[ 2 ] % 17 || px[ 3 ] != 255 )
					++res.off;
				seen.insert( img.rgb( x, y ) );
			}
	}
	res.distinct = static_cast< int >( seen.size() );
	return res;
}

int runPalette()
{
	std::printf( "every output channel is a multiple of 17: four bits a gun, expanded (HRM Appendix A, COLORxx)\n\n" );
	if( !openGL() )
		return 1;
	for( const Raster& r : kRasters )
		for( int scaling : { 0, 1 } )
		{
			const PaletteResult res = measurePalette( r, scaling, false );
			Check( res.off == 0 && res.distinct >= 32,
				   std::to_string( r.w ) + "x" + std::to_string( r.h ) + ( scaling == 0 ? " Integer" : " Fit    " ) +
					   ": " + std::to_string( res.off ) + " of " + std::to_string( res.pixels ) +
					   " pixels off the 17-step lattice, " + std::to_string( res.distinct ) + " colours" );
		}
	std::printf( "\n  negative controls -- each must FAIL:\n" );
	for( const Raster& r : kRasters )
	{
		const PaletteResult res = measurePalette( r, 0, true );
		Check( res.off > 0, std::to_string( r.w ) + "x" + std::to_string( r.h ) +
								": 8-bit bar gradients are rejected (" + std::to_string( res.off ) + " pixels)" );
	}
	const PaletteResult smooth = measurePalette( kRasters[ 0 ], 2, false );
	Check( smooth.off > 0, "Fit Smooth, which blends, is rejected (" + std::to_string( smooth.off ) +
							   " pixels) -- the claim is for the nearest modes only" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --scroll
//---------------------------------------------------------------------------
/// The integer horizontal shift that maps `a` onto `b` exactly, over the
/// rows [y0, y1) and the columns [x0, x1) that stay inside both. Returns
/// every shift in [-range, range] with zero mismatches and at least `minLit`
/// non-black pixels compared.
std::vector< int > exactShifts( const Image& a, const Image& b, int x0, int x1, int y0, int y1, int range,
								long minLit )
{
	std::vector< int > found;
	for( int s = -range; s <= range; ++s )
	{
		long bad = 0, lit = 0;
		for( int y = y0; y < y1 && bad == 0; ++y )
			for( int x = std::max( x0, x0 + s ); x < std::min( x1, x1 + s ); ++x )
			{
				const uint32_t pa = a.rgb( x - s, y );
				const uint32_t pb = b.rgb( x, y );
				if( pa != pb )
				{
					++bad;
					break;
				}
				lit += pa != 0;
			}
		if( bad == 0 && lit >= minLit )
			found.push_back( s );
	}
	return found;
}

int runScroll()
{
	std::printf( "between consecutive fields the scroller moves by exactly an integer, found by exact matching\n\n" );
	if( !openGL() )
		return 1;
	const int speed = 3;

	// The chip's own field: shift in playfield pixels is -speed.
	{
		CopperlistPlugin p;
		p.ForceSecondsClock();
		quiet( p );
		p.SetTextParameter( PT_TEXT, "SCROLLING BY WHOLE PIXELS 0123456789 " );
		p.SetFloatParameter( PT_SCROLL_SPEED, speed );
		p.SetFloatParameter( PT_WAVE_HEIGHT, 0 );
		advanceTo( p, 40.5 / 50.0 );
		const Image a = chipPicture( p.Chip() );
		advanceTo( p, 41.5 / 50.0 );
		const Image b = chipPicture( p.Chip() );
		const auto  s = exactShifts( a, b, 1, 321, 1, lines( p ) + 1, 32, 200 );
		Check( s.size() == 1 && s[ 0 ] == -speed,
			   "chip field: exactly one shift matches, " + ( s.empty() ? std::string( "none" ) : std::to_string( s[ 0 ] ) ) +
				   " px (Scroll Speed " + std::to_string( speed ) + ")" );

		// With the wave on, every column is the flat column moved by a whole
		// number of lines.
		p.SetFloatParameter( PT_WAVE_HEIGHT, 0.8f );
		advanceTo( p, 41.5 / 50.0 );
		const Image w      = chipPicture( p.Chip() );
		int         cols   = 0, whole = 0;
		for( int x = 1; x <= chip::kWidth; ++x )
		{
			bool any = false;
			for( int y = 1; y <= lines( p ); ++y )
				any = any || b.rgb( x, y ) != 0;
			if( !any )
				continue;
			++cols;
			for( int dy = -48; dy <= 48; ++dy )
			{
				bool same = true;
				for( int y = 1; y <= lines( p ) && same; ++y )
				{
					const int ys = y - dy;
					const uint32_t flat = ( ys >= 1 && ys <= lines( p ) ) ? b.rgb( x, ys ) : 0u;
					same = w.rgb( x, y ) == flat;
				}
				if( same )
				{
					++whole;
					break;
				}
			}
		}
		Check( cols > 100 && whole == cols, "waved: " + std::to_string( whole ) + " of " + std::to_string( cols ) +
												" columns are the flat column moved by whole lines" );
	}

	// The output, at both rasters, Integer: the shift is speed x scale.
	for( const Raster& r : kRasters )
	{
		CopperlistPlugin p;
		p.ForceSecondsClock();
		quiet( p );
		p.SetTextParameter( PT_TEXT, "SCROLLING BY WHOLE PIXELS 0123456789 " );
		p.SetFloatParameter( PT_SCROLL_SPEED, speed );
		p.SetFloatParameter( PT_WAVE_HEIGHT, 0 );
		Gl           gl( p, r.w, r.h );
		const Layout l = p.LayoutFor( r.w, r.h );
		const int    k = static_cast< int >( l.scaleX );
		const Image  a = gl.frame( 40.5 / 50.0 );
		const Image  b = gl.frame( 41.5 / 50.0 );
		const int    x0 = std::max( 0, static_cast< int >( l.originX ) ), x1 = std::min( r.w, x0 + 320 * k );
		const auto   s  = exactShifts( a, b, x0, x1, 0, r.h, 32 * k, 200 );
		Check( s.size() == 1 && s[ 0 ] == -speed * k,
			   std::to_string( r.w ) + "x" + std::to_string( r.h ) + " Integer x" + std::to_string( k ) +
				   ": exactly one shift matches, " + ( s.empty() ? std::string( "none" ) : std::to_string( s[ 0 ] ) ) +
				   " px (expected " + std::to_string( -speed * k ) + ")" );
	}

	std::printf( "\n  negative control -- must FAIL:\n" );
	for( const Raster& r : kRasters )
	{
		CopperlistPlugin p;
		p.ForceSecondsClock();
		quiet( p );
		p.SetTextParameter( PT_TEXT, "SCROLLING BY WHOLE PIXELS 0123456789 " );
		p.SetFloatParameter( PT_SCROLL_SPEED, speed );
		p.SetFloatParameter( PT_WAVE_HEIGHT, 0 );
		p.SetFloatParameter( PT_SCALING, 2 );// Fit Smooth: a sub-pixel resample
		Gl           gl( p, r.w, r.h );
		const Layout l = p.LayoutFor( r.w, r.h );
		const Image  a = gl.frame( 40.5 / 50.0 );
		const Image  b = gl.frame( 41.5 / 50.0 );
		const int    x0 = std::max( 0, static_cast< int >( std::ceil( l.originX ) ) );
		const int    x1 = std::min( r.w, static_cast< int >( l.originX + 320 * l.scaleX ) );
		const auto   s  = exactShifts( a, b, x0, x1, 0, r.h, 32, 200 );
		Check( s.empty(), std::to_string( r.w ) + "x" + std::to_string( r.h ) + " Fit Smooth (x" +
							  F( l.scaleX, 4 ) + "): no whole-pixel shift reproduces the next field" );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --scaling
//---------------------------------------------------------------------------
int runScaling()
{
	std::printf( "the output is the chip's picture: Integer bit for bit, Fit texel for texel\n\n" );
	if( !openGL() )
		return 1;
	for( const Raster& r : kRasters )
	{
		const std::string at = std::to_string( r.w ) + "x" + std::to_string( r.h );
		for( int bg = 0; bg < 3; ++bg )
		{
			CopperlistPlugin p;
			everything( p );
			p.SetFloatParameter( PT_BACKGROUND, static_cast< float >( bg ) );
			Gl           gl( p, r.w, r.h );
			const Image  img = gl.frame( 1.0 );
			const Image  pic = chipPicture( p.Chip() );
			const Layout l   = p.LayoutFor( r.w, r.h );
			const int    k   = static_cast< int >( l.scaleX );
			long         bad = 0;
			for( int y = 0; y < r.h; ++y )
				for( int x = 0; x < r.w; ++x )
				{
					const int  u      = FloorDivide( x - static_cast< int >( l.originX ), k );
					const int  v      = FloorDivide( y - static_cast< int >( l.originY ), k );
					const bool inside = u >= 0 && v >= 0 && u < l.playW && v < l.playH;
					const unsigned char* got = img.at( x, y );
					unsigned char        want[ 4 ];
					if( inside || bg == 0 )
					{
						const int tu = std::min( std::max( u + 1, 0 ), l.playW + 1 );
						const int tv = std::min( std::max( v + 1, 0 ), l.playH + 1 );
						std::memcpy( want, pic.at( tu, tv ), 4 );
					}
					else
					{
						want[ 0 ] = want[ 1 ] = want[ 2 ] = 0;
						want[ 3 ] = bg == 1 ? 255 : 0;
					}
					bad += std::memcmp( got, want, 4 ) != 0;
				}
			const char* names[] = { "Border", "Black", "Transparent" };
			Check( bad == 0, at + " Integer x" + std::to_string( k ) + ", Background " + names[ bg ] + ": " +
								 std::to_string( bad ) + " pixels differ from the chip's picture" );
		}

		// Fit, nearest: every output pixel is the texel its centre maps into.
		// A centre within 1e-3 of a texel boundary may go either way on
		// another GPU, so either neighbour is accepted there and only there.
		{
			CopperlistPlugin p;
			everything( p );
			p.SetFloatParameter( PT_SCALING, 1 );
			Gl           gl( p, r.w, r.h );
			const Image  img = gl.frame( 1.0 );
			const Image  pic = chipPicture( p.Chip() );
			const Layout l   = p.LayoutFor( r.w, r.h );
			long         bad = 0;
			for( int y = 0; y < r.h; ++y )
				for( int x = 0; x < r.w; ++x )
				{
					const double u = ( x + 0.5 - l.originX ) / l.scaleX;
					const double v = ( y + 0.5 - l.originY ) / l.scaleY;
					bool         ok = false;
					for( double du : { -1e-3, 0.0, 1e-3 } )
						for( double dv : { -1e-3, 0.0, 1e-3 } )
						{
							const int tu = std::min( std::max( static_cast< int >( std::floor( u + du ) ) + 1, 0 ), l.playW + 1 );
							const int tv = std::min( std::max( static_cast< int >( std::floor( v + dv ) ) + 1, 0 ), l.playH + 1 );
							ok = ok || std::memcmp( img.at( x, y ), pic.at( tu, tv ), 4 ) == 0;
						}
					bad += !ok;
				}
			Check( bad == 0, at + " Fit (x" + F( l.scaleY, 4 ) + ", non-square): " + std::to_string( bad ) +
								 " pixels are not the texel their centre maps into" );
		}

		// Pixel aspect, measured out of the picture: with a transparent
		// background the playfield is the opaque rectangle, and its width
		// over its height must be 320 x 1.0397 / 256. Each edge is within half
		// a pixel, so each extent within one.
		{
			CopperlistPlugin p;
			everything( p );
			p.SetFloatParameter( PT_SCALING, 1 );
			p.SetFloatParameter( PT_BACKGROUND, 2 );
			Gl          gl( p, r.w, r.h );
			const Image img = gl.frame( 1.0 );
			int         xmin = r.w, xmax = -1, ymin = r.h, ymax = -1;
			for( int y = 0; y < r.h; ++y )
				for( int x = 0; x < r.w; ++x )
					if( img.at( x, y )[ 3 ] == 255 )
					{
						xmin = std::min( xmin, x );
						xmax = std::max( xmax, x );
						ymin = std::min( ymin, y );
						ymax = std::max( ymax, y );
					}
			const Layout l     = p.LayoutFor( r.w, r.h );
			const double wantW = 320.0 * l.scaleX, wantH = 256.0 * l.scaleY;
			const double gotW = xmax - xmin + 1, gotH = ymax - ymin + 1;
			Check( std::abs( gotW - wantW ) <= 1.0 && std::abs( gotH - wantH ) <= 1.0 &&
					   std::abs( l.scaleX / l.scaleY - PixelAspect( false ) ) < 1e-6,
				   at + " Fit, PAL non-square: the opaque rectangle is " + F( gotW, 0 ) + " x " + F( gotH, 0 ) +
					   ", expected " + F( wantW, 2 ) + " x " + F( wantH, 2 ) + " (aspect " + F( PixelAspect( false ), 4 ) + ")" );
		}
	}

	std::printf( "\n  negative control -- must FAIL:\n" );
	{
		CopperlistPlugin p;
		everything( p );
		p.SetFloatParameter( PT_SCALING, 2 );
		Gl           gl( p, 1280, 720 );
		const Image  img = gl.frame( 1.0 );
		const Image  pic = chipPicture( p.Chip() );
		const Layout l   = p.LayoutFor( 1280, 720 );
		long         bad = 0;
		for( int y = 0; y < 720; ++y )
			for( int x = 0; x < 1280; ++x )
			{
				const int tu = std::min( std::max( static_cast< int >( std::floor( ( x + 0.5 - l.originX ) / l.scaleX ) ) + 1, 0 ), l.playW + 1 );
				const int tv = std::min( std::max( static_cast< int >( std::floor( ( y + 0.5 - l.originY ) / l.scaleY ) ) + 1, 0 ), l.playH + 1 );
				bad += std::memcmp( img.at( x, y ), pic.at( tu, tv ), 4 ) != 0;
			}
		Check( bad > 0, "Fit Smooth is not texel-for-texel (" + std::to_string( bad ) + " pixels blended)" );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
int runBench()
{
	if( !openGL() )
		return 1;

	// The emulation, alone: one field, no GL.
	{
		CopperlistPlugin p;
		everything( p );
		for( int f = 0; f < 10; ++f )
			advanceTo( p, f / 50.0 );
		double best = 1e9, worst = 0.0;
		for( int pass = 0; pass < 5; ++pass )
		{
			const auto start = std::chrono::steady_clock::now();
			for( int f = 0; f < 50; ++f )
				advanceTo( p, ( 10 + pass * 50 + f ) / 50.0 );
			const double ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() / 50.0;
			best  = std::min( best, ms );
			worst = std::max( worst, ms );
		}
		std::printf( "emulation, one field on the CPU (every scene on): %s ms (spread %s-%s)\n\n", F( best ).c_str(),
					 F( best ).c_str(), F( worst ).c_str() );
	}

	std::printf( "ms/frame, a new field every frame (so emulation + upload + draw):\n"
				 "five passes of 100 frames after a 20-frame warm-up, fastest quoted, glFinish both sides\n\n" );
	std::printf( "  %-12s %-10s %-14s %s\n", "raster", "ms/frame", "spread", "% of a 60 fps frame" );
	const Raster sizes[] = { { 1280, 720 }, { 1920, 1080 }, { 3840, 2160 } };
	for( const Raster& s : sizes )
	{
		CopperlistPlugin p;
		everything( p );
		Gl                  gl( p, s.w, s.h );
		ProcessOpenGLStruct pg = {};
		pg.HostFBO             = gl.target.fbo;
		for( int f = 0; f < 20; ++f )
		{
			p.SetTime( f / 50.0 );
			p.ProcessOpenGL( &pg );
		}
		glFinish();
		double best = 1e9, worst = 0.0;
		for( int pass = 0; pass < 5; ++pass )
		{
			const auto start = std::chrono::steady_clock::now();
			for( int f = 0; f < 100; ++f )
			{
				p.SetTime( ( 20 + pass * 100 + f ) / 50.0 );
				p.ProcessOpenGL( &pg );
			}
			glFinish();
			const double ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() / 100.0;
			best  = std::min( best, ms );
			worst = std::max( worst, ms );
		}
		std::printf( "  %-12s %-10s %-14s %.1f%%\n", ( std::to_string( s.w ) + "x" + std::to_string( s.h ) ).c_str(),
					 F( best ).c_str(), ( F( best ) + "-" + F( worst ) ).c_str(), 100.0 * best / ( 1000.0 / 60.0 ) );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --out
//---------------------------------------------------------------------------
int runOut( const std::string& path, int w, int h, int frames,
			const std::vector< std::pair< std::string, std::string > >& sets, const char* text )
{
	if( !openGL() )
		return 1;
	CopperlistPlugin plugin;
	plugin.ForceSecondsClock();
	if( text != nullptr )
		plugin.SetTextParameter( PT_TEXT, text );
	for( const auto& kv : sets )
	{
		const unsigned int id = paramByName( plugin, kv.first );
		if( id == PT_COUNT_ )
		{
			std::printf( "no parameter named '%s'\n", kv.first.c_str() );
			return 1;
		}
		plugin.SetFloatParameter( id, static_cast< float >( std::atof( kv.second.c_str() ) ) );
	}
	Gl    gl( plugin, w, h );
	Image img;
	for( int f = 0; f < std::max( 1, frames ); ++f )
		img = gl.frame( f / 60.0 );
	if( !writePng( path, w, h, img.px ) )
	{
		std::printf( "could not write %s\n", path.c_str() );
		return 1;
	}
	return 0;
}

//---------------------------------------------------------------------------
// --pipe: raw RGBA frames out, a cue sheet in. The fleet's format (astable's
// attest, needle's ndtest), so one filming script drives any plugin. A
// source reads nothing, so there is no stdin side.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

/// One `frame  Parameter Name  value` per line, or `frame  Name=value`. `#`
/// starts a comment. Held before the first key and after the last, linear
/// between; integer and option parameters are rounded by the plugin itself,
/// so key them one frame apart to cut.
std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream                  file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}
	std::string line;
	int         lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );
		int                frame = 0;
		if( !( in >> frame ) )
			continue;
		std::vector< std::string > words;
		std::string                word;
		while( in >> word )
			words.push_back( word );
		const std::string where = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
		if( words.empty() )
		{
			error = where;
			return {};
		}
		std::string  name;
		float        value  = 0.0f;
		const size_t equals = words.back().find( '=' );
		if( words.size() == 1 || equals != std::string::npos )
		{
			if( equals == std::string::npos )
			{
				error = where;
				return {};
			}
			value = std::strtof( words.back().substr( equals + 1 ).c_str(), nullptr );
			words.back().erase( equals );
		}
		else
		{
			value = std::strtof( words.back().c_str(), nullptr );
			words.pop_back();
		}
		for( const std::string& part : words )
			if( !part.empty() )
				name += name.empty() ? part : " " + part;
		if( name.empty() )
		{
			error = where;
			return {};
		}
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? static_cast< float >( frame - a.first ) / span : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	return track.back().second;
}

struct PipeOptions
{
	int         width = 1920, height = 1080;
	int         frames = 0;///< 0: until the reader hangs up
	double      fps    = 60.0;
	std::string scriptPath;
	const char* text = nullptr;
	std::vector< std::pair< std::string, std::string > > sets;
};

int runPipe( const PipeOptions& o )
{
	// A reader that stops early would otherwise kill this with SIGPIPE before
	// the plugin is shut down.
	std::signal( SIGPIPE, SIG_IGN );
	if( o.width <= 0 || o.height <= 0 || !( o.fps > 0.0 ) )
	{
		std::fprintf( stderr, "cptest: --pipe needs a positive size and --fps\n" );
		return 1;
	}
	if( !openGL() )
		return 1;

	int status = 0;
	{
		CopperlistPlugin plugin;
		plugin.ForceSecondsClock();
		if( o.text != nullptr )
			plugin.SetTextParameter( PT_TEXT, o.text );

		// Names to ids. The Text and the About block are not values to
		// automate -- an About button that "moved" would open a browser.
		std::map< std::string, unsigned int > byName;
		for( unsigned int id = PT_TEXT + 1; id < PT_ABOUT_TEXT; ++id )
			if( const char* name = plugin.GetParamName( id ) )
				byName[ name ] = id;

		for( const auto& kv : o.sets )
		{
			const auto found = byName.find( kv.first );
			if( found == byName.end() )
			{
				std::fprintf( stderr, "cptest: no parameter named '%s'\n", kv.first.c_str() );
				return 1;
			}
			plugin.SetFloatParameter( found->second, static_cast< float >( std::atof( kv.second.c_str() ) ) );
		}

		std::map< unsigned int, Track > automation;
		if( !o.scriptPath.empty() )
		{
			std::string error;
			const auto  tracks = loadScript( o.scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "cptest: %s\n", error.c_str() );
				return 1;
			}
			for( const auto& entry : tracks )
			{
				const auto found = byName.find( entry.first );
				if( found == byName.end() )
				{
					std::fprintf( stderr, "cptest: the script names \"%s\", which is not an automatable parameter (try --list)\n",
								  entry.first.c_str() );
					return 1;
				}
				automation[ found->second ] = entry.second;
			}
		}

		Gl         gl( plugin, o.width, o.height );
		const size_t bytes = static_cast< size_t >( o.width ) * static_cast< size_t >( o.height ) * 4u;
		for( int f = 0; o.frames <= 0 || f < o.frames; ++f )
		{
			for( const auto& track : automation )
				plugin.SetFloatParameter( track.first, valueAt( track.second, f ) );
			// Time is the frame counter, never a wall clock.
			const Image img = gl.frame( static_cast< double >( f ) / o.fps );
			size_t      written = 0;
			while( written < bytes )
			{
				const ssize_t put = write( STDOUT_FILENO, img.px.data() + written, bytes - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
			if( written < bytes )
				break;// the reader hung up; not a failure
		}
	}
	return status;
}

int main( int argc, char** argv )
{
	std::string                                          out;
	int                                                  w = 1280, h = 720, frames = 60;
	std::vector< std::pair< std::string, std::string > > sets;
	const char*                                          text = nullptr;
	bool                                                 pipe = false, framesGiven = false;
	PipeOptions                                          po;

	std::map< std::string, std::function< int() > > checks = {
		{ "--names", runNames },   { "--list", runList },       { "--copper", runCopper },
		{ "--line", runLine },     { "--bitplanes", runBitplanes }, { "--fields", runFields },
		{ "--replay", runReplay }, { "--palette", runPalette }, { "--scroll", runScroll },
		{ "--scaling", runScaling }, { "--bench", runBench },
	};

	for( int a = 1; a < argc; ++a )
	{
		const std::string arg = argv[ a ];
		if( checks.count( arg ) )
			return checks[ arg ]();
		if( arg == "--out" && a + 1 < argc )
			out = argv[ ++a ];
		else if( arg == "--size" && a + 1 < argc )
			std::sscanf( argv[ ++a ], "%dx%d", &w, &h );
		else if( arg == "--frames" && a + 1 < argc )
		{
			frames      = std::atoi( argv[ ++a ] );
			framesGiven = true;
		}
		else if( arg == "--pipe" )
			pipe = true;
		else if( arg == "--width" && a + 1 < argc )
			w = std::atoi( argv[ ++a ] );
		else if( arg == "--height" && a + 1 < argc )
			h = std::atoi( argv[ ++a ] );
		else if( arg == "--fps" && a + 1 < argc )
			po.fps = std::atof( argv[ ++a ] );
		else if( arg == "--script" && a + 1 < argc )
			po.scriptPath = argv[ ++a ];
		else if( arg == "--text" && a + 1 < argc )
			text = argv[ ++a ];
		else if( arg == "--set" && a + 1 < argc )
		{
			const std::string kv = argv[ ++a ];
			const size_t      eq = kv.find( '=' );
			if( eq == std::string::npos )
			{
				std::printf( "--set wants Name=value\n" );
				return 1;
			}
			sets.emplace_back( kv.substr( 0, eq ), kv.substr( eq + 1 ) );
		}
	}

	if( pipe )
	{
		po.width  = w;
		po.height = h;
		po.frames = framesGiven ? frames : 0;
		po.sets   = sets;
		po.text   = text;
		return runPipe( po );
	}
	if( !out.empty() )
		return runOut( out, w, h, frames, sets, text );

	std::printf( "usage: cptest --names | --list | --copper | --line | --bitplanes | --fields\n"
				 "              --replay | --palette | --scroll | --scaling | --bench\n"
				 "       cptest --out f.png [--size WxH] [--frames N] [--text T] [--set Name=value ...]\n"
				 "       cptest --pipe [--size WxH | --width W --height H] [--fps N] [--frames N]\n"
				 "              [--script cues.txt] [--text T] [--set Name=value ...]   raw RGBA on stdout\n" );
	return 2;
}
