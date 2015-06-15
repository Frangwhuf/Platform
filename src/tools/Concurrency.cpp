#include "toolsprecompiled.h"

#include <tools/Algorithms.h>
#include <tools/AsyncTools.h>
#include <tools/Concurrency.h>
#include <tools/InterfaceTools.h>
#include <tools/Threading.h>
#include <tools/Timing.h>
#include <tools/Tools.h>
#include <tools/WeakPointer.h>

#include "TimingImpl.h"

#include <algorithm>
#include <unordered_map>

using namespace tools;

namespace tools {
    namespace impl {
        AutoDispose< Monitor > monitorPlatformNew( void );
        AutoDispose< ConditionVar > conditionVarPlatformNew( void );
        void threadLocalBegin( void );
        void * threadLocalAlloc( void );
        void threadLocalSet( void *, void * );
        void threadLocalFree( void * );
    };  // impl namespace
};  // tools namespace

namespace {
    struct MonitorDebugInfo
    {
        MonitorDebugInfo( impl::ResourceSample const &, unsigned );
        ~MonitorDebugInfo( void );

        // Previous monitor in a stack on this thread
        MonitorDebugInfo * previous_;
        impl::ResourceSample sample_;
        unsigned level_;
        impl::ResourceTrace * trace_;
    };

    struct MonitorLocalInfo
    {
        MonitorLocalInfo( void );
        ~MonitorLocalInfo( void );

        void push( impl::ConditionVarLock * );
        void pop( impl::ConditionVarLock * );
        void tryPush( MonitorDebugInfo *, impl::ResourceSample const & );
        void push( MonitorDebugInfo * );
        void pop( MonitorDebugInfo * );

        // Current condition variable related monitor
        impl::ConditionVarLock * cvarLock_;
        // Current monitor on this thread for verification
        MonitorDebugInfo * debug_;
        // Is this for a real-time thread?
        bool isRt_;  // TODO: mark this
    };

    struct VerifyMonitorLock
        : Disposable
    {
        void dispose( void );
    };

    struct VerifyMonitorContendedLock
        : Disposable
    {
        void dispose( void );
    };

    struct VerifyMonitor
        : StandardDisposable< VerifyMonitor, Monitor, AllocStatic< Platform >>
        , VerifyMonitorLock
        , VerifyMonitorContendedLock
    {
        VerifyMonitor( AutoDispose< Monitor > &, impl::ResourceSample const &, unsigned, Policy );
        void update( impl::ResourceSample const &, MonitorLocalInfo & );
        AutoDispose<> enter( impl::ResourceSample const &, bool );
        bool isAquired( void );

#ifdef TOOLS_DEBUG
        static uint64 const realtimeContentionMax_ = 10 * TOOLS_NANOSECONDS_PER_MILLISECOND;
        static uint64 const contentionMax_ = 100 * TOOLS_NANOSECONDS_PER_MILLISECOND;
#else // TOOLS_DEBUG
        static uint64 const realtimeContentionMax_ = TOOLS_NANOSECONDS_PER_MILLISECOND;
        static uint64 const contentionMax_ = 10 * TOOLS_NANOSECONDS_PER_MILLISECOND;
#endif // TOOLS_DEBUG

        AutoDispose< Monitor > inner_;
        AutoDispose<> innerLock_;
        MonitorDebugInfo debug_;
        Policy policy_;
        bool everAquiredFromRt_;
        uint64 contendedNsec_;
        bool prevRt_;
        impl::ResourceTrace * prevTrace_;
        bool currRt_;
        impl::ResourceTrace * currTrace_;
    };

    struct LocalManager;
    struct PerThreadLocalManager;

    struct LocalHandleRecord
        : StandardDisposable< LocalHandleRecord, ThreadLocalHandle, AllocStatic< Platform >>
    {
        explicit LocalHandleRecord( ThreadLocalFactory & );
        ~LocalHandleRecord( void );

        // ThreadLocalHandle
        void * get( void );

        // The platform specific handle for the interface and the dispose (so we don't
        // keep a structure in dispose form).
        void * tlsItf_;
        // Configuration
        ThreadLocalFactory * factory_;
        LocalManager * parent_;
    };

    struct LocalManager
        //: AllocStatic< Platform >
    {
        typedef std::vector< PerThreadLocalManager * > ThreadVec;
        typedef std::vector< AutoDispose<> > DisposableVec;

        LocalManager( void );
        ~LocalManager( void );

        // Record a newly created service on the current thread
        void beginHandle( LocalHandleRecord *, AutoDispose<> & );
        void endHandle( LocalHandleRecord * );
        // Pre-notice that a thread is terminating
        void endThread( void );

        // When we're pulling peer threads
        AutoDispose< Monitor > threadLock_;
        // Threads we're witnessed being created.
        ThreadVec threads_;
        // The system thread-local record.  This is created when the first service is
        // instantiated.
        void * tlsThread_;
    };

    struct PerThreadLocalManager
        //: AllocStatic< Platform >
    {
        struct HandleRecord
        {
            HandleRecord( void );
            HandleRecord( LocalHandleRecord *, AutoDispose<> & );
            HandleRecord( HandleRecord && );

            HandleRecord & operator=( HandleRecord && );

            LocalHandleRecord * handle_;
            AutoDispose<> value_;
        };
        // An additional copy of each of the services available on this thread.
        // Do not use an explicit allocator here because it can create a circularity
        // during startup, as follows:
        //   1 - When attempting to allocate, we look in the TLS to see if we have a
        //       thread-local allocation pool.  If not (as is the case during startup),
        //       we create one and try to store a pointer to it in TLS.
        //   2 - Storing something in TLS requires space allocation.  Goto step 1.
        // Break this circularity by not using an explicit allocator here.
        typedef std::vector< HandleRecord > HandleVec;

        PerThreadLocalManager( void );

        // Register this instance in the context of the thread
        void beginHandle( LocalHandleRecord *, AutoDispose<> & );
        AutoDispose<> endHandle( LocalHandleRecord * );

        HandleVec handles_;
        AutoDispose< Monitor > handleLock_;
    };

    // Pooled monitor, use these for distributed locks.
    struct MonitorPool
    {
        // This should be large enough to avoid aliasing and most bouncing around when
        // randomly accessed.  Target size is the (# of processors * 2) ^ 2.  It's thus
        // very unlikely that a lock will collide or flutter between CPUs more than a
        // private lock.
        //
        // The monitors themselves are created on demand and at level 0.
        static size_t const monitorsUsed = 4096U;

        struct PooledMonitor
            : Monitor
        {
            PooledMonitor( void );
            ~PooledMonitor( void );

            // Monitor
            void dispose( void );
            AutoDispose<> enter( impl::ResourceSample const &, bool );
            bool isAquired( void );

            // local methods
            AutoDispose< Monitor > get( void );

            Monitor * volatile inner_;
        };

        MonitorPool( void );
        AutoDispose< Monitor > get( void * );

        PooledMonitor pool_[ monitorsUsed ];
        AutoDispose< Monitor > dependency_;
    };

    struct RwMonitorImpl
        : StandardDisposable< RwMonitorImpl, RwMonitor, AllocStatic< Platform > >
    {
        struct Reader
        {
            Reader( RwMonitorImpl *, impl::ResourceSample const & );
            ~Reader( void );
            Monitor * inner( void );

            RwMonitorImpl * rw_;
            AutoDispose< Monitor > inner_;
        };

        typedef std::vector< Reader *, AllocatorAffinity< Reader *, Platform >> ReaderVec;

        struct ReaderMonitor
            : Monitor
        {
            // Monitor
            void dispose( void );
            AutoDispose<> enter( impl::ResourceSample const &, bool );
            bool isAquired( void );

            RwMonitorImpl * rw_;
        };

        struct WriterMonitor
            : Monitor
        {
            typedef std::vector< Disposable *, AllocatorAffinity< Disposable *, Platform >> DisposableVec;

            WriterMonitor( void );

            // Monitor
            void dispose( void );
            AutoDispose<> enter( impl::ResourceSample const &, bool );
            bool isAquired( void );

            RwMonitorImpl * rw_;
            AutoDispose<> config_;
            DisposableVec locks_;
        };

        RwMonitorImpl( impl::ResourceSample const &, Monitor::Policy );
        ~RwMonitorImpl( void );

        // RwMonitor
        AutoDispose<> enter( impl::ResourceSample const &, bool );
        bool isAquired( void );
        AutoDispose<> enterShared( impl::ResourceSample const &, bool );

        Monitor::Policy policy_;
        bool everAquiredRt_;
        ReaderMonitor reader_;
        ReaderVec readers_;
        AutoDispose< Monitor > config_;
        WriterMonitor writer_;
        StandardThreadLocalHandle< Reader > localReader_;
        bool dead_;
    };

    struct ConditionVarMonitorUnlock
        : Disposable
    {
        void dispose( void );
    };

    struct ConditionVarMonitor
        : StandardDisposable< ConditionVarMonitor, Monitor >
        , ConditionVarMonitorUnlock
    {
        ConditionVarMonitor( AutoDispose< Monitor > &&, ConditionVar & );
        ~ConditionVarMonitor( void );

        // Monitor
        AutoDispose<> enter( impl::ResourceSample const &, bool );
        bool isAquired( void );

        impl::ConditionVarLock inner_;
    };

    static MonitorPool monitorPool_;

	struct TaskLocalStat
	{
		TaskLocalStat( void );

		bool pushed( void );
		bool pushedShared( void );
		void idle( void );

		// Count of spawns since last time at idle.  When the number of
		// spawns doubles, a signal is sent to wake a peer.  This count
		// is reset each time we return to idle.
		unsigned volatile spawns_;
		unsigned signal_;
		size_t tail_;
		// Need a lock to remove work (typically from another thread).
		// Insert is done via CAS.
		// TODO: replace this all with MPSC queue.
		AutoDispose< Monitor > lock_;
	};

	struct TaskLocalQueue
	{
        enum : size_t {
            spawnsPreCacheMax = 16,
            spawnsPreCacheTarget = 8, // Target number of pre-cache items
        };

		TaskLocalQueue( TaskLocalStat * );
		~TaskLocalQueue( void );

		void pushQueue( Task * );
		void pushQueueAll( Task * );
		// Enqueue a Task, returning if a peer should be woken.
		bool push( Task * );
		// Enqueue a possibly large number of Tasks, keeping only the last.
		Task * pushMany( Task *, ConditionVar & );
		// Get work from the local spawns_ array, if any.
		Task * popSpawns( void );
		// Scanning has 3 behaviors, at varying levels of disruption to
		// the target queue:
		//   1) Attempt to get the lock, then try to take the (second) item.
		//   2) Wait for the lock, then take the (second) item.
		//   3) Wait for the lock, then take the root of the queue.
		// By taking the second item, peer threads don't compete with the
		// local thread when it's adding work.  Further the local thread
		// will prefer its spawns_ array before this queue.
		Task * popQueueSecond( size_t, bool );
		Task * popQueue( size_t );
		// Get the full chain of 'queue all's
		Task * popQueueAll( void );

		TaskLocalStat *	stat_;
		// This is managed lock-free.  Any task can remove, only the local
		// thread adds.
		Task * volatile spawns_[ spawnsPreCacheMax ];
		size_t head_;
		Task * volatile queue_;  // overflow from spawns_
		Task * volatile queueAll_;  // everything
		// If the queue is spawning from an ordered (thus low priority)
		// task, such tasks will not be queued locally, rather they are
		// enqueued on a global list.
		bool ordered_;
	};

	struct OrderedTasks
	{
		OrderedTasks( StringId const & );

		bool push( Task * );
		Task * pop( void );

		StringId name_;  // of this queue
		Task * volatile incoming_;  // reverse order of priority. Only drained under external lock.
		Task * volatile ordered_;  // in order, popped under external lock.
	};

	struct OrderedTasksSet
	{
        enum : size_t {
		    numBuckets = 64, // Items are only added to this array, never removed or changed.
        };

		OrderedTasksSet( void );
		~OrderedTasksSet( void );

		void push(StringId const &, Task *);
		void push(unsigned, Task *);
		Task * pop( void );

		OrderedTasks * volatile tasks_[ numBuckets ];
		// The next bucket to look into for a pop.
		unsigned nextBucket_;
		// Lock for handling pops.
		AutoDispose< Monitor > lock_;
	};

	struct ThreadSafeOrderedTasks
		: OrderedTasks
	{
		ThreadSafeOrderedTasks( StringId const & );

		Task * pop( void );

		AutoDispose< Monitor > popLock_;
	};

    struct WorkDoneItem
    {
        WorkDoneItem( Task const * );

