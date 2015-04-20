#include "../toolsprecompiled.h"

#include <tools/Tools.h>

#include <stdio.h>
#include <stdlib.h>

using namespace tools;

extern "C" {
  int ToolsHandleAssertFailure( char const * txt, char const * file, int line )
  {
    printf( "Assert failure - %s\n%s:%d\n", txt, file, line );
#ifdef TOOLS_RELEASE
    // Force a crash
    *(volatile char *)0;
#endif // TOOLS_RELEASE
    abort();
  }
} // extern C
