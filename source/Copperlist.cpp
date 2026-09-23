#include "Copperlist.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

#include "Diag.h"

namespace copperlist
{
static_assert( PT_COUNT_ - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
			   "the About block's size changed with the generated header -- "
			   "add or remove a PT_ABOUT_BUTTON_n to match" );

namespace
{
const char* const kDefaultText =
	"COPPERLIST ... AN AMIGA CRACKTRO, EMULATED CHIP BY CHIP ... EVERY COLOUR ON THIS SCREEN IS A "
	"COPPER MOVE AND EVERY PIXEL A BLIT ... THE BARS ARE REGISTER WRITES AT THE START OF A LINE, "
	"THE STARS ARE ONE SPRITE PER LAYER MOVED DOWN THE SCREEN BY THE COPPER, THE CUBE IS THE "
	"BLITTER'S LINE MODE ... GREETINGS TO EVERYONE WHO EVER WAITED FOR THE BEAM ... "
	"STOATWORKS LABS 2026 ...          ";

/// Turn an option parameter back into an index. Resolume hands back the
/// element VALUE, which here is the index; a host that normalises would hand
/// back 0..1. Both are accepted.
int ToOption( float v, int count )
{
	if( count <= 1 )
		return 0;
	const int i = ( v <= 1.0f && count > 2 && v != std::floor( v ) )
					  ? static_cast< int >( v * static_cast< float >( count - 1 ) + 0.5f )
					  : static_cast< int >( v + 0.5f );
	return std::min( std::max( i, 0 ), count - 1 );
}
} // namespace

CopperlistPlugin::CopperlistPlugin()
{
	static std::atomic< int > sNextInstance{ 1 };
	mInstanceId = sNextInstance.fetch_add( 1 );
	mTag        = "[" + std::to_string( mInstanceId ) + "] ";
	mText       = kDefaultText;

	// A source: no inputs.
	SetMinInputs( 0 );
	SetMaxInputs( 0 );

	auto group = [ this ]( unsigned int from, unsigned int to, const char* name ) {
		for( unsigned int i = from; i <= to; ++i )
			SetParamGroup( i, name );
	};
	auto option = [ this ]( unsigned int id, const char* name, std::initializer_list< const char* > elements,
							float def ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( elements.size() ), def );
		unsigned int i = 0;
		for( const char* e : elements )
		{
			SetParamElementInfo( id, i, e, static_cast< float >( i ) );
			++i;
		}
		mParams[ id ] = def;
	};
	auto boolean = [ this ]( unsigned int id, const char* name, bool def ) {
		SetParamInfo( id, name, FF_TYPE_BOOLEAN, def );
		mParams[ id ] = def ? 1.0f : 0.0f;
	};
	auto standard = [ this ]( unsigned int id, const char* name, float def ) {
		SetParamInfo( id, name, FF_TYPE_STANDARD, def );
		mParams[ id ] = def;
	};
	// FF_TYPE_INTEGER is exempt from the 0..1 clamp, so these are the
	// machine's own units: pixels a field, lines, counts, planes.
	auto integer = [ this ]( unsigned int id, const char* name, int lo, int hi, int def ) {
		SetParamInfo( id, name, FF_TYPE_INTEGER, static_cast< float >( def ) );
		SetParamRange( id, static_cast< float >( lo ), static_cast< float >( hi ) );
		mParams[ id ] = static_cast< float >( def );
	};

	// -- Text ------------------------------------------------------------------
	SetParamInfo( PT_TEXT, "Text", FF_TYPE_TEXT, kDefaultText );
	integer( PT_SCROLL_SPEED, "Scroll Speed", 0, kScrollSpeedMax, 2 );
	standard( PT_WAVE_HEIGHT, "Wave Height", 0.40f );
	standard( PT_WAVE_LENGTH, "Wave Length", 0.55f );
	standard( PT_FONT_CYCLE, "Colour Cycle", 0.25f );
	group( PT_TEXT, PT_FONT_CYCLE, "Text" );