        void * callSite_;
        uint64 threadId_;
        uint64 queueTime_;
        uint64 startTime_;
        uint64 runTime_;
        bool longQueued_;
    };

    struct TaskThreadStats
    {
        enum : size_t {
            maxItems = 40000,
        };

        typedef std::vector< WorkDoneItem, AllocatorAffinity< WorkDoneItem >> WorkDoneVec;

        TaskThreadStats( bool );
        void addTask( WorkDoneItem const & );
        WorkDoneItem * lastTask( void );
        void foundLongDequeue( void );
        void wake( void );
        void sleep( void );

        uint64 begin_;
        uint64 cpuBegin_;
        bool dequeuedLong_;
        WorkDoneVec dequeued_;
        size_t phase_;
        bool doDump_;
    };

    struct SchedulerBind;

	struct TaskSchedImpl
		: ThreadScheduler
		, detail::StandardNoBindService< TaskSchedImpl, boost::mpl::list< ThreadScheduler >::type >
		, Notifiable< TaskSchedImpl >
		, Completable< TaskSchedImpl >
	{
        enum : uint64 {
            kickTimeout = 628ULL * TOOLS_NANOSECONDS_PER_MILLISECOND,  // 2pi 100 milliseconds
        };

		typedef std::vector< std::unique_ptr< TaskLocalQueue >, AllocatorAffinity< std::unique_ptr< TaskLocalQueue >>> Queues;

		TaskSchedImpl( Environment & );
		~TaskSchedImpl( void );

		// ThreadScheduler
		void spawn( Task &, SpawnParam const &, void * );
		AutoDispose< Request > spawnAll( Task & );
		AutoDispose< Generator > fork( Task * & );
		AutoDispose< Request > proxy( AutoDispose< Request > &&, Affinity &, SpawnParam const &, void * );
		AutoDispose< Request > bind( AutoDispose< Request > &&, void * );
		AutoDispose< Generator > bind( AutoDispose< Generator > &&, void * );
        SpawnParam defaultParam( void );

		// Service
		AutoDispose< Request > serviceStart( void );
		AutoDispose< Request > serviceStop( void );

		// local methods
        void computeRates( SchedulerBind * );
        void reportQtime( Task *, SchedulerBind *, TaskThreadStats * );
		void threadEntry( void );
        void runAndReport( Task *, SchedulerBind *, impl::HungThreadDetector *, WorkDoneItem * );
        bool peek( void );
        // TODO: These may no longer be needed
		void notifyKick( Error * );
		void preIdle( void );

		Environment & env_;
		Threading & innerScheduler_;
		Timing & timer_;
		AutoDispose< Monitor > peersLock_;
		Queues peers_;
		unsigned volatile peersUsed_;
		std::unique_ptr<OrderedTasksSet> ordered_;
		std::unique_ptr<ThreadSafeOrderedTasks> orderedSpawns_;
		AutoDispose<Request> forkAll_;
		AutoDispose<ConditionVar> idleCvar_;
		AutoDispose<Monitor> idleLock_;
		AtomicAny< bool > shutdown_;
        unsigned volatile awake_;
		std::unique_ptr<TaskLocalStat> externalStat_;
		std::unique_ptr<TaskLocalQueue> externalQueue_;
        // TODO: convert these to tracking configuration
        bool dumpLongTasks_;
        bool useOrderedQueue_;
        bool freqDetect_;
        unsigned peekThreshold_;
        unsigned rateInterval_;
        // TODO: these may be no longer needed
		AutoDispose<Request> kick_;
		AutoDispose<ConditionVar> kickCvar_;
		AutoDispose<Monitor> kickLock_;
		bool kickShutdown_;
	};

    struct TaskSchedStop
        : StandardRequest< TaskSchedStop >
    {
        TaskSchedStop( TaskSchedImpl * );

        // StandardRequest
        RequestStep start( void );

        TaskSchedImpl * parent_;
    };

    static RegisterEnvironment< TaskScheduler, TaskSchedImpl > regTaskSched;

    struct RateData
    {
        TOOLS_FORCE_INLINE RateData( void ) : events_( 0 ), averageRate_( 0. ) {}

        uint64 events_;
        double averageRate_;
    };

    struct SchedulerBind
        : AllocStatic<>
    {
        enum : size_t {
            maxFreq = 500,
        };

        typedef std::pair< void *, uint64 > CountT;
        typedef std::list< CountT, AllocatorAffinity< CountT, Inherent >> CounterT;
        typedef std::unordered_map< void *, CounterT::iterator, HashAnyOf< void * >, std::equal_to< void * >, AllocatorAffinity< std::pair< void *, CounterT::iterator >, Inherent >> IndexT;

        SchedulerBind( void );
        void poke( StringId const & );
        void setScheduler( ThreadScheduler * );
        void reset( void );
        void count( void * );

        StringId envRole_;
        ThreadScheduler * current_;
        TaskLocalQueue * queue_;
        IndexT callSites_;
        CounterT counters_;
        bool full_;
        uint64 lastTime_;
        double serviceTime_;
        RateData spawns_;
        RateData execs_;
        void * capQueue_;
    };

	struct SynchronousSched
		: ThreadScheduler
	{
		// ThreadScheduler
		void spawn( Task &, SpawnParam const &, void * );
		AutoDispose< Request > spawnAll( Task & );
		AutoDispose< Generator > fork( Task * & );
		AutoDispose< Request > proxy( AutoDispose< Request > &&, Affinity &, SpawnParam const &, void * );
		AutoDispose< Request > bind( AutoDispose< Request > &&, void * );
		AutoDispose< Generator > bind( AutoDispose< Generator > &&, void * );
        SpawnParam defaultParam( void );
	};

    static StandardThreadLocalHandle< SchedulerBind > localScheduler_;
	static SynchronousSched synchronousScheduler_;

	struct SyncSpawnReq
		: StandardManualRequest< SyncSpawnReq >
	{
		SyncSpawnReq( Task & );

		// Request
		void start( void );

		Task & task_;
	};

	struct TaskAll;

	struct TaskAllEntry
		: Task
	{
		TaskAllEntry( void );
		TaskAllEntry( TaskAll & );

		// Task
		void execute( void );

		TaskAll * parent_;
	};

	struct TaskAll
		: StandardManualRequest< TaskAll, AllocTail< TaskAllEntry, Temporal >>
	{
        enum : int {
		    crackRefs = 0U,
		    crackEnters = 1U,
		    crackExits = 2U,
        };

		union crack {
			unsigned packed_;
			uint8_t unpacked_[ 4 ];
		};

		TaskAll( TaskSchedImpl &, Task &, size_t );

		// Request
		void dispose( void );
		void start( void );

		// local methods
		void execute( void );

		static_assert( sizeof( crack ) == sizeof( unsigned ), "Bad union layout" );

		TaskSchedImpl & parent_;
		Task & user_;
		unsigned volatile starts_;
		unsigned used_;
		TaskAllEntry entries_[];
	};

	struct TaskForkGen;

	struct TaskFork
		: Task
	{
		TaskFork( TaskForkGen &, Task & );

		// Task
		void execute( void );

		TaskForkGen & parent_;
		Task & inner_;
	};

	struct TaskForkGen
		: StandardManualGenerator< TaskForkGen, AllocStatic< Temporal >>
	{
        enum : unsigned {
            notify = 0x80000000U,
            generator = 0x40000000U,
            mask = 0x3fffffffU,
        };

		TaskForkGen( ThreadScheduler &, Task *& );

		// Generator
		void dispose( void );
		void start( void );
		bool next( void );

		// local methods
		void spawn( Task & );
		void complete( void );

		ThreadScheduler & parent_;
		Task *& taskRef_;

		// Upper two bits are used as flags.  The high bit for if we've
		// got notification, the next is for if the user has disposed.
		unsigned volatile refs_;
	};

	struct ProxyStartReq
		: StandardManualRequest< ProxyStartReq, AllocDynamic< Temporal >>
		, Task
	{
		ProxyStartReq( AutoDispose< Request > &&, ThreadScheduler &, ThreadScheduler::SpawnParam const &, void * );

		// Request
		void start( void );

		// Task
		void execute( void );

		AutoDispose< Request > inner_;
		ThreadScheduler & target_;
        ThreadScheduler::SpawnParam param_;
        void * callSite_;
	};

	struct ProxyNotifyReq
		: StandardManualRequest< ProxyNotifyReq >
		, Task
	{
		ProxyNotifyReq( AutoDispose< Request > &&, ThreadScheduler &, void * );

		// Request
		void start( void );

		// Task
		void execute( void );

		// local methods
		void notifyInner( Error * );

		AutoDispose< Request > inner_;
		AutoDispose< Error::Reference > error_;
		ThreadScheduler & target_;
        void * callSite_;
        ThreadScheduler::SpawnParam param_;
	};

	struct ProxyNotifyGen
		: StandardManualGenerator< ProxyNotifyGen >
		, Task
	{
		ProxyNotifyGen( AutoDispose< Generator > &&, ThreadScheduler &, void * );

		// Generator
		void start( void );
		bool next( void );

		// Task
		void execute( void );

		// local methods
		void notifyInner( Error * );

		AutoDispose< Generator > inner_;
		AutoDispose< Error::Reference > error_;
		ThreadScheduler & target_;
        void * callSite_;
        ThreadScheduler::SpawnParam param_;
	};

	struct SyncGenerator
		: StandardSynchronousGenerator< SyncGenerator >
	{
		explicit SyncGenerator( Task *& );

		// StandardSynchronousGenerator
		void generatorNext( void );

		Task *& taskRef_;
	};

	struct ForkReq
		: StandardManualRequest< ForkReq >
	{
		ForkReq( AutoDispose< Request > && );

		// Request
		void dispose( void );
		void start( void );

		// local methods
		void notify( Error * );
		void begin( void );

		AutoDispose< Request > inner_;
		AutoDispose< Error::Reference > error_;
		unsigned volatile refs_;
		bool started_;
	};

    struct VerifyStaticMonitor
        : StandardDisposable< VerifyStaticMonitor, Monitor, AllocStatic< Platform >>
    {
        enum : uint64 {
#ifdef TOOLS_DEBUG
            realTimeContentionMax = 10 * TOOLS_NANOSECONDS_PER_MILLISECOND,
#else // TOOLS_DEBUG
            realTimeContentionMax = TOOLS_NANOSECONDS_PER_MILLISECOND,
#endif // TOOLS_DEBUG
        };

        VerifyStaticMonitor( AutoDispose< Monitor > &&, impl::ResourceSample const &, StringId const &, Monitor::Policy );

        // Monitor
        AutoDispose<> enter( impl::ResourceSample const &, bool tryOnly );
        bool isAquired( void );

        // local methods
        void update( impl::ResourceSample const & );

        AutoDispose< Monitor > inner_;
        AutoDispose<> innerLock_;
        impl::ResourceTrace * trace_;
        StringId stereotype_;
        Monitor::Policy policy_;
        bool everRealTime_;
        bool prevRealTime_;
        impl::ResourceTrace * prevTrace_;
        bool curRealTime_;
        impl::ResourceTrace * curTrace_;
    };

    struct FdrImpl
        : impl::Fdr
    {
        void memoryTracking( unsigned &, uint64 & );
    };
};  // anonymous namespace

///////////////////////
// Non-member fucntions
///////////////////////

namespace {
    static bool
    monitorVerifyEnabled( void )
    {
        // TODO: convert this to configuration
        return Build::isDebug_;
    }

    static void
    reportRunTime(
        void * callSite,
        uint64 before,
        uint64 now,
        WorkDoneItem * item,
        unsigned awake )
    {
        // TODO: possibly unify this with ComplainTimer
        TOOLS_ASSERT( !!callSite );
        if( !!item ) {
            item->startTime_ = before;
        }
        if( ( before + TOOLS_NANOSECONDS_PER_SECOND ) < now ) {
            // TODO: convert this to logging
            fprintf( stdout, "Long task run-time (%llu ms) by '%s', awake %d\n",
                (now - before) / TOOLS_NANOSECONDS_PER_MILLISECOND,
                detail::symbolNameFromAddress( callSite ),
                awake );
        }
        if( !!item ) {
            item->runTime_ = now - before;
        }
    }
};  // anonymous namespace

namespace tools {
    AutoDispose< Monitor >
    monitorNew(
        impl::ResourceSample const & res,
        unsigned level,
        Monitor::Policy policy)
    {
        AutoDispose< Monitor > ret( std::move( impl::monitorPlatformNew() ));

#ifdef TOOLS_DEBUG
        return new VerifyMonitor( ret, res, level, policy );
#else // TOOLS_DEBUG
        return std::move( ret );
#endif // TOOLS_DEBUG
    }

    AutoDispose< Monitor >
    monitorPoolNew(
        void * parent )
    {
        return monitorPool_.get( parent );
    }

