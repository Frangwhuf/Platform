#include "toolsprecompiled.h"

#include <tools/Error.h>

using namespace tools;

////////
// Types
////////

struct CancelError
    : StandardStaticReferenced< CancelError, Error >
{
    CancelError( void );
};

///////////////////////
// Non-member Functions
///////////////////////

AutoDispose< Error::Reference >
tools::errorCancelNew( void )
{
    static CancelError ret;
    return static_cast< Error::Reference * >( &ret );
}

//////////////
// CancelError
//////////////

CancelError::CancelError( void )
{
}