	// -- Bars ------------------------------------------------------------------
	integer( PT_BAR_COUNT, "Bar Count", 0, kBarCountMax, 5 );
	integer( PT_BAR_HEIGHT, "Bar Height", kBarHeightMin, kBarHeightMax, 18 );
	standard( PT_BAR_SPEED, "Bar Speed", 0.35f );
	standard( PT_BAR_WAVE, "Bar Wave", 0.0f );
	option( PT_BAR_PALETTE, "Bar Palette", { "Rainbow", "Fire", "Ice", "Mono" }, 0.0f );
	group( PT_BAR_COUNT, PT_BAR_PALETTE, "Bars" );

	// -- Stars -----------------------------------------------------------------
	integer( PT_STAR_COUNT, "Star Count", 0, kStarCountMax, 40 );
	integer( PT_STAR_SPEED, "Star Speed", kStarSpeedMin, kStarSpeedMax, 1 );
	integer( PT_LAYERS, "Layers", 1, kLayersMax, 3 );
	group( PT_STAR_COUNT, PT_LAYERS, "Stars" );

	// -- Cube ------------------------------------------------------------------
	boolean( PT_CUBE_ON, "Cube On", true );
	standard( PT_CUBE_SIZE, "Cube Size", 0.6f );
	standard( PT_SPIN_X, "Spin X", 0.62f );
	standard( PT_SPIN_Y, "Spin Y", 0.70f );
	boolean( PT_FILLED, "Filled", false );
	group( PT_CUBE_ON, PT_FILLED, "Cube" );

	// -- Bobs ------------------------------------------------------------------
	integer( PT_BOB_COUNT, "Bob Count", 0, kBobCountMax, 8 );
	option( PT_BOB_PATH, "Bob Path", { "Lissajous", "Circle", "Wave", "Figure Eight" }, 0.0f );
	group( PT_BOB_COUNT, PT_BOB_PATH, "Bobs" );

	// -- Machine ---------------------------------------------------------------
	option( PT_STANDARD, "Standard", { "PAL", "NTSC" }, 0.0f );
	integer( PT_BITPLANES, "Bitplanes", 1, kBitplanesMax, 5 );
	option( PT_SCALING, "Scaling", { "Integer", "Fit", "Fit Smooth" }, 0.0f );
	option( PT_PIXEL_ASPECT, "Pixel Aspect", { "Non-square", "Square" }, 0.0f );
	option( PT_BACKGROUND, "Background", { "Border", "Black", "Transparent" }, 0.0f );
	group( PT_STANDARD, PT_BACKGROUND, "Machine" );

	// -- About -----------------------------------------------------------------
	SetParamInfo( PT_ABOUT_TEXT, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_TEXT + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	group( PT_ABOUT_TEXT, PT_COUNT_ - 1, "About" );
}

FFResult CopperlistPlugin::InitGL( const FFGLViewportStruct* vp )
{
	if( mGlReady )
		return CFFGLPlugin::InitGL( vp );

	diag::init();
	auto glString = []( GLenum name ) {
		const GLubyte* s = glGetString( name );
		return s != nullptr ? std::string( reinterpret_cast< const char* >( s ) ) : std::string( "?" );
	};
	diag::info( mTag + "GL vendor=" + glString( GL_VENDOR ) + " renderer=" + glString( GL_RENDERER ) +
				" version=" + glString( GL_VERSION ) );

	if( !mRenderer.InitGL() )
	{
		diag::error( mTag + "InitGL failed: " + mRenderer.Note() );
		return FF_FAIL;
	}
	mUploaded = false;
	mGlReady  = true;
	return CFFGLPlugin::InitGL( vp );
}

FFResult CopperlistPlugin::DeInitGL()
{
	mRenderer.DeInitGL();
	mGlReady  = false;
	mUploaded = false;
	return FF_SUCCESS;
}

FFResult CopperlistPlugin::SetTime( double time )
{
	mHostTimeSeen = true;
	mHostTime     = time;
	return CFFGLPlugin::SetTime( time );
}

FFResult CopperlistPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT_ )
		return FF_FAIL;
	if( index >= PT_ABOUT_TEXT )
		return stoatworks::about::handleParam( index - PT_ABOUT_TEXT, value ) ? FF_SUCCESS : FF_FAIL;
	if( mParams[ index ] != value )
		mDirty = true;
	mParams[ index ] = value;
	return FF_SUCCESS;
}

