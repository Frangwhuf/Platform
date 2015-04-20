#include "toolsprecompiled.h"

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

#pragma comment(lib, "dbghelp")

using namespace tools;

namespace tools {
    namespace impl {
        void threadLocalThreadEnd( void );
    };  // impl namespace
};  // tools namespace

static DWORD tlsSlot;
static bool shutdownOk = true;

bool
tools::impl::isAbnormalShutdown( void )
{
    return !shutdownOk;
}

static BOOL WINAPI
CtrlHandlerCallback(
        DWORD /*ctrlType*/)
{
        shutdownOk = false;
        return FALSE;
}

BOOL WINAPI
DllMain(
        HINSTANCE,
        DWORD                dwReason,
        LPVOID)
{
        //Win32TlsData * tlsData;

        switch( dwReason ) {
        case DLL_PROCESS_ATTACH:
                // Configure this app into the assert handler
                //AssertSetHandler(AssertHandlerDiag);

                // Do not use a priority boosting pattern
                SetProcessPriorityBoost( GetCurrentProcess(), TRUE );
                //SetProcessAffinityUpdateMode( GetCurrentProcess(), 0 );

                tlsSlot = TlsAlloc();

                //InitializeCriticalSection(&win32Globals.diag_console_cs);
                //win32Globals.diag_console_prev_level = 0;
                //win32Globals.diag_console_prev_src = NULL

                SetConsoleCtrlHandler(&CtrlHandlerCallback, TRUE);
        case DLL_THREAD_ATTACH:
                // TODO: fill this in
                //tlsData = static_cast< Win32TlsData * >( affinity_platform().zalloc( sizeof( Win32TlsData )));
                //tlsData->diag_state.buffer = static_cast< char * >( affinity_platform().alloc( 4096 ));
                //tlsData->diag_state.buffer_sz = 4096;
                //tlsData->diag_state.indent_level = 0;
                //tlsData->lkCurrent = NULL;

                //TlsSetValue( tlsSlot, tls_data );
                break;
        case DLL_PROCESS_DETACH:
        case DLL_THREAD_DETACH:
                // Cleanup any registered systems
                if( dwReason != DLL_PROCESS_DETACH ) {
                        // TODO: fill this in
                        impl::threadLocalThreadEnd();
                        //tlsData = static_cast< Win32TlsData * >( TlsGetValue( tlsSlot ));
                        //heap::free( tlsData->diag_state.buffer );
                        //TOOLS_ASSERT( !tlsData->lkCurrent );
                        //heap::free( tlsData );
                }
                if( dwReason == DLL_PROCESS_DETACH ) {
                        SetConsoleCtrlHandler(&CtrlHandlerCallback, FALSE);
                        // TlsFree( tlsSlot );
                        // DeleteCriticalSection( &Win32Globals.diag_console_cs );
                }
                break;
        }

        return TRUE;
}
