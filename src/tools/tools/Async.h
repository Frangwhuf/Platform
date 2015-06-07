#pragma once

#include <tools/Error.h>
#include <tools/Interface.h>

namespace tools {
    struct Request
        : Disposable
    {
        virtual void start( Completion const & ) = 0;
    };

    TOOLS_API AutoDispose< Error::Reference > runRequestSynchronously( NoDispose< Request > const & );

    struct Generator
        : Request
    {
        virtual bool next( void ) = 0;
    };
};  // tools namespace
