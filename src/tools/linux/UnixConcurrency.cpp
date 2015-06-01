#include "../toolsprecompiled.h"

#include <tools/Concurrency.h>
#include <tools/Environment.h>
#include <tools/AsyncTools.h>
#include <tools/Threading.h>
#include <tools/Tools.h>

#include "TimingImpl.h"

#include <pthread.h>

#include <algorithm>
#include <bitset>
#include <list>
#include <memory>
#include <set>
#include <unordered_map>

#include <numa.h>
#include <numaif.h>

#include <linux/futex.h>

using namespace tools;

typedef void * ( *EntryPointT )( void * );

namespace {
    static pthread_key_t destructKey;
    static unsigned volatile totalNumStacks = 0;
    static size_t volatile totalStackBytes = 0;
    static __thread void * stackTopAddr;
};  // anonymous namespace

namespace tools {
    namespace impl {
        void threadLocalThreadEnd( void );
        ConditionVarLock ** conditionVarPlatformLockRef( ConditionVar * );
        AutoDispose< Monitor > monitorConditionVarNew( ConditionVar * );
    };
};  // tools namespace

////////
// Types
////////

namespace {
    struct PthreadMonitorUnlock
        : Disposable
    {
        void dispose( void );

        pthread_mutex_t mutable lock_;
    };

    struct PthreadMutexMonitor
        : StandardDisposable< PthreadMutexMonitor, Monitor, AllocStatic< Platform > >
        , PthreadMonitorUnlock
    {
        PthreadMutexMonitor( void );
        ~PthreadMutexMonitor( void );

        // Monitor
        AutoDispose<> enter( impl::ResourceSample const &, bool );
    };

    struct PthreadCvar
        : StandardDisposable< PthreadCvar, ConditionVar, AllocStatic< Platform > >
    {
        PthreadCvar( void );
        ~PthreadCvar( void );

        // ConditionVar
        AutoDispose< Monitor > monitorNew( impl::ResourceSample const & );
        void wait( void );
        void signal( bool );

        pthread_cond_t cvar_;
    };

    struct ThreadingImpl
        : Threading
        , detail::StandardNoBindService< ThreadingImpl, boost::mpl::list< Threading >::type >
    {
        ThreadingImpl( Environment & );

        // Threading
        AutoDispose< Thread > fork( StringId const &, Thunk const & );
        AutoDispose< Request > forkAll( StringId const &, Thunk const & );

        unsigned numCores_;
        unsigned maxCores_;
    };

    struct UnixEvent
        : StandardDisposable< UnixEvent, Event >
    {
        UnixEvent( void );
        ~UnixEvent( void );

        void post( void );
        void wait( void );

        pthread_cond_t cond_;
        pthread_mutex_t lock_;
        bool posted_;
    };

    struct UnixPosixThread
        : StandardDisposable< UnixPosixThread, Thread >
    {
        UnixPosixThread( StringId const &, Thunk const &, ThreadingImpl & );
        ~UnixPosixThread( void );

        // Thread
        void waitSync( void );
        AutoDispose< Request > wait( void );
        static void * entry( void * );

        Thunk thunk_;
        StringId name_;
        ThreadingImpl & parent_;
        AutoDispose< UnixEvent > event_;
        unsigned synced_;
        pthread_t thread_;
    };

    struct UnixPosixThreadAll
        : StandardManualRequest< UnixPosixThreadAll, AllocTail< pthread_t >>
    {
        UnixPosixThreadAll( StringId const &, Thunk const &, ThreadingImpl & );
        UnixPosixThreadAll( UnixPosixThreadAll const & ) = delete;
        ~UnixPosixThreadAll( void );

        UnixPosixThreadAll & operator=( UnixPosixThreadAll const & ) = delete;

        // Thread
        void start( void );

        // local methodsa
        static void * entry( void * );

        Thunk thunk_;
        StringId name_;
        ThreadingImpl & parent_;
        unsigned volatile numRunning_;
        unsigned numThreads_;
        pthread_t threads_[];
    };

    struct PosixThreadWrapperData
        : AllocStatic<>
    {
        PosixThreadWrapperData( EntryPointT, void *, StringId const &, StringId const &, size_t, std::bitset< 64 > );
        ~PosixThreadWrapperData( void );

        EntryPointT entry_;
        void * param_;
        StringId name_;  // user-friendly name for logging
        StringId envRole_;
        size_t stackSize_;
        std::bitset< 64 > allowedCpus_;
    };

