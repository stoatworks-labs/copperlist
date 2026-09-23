#pragma once

/**
    What time it is, in seconds, whatever the host thinks it is sending.

    **FFGL never says what unit `SetTime` arrives in, and hosts disagree.**
    Resolume sends *milliseconds* -- measured live at 20.0 per frame at its 50fps,
    and the SDK's own Particles sample divides by 1000 -- while an offline harness
    naturally sends seconds. Reading the value raw is exactly right on the host
    that gets tested and a thousand times fast on the host that matters, which is
    precisely how this stays hidden until someone loads the plugin in Arena and
    every animation is a strobe.

    The fleet paid for this once already: the same bug was repaired across
    fourteen repos after the fact.

    ## Measured, not guessed

    Carried from flipbook, whose earlier version guessed the unit from the
    magnitude of one frame delta and had three holes: a delta between 0.5 and 2.0
    decided nothing, a burst of sub-millisecond frames at load -- a thumbnail
    render on a quick GPU -- locked it to "seconds" for the session, and while
    undecided it assumed seconds, which is the millisecond host's wrong answer.

    So this measures. `steady_clock` says how much real time passed, the host says
    how much host time passed, and the ratio names the unit outright. Nothing
    plausible sits between 1 and 1000, so both bands are wide and a frame fitting
    neither does not vote at all. Several frames must agree before it locks, so
    one odd frame -- the first after a seek -- cannot decide it alone.

    Until it is settled, and for a host that never calls `SetTime`, the real
    clock is used: wrong in origin, right in rate.
*/
namespace copperlist
{
class Clock
{
public:
	/// Call once per frame, before reading `Seconds()`. `hostTime` is whatever
	/// the host last handed to `SetTime`, or negative if it never has.
	void Tick( double hostTime, bool hostTimeSeen );

	/// Elapsed time in real seconds.
	double Seconds() const { return mSeconds; }

	/// "milliseconds", "seconds", or "undecided" -- for the log and the harness.
	const char* Unit() const;

	/// Force a unit, for the offline harness, where guessing is noise.
	void ForceSeconds() { mScale = 1.0; mForced = true; }

	/// Force milliseconds, for the harness to drive the clock the way
	/// Resolume does -- including at Resolume's measured ~499 million ms.
	void ForceMilliseconds() { mScale = 0.001; mForced = true; }

private:
	static constexpr int kVotesNeeded = 4;

	double mScale        = 0.0;///< 0 until decided; then 1.0 or 0.001
	double mSeconds      = 0.0;
	double mWallStart    = -1.0;
	double mLastRaw      = -1.0;
	double mLastWall     = -1.0;
	int    mSecondsVotes = 0;
	int    mMillisVotes  = 0;
	bool   mForced       = false;
};

} // namespace copperlist