    AutoDispose< Monitor >
    monitorStaticNew(
        impl::ResourceSample const & res,
        StringId const & stereotype,
        Monitor::Policy policy )
    {
        AutoDispose< Monitor > m( impl::monitorPlatformNew() );
        if( monitorVerifyEnabled() ) {
            return new VerifyStaticMonitor( std::move( m ), res, stereotype, policy );
        }
        return std::move( m );
    }

    AutoDispose< RwMonitor >
    rwMonitorNew(
        impl::ResourceSample const & sample,
        Monitor::Policy policy )
    {
        return new RwMonitorImpl( sample, policy );
    }

    AutoDispose< ConditionVar >
    conditionVarNew(
        impl::ResourceSample const & )
    {
        return std::move( impl::conditionVarPlatformNew() );
    }

    AutoDispose< ThreadLocalHandle >
    registerThreadLocalFactory(
        void ** bound,
        ThreadLocalFactory & factory )
    {
        AutoDispose< LocalHandleRecord > ret( new LocalHandleRecord( factory ));
        *bound = ret->tlsItf_;
        return ret.release();
    }
}; // tools namespace

namespace
{
    static void
    monitorInfoGlobalStorage(
        StandardThreadLocalHandle< MonitorLocalInfo > ** ref )
    {
        static StandardThreadLocalHandle< MonitorLocalInfo > threadLocal_;
        *ref = &threadLocal_;
    }

    static StandardThreadLocalHandle< MonitorLocalInfo > &
    monitorInfoGlobal( void )
    {
        static StandardThreadLocalHandle< MonitorLocalInfo > * local_ = nullptr;
        if( !local_ ) {
            monitorInfoGlobalStorage( &local_ );
        }
        return *local_;
    }
};  // anonymous namespace

namespace tools
{
    namespace impl {
        void
        annotateThread(
            StringId const & envRole )
        {
            localScheduler_.get()->poke( IsNullOrEmptyStringId( envRole ) ? StaticStringId( "[Unknown]" ) : envRole );
        }

        ConditionVarLock **
        conditionVarPlatformLockRef(
            ConditionVar * cvar )
        {
            MonitorLocalInfo & info = *monitorInfoGlobal();
            TOOLS_ASSERT( !info.debug_ );
            ConditionVarLock ** cvarMonitor = &info.cvarLock_;
            TOOLS_ASSERT( !!*cvarMonitor );
            TOOLS_ASSERT( ( *cvarMonitor )->cvar_ == cvar );
            TOOLS_ASSERT( !!( *cvarMonitor )->lock_ );
            return cvarMonitor;
        }

        AutoDispose< Monitor >
        monitorConditionVarNew(
            ConditionVar * cvar )
        {
            return new ConditionVarMonitor( std::move( impl::monitorPlatformNew() ), *cvar );
        }

        impl::Fdr *
        globalFdr( void )
        {
            static FdrImpl ret;
            return &ret;
        }
    };  // impl namespace
};  // tools namespace

bool
tools::detail::setThreadIsRealtime(
    bool realtime )
{
    MonitorLocalInfo & info = *monitorInfoGlobal();
    bool prev = info.isRt_;
    info.isRt_ = realtime;
    return prev;
}

bool
tools::detail::threadIsRealtime( void )
{
    MonitorLocalInfo * info = monitorInfoGlobal().peek();
    if( !!info ) {
        return info->isRt_;
    }
    return false;
}

static LocalManager &
localManagerGet( void )
{
    static LocalManager local_;
    return local_;
}

namespace tools {
    namespace impl {
        void
        threadLocalThreadEnd( void )
        {
            localManagerGet().endThread();
        }
    };  // impl namespace
};  // tools namespace

///////////////////
// MonitorDebugInfo
///////////////////

MonitorDebugInfo::MonitorDebugInfo(
    impl::ResourceSample const & sample,
    unsigned level )
    : previous_( nullptr )
    , sample_( sample )
    , level_( level )
    , trace_( impl::resourceTraceBuild( sample ))
{
}

MonitorDebugInfo::~MonitorDebugInfo( void )
{
    TOOLS_ASSERT( !previous_ );
    // TODO: maybe log contention events
}

///////////////////
// MonitorLocalInfo
///////////////////

MonitorLocalInfo::MonitorLocalInfo( void )
    : cvarLock_( nullptr )
    , debug_( nullptr )
    , isRt_( false )
{
}

MonitorLocalInfo::~MonitorLocalInfo( void )
{
    // Make sure everything is cleared before tearing down the thread.
    TOOLS_ASSERT( !cvarLock_ );
    TOOLS_ASSERT( !debug_ );
}

void
MonitorLocalInfo::push(
    impl::ConditionVarLock * lock )
{
    TOOLS_ASSERT( !!lock->lock_ );
    TOOLS_ASSERT( !cvarLock_ );
    TOOLS_ASSERT( !debug_ );
    cvarLock_ = lock;
}

void
MonitorLocalInfo::pop(
    impl::ConditionVarLock * lock )
{
    TOOLS_ASSERT( !debug_ );
    TOOLS_ASSERT( cvarLock_ == lock );
    TOOLS_ASSERT( !!lock->lock_ );
    cvarLock_ = nullptr;
}

void
MonitorLocalInfo::tryPush(
    MonitorDebugInfo * info,
    impl::ResourceSample const & )
{
    if( !debug_ ) {
        return;
    }
    if( debug_->level_ >= info->level_ ) {
        // emit message about level mismatch.  Implement resourceTraceName from info->trace_.
        for( auto i=debug_; !!i; i=i->previous_ ) {
            // emit message about this info
        }
        TOOLS_ASSERT( !"Monitor level inversion" );
    }
}

void
MonitorLocalInfo::push(
    MonitorDebugInfo * info )
{
    TOOLS_ASSERT( !info->previous_ );
    if( !!debug_ ) {
        // TOOLS_ASSERT( debug_->level_ < info->level_ );
        info->previous_ = debug_;
    }
    debug_ = info;
}

void
MonitorLocalInfo::pop(
    MonitorDebugInfo * info )
{
    TOOLS_ASSERT( debug_ == info );
    debug_ = info->previous_;
    info->previous_ = nullptr;
}

////////////////////
// VerifyMonitorLock
////////////////////

void
VerifyMonitorLock::dispose( void )
{
    VerifyMonitor * this_ = static_cast< VerifyMonitor * >( this );
    // Pop before we're actureally released.
    monitorInfoGlobal()->pop( &this_->debug_ );
    this_->innerLock_.reset();
}

/////////////////////////////
// VerifyMonitorContendedLock
/////////////////////////////

void
VerifyMonitorContendedLock::dispose( void )
{
    VerifyMonitor * this_ = static_cast< VerifyMonitor * >( this );
    // TODO: this are good for logging
    //uint64 localNsec = this_->contendedNsec_;
    //bool localPrevRt = this_->prevRt_;
    //impl::ResourceTrace * localPrevTrace = this_->prevTrace_;
    //bool localCurrRt = this_->currRt_;
    impl::ResourceTrace * localCurrTrace = this_->currTrace_;
    TOOLS_ASSERT( !!localCurrTrace );
    static_cast< VerifyMonitorLock * >( this_ )->dispose();
    // TODO: log the monitor contention
}

////////////////
// VerifyMonitor
////////////////

VerifyMonitor::VerifyMonitor(
    AutoDispose< Monitor > & inner,
    impl::ResourceSample const & res,
    unsigned level,
    Policy policy )
    : inner_( std::move( inner ))
    , debug_( res, level )
    , policy_( policy )
    , everAquiredFromRt_( false )
    , contendedNsec_( 0ULL )
    , prevRt_( false )
    , prevTrace_( nullptr )
    , currRt_( false )
    , currTrace_( nullptr )
{
    // Make sure the thread local information is initialized
    monitorInfoGlobal().get();
}

void
VerifyMonitor::update(
    impl::ResourceSample const & sample,
    MonitorLocalInfo & info )
{
    // Update tracking information
    TOOLS_ASSERT( !!innerLock_ );
    debug_.sample_ = sample;
    info.push( &debug_ );
    prevRt_ = currRt_;
    prevTrace_ = currTrace_;
    currRt_ = info.isRt_;
    currTrace_ = impl::resourceTraceBuild( sample, debug_.trace_ );  // resourceTraceBuildParent?
}

AutoDispose<>
VerifyMonitor::enter(
    impl::ResourceSample const & res,
    bool tryOnly )
{
    MonitorLocalInfo & info = *monitorInfoGlobal();

    // Do an initial level check
    info.tryPush( &debug_, res );
    bool isRt = detail::threadIsRealtime();
    if( isRt ) {
        if( policy_ == PolicyStrict ) {
            // TODO: log monitor taken on real-time thread
        } else {
            everAquiredFromRt_ = true;
        }
    } else if( everAquiredFromRt_ && ( policy_ != PolicyAllowPriorityInversion )) {
        // TODO: log possible priority inversion
        everAquiredFromRt_ = false;
    }
    AutoDispose<> tryIt( inner_->enter( res, true ));
    if( !!tryIt ) {
        TOOLS_ASSERT( !innerLock_ );
        innerLock_ = std::move( tryIt );
        update( res, info );
        return static_cast< VerifyMonitorLock * >( this );
    } else if( tryOnly ) {
        return static_cast< Disposable * >( nullptr );
    }
    if( isRt && !!currTrace_ && !currRt_ ) {
        // TODO: lock a priority inversion
    }
    uint64 contentionWarnNsec = info.isRt_ ? realtimeContentionMax_ : contentionMax_;
    uint64 startTime = impl::getHighResTime();
    innerLock_ = inner_->enter( res );
    update( res, info );
    uint64 endTime = impl::getHighResTime();
    if( ( startTime + contentionWarnNsec ) < endTime ) {
        contendedNsec_ = endTime - startTime;
        return static_cast< VerifyMonitorContendedLock * >( this );
    }
    return static_cast< VerifyMonitorLock * >( this );
}

bool
VerifyMonitor::isAquired( void )
{
    for( MonitorDebugInfo * i = monitorInfoGlobal()->debug_; !!i; i = i->previous_ ) {
        if( i == &debug_ ) {
            // found ourselves on the list
            return true;
        }
    }
    return false;
}

////////////////////
// LocalHandleRecord
////////////////////

LocalHandleRecord::LocalHandleRecord(
    ThreadLocalFactory & factory )
    : tlsItf_( impl::threadLocalAlloc() )
    , factory_( &factory )
    , parent_( &localManagerGet() )
{
    // We don't actually do anything with the parent; we just touch it to make sure it
    // initializes before us.
}

LocalHandleRecord::~LocalHandleRecord( void )
{
    void * itf = tlsItf_;
    LocalManager * parent = parent_;
#ifdef TOOLS_DEBUG
    factory_ = nullptr;
    tlsItf_ = nullptr;
    parent_ = nullptr;
#endif // TOOLS_DEBUG
    // Make certain all of the instances are removed.
    parent->endHandle( this );
    impl::threadLocalFree( itf );
}

void *
LocalHandleRecord::get( void )
{
    void * itf;
    // First time create for this object on this thread
    AutoDispose<> d( factory_->factory( &itf ));
    TOOLS_ASSERT( !!d && !!itf );
    parent_->beginHandle( this, d );
    impl::threadLocalSet( tlsItf_, itf );
    return itf;
}

///////////////
// LocalManager
///////////////

LocalManager::LocalManager( void )
    : threadLock_( impl::monitorPlatformNew() )
    , tlsThread_( impl::threadLocalAlloc() )
{
}

LocalManager::~LocalManager( void )
{
    // Ensure all the thread content is torn down
    for( auto && th : threads_ ) {
        delete th;
    }
}

void
LocalManager::beginHandle(
    LocalHandleRecord * handle,
    AutoDispose<> & value )
{
    PerThreadLocalManager * thread = static_cast< PerThreadLocalManager * >( impl::threadLocalGet( tlsThread_ ));
    if( !thread ) {
        thread = new PerThreadLocalManager();
        impl::threadLocalSet( tlsThread_, thread );
        AutoDispose<> l_( threadLock_->enter() );
        threads_.push_back( thread );
    }
    thread->beginHandle( handle, value );
}

void
LocalManager::endHandle(
    LocalHandleRecord * handle )
{
    DisposableVec disposables;
    {
        AutoDispose<> l_( threadLock_->enter() );
        disposables.reserve( threads_.size() );
        for( auto && th : threads_ ) {
            AutoDispose<> h( std::move( th->endHandle( handle )));
            if( !!h ) {
                disposables.push_back( std::move( h ));
            }
        }
    }
}