    struct UnixThreadSleepVariable
        : StandardDisposable< UnixThreadSleepVariable, impl::ThreadSleepVariable, AllocStatic< Platform >>
    {
        UnixThreadSleepVariable( void );

        // ThreadSleepVariable
        void wakeOne( void );
        void wakeAll( bool );
        void sleep( uint64 );

        int seq_;
    };

    struct UnixHungThreadDetector
        : StandardDisposable< UnixHungThreadDetector, impl::HungThreadDetector >
    {
        UnixHungThreadDetector( StringId const &, uint64, uint64, uint64 );
        ~UnixHungThreadDetector( void );

        // HungThreadDetector
        void arm( void );
        void disarm( void );
        bool enabled( void );
        void noteExecBegin( uint64 );
        void noteExecFinish( void );
        void timerFire( uint64 );

        // local methods
        static inline uint64 timevalToUsec( timeval const & tv ) {
            return ( tv.tv_sec * TOOLS_MICROSECONDS_PER_SECOND ) + tv.tv_usec;
        }

        StringId name_;
        timer_t timerId_;
        bool constructed_;
        uint64 lastExecStartTime_;  // If 0, not current in exec
        rusage usageStart_;
        uint64 complainDuration_;
        uint64 assertDuration_;
        uint64 checkPeriod_;
        pid_t expectedTid_;
        uint64 expectedThreadId_;
    };
};  // anonymous namespace

//////////
// Statics
//////////

static RegisterEnvironment< Threading, ThreadingImpl > regThreading;

///////////////////////
// Non-member functions
///////////////////////

static void
pooiSchedulerKeyDestruct(
    void * key )
{
    impl::threadLocalThreadEnd();
}

static void
registerPthreadDestructor( void )
{
    int ret = pthread_key_create( &destructKey, pooiSchedulerKeyDestruct );
    TOOLS_ASSERT( ret == 0 );
}

namespace tools {
    namespace impl {
        AutoDispose< ThreadSleepVariable >
        threadSleepVariableNew( void )
        {
            return new UnixThreadSleepVariable;
        }

        AutoDispose< Monitor >
        monitorPlatformNew( void )
        {
            return new PthreadMutexMonitor;
        }

        AutoDispose< ConditionVar >
        conditionVarPlatformNew( void )
        {
            return new PthreadCvar;
        }

        void
        threadLocalBegin( void )
        {
            static pthread_once_t destructKeyOnce_ = PTHREAD_ONCE_INIT;
            pthread_once( &destructKeyOnce_, registerPthreadDestructor );
            // Touch the destruct key so weget destructed.
            pthread_setspecific( destructKey, reinterpret_cast< void * >( 0x743ADU ));
        }

        void *
        threadLocalAlloc( void )
        {
            // Make sure the destruct key is registered first.
            threadLocalBegin();
            pthread_key_t key;
            int ret = pthread_key_create( &key, nullptr );
            TOOLS_ASSERT( ret == 0 );
            return reinterpret_cast< void * >( static_cast< size_t >( key ));
        }

        void *
        threadLocalGet(
            void * key )
        {
            return pthread_getspecific( static_cast< pthread_key_t >(
                reinterpret_cast< size_t >( key )));
        }

        void
        threadLocalSet(
            void * key,
            void * value )
        {
            // Make sure the destruct key is registered first
            threadLocalBegin();
            pthread_setspecific( static_cast< pthread_key_t >( reinterpret_cast< size_t >( key )),
                value );
        }

        void
        threadLocalFree(
            void * key )
        {
            pthread_key_delete( static_cast< pthread_key_t >( reinterpret_cast< size_t >( key )));
        }

        uint64
        threadId( void )
        {
            return pthread_self();
        }

        uint32
        cpuNumber( void )
        {
            // Get the current CPU/hyperthread ID. This is racy by nature as we could get migrated
            // without warning. However, if your use is not sensitive to this (e.g.: keeping per-CPU
            // performance counters), then it is still handy.
            return static_cast< uint32 >( sched_getcpu() );
        }

        unsigned
        platformStackCount( void )
        {
            return atomicRead( &totalNumStacks );
        }

        size_t
        platformStackBytes( void )
        {
            return atomicRead( &totalStackBytes );
        }

        void
        logUntrackedMemory( void )
        {
            // TODO: implement this
        }

