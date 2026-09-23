#pragma once

#include <cmath>
#include <cstdint>

/**
	The demo's arithmetic: a sine table and an integer hash, and nothing that
	touches a float once the table exists.

	A cracktro did its trigonometry by table lookup in fixed point, because a
	7 MHz 68000 has no FPU and no time. This one does too, for a different
	reason: every position on screen is then an integer function of the field
	number and the settings, identical on every machine and every compiler,
	which is what lets `cptest` demand byte-identical fields.

	The one place a float appears is building the table. `std::sin` in double
	is correct to well under one ULP on every libm this will meet, and the
	result is rounded to Q14 -- so two libms could only disagree about an entry
	whose exact value sits within about 1e-12 of a half-step. None of the 1024
	does; `cptest --replay` would say so if one did on some other machine.
*/
namespace copperlist::demo
{
constexpr int kSineSteps = 1024;///< a full turn
constexpr int kSineOne   = 16384;///< Q14

/// sin( 2 pi i / 1024 ) in Q14, i taken modulo 1024.
inline int Sin( int64_t i )
{
	struct Table
	{
		int v[ kSineSteps ];
		Table()
		{
			for( int k = 0; k < kSineSteps; ++k )
				v[ k ] = static_cast< int >(
					std::lround( std::sin( 2.0 * 3.14159265358979323846 * k / kSineSteps ) * kSineOne ) );
		}
	};
	static const Table table;
	return table.v[ static_cast< int >( ( ( i % kSineSteps ) + kSineSteps ) % kSineSteps ) ];
}

inline int Cos( int64_t i )
{
	return Sin( i + kSineSteps / 4 );
}

/// Multiply by a Q14 fraction and floor. An arithmetic right shift of a
/// negative int floors on every compiler this builds with; it is written as a
/// division by a power of two with a correction so the claim does not depend
/// on that.
inline int MulQ14( int64_t a, int s )
{
	const int64_t p = a * s;
	return static_cast< int >( p >= 0 ? p / kSineOne : -( ( -p + kSineOne - 1 ) / kSineOne ) );
}

/// A PCG-style output mix: exact in 32 bits, the same everywhere.
inline uint32_t Hash( uint32_t x )
{
	x = x * 747796405u + 2891336453u;
	x = ( ( x >> ( ( x >> 28u ) + 4u ) ) ^ x ) * 277803737u;
	return ( x >> 22u ) ^ x;
}

/// A non-negative modulo, for positions that run forever.
inline int64_t Wrap( int64_t v, int64_t m )
{
	return ( ( v % m ) + m ) % m;
}

} // namespace copperlist::demo