void
LocalManager::endThread( void )
{
    PerThreadLocalManager * thread = static_cast< PerThreadLocalManager * >( impl::threadLocalGet( tlsThread_ ));
    if( !thread ) {
        // There was never content allocated for this thread.
        return;
    }
    {
        AutoDispose<> l_( threadLock_->enter() );
        auto i = threads_.begin();
        for( ; i!=threads_.end(); ++i ) {
            if( *i == thread ) {
                *i = threads_.back();
                break;
            }
        }
        TOOLS_ASSERT( i!=threads_.end() );
        threads_.pop_back();
    }
    // Clear the slot, just in case.
    impl::threadLocalSet( tlsThread_, nullptr );
    // Teardown any thread-local services
    delete thread;
}

//////////////////////////////////////
// PerThreadLocalManager::HandleRecord
//////////////////////////////////////

PerThreadLocalManager::HandleRecord::HandleRecord( void )
{
}

PerThreadLocalManager::HandleRecord::HandleRecord(
    LocalHandleRecord * handle,
    AutoDispose<> & value )
    : handle_( handle )
    , value_( std::move( value ))
{
}

PerThreadLocalManager::HandleRecord::HandleRecord(
    PerThreadLocalManager::HandleRecord && r )
    : handle_( r.handle_ )
    , value_( std::move( r.value_ ))
{
}

PerThreadLocalManager::HandleRecord &
PerThreadLocalManager::HandleRecord::operator=(
    PerThreadLocalManager::HandleRecord && r )
{
    handle_ = r.handle_;
    value_ = std::move( r.value_ );
    return *this;
}

////////////////////////
// PerThreadLocalManager
////////////////////////

PerThreadLocalManager::PerThreadLocalManager( void )
    : handleLock_( impl::monitorPlatformNew() )
{
    // Mark this thread as begun
    impl::threadLocalBegin();
}

void
PerThreadLocalManager::beginHandle(
    LocalHandleRecord * handle,
    AutoDispose<> & value )
{
    AutoDispose<> l_( handleLock_->enter() );
    handles_.push_back( std::move( HandleRecord( handle, value )));
}

AutoDispose<>
PerThreadLocalManager::endHandle(
    LocalHandleRecord * handle )
{
    AutoDispose<> l_( handleLock_->enter() );
    for( auto i=handles_.begin(); i!=handles_.end(); ++i ) {
        if( i->handle_ != handle ) {
            continue;
        }
        AutoDispose<> r( std::move( i->value_ ));
        // We want the handles to stay in order, thus..
        handles_.erase( i );
        return std::move( r );
    }
    return nullptr;
}

/////////////////////////////
// MonitorPool::PooledMonitor
/////////////////////////////

MonitorPool::PooledMonitor::PooledMonitor( void )
    : inner_( nullptr )
{
}

MonitorPool::PooledMonitor::~PooledMonitor( void )
{
    if( !!inner_ ) {
        inner_->dispose();
    }
}

void
MonitorPool::PooledMonitor::dispose( void )
{
    // Nothing to do, this is a pooled entity.
}

AutoDispose<>
MonitorPool::PooledMonitor::enter(
    impl::ResourceSample const & sample,
    bool tryOnly )
{
    TOOLS_ASSERT( !!inner_ );
    return inner_->enter( sample, tryOnly );
}

bool
MonitorPool::PooledMonitor::isAquired( void )
{
    return inner_->isAquired();
}

AutoDispose< Monitor >
MonitorPool::PooledMonitor::get( void )
{
    AutoDispose< Monitor > newMonitor;
    Monitor * previousMonitor;

    do {
        previousMonitor = inner_;
        if( !!previousMonitor ) {
            return std::move(this);
        }
        if( !newMonitor ) {
            newMonitor = std::move( monitorNew() );
        }
    } while( atomicCas( &inner_, previousMonitor, newMonitor.get() ) != previousMonitor );
    // It's now in the list.
    newMonitor.release();
    return std::move(this);
}

//////////////
// MonitorPool
//////////////

MonitorPool::MonitorPool( void )
    : dependency_( monitorNew() )
{
}

AutoDispose< Monitor >
MonitorPool::get(
    void * owner )
{
    return pool_[ std::hash< void * >()( owner ) % monitorsUsed ].get();
}

////////////////////////
// RwMonitorImpl::Reader
////////////////////////

RwMonitorImpl::Reader::Reader(
    RwMonitorImpl * rw,
    impl::ResourceSample const & )
    : rw_( rw )
    , inner_( impl::monitorPlatformNew() )
{
    TOOLS_ASSERT( !rw_->dead_ );
    // We are constructing a new reader, lock the configuration.
    AutoDispose<> l_( rw_->config_->enter() );
    // Register this reader.
    rw_->readers_.push_back( this );
}

RwMonitorImpl::Reader::~Reader( void )
{
    AutoDispose<> l_( rw_->config_->enter() );
    auto me = std::find( rw_->readers_.begin(), rw_->readers_.end(), this );
    TOOLS_ASSERT( me != rw_->readers_.end() );
    *me = rw_->readers_.back();
    rw_->readers_.pop_back();
}

Monitor *
RwMonitorImpl::Reader::inner( void )
{
    return inner_.get();
}

///////////////////////////////
// RwMonitorImpl::ReaderMonitor
///////////////////////////////

void
RwMonitorImpl::ReaderMonitor::dispose( void )
{
    // Nothing to do.
}

AutoDispose<>
RwMonitorImpl::ReaderMonitor::enter(
    impl::ResourceSample const & sample,
    bool tryOnly )
{
    // Resolve the thread-local monitor and pass this call to it.
    return std::move( rw_->localReader_->inner()->enter( sample, tryOnly ));
}

bool
RwMonitorImpl::ReaderMonitor::isAquired( void )
{
    return rw_->localReader_->inner()->isAquired();
}

///////////////////////////////
// RwMonitorImpl::WriterMonitor
///////////////////////////////

RwMonitorImpl::WriterMonitor::WriterMonitor( void )
{
    locks_.reserve( 64U );
}

void
RwMonitorImpl::WriterMonitor::dispose( void )
{
    // Release the reader locks in reverse order, then drop the config.
    std::for_each( locks_.rbegin(), locks_.rend(), []( Disposable * d ){ AutoDispose<> toDisp(d); });
    locks_.clear();
    config_.reset();
}

AutoDispose<>
RwMonitorImpl::WriterMonitor::enter(
    impl::ResourceSample const &,
    bool tryOnly )
{
    // First, take the config lock.  This will guarentee no existing writer.
    AutoDispose<> l_( rw_->config_->enter( tryOnly ) );
    if( !l_ ) {
        TOOLS_ASSERT( tryOnly );
        return nullptr;
    }
    config_ = std::move( l_ );

    // Now take all of the reader locks, in order.
    TOOLS_ASSERT( locks_.empty() );
    if( locks_.capacity() < rw_->readers_.size() ) {
        locks_.reserve( rw_->readers_.size() * 2U );
    }
    for( auto && reader : rw_->readers_ ) {
        l_ = reader->inner()->enter( tryOnly );
        if( !l_ ) {
            TOOLS_ASSERT( tryOnly );
            // We failed during a try, rewind.
            dispose();
            return nullptr;
        }
        locks_.push_back( l_.release() );
    }

    return this;
}

bool
RwMonitorImpl::WriterMonitor::isAquired( void )
{
    return rw_->config_->isAquired();
}

////////////////
// RwMonitorImpl
////////////////

RwMonitorImpl::RwMonitorImpl(
    impl::ResourceSample const & samp,
    Monitor::Policy policy )
    : policy_( policy )
    , everAquiredRt_( false )
    , config_( impl::monitorPlatformNew() )
    , localReader_( [ this, samp ]( RwMonitorImpl::Reader ** ref ) { return anyDisposableAllocNew< AllocStatic< Platform >>( ref, this, samp ); })
    , dead_( false )
{
    reader_.rw_ = this;
    writer_.rw_ = this;
    readers_.reserve( 64U );
}

RwMonitorImpl::~RwMonitorImpl( void )
{
    TOOLS_ASSERT( !dead_ );
    dead_ = true;
}

AutoDispose<>
RwMonitorImpl::enter(
    impl::ResourceSample const & sample,
    bool tryOnly )
{
    TOOLS_ASSERT( !dead_ );
    if( detail::threadIsRealtime() ) {
        if( policy_ == Monitor::PolicyStrict ) {
            // TODO: log lock taken on RT thread
        } else {
            everAquiredRt_ = true;
        }
    } else if( everAquiredRt_ && ( policy_ != Monitor::PolicyAllowPriorityInversion )) {
        // TODO: log possible priority inversion
        everAquiredRt_ = false;
    }
    return std::move( writer_.enter( sample, tryOnly ));
}

bool
RwMonitorImpl::isAquired( void )
{
    return reader_.isAquired() || writer_.isAquired();
}

AutoDispose<>
RwMonitorImpl::enterShared(
    impl::ResourceSample const & sample,
    bool tryOnly )
{
    TOOLS_ASSERT( !dead_ );
    if( !everAquiredRt_ || ( policy_ == Monitor::PolicyStrict )) {
        if( /* TOOLS_UNLIKELY */ detail::threadIsRealtime() ) {
            if( policy_ == Monitor::PolicyStrict ) {
                // TODO: log lock taken on RT thread
            } else {
                everAquiredRt_ = true;
            }
        }
    }
    return std::move( reader_.enter( sample, tryOnly ));
}

////////////////////////////
// ConditionVarMonitorUnlock
////////////////////////////

void
ConditionVarMonitorUnlock::dispose( void )
{
    ConditionVarMonitor * this_ = static_cast< ConditionVarMonitor * >( this );
    // Pop before we've actually released
    monitorInfoGlobal()->pop( &this_->inner_ );
    AutoDispose<> lock( this_->inner_.lock_ );
    this_->inner_.lock_ = nullptr;
}

//////////////////////
// ConditionVarMonitor
//////////////////////

ConditionVarMonitor::ConditionVarMonitor(
    AutoDispose< Monitor > && inner,
    ConditionVar & innerVar )
{
    inner_.monitor_ = inner.release();
    inner_.cvar_ = &innerVar;
    inner_.lock_ = nullptr;
}

ConditionVarMonitor::~ConditionVarMonitor( void )
{
    TOOLS_ASSERT( !inner_.lock_ );
    inner_.monitor_->dispose();
}

AutoDispose<>
ConditionVarMonitor::enter(
    impl::ResourceSample const & sample,
    bool tryOnly )
{
    AutoDispose<> l_( std::move( inner_.monitor_->enter( sample, tryOnly )));
    if( !!l_ ) {
        TOOLS_ASSERT( !inner_.lock_ );
        inner_.lock_ = l_.release();
        monitorInfoGlobal()->push( &inner_ );
        return static_cast< ConditionVarMonitorUnlock * >( this );
    }
    return nullptr;
}

bool
ConditionVarMonitor::isAquired( void )
{
    return inner_.monitor_->isAquired();
}

////////////////
// TaskLocalStat
////////////////

TaskLocalStat::TaskLocalStat( void )
	: spawns_( 0U )
	, signal_( 2U )
	, tail_( 0U )
	, lock_( std::move( monitorNew() ) )
{
}

bool
TaskLocalStat::pushed( void )
{
	if( ( ++spawns_ ) == signal_ ) {
		signal_ *= 2U;
		return true;
	}
	return false;
}

bool
TaskLocalStat::pushedShared( void )
{
	unsigned oldSpawns, newSpawns;
	do {
		oldSpawns = spawns_;
		newSpawns = oldSpawns + 1U;
	} while( atomicCas( &spawns_, oldSpawns, newSpawns ) != oldSpawns );
	return isPow2( newSpawns );
}

void
TaskLocalStat::idle( void )
{
	spawns_ = 0U;
	signal_ = 2U;
}

/////////////////
// TaskLocalQueue
/////////////////

TaskLocalQueue::TaskLocalQueue(
	TaskLocalStat * s )
	: stat_( s )
	, head_( 0U )
	, queue_( nullptr )
	, queueAll_( nullptr )
	, ordered_( false )
{
	std::fill( spawns_, spawns_ + spawnsPreCacheMax, static_cast<Task *>( nullptr ) );
}

TaskLocalQueue::~TaskLocalQueue( void )
{
	std::for_each( spawns_, spawns_ + spawnsPreCacheMax, []( Task * t )->void {
		TOOLS_ASSERT( !t );
	} );
	TOOLS_ASSERT( !queue_ && !queueAll_ );
}

void
TaskLocalQueue::pushQueue(
	Task * t )
{
	TOOLS_ASSERT( !t->nextTask_ );
	// We're evidentally full up in the spawns_ array, queue it.
	Task * old;
	do {
		old = queue_;
		t->nextTask_ = old;
	} while( atomicCas( &queue_, old, t ) != old );
}

void
TaskLocalQueue::pushQueueAll(
	Task * t )
{
	TOOLS_ASSERT( !t->nextTask_ );
	// We're evidentally full up in the spawns_ array, queue it.
	Task * old;
	do {
		old = queueAll_;
		t->nextTask_ = old;
	} while( atomicCas( &queueAll_, old, t ) != old );
}

bool
TaskLocalQueue::push(
	Task * t )
{
	TOOLS_ASSERT( !t->nextTask_ );
	if( atomicCas< Task *, Task * >( spawns_ + stat_->tail_, nullptr, t ) == nullptr ) {
		// Advance the tail
		stat_->tail_ = ( stat_->tail_ + 1U ) & ( spawnsPreCacheMax - 1U );
		return stat_->pushed();
	}
	pushQueue( t );
	return stat_->pushed();
}

Task *
TaskLocalQueue::pushMany(
	Task * t,
	ConditionVar & idle )
{
	if( !t ) {
		return nullptr;
	}
	// Enqueue all but the last.
	while( !!t->nextTask_ ) {
		Task * next = t->nextTask_;
		t->nextTask_ = nullptr;
		if( push( t ) ) {
			idle.signal();
		}
		t = next;
	}
	return t;
}

Task *
TaskLocalQueue::popSpawns( void )
{
	size_t base = head_;
	for( size_t i=0; i!=spawnsPreCacheMax; ++i ) {
		size_t spawn = ( base + i ) & ( spawnsPreCacheMax - 1U );
		Task * t = atomicExchange< Task *, Task * >( spawns_ + spawn, nullptr );
		if( !!t ) {
			head_ = ( spawn + 1U ) & ( spawnsPreCacheMax - 1U );
			TOOLS_ASSERT( !t->nextTask_ );
			return t;
		}
	}
	return nullptr;
}

Task *
TaskLocalQueue::popQueueSecond(
	size_t maximum,
	bool /*tryWait*/ )
{
	if( !queue_ ) {
		return nullptr;
	}
	// TODO: try wait
	AutoDispose<> l( stat_->lock_->enter() );
	if( !queue_ ) {
		return nullptr;
	}
	Task ** base = &queue_->nextTask_;
	Task * ret;
	Task ** tail = &ret;
	for( *tail=*base; !!*tail; tail=&(*tail)->nextTask_ ) {
		if( !!maximum ) {
			break;
		}
		--maximum;
	}
	// Remove this range form the queue
	*base = *tail;
	*tail = nullptr;
	return ret;
}

Task *
TaskLocalQueue::popQueue(
	size_t maximum )
{
	if( !queue_ ) {
		return nullptr;
	}
	AutoDispose<> l( stat_->lock_->enter() );
	Task * old;
	Task ** newQueue;
	do {
		old = queue_;
		if( !old ) {
			return nullptr;
		}
		size_t found = 1U;
		for( newQueue=&old->nextTask_; !!*newQueue; newQueue=&(*newQueue)->nextTask_ ) {
			if( found == maximum ) {
				break;
			}
			++found;
		}
	} while( atomicCas( &queue_, old, *newQueue ) != old );
	*newQueue = nullptr;
	return old;
}

Task *
TaskLocalQueue::popQueueAll( void )
{
	if( !queueAll_ ) {
		return nullptr;
	}
	return atomicExchange( &queueAll_, static_cast< Task *>( nullptr ));
}

///////////////
// OrderedTasks
///////////////

OrderedTasks::OrderedTasks(
	StringId const & n )
	: name_( n )
	, incoming_( nullptr )
	, ordered_( nullptr )
{
}

bool
OrderedTasks::push(
	Task * t )
{
	TOOLS_ASSERT( !t->nextTask_ );
	Task * next;
	do {
		next = incoming_;
		t->nextTask_ = next;
	} while( atomicCas( &incoming_, next, t) != next );
	// signal if this is the only item in the queue
	return ( !next && !ordered_ );
}

Task *
OrderedTasks::pop( void )
{
	if( !ordered_ && !incoming_ ) {
		return nullptr;
	}
	Task * t = ordered_;
	if( !!t ) {
		ordered_ = t->nextTask_;
		t->nextTask_ = nullptr;
		return t;
	}
	t = atomicExchange( &incoming_, static_cast< Task * >( nullptr ));
	if( !t || !t->nextTask_ ) {
		return t;
	}
	// Reverse the list.
	while( Task * prev = t ) {
		t = t->nextTask_;
		prev->nextTask_ = ordered_;
		ordered_ = prev;
	}
	t = ordered_;
	ordered_ = t->nextTask_;
	t->nextTask_ = nullptr;
	return t;
}

//////////////////
// OrderedTasksSet
//////////////////

OrderedTasksSet::OrderedTasksSet( void )
	: nextBucket_( 0U )
	, lock_( monitorNew() )
{
	tools::impl::memset( const_cast< void ** >( reinterpret_cast< void * volatile * >( tasks_ )), 0, sizeof( tasks_ ) );
}

OrderedTasksSet::~OrderedTasksSet( void )
{
	std::for_each( tasks_, tasks_ + numBuckets, []( OrderedTasks * ot )->void {
		std::unique_ptr< OrderedTasks > to_del( ot );
	});
}

void
OrderedTasksSet::push(
	StringId const & queue,
	Task * t)
{
	size_t hash = queue.hash();
	unsigned const first = hash % numBuckets;
	unsigned bucket = first;
	std::unique_ptr< OrderedTasks > newQueue;
	do {
		if( !tasks_[ bucket ] ) {
			if( !newQueue.get() ) {
				newQueue.reset( new OrderedTasks( queue ) );
			}
			if( atomicCas( &tasks_[ bucket ], static_cast< OrderedTasks * >( nullptr ), newQueue.get() ) == nullptr ) {
				push( bucket, t );
				newQueue.release();
				return;
			} else {
				continue;
			}
		} else if( tasks_[ bucket ]->name_ == queue ) {
			push( bucket, t );
			return;
		}
		bucket = ( bucket + 1 ) % numBuckets;
	} while( bucket != first );
	TOOLS_ASSERT( bucket == first );
}

void
OrderedTasksSet::push(
	unsigned bucket,
	Task * t)
{
	bool single = tasks_[ bucket ]->push( t );
	if( single ) {
		nextBucket_ = bucket;
	}
}

#ifdef WINDOWS_PLATFORM
#  pragma warning( disable : 4706 )
#endif // WINDOWS_PLATFORM
Task *
OrderedTasksSet::pop( void )
{
	AutoDispose<> l( lock_->enter() );
	Task * ret;
	unsigned const first = nextBucket_;
	do {
		unsigned cur = nextBucket_;
		nextBucket_ = ( nextBucket_ + 1U ) % numBuckets;
		if( tasks_[ cur ] && ( ret = tasks_[ cur ]->pop() ) ) {
			return ret;
		}
	} while( nextBucket_ != first );
	return nullptr;
}
#ifdef WINDOWS_PLATFORM
#  pragma warning( default : 4706 )
#endif // WINDOWS_PLATFORM

/////////////////////////
// ThreadSafeOrderedTasks
/////////////////////////

ThreadSafeOrderedTasks::ThreadSafeOrderedTasks(
	StringId const & name )
	: OrderedTasks( name )
	, popLock_( monitorNew() )
{
}

Task *
ThreadSafeOrderedTasks::pop( void )
{
	AutoDispose<> l( popLock_->enter() );
	return OrderedTasks::pop();
}

///////////////
// WorkDoneItem
///////////////

WorkDoneItem::WorkDoneItem(
    Task const * t )
    : callSite_( t->callSite_ )
    , threadId_( t->threadId_ )
    , queueTime_( t->queueTime_ )
    , startTime_( 0ULL )
    , longQueued_( false )
{
}

//////////////////
// TaskThreadStats
//////////////////

TaskThreadStats::TaskThreadStats(
    bool doDump )
    : doDump_( doDump )
{
}

void
TaskThreadStats::addTask(
    WorkDoneItem const & item )
{
    if( dequeued_.size() < maxItems ) {
        dequeued_.push_back( item );
    } else {
        dequeued_[ phase_ ] = item;
        phase_ = ( phase_ + 1 ) % maxItems;
    }
}

WorkDoneItem *
TaskThreadStats::lastTask( void )
{
    if( dequeued_.size() < maxItems ) {
        return &dequeued_.back();
    } else {
        return &dequeued_[ ( phase_ + maxItems - 1 ) % maxItems ];
    }
}

void
TaskThreadStats::foundLongDequeue( void )
{
    dequeuedLong_ = true;
}

void
TaskThreadStats::wake( void )
{
    dequeuedLong_ = false;
    dequeued_.clear();
    phase_ = 0;
    cpuBegin_ = impl::getHighResTime();  // threadHighResTime?
    begin_ = impl::getHighResTime();
}

void
TaskThreadStats::sleep( void )
{
    uint64 end, cpuEnd;
    end = impl::getHighResTime();
    cpuEnd = impl::getHighResTime();  // threadHighResTime ?
    if( !dequeuedLong_ || !doDump_ ) {
        return;
    }
    // TODO: log thread going to sleep after dequeueing log queued task (end - begin) ns, (cpuEnd - cpuBegin) ns CPU
    // TODO: log for each item in dequeued_: enqueue time (queueTime_), start time (startTime_), run time (runTime_), name (symbol name from addr (callSite_)), thread ID (threadId_), if longQueued_
}

////////////////
// TaskSchedImpl
////////////////

TaskSchedImpl::TaskSchedImpl(
	Environment & e )
	: env_( e )
	, innerScheduler_( *e.get< Threading >() )
	, timer_( *e.get< Timing >() )
	, peersLock_( monitorNew() )
	, peersUsed_( 0U )
	, ordered_( new OrderedTasksSet() )
	, orderedSpawns_( new ThreadSafeOrderedTasks( StringId( "subspawn" ) ) )
	, idleCvar_( conditionVarNew() )
	, idleLock_( idleCvar_->monitorNew() )
	, shutdown_( false )
    , awake_( 0U )
	, externalStat_( new TaskLocalStat() )
	, externalQueue_( new TaskLocalQueue( externalStat_.get() ) )
    // TODO: convert these to tracking configuration
    , dumpLongTasks_( false )
    , useOrderedQueue_( true )
    , freqDetect_( false )
    , peekThreshold_( 63U )
    , rateInterval_( 30U )
    // TODO: maybe obsoleete
	, kickCvar_( conditionVarNew() )
	, kickLock_( kickCvar_->monitorNew() )
	, kickShutdown_( false )
{
	peers_.resize( 48 );
}

TaskSchedImpl::~TaskSchedImpl( void )
{
	// delete the queues manually to avoid locking during thread termination.
	peers_.clear();
}

void
TaskSchedImpl::spawn(
	Task & t,
    SpawnParam const & param,
    void * callSite )
{
	TOOLS_ASSERT( !t.nextTask_ );
	if( atomicRead( &shutdown_ )) {
		// run inline on shutodwn
		t.execute();
		return;
	}
    t.callSite_ = !!callSite ? callSite : TOOLS_RETURN_ADDRESS();
    t.queueTime_ = impl::getHighResTime();
    auto annotation = localScheduler_.get();
    auto localQueue = annotation->queue_;
    if( TOOLS_UNLIKELY( freqDetect_ ) && ( annotation->current_ == this )) {
        annotation->count( t.callSite_ );
    }
    ++annotation->spawns_.events_;
    if( !IsNullOrEmptyStringId( param.queue_ )) {
        TOOLS_ASSERT( param.priority_ == PriorityNewWork );
        ordered_->push( param.queue_, &t );
        if( externalStat_->pushedShared() && !peek() ) {
            // Need more task threads awake.
            idleCvar_->signal();
        }
        return;
    }
    if( !!localQueue && ( annotation->current_ == this ) && ( param.priority_ != PriorityNewWork )) {
        t.threadId_ = impl::threadId();
        if( !localQueue->push( &t )) {
            return;  // no signal required
        }
        if( !peek() ) {
            // Need more task threads awake.
            idleCvar_->signal();
        }
        return;
    }
    externalQueue_->push( &t );
    if( externalStat_->pushedShared() && !peek() ) {
        // Need more task threads awake.
        idleCvar_->signal();
    }
}

AutoDispose< Request >
TaskSchedImpl::spawnAll(
	Task & t )
{
	t.nextTask_ = nullptr;
	if( atomicRead( &shutdown_ )) {
		return new SyncSpawnReq( t );
	}
	size_t peers = peersUsed_;
    t.callSite_ = TOOLS_RETURN_ADDRESS();
	return new( peers ) TaskAll( *this, t, peers );
}

