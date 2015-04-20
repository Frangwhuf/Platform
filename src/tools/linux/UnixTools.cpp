#include "../toolsprecompiled.h"

#include <tools/Tools.h>

using namespace tools;

///////////////////////
// Non-member functions
///////////////////////

bool
tools::impl::isAbnormalShutdown( void )
{
    // TODO: check for a signal or something?
    return true;
}