        void
        platformMallocStats( void )
        {
            // TODO: implement this
            // malloc_stats()?
        }

        AutoDispose< HungThreadDetector >
        platformHungThreadDetectorNew(
            StringId const & name,
            uint64 complainMsec,
            uint64 assertMsec,
            uint64 checkMsec )
        {
            return new UnixHungThreadDetector( name, complainMsec, assertMsec, checkMsec );
        }
    };  // impl namespace

    AutoDispose< Event >
    eventNew( impl::ResourceSample const & )
    {
        return new UnixEvent();
    }
};  // tools namespace

namespace {
    void
    assertIfRoot(
        char const * msg )
    {
        int err = errno;  // save this before calling geteuid() which might reset it
        char const * errStr = strerror( err );
        if( static_cast< uint16 >( geteuid() ) == 0 ) {
            // Should not have failed if we are root
            // convert this to logging
            fprintf( stderr, msg, errStr );
            abort();
        } else {
            // convert this to logging
            fprintf( stdout, msg, errStr );
        }
    }

    std::bitset< 64 >
    getCurrentThreadAffinity( void )
    {
        std::bitset< 64 > ret;  // Just think, someday I'll have to think about more than 64 CPUs!
        cpu_set_t cpuset;
        if( pthread_getaffinity_np( impl::threadId(), sizeof( cpuset ), &cpuset ) != 0 ) {
            // convert this to logging
            fprintf( stderr, "Could not query thread affinity\n" );
        }
        unsigned long numCpus = sysconf( _SC_NPROCESSORS_CONF );
        for( unsigned n=0; n<numCpus; ++n ) {
            if( CPU_ISSET( n, &cpuset )) {
                ret[ n ] = 1;
            }
        }
        TOOLS_ASSERT( ret.count() > 0 );  // 0 means there is no where we can run, that would be bad
        return ret;
    }

    void *
    posixThreadWrapper(
        void * wrapperParam )
    {
        std::unique_ptr< PosixThreadWrapperData > data( static_cast< PosixThreadWrapperData * >( wrapperParam ));
        int policy;
        struct sched_param param;
        pthread_attr_t attr;
        // Find our exact stack bounds
        void * stackBaseAddr;
        size_t stackSize;
        if( pthread_getattr_np( pthread_self(), &attr ) || pthread_attr_getstack( &attr, &stackBaseAddr, &stackSize )) {
            // convert this to logging
            fprintf( stderr, "Could not query thread stack address: %d\n", errno );
        }
        TOOLS_ASSERT( ( reinterpret_cast< uintptr_t >( stackBaseAddr ) & 0xFFF ) == 0 );
        stackTopAddr = reinterpret_cast< uint8 * >( stackBaseAddr ) + stackSize;
        if( pthread_getschedparam( impl::threadId(), &policy, &param ) != 0 ) {
            // convert this to logging
            fprintf( stderr, "Could not query thread scheduling parameters\n" );
        }
        pid_t tid = syscall( SYS_gettid );
        std::bitset< 64 > allowable = getCurrentThreadAffinity();
        TOOLS_ASSERT( allowable.count() > 0 );
        // Install thread annotations.
        annotateThread( data->envRole_ );
        // convert this to logging (tid, allowedCpus, policy, param.sched_priority, data->envRole_, data->stackSize_, data->name_
        fprintf( stderr, "new thread: TID %d\n", tid );
        // If this thread is bound to CPU(s) that all fall on the same NUMA node, and there is more than
        // one, then bind this stack memory onto that node.
        std::set< unsigned, std::less< unsigned >, AllocatorAffinity< unsigned >> numaNodes;
        for( unsigned i=0; i<64; ++i ) {
            if( data->allowedCpus_[ i ]) {
                numaNodes.insert( numa_node_of_cpu( i ));
            }
        }
        TOOLS_ASSERT( numaNodes.size() > 0 );
        if( ( numaNodes.size() == 1 ) && ( numa_num_configured_nodes() > 1 )) {
            unsigned aNode = *numaNodes.begin();
            // Unfortunately none of the numa_* functions (see numa(3)) has semantics equivalent to MPOL_MF_MOVE.
            unsigned long nodeMask = ( 1ULL << aNode );
            int result = mbind( stackBaseAddr, stackSize, MPOL_PREFERRED, &nodeMask, sizeof( nodeMask ) * 8, MPOL_MF_MOVE );
            if( result != 0 ) {
                // convert this to logging
                fprintf( stderr, "mbind on stack failed, errno %d\n", errno );
            } else {
                // convert this to logging
                fprintf( stdout, "bound stack (%lx to %lx) to NUMA node %d\n", stackBaseAddr, static_cast< uint8 * >( stackBaseAddr ) + stackSize, aNode );
            }
        } else {
            // convert this to logging
            fprintf( stdout, "Not trying to bind stack to numa node\n" );
        }
        return data->entry_( data->param_ );
    }

