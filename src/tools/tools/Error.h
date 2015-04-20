#pragma once

#include <tools/Concurrency.h>
#include <tools/Interface.h>
#include <tools/InterfaceTools.h>

namespace tools {
    struct Error
      : Referenced< Error >
    {};

    TOOLS_API AutoDispose< Error::Reference > errorCancelNew( void );

    typedef Delegate< Error * > Completion;

    ///
    // A convenience template for creating callback error thunks.
    template< typename ImplementationT >
    struct Completable
        : tools::Delegatable< ImplementationT, Error * >
    {
        template< void ( ImplementationT::*ErrorT )( tools::Error * ) >
        tools::Completion
        toCompletion( void )
        {
            return this->template toDelegate< ErrorT >();
        }
    };
};  // tools namespace
