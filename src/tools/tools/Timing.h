#pragma once

#include <tools/Environment.h>
#include <tools/Tools.h>

#define TOOLS_MILLISECONDS_PER_SECOND (1000ULL)
#define TOOLS_MICROSECONDS_PER_MILLISECOND (1000ULL)
#define TOOLS_NANOSECONDS_PER_MICROSECOND (1000ULL)
#define TOOLS_MICROSECONDS_PER_SECOND (TOOLS_MILLISECONDS_PER_SECOND * TOOLS_MICROSECONDS_PER_MILLISECOND)
#define TOOLS_NANOSECONDS_PER_SECOND (TOOLS_MICROSECONDS_PER_SECOND * TOOLS_NANOSECONDS_PER_MICROSECOND)
#define TOOLS_NANOSECONDS_PER_MILLISECOND (TOOLS_NANOSECONDS_PER_MICROSECOND * TOOLS_MICROSECONDS_PER_MILLISECOND)

TOOLS_FORCE_INLINE tools::uint64 operator "" _ns(unsigned long long nanoseconds) { return static_cast<tools::uint64>(nanoseconds); }
TOOLS_FORCE_INLINE tools::uint64 operator "" _us(unsigned long long microseconds) { return static_cast<tools::uint64>(microseconds * TOOLS_NANOSECONDS_PER_MICROSECOND); }
TOOLS_FORCE_INLINE tools::uint64 operator "" _ms(unsigned long long milliseconds) { return static_cast<tools::uint64>(milliseconds * TOOLS_NANOSECONDS_PER_MILLISECOND); }
TOOLS_FORCE_INLINE tools::uint64 operator "" _s(unsigned long long seconds) { return static_cast<tools::uint64>(seconds * TOOLS_NANOSECONDS_PER_SECOND); }
TOOLS_FORCE_INLINE tools::uint64 operator "" _ns(long double nanoseconds) { return static_cast<tools::uint64>(nanoseconds); }
TOOLS_FORCE_INLINE tools::uint64 operator "" _us(long double microseconds) { return static_cast<tools::uint64>(microseconds * TOOLS_NANOSECONDS_PER_MICROSECOND); }
TOOLS_FORCE_INLINE tools::uint64 operator "" _ms(long double milliseconds) { return static_cast<tools::uint64>(milliseconds * TOOLS_NANOSECONDS_PER_MILLISECOND); }
TOOLS_FORCE_INLINE tools::uint64 operator "" _s(long double seconds) { return static_cast<tools::uint64>(seconds * TOOLS_NANOSECONDS_PER_SECOND); }

namespace tools {
    struct Timing {
        virtual uint64 mark( void ) = 0;
        virtual uint64 mark( uint64 ) = 0;
        virtual AutoDispose< Request > timer( uint64, uint64 * = 0 ) = 0;
    };
};  // tools namespace
