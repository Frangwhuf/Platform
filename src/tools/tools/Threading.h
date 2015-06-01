#pragma once

#include <tools/Concurrency.h>
#include <tools/Interface.h>
#include <tools/InterfaceTools.h>
#include <tools/Async.h>
#include <tools/String.h>
#include <tools/Tools.h>

#include <array>
#include <numeric>

namespace tools {
    namespace impl {
        TOOLS_API uint64 threadId( void );
        TOOLS_API uint32 cpuNumber( void );
        TOOLS_API void * threadLocalGet( void * );

        // Lightweight concurrency object used to allow threads to sleep and be woken by other threads.
        // All without priority inversions. Because this implementation has no Monitor/ConditionVariable
        // it is racey by nature.  For example:
        //
        //   * Thread 1 calls sleep(). However before that takes effect, it gets preempted.
        //   * Thread 2 calls wakeOne(). This does nothing as nothing is asleep yet.
        //   * Thread 1 gets to run again, completing sleep().
        //   * With no further wake*()s, thread 1 will sleep for the maximum timeout time.
        //
        // However we should implement our scheduler so that this kind of race is not a problem.
        //
        // The Linux implementation uses futex directly, and as such is not susceptible to priority
        // inversion.
        struct ThreadSleepVariable
            : Disposable
        {
            virtual void wakeOne( void ) = 0;
            // If the parameter is false, pick one thread and wake it (yes, like wakeOne()). If the
            // parameter is true, this will wake _all_ sleeping threads _and_ prevent future calls to
            // sleep to do nothing.
            virtual void wakeAll( bool = false ) = 0;
            virtual void sleep( uint64 ) = 0;
        };

        AutoDispose< ThreadSleepVariable > threadSleepVariableNew( void );
    };  // impl namespace

    // Thread-local objects are created on demand in each thread as they're touched
    // and then are cleaned up after the thread exits.  Unlike environment services,
    // thread-local services are global and identified by object instead of name for
    // faster dispatch.
    struct ThreadLocalFactory
    {
        // Create a new thread-local object.  This is called when it is retrieved for
        // the first time.
        virtual AutoDispose<> factory( void ** ) = 0;
    };

    namespace detail {
        template< typename ImplementationT >
        struct DefaultThreadLocalFactory
            : ThreadLocalFactory
        {
            tools::AutoDispose<> factory( void ** itf )
            {
                ImplementationT * ref;
                tools::AutoDispose<> ret( anyDisposableAllocNew< AllocStatic< Platform >>( &ref ));
                *itf = ref;
                return std::move( ret );
            }
            static ThreadLocalFactory & instance( void )
            {
                // A default instance
                static DefaultThreadLocalFactory< ImplementationT > global_;
                return global_;
            }
        };

        template< typename ImplementationT, typename FactoryT >
        struct FunctorThreadLocalFactory
            : ThreadLocalFactory
            , tools::StandardDisposable< FunctorThreadLocalFactory< ImplementationT, FactoryT >, tools::Disposable, tools::AllocStatic< Platform >>
        {
            FunctorThreadLocalFactory( FactoryT && factory, tools::AutoDispose<> * ref )
                : factory_( std::forward< FactoryT >( factory ))
            {
                *ref = this;
            }
            tools::AutoDispose<> factory( void ** itf )
            {
                ImplementationT * ref;
                tools::AutoDispose<> ret( factory_( &ref ));
                *itf = ref;
                return std::move( ret );
            }

            FactoryT factory_;
        };
    };  // detail namespace

    struct ThreadLocalHandle
        : Disposable
    {
        // Get the thread-local object, creating it if neccessary.
        virtual void * get( void ) = 0;
        TOOLS_FORCE_INLINE void * get( void * __restrict handle )
        {
            void * ret = tools::impl::threadLocalGet( handle );
            if( /* TOOLS_LIKELY */ !!ret ) {
                return ret;
            }
            return get();
        }
        TOOLS_FORCE_INLINE void * peek( void * __restrict handle )
        {
            return tools::impl::threadLocalGet( handle );
        }
    };

    TOOLS_API AutoDispose< ThreadLocalHandle > registerThreadLocalFactory( void **, ThreadLocalFactory & );

    template< typename ImplementationT, typename InterfaceT = ImplementationT >
    struct StandardThreadLocalHandle
    {
        static const tools::uint64 DEADLOCALID = 0xdeaddeadbeefbeefULL;

