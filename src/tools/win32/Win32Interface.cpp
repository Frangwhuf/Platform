#include "toolsprecompiled.h"

#include <tools/Interface.h>
#include <tools/String.h>
#include <tools/Tools.h>

#ifdef WINDOWS_PLATFORM
#  pragma warning( disable : 4987 )
#endif // WINDOWS_PLATFORM
#include "Win32Tools.h"
#include <assert.h>
#include <DbgHelp.h>
#ifdef WINDOWS_PLATFORM
#  pragma warning( default : 4987 )
#endif // WINDOWS_PLATFORM

#include <boost/cast.hpp>

#pragma comment(lib, "dbghelp")

using namespace tools;
using boost::numeric_cast;

StringId
detail::platformDemangleTypeInfo(
    std::type_info const & typ) throw()
{
    char const * raw = typ.name();

    if( ( raw[0] == 's' ) && ( raw[1] == 't' ) && ( raw[2] == 'r' ) &&
        ( raw[3] == 'u' ) && ( raw[4] == 'c' ) && ( raw[5] == 't' ) &&
        ( raw[6] == ' ' )) {
            // Remove leading 'struct'
            raw += 7;
    } else if( ( raw[0] == 'c' ) && ( raw[1] == 'l' ) && ( raw[2] == 'a' ) &&
        ( raw[3] == 's' ) && ( raw[4] == 's' ) && ( raw[5] == ' ' )) {
            // Remove leading 'class'
            raw += 6;
    } else if( ( raw[0] == 'e' ) && ( raw[1] == 'n' ) && ( raw[2] == 'u' ) &&
        ( raw[3] == 'm' ) && ( raw[4] == ' ' )) {
            // Remove leading 'enum'
            raw += 5;
    }
    return StringId( raw );
}

StringId
detail::platformDemangleSymbol(
    StringId const & s) throw()
{
    return s;
}

static bool symbolInit = false;
static SYMBOL_INFO * symbolInfo;

StringId
detail::platformSymbolNameFromAddress(
    void * site,
    unsigned * offset )
{
    if( !!offset ) {
        *offset = 0;  // TODO: implement this
    }
    // This is lock protected by the user, so we don't need to worry about that here.
    if( !symbolInit ) {
        SymSetOptions( SYMOPT_DEFERRED_LOADS );
        SymInitialize( GetCurrentProcess(), NULL, TRUE );
        // Bypass allocation checking for this structure
        symbolInfo = static_cast< SYMBOL_INFO * >( HeapAlloc( GetProcessHeap(), 0,
            sizeof( SYMBOL_INFO ) + sizeof( char[ 256 ])));
        symbolInit = true;
    }
    symbolInfo->SizeOfStruct = sizeof( SYMBOL_INFO );
    symbolInfo->MaxNameLen = 255;
    if( !SymFromAddr( GetCurrentProcess(), (DWORD64)(size_t)site, NULL, symbolInfo )) {
        return StringIdNull();
    }
    return StringId( symbolInfo->Name, numeric_cast<sint32>( symbolInfo->NameLen ) );
}

void
detail::logStackTrace(
    bool,
    bool )
{
    // Not implemented
}