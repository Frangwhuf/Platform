#include "toolsprecompiled.h"

#include <tools/Concurrency.h>
#include <tools/Environment.h>
#include <tools/AsyncTools.h>
#include <tools/Threading.h>
#include <tools/Timing.h>
#include <tools/Tools.h>

#include "Win32Tools.h"

#include <boost/cast.hpp>
#include <algorithm>

using namespace tools;
using boost::numeric_cast;

#define MS_VC_EXCEPTION 0x406D1388
#pragma warning ( disable : 4200 )

namespace {
    struct SimpleMonitorUnlock
        : Disposable
    {
        void dispose( void );

        SRWLOCK mutable lock_;
    };

    struct SimpleMonitor
        : StandardDisposable< SimpleMonitor, Monitor, AllocStatic< Platform > >
        , SimpleMonitorUnlock
    {
        SimpleMonitor( void );
        AutoDispose<> enter( impl::ResourceSample const &, bool );
        bool isAquired( void );
    };

    struct SimpleConditionVar
        : StandardDisposable< SimpleConditionVar, ConditionVar, AllocStatic< Platform > >
    {
        SimpleConditionVar( void );

        // ConditionVar
        AutoDispose< Monitor > monitorNew( impl::ResourceSample const & );
        void wait( void );
        void signal( bool );

        CONDITION_VARIABLE cvar_;
    };

    struct SimpleEvent
        : StandardDisposable< SimpleEvent, Event >
    {
        SimpleEvent( void );
        ~SimpleEvent( void );

        // Event
        void wait( void );
        void post( void );

        HANDLE handle_;
    };

    struct SrwMonitorUnlock
        : Disposable
    {
        void dispose( void );

        SRWLOCK mutable lock_;
    };

    struct SrwMonitor
        : StandardDisposable< SrwMonitor, Monitor, AllocStatic< Platform > >
        , SrwMonitorUnlock
    {
        SrwMonitor( void );

        // Monitor
        AutoDispose<> enter( impl::ResourceSample const &, bool );
    };

    struct ThreadingImpl
        : Threading
        , detail::StandardNoBindService< ThreadingImpl, boost::mpl::list< Threading >::type >
    {
        ThreadingImpl( Environment & );

        // Threading
        AutoDispose< Thread > fork( StringId const &, Thunk const & );
        AutoDispose< Request > forkAll( StringId const &, Thunk const & );

        DWORD   numCores_;
        DWORD   numThreads_;
        DWORD   threadAffinity_;
        DWORD   numThreadAffinity_;
    };

    struct WinThread
        : StandardDisposable< WinThread, Thread >
    {
        WinThread( StringId const &, Thunk const & );
        ~WinThread( void );

        // Thread
        void waitSync( void );
        AutoDispose< Request > wait( void );
        static DWORD WINAPI entry( void * );

        Thunk thunk_;
        StringId name_;
        HANDLE handle_;
        unsigned volatile waiters_;
    };

    struct WinThreadWaiter
        : StandardManualRequest<WinThreadWaiter>
    {
        WinThreadWaiter( WinThread & );
        ~WinThreadWaiter( void );

        // Request
        void start( void );

        // local methods
        static void CALLBACK done( void *, BOOLEAN );

        WinThread & thread_;
        HANDLE waitObject_;
    };

    struct WinThreadAll
        : StandardManualRequest< WinThreadAll, AllocTail< HANDLE >>
    {
        WinThreadAll( Thunk const &, StringId const &, ThreadingImpl &, unsigned );
        ~WinThreadAll( void );
#ifdef WINDOWS_PLATFORM
    private:
        WinThreadAll( WinThreadAll const & c ) : parent_( c.parent_ ) {}
        WinThreadAll & operator=( WinThreadAll const & ) { return *this; }
    public:
#endif // WINDOWS_PLATFORM

        // Request
        void start( void );

        // local methods
        static DWORD WINAPI entry( void * );

        Thunk thunk_;
        StringId name_;
        ThreadingImpl & parent_;
        bool started_;
        unsigned volatile numRunning_;
        unsigned numThreads_;
        HANDLE threads_[];
    };

#pragma pack(push,8)
    struct THREADNAME_INFO
    {
        DWORD dwType;
        LPCSTR szName;
        DWORD dwThreadID;
        DWORD dwFlags;
    };
#pragma pack(pop)

    struct WinThreadSleepVariable
        : StandardDisposable< WinThreadSleepVariable, impl::ThreadSleepVariable, AllocStatic< Platform >>
    {
        WinThreadSleepVariable( void );

        // ThreadSleepVariable
        void wakeOne( void );
        void wakeAll( bool );
        void sleep( uint64 );

        CONDITION_VARIABLE condVar_;
        SRWLOCK lock_;
        int seq_;
    };

    struct WinHungThreadDetector
        : StandardDisposable< WinHungThreadDetector, impl::HungThreadDetector >
    {
        // This is basically unimplemented
        WinHungThreadDetector( void ) {}

        // HungThreadDetector
        void arm( void ) {}
        void disarm( void ) {}
        bool enabled( void ) { return false; }
        void noteExecBegin( uint64 ) {}
        void noteExecFinish( void ) {}
        void timerFire( uint64 ) {}
    };
};  // anonymous namespace

//////////
// Statics
//////////

static RegisterEnvironment< Threading, ThreadingImpl > regThreading;
namespace {
    static unsigned volatile threadStackCount = 0;
    static size_t volatile threadStackBytes = 0;
};  // anonymous namespace

///////////////////////
// Non-member functions
///////////////////////

namespace tools {
    namespace impl {
        uint64
        threadId( void )
        {
            return static_cast< uint64 >( GetCurrentThreadId() );
        }

        uint32
        cpuNumber( void )
        {
            return static_cast< uint32 >( GetCurrentProcessorNumber() );
        }

        AutoDispose< ThreadSleepVariable >
        threadSleepVariableNew( void )
        {
            return new WinThreadSleepVariable;
        }

        AutoDispose< Monitor >
        monitorPlatformNew( void )
        {
            return new SimpleMonitor();
        }

        AutoDispose< ConditionVar >
        conditionVarPlatformNew( void )
        {
            return new SimpleConditionVar();
        }

        void
        threadLocalBegin( void )
        {
            // nothing really to do here
        }

        void *
        threadLocalAlloc( void )
        {
            return reinterpret_cast< void * >( static_cast< size_t >( TlsAlloc() ));
        }

        void *
        threadLocalGet(
            void * key )
        {
            return TlsGetValue( static_cast< DWORD >( reinterpret_cast< size_t >( key )));
        }

        void
        threadLocalSet(
            void * key,
            void * value )
        {
            TlsSetValue( static_cast< DWORD >( reinterpret_cast< size_t >( key )), value );
        }

        void
        threadLocalFree(
            void * key )
        {
            TlsFree( static_cast< DWORD >( reinterpret_cast< size_t >( key )));
        }

        ConditionVarLock ** conditionVarPlatformLockRef( ConditionVar * );
        AutoDispose< Monitor > monitorConditionVarNew( ConditionVar * );

        void
        SetThreadName(
            DWORD id,
            char const * name )
        {
#ifdef TOOLS_DEBUG
            THREADNAME_INFO info;
            info.dwType = 0x1000;
            info.szName = name;
            info.dwThreadID = id;
            info.dwFlags = 0;
            __try {
                RaiseException( MS_VC_EXCEPTION, 0, sizeof( info ) / sizeof( ULONG_PTR ),
                    ( ULONG_PTR * )&info );
            }
            __except( EXCEPTION_EXECUTE_HANDLER ) {
            }
#endif // TOOLS_DEBUG
        }

        unsigned
        platformStackCount( void )
        {
            return atomicRead( &threadStackCount );
        }

        size_t
        platformStackBytes( void )
        {
            return atomicRead( &threadStackBytes );
        }

        void
        logUntrackedMemory( void )
        {
        }

        void
        platformMallocStats( void )
        {
        }

        AutoDispose< HungThreadDetector >
        platformHungThreadDetectorNew(
            StringId const &,
            uint64,
            uint64,
            uint64 )
        {
            return new WinHungThreadDetector();
        }
    };  // impl namespace

    AutoDispose< Event >
    eventNew( impl::ResourceSample const & )
    {
        return new SimpleEvent();
    }
};  // tools namespace

//////////////////////
// SimpleMonitorUnlock
//////////////////////

void
SimpleMonitorUnlock::dispose( void )
{
    ReleaseSRWLockExclusive( &lock_ );
}

////////////////
// SimpleMonitor
////////////////

SimpleMonitor::SimpleMonitor( void )
{
    InitializeSRWLock( &lock_ );
}

AutoDispose<>
SimpleMonitor::enter(
    impl::ResourceSample const &,
    bool tryOnly )
{
    if( tryOnly ) {
        return ( TryAcquireSRWLockExclusive( &lock_ )) ?
            static_cast< SimpleMonitorUnlock * >( this ) : NULL;
    }
    AcquireSRWLockExclusive( &lock_ );
    return static_cast< SimpleMonitorUnlock * >( this );
}

bool
SimpleMonitor::isAquired( void )
{
    // TODO: implement this at some point
    return true;
}

/////////////////////
// SimpleConditionVar
/////////////////////

SimpleConditionVar::SimpleConditionVar( void )
{
    InitializeConditionVariable( &cvar_ );
}

AutoDispose< Monitor >
SimpleConditionVar::monitorNew(
    impl::ResourceSample const & )
{
    return std::move( impl::monitorConditionVarNew( this ));
}

void
SimpleConditionVar::wait( void )
{
    impl::ConditionVarLock ** cvarMonitor = impl::conditionVarPlatformLockRef( this );
    impl::ConditionVarLock * lockVal = *cvarMonitor;
    Disposable * lock = lockVal->lock_;

    TOOLS_ASSERT( !!lockVal );
    TOOLS_ASSERT( !!lock );

    *cvarMonitor = NULL;
    lockVal->lock_ = NULL;

    // Since condition variables users must tollerate spurrious wakeups, insert some to
    // prevent spurious drops (about 5 minutes).
    SleepConditionVariableSRW( &cvar_, &static_cast< SrwMonitor * >( lockVal->monitor_ )->lock_,
        INFINITE /* 177817 */, 0 );

    TOOLS_ASSERT( !lockVal->lock_ );
    lockVal->lock_ = lock;
    TOOLS_ASSERT( !*cvarMonitor );
    *cvarMonitor = lockVal;
}

void
SimpleConditionVar::signal(
    bool all )
{
    if( all ) {
        WakeAllConditionVariable( &cvar_ );
    } else {
        WakeConditionVariable( &cvar_ );
    }
}

//////////////
// SimpleEvent
//////////////

SimpleEvent::SimpleEvent( void )
{
    handle_ = ::CreateEventA( 0, false, false, "Event" );
}

SimpleEvent::~SimpleEvent( void )
{
    CloseHandle( handle_ );
}

void
SimpleEvent::wait( void )
{
    WaitForSingleObject( handle_, INFINITE );
}

void
SimpleEvent::post( void )
{
    ::SetEvent( handle_ );
}

///////////////////
// SrwMonitorUnlock
///////////////////

void
SrwMonitorUnlock::dispose( void )
{
    ReleaseSRWLockExclusive( &lock_ );
}

/////////////
// SrwMonitor
/////////////

SrwMonitor::SrwMonitor( void )
{
    InitializeSRWLock( &lock_ );
}

AutoDispose<>
SrwMonitor::enter(
    impl::ResourceSample const &,
    bool tryOnly )
{
    if( tryOnly ) {
        return ( TryAcquireSRWLockExclusive( &lock_ )) ? static_cast< SrwMonitorUnlock * >( this ) : NULL;
    }
    AcquireSRWLockExclusive( &lock_ );
    return static_cast< SrwMonitorUnlock * >( this );
}

////////////////
// ThreadingImpl
////////////////