    // Helper function for creating new posix threads with a given entry, CPU binding (optional), and
    // scheduling policy. This is preferred to calling pthread_create() directly as this consolidates
    // all of the setup details.
    void
    posixThreadCreate(
        pthread_t volatile * pthread,
        EntryPointT entry,
        void * param,
        StringId const & name,  // user-friendly thread name for logging
        StringId const & envRole,
        std::bitset< 64 > allowedCpus,  // not allowed to be empty
        ThreadScheduler::SchedulingPolicy policy,
        uint64 maxStackSize )
    {
        pthread_attr_t attr;
        pthread_attr_init( &attr );
        TOOLS_ASSERT( !allowedCpus.none() );
        unsigned numCpus = sysconf( _SC_NPROCESSORS_ONLN );
        // Set CPU affinity for the new thread
        cpu_set_t cpuset;
        CPU_ZERO( &cpuset );
        for( unsigned i=0; i<numCpus; ++i ) {
            if( allowedCpus[ i ] ) {
                CPU_SET( i, &cpuset );
            }
        }
        if( pthread_attr_setaffinity_np( &attr, sizeof( cpuset ), &cpuset ) != 0 ) {
            assertIfRoot( "pthread_attr_setaffinity_np failed: %s" );
        }
        // Set scheduling policy and priority for the new thread
        if( pthread_attr_setinheritsched( &attr, PTHREAD_EXPLICIT_SCHED ) != 0 ) {
            assertIfRoot( "pthread_attr_setinheritsched failed: %s" );
        }
        if( ( static_cast< uint16 >( geteuid() ) != 0 ) && ( policy != ThreadScheduler::ThreadPolicyNormal )) {
            // Can only create realtime threads if root
            // Change this to logging
            fprintf( stderr, "posixThreadCreate: not privileged, changing scheduling policy to non-realtime\n" );
            policy = ThreadScheduler::ThreadPolicyNormal;
        }
        int schedPolicy;
        int schedPrio;
        switch( policy ) {
        case ThreadScheduler::ThreadPolicyNormal:
            schedPolicy = SCHED_OTHER;
            schedPrio = 0;
            break;
        case ThreadScheduler::ThreadPolicyRealtimeLow:
            schedPolicy = SCHED_FIFO;
            schedPrio = sched_get_priority_min( SCHED_FIFO );
            break;
        case ThreadScheduler::ThreadPolicyRealtimeMedium:
            schedPolicy = SCHED_FIFO;
            schedPrio = ( sched_get_priority_min( SCHED_FIFO ) + sched_get_priority_max( SCHED_FIFO )) / 2;
            break;
        case ThreadScheduler::ThreadPolicyRealtimeHigh:
            schedPolicy = SCHED_FIFO;
            schedPrio = sched_get_priority_max( SCHED_FIFO );
            break;
        default:
            TOOLS_ASSERT( !"Unknown scheduling policy" );
            abort();
        }
        if( pthread_attr_setschedpolicy( &attr, schedPolicy ) != 0 ) {
            assertIfRoot( "pthread_attr_setschedpolicy failed: %s" );
        }
        sched_param schedParam;
        schedParam.sched_priority = schedPrio;
        if( pthread_attr_setschedparam( &attr, &schedParam ) != 0 ) {
            assertIfRoot( "pthread_attr_setschedparam failed: %s" );
        }
        size_t prevStackSize = 0;
        if( pthread_attr_getstacksize( &attr, &prevStackSize ) != 0 ) {
            assertIfRoot( "pthread_attr_getstacksize failed: %s" );
        }
        // At this point prevStackSize is likely 8 MB
        size_t newStackSize = std::min< size_t >( prevStackSize, maxStackSize );
        if( newStackSize != prevStackSize ) {
            if( pthread_attr_setstacksize( &attr, newStackSize ) != 0 ) {
                assertIfRoot( "pthread_attr_setstacksize failed: %s" );
            }
        }
        // Create that thread now
        PosixThreadWrapperData * data = new PosixTreadWrapperData( entry, param, name, envRole, newStackSize, allowedCpus );
        int err = pthread_create( const_cast< pthread_t * >( pthread ), &attr, posixThreadWrapper, data );
        if( err != 0 ) {
            // log this
            fprintf( stderr, "Unable to create thread: %s\n", strerror( err ));
        }
        pthread_attr_destroy( &attr );
    }

