#include "../toolsprecompiled.h"

#include <tools/InterfaceTools.h>
#include <tools/Async.h>
#include <tools/Timing.h>

#include "../TimingImpl.h"

#include <boost/cast.hpp>

#include <sys/time.h>
#include <time.h>
#include <pthread.h>

using namespace tools;
using boost::numeric_cast;

////////
// Types
////////

struct PosixTimerThread
    : Notifiable< PosixTimerThread >
    , StandardDisposable< PosixTimerThread >
{
    PosixTimerThread( void );
    ~PosixTimerThread( void );

    // local methods
    void notifyWake( void );
    void entry( void );
    static void * threadEntry( void * );

    pthread_t thread_;
    pthread_cond_t waitCond_;
    pthread_mutex_t waitLock_;
    bool shutdown_;
    bool wake_;
    AutoDispose< TimerQueue > queue_;
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

    AutoDispose< PosixTimerThread > ptimer_;
    unsigned ncpu_;
    unsigned coreMax_;
};

//////////
// Statics
//////////

static RegisterEnvironment< Timing, TimingImpl > regTiming;

///////////////////////
// Non-member Functions
///////////////////////

uint64
tools::impl::getHighResTime()
{
    timespec localTimeOfDay;
    clock_gettime(CLOCK_MONOTONIC, &localTimeOfDay);
    return numeric_cast< uint64 >( localTimeOfDay.tv_sec ) * TOOLS_NANOSECONDS_PER_SECOND +
        numeric_cast< uint64 >( localTimeOfDay.tv_nsec );
}

static void
nsecToTimespec(
    uint64 nsec,
    timespec & spec )
{
    spec.tv_sec = nsec / TOOLS_NANOSECONDS_PER_SECOND;
    spec.tv_nsec = nsec - ( spec.tv_sec * TOOLS_NANOSECONDS_PER_SECOND );
    TOOLS_ASSERT( spec.tv_nsec >= 0 );
}

///////////////////
// PosixTimerThread
///////////////////

PosixTimerThread::PosixTimerThread( void )
    : shutdown_( false )
    , wake_( false )
    , queue_( impl::timerQueueNew( toThunk< &PosixTimerThread::notifyWake >() ))
{
    pthread_mutex_init( &waitLock_, nullptr );
    pthread_condattr_t attr;
    pthread_condattr_init( &attr );
    auto ret = pthread_condattr_setclock( &attr, CLOCK_MONOTONIC );
    TOOLS_ASSERTR(ret == 0);
    pthread_cond_init( &waitCond_, &attr );
    pthread_condattr_destroy( &attr );
    pthread_create( &thread_, nullptr, &PosixTimerThread::threadEntry, this );
}

PosixTimerThread::~PosixTimerThread( void )
{
    pthread_mutex_lock( &waitLock_ );
        shutdown_ = true;
        wake_ = true;
    pthread_mutex_unlock( &waitLock_ );
    // Wake up the thread
    pthread_cond_signal( &waitCond_ );
    void * status;
    pthread_join( thread_, &status );
    TOOLS_ASSERT( status == nullptr );
}

void
PosixTimerThread::notifyWake( void )
{
    // This should only be called for the first sleeper after we wake.
    pthread_mutex_lock( &waitLock_ );
        bool doWake = !wake_;
        if( doWake ) {
            wake_ = true;
        }
    pthread_mutex_unlock( &waitLock_ );
    if( doWake ) {
        pthread_cond_signal( &waitCond_ );
    }
}

void
PosixTimerThread::entry( void )
{
    while( !shutdown_ ) {
        uint64 nextTime;
        queue_->eval( &nextTime );
        pthread_mutex_lock( &waitLock_ );
            if( !shutdown_ ) {
                if( !wake_ ) {
                    timespec duration;
                    nsecToTimespec( nextTime, duration );
                    pthread_cond_timedwait( &waitCond_, &waitLock_, &duration );
                }
                wake_ = false;
            }
        pthread_mutex_unlock( &waitLock_ );
    }
}

void *PosixTimerThread::threadEntry(
    void * self )
{
    // The timer thread runs at maximum priority, the resulting operations should be time limited.
    sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if( pthread_setschedparam( pthread_self(), SCHED_FIFO, &param )) {
        // TODO: log this error
    }
    static_cast< PosixTimerThread * >( self )->entry();
    return nullptr;
}

/////////////
// TimingImpl
/////////////

TimingImpl::TimingImpl(
    Environment & )
{
}

AutoDispose< Request >
TimingImpl::serviceStart( void )
{
    ptimer_ = new PosixTimerThread;
    return static_cast< Request * >( nullptr );
}

AutoDispose< Request >
TimingImpl::serviceStop( void )
{
    ptimer_.reset();
    return static_cast< Request * >( nullptr );
}

uint64
TimingImpl::mark( void )
{
    return tools::impl::getHighResTime();
}

uint64
TimingImpl::mark(
    uint64 startMark )
{
    return tools::impl::getHighResTime() - startMark;
}

AutoDispose< Request >
TimingImpl::timer(
    uint64 duration,
    uint64 * startTime )
{
    return std::move(ptimer_->queue_->timer( duration, startTime ));
}