ThreadingImpl::ThreadingImpl(
    Environment & )
    : numCores_( 0 )
    , threadAffinity_( 0 )
{
    SYSTEM_INFO si;
    GetSystemInfo( &si );
    numThreads_ = si.dwNumberOfProcessors;
    //SYSTEM_LOGICAL_PROCESSOR_INFORMATION procInfo[ 64 ];
    //unsigned procInfoLen = sizeof( procInfo );
    //GetLogicalProcessorInformation( procInfo, &procInfoLen );
    //for( unsigned i=0; i!=procInfoLen/sizeof( SYSTEM_LOGICAL_PROCESSOR_INFORMATION ); ++i ) {
    //    if( procInfo[ i ].Relationship == RelationProcessorCore ) {
    //        ++numCores_;
    //    }
    //}
    numCores_ = numThreads_;
    numThreadAffinity_ = numThreads_ / numCores_;
}

AutoDispose< Thread >
ThreadingImpl::fork(
    StringId const &    name,
    Thunk const &  thunk )
{
    return new WinThread( name, thunk );
}

AutoDispose< Request >
ThreadingImpl::forkAll(
    StringId const & name,
    Thunk const & thunk )
{
    return new WinThreadAll( thunk, name, *this, numThreads_ );
}

////////////
// WinThread
////////////

WinThread::WinThread(
    StringId const & name,
    Thunk const & thunk )
    : thunk_( thunk )
    , name_( name )
    , waiters_( 0 )
{
    DWORD id;
    handle_ = CreateThread( NULL, 0, entry, this, 0, &id );
    TOOLS_ASSERT(!!handle_);
    impl::SetThreadName( id, name.c_str() );
    atomicIncrement( &threadStackCount );
    atomicAdd( &threadStackBytes, 1024 * 1024 );
}

WinThread::~WinThread( void )
{
    CloseHandle( handle_ );
    atomicDecrement( &threadStackCount );
    atomicSubtract( &threadStackBytes, 1024 * 1024 );
}

void
WinThread::waitSync( void )
{
    WaitForSingleObject( handle_, INFINITE );
}

AutoDispose< Request >
WinThread::wait( void )
{
    return new WinThreadWaiter( *this );
}

DWORD
WinThread::entry(
    void * context )
{
    WinThread * self = static_cast< WinThread * >( context );
    // Reset any implied affinity mask
    SetThreadAffinityMask( GetCurrentThread(), 0xffffff );
    SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_NORMAL );
    // invoke the thunk
    self->thunk_();
    return 0;
}

//////////////////
// WinThreadWaiter
//////////////////

WinThreadWaiter::WinThreadWaiter(
    WinThread & thread )
    : thread_( thread )
    , waitObject_( 0 )
{
}

WinThreadWaiter::~WinThreadWaiter( void )
{
    if( !!waitObject_ ) {
        BOOL res = UnregisterWait( waitObject_ );
        TOOLS_ASSERT( res );
    }
}

void
WinThreadWaiter::start( void )
{
    atomicIncrement( &thread_.waiters_ );
    BOOL res = RegisterWaitForSingleObject( &waitObject_, thread_.handle_, done, this,
        INFINITE, WT_EXECUTEDEFAULT | WT_EXECUTELONGFUNCTION | WT_EXECUTEONLYONCE );
    TOOLS_ASSERT( res );
}

void
WinThreadWaiter::done(
    void * _this,
    BOOLEAN )
{
    WinThreadWaiter * self = static_cast< WinThreadWaiter * >( _this );
    atomicDecrement( &self->thread_.waiters_ );
    self->finish();
}

///////////////
// WinThreadAll
///////////////

WinThreadAll::WinThreadAll(
    Thunk const & thunk,
    StringId const & name,
    ThreadingImpl & parent,
    unsigned count )
    : thunk_( thunk )
    , name_( name )
    , parent_( parent )
    , started_( false )
    , numRunning_( 0 )
    , numThreads_( count )
{
    std::fill( threads_, threads_ + count, static_cast< HANDLE >( NULL ));
}

WinThreadAll::~WinThreadAll( void )
{
    if( started_ ) {
        // Wakt for all threads to exit.
        WaitForMultipleObjects( numThreads_, threads_, TRUE, INFINITE );
        std::for_each( threads_, threads_ + numThreads_, [&]( HANDLE t )->void {
            CloseHandle( t );
        });
    } else {
#ifdef TOOLS_DEBUG
        std::for_each( threads_, threads_ + numThreads_, [&]( HANDLE t )->void {
            TOOLS_ASSERT( !t );
        });
#endif // TOOLS_DEBUG
    }
}