    int
    sysFutex(
        int * uaddr,
        int op,
        int val,
        struct timespec const * timeout,
        int * uaddr2,
        int val2 )
    {
        return syscall( SYS_futex, uaddr, op, val, timeout, uaddr2, val2 )
    }
};  // anonymous namespace

///////////////////////
// PthreadMonitorUnlock
///////////////////////

void
PthreadMonitorUnlock::dispose( void )
{
    pthread_mutex_unlock( &lock_ );
}

//////////////////////
// PthreadMutexMonitor
//////////////////////

PthreadMutexMonitor::PthreadMutexMonitor( void )
{
    pthread_mutex_init( &lock_, nullptr );
}

PthreadMutexMonitor::~PthreadMutexMonitor( void )
{
    pthread_mutex_destroy( &lock_ );
}

AutoDispose<>
PthreadMutexMonitor::enter(
    impl::ResourceSample const & sample,
    bool tryOnly )
{
    if( tryOnly ) {
        return ( pthread_mutex_trylock( &lock_ ) == 0 ) ?
            static_cast< PthreadMonitorUnlock * >( this ) : nullptr;
    }
    pthread_mutex_lock( &lock_ );
    return static_cast< PthreadMonitorUnlock * >( this );
}

//////////////
// PthreadCvar
//////////////

PthreadCvar::PthreadCvar( void )
{
    pthread_cond_init( &cvar_, nullptr );
}

PthreadCvar::~PthreadCvar( void )
{
    pthread_cond_destroy( &cvar_ );
}

AutoDispose< Monitor >
PthreadCvar::monitorNew(
    impl::ResourceSample const & )
{
    return impl::monitorConditionVarNew( this );
}

void
PthreadCvar::wait( void )
{
    impl::ConditionVarLock ** cvarMonitor = impl::conditionVarPlatformLockRef( this );
    impl::ConditionVarLock * lockVal = *cvarMonitor;
    Disposable * lock = lockVal->lock_;
    TOOLS_ASSERT( !!lockVal );
    TOOLS_ASSERT( !!lock );
    *cvarMonitor = nullptr;
    lockVal->lock_ = nullptr;
    pthread_cond_wait( &cvar_, &static_cast< PthreadMutexMonitor * >( lockVal->monitor_ )->lock_ );
    TOOLS_ASSERT( !lockVal->lock_ );
    lockVal->lock_ = lock;
    TOOLS_ASSERT( !*cvarMonitor );
    *cvarMonitor = lockVal;
}

void
PthreadCvar::signal(
    bool all )
{
    if( all ) {
        pthread_cond_broadcast( &cvar_ );
    } else {
        pthread_cond_signal( &cvar_ );
    }
}

////////////////
// ThreadingImpl
////////////////

ThreadingImpl::ThreadingImpl(
    Environment & )
{
    numCores_ = maxCores_ = sysconf( _SC_NPROCESSORS_ONLN );
}

AutoDispose< Thread >
ThreadingImpl::fork(
    StringId const & name,
    Thunk const & thunk )
{
    return new UnixPosixThread( name, thunk, *this );
}

AutoDispose< Request >
ThreadingImpl::forkAll(
    StringId const & name,
    Thunk const & thunk )
{
    return new(numCores_) UnixPosixThreadAll( name, thunk, *this );
}

////////////
// UnixEvent
////////////

UnixEvent::UnixEvent( void )
    : posted_( false )
{
    pthread_mutex_init( &lock_, nullptr );
    pthread_condattr_t attr;
    pthread_condattr_init( &attr );
    auto ret = pthread_condattr_setclock( &attr, CLOCK_MONOTONIC );
    TOOLS_ASSERTR( ret == 0 );
    pthread_cond_init( &cond_, &attr );
    pthread_condattr_destroy( &attr );
}

UnixEvent::~UnixEvent( void )
{
    pthread_mutex_lock( &lock_ );
    pthread_mutex_unlock( &lock_ );
    pthread_cond_destroy( &cond_ );
    pthread_mutex_destroy( &lock_ );
}