float CopperlistPlugin::GetFloatParameter( unsigned int index )
{
	return index < PT_COUNT_ ? mParams[ index ] : 0.0f;
}

FFResult CopperlistPlugin::SetTextParameter( unsigned int index, const char* value )
{
	if( index == PT_TEXT )
	{
		const std::string next = value != nullptr ? value : "";
		if( next != mText )
			mDirty = true;
		mText = next;
		return FF_SUCCESS;
	}
	// Must return FF_SUCCESS for the About line, or no host can instantiate
	// the plugin: instantiateGL pushes every default back through the setters
	// and deletes the instance on the first FF_FAIL.
	if( index == PT_ABOUT_TEXT )
		return FF_SUCCESS;
	return FF_FAIL;
}

char* CopperlistPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_TEXT )
		return const_cast< char* >( mText.c_str() );
	if( index == PT_ABOUT_TEXT )
	{
		static const std::string text = stoatworks::about::textParam( 0 );
		return const_cast< char* >( text.c_str() );
	}
	return const_cast< char* >( "" );
}

int CopperlistPlugin::Int( unsigned int param, int lo, int hi ) const
{
	const int v = static_cast< int >( std::lround( mParams[ param ] ) );
	return std::min( std::max( v, lo ), hi );
}

int CopperlistPlugin::OptionIndex( unsigned int param, int count ) const
{
	return ToOption( mParams[ param ], count );
}

demo::Scene CopperlistPlugin::CurrentScene() const
{
	demo::Scene s;
	s.text        = mText;
	s.scrollSpeed = Int( PT_SCROLL_SPEED, 0, kScrollSpeedMax );
	s.waveHeight  = WaveHeightLines( mParams[ PT_WAVE_HEIGHT ] );
	s.waveLength  = WaveLengthColumns( mParams[ PT_WAVE_LENGTH ] );
	s.barCount    = Int( PT_BAR_COUNT, 0, kBarCountMax );
	s.barHeight   = Int( PT_BAR_HEIGHT, kBarHeightMin, kBarHeightMax );
	s.barWave     = BarWaveSteps( mParams[ PT_BAR_WAVE ] );
	s.barPalette  = OptionIndex( PT_BAR_PALETTE, static_cast< int >( demo::BarPalette::Count ) );
	s.starCount   = Int( PT_STAR_COUNT, 0, kStarCountMax );
	s.layers      = Int( PT_LAYERS, 1, kLayersMax );
	s.cubeOn      = mParams[ PT_CUBE_ON ] > 0.5f;
	s.cubeSize    = CubeHalfEdge( mParams[ PT_CUBE_SIZE ] );
	s.filled      = mParams[ PT_FILLED ] > 0.5f;
	s.bobCount    = Int( PT_BOB_COUNT, 0, kBobCountMax );
	s.bobPath     = OptionIndex( PT_BOB_PATH, static_cast< int >( demo::BobPath::Count ) );
	s.ntsc        = OptionIndex( PT_STANDARD, 2 ) == 1;
	s.bitplanes   = Int( PT_BITPLANES, 1, kBitplanesMax );
	return s;
}

Layout CopperlistPlugin::LayoutFor( int width, int height ) const
{
	const bool ntsc = OptionIndex( PT_STANDARD, 2 ) == 1;
	return ComputeLayout( width, height, chip::kWidth, ntsc ? chip::kNtsc.visible : chip::kPal.visible,
						  static_cast< Scaling >( OptionIndex( PT_SCALING, static_cast< int >( Scaling::Count ) ) ),
						  static_cast< Aspect >( OptionIndex( PT_PIXEL_ASPECT, static_cast< int >( Aspect::Count ) ) ),
						  ntsc,
						  static_cast< Background >( OptionIndex( PT_BACKGROUND, static_cast< int >( Background::Count ) ) ) );
}