void
WinThreadAll::start( void )
{
    ULONG_PTR sz;
    LPPROC_THREAD_ATTRIBUTE_LIST attribs;
    DWORD affinity = (++parent_.threadAffinity_ % parent_.numThreadAffinity_ );
    DWORD id = 0;
    started_ = true;
    numRunning_ = numThreads_;
    InitializeProcThreadAttributeList( NULL, 2, 0, &sz );
    attribs = static_cast< LPPROC_THREAD_ATTRIBUTE_LIST >( alloca( sz ));
    for( unsigned i=0; i!=numThreads_; ++i ) {
        PROCESSOR_NUMBER proc;
        UCHAR node;
        ULONGLONG nodeMask;
        proc.Group = 0;
        proc.Number = static_cast< BYTE >( i*parent_.numThreadAffinity_ + affinity );
        proc.Reserved = 0;
        GetNumaProcessorNode( static_cast< UCHAR >( i*parent_.numThreadAffinity_ ), &node );
        GetNumaNodeProcessorMask( node, &nodeMask );
        InitializeProcThreadAttributeList( attribs, 2, 0, &sz );
        UpdateProcThreadAttribute( attribs, 0, PROC_THREAD_ATTRIBUTE_IDEAL_PROCESSOR, &proc, sizeof( PROCESSOR_NUMBER ), NULL, NULL );
        UpdateProcThreadAttribute( attribs, 0, PROC_THREAD_ATTRIBUTE_PREFERRED_NODE, &node, sizeof( UCHAR ), NULL, NULL );
        threads_[ i ] = CreateRemoteThreadEx( GetCurrentProcess(), NULL, 0, entry, this, CREATE_SUSPENDED, attribs, &id );
        TOOLS_ASSERT( !!threads_[ i ] );
        // lock the thread to a particular node
        SetThreadAffinityMask( threads_[ i ], static_cast< DWORD_PTR >( nodeMask ));
        impl::SetThreadName( id, name_.c_str() );
        ResumeThread( threads_[ i ] );
        DeleteProcThreadAttributeList( attribs );
    }
}

DWORD
WinThreadAll::entry(
    void * context )
{
    WinThreadAll * self = static_cast< WinThreadAll * >( context );
    self->thunk_();
    if( !atomicDecrement( &self->numRunning_ )) {
        // Look, we're done.  Let's tell someone.
        self->finish();
    }
    return 0;
}

/////////////////////////
// WinThreadSleepVariable
/////////////////////////

WinThreadSleepVariable::WinThreadSleepVariable( void )
    : seq_( 0 )
{
    InitializeConditionVariable( &condVar_ );
    InitializeSRWLock( &lock_ );
}

void
WinThreadSleepVariable::wakeOne( void )
{
    AcquireSRWLockExclusive( &lock_ );
    seq_ += 2;
    ReleaseSRWLockExclusive( &lock_ );
    WakeConditionVariable( &condVar_ );
}

void
WinThreadSleepVariable::wakeAll( bool stopping )
{
    // Use the low bit to signal that all threads should stay awake from here on.
    AcquireSRWLockExclusive( &lock_ );
    if( stopping ) {
        seq_ |= 1;
    } else {
        seq_ += 2;
    }
    ReleaseSRWLockExclusive( &lock_ );
    WakeAllConditionVariable( &condVar_ );
}

void
WinThreadSleepVariable::sleep( uint64 timeout )
{
    DWORD ms = static_cast< DWORD >( timeout / TOOLS_NANOSECONDS_PER_MILLISECOND );
    int prevSeq;
    AcquireSRWLockExclusive( &lock_ );
    prevSeq = seq_;
    while( seq_ == ( prevSeq & ~1 )) {
        SleepConditionVariableSRW( &condVar_, &lock_, ms, 0 );
    }
    ReleaseSRWLockExclusive( &lock_ );
}