void
UnixEvent::post( void )
{
    {
        pthread_mutex_lock( &lock_ );
	posted_ = true;
	pthread_mutex_unlock( &lock_ );
    }
    pthread_cond_signal( &cond_ );
}

void
UnixEvent::wait( void )
{
    if( posted_ ) {
        return;
    }
    pthread_mutex_lock( &lock_ );
    if( posted_ ) {
        pthread_mutex_unlock( &lock_ );
        return;
    }
    pthread_cond_wait( &cond_, &lock_ );
    pthread_mutex_unlock( &lock_ );
}

//////////////////
// UnixPosixThread
//////////////////

UnixPosixThread::UnixPosixThread(
    StringId const & name,
    Thunk const & thunk,
    ThreadingImpl & parent )
    : thunk_( thunk )
    , name_( name )
    , parent_( parent )
    , event_( new UnixEvent() )
    , synced_( 0 )
{
    int res = pthread_create( &thread_, nullptr, &UnixPosixThread::entry, this );
    //TOOLS_ASSERT( res == 0 && !!thread_ );
    event_->post();
}

UnixPosixThread::~UnixPosixThread( void )
{
    TOOLS_ASSERT( !!synced_ );
}

void
UnixPosixThread::waitSync( void )
{
    int res = pthread_join( thread_, nullptr );
    TOOLS_ASSERT( !res );
}

AutoDispose< Request >
UnixPosixThread::wait( void )
{
    // TODO: figure out how to do this better
    waitSync();
    return nullptr;
}

void *
UnixPosixThread::entry(
    void * context )
{
    UnixPosixThread * self = static_cast< UnixPosixThread * >( context );
    self->event_->wait();
    atomicIncrement( &self->synced_ );
    self->thunk_();
    return nullptr;
}

/////////////////////
// UnixPosixThreadAll
/////////////////////

UnixPosixThreadAll::UnixPosixThreadAll(
    StringId const & name,
    Thunk const & thunk,
    ThreadingImpl & parent )
    : thunk_( thunk )
    , name_( name )
    , parent_( parent )
    , numRunning_( 0 )
    , numThreads_( parent.numCores_ )
{
    std::fill( threads_, threads_ + numThreads_, static_cast< pthread_t >( -1 ));
}

UnixPosixThreadAll::~UnixPosixThreadAll( void )
{
    std::for_each( threads_, threads_ + numThreads_, [&]( pthread_t & t )->void {
	if( t != static_cast< pthread_t >( -1 )) {
	    int e = pthread_join( t, nullptr );
	    TOOLS_ASSERT( !e );
	}
    });
}

void
UnixPosixThreadAll::start( void )
{
    numRunning_ = numThreads_;
    std::for_each( threads_, threads_ + numThreads_, [&]( pthread_t & t )->void {
	int e = pthread_create( &t, nullptr, &UnixPosixThreadAll::entry, this );
	TOOLS_ASSERT( !e );
    });
}

void *
UnixPosixThreadAll::entry(
    void * context )
{
    UnixPosixThreadAll * self = static_cast< UnixPosixThreadAll * >( context );
    self->thunk_();
    if( !atomicDecrement( &self->numRunning_ )) {
        self->finish();
    }
    return nullptr;
}

/////////////////////////
// PosixThreadWrapperData
/////////////////////////

PosixThreadWrapperData::PosixThreadWrapperData(
    EntryPointT entry,
    void * param,
    StringId const & name,
    StringId const & envRole,
    size_t stackSize,
    std::bitset< 64 > allowedCpus )
    : entry_( entry )
    , param_( param )
    , name_( name )
    , envRole_( envRole )
    , stackSize_( stackSize )
    , allowedCpus_( allowedCpus )
{
    atomicIncrement( &totalNumStacks );
    atomicAdd( &totalStackBytes, stackSize_ );
}

PosixThreadWrapperData::~PosixThreadWrapperData( void )
{
    atomicDecrement( &totalNumStacks );
    atomicSubtract( &totalStackBytes, stackSize_ );
}

//////////////////////////
// UnixThreadSleepVariable
//////////////////////////

UnixThreadSleepVariable::UnixThreadSleepVariable( void )
    : seq_( 0 )
{
}

