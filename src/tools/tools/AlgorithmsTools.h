#pragma once

#include <tools/Interface.h>

namespace tools {
    // Random number functionality. Thread-local random sources can be accessed from the non-member functions.
    // Repeatable state-based random sources can be accessed from a RandomState structure. Note: RandomState
    // is not currently thread-safe.
    TOOLS_API sint32 randomS32(void);
    TOOLS_API uint32 randomU32(void);
    TOOLS_API uint64 randomU64(void);
    TOOLS_API double randomD(void);

    struct RandomState
        : tools::Disposable
    {
        virtual void reseed(uint32) = 0;
        virtual void reseed(uint8 const *, unsigned) = 0;
        virtual uint64 rndU64(void) = 0;
        virtual sint32 rndS32(void) = 0;
        virtual uint32 rndU32(void) = 0;
        virtual double rndD(void) = 0;
    };

    TOOLS_API tools::AutoDispose<RandomState> randomStateNew(void);
    TOOLS_API tools::AutoDispose<RandomState> randomStateNew(uint32);
    TOOLS_API tools::AutoDispose<RandomState> randomStateNew(uint8 const *, unsigned);
}; // namespace tools
