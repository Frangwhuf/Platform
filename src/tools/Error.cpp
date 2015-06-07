#include "toolsprecompiled.h"

#include <tools/Error.h>

using namespace tools;

////////
// Types
////////

struct CancelError
    : StandardStaticReferenced< CancelError, Error >
{
    CancelError(void);
};

///////////////////////
// Non-member Functions
///////////////////////

AutoDispose< Error::Reference >
tools::errorCancelNew( void )
{
    static CancelError ret;
    return ret.ref();
}

//////////////
// CancelError
//////////////

CancelError::CancelError( void )
{
}