int64_t CopperlistPlugin::FieldIndex( double seconds, int fieldHz )
{
	// floor( t x rate ), in double. The 1e-6 of a field (20 ns of host time)
	// absorbs the host's own representation of t: 1/60 s is not a double, so
	// the frame that lands exactly on a field boundary arrives a few ULPs
	// either side of it. At a 60 Hz host no frame is closer than 1/6 of a
	// field to a boundary it does not sit on, so this cannot move one that
	// is not ON a boundary. At 5e5 s a double resolves ~6e-11 s.
	return static_cast< int64_t >( std::floor( seconds * static_cast< double >( fieldHz ) + 1e-6 ) );
}

demo::Clocks CopperlistPlugin::ClocksAt( int64_t field ) const
{
	demo::Clocks c;
	c.scroll   = mScroll.At( field );
	c.barPhase = mBarPhase.At( field );
	c.spinX    = mSpinX.At( field );
	c.spinY    = mSpinY.At( field );
	c.stars    = mStars.At( field );
	c.cycle    = mCycle.At( field );
	return c;
}

void CopperlistPlugin::RunField( int64_t field, const demo::Scene& scene )
{
	mDemo.Field( mChip, scene, ClocksAt( field ), field );
	++mFieldsRun;
}

bool CopperlistPlugin::Advance( double seconds )
{
	const demo::Scene scene = CurrentScene();
	const int         hz    = scene.ntsc ? chip::kNtsc.fieldHz : chip::kPal.fieldHz;

	int64_t field = FieldIndex( seconds, hz );
	if( debugExactFloor )
		field = static_cast< int64_t >( std::floor( seconds * static_cast< double >( hz ) ) );
	if( debugFloatClock )
		field = static_cast< int64_t >( std::floor( static_cast< float >( seconds ) * static_cast< float >( hz ) ) );

	// A change of standard changes what a field number means, so every
	// position is carried across to the new numbering where it stood.
	if( mHaveField && mLastHz != 0 && hz != mLastHz )
	{
		for( Accumulator* a : { &mScroll, &mBarPhase, &mSpinX, &mSpinY, &mStars, &mCycle } )
			a->Reanchor( mLastField, field );
		mHaveField = false;
	}
	mLastHz = hz;

	const int64_t anchor = mHaveField ? mLastField : field;
	mScroll.Rate( scene.scrollSpeed, anchor );
	mBarPhase.Rate( BarSpeedQ8( mParams[ PT_BAR_SPEED ] ), anchor );
	mSpinX.Rate( SpinQ8( mParams[ PT_SPIN_X ] ), anchor );
	mSpinY.Rate( SpinQ8( mParams[ PT_SPIN_Y ] ), anchor );
	mStars.Rate( Int( PT_STAR_SPEED, kStarSpeedMin, kStarSpeedMax ), anchor );
	mCycle.Rate( CycleRateQ8( mParams[ PT_FONT_CYCLE ] ), anchor );

	mFieldsRun = 0;
	if( mHaveField && field == mLastField && !mDirty )
		return false;

	int64_t first = field;
	if( mHaveField && field > mLastField && field - mLastField <= kCatchUp )
		first = mLastField + 1;
	for( int64_t f = first; f <= field; ++f )
		RunField( f, scene );

	mLastField = field;
	mHaveField = true;
	mDirty     = false;
	return true;
}

bool CopperlistPlugin::Tick()
{
	mClock.Tick( mHostTime, mHostTimeSeen );
	return Advance( mClock.Seconds() );
}

FFResult CopperlistPlugin::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL == nullptr || !mGlReady )
		return FF_FAIL;

	const int width  = static_cast< int >( currentViewport.width );
	const int height = static_cast< int >( currentViewport.height );
	if( width <= 0 || height <= 0 )
		return FF_SUCCESS;

	const bool advanced = Tick();
	diag::stateChanged( mTag + "clock", mTag + "host clock is " + mClock.Unit() );
	if( advanced || !mUploaded )
	{
		mRenderer.Upload( mChip.Picture(), mChip.PictureWidth(), mChip.PictureHeight() );
		mUploaded = true;
	}
	mRenderer.Draw( LayoutFor( width, height ), pGL->HostFBO );
	return FF_SUCCESS;
}

} // namespace copperlist
