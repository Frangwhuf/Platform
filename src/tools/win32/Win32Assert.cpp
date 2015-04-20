#include "toolsprecompiled.h"

//#include <tools/Configuration.h>
//#include <tools/Notification.h>
#include <tools/Tools.h>

#include "Win32Tools.h"

#ifdef WINDOWS_PLATFORM
#  pragma warning( disable : 4365 )
#  pragma warning( disable : 4371 )
#  pragma warning( disable : 4571 )
#  pragma warning( disable : 4619 )
#endif // WINDOWS_PLATFORM
#include <boost/format.hpp>
#ifdef WINDOWS_PLATFORM
#  pragma warning( default : 4365 )
#  pragma warning( default : 4371 )
#  pragma warning( default : 4571 )
#  pragma warning( default : 4619 )
#endif // WINDOWS_PLATFORM
#include <unordered_set>
#include <windows.h>

using namespace tools;

//CONFIGURE_STATIC( configAssert );
//CONFIGURE_FUNCTION( configAssert ) {
//}

namespace {
  bool ControlHeld( void )
  {
    if( GetAsyncKeyState( VK_CONTROL ) & 0x8000 ) { return true; }
    if( GetAsyncKeyState( VK_LCONTROL ) & 0x8000 ) { return true; }
    if( GetAsyncKeyState( VK_RCONTROL ) & 0x8000 ) { return true; }
    return false;
  }
  //  The way that ToolsHandleAssertFailure() is called, a single assert will have 
  //  a unique and non-changing pair of file pointer and line integer (because 
  //  the file comes from __FILE__).
  //  Thus, we can use that information to determine which asserts are disabled.
  struct AssertHash
  {
    ptrdiff_t operator()( std::pair< char const *, int > const & p ) const {
      return ( p.first - (char const*)0 ) ^ ( p.second );
    }
  };
  bool AssertIsDisabled( char const * file, int line, bool makeDisabled )
  {
    static std::unordered_set< std::pair< char const *, int >, AssertHash, std::equal_to< std::pair< char const *, int > > > DisabledAsserts;
    bool ret = DisabledAsserts.find( std::pair< char const *, int >( file, line ) ) != DisabledAsserts.end();
    if( !ret && makeDisabled ) {
      DisabledAsserts.insert( std::pair< char const *, int >( file, line ) );
    }
    return ret;
  }

  struct MessageBoxParams {
    HWND parent;
    char const * msg;
    char const * title;
    DWORD options;
    DWORD result;
    HANDLE event;
  };

  static DWORD WINAPI SyncAssertThreadProc( LPVOID param )
  {
    MessageBoxParams & mbp = *(MessageBoxParams *)param;
    mbp.result = static_cast< DWORD >( ::MessageBoxA( mbp.parent, mbp.msg, mbp.title, mbp.options ));
    ::SetEvent( mbp.event );
    return 0;
  }

  ///
  // The assert will be in a thread of its own. To see the call stack of the main 
  // thread causing the assert, look for a thread blocked on the event "MainThreadInAssert" 
  // or inside the function SyncMessageBoxA().
  // If that doesn't work, put a breakpoint where SyncMessageBoxA returns, and dismiss the 
  // dialog (or just press the "Retry" button as usual).
  DWORD SyncMessageBoxA( HWND parent, char const * msg, char const * title, DWORD options )
  {
    HANDLE anEvent = ::CreateEventA( 0, false, false, "MainThreadInAssert" );
    MessageBoxParams mbp;
    mbp.parent = parent;
    mbp.msg = msg;
    mbp.title = title;
    mbp.options = options;
    mbp.result = 0;
    mbp.event = anEvent;
    DWORD threadId;
    HANDLE aThread = CreateThread( NULL, 0, &SyncAssertThreadProc, &mbp, 0, &threadId );
    if( aThread != NULL ) {
        SetThreadPriority( aThread, THREAD_PRIORITY_HIGHEST );
        impl::SetThreadName( threadId, "Timer thread" );
        WaitForSingleObject( anEvent, INFINITE );
        CloseHandle( aThread );
    } else {
        DebugBreak();
    }
    CloseHandle( anEvent );
    return mbp.result;
  }

  //  static ConfiguredMembers< CONFIGURE_NAME_SOURCE( configAssert ) >::TrackedConfiguredValue< bool > DialogEnabled( L"assertDialogEnabled", true );
#ifdef TOOLS_RELEASE
  //  static ConfiguredMembers< CONFIGURE_NAME_SOURCE( configAssert ) >::TrackedConfiguredValue< bool > ReleaseAssertExit( L"releaseAssertExit", false );
#endif // TOOLS_RELEASE
}; // anonymous namespace

int tools::ToolsHandleAssertFailure( char const * /*assertion*/, char const * file, int line )
{
  // if( configAssert.get< bool >( L"crashOnAssert" ) ) {
  //   boost::wformat fmt( L"                                                                        \n"
  //                       L"Assertion failed!\n\n"
  //                       L"File: %s\n" 
  //                       L"Line: %d\n" 
  //                       L"Expression: %s\n\n" );
  //   fmt % Widen( file ) % line % Widen( assertion );
  //   tools::notification::Category aCat( tools::StaticStringId( L"Assert" ), tools::StringIdNull() );
  //   NOTIFY_ERROR( aCat ) << fmt.str() << std::endl;
  //   // now make us a bouncing baby call stack
  //   *(volatile char *)0;
  // }
#ifdef TOOLS_RELEASE
  // Within here we know that we came in from a PIT_ASSERTR or a fatal notification message
  // if( ReleaseAssertExit.get() ) {
  //   exit( -1 );  // We might want this to be abort, we might also want to log
  // }
  // otherwise crash
  *(volatile char *)0;
#endif // TOOLS_RELEASE
  if( AssertIsDisabled( file, line, false ) ) {
    return 1;
  }
  boost::format msg( "                                                                        \n"
                     "Assertion failed!\n\n"
                     "File: %s\n" 
                     "Line: %d\n" 
                     "Expression: %s\n\n" 
                     "Press Abort to kill the current object and continue\n"
                     "Press Retry to enter the debugger\n" 
                     "Press Ignore to ignore the Assert\n"
                     "Hold down CTRL with ignore to Disable the Assert\n" );
  msg % file % line % file;
  ///////////////////////////
  // Write out via MessageBox
  ///////////////////////////
  // if( DialogEnabled.get() ) {
    int nCode;
    {
      {
        nCode = static_cast< int >( SyncMessageBoxA( NULL, msg.str().c_str(), "Assertion Failed!",
                                 MB_ABORTRETRYIGNORE|MB_ICONHAND|MB_SETFOREGROUND|MB_TASKMODAL ));
      }
      // Retry: call the debugger
      if(nCode == IDRETRY) {
        DebugBreak();
        // return to user code
        return 0;
      }
      // Ignore: continue execution
      if( nCode == IDIGNORE ) {
        bool disable = ControlHeld();
        if( disable ) {
          AssertIsDisabled( file, line, true );
        }
        return disable;
      }
    }
    if( nCode == IDABORT ) {
      abort();
      return 0;
    }
  // } else {
  //   // TODO: maybe move this for performance
  //   tools::notification::Category aCat( tools::StaticStringId( L"Assert" ), tools::StringIdNull() );
  //   NOTIFY_ERROR( aCat ) << Widen( msg.str() ) << std::endl;
  //   abort();
  // }
  // should never get here
  abort();
  return 1;
}
