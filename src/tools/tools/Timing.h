#pragma once

#include <tools/Environment.h>
#include <tools/Tools.h>

#define TOOLS_MILLISECONDS_PER_SECOND (1000ULL)
#define TOOLS_MICROSECONDS_PER_MILLISECOND (1000ULL)
#define TOOLS_NANOSECONDS_PER_MICROSECOND (1000ULL)
#define TOOLS_MICROSECONDS_PER_SECOND (TOOLS_MILLISECONDS_PER_SECOND * TOOLS_MICROSECONDS_PER_MILLISECOND)
#define TOOLS_NANOSECONDS_PER_SECOND (TOOLS_MICROSECONDS_PER_SECOND * TOOLS_NANOSECONDS_PER_MICROSECOND)
#define TOOLS_NANOSECONDS_PER_MILLISECOND (TOOLS_NANOSECONDS_PER_MICROSECOND * TOOLS_MICROSECONDS_PER_MILLISECOND)

namespace tools {
    struct Timing {
        virtual uint64 mark( void ) = 0;
        virtual uint64 mark( uint64 ) = 0;
        virtual AutoDispose< Request > timer( uint64, uint64 * = 0 ) = 0;
    };
};  // tools namespace
