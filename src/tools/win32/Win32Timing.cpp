#include "toolsprecompiled.h"

#include <tools/InterfaceTools.h>
#include <tools/Async.h>
#include <tools/Timing.h>

#include "../TimingImpl.h"
#include "Win32Tools.h"

#ifdef WINDOWS_PLATFORM
#  pragma warning( disable : 4371 )
#endif // WINDOWS_PLATFORM
#include <boost/numeric/conversion/cast.hpp>
#ifdef WINDOWS_PLATFORM
#  pragma warning( default : 4371 )
#endif // WINDOWS_PLATFORM

using namespace tools;
using boost::numeric_cast;

////////
// Types
////////

struct WinTimerThread
    : Notifiable< WinTimerThread >
    , StandardDisposable< WinTimerThread >
{
    WinTimerThread( void );
    ~WinTimerThread( void );

    void wake( void );
    void entry( void );
    static DWORD WINAPI threadEntry( LPVOID );

    HANDLE                      thread_;
    HANDLE                      events_[2];
    unsigned volatile           shutdown_;
    AutoDispose< TimerQueue >   queue_;
};

struct TimingImpl
    : Timing
    , detail::StandardNoBindService< TimingImpl, boost::mpl::list< Timing >::type >
{
    TimingImpl( Environment & );

    // Service
    AutoDispose< Request > serviceStart( void );
    AutoDispose< Request > serviceStop( void );

    // Timing
    uint64 mark( void );
    uint64 mark( uint64 );
    AutoDispose< Request > timer( uint64, uint64 * );

    double                          timerFreq_;  // in nanoseconds
    AutoDispose< WinTimerThread >   timerThread_;
};

//////////
// Statics
//////////

static RegisterEnvironment< Timing, TimingImpl > regTiming;

///////////////////////
// Non-member Functions
///////////////////////

uint64
tools::impl::getHighResTime( void )
{
    LARGE_INTEGER i, j;
    QueryPerformanceCounter( &i );
    QueryPerformanceFrequency( &j );
    return numeric_cast< uint64 >( i.QuadPart * ( 1000000000LL / j.QuadPart ));
}

/////////////////
// WinTimerThread
/////////////////

WinTimerThread::WinTimerThread( void )
    : shutdown_( 0 )
{
    events_[0] = CreateEvent( nullptr, FALSE, FALSE, nullptr );
    events_[1] = CreateWaitableTimer( nullptr, TRUE, nullptr );
    queue_ = std::move( tools::impl::timerQueueNew( toThunk< &WinTimerThread::wake >() ));
    DWORD i;
    thread_ = CreateThread( nullptr, 0, &WinTimerThread::threadEntry, this, 0, &i );
    SetThreadPriority( thread_, THREAD_PRIORITY_HIGHEST );
    impl::SetThreadName( i, "timerThread" );
}

WinTimerThread::~WinTimerThread( void )
{
    atomicExchange( &shutdown_, 1U );
    SetEvent( events_[0] );
    WaitForSingleObject( thread_, INFINITE );
    CloseHandle( events_[0] );
    CloseHandle( events_[1] );
    CloseHandle( thread_ );
}

void
WinTimerThread::wake( void )
{
    SetEvent( events_[0] );
}

void
WinTimerThread::entry( void )
{
    while( !shutdown_ ) {
        // guarenteed to be positive
        uint64 delta = queue_->eval();
        LARGE_INTEGER delay;
        delay.QuadPart = -numeric_cast< sint64 >( delta ) / 100LL;
        BOOL ret = SetWaitableTimer( events_[1], &delay, 0, nullptr, nullptr, FALSE );
        if( !ret ) {
            // send cancel to all threads and abort
            //AutoDipose< Error::Reference > err(xxxx);
            break;
        }
        DWORD next = WaitForMultipleObjects( 2, events_, FALSE, INFINITE );
        if( next != ( WAIT_OBJECT_0 + 0) && next != (WAIT_OBJECT_0 + 1)) {
            //AutoDispose< Error::Reference > err(xxx);
            break;
        }
    }
}

DWORD WINAPI
WinTimerThread::threadEntry(
    LPVOID  param )
{
    static_cast< WinTimerThread * >( param )->entry();
    return 0U;
}

/////////////
// TimingImpl
/////////////

TimingImpl::TimingImpl(
    Environment & )
{
    LARGE_INTEGER i;
    BOOL res = QueryPerformanceFrequency( &i );
    TOOLS_ASSERT( res );
    timerFreq_ = 1000000000. / numeric_cast< double >( i.QuadPart );
}

AutoDispose< Request >
TimingImpl::serviceStart( void )
{
    timerThread_ = new WinTimerThread;
    return static_cast< Request * >( nullptr );
}

AutoDispose< Request >
TimingImpl::serviceStop( void )
{
    timerThread_.reset();
    return static_cast< Request * >( nullptr );
}

uint64
TimingImpl::mark( void )
{
    LARGE_INTEGER   i;
    QueryPerformanceCounter( &i );
    return numeric_cast< uint64 >( i.QuadPart );
}

uint64
TimingImpl::mark(
    uint64  begin )
{
    LARGE_INTEGER   i;
    QueryPerformanceCounter( &i );
    return numeric_cast< uint64 >( numeric_cast< double >( i.QuadPart -
        numeric_cast< sint64 >( begin )) * timerFreq_ );
}

AutoDispose< Request >
TimingImpl::timer(
    uint64      duration,
    uint64 *    startTime )
{
    return std::move( timerThread_->queue_->timer( duration, startTime ));
}