AutoDispose< Generator >
TaskSchedImpl::fork(
	Task * & tref )
{
	return new TaskForkGen( *this, tref );
}

AutoDispose< Request >
TaskSchedImpl::proxy(
	AutoDispose< Request > && inner,
	Affinity & aff,
    SpawnParam const & param,
    void * callSite )
{
	if( !inner ) {
		return static_cast< Request * >( nullptr );
	}
	return new( aff, TOOLS_RESOURCE_SAMPLE_CALLER( sizeof( ProxyStartReq ))) ProxyStartReq( std::move( inner ), *this, param, !!callSite ? callSite : TOOLS_RETURN_ADDRESS() );
}

AutoDispose< Request >
TaskSchedImpl::bind(
	AutoDispose< Request > && inner,
    void * callSite )
{
	return new ProxyNotifyReq( std::move( inner ), *this, !!callSite ? callSite : TOOLS_RETURN_ADDRESS() );
}

AutoDispose< Generator >
TaskSchedImpl::bind(
	AutoDispose< Generator > && inner,
    void * callSite )
{
	return new ProxyNotifyGen( std::forward< AutoDispose< Generator >>( inner ), *this, !!callSite ? callSite : TOOLS_RETURN_ADDRESS() );
}

TaskSchedImpl::SpawnParam
TaskSchedImpl::defaultParam( void )
{
    SpawnParam ret;
    auto localQueue = localScheduler_.get()->queue_;
    if( !!localQueue ) {
        ret.priority_ = PriorityExistingWork;
    } else {
        ret.priority_ = PriorityNewWork;
    }
    return ret;
}

AutoDispose< Request >
TaskSchedImpl::serviceStart( void )
{
	// Start all threads
	forkAll_ = std::move( ThreadScheduler::fork( innerScheduler_.forkAll( "TaskScheduler", this->toThunk< &TaskSchedImpl::threadEntry >() ), this ));
    return static_cast< Request * >( nullptr );
}

AutoDispose< Request >
TaskSchedImpl::serviceStop( void )
{
    return new TaskSchedStop( this );
}

void
TaskSchedImpl::computeRates(
    SchedulerBind * annotation )
{
    if( rateInterval_ == 0 ) {
        return;
    }
    auto now = impl::getHighResTime();
    if( now > ( annotation->lastTime_ + ( rateInterval_ * TOOLS_NANOSECONDS_PER_SECOND ))) {
        double delta = static_cast< double >( now - annotation->lastTime_ ) / TOOLS_NANOSECONDS_PER_SECOND;
        double rateSpawns = annotation->spawns_.events_ / delta;
        double rateExecs = annotation->execs_.events_ / delta;
        // double service = ( annotation->serviceTime_ / TOOLS_NANOSECONDS_PER_SECOND ) / annotation->execs_.events_;
        // low-pass filter
        annotation->spawns_.averageRate_ = ( ( annotation->spawns_.averageRate_ * 7.0 ) + rateSpawns ) / 8.0;
        annotation->execs_.averageRate_ = ( ( annotation->execs_.averageRate_ * 7.0 ) + rateExecs ) / 8.0;
        // TODO: log spawn (and average) rate, exec (and average) rate [tasks/sec], average queue time (service), estimated queue length (rateSpawns * service)
        if( freqDetect_ ) {
            auto iter = annotation->counters_.rbegin();
            if( iter != annotation->counters_.rend() ) {
                // TODO: log most frequently spawned task (symbolNameFromAddress(iter->first)), estimated frequency (<double>iter->second / rateInterval_) tasks/sec
#ifdef VERIFY_TASK_ORDER
                std::accumulate( annotation->counters_.begin(), annotation->counters_.end(), static_cast< uint64 >( 0 ), []( uint64 left, SchedulerBind::CountT const & val )->uint64 {
                    TOOLS_ASSERT( left <= val.second );
                    return val.second;
                });
#endif // VERIFY_TASK_ORDER
            }
            annotation->reset();
        }
        annotation->spawns_.events_ = 0;
        annotation->execs_.events_ = 0;
        annotation->lastTime_ = now;
        annotation->serviceTime_ = 0.0;
    }
}

void
TaskSchedImpl::reportQtime(
    Task * task,
    SchedulerBind * annotation,
    TaskThreadStats * stats )
{
    WorkDoneItem item( task );
    auto now = impl::getHighResTime();
    if( ( task->queueTime_ + TOOLS_NANOSECONDS_PER_SECOND ) < now ) {
        TOOLS_ASSERT( !!task->callSite_ );
        auto delay = ( now - task->queueTime_ ) / TOOLS_NANOSECONDS_PER_MILLISECOND;
        // TODO: convert to logging
        fprintf( stdout, "Long task queue time (%llu ms) by '%s' (on thread %x), awake %d\n",
            delay, detail::symbolNameFromAddress( task->callSite_ ), task->threadId_, atomicRead( &awake_ ));
        item.longQueued_ = true;
        stats->foundLongDequeue();
    }
    annotation->serviceTime_ += now - task->queueTime_;
    stats->addTask( item );
}

void
TaskSchedImpl::threadEntry( void )
{
    TaskThreadStats measure( dumpLongTasks_ );
	AutoDispose<> l( peersLock_->enter() );
	// Take over as the scheduler for this thread
    auto annotation = localScheduler_.get();
	annotation->setScheduler( this );
	TaskLocalStat stat;
	TaskLocalQueue * queue = new TaskLocalQueue( &stat );
	size_t peerOffset = peersUsed_++;
	peers_[ peerOffset ] = std::unique_ptr< TaskLocalQueue >( queue );
	localScheduler_->queue_ = queue;
	l.reset();
    atomicIncrement( &awake_ );  // This thread is running!
    measure.wake();
    uint64 checkDefaultMs = 10000;   // 0 to disable
    uint64 complainDefaultMs = 0;    // 0 to disable
    uint64 assertDefaultMs = 300000; // 0 to disable
    if( !Build::isDebug_ ) {
        // By default this is only enabled in debug.
        checkDefaultMs = 0;
    }
    // TODO: drive this from configuration
    AutoDispose< impl::HungThreadDetector > detector( impl::platformHungThreadDetectorNew( "task", complainDefaultMs, assertDefaultMs, checkDefaultMs ));
    // if( !impl::platformIsDebuggerAttached() ) {
        detector->arm();
    // }
    PhantomPrototype * prototype = &phantomBindPrototype< PhantomUniversal >();
    AutoDispose<> phantomEntry;
	while( true ) {
        if( !phantomEntry ) {
            phantomEntry = prototype->select();
        } else {
            // Quick recycle
            prototype->touch();
        }
		// Let's find some work! Start with spawn all.
		if( Task * t = queue->popQueueAll() ) {
			do {
				Task * run = t;
				t = t->nextTask_;
				run->nextTask_ = nullptr;
                measure.addTask( WorkDoneItem( run ));
                runAndReport( run, nullptr, detector.get(), measure.lastTask() );
			} while( !!t );
			// try again
			continue;
		}
		// Next, local spawns
		if( Task * t = queue->popSpawns() ) {
			TOOLS_ASSERT( !t->nextTask_ );
            reportQtime( t, annotation, &measure );
            runAndReport( t, annotation, detector.get(), measure.lastTask() );
			continue;
		}
		// Next, local queue
		if( Task * t = queue->pushMany( queue->popQueue( TaskLocalQueue::spawnsPreCacheTarget ), *idleCvar_ ) ) {
			TOOLS_ASSERT( !t->nextTask_ );
            reportQtime( t, annotation, &measure );
            runAndReport( t, annotation, detector.get(), measure.lastTask() );
			continue;
		}
        stat.idle();
		// Nothing local, check peers.
		size_t sz = peersUsed_;
		bool any = false;
		for( size_t i=0; i!=sz; ++i ) {
			TaskLocalQueue * q = peers_[ ( i + peerOffset ) % sz ].get();
			if( !q || ( q == queue ) ) {
				continue;
			}
			if( Task * t = queue->pushMany( q->popQueueSecond( TaskLocalQueue::spawnsPreCacheTarget / 2U, false ), *idleCvar_ ) ) {
				TOOLS_ASSERT( !t->nextTask_ );
                reportQtime( t, annotation, &measure );
                runAndReport( t, annotation, detector.get(), measure.lastTask() );
				any = true;
				peerOffset = ( i + peerOffset + 1U ) % sz;
				break;
			}
		}
		if( any ) {
			continue;
		}
		// Check our peers again.  Try harder.
		for( size_t i=0; i!=sz; ++i ) {
			TaskLocalQueue * q = peers_[ ( i + peerOffset ) % sz ].get();
			if( !q || ( q == queue ) ) {
				continue;
			}
			if( Task * t = queue->pushMany( q->popQueue( TaskLocalQueue::spawnsPreCacheTarget / 2U ), *idleCvar_ ) ) {
				TOOLS_ASSERT( !t->nextTask_ );
                reportQtime( t, annotation, &measure );
                runAndReport( t, annotation, detector.get(), measure.lastTask() );
				any = true;
				peerOffset = ( i + peerOffset + 1U ) % sz;
				break;
			}
		}
		if( any ) {
			continue;
		}
		// Scan again, now looking for a spawn
		for( size_t i=0; i!=sz; ++i ) {
			TaskLocalQueue * q = peers_[ ( i + peerOffset ) % sz ].get();
			if( !q || ( q == queue ) ) {
				continue;
			}
			if( Task * t = q->popSpawns() ) {
				// Move all but the last
				TOOLS_ASSERT( !t->nextTask_ );
                reportQtime( t, annotation, &measure );
                runAndReport( t, annotation, detector.get(), measure.lastTask() );
				any = true;
				peerOffset = ( i + peerOffset + 1U ) % sz;
				break;
			}
		}
		if( any ) {
			continue;
		}
		// Ok, maybe the root has something?
        Task * t = nullptr;
		if( t = queue->pushMany( externalQueue_->popQueue( TaskLocalQueue::spawnsPreCacheTarget / 4U ), *idleCvar_ ) ) {
			TOOLS_ASSERT( !t->nextTask_ );
            reportQtime( t, annotation, &measure );
            runAndReport( t, annotation, detector.get(), measure.lastTask() );
			continue;
		} else if( t = orderedSpawns_->pop() ) {
			// Ordered spawns next because the ordered queue may the
			// ordered queue is likely to be expansionary
			TOOLS_ASSERT( !t->nextTask_ );
			TOOLS_ASSERT( !queue->ordered_ );
			queue->ordered_ = true;
            reportQtime( t, annotation, &measure );
            runAndReport( t, annotation, detector.get(), measure.lastTask() );
			TOOLS_ASSERT( queue->ordered_ );
			queue->ordered_ = false;
			continue;
		} else if( t = ordered_->pop() ) {
			// Lowest priority, this may create more unordered work.
			TOOLS_ASSERT( !t->nextTask_ );
			TOOLS_ASSERT( !queue->ordered_ );
			queue->ordered_ = useOrderedQueue_;
            reportQtime( t, annotation, &measure );
            runAndReport( t, annotation, detector.get(), measure.lastTask() );
			queue->ordered_ = false;
			continue;
		} else {
			externalStat_->idle();
		}
        // Drop the phantom before trying to sleep
        phantomEntry.reset();
        measure.sleep();
        computeRates( annotation );
        unsigned prevAwake = atomicSubtract( &awake_, 1 );
        TOOLS_ASSERT( prevAwake > 0U );
        if( atomicRead( &shutdown_ )) {
            break;  // hey look!  We're stopping.
        }
		preIdle(); // make sure we'll get woken up eventually
        {
		    AutoDispose<> lIdle( idleLock_->enter() );
		    idleCvar_->wait();
        }
        atomicIncrement( &awake_ );
        measure.wake();
	}
    annotation->reset();
    detector->disarm();
	// synchronize shutdown
	l = peersLock_->enter();
	// I am not the scheduler for this thread anymore.
	annotation->setScheduler( &synchronousScheduler_ );
}

void
TaskSchedImpl::runAndReport(
    Task * task,
    SchedulerBind * annotation,
    impl::HungThreadDetector * detector,
    WorkDoneItem * item )
{
    auto before = impl::getHighResTime();
    detector->noteExecBegin( before );
    auto callSite = task->callSite_;
    task->execute();
    auto after = impl::getHighResTime();
    detector->noteExecFinish();
    reportRunTime( callSite, before, after, item, awake_ );
    if( !!annotation ) {
        annotation->serviceTime_ += after - before;
        ++annotation->execs_.events_;
    }
}

bool
TaskSchedImpl::peek( void )
{
    return ( atomicRead( &awake_ ) >= peekThreshold_ );
}