void
UnixThreadSleepVariable::wakeOne( void )
{
    auto start = impl::getHighResTime();
    atomicAdd( &seq_, 2 );
    sysFutex( &seq_, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0 );
    auto now = impl::getHighResTime();
    if( ( now - start ) > (100U * TOOLS_NANOSECONDS_PER_MILLISECOND )) {
        // convert this to logging
        fprintf( stdout, "wakeOne tool %lld ms %s\n",
            ( now - start ) / TOOLS_NANOSECONDS_PER_MILLISECOND,
            "{thread type}" );
    }
}

void
UnixThreadSleepVariable::wakeAll( bool stopping )
{
    // Ensure that all threads get woken, and none ever sleep again.
    if( stopping ) {
        atomicOr( &seq_, 1 );
    } else {
        atomicAdd( &seq_, 2 );
    }
    sysFutex( &seq_, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0 );
}

void
UnixThreadSleepVariable::sleep( uint64 timeout )
{
    struct timespec spec;
    spec.tv_sec = timeout / TOOLS_NANOSECONDS_PER_SECOND;
    spec.tv_nsec = timeout % TOOLS_NANOSECONDS_PER_SECOND;
    sysFutex( &seq_, FUTEX_WAIT_PRIVATE, atomicRead( &seq_ ) & ~1, &spec, nullptr, 0 );
}

/////////////////////////
// UnixHungThreadDetector
/////////////////////////

UnixHungThreadDetector::UnixHungThreadDetector(
    StringId const & name,
    uint64 complainMsec,
    uint64 assertMsec,
    uint64 checkMsec )
    : name_( name )
    , lastExecStartTime_( 0 )
    , complainDuration_( complainMsec * TOOLS_NANOSECONDS_PER_MILLISECOND )
    , assertDuration_( assertMsec * TOOLS_NANOSECONDS_PER_MILLISECOND )
    , checkPeriod_( checkMsec * TOOLS_NANOSECONDS_PER_MILLISECOND )
    , expectedTid_( syscall( SYS_gettid ))
    , expectedThreadId_( impl::threadId() )
{
    sigevent evp;
    evp.sigev_notify = SIGEV_THREAD_ID;  // signal targeted sigev_notify_thread_id
    evp.sigev_signo = SIGALRM;
    evp.sigev_value.sival_ptr = this;
    evp._sigev_un._tid = expectedTid_;  // sigev_notify_thread_id?
    // Possible values for the first argument also include pthread_getcpuclockid().
    constructed_ = ( timer_create( CLOCK_REALTIME, &evp, &timerId_ ) == 0 );
    if( !construct_ ) {
        // TODO: convert this to logging
        fprintf( stderr, "Could not create thread timer, errno %d\n", errno );
    }
}

UnixHungThreadDetector::~UnixHungThreadDetector( void )
{
    if( constructed_ ) {
        // This handles disarming if needed
        timer_delete( timerId_ );
    }
}

void
UnixHungThreadDetector::arm( void )
{
    if( !constructed_ ) {
        return;
    }
    if( checkPeriod_ == 0 ) {
        return;
    }
    itimerspec newValue;
    newValue.it_value.tv_sec = checkPeriod_ / TOOLS_NANOSECONDS_PER_SECOND;
    newValue.it_value.tv_nsec = checkPeriod_ % TOOLS_NANOSECONDS_PER_SECOND;
    newValue.it_interval.tv_sec = checkPeriod_ / TOOLS_NANOSECONDS_PER_SECOND;
    newValue.it_interval.tv_nsec = checkPeriod_ % TOOLS_NANOSECONDS_PER_SECOND;
    if( timer_settime( timerId_, 0, &newValue, nullptr ) != 0 ) {
        // TODO: convert this to logging
        fprintf( stderr, "HungThreadDetector: arm timer_settime failed, errno %d\n", errno );
    }
}

void
UnixHungThreadDetector::disarm( void )
{
    if( !constructed_ ) {
        return;
    }
    itimerspec newValue;
    newValue.it_value.tv_sec = 0;
    newValue.it_value.tv_nsec = 0;
    newValue.it_interval.tv_sec = 0;
    newValue.it_interval.tv_nsec = 0;
    if( timer_settime( timerId_, 0, &newValue, nullptr ) != 0 ) {
        // TODO: convert this to logging
        fprintf( stderr, "HungThreadDetector: disarm timer_settime failed, errno %d\n", errno );
    }
}

