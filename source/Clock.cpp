#include "Clock.h"

#include <chrono>

namespace copperlist
{
namespace
{
double WallSeconds()
{
	using namespace std::chrono;
	return duration_cast< duration< double > >( steady_clock::now().time_since_epoch() ).count();
}
} // namespace

const char* Clock::Unit() const
{
	if( mScale == 0.0 )
		return "undecided";
	return mScale == 0.001 ? "milliseconds" : "seconds";
}

void Clock::Tick( double hostTime, bool hostTimeSeen )
{
	const double wallNow = WallSeconds();
	if( mWallStart < 0.0 )
		mWallStart = wallNow;

	// Never read the host's time before the host has set it: CFFGLPlugin's
	// constructor leaves `hostTime` uninitialised, so until SetTime lands it is
	// whatever happened to be in that memory.
	const double raw = hostTimeSeen ? hostTime : -1.0;

	if( !mForced && mScale == 0.0 && raw >= 0.0 && mLastRaw >= 0.0 && mLastWall >= 0.0 )
	{
		const double hostDelta = raw - mLastRaw;
		const double wallDelta = wallNow - mLastWall;

		// A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++mSecondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++mMillisVotes;

			if( mSecondsVotes >= kVotesNeeded || mMillisVotes >= kVotesNeeded )
				mScale = mMillisVotes > mSecondsVotes ? 0.001 : 1.0;
		}
	}

	if( raw >= 0.0 )
		mLastRaw = raw;
	mLastWall = wallNow;

	mSeconds = ( raw >= 0.0 && mScale != 0.0 ) ? raw * mScale : ( wallNow - mWallStart );
}

} // namespace copperlist