void
TaskSchedImpl::notifyKick(
	Error * err )
{
	{
		AutoDispose<> l( kickLock_->enter() );
		kick_.release();
		if( kickShutdown_ ) {
			kickCvar_->signal( true );
			return;
		}
		if( !!err ) {
			// TODO: assert that this is a cancelation error
			return;
		}
	}
	idleCvar_->signal();
}

void
TaskSchedImpl::preIdle( void )
{
	{
		AutoDispose<> l( kickLock_->enter() );
		if( !!kick_ || atomicRead( &shutdown_ )) {
			return;
		}
		kick_ = std::move( timer_.timer( kickTimeout ) );
	}
	kick_->start( toCompletion< &TaskSchedImpl::notifyKick >() );
}

////////////////
// TaskSchedStop
////////////////

TaskSchedStop::TaskSchedStop(
    TaskSchedImpl * sched )
    : parent_( sched )
{
}

RequestStep
TaskSchedStop::start( void )
{
    atomicSet( &parent_->shutdown_, true );
	//{
	//	AutoDispose<> l( parent_->kickLock_->enter() );
	//	parent_->kickShutdown_ = true;
	//	while( !!parent_->kick_ ) {
	//		parent_->kickCvar_->wait();
	//	}
	//}
    parent_->idleCvar_->signal( true );
    // Now wait for all threads to exit. We don't take ownership of the fork request as disposing it (shortly
    // after this request completes) will, itself, join on the task threads. This is a problem because one
    // of those very threads may be running this code. That turns into deadlock.
    return waitFinish(*parent_->forkAll_);
}

////////////////
// SchedulerBind
////////////////

SchedulerBind::SchedulerBind( void )
    : envRole_( StaticStringId( "[Unset]" ))
    , current_( &synchronousScheduler_ )
    , queue_( nullptr )
    , full_( false )
    , lastTime_( 0 )
    , capQueue_( nullptr )
{
}

void
SchedulerBind::poke(
    StringId const & envRole )
{
    envRole_ = envRole;
    spawns_.events_ = execs_.events_ = 0;
    lastTime_ = impl::getHighResTime();
    serviceTime_ = 0;
    capQueue_ = nullptr;
}

void
SchedulerBind::setScheduler(
    ThreadScheduler * current )
{
    current_ = current;
}

void
SchedulerBind::reset( void )
{
    counters_.clear();
    callSites_.clear();
    full_ = false;
}

void
SchedulerBind::count(
    void * address )
{
    auto iter = callSites_.find( address );
    if( iter != callSites_.end() ) {
        auto countIter = iter->second;
        TOOLS_ASSERT( countIter != counters_.end() );
        uint64 value = countIter->second + 1;
        ++countIter;
        while( ( countIter != counters_.end() ) && ( countIter->second < value )) {
            ++countIter;
        }
        // save the updated location
        counters_.erase( iter->second );
        iter->second = counters_.insert( countIter, CountT( address, value ));
    } else if( !full_ && ( counters_.size() < maxFreq )) {
        counters_.push_front( CountT( address, 1 ));
        auto ins = callSites_.insert( IndexT::value_type( address, counters_.begin() ));
        TOOLS_ASSERT( ins.second );
    } else {
        full_ = true;
        CountT evict = *counters_.begin();
        counters_.pop_front();
        callSites_.erase( evict.first );
        CountT newCount = CountT( address, evict.second + 1 );
        auto countIter = counters_.begin();
        while( ( countIter != counters_.end() ) && ( countIter->second < newCount.second )) {
            ++countIter;
        }
        auto ins = callSites_.insert( IndexT::value_type( address, counters_.insert( countIter, newCount )));
        TOOLS_ASSERT( ins.second );
    }
}

///////////////////
// SynchronousSched
///////////////////

void
SynchronousSched::spawn(
	Task & t,
	SpawnParam const &,
    void * )
{
	TOOLS_ASSERT( !t.nextTask_ );
	t.execute();
}

AutoDispose< Request >
SynchronousSched::spawnAll(
	Task & t )
{
	TOOLS_ASSERT( !t.nextTask_ );
	return new SyncSpawnReq( t );
}

AutoDispose< Generator >
SynchronousSched::fork(
	Task * & tref )
{
	return new SyncGenerator( tref );
}

AutoDispose< Request >
SynchronousSched::proxy(
	AutoDispose< Request > && inner,
	Affinity &,
    SpawnParam const &,
    void * callSite )
{
	if( !inner ) {
		return static_cast< Request * >( nullptr );
	}
	ThreadScheduler * current = localScheduler_->current_;
	if( current == &synchronousScheduler_ ) {
        // No point in proxying to ourselves
		return std::forward< AutoDispose< Request >>( inner );
	}
	return new ProxyNotifyReq( std::forward< AutoDispose< Request >>( inner ), *current, !!callSite ? callSite : TOOLS_RETURN_ADDRESS() );
}

AutoDispose< Request >
SynchronousSched::bind(
	AutoDispose< Request > && inner,
    void * )
{
    return std::forward< AutoDispose< Request >>( inner );
}

AutoDispose< Generator >
SynchronousSched::bind(
	AutoDispose< Generator > && inner,
    void * )
{
    return std::forward< AutoDispose< Generator >>( inner );
}

ThreadScheduler::SpawnParam
SynchronousSched::defaultParam( void )
{
    return SpawnParam();
}

///////////////
// SyncSpawnReq
///////////////

SyncSpawnReq::SyncSpawnReq(
	Task & t )
	: task_( t )
{
}

void
SyncSpawnReq::start( void )
{
    auto callSite = task_.callSite_;
    auto before = impl::getHighResTime();
	task_.execute();
    auto after = impl::getHighResTime();
    reportRunTime( callSite, before, after, nullptr, 1000U );
	finish();
}

///////////////
// TaskAllEntry
///////////////

TaskAllEntry::TaskAllEntry( void )
	: parent_( nullptr )
{
}

TaskAllEntry::TaskAllEntry(
	TaskAll & p )
	: parent_( &p )
{
}

void
TaskAllEntry::execute( void )
{
	TOOLS_ASSERT( !!parent_ );
	parent_->execute();
}

//////////
// TaskAll
//////////

TaskAll::TaskAll(
	TaskSchedImpl & parent,
	Task & task,
	size_t used )
	: parent_( parent )
	, user_( task )
	, used_( static_cast< unsigned >( used ) )
{
	crack refs;
	refs.packed_ = 0U;
	// One reference per peer, plus one for this
	refs.unpacked_[ crackRefs ] = static_cast< uint8_t >( parent_.peersUsed_ + 1U );
	starts_ = refs.packed_;
	std::uninitialized_fill_n( entries_, used_, TaskAllEntry( *this ) );
}

void
TaskAll::dispose( void )
{
	unsigned oldRefs;
	crack newRefs;
	do {
		oldRefs = starts_;
		newRefs.packed_ = oldRefs;
		TOOLS_ASSERT( newRefs.unpacked_[ crackRefs ] != 0 );
		TOOLS_ASSERT( newRefs.unpacked_[ crackEnters ] == newRefs.unpacked_[ crackExits ] );
		--newRefs.unpacked_[ crackRefs ];
	} while( atomicCas( &starts_, oldRefs, newRefs.packed_ ) != oldRefs );
	if( newRefs.unpacked_[ crackRefs ] == 0U ) {
		delete this;
	}
}
void
TaskAll::start( void )
{
	size_t offset = HashAnyOf< uint64 >()( impl::threadId() ) % used_;
	for( size_t i=0; i!=used_; ++i ) {
		TaskLocalQueue & q = *parent_.peers_[ ( i + offset ) % used_ ];
		q.pushQueueAll( entries_ + i );
	}
	parent_.idleCvar_->signal( true );
}

void
TaskAll::execute( void )
{
	unsigned oldRefs;
	crack newRefs;
	do {
		oldRefs = starts_;
		newRefs.packed_ = oldRefs;
		TOOLS_ASSERT( newRefs.unpacked_[ crackRefs ] != 0U );
		if( newRefs.unpacked_[ crackExits ] != 0 ) {
			TOOLS_ASSERT( newRefs.unpacked_[ crackEnters ] != newRefs.unpacked_[ crackExits ] );
			--newRefs.unpacked_[ crackRefs ];
		} else {
			++newRefs.unpacked_[ crackEnters ];
		}
	} while( atomicCas( &starts_, oldRefs, newRefs.packed_ ) != oldRefs );
	if( newRefs.unpacked_[ crackExits ] != 0U ) {
		// An exit, we're not notifying, but may be deleting.
		if( newRefs.unpacked_[ crackRefs ] == 0U ) {
			delete this;
		}
		return;
	}
	user_.execute();
	do {
		oldRefs = starts_;
		newRefs.packed_ = oldRefs;
		TOOLS_ASSERT( newRefs.unpacked_[ crackRefs ] > 1U );
		--newRefs.unpacked_[ crackRefs ];
		++newRefs.unpacked_[ crackExits ];
	} while( atomicCas( &starts_, oldRefs, newRefs.packed_ ) != oldRefs );
	if( newRefs.unpacked_[ crackExits ] == newRefs.unpacked_[ crackEnters ] ) {
		// Woo! I get to notify!
		finish();
	}
}

///////////
// TaskFork
///////////

TaskFork::TaskFork(
	TaskForkGen & parent,
	Task & task )
	: parent_( parent )
	, inner_( task )
{
}

void
TaskFork::execute( void )
{
	inner_.execute();
	parent_.complete();
	delete this;
}

//////////////
// TaskForkGen
//////////////

TaskForkGen::TaskForkGen(
	ThreadScheduler & parent,
	Task *& taskRef )
	: parent_( parent )
	, taskRef_( taskRef )
	, refs_( generator )
{
}

void
TaskForkGen::dispose( void )
{
	unsigned oldRefs;
	unsigned newRefs;
	do {
		oldRefs = refs_;
		TOOLS_ASSERT( ( oldRefs & notify ) == 0U );
		TOOLS_ASSERT( ( oldRefs & generator ) != 0U );
		newRefs = oldRefs & ~generator;
	} while( atomicCas( &refs_, oldRefs, newRefs ) != oldRefs );
	if( newRefs == 0U ) {
		delete this;
	}
}

void
TaskForkGen::start( void )
{
	unsigned oldRefs;
	unsigned newRefs;
	do {
		oldRefs = refs_;
		TOOLS_ASSERT( ( oldRefs & notify ) == 0U );
		TOOLS_ASSERT( ( oldRefs & generator ) != 0U );
		if( ( oldRefs & mask ) == 0U ) {
			// nothiing outstanding
			finish();
			return;
		}
		newRefs = oldRefs | notify;
	} while( atomicCas( &refs_, oldRefs, newRefs ) != oldRefs );
}

bool
TaskForkGen::next( void )
{
	Task * userTask = taskRef_;
	if( !!userTask ) {
		unsigned sample = ( refs_ & mask );
		return ( sample == 0U );
	}
	TaskFork * t = new TaskFork( *this, *userTask );
	unsigned oldRefs;
	unsigned newRefs;
	do {
		oldRefs = refs_;
		TOOLS_ASSERT( ( oldRefs & notify ) == 0U );
		TOOLS_ASSERT( ( oldRefs & generator ) != 0U );
		newRefs = generator | ( ( oldRefs & mask ) + 1U );
	} while( atomicCas( &refs_, oldRefs, newRefs ) != oldRefs );
	spawn( *t );
	return true;
}

void
TaskForkGen::spawn(
	Task & t )
{
	TOOLS_ASSERT( !t.nextTask_ );
	parent_.spawn( t, parent_.defaultParam() );
}

void
TaskForkGen::complete( void )
{
	// Mark a task as complete.
	unsigned oldRefs;
	unsigned newRefs;
	unsigned refCount;
	unsigned refFlags;
	do {
		oldRefs = refs_;
		refCount = ( oldRefs & mask );
		refFlags = ( oldRefs & ~mask );
		TOOLS_ASSERT( refCount > 0U );
		--refCount;
		if( refCount == 0U ) {
			refFlags &= ~notify;
		}
		newRefs = ( refCount | refFlags );
	} while( atomicCas( &refs_, oldRefs, newRefs ) != oldRefs );
	if( ( refCount != 0U ) || (( refFlags & notify ) == 0U ) ) {
		return;
	}
	finish();  // a notification edge
}

////////////////
// ProxyStartReq
////////////////

ProxyStartReq::ProxyStartReq(
	AutoDispose< Request > && inner,
	ThreadScheduler & target,
    ThreadScheduler::SpawnParam const & param,
    void * callSite )
	: inner_( std::forward< AutoDispose< Request >>( inner ))
	, target_( target )
    , param_( param )
    , callSite_( callSite )
{
}