bool
UnixHungThreadDetector::enabled( void )
{
    return constructed_ && ( checkPeriod_ > 0 );
}

void
UnixHungThreadDetector::noteExecBegin( uint64 now )
{
    // From testing, getrusage() is crazy expensive (> 4x getHighResTime). So we want to be sure to
    // avoid it if not enabled.
    if( TOOLS_UNLIKELY( ( checkPeriod_ > 0 ) && ( getrusage( RUSAGE_THREAD, &usageStart_ ) != 0 ))) {
        // TODO: convert this to logging
        fprintf( stderr, "HungThreadDetector: getrusage failed, errno %d\n", errno );
        abort();
    }
    lastExecStartTime_ = now;
}

void
UnixHungThreadDetector::noteExecFinish( void )
{
    lastExecStartTime_ = 0;
}

void
UnixHungThreadDetector::timerFire( uint64 now )
{
    // SIGALRM handler needs to call us
    TOOLS_ASSERT( syscall( SYS_gettid ) == expectedTid_ );
    TOOLS_ASSERT( impl::threadId() == expectedThreadId_ );
    if( lastExecStartTime_ == 0 ) {
        // Nothing running, nothing to complain about
        return;
    }
    if( ( assertDuration_ > 0 ) && TOOLS_UNLIKELY( ( now - lastExecStartTime_ ) >= assertDuration_ )) {
        rusage usageFinish;
        if( TOOLS_UNLIKELY( getrusage( RUSAGE_THREAD, &usageFinish ))) {
            // TODO: convert this to logging
            fprintf( stderr, "HungThreadDetector: call to getrusage failed, errno %d\n", errno );
            abort();
        }
        uint64 userFinishUsec = timevalToUsec( usageFinish.ru_utime );
        uint64 userStartUsec = timevalToUsec( usageStart_.ru_utime );
        uint64 sysFinishUsec = timevalToUsec( usageFinish.ru_stime );
        uint64 sysStartUsec = timevalToUsec( usageStart_.ru_stime );
        // TODO: convert this to logging
        fprintf( stderr, "Hung thread '%s'\n%lf wall clock seconds, %lf user CPU seconds, %lf kernel CPU seconds, %lf minor page faults, %lf voluntary context switches, %lf involuntary context switches\n",
            name_.c_str(), static_cast< double >( now - lastExecStartTime_ ) / TOOLS_NANOSECONDS_PER_SECOND,
            static_cast< double >( userFinishUsec - userStartUsec ) / TOOLS_MICROSECONDS_PER_SECOND,
            static_cast< double >( sysFinishUsec - sysStartUsec ) / TOOLS_MICROSECONDS_PER_SECOND,
            static_cast< double >( usageFinish.ru_minflt - usageStart_.ru_minflt ),
            static_cast< double >( usageFinish.ru_nvcsw - usageStart_.ru_nvcsw ),
            static_cast< double >( usageFinish.ru_nivcsw - usageStart_.ru_nivcsw ));
        abort();
    }
    if( ( complainDuration_ > 0 ) && TOOLS_UNLIKELY( ( now - lastExecStartTime_ ) >= complainDuration_ )) {
        rusage usageFinish;
        if( TOOLS_UNLIKELY( getrusage( RUSAGE_THREAD, &usageFinish ))) {
            // TODO: convert this to logging
            fprintf( stderr, "HungThreadDetector: call to getrusage failed, errno %d\n", errno );
            abort();
        }
        uint64 userFinishUsec = timevalToUsec( usageFinish.ru_utime );
        uint64 userStartUsec = timevalToUsec( usageStart_.ru_utime );
        uint64 sysFinishUsec = timevalToUsec( usageFinish.ru_stime );
        uint64 sysStartUsec = timevalToUsec( usageStart_.ru_stime );
        // TODO: convert this to logging
        fprintf( stdout, "Thread '%s' is taking a long time to return to its main loop.\n%lf wall clock seconds, %lf user CPU seconds, %lf kernel CPU seconds\n",
            name_.c_str(), static_cast< double >( now - lastExecStartTime_ ) / TOOLS_NANOSECONDS_PER_SECOND,
            static_cast< double >( userFinishUsec - userStartUsec ) / TOOLS_MICROSECONDS_PER_SECOND,
            static_cast< double >( sysFinishUsec - sysStartUsec ) / TOOLS_MICROSECONDS_PER_SECOND );
        // TODO: maybe log stack trace
    }
}