        StandardThreadLocalHandle( void )
            : handle_( registerThreadLocalFactory( &id_, tools::detail::DefaultThreadLocalFactory< ImplementationT >::instance() ))
        {
            // If this is global, then this constructor is running during static initialization. Objects
            // created by the factory may be disposed during the destructor for this type, which may run
            // somewhere in the static destruction order. The dispose may depend on objects that are
            // created during static initialization. We want to ensure that these objects are alive when
            // dispose is run.
            //
            // To accomplish this, we need to ensure that those objects are placed in the global static
            // initialization order before this object. Static destruction happens in reverse order of
            // static initialization. Here, we accomplish that by invoking a factory in the hopes that
            // it asserts all the static dependencies needed by dispose. Any dependence not explicityly
            // enumerated by dispose may not be available during destruction.
            //
            // Moral of the story: always assert all your dependencies!
            void * ignore;
            tools::AutoDispose<> toDisp( tools::detail::DefaultThreadLocalFactory< ImplementationT >::instance().factory( &ignore ));
        }

        // provided factory
        template< typename FactoryT >
        StandardThreadLocalHandle( FactoryT && factory )
            : handle_( registerThreadLocalFactory( &id_, *new tools::detail::FunctorThreadLocalFactory< ImplementationT, typename std::decay< FactoryT >::type >( std::forward< FactoryT >( factory ), &factory_ )))
        {
        }

        ~StandardThreadLocalHandle( void )
        {
            id_ = reinterpret_cast< void * >( DEADLOCALID );
        }

        TOOLS_FORCE_INLINE InterfaceT * operator->( void ) const {
            return static_cast< InterfaceT * >( handle_->get( id_ ) );
        }
        TOOLS_FORCE_INLINE InterfaceT & operator*( void ) const {
            return *static_cast< InterfaceT * >( handle_->get( id_ ) );
        }
        TOOLS_FORCE_INLINE InterfaceT * get( void ) const {
            return static_cast< InterfaceT * >( handle_->get( id_ ) );
        }
        TOOLS_FORCE_INLINE InterfaceT * peek( void ) const {
            return static_cast< InterfaceT * >( handle_->peek( id_ ) );
        }
        TOOLS_FORCE_INLINE bool operator!( void ) const {
            return !handle_->peek( id_ );
        }
        bool isDead( void ) const {
            return id_ == reinterpret_cast< void * >( DEADLOCALID );
        }

        tools::AutoDispose<> factory_;
        void * id_;
        tools::AutoDispose< ThreadLocalHandle > handle_;
    };

    // Abstract thread abstraction.
    struct Thread
        : Disposable
    {
        // Sleep until the thread exits.
        virtual void waitSync( void ) = 0;
        // Create a request that completes after the thread exits.
        virtual AutoDispose< Request > wait( void ) = 0;
    };

    // Thread factory service
    struct Threading
    {
        // Factory and start a new thread which calls the thunk.
        virtual AutoDispose< Thread > fork( StringId const &, Thunk const & ) = 0;
        // virtual void forkAnonymous( StringId const &, Thunk const & ) = 0;
        // Factory several threads that run a copy of the thunk on multiple cores.
        // Because the same state is passed to each thread, this should contain general
        // configuration.  The factory will start at most one thread per
        // core/hyperthread.  The returned request completes after all threads started
        // by this method have exited.
        virtual AutoDispose< Request > forkAll( StringId const &, Thunk const & ) = 0;
    };

	struct Task
	{
		Task() : nextTask_( nullptr ) {}

		// This gets called when the task is run.  The scheduler does not manage the
		// lifetime of the tasks, it only controlls when they get run.  In most important
		// ways a task represents a deferred function call.
		//
		// TODO: convert this to call operator
		virtual void execute( void ) = 0;

		Task * nextTask_;
        void * callSite_;
        uint64 queueTime_;
        uint64 threadId_;
	};

	struct ThreadScheduler
	{
        enum SchedulingPolicy {
            ThreadPolicyNormal,
            ThreadPolicyRealtimeLow,
            ThreadPolicyRealtimeMedium,
            ThreadPolicyRealtimeHigh,
        };
        enum SchedulingPriority {
            PriorityExistingWork,
            PriorityNewWork,
        };
        struct SpawnParam {
            explicit SpawnParam( void ) : priority_( PriorityNewWork ) {}
            explicit SpawnParam( SchedulingPriority p, StringId const & q ) : priority_( p ), queue_( q ) {}
            SpawnParam( SpawnParam const & c ) : priority_( c.priority_ ), queue_( c.queue_ ) {}

            TOOLS_FORCE_INLINE SpawnParam & operator=( SpawnParam const & c )
            {
                priority_ = c.priority_;
                queue_ = c.queue_;
                return *this;
            }

            SchedulingPriority priority_;
            StringId queue_;
        };

		// Enqueue a Task to run on some thread managed by this scheduler.
		// A specific queue can be specified to enqueue the Task into. If you
        // care about knowing when the Task finishes, use fork.
		virtual void spawn( Task &, SpawnParam const &, void * = nullptr ) = 0;