void
ProxyStartReq::start( void )
{
	TOOLS_ASSERT( !nextTask_ );
	target_.spawn( *this, param_, callSite_ );
}

void
ProxyStartReq::execute( void )
{
	TOOLS_ASSERT( !nextTask_ );
	callFinish( *inner_ );
}

/////////////////
// ProxyNotifyReq
/////////////////

ProxyNotifyReq::ProxyNotifyReq(
	AutoDispose< Request > && inner,
	ThreadScheduler & target,
    void * callSite )
	: inner_( std::forward< AutoDispose< Request >>( inner ))
	, target_( target )
    , callSite_( callSite )
{
}

void
ProxyNotifyReq::start( void )
{
    param_ = target_.defaultParam();
	call< &ProxyNotifyReq::notifyInner >( *inner_ );
}

void
ProxyNotifyReq::execute( void )
{
	TOOLS_ASSERT( !nextTask_ );
	if( !error_ ) {
		finish();
	} else {
		AutoDispose< Error::Reference > e( std::move( error_ ));
		finish( *e );
	}
}

void
ProxyNotifyReq::notifyInner(
	Error * err )
{
	if( !!err ) {
		error_ = err->ref();
	}
	TOOLS_ASSERT( !nextTask_ );
	target_.spawn( *this, param_, callSite_ );
}

/////////////////
// ProxyNotifyGen
/////////////////

ProxyNotifyGen::ProxyNotifyGen(
	AutoDispose< Generator > && inner,
	ThreadScheduler & target,
    void * callSite )
	: inner_( std::forward< AutoDispose< Generator >>( inner ))
	, target_( target )
    , callSite_( callSite )
{
}

void
ProxyNotifyGen::start( void )
{
    param_ = target_.defaultParam();
	call< &ProxyNotifyGen::notifyInner >( *inner_ );
}

bool
ProxyNotifyGen::next( void )
{
	return inner_->next();
}

void
ProxyNotifyGen::execute( void )
{
	TOOLS_ASSERT( !nextTask_ );
	if( !!error_ ) {
		finish();
	} else {
		AutoDispose< Error::Reference > e( std::move( error_ ));
		finish( *e );
	}
}

void
ProxyNotifyGen::notifyInner(
	Error * err )
{
	if( !!err ) {
		error_ = err->ref();
	}
	TOOLS_ASSERT( !nextTask_ );
	target_.spawn( *this, param_, callSite_ );
}

////////////////
// SyncGenerator
////////////////

SyncGenerator::SyncGenerator(
	Task *& taskRef )
	: taskRef_( taskRef )
{
}

void
SyncGenerator::generatorNext( void )
{
	Task * t = taskRef_;
	if( !!t ) {
		t->execute();
	}
}

//////////
// ForkReq
//////////

ForkReq::ForkReq(
	AutoDispose< Request > && inner )
	: inner_( std::move( ThreadScheduler::current().proxy( std::forward< AutoDispose< Request >>( inner ), impl::affinityInstance<Temporal>(), ThreadScheduler::current().defaultParam() )))
	, refs_( 2U )
	, started_( false )
{
}

void
ForkReq::dispose( void )
{
	if( !started_ && atomicDeref( &refs_ )) {
		// Dispose without start, though inner is still in-flight
		return;
	}
	TOOLS_ASSERT( refs_ == 0U );
	delete this;
}

void
ForkReq::start( void )
{
	TOOLS_ASSERT( refs_ != 0U );
	started_ = true;
	if( atomicDeref( &refs_ )) {
		return;
	}
	if( !!error_ ) {
		AutoDispose< Error::Reference > err( std::move( error_ ));
		finish( *err );
	} else {
		finish();
	}
}

void
ForkReq::notify(
	Error * err )
{
	if( !!err ) {
		error_ = err->ref();
	}
	if( atomicDeref( &refs_ )) {
		return;
	}
	if( !started_ ) {
		// disposed without start
		delete this;
		return;
	}
	if( !!error_ ) {
		finish( *error_ );
	} else {
		finish();
	}
}

void
ForkReq::begin( void )
{
	inner_->start( toCompletion< &ForkReq::notify >() );
}

//////////////////////
// VerifyStaticMonitor
//////////////////////

VerifyStaticMonitor::VerifyStaticMonitor(
    AutoDispose< Monitor > && inner,
    impl::ResourceSample const & res,
    StringId const & stereotype,
    Monitor::Policy policy )
    : inner_( std::move( inner ))
    , trace_( impl::resourceTraceBuild( res ))
    , stereotype_( /*!IsNullOrEmptyStringId( stereotype ) ? stereotype : "static"*/stereotype )  // TODO: fix the infinite initialization loop here
    , policy_( policy )
    , everRealTime_( false )
    , prevRealTime_( false )
    , prevTrace_( nullptr )
    , curRealTime_( false )
    , curTrace_( nullptr )
{
    // Make sure the thread-local data is initialized
    monitorInfoGlobal().get();
}

AutoDispose<>
VerifyStaticMonitor::enter(
    impl::ResourceSample const & res,
    bool tryOnly )
{
    // TODO: think about this
    // bool isRealTime = monitorVerifyIsRealTime();
    bool isRealTime = false;
    if( isRealTime ) {
        if( policy_ == Monitor::PolicyStrict ) {
            // TODO: log priority inversion, task lock taken on real-time thread
        } else {
            everRealTime_ = true;
        }
    } else if( everRealTime_ && ( policy_ != Monitor::PolicyAllowPriorityInversion )) {
        // TODO: log priority inversion
        everRealTime_ = false;
    }
    AutoDispose<> tryEnter( inner_->enter( res, true ));
    if( !tryEnter && tryOnly ) {
        // oh well
        return nullptr;
    }
    if( !!tryEnter ) {
        update( res );
        return std::move( tryEnter );
    }
    if( !isRealTime ) {
        tryEnter = std::move( inner_->enter( res ));
        update( res );
        return std::move( tryEnter );
    }
    if( !!curTrace_ && !curRealTime_ ) {
        // TODO: log priority inversion, real-time source {resourceTraceBuild( res, trace_ )->name()} blocked by task {curTrace_->name()}
    }
    // Measure contention
    uint64 startTime = impl::getHighResTime();
    tryEnter = std::move( inner_->enter( res ));
    update( res );
    uint64 endTime = impl::getHighResTime();
    if( ( startTime + realTimeContentionMax ) < endTime ) {
        // TODO: log, lock contention real-time source {curTrace_->name()} block for {(endTime - startTime)/TOOLS_NANOSECONDS_PER_MICROSECOND} us by {prevRealTime_ ? "real-time" : "task"} {!!prevTrace_ ? prevTrace_->name() : StringIdEmpty()}
    }
    return std::move( tryEnter );
}

bool
VerifyStaticMonitor::isAquired( void )
{
    return inner_->isAquired();
}

void
VerifyStaticMonitor::update(
    impl::ResourceSample const & res )
{
    prevRealTime_ = curRealTime_;
    prevTrace_ = curTrace_;
    // TODO: think about this
    // curRealTime_ = monitorVerifyIsRealTime();
    curRealTime_ = false;
    curTrace_ = impl::resourceTraceBuild( res, trace_ );
}

//////////
// FdrImpl
//////////

void
FdrImpl::memoryTracking(
    unsigned & numBufs,
    uint64 & perBufBytes )
{
    numBufs = 0;
    perBufBytes = 0;
}

//////////////////
// ThreadScheduler
//////////////////

ThreadScheduler &
ThreadScheduler::current( void )
{
	return *localScheduler_->current_;
}

AutoDispose< Request >
ThreadScheduler::fork(
	AutoDispose< Request > && inner,
    void * )
{
	AutoDispose< ForkReq > ret( new ForkReq( std::forward< AutoDispose< Request >>( inner )));
	ret->begin();
	return ret.release();
}

#include <tools/UnitTest.h>
#if TOOLS_UNIT_TEST
#include <tools/Environment.h>
#include <tools/Interface.h>
#include <tools/Timing.h>

//////////
// Testing
//////////

namespace {
    struct ThreadingTestImpl
        : Notifiable< ThreadingTestImpl >
    {
        ThreadingTestImpl(NoDispose< ConditionVar >, NoDispose< Monitor >, NoDispose< Monitor >);

        // support methods
        void trivialSupport( void );
        void singleThreadSupport( void );

        NoDispose< ConditionVar > cvar_;
        NoDispose< Monitor > cvarLock_;
        NoDispose< Monitor > lock_;
        unsigned volatile flag1_;
        unsigned volatile flag2_;
    };
};  // anonymous namespace

////////
// Tests
////////

TOOLS_TEST_CASE("threading.trivial", [](Test & test)
{
    AutoDispose<ConditionVar> cvar(conditionVarNew());
    AutoDispose<Monitor> cvarLock(cvar->monitorNew());
    AutoDispose<Monitor> lock(monitorNew());
    auto threading = test.environment().unmockNow<Threading>();
    TOOLS_ASSERTR(!!threading);
});

TOOLS_TEST_CASE("threading.trivial.create", "environment isn't disposing things correctly", [](Test & test)
{
    AutoDispose<ConditionVar> cvar(conditionVarNew());
    AutoDispose<Monitor> cvarLock(cvar->monitorNew());
    AutoDispose<Monitor> lock(monitorNew());
    auto timing = test.environment().unmockNow<Timing>();
    auto threading = test.environment().unmockNow<Threading>();
    TOOLS_ASSERTR(!!threading);
    ThreadingTestImpl state(cvar, cvarLock, lock);

    AutoDispose< Thread > thr(threading->fork("threadTesting", state.toThunk< &ThreadingTestImpl::trivialSupport >()));
    TOOLS_ASSERTR(!!thr);
    thr->waitSync();
    thr = threading->fork("threadTesting2", state.toThunk< &ThreadingTestImpl::trivialSupport >());
    TOOLS_ASSERTR(!!thr);
    AutoDispose< Request > done(thr->wait());
    TOOLS_ASSERTR(!!done);
    AutoDispose< Referenced< Error >::Reference > err(runRequestSynchronously(done));
    TOOLS_ASSERTR(!err);
    AutoDispose< Request > delay(timing->timer(250 * TOOLS_NANOSECONDS_PER_MILLISECOND));
    err = runRequestSynchronously(delay);
    TOOLS_ASSERTR(!err);
});

TOOLS_TEST_CASE("threading.trivial.single", "environment isn't disposing things correctly", [](Test & test)
{
    AutoDispose<ConditionVar> cvar(conditionVarNew());
    AutoDispose<Monitor> cvarLock(cvar->monitorNew());
    AutoDispose<Monitor> lock(monitorNew());
    auto timing = test.environment().unmockNow<Timing>();
    auto threading = test.environment().unmockNow<Threading>();
    TOOLS_ASSERTR(!!threading);
    ThreadingTestImpl state(cvar, cvarLock, lock);

    AutoDispose< Thread > thr(threading->fork("threadTesting", state.toThunk< &ThreadingTestImpl::singleThreadSupport >()));
    TOOLS_ASSERTR(!!thr);
    AutoDispose< Request > delay(timing->timer(250 * TOOLS_NANOSECONDS_PER_MILLISECOND));
    AutoDispose< Referenced< Error >::Reference > err(runRequestSynchronously(delay));
    TOOLS_ASSERTR(!err);
    while (!state.flag1_);  // get the thread started
    AutoDispose<> l_(lock->enter());
    TOOLS_ASSERTR(!state.flag2_);
    l_ = nullptr;
    delay = timing->timer(250 * TOOLS_NANOSECONDS_PER_MILLISECOND);
    err = runRequestSynchronously(delay);
    TOOLS_ASSERTR(!err);
    cvar->signal();
    thr->waitSync();
    TOOLS_ASSERTR(!!state.flag2_);
});

////////////////////
// ThreadingTestImpl
////////////////////

ThreadingTestImpl::ThreadingTestImpl(
    NoDispose<ConditionVar> cv,
    NoDispose<Monitor> cvl,
    NoDispose<Monitor> l)
    : cvar_( cv )
    , cvarLock_( cvl )
    , lock_( l )
    , flag1_( 0U )
    , flag2_( 0U )
{
}

void
ThreadingTestImpl::trivialSupport( void )
{
    // Do nothing
}

void
ThreadingTestImpl::singleThreadSupport( void )
{
    {
        AutoDispose<> l_( lock_->enter() );
        atomicIncrement( &flag1_ );
    }
    AutoDispose<> l_( cvarLock_->enter() );
    cvar_->wait();
    atomicIncrement( &flag2_ );
}

#endif // TOOLS_UNIT_TETS
