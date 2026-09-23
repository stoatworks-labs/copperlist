#pragma once

#include <string>

#include "Clock.h"
#include "Controls.h"
#include "Render.h"
#include "chip/Chipset.h"
#include "demo/Demo.h"

#include <FFGLSDK.h>

/**
	The plugin: an emulated Amiga running a cracktro, one field per 1/50 s.

	This class is the wiring. Everything that decides anything is elsewhere
	and testable without a host, and all of it but the last step without GL:

	  `chip/Chipset`  the copper, the blitter, sprites, colour registers and
	                  the beam that composes a field from them
	  `demo/Demo`     the cracktro, written against that chipset
	  `Render`        the field onto the output: Integer, Fit or Fit Smooth

	## Fields, not frames

	The host's clock is turned into a field number, `floor( t x 50 )` for PAL
	(60 for NTSC), in double, and the emulation runs every whole field that
	has elapsed since the last one it ran. At a 60 Hz host some frames repeat
	a field and none is interpolated, which is what a real Amiga on a 60 Hz
	monitor could never do and what a 50 Hz composition shows exactly.

	A jump -- a seek, a clip trigger, the first frame at Resolume's ~499
	million ms -- runs only the field it lands on. That is safe because every
	field is a pure function of its number and the settings; `cptest
	--replay` holds it to that, byte for byte.

	## Speeds without jumps

	Positions that a speed control drives are accumulated here (`Accumulator`):
	`position = base + ( field - anchor ) x rate`, re-anchored where the rate
	changes. At constant settings that is `rate x field`, so the purity above
	holds; when an operator drags `Scroll Speed`, the text carries on from
	where it was rather than leaping to `new speed x 25 million`.
*/
namespace copperlist
{
class CopperlistPlugin : public CFFGLPlugin
{
public:
	CopperlistPlugin();
	~CopperlistPlugin() override = default;

	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float    GetFloatParameter( unsigned int index ) override;
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char*    GetTextParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	/// The settings in the demo's units.
	demo::Scene CurrentScene() const;

	/// Where the field lands on a raster of this size.
	Layout LayoutFor( int width, int height ) const;

	/// The field a host time (seconds) names at this field rate.
	static int64_t FieldIndex( double seconds, int fieldHz );

	/// Bring the emulation up to the field `seconds` names. No GL. Returns
	/// true if a field was run.
	bool Advance( double seconds );

	/// What `ProcessOpenGL` does before it draws: read the host's clock
	/// (whatever `SetTime` last said, in whatever unit `Clock` settled on)
	/// and `Advance` to it. Public so the harness can drive the real clock
	/// path with no GL.
	bool Tick();

	int64_t LastField() const { return mLastField; }
	int     FieldsRunLastAdvance() const { return mFieldsRun; }

	const chip::Chipset& Chip() const { return mChip; }
	chip::Chipset&       ChipForTest() { return mChip; }
	demo::Demo&          DemoForTest() { return mDemo; }
	const demo::Demo&    DemoState() const { return mDemo; }

	void ForceSecondsClock() { mClock.ForceSeconds(); }
	void ForceMillisecondsClock() { mClock.ForceMilliseconds(); }
	double ClockSeconds() const { return mClock.Seconds(); }

	/// Negative controls for `--fields`: the field number from a float, and
	/// from a double with no allowance for the host's own rounding of t.
	bool debugFloatClock   = false;
	bool debugExactFloor   = false;

	/// Fields a single host frame will run before it treats the gap as a jump.
	static constexpr int kCatchUp = 4;

private:
	struct Accumulator
	{
		int64_t base = 0, anchor = 0, rate = 0;
		bool    set  = false;

		int64_t At( int64_t field ) const { return base + ( field - anchor ) * rate; }
		void    Rate( int64_t r, int64_t field )
		{
			if( !set )
			{
				rate = r;
				set  = true;
				return;
			}
			if( r != rate )
			{
				base   = At( field );
				anchor = field;
				rate   = r;
			}
		}
		void Reanchor( int64_t fromField, int64_t toField )
		{
			base   = At( fromField );
			anchor = toField;
		}
	};

	int  Int( unsigned int param, int lo, int hi ) const;
	int  OptionIndex( unsigned int param, int count ) const;
	void RunField( int64_t field, const demo::Scene& scene );
	demo::Clocks ClocksAt( int64_t field ) const;

	float       mParams[ PT_COUNT_ ] = {};
	std::string mText;

	chip::Chipset mChip;
	demo::Demo    mDemo;
	Renderer      mRenderer;
	Clock         mClock;

	Accumulator mScroll, mBarPhase, mSpinX, mSpinY, mStars, mCycle;

	int64_t mLastField = 0;
	bool    mHaveField = false;
	bool    mDirty     = true;
	bool    mUploaded  = false;
	int     mLastHz    = 0;
	int     mFieldsRun = 0;

	int         mInstanceId = 0;
	std::string mTag;
	bool        mGlReady      = false;
	bool        mHostTimeSeen = false;
	double      mHostTime     = 0.0;
};

} // namespace copperlist