		// Run the Task on many threads.  The actual number is undefined.
		virtual AutoDispose< Request > spawnAll( Task & ) = 0;

		// Start some number of Tasks, synchronizing their completion
		// on getting a nullptr from the Generator.  Disposing the Generator
		// detaches outstanding Tasks.
		virtual AutoDispose< Generator > fork( Task * & ) = 0;

		// Wrap a request so that it starts in this scheduler.
		virtual AutoDispose< Request > proxy( AutoDispose< Request > &&, Affinity &, SpawnParam const &, void * ) = 0;
		TOOLS_FORCE_INLINE AutoDispose< Request > proxy( AutoDispose< Request > && inner, Affinity & affinity, SpawnParam const & param )
		{
			return this->proxy( std::move( inner ), affinity, param, TOOLS_RETURN_ADDRESS() );
		}

		// Ensure that any asynchronous operation (Request completion)
		// completes on this scheduler.
		virtual AutoDispose< Request > bind( AutoDispose< Request > &&, void * = nullptr ) = 0;
		TOOLS_FORCE_INLINE AutoDispose< Request > bind( AutoDispose< Request > & inner )
		{
			return bind( std::move( inner ), TOOLS_RETURN_ADDRESS() );
		}
		virtual AutoDispose< Generator > bind( AutoDispose< Generator > &&, void * = nullptr ) = 0;
		TOOLS_FORCE_INLINE AutoDispose< Generator > bind( AutoDispose< Generator > & inner )
		{
			return bind( std::move( inner ), TOOLS_RETURN_ADDRESS() );
		}

        // Return the default spawn param for a task. The default param may be a function of the currently
        // running task. This permits tasks to inherit queues/priorities.
        virtual SpawnParam defaultParam( void ) = 0;

		// Return access to the scheduler running this thread.
		static TOOLS_API ThreadScheduler & current( void );

        // Get the sync scheduler
        static TOOLS_API ThreadScheduler & sync( void );

        // For diagnostics
        static TOOLS_API StringId envRole( void );

        static TOOLS_API bool currentIsSync( void );

		// Create a Request that will notify only after the passed in
		// Request has notified.  If the passed in Request completes
		// before the returned Request is started, that Request will
		// complete synchronously
		static TOOLS_API AutoDispose< Request > fork( AutoDispose< Request > &&, void * = nullptr );
		static inline AutoDispose< Request > fork( AutoDispose< Request > & inner )
		{
			return ThreadScheduler::fork( std::move( inner ), TOOLS_RETURN_ADDRESS() );
		}
	};

	struct TaskScheduler : SpecifyService< ThreadScheduler > {};

    struct ScalableCounter
    {
        struct Data
        {
            Data( void ) : count_( 0ULL ) {}

            uint64 volatile count_;
            uint64 pad_[ 7 ];  // fill out cache line
        };

        ScalableCounter( void ) {}

        inline void operator+=( uint64 delta )
        {
            unsigned id = tools::impl::cpuNumber();
            unsigned idx = id + 1;  // leave entry 0 empty to guarentee no false sharing.  See note above.
            TOOLS_ASSERT( idx < ( vec_.size() - 1 ));
            // Use atomics, we might migrate between looking up the CPU number and indexing into the array.
            // This isn't so bad as most of the time we will still be there, so there is no contention.
            tools::atomicAdd( &vec_[ idx ].count_, delta );
        }
        inline void operator-=( uint64 delta )
        {
            sint64 wrapped = static_cast< sint64 >( delta );
            operator+=( static_cast< uint64 >( -wrapped ));
        }
        inline operator uint64( void )
        {
            // This is very racy. But it is close enough to be very useful.
            TOOLS_ASSERT( vec_.size() >= 2 );
            // The first and last entry in the vector are unused (to prevent false sharing, see above), so
            // skip them.
            return std::accumulate( vec_.begin() + 1, vec_.end() - 1, static_cast< uint64 >( 0 ), []( uint64 left, Data const & right) { return left + tools::atomicRead( &right.count_ ); });
        }

    private:
        static const unsigned maxNumCpus = 64;

        ScalableCounter( ScalableCounter const & ) { TOOLS_ASSERT( !"Copy not allowed" ); }
        inline ScalableCounter & operator=( ScalableCounter const & ) { TOOLS_ASSERT( !"Assignment not allowed" ); return *this; }

        std::array< Data, maxNumCpus + 2 > vec_;
    };

    namespace impl {
        // Per-thread circular buffers for logging low priority messages
        struct Fdr {
            virtual void memoryTracking( unsigned &, uint64 & ) = 0;
        };

        TOOLS_API Fdr * globalFdr( void );
    };  // detail namespace
}; // namespace tools
