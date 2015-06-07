#include "toolsprecompiled.h"

#include <tools/Algorithms.h>
#include <tools/Concurrency.h>
#include <tools/InterfaceTools.h>
#include <tools/Threading.h>
#include <tools/Timing.h>

#include "TimingImpl.h"

#include <algorithm>
#include <map>
#ifdef WINDOWS_PLATFORM
#  pragma warning( disable : 4365 )
#  pragma warning( disable : 4371 )
#  pragma warning( disable : 4571 )
#  pragma warning( disable : 4619 )
#endif // WINDOWS_PLATFORM
#include <boost/intrusive/list.hpp>
#include <boost/format.hpp>
#ifdef WINDOWS_PLATFORM
#  pragma warning( default : 4365 )
#  pragma warning( default : 4371 )
#  pragma warning( disable : 4571 )
#  pragma warning( default : 4619 )
#endif // WINDOWS_PLATFORM
#ifdef WINDOWS_PLATFORM
#  pragma warning( disable : 4200 )
#endif // WINDOWS_PLATFORM

using namespace tools;

//static void * verifyHeapMap( Heap &, impl::ResourceTrace *, bool, size_t,
//    impl::ResourceSample const &, size_t );
//static void verifyHeapUnmap( Heap &, void * );

namespace {
    enum : size_t {
        temporalSlabSizeSmall = 32U * 1024U,  // [0, 256)
        temporalSlabSizeMedium = 256U * 1024U,  // [256, 16384)
        temporalSlabSizeLarge = 2U * 1024U * 1024U,  // [16384, 1MB)
    };

    enum AlignModel
    {
        alignModelTiny, // Sub cache-line alignment.  Either word or x-word aligned.
        alignModelLine, // Cache-line alignment.  Any object larger than a half cache-line.
        alignModelPage, // Page based alignment.
    };

    enum AlignScale
    {
        alignScaleLine,   // manage at cache-line scale
        alignScalePage,   // manage at page scale.  This may be alignModelLine if the
                          // allocation is long and ragged.
        alignScaleUnique, // Large enough to be uniquely managed
    };

    struct AlignSpec
    {
        size_t size_;
        size_t phase_;
        // Gross dimensions for allocation
        AlignModel model_;
        AlignScale scale_;
        // Proposed alignment units from the payload size.  Unlike the scale, this may
        // specify exact allocation sizes.
        size_t alignBytes_;
        // Minimum number of bytes to request in order to guarantee enough sliding room
        // given an 8-byte header (and the scale allocator).
        size_t alignAlloc_;
        // Number of bytes to advance during placement.  This may be longer than size_
        // in order to force bank rotation.
        size_t placeBytes_;
    };

    TOOLS_FORCE_INLINE uint32 defineHashAny( AlignSpec const & spec, uint32 initial )
    {
        return impl::hashMix( spec.phase_, impl::hashMix( spec.size_, initial ));
    }

    // There are three binary masters.  They are at 1MB, 512KB, and 256KB.  But, it's
    // possible they aren't all present if they're parented off to another service provider.
    //
    // Note: These are in descending order so they'll destruct from smallest to largest.
    enum BinaryMasterSize
    {
        binaryMasterSize1024,
        binaryMasterSize512,
        binaryMasterSize256,
        binaryMasterSize128,
        binaryMasterSize64,
        binaryMasterSize32,
        binaryMasterSizeMax
    };

    // Helper structure embedded in the block, when it's freed, for keeping it in the list.
    struct BinaryBlock
    {
        BinaryBlock * next_;
        // Base address for the moity block (this address is the free-block itself).
        void * base_;
    };

    // The master keeps an array of buckets for recoalescing binary pool blocks with their
    // peers.
    struct BinaryPoolMaster
        : Pool
    {
        enum : size_t {
            // Array of blocks - this should be approximately proportional to the amount of concurrency.
            tableSize = 128,
        };

        BinaryPoolMaster( Pool &, impl::ResourceSample const & );
        ~BinaryPoolMaster( void );

        // Pool
        Pool::Desc const & describe( void );
        void * map( void );
        void unmap( void * );

        // local methods
        BinaryBlock * extract( size_t * );
        void insertUnique( size_t, BinaryBlock * );
        static size_t pointerBucket( void * );
        static void * innerMap( BinaryBlock **, Pool *, size_t );
        
        // Internal source of buffers
        Pool * inner_;
        // My desc
        Pool::Desc desc_;
        // Un-reunited moities
        BinaryBlock * volatile blocks_[ tableSize ];
    };

    template< typename ElementT, size_t TableSize >
    struct HashAccumTable
    {
        // Do a separate init to keep this POD
        inline void
        clear( void )
        {
            TOOLS_ASSERT( tools::isPow2( TableSize ));
            std::fill_n( buckets_, TableSize, static_cast< ElementT * >( nullptr ));
        }
        // Lookup an element by its key
        template< typename KeyT >
        inline ElementT *
        peek(
            ElementT * item,
            KeyT const & key )
        {
            for( ; !!item; item=item->next_ ) {
                if( *item == key ) {
                    break;
                }
            }
            return item;
        }
        template< typename KeyT >
        inline ElementT *
        peek(
            uint32 hash,
            KeyT const & key )
        {
            return peek( buckets_[ hash & (TableSize - 1U )], key );
        }
        inline ElementT *
        insert(
            uint32 hash,
            ElementT * elem )
        {
            ElementT * volatile * bucket = buckets_ + ( hash & ( TableSize - 1U ));
            ElementT * prev;
            do {
                prev = *bucket;
                ElementT * existing = peek( prev, *elem );
                if( !!existing ) {
                    return existing;
                }
                elem->next_ = prev;
            } while( atomicCas( bucket, prev, elem ) != prev );
            // We correctly inserted what we hoped to
            return elem;
        }
        // Return a flat, linked array and remove all of the array contents
        inline ElementT *
        detach( void )
        {
            ElementT * head;
            ElementT ** tail = &head;
            for( size_t i=0; i!=TableSize; ++i ) {
                for( ElementT * j=const_cast< ElementT * >( buckets_[ i ]); !!j; j=j->next_ ) {
                    *tail = j;
                    tail = &j->next_;
                }
                // Reset the bucket pointers
                buckets_[ i ] = nullptr;
            }
            *tail = nullptr;
            return head;
        }
        // Return a flat, linked array based on a condition
        template< typename PredicateT >
        inline ElementT *
        detachIf(
            PredicateT const & pred )
        {
            ElementT * head;
            ElementT ** tail = &head;
            for( size_t i=0; i!=TableSize; ++i ) {
                ElementT ** bucketTail = const_cast< ElementT ** >( buckets_ + i );
                ElementT * next;
                for( ElementT * j = *bucketTail; !!j; j=next ) {
                    next = j->next_;
                    if( pred(*j) ) {
                        *tail = j;
                        tail = &j->next_;
                    } else {
                        *bucketTail = j;
                        bucketTail = &j->next_;
                    }
                }
                // Reset the bucket pointers
                *bucketTail = nullptr;
            }
            *tail = nullptr;
            return head;
        }

        ElementT * volatile buckets_[ TableSize ];
    };

    struct ProxyPool
        : Pool
        , AllocStatic< Platform >
    {
        ProxyPool( Pool &, size_t );

        // Pool
        Pool::Desc const & describe( void );
        void * map( void );
        void unmap( void * );

        // local methods
        inline bool operator==( ProxyPool const & ) const;
        inline bool operator==( AlignSpec const & ) const;
        inline size_t phaseDiff( void ) const;

        // Hash chain information
        ProxyPool * next_;
        // New desciptor (the inner pool will have the same alignment and size).  When
        // there's no phase, no proxy is required.
        Pool::Desc desc_;
        Pool * inner_;
    };

    // The common affinity implementation is a heap allocator that classifies and aligns
    // allocations as well as a mechanism for pool lifetime control.  Derived classese
    // must implement the binding and forking policy.
    struct AffinityInherentBase
        : Affinity
    {
        struct PoolInstance
            : AllocStatic< Platform >
        {
#ifdef WINDOWS_PLATFORM
            PoolInstance( void );
        private:
            PoolInstance( PoolInstance const & );
            PoolInstance & operator=( PoolInstance const & );
        public:
#endif // WINDOWS_PLATFORM
#ifdef UNIX_PLATFORM
            PoolInstance( void ) = default;
            PoolInstance( PoolInstance const & ) = delete;
            PoolInstance & operator=( PoolInstance const & ) = delete;
#endif // UNIX_PLATOFRM
            PoolInstance( PoolInstance && );

            PoolInstance & operator=( PoolInstance && );
            bool operator==( PoolInstance const & ) const;
            bool operator==( AlignSpec const & ) const;
            bool operator<( PoolInstance const & right ) const;

            PoolInstance * next_;
            // Internal pool description
            size_t size_;
            size_t phase_;
            Pool * pool_;
            AutoDispose<> poolDispose_;
        };

        struct IsNonUniquePool
            : std::unary_function< PoolInstance const &, bool >
        {
            inline bool operator()( PoolInstance const & pool ) const
            {
                return pool.size_ < ( 256U * 1024U );
            }
        };

        explicit AffinityInherentBase( Affinity & );
        ~AffinityInherentBase( void );

        // Affinity
        Pool & pool( size_t, impl::ResourceSample const &, size_t );

        // Heap
        void * map( size_t, impl::ResourceSample const &, size_t );
        void unmap( void * );

        // local methods

        // This must return a new pool, however the disposable is optional and depends on
        // if the pool needs to be cleaned up.
        virtual AutoDispose<> newPool( Pool **, AlignSpec const & ) = 0;
        static inline uint32 hashAlignSpec( AlignSpec const & );

        template< size_t tableSize >
        Pool &
        findPool(
            HashAccumTable< PoolInstance, tableSize > & poolSet,
            AlignSpec const & spec )
        {
            uint32 hash = hashAlignSpec( spec );
            PoolInstance * inst = poolSet.peek( hash, spec );
            if( !!inst ) {
                return *inst->pool_;
            }
            std::auto_ptr< PoolInstance > next( new PoolInstance );
            AutoDispose<> poolDispose( std::move( newPool( &next->pool_, spec )));
            next->size_ = spec.size_;
            next->phase_ = spec.phase_;
            next->poolDispose_ = std::move( poolDispose );
            PoolInstance * inserted = poolSet.insert( hash, next.get() );
            if( inserted == next.get() ) {
                // Our instance was inserted
                next.release();
            }
            return *inserted->pool_;
        }
        // Normalize some of the parameters to better match the set of pools we are
        // interested in keeping
        static inline void normalize( AlignSpec * );
        static inline void flatten( AlignSpec * );
        // Pool size hierarchy.  This is so that pools are placed within other pools of
        // appropriate efficiency.  Parent pools are always power-of-two aligned and in
        // the page or unique range.  The 2MB pool is terminal for all pools.  At or above
        // the unique range, this enters a doubling pattern.  Below, the range pattern is
        // much steeper.
        static inline bool parentPoolOf( size_t *, AlignScale, size_t poolSize );
        // Delete a linked list of pools that were originally hash table entries (see detach()).
        // Also, dispose() the pools, and so so in sorted order from smaller sizes toward larger.
        static inline void clearInstances( PoolInstance * );
        static uint32 alignSpecProxyHash( AlignSpec const & );
        Pool & findProxy( AlignSpec const & );
        void shutdownNonUniquePools( void );
        void shutdownPools( void );

        // Inner allocator (likely 'Platform') is used for small allocations and allocations > 2MB.
        Affinity * inner_;
        // Pool instances for page/unique scale allocation.  These are kept seperately
        // from the small pools and a little wider because we failback here during map().
        HashAccumTable< PoolInstance, 32 > pagePools_;
        // A smaller table of line-oriented pools.  Proxy pools are kept here to correct
        // for varients on user pools that effectively only vary in their alignment
        // requirements, but otherwise exist nicely in the same space..
        // 
        // We don't expect to search this table often, so it's probably alright to be small.
        HashAccumTable< PoolInstance, 16 > linePools_;
        // Proxy pools just perform spot adjustment within an already aligned parent.
        HashAccumTable< ProxyPool, 16 > proxyPools_;
    };

    struct AffinityInherentMaster;

    // The thread local type stars out a little lame; just forward almost everything
    // back to the master and/or root.
    struct AffinityInherentThreadLocal
        : AffinityInherentBase
    {
        AffinityInherentThreadLocal( AffinityInherentMaster &, Affinity &, bool );
        ~AffinityInherentThreadLocal( void );

        // AffinityInherentBase
        AutoDispose<> newPool( Pool **, AlignSpec const & );

        // Affinity
        Affinity & bind( void );
        AutoDispose<> fork( Affinity **, impl::ResourceSample const & );

        Affinity * root_;
        AffinityInherentMaster * master_;
        // The unique pools are kept local to provide some capacitance at this level and
        // ease the competition a little.
        Pool * uniquePools_[ binaryMasterSizeMax ];
    };

    // The master affinity represents a multi-threaded affinity over several clones.  It
    // hosts the shared state (binary masters) and implements a fan-out pool that redirects
    // to thread-local state.
    struct AffinityInherentMaster
        : AffinityInherentBase
        , StandardDisposable< AffinityInherentMaster, Disposable, AllocStatic< Platform >>
    {
        struct FanoutPool
            : Pool
            , StandardDisposable< FanoutPool, Disposable, AllocStatic< Platform >>
        {
            FanoutPool( AffinityInherentMaster &, StandardThreadLocalHandle< AffinityInherentThreadLocal > &, BinaryMasterSize );
            // Pool
            Pool::Desc const & describe( void );
            void * map( void );
            void unmap( void * );
            // local methods
            Pool & localPool( void );
            Pool & peekLocalPool( void );

            AffinityInherentMaster * parent_;
            StandardThreadLocalHandle< AffinityInherentThreadLocal > * threadLocal_;
            BinaryMasterSize idx_;
        };

        explicit AffinityInherentMaster( Affinity &, Affinity *,
            impl::ResourceSample const &, size_t, bool );
        ~AffinityInherentMaster( void );

        // Affinity
        Affinity & bind( void );
        AutoDispose<> fork( Affinity **, impl::ResourceSample const & );

        // local methods
        BinaryPoolMaster * binaryMasterOf( size_t );
        AutoDispose<> newPool( Pool **, AlignSpec const & );

        // Root for building super-block sets, the actual root will point to itself.
        Affinity * root_;
        impl::ResourceSample sample_;
        size_t poolMaxBuffer_;
        atomicPointer< BinaryPoolMaster > masters_[ binaryMasterSizeMax ];
        // There is a specific parent for threaded pools
        Pool * poolParent_;
        AutoDispose<> poolParentOwner_;
        // Retreive the thread local affinity so we can tunnel through a request for a parent pool
        StandardThreadLocalHandle< AffinityInherentThreadLocal > threadLocal_;
    };

    struct HeapPrefix
    {
        impl::ResourceTrace * trace_;  // memory usage tracking
#ifdef TOOLS_DEBUG
        uint32 size_;
        uint32 phase_;
        // for detecting memory smashing
        uint64 checkin1_;
        uint64 checkin2_;
#endif // TOOLS_DEBUG
    };

#ifdef TOOLS_DEBUG
    struct HeapSuffix
    {
        uint64 checkout1_;
        uint64 checkout2_;
    };
#endif // TOOLS_DEBUG

    static inline bool
    hasSuffix( size_t size ) {
        return ( ( size < 4096 ) || !!( size & ( 4096U - 1U )));
    }

    struct IntervalCounter
    {
        IntervalCounter( void ) : value_( 0 ) {}

        unsigned bumpUp( void ) {
            return ++value_;
        }

        unsigned value_;
    };

    static ScalableCounter totalTrackedMemory;

    static ScalableCounter &
    getTotalTrackedMemory( void ) {
        return totalTrackedMemory;
    }

    template< typename InterfaceT >
    struct VerifyHeapBase
        : InterfaceT
    {
        VerifyHeapBase( unsigned trackingInterval, InterfaceT & inner, impl::ResourceTrace * target, bool checkRt = false )
            : inner_( &inner )
            , target_( target )
            , checkRt_( checkRt )
            , trackingInterval_( roundUpPow2( trackingInterval, 2 ))
        {
            TOOLS_ASSERT( trackingInterval_ > 0 );
            TOOLS_ASSERT( isPow2( trackingInterval_ ));
        }

        // Heap

        // Allocate, using the inner allocator, a chunk of memory containing HeapPrefix, then phase +
        // userData, then HeapSuffix.  Return a pointer to the phase portion.  Can't return a pointer
        // to userData, since without the phase, we can't obtain the HeapPrefix.
        void *
        map(
            size_t size,
            impl::ResourceSample const & sample,
            size_t phase )
        {
            HeapPrefix * prefix;
            uint8 * body;
            size_t userSize = size - phase;
            if( hasSuffix( userSize )) {
                prefix = static_cast< HeapPrefix * >( inner_->map( size + sizeof( HeapPrefix )
#ifdef TOOLS_DEBUG
                    + sizeof( HeapSuffix )  // has non-zero size even if it's empty
#endif // TOOLS_DEBUG
                    , sample, phase + sizeof( HeapPrefix )));
                if( !prefix ) {
                    impl::outOfMemoryDie();
                }
                body = reinterpret_cast< uint8 * >( prefix + 1 );
#ifdef TOOLS_DEBUG
                HeapSuffix * suffix = reinterpret_cast< HeapSuffix * >( body + size );
                suffix->checkout1_ = 0xCACACACACACACACA;
                suffix->checkout2_ = 0x1010101010101010;
#endif // TOOLS_DEBUG
            } else {
                prefix = static_cast< HeapPrefix * >( inner_->map( size + sizeof( HeapPrefix ), sample, phase + sizeof( HeapPrefix )));
                if( !prefix ) {
                    impl::outOfMemoryDie();
                }
                body = reinterpret_cast< uint8 * >( prefix + 1 );
            }
#ifdef TOOLS_DEBUG
            // To save space, we're going to fit size and phase into 32 bit fields, thus the following:
            TOOLS_ASSERT( size < 0xFFFFFFFF );
            TOOLS_ASSERT( phase < 0xFFFFFFFF );
            prefix->size_ = static_cast< uint32 >( size );  // includes user + phase
            prefix->phase_ = static_cast< uint32 >( phase );
            prefix->checkin1_ = 0xABABABABABABABAB;
            prefix->checkin2_ = 0xBABABABABABABABA;
            if( impl::memoryPoison() ) {
                size_t poisonBytes = std::min( size, static_cast< size_t >( 65536U ));
                std::fill_n( body, poisonBytes, static_cast< uint8 >( '\xC4' ));
            }
#endif // TOOLS_DEBUG
            // Memory usage tracking.
            unsigned newval = intervalCounter_->bumpUp();
            if( ( newval & ( trackingInterval_ - 1 )) == 0 ) {
                prefix->trace_ = impl::resourceTraceBuild( trackingInterval_, sample, target_ );
                prefix->trace_->inc();
                getTotalTrackedMemory() += prefix->trace_->size() * trackingInterval_;
            } else {
                prefix->trace_ = nullptr;
            }
            return body;
        }

        // Unmaps a pointer preceeded by HeapPrefix.  Checks for memory corruption by detecting smashes
        // of prefix->checkin1/2 and suffix->checkout1/2.
        void
        unmap(
            void * site )
        {
            HeapPrefix * prefix = static_cast< HeapPrefix * >( site ) - 1;
            // First check for over/under runs
            TOOLS_ASSERT( prefix->checkin1_ == 0xABABABABABABABAB );
            TOOLS_ASSERT( prefix->checkin2_ == 0xBABABABABABABABA );
#ifdef TOOLS_DEBUG
            size_t userSize = prefix->size_ - prefix->phase_;
            if( hasSuffix( userSize )) {
                HeapSuffix * suffix = reinterpret_cast< HeapSuffix * >( static_cast< uint8 * >( site ) + prefix->size_ );
                TOOLS_ASSERT( suffix->checkout1_ == 0xCACACACACACACACA );
                TOOLS_ASSERT( suffix->checkout2_ == 0x1010101010101010 );
            }
            if( impl::memoryPoison() ) {
                // Check for double-free (i.e.: already all 0xD4).  First clause copied from VerifyPoolBase::unmap()s
                // check.
                size_t poisonBytes = std::min( prefix->size_, static_cast< uint32 >( 65536U ));
                TOOLS_ASSERT( "double free" && ( ( userSize <= sizeof( void * )) || !impl::regionIsUnmapped( static_cast< uint8 * >( site ), poisonBytes )));
                // The following is an interesting assert, but may be too string.  (Assert fails if the
                // object has >= 50% of 0xD4 bytes).
                // TOOLS_ASSERT( "possible double free" && !impl::regionIsPartiallyUnmapped( static_cast< uint8 * >( site ), prefix->size_ ));
                std::fill_n( static_cast< uint8 * >( site ), poisonBytes, static_cast< uint8 >( '\xD4' ));
            }
#endif // TOOLS_DEBUG
            if( !!prefix->trace_ ) {
                // Memory tracking was done on this object at map() time.
                getTotalTrackedMemory() -= prefix->trace_->size() * trackingInterval_;
                prefix->trace_->dec();
            }
            inner_->unmap( prefix );
        }

        InterfaceT * inner_;
        impl::ResourceTrace * target_;
        bool checkRt_;
        unsigned trackingInterval_;  // must be a power of 2
        StandardThreadLocalHandle< IntervalCounter > intervalCounter_;
    };

    // A wrapper around an inner pool allocator which adds memory tracking and 0xc4/0xd4 clearing/double-free
    // detection.
    struct VerifyPoolBase
        : Pool
    {
        static uint8 const verifyHeapAlloc = static_cast< uint8 >( '\xC4' );
        static uint8 const verifyHeapFree = static_cast< uint8 >( '\xD4' );
        static uint32 const verifyHeapFree4 = static_cast< uint32 >( 0xD4D4D4D4 );
        static uint64 const verifyHeapFree8 = static_cast< uint64 >( 0xD4D4D4D4D4D4D4D4 );

        VerifyPoolBase( unsigned, Pool &, impl::ResourceTrace *, size_t = 1U );

        // Pool
        Desc const & describe( void );
        void * map( void );
        void unmap( void * );

        Pool * inner_;
        Desc innerDesc_;
        impl::ResourceTrace * mapTrace_;
        size_t mapTraceElements_;  // number of times to inc/dec the trace
        unsigned const trackingInterval_;
    };

    struct VerifyPool
        : VerifyPoolBase
        , StandardDisposable< VerifyPool, Disposable, AllocStatic< Platform >>
    {
        VerifyPool( unsigned, Pool &, impl::ResourceTrace *, size_t );
    };

    // A wrapper around Affinity, which tracks memory usage as well as detecting some types of corruptions.
    // Pool() is implemented here; its map() is implemented in VerifyPoolBase.  Heap-style map() and unmap()
    // are implemented by VerifyHeapBase.
    struct VerifyAffinityBase
        : VerifyHeapBase< Affinity >
    {
        struct PoolId
        {
            inline bool operator<( PoolId const & r ) const {
                if( size_ == r.size_ ) {
                    if( phase_ == r.phase_ ) {
                        return ( trace_ < r.trace_ );
                    }
                    return ( phase_ < r.phase_ );
                }
                return ( size_ < r.size_ );
            }

            size_t size_;
            size_t phase_;
            impl::ResourceTrace * trace_;
        };

        // Don't use an explicit allocator here because it will cause an infinite loop
        // (cannot allocate from the affinity we are trying to verify).
        typedef std::map< PoolId, VerifyPoolBase > PoolMap;

        // Assume the target already contains the allocation function for "this"
        VerifyAffinityBase( unsigned, Affinity &, impl::ResourceTrace *, unsigned volatile *, bool = false );
        ~VerifyAffinityBase( void );

        AutoDispose<> fork( Affinity **, impl::ResourceSample const & );
        Pool & pool( size_t, impl::ResourceSample const &, size_t );

        PoolMap pools_;
        AutoDispose< Monitor > poolsLock_;
        unsigned volatile forks_;
        unsigned volatile * forkRefs_;
    };

    // This has been bound to a particular thread, and will check all of the entry points
    // against the original thread ID.
    struct VerifyAffinityBound
        : VerifyAffinityBase
    {
        VerifyAffinityBound( unsigned, Affinity &, impl::ResourceTrace * );

        // Affinity
        Affinity & bind( void );
    };

    struct VerifyAffinity
        : VerifyAffinityBase
        , StandardDisposable< VerifyAffinity, Disposable, AllocStatic< Platform >>
    {
        VerifyAffinity( unsigned, Affinity &, Disposable *, impl::ResourceTrace *, unsigned volatile *, bool = false );

        // Affinity
        Affinity & bind( void );

        StandardThreadLocalHandle< VerifyAffinityBound > threadLocal_;
        AutoDispose<> innerDispose_;
    };

    // VmemPoolUniqueAddr places every allocation at a unique address, only works for line scale allocations.
    // Because this is already part of a pool we'll do a little bit of local locking.
    struct VmemPoolUniqueAddr
        : Pool
        , StandardDisposable< VmemPoolUniqueAddr, Disposable, AllocStatic< Platform >>
    {
        static size_t const vmemRegionSize = 65536U;
        static size_t const vmemPageSize = 4096U;
        static size_t const vmemAlignSize = 64U;

        struct RegionHead
        {
            unsigned volatile refs_;
        };

        VmemPoolUniqueAddr( impl::ResourceSample const &, size_t, size_t, unsigned );
        ~VmemPoolUniqueAddr( void );

        // Pool
        Pool::Desc const & describe( void );
        void * map( void );
        void unmap( void * );

        static inline void * pageOf( void * site ) {
            return reinterpret_cast< void * >( reinterpret_cast< size_t >( site ) & ~static_cast< size_t >( vmemPageSize - 1U ));
        }
        static inline RegionHead * regionOf( void * site ) {
            return *reinterpret_cast< RegionHead ** >( pageOf( site ));
        }
        static void unmapAlloc( void * );

        AutoDispose< Monitor > lock_;
        RegionHead * regionBegin_;  // start of the region we're mapping
        uint8 * regionEnd_;  // end of the region
        uint8 * regionNext_;  // next mapping location
        uint8 * reserveNext_;
        Pool::Desc desc_;
        AlignSpec spec_;
    };

    struct HeapHugeImpl
        : Heap
    {
        static size_t const pageSize = 4096U;  // should always be a power of 2
        static unsigned const interval = 1;

        struct Prefix
        {
            impl::ResourceTrace * trace_;
            void * mmappedAddr_;
            size_t mmappedSize_;
        };

        HeapHugeImpl( void );

        // Heap
        void * map( size_t, impl::ResourceSample const &, size_t );
        void unmap( void * );

        impl::ResourceTrace * parentTrace_;
    };

    struct MemoryDumpTask
        : Task
        , StandardDisposable< MemoryDumpTask, Disposable, AllocStatic< Platform >>
    {
        enum : unsigned {
            intervalSec = 30,  // TODO: make configuration
            intervalSecMin = 5,  // TODO: make configuration
        };

        MemoryDumpTask( ThreadScheduler & );

        // Task
        void execute( void );

        // local methods
        void checkUsage( uint64 );
        void tryLaunch( impl::ResourceTraceDumpPhase );

        unsigned volatile nesting_;
        unsigned pad_[ 7 ];  // prevent false sharing of nesting_
        ThreadScheduler & scheduler_;
        uint64 dumpIntervalNs_;
        static uint64 lastDumpNs_;
        static uint64 lastDumpBytes_;
        impl::ResourceTraceDumpPhase phase_;
        std::vector< impl::ResourceTraceSum, AllocatorAffinity< impl::ResourceTraceSum, Platform >> storage_;
    };

    // Single-threaded fork.  All pools are created single-threaded against a parent
    // multi-threaded base.
    struct AffinityInherentForkBound
        : AffinityInherentBase
        , StandardDisposable< AffinityInherentForkBound, Disposable, AllocStatic< Platform >>
    {
        AffinityInherentForkBound( Affinity &, impl::ResourceSample const &, Affinity & );

        // AffinityInherentBase
        AutoDispose<> newPool( Pool **, AlignSpec const & );

        // Affinity
        Affinity & bind( void );
        AutoDispose<> fork( Affinity **, impl::ResourceSample const & );

        // The multi-threaded root affinity
        Affinity * root_;
        impl::ResourceSample sample_;
    };

    // A per-thread buffer to stave off high frequency allocations and frees that might
    // occur on each thread.  The depth of the cache is variable, but can be reasonably
    // set to match the ratio from the base allocation.
    //
    // Pair matches are committed to the inner pool as soon as they're discovered.
    struct BinaryPoolThreadBuffer
        : Pool
        , StandardDisposable< BinaryPoolThreadBuffer, Disposable, AllocTail< BinaryBlock *, Platform >>
    {
        BinaryPoolThreadBuffer( BinaryPoolMaster * );
        ~BinaryPoolThreadBuffer( void );

        // Pool
        Pool::Desc const & describe( void );
        void * map( void );
        void unmap( void * );

        // Pool we're buffereing for
        BinaryPoolMaster * master_;
        // Copies of fields from the master to avoid false sharing
        Pool * inner_;
        size_t parentSize_;
        // The second half of a recent map
        BinaryBlock * mapOf_;
        // Phase to help prevent conflicts when scanning the parent.
        size_t threadBias_;
        // Buffered unmaps that were not matched on this thread.  They remain here until
        // either we overflow or they find their mapping peer.
        unsigned unmappedUsed_;
        unsigned unmappedMax_;
        BinaryBlock * unmapped_[];
    };

    TOOLS_FORCE_INLINE uint32 defineHashAny( BinaryPoolThreadBuffer const &, uint32 initial )
    {
        return impl::hashMix( impl::threadId(), initial );
    }

    // BufferingPool hangs onto pages for a particular client (e.g.: the inherent allocator).  There can be
    // more than one BufferingPool, which is the only performance advantage over the platform-side
    // VmemPool/Win32HeapInfo. In practice, 'inner' here is one of those.
    //
    // However with multiple BufferingPools, available memory in one cannot be used to satisfy a request
    // from another, or to satisfy a request from the platform allocator. So keep the max_ parameter to
    // the constructor moderate, to avoid causing memory problems. A full BufferingPool will get a page
    // from 'inner' which isn't much slower.
    struct BufferingPool
        : Pool
        , StandardDisposable< BufferingPool, Disposable, AllocTail< void *, Platform >>
    {
        BufferingPool( Pool &, impl::ResourceSample const &, size_t );

        // Pool
        Pool::Desc const & describe( void );
        void * map( void );
        void unmap( void * );

        Pool * inner_;
        Pool::Desc desc_;
        AutoDispose< Monitor > mapsLock_;
        unsigned mapsUsed_;
        unsigned mapsMax_;
        void * maps_[];
    };

    struct BufferingPoolNil
        : Pool
        , StandardDisposable< BufferingPoolNil, Disposable, AllocStatic< Platform >>
    {
        BufferingPoolNil( Pool &, impl::ResourceSample const & );

        // Pool
        Pool::Desc const & describe( void );
        void * map( void );
        void unmap( void * );

        Pool * inner_;
        Pool::Desc desc_;
    };

    // VmemPool places every allocation at a unique address.  This only works for line
    // scale allocations.  Because this is already part of a pool, we'll do a little bit
    // of local locking.
    //struct VmemPool
    //    : Pool
    //    , StandardDisposable< VmemPool, Disposable, AllocStatic< Platform >>
    //{
    //    static size_t const sizeVmemRegion = 65536U;
    //    static size_t const sizeVmemPage = 4096U;
    //    static size_t const sizeVmemAlign = 64U;

    //    struct RegionHead
    //    {
    //        unsigned volatile refs_;
    //    };

    //    VmemPool( impl::ResourceSample const &, size_t, size_t, unsigned );
    //    ~VmemPool( void );

    //    // Pool
    //    Pool::Desc const & describe( void );
    //    void * map( void );
    //    void unmap( void * );

    //    // local methods
    //    // Beginning of regionss for pages
    //    static inline void * pageOf( void * );
    //    // Beginning of the region
    //    static inline RegionHead * regionOf( void * );
    //    // A full unmap from a user-data pointer
    //    static inline void unmapAlloc( void * );

    //    AutoDispose< Monitor > lock_;
    //    // Top of the region we're inserting into
    //    RegionHead * regionBegin_;
    //    // End of the region
    //    uint8 * regionEnd_;
    //    // Next place we're putting memory
    //    uint8 * regionNext_;
    //    uint8 * reserveNext_;
    //    Pool::Desc desc_;
    //    AlignSpec spec_;
    //};

    // The node pool is a complex subdivision of a larger power of two pool size.
    struct SuperBlock
        : boost::intrusive::list_base_hook<boost::intrusive::link_mode< boost::intrusive::auto_unlink > >
    {
        // Current number of allocations, so we can tell if the block is free.
        unsigned refs_;
        // The next free node within this slab.  If this is nullptr the slab is entirely occupied.
        void * freeMap_;
    };

    // Common sync/async behavior, this leaves the final call to map untouched.
    struct NodePoolBase
        : Pool
    {
        typedef boost::intrusive::list< SuperBlock, boost::intrusive::constant_time_size< false > > FreeList;

        NodePoolBase( Pool &, impl::ResourceSample const &, size_t, size_t, size_t = 0U );
        ~NodePoolBase( void );

        // Pool
        Pool::Desc const & describe( void );

        // local methods
        SuperBlock * superBlockOf( void * );
        SuperBlock * firstFree( void );
        // Format this into the new block area
        void acceptSuperBlock( void * );
        // Try and map out of existing user data
        void * tryMapUser( void );
        // Unmap user data and return a SuperBlock pointer if it was entirely derefed this
        // is separated to allow sync access without locking over a call into the parent.
        void * unmapUser( void * );

        // Parameters for allocation
        Pool * super_;
        size_t superSize_;
        Pool::Desc desc_;
        AlignSpec spec_;
        // Current allocation units
        // List of superblocks that have at least one free block.
        FreeList freeBlocks_;
        // Current source for new blocks when all other free blocks are exhausted.
        SuperBlock * newBlock_;
        // Range of bytes present in a new map block
        void * newMapBegin_;
        void * newMapEnd_;
    };

    struct NodePool
        : NodePoolBase
        , StandardDisposable< NodePool, Disposable, AllocStatic< Platform >>
    {
        NodePool( Pool &, impl::ResourceSample const &, size_t, size_t );

        // Pool
        void * map( void );
        void unmap( void * );
    };

    // A node-style pool allocator that uses locks to achieve thread safety, with all corresponding
    // performance issues. Contrast this with NodePool, which is non-locking and only safe in thread
    // local applications. Though limiting to thread-local is much harder then it may sound. Generally,
    // a block is commonly freed on a different thread than the one on which it was allocated.
    struct NodePoolSync
        : NodePoolBase
        , StandardDisposable< NodePoolSync, Disposable, AllocStatic< Platform >>
    {
        NodePoolSync( Pool &, impl::ResourceSample const &, size_t, size_t, size_t );

        // Pool
        void * map( void );
        void unmap( void * );

        AutoDispose< Monitor > lockFree_;
    };

    // An unformatted 'free' node
    struct FreeNode
    {
        FreeNode * next_;
    };

    struct NodeSmallPool;

    struct SlabHeadSmall
    {
        enum State : uint8
        {
            StateAttached = 0U,  // In use on a particular thread
            StateLowFrag = 1U,  // Low frag (not attached, but also not suitable for reuse)
            StateFree = 2U,  // Returned to the parent
        };

        struct RefState
        {
            State state_;
            unsigned refs_;  // # allocated
        };

        // Calculate the ref barriers against final format parameters
        void calcRefs( unsigned );
        // Attach this to a thread, the thread will imply a minimum set of references so it can track
        // them internally without memory contention going allocations.
        void attach( unsigned, bool );
        // Remove (threadRefs) references from a slab of state 'StateAttached'. If refs_ is <= logFragRefs_,
        // change state to 'StateFree' and unmap; otherwise, change state to 'StateLowFrag'. Currently
        // just called from the NodeSmallPool destructor.
        void detach( NodeSmallPool *, unsigned );
        // Try and reuse this slab if there are free nodes available for reuse. If this returns non-nullptr
        // the slab is configured for reuse otherwise it is configured for return to the master pool when
        // it is sufficiently fragmented.
        FreeNode * reuse( unsigned, unsigned );
        void unmapSlab( NodeSmallPool * );
        void unmap( NodeSmallPool *, void * );

        // If refs_ drops below this, the slab may be put on the free list. This takes into account some
        // hysteresis. This is set after the slab is formatted.
        unsigned lowFragRefs_;
        // Maximum number of references for immediate reuse. This is always higher than the low frag refs
        // because the reuse cost is much lower, but it is also above zero to prevent pathological reuse.
        unsigned reuseRefs_;
        // Links for when this slab is admitted to the master free list.
        SlabHeadSmall * nextSlab_;
        tools::AtomicAny< RefState > refs_;
        FreeNode * volatile frees_;  // Exact pointers to free nodes; frees are queued here to be recycled later
    };

    // Thread-local allocation of small, fixed size items from backing slabs of type SlabHeadSmall.
    // Deallocations don't come back to this class, they interact only with the backing slab (in a lock-
    // free manner). This is similar to temporal. This is a helper for NodeSmallPool.
    struct NodeSmallLocal
    {
        NodeSmallLocal( NodeSmallPool * );
        ~NodeSmallLocal( void );

        unsigned maxRefs( void );
        void allocSlab( void );
        void * tryMapCurrent( void );
        void * map( void );
        SlabHeadSmall * allocSlabParent( bool * );

        NodeSmallPool * parent_;
        AlignSpec spec_;
        size_t slabSize_;
        SlabHeadSmall * currentSlab_;
        FreeNode * currentFrees_;  // Try to allocate from here first.
        unsigned currentRefs_;  // Count of suspended references.
        void * newMapBegin_;  // Next never-before-allocated spot in the slab to use.
        void * newMapEnd_;  // When newMapBegin_ reaches here, the slab is full.
        unsigned newMapNodes_;  // # of items that have been allocated.
    };

    // A base for NodeSmallPool, which does most of the work. Having this in a seperate type makes management
    // of sub-references during destruction (after the thread local pool tears down) much easier.
    struct NodeSmallPoolBase
        : Pool
    {
        enum : size_t {
            superUnmapsMax = 2U,
        };

        struct FreeSlab
        {
            bool operator<( FreeSlab const & r ) const {
                // This is in reverse order, fewer refs is emptier (and a better candidate to hand out)
                return (r.slabRefs_ < slabRefs_ );
            }

            SlabHeadSmall * slab_;
            size_t slabRefs_;
        };
        typedef std::vector< FreeSlab, AllocatorAffinity< FreeSlab, Jumbo< Platform >>> SlabVec;
        struct SuperUnmapQueue
        {
            SuperUnmapQueue( Unmapper &, unsigned, StringId const & );
            ~SuperUnmapQueue( void );

            // can this be freed?
            uint32 const * operator()( SlabHeadSmall * );

            Unmapper * slabPool_;
            unsigned poolSize_;
            StringId leakProtectNote_;
            SlabHeadSmall * superUnmaps_;
            bool passSuperUnmap_;
            uint32 refs_;
        };

        NodeSmallPoolBase( Pool & );
        ~NodeSmallPoolBase( void );

        // local methods
        bool tryRefill( void );
        void pushSlab( SlabHeadSmall *, bool = true );
        SlabHeadSmall * popSlab( void );

        Pool * slabPool_;
        SlabVec freeSlabs_;
        SlabVec refillSlabs_;
        SlabHeadSmall * volatile freeQueueSlabs_;
        unsigned volatile freeQueueLength_;
        unsigned volatile queueRefillLength_;  // Refill when the queue reaches this
        unsigned volatile freeRefillLength_;  // Refill when the free vec reaches this
        AutoDispose< Monitor > freeSlabsLock_;
        AutoDispose< Monitor > refillSlabsLock_;
    };

    // A (almost completely) lock-free pool allocator for small objects. Items are allocated and freed from
    // backing slabs. Allocation is thread local and managed by NodeSmallLocal with the help of SlabHeadSmall;
    // frees only go through SlabHeadSmall code. This separation of allocation and free code is similar
    // to temporal. The most interesting algorithms are in the base class NodeSmallPoolBase.
    struct NodeSmallPool
        : NodeSmallPoolBase
        , StandardDisposable< NodeSmallPool, Disposable, AllocStatic< Platform >>
    {
        NodeSmallPool( Pool &, impl::ResourceSample const &, size_t, size_t );

        // Pool
        Pool::Desc const & describe( void );
        void * map( void );
        void unmap( void * );

        // local methods
        SlabHeadSmall * slabHeadOf( void * );

        Pool::Desc desc_;
        AlignSpec spec_;
        size_t slabSize_;
        StandardThreadLocalHandle< NodeSmallLocal > localPool_;
    };

    // A simple pool implementation to wrap around a given memory region (ptr, len) to allocate fixed size
    // memory chunks out of it. Useful, for example, to allocate shared memory into pages or slabs.
    struct MemoryWrapPool
        : Pool
        , StandardDisposable< MemoryWrapPool, Disposable, AllocStatic< Platform >>
    {
        MemoryWrapPool( void *, size_t, size_t );

        // Pool
        Pool::Desc const & describe( void );
        void * map( void );
        void unmap( void * );

        void * base_;
        size_t length_;
        size_t allocSize_;
        AutoDispose< Monitor > freeLock_;
        void * freeList_;
        uint8 * nextChunk_;
        Pool::Desc desc_;
        AlignSpec spec_;
    };

    // Unbuffered binary pool.  Integrates the mater internally.
    struct BinaryPool
        : Pool
        , StandardDisposable< BinaryPool, Disposable, AllocStatic< Platform >>
    {
        BinaryPool( Pool &, impl::ResourceSample const & );

        // Pool
        Pool::Desc const & describe( void );
        void * map( void );
        void unmap( void * );

        BinaryPoolMaster master_;
    };

    struct MallocPool
        : Pool
        , StandardDisposable< MallocPool, Disposable, AllocStatic< Platform >>
    {
        MallocPool( impl::ResourceSample const &, size_t, size_t );

        // Pool
        Pool::Desc const & describe( void );
        void * map( void );
        void unmap( void * );

        Pool::Desc desc_;
        impl::ResourceSample sample_;
        AlignSpec spec_;
        Affinity * platform_;
    };

    struct PoolSpec
    {
        PoolSpec( void ) {}
        PoolSpec( size_t size, size_t phase ) : size_( size ), phase_( phase ) {}
        bool
        operator<(
            PoolSpec const & c ) const
        {
            if( size_ == c.size_ ) {
                return phase_ < c.phase_;
            }
            return size_ < c.size_;
        }

        size_t size_;
        size_t phase_;
    };

    struct AllocHeadPlain;
    struct AllocHeadCheckLifetime;

    struct SlabHead
    {
        SlabHead( unsigned, Pool *, bool, bool );
        size_t getSlabSize( void );
        void setAllocsIfNull( AllocHeadCheckLifetime * );
        void unmapSlab( void );
        void preUnmapCheck( void );
        void lifetimeSkewCheck( AllocHeadCheckLifetime * );
        void unmap( AllocHeadPlain * );

        unsigned volatile refs_;
        bool leakProtect_;
        bool checkLifetime_;  // are we checking lifetime skew?
        Pool * source_;
        // If checkLifetime, the following are defined.
        uint64 genesis_;
        AllocHeadCheckLifetime * allocs_;  // first allocation in this slab
    };

    struct AllocHeadPlain
    {
        AllocHeadPlain( SlabHead * parent, impl::ResourceSample const &, impl::ResourceTrace * ) : parent_( parent ) {}
        void unmap( void ) { parent_->unmap( this ); }

        SlabHead * parent_;
    };

    static_assert( sizeof( AllocHeadPlain ) <= 8, "AllocHeadPlain is wrong size" );

    // In sampled slabs, some data is stored before the AllocHeadPlain, which formas a linked list of all
    // allocations on the slab.  AllocHeadCheckLifetime is responsible for maintaining this data.
    struct CheckLifetimeData
    {
        CheckLifetimeData( impl::ResourceTrace * trace ) : trace_( trace ), nextOffset_( 0 ) {}

        impl::ResourceTrace * trace_;
        uint32 nextOffset_;  // byte offset from slab head
        uint32 timeMs_;  // if currently allocated, allocation time (ms since slab genesis), if freed, delta-t (freed - allocation time in ms).
    };

    struct AllocHeadCheckLifetime
        : CheckLifetimeData
        , AllocHeadPlain
    {
        AllocHeadCheckLifetime( SlabHead *, impl::ResourceSample const &, impl::ResourceTrace * );
        void setNext( AllocHeadCheckLifetime * );
        AllocHeadCheckLifetime * getNext( void );
        void onUnmap( void );
    };

    static_assert( sizeof( AllocHeadCheckLifetime ) <= 24, "AllocHeadCheckLifetime is wrong size" );

    struct CycleSampler
    {
        CycleSampler( unsigned length ) : next_( 0 ), length_( length ) {}
        bool sample( void ) {
            if( length_ == 0 ) {
                return false;
            }
            if( next_ != 0 ) {
                --next_;
                return false;
            } else {
                next_ = length_ - 1;
                return true;
            }
        }

        unsigned next_;
        unsigned length_;
    };

    struct TemporalBase
    {
        explicit TemporalBase( Pool &, impl::ResourceTrace *, bool );
        ~TemporalBase( void );

        void attach( uint8 * );
        bool release( void );
        void linkAllocationIn( AllocHeadPlain * );
        void linkAllocationIn( AllocHeadCheckLifetime * );
        void * mapOnCurrentSlab( size_t, impl::ResourceSample const &, size_t, size_t );
        void * map( size_t, impl::ResourceSample const &, size_t, size_t );
        bool isInitialized( void ) const;
        static void unmapAny( void * );
        template< typename AllocHeadT >
        void * mapOnCurrentSlabWithHead( size_t size, impl::ResourceSample const & sample, size_t phase, size_t align ) {
            void * place;
            place = alignPlace( size + sizeof( AllocHeadT ), phase + sizeof( AllocHeadT ), align, slabUsed_, slabMax_ );
            if( TOOLS_UNLIKELY( !place )) {
                return nullptr;
            }
            AllocHeadT * aHead = static_cast< AllocHeadT * >( place );
            TOOLS_ASSERT( reinterpret_cast< uint8 * >( aHead ) >= slabUsed_ );
            // The placement function may leave gaps between the allocations, take that into account.
            slabUsed_ = reinterpret_cast< uint8 * >( place ) + ( size + sizeof( AllocHeadT ));
            TOOLS_ASSERT( slabUsed_ <= slabMax_ );
            TOOLS_ASSERT( slabHead_ != nullptr );
            // Construction via placement new
            ( void )::new( static_cast< void * >( aHead )) AllocHeadT( slabHead_, sample, parentTrace_ );
            linkAllocationIn( aHead );
            // One might expect to see slabHead_->refs_++ here.  Instead we do something semantically
            // equivalent, but subtley different.  We decrement innerRefs_.  This based on the observation
            // that when the slab fills, slabHead_->refs_ -= innerRefs_.  We can add 1 now, or decrement 1
            // less later on.  They are equivalent.
            //
            // The up side is that we avoid an atomic operation, innerRefs_ is thread local.
            --innerRefs_;
            return aHead + 1;
        }

        // The underlying buffer source is a pool
        Pool * runPool_;
        Pool::Desc runPoolDesc_;
        impl::ResourceTrace * parentTrace_;
        // This is the number of refs we will remove when the slab is released to the
        // world.  Because this is thread-local, no synchronization is performed.  The
        // block is overprovisioned with references until it's full, then the excess
        // references are released all at once.  Memory frees that occur before the
        // block is filled twiddle on their own cache line.
        unsigned innerRefs_;
        // Dimensions of our current slab
        SlabHead * slabHead_;
        uint8 * slabUsed_;
        uint8 * slabMax_;
        AllocHeadCheckLifetime * slabLast_;
        bool leakProtect_;
        CycleSampler checkLifetimeSampler_;
        bool slabHeadCheckLifetime_;
    };

    struct TemporalPoolProxy
        : Pool
    {
        TemporalPoolProxy( TemporalBase &, Pool::Desc const &, impl::ResourceSample const & );

        // Pool
        Pool::Desc const & describe( void );
        void * map( void );
        void unmap( void * );

        TemporalBase * base_;
        Pool::Desc desc_;
        impl::ResourceSample sample_;
    };

    // The thread-local temporal run is, itself, thread-local and permenantly bound to
    // the associated pool pages.
    struct ThreadLocalTemporalAffinity
        : Affinity
    {
        // A default run size.  TODO: start this smaller and make it more progressive
        // as use increases.
        enum : size_t {
            sizeRun = temporalSlabSizeLarge,
            sizeRunMedium = temporalSlabSizeMedium,
            sizeRunSmall = temporalSlabSizeSmall,
            parentPoolCutoff = 1024U * 1024U,
        };

        typedef std::map< PoolSpec, TemporalPoolProxy > PoolMap;

        ThreadLocalTemporalAffinity( Affinity &, impl::ResourceTrace *, bool );

        // Affinity
        Affinity & bind( void );
        AutoDispose<> fork( Affinity **, impl::ResourceSample const & );
        Pool & pool( size_t, impl::ResourceSample const &, size_t );

        // Heap
        void * map( size_t, impl::ResourceSample const &, size_t );
        void unmap( void * );

        // Parent configuration
        Affinity * parent_;
        impl::ResourceSample parentPoolSample_;
        Pool * parentPool_;  // 2MB packing slabs for temporalHeapLarge
        Pool::Desc parentPoolDesc_;
        Pool * parentPoolSmall_;  // 32K backing slabs for temporalHeapSmall
        Pool * parentPoolMedium_;  // 256K backing slabs for temporalHeapMedium
        impl::ResourceTrace * parentTrace_;
        bool leakProtect_;
        TemporalBase temporalHeapSmall_;  // temporal allocations [0, 256) go here
        TemporalBase temporalHeapMedium_;  // temporal allocations [256, 16384) go here
        TemporalBase temporalHeapLarge_;  // temporal allocations [16384, 1MB) go here
        // Lookup for specific pools
        PoolMap pools_;
    };

    // A multi-threaded temporal affinity
    struct TemporalAffinity
        : Affinity
        , StandardDisposable< TemporalAffinity, Disposable, AllocStatic< Platform >>
    {
        struct PoolProxyBase
            : Pool
        {
            explicit PoolProxyBase( impl::ResourceSample const & );
            // Pool
            Desc const & describe( void );
            void * map( void );
            void unmap( void * );

            Pool::Desc desc_;
            StandardThreadLocalHandle< ThreadLocalTemporalAffinity > * bound_;
            impl::ResourceSample sample_;
        };
        typedef std::map< PoolSpec, PoolProxyBase, std::less< PoolSpec >, AllocatorAffinity< std::pair< PoolSpec const, PoolProxyBase >, Platform >> PoolMap;

        TemporalAffinity( bool, Affinity &, impl::ResourceTrace *, bool );
        ~TemporalAffinity( void );

        // Affinity
        Affinity & bind( void );
        AutoDispose<> fork( Affinity **, impl::ResourceSample const & );
        Pool & pool( size_t, impl::ResourceSample const &, size_t );

        // Heap
        void * map( size_t, impl::ResourceSample const &, size_t );
        void unmap( void * );

        Affinity * parent_;
        AutoDispose<> parentDisp_;
        impl::ResourceTrace * trace_;
        // The affinities already bound to threads.
        StandardThreadLocalHandle< ThreadLocalTemporalAffinity > bound_;
        PoolMap pools_;
        AutoDispose< Monitor > poolsLock_;
        bool leakProtect_;
    };

    struct ThreadLocalTemporalFactoryBase
    {
        ThreadLocalTemporalFactoryBase( TemporalAffinity & );
        AutoDispose<> operator()( ThreadLocalTemporalAffinity ** ) const;

        TemporalAffinity * outer_;
    };

    struct ThreadLocalTemporalAffinityFork
        : ThreadLocalTemporalAffinity
        , StandardDisposable< ThreadLocalTemporalAffinityFork, Disposable, AllocStatic< Platform >>
    {
        explicit ThreadLocalTemporalAffinityFork( Affinity &, impl::ResourceTrace *, bool );
    };

    struct ShutdownDump
    {
        ~ShutdownDump( void );
    };

    struct SlabUnitDesc
    {
        uint8 idx_;  // Index for this unit.
        bool isSaturated_;  // Is this saturated? We will only accumulated saturated slabs.
        uint16 minFreeReuse_;  // Minimum count of items free before we are reusable.
        uint32 srcBytes_;  // Be stingy on the size here.
        Pool * src_;  // Underlying pool for maps/unmaps.
        tools::detail::CyclicSlab * term_;  // The terminator we will use for this entry.
    };

    struct CyclicPoolDescImpl
        : tools::detail::CyclicPoolDesc
        , StandardDisposable< CyclicPoolDescImpl, Disposable, AllocStatic< Platform >>
    {
        CyclicPoolDescImpl( StringId const & );
    };
};  // anonymous namespace

///////////////////////
// Non-member functions
///////////////////////

namespace tools {
    namespace impl {
        AutoDispose< Monitor > monitorPlatformNew( void );
        void * platformHugeAlloc( size_t );
        void platformHugeFree( void *, size_t );
        void * vmemPoolMap( void * );
        void vmemPoolDecommit( void * );
        void vmemPoolRelease( void * );
        void leakAndProtect( void *, size_t, StringId const & );
        bool memoryPoison( void );
        bool memoryTrack( void );
        unsigned memoryTrackingIntervalInherent( void );
        unsigned memoryTrackingIntervalLifetime( void );
        unsigned memoryTrackingIntervalTemporal( void );
        void platformCapVsize( uint64 );
        void platformReleaseMemory( void );
        void platformUncapVsize( void );
        uint64 platformVsizeCap( void );
    }; // impl namespace
}; // tools namespace

namespace {
    StringId
    leakProtectNoteSlab( void )
    {
        static StringId ret = StaticStringId( "temporal slab" );
        return ret;
    }
};  // anonymous namespace

static AutoDispose<>
affinityVerifyNew(
    unsigned trackingInterval,
    Affinity ** outAffinity,
    Affinity * inner,
    Disposable * innerDispose,
    impl::ResourceTrace * target,
    unsigned volatile * forkOut,
    bool checkRt = false )
{
    impl::ResourceTrace * trace = target;
    if( !target ) {
        trace = impl::resourceTraceBuild( TOOLS_RESOURCE_SAMPLE_CALLER( 0 ));
    }
    VerifyAffinity * ret = new VerifyAffinity( trackingInterval, *inner, innerDispose, trace, forkOut, checkRt );
    *outAffinity = ret;
    return std::move(ret);
}

static void
alignSpecOf(
    AlignSpec * spec,
    size_t size,
    size_t phase,
    bool locator = true )
{
    TOOLS_ASSERT( ( size != 0 ) && ( phase < size ));
    // The size must be aligned to at least a word.
    TOOLS_ASSERT( ( size & ( sizeof( void * ) - 1U )) == 0 );
    spec->size_ = size;
    spec->phase_ = phase;
    size_t sizeUser = size - phase;
    if( sizeUser <= 56 ) {
        // These are aligned smaller than a cache line.  They can and should be allowed
        // to jostle within the cache lines.
        spec->model_ = alignModelTiny;
        spec->scale_ = alignScaleLine;
        // A 16-byte unit may contain a 16-byte variable.  Take that, otherwise align
        // to word size.
        spec->alignBytes_ = ( sizeUser & ( 16U - 1U )) ? 8U : 16U;
        // Alloc size is based on sliding the entire allocation around within the block
        // (assume we cannot allocate less then a word).
        spec->alignAlloc_ = ( size + ( sizeof( void *[ 1 ] ) * locator )) +
            sizeUser - 8U;
        // TODO: Tweak the size to pad out the end so the next call to allocate this
        // exact same spec will rotate among the 4 16-byte banks (without splitting
        // across a cache line).
        spec->placeBytes_ = size;
        return;
    }
    if( ( sizeUser & ( 4096U - 1U )) != 0 ) {
        // This is a ragged allocation request of any size.
        spec->model_ = alignModelLine;
        spec->alignBytes_ = 64U;
        // The scale may push up the number of bytes we consider allocating.
    } else {
        // This is a page aligned unphased request
        spec->model_ = alignModelPage;
        spec->alignBytes_ = 4096U;
    }
    if( locator ) {
        spec->alignAlloc_ = size + sizeof( void *[ 1 ] ) + spec->alignBytes_;
    } else {
        spec->alignAlloc_ = tools::roundUpPow2( size, spec->alignBytes_ );
    }
    spec->placeBytes_ = size;
    // Assign the scale seperately from the alignment model
    if( spec->alignAlloc_ < ( 16U * 1024U )) {
        spec->scale_ = alignScaleLine;
        return;
    }
    if( spec->alignAlloc_ < ( 256U * 1024U )) {
        // These are pool managed sizes.
        spec->scale_ = alignScalePage;
    } else {
        // These are unique sizes (because of the size they aren't efficiently bulk
        // allocated and will instead be rounded up to a power of two at or under 2MB).
        spec->scale_ = alignScaleUnique;
    }
    if( ( spec->alignAlloc_ & ( 4096U - 1U )) != 0 ) {
        // Make the page scale allocator allocate in page units
        spec->alignAlloc_ = ( spec->alignAlloc_ & ~( static_cast< size_t >( 4096 ) - 1U )) + 4096U;
        // The align remains the same so the beginning can float inside the allocated
        // blocks.
    }
}

// Measure an area for aligned placement potential.  This may return nullptr if the
// allocation cannot fit.  If this returns non-nullptr, the return value is the beginning
// of the user data area and the new end value is AlignSpec.placementBytes_ afteer.
// The pointers are not dereferenced.
static inline void *
alignPlace(
    size_t size,
    size_t phase,
    size_t align,
    void * freeBegin,
    void * freeEnd )
{
    // Given an allocation region, begin by aligning to the start of user data.
    size_t userSize = ( size - phase );
    size_t freeBeginSize = reinterpret_cast< size_t >( freeBegin );
    size_t userBeginSize = freeBeginSize + phase;
    // Align the user start to the next position.
    size_t userBeginAlignSize = tools::roundUpPow2( userBeginSize, align );
    if( align < 64U ) {
        // The tiny model tries to ensure the beginning and the end are within the same cache line.
        size_t userEndAlignSize = userBeginAlignSize + userSize - 8U;
        if( tools::roundDownPow2( userBeginAlignSize, 64 ) != tools::roundDownPow2( userEndAlignSize, 64 )) {
            // This is a cache line split.  Reallign to the next cache line
            userBeginAlignSize = tools::roundUpPow2( userBeginSize, 64 );
        }
    }
    size_t sizeEnd = ( userBeginAlignSize + userSize );
    if( sizeEnd > reinterpret_cast< size_t >( freeEnd )) {
        return nullptr;
    }
    // Every thing is all nice and aligned now
    return reinterpret_cast< void * >( userBeginAlignSize - phase );
}

static inline void *
alignPlace(
    AlignSpec const & spec,
    void * freeBegin,
    void * freeEnd )
{
    return alignPlace( spec.size_, spec.phase_, spec.alignBytes_, freeBegin, freeEnd );
}

// Take a block allocated to size 'spec.alignAlloc_' and adjust it to the ideal
// alignment position; leaving a mark for unaligning at a later time.
static inline void *
alignAlloc(
    AlignSpec const & spec,
    void * site )
{
    // We trust the allocation is at least spec.alignAlloc_ long, and we're at the beginning.
    void * placeBegin = alignPlace( spec, static_cast< void ** >( site ) + 1,
        static_cast< uint8 * >( site ) + spec.alignAlloc_ );
    TOOLS_ASSERT( !!placeBegin );
    static_cast< void ** >( placeBegin )[ -1 ] = site;
    return placeBegin;
}

// Retrieve the original unaligned pointer for return to the allocator.
static inline void *
unalignAlloc(
    void * site )
{
    // Use a leading pointer to correct for the unalign
    return static_cast< void ** >( site )[ -1 ];
}

//static void *
//verifyHeapMap(
//    Heap & inner,
//    impl::ResourceTrace * target,
//    bool bound,
//    size_t size,
//    impl::ResourceSample const & sample,
//    size_t phase )
//{
//    HeapPrefix * prefix;
//    uint8 * body;
//    size_t userSize = ( size - phase );
//    if( hasSuffix( userSize )) {
//        prefix = static_cast< HeapPrefix * >( inner.map(
//            size + sizeof( HeapPrefix ) + sizeof( HeapSuffix ), sample,
//            phase + sizeof( HeapPrefix )));
//        if( !prefix ) {
//            impl::resourceTraceDump( impl::resourceTraceDumpPhaseTerminal );
//            // TODO: log out of memory message
//            return nullptr;
//        }
//        body = reinterpret_cast< uint8 * >( prefix ) + 1;
//        HeapSuffix * suffix = reinterpret_cast< HeapSuffix * >( body + size );
//        suffix->checkout1_ = 0xCACACACA;
//        suffix->checkout2_ = 0x10101010;
//    } else {
//        prefix = static_cast< HeapPrefix * >( inner.map( size + sizeof( HeapPrefix ),
//            sample, phase + sizeof( HeapPrefix )));
//        if( !prefix ) {
//            impl::resourceTraceDump( impl::resourceTraceDumpPhaseTerminal );
//            // TODO: log out of memory message
//            return nullptr;
//        }
//        body = reinterpret_cast< uint8 * >( prefix ) + 1;
//    }
//    prefix->size_ = size;
//    prefix->phase_ = phase;
//    prefix->boundHeap_ = &inner;
//    prefix->boundThread_ = bound ? impl::threadId() : ~0ULL;
//    prefix->trace_ = impl::resourceTraceBuild( sample, target );
//    prefix->checkin1_ = 0xABABABAB;
//    prefix->checkin2_ = 0xBABABABA;
//    // We assume that fill is perfomed by the pool being verified.
//    impl::resourceTraceInc( prefix->trace_, 1U );
//    return body;
//}
//
//static void
//verifyHeapUnmap(
//    Heap & inner,
//    void * site )
//{
//    HeapPrefix * prefix = static_cast< HeapPrefix * >( site ) - 1;
//    // First a simple check for overruns and underruns
//    TOOLS_ASSERT( prefix->checkin1_ == 0xABABABAB );
//    TOOLS_ASSERT( prefix->checkin2_ == 0xBABABABA );
//    TOOLS_ASSERT( &inner == prefix->boundHeap_ );
//    TOOLS_ASSERT( ( prefix->boundThread_ == ~0ULL ) ||
//        ( prefix->boundThread_ == impl::threadId() ));
//    if( hasSuffix( prefix->size_ - prefix->phase_ )) {
//        HeapSuffix * suffix = reinterpret_cast< HeapSuffix * >(
//            static_cast< uint8 * >( site ) + prefix->size_ );
//        if( !!suffix ) {
//            TOOLS_ASSERT( suffix->checkout1_ == 0xCACACACA );
//            TOOLS_ASSERT( suffix->checkout2_ == 0x10101010 );
//        }
//    }
//    impl::resourceTraceDec( prefix->trace_, 1U );
//    // This must be performed last because it'll clobber and/or unmap the underlying data.
//    inner.unmap( prefix );
//}

static BinaryMasterSize
binaryMasterSizeFromSize(
    size_t size )
{
    if( size == 32U * 1024U ) {
        return binaryMasterSize32;
    } else if( size == 64U * 1024U ) {
        return binaryMasterSize64;
    } else if( size == 128U * 1024U ) {
        return binaryMasterSize128;
    } else if( size == 256U * 1024U ) {
        return binaryMasterSize256;
    } else if( size == 512U * 1024U ) {
        return binaryMasterSize512;
    }
    TOOLS_ASSERT( size == 1024U * 1024U );
    return binaryMasterSize1024;
}

static size_t
sizeFromBinaryMasterSize(
    BinaryMasterSize size )
{
    switch( size )
    {
    case binaryMasterSize1024:
        return 1024U * 1024U;
    case binaryMasterSize512:
        return 512U * 1024U;
    case binaryMasterSize256:
        return 256U * 1024U;
    case binaryMasterSize128:
        return 128U * 1024U;
    case binaryMasterSize64:
        return 64U * 1024U;
    case binaryMasterSize32:
        return 32U * 1024U;
    case binaryMasterSizeMax:
    default:
        TOOLS_ASSERT( !"Invalid size" );
        break;
    };
    return 0U;
}

static AutoDispose<>
poolBinaryLocalBufferNew(
    Pool ** referencePool,
    BinaryPoolMaster & master )
{
    size_t tailUnits = ( 1024U * 1024U ) / master.desc_.size_;
    BinaryPoolThreadBuffer * newPool = new( tailUnits ) BinaryPoolThreadBuffer( &master );
    *referencePool = newPool;
    return std::move(newPool);
}

static AutoDispose<>
poolBufferNew(
    Pool ** referencePool,
    Pool & inner,
    impl::ResourceSample const & sample,
    size_t max )
{
#ifdef TOOLS_MEM_PLATFORM
    *referencePool = &impl::affinityInstance< Platform >().pool( inner.describe().size_, inner.describe().phase_ );
    return nullDisposable();
#else // TOOLS_MEM_PLATFORM
    if( max > 0 ) {
        BufferingPool * ret = new( max ) BufferingPool( inner, sample, max );
        *referencePool = ret;
        return std::move(ret);
    }
    BufferingPoolNil * ret = new BufferingPoolNil( inner, sample );
    *referencePool = ret;
    return std::move(ret);
#endif // TOOLS_MEM_PLATFORM
}

//static AutoDispose<>
//poolVmemNew(
//    Pool ** referencePool,
//    impl::ResourceSample const & sample,
//    size_t size,
//    size_t phase,
//    unsigned lockLevel )
//{
//    VmemPool * newPool = new VmemPool( sample, size, phase, lockLevel );
//    *referencePool = newPool;
//    return newPool;
//}

static AutoDispose<>
poolMallocNew(
    Pool ** referencePool,
    impl::ResourceSample const & sample,
    size_t size,
    size_t phase )
{
    MallocPool * newPool = new MallocPool( sample, size, phase );
    *referencePool = newPool;
    return std::move(newPool);
}

static AutoDispose<>
poolNodeNew(
    Pool ** referencePool,
    Pool & parent,
    impl::ResourceSample const & sample,
    size_t size,
    size_t phase = 0 )
{
#ifdef TOOLS_MEM_PLATFORM
    return poolMallocNew( referencePool, sample, size, phase );
#else // TOOLS_MEM_PLATFORM
    NodePool * ret = new NodePool( parent, sample, size, phase );
    *referencePool = ret;
    return std::move(ret);
#endif // TOOLS_MEM_PLATFORM
}

static AutoDispose<>
poolNodeSyncNew(
    Pool ** referencePool,
    Pool & parent,
    impl::ResourceSample const & sample,
    size_t size,
    size_t phase = 0,
    size_t align = 0 )
{
#ifdef TOOLS_MEM_PLATFORM
    return poolMallocNew( referencePool, sample, size, phase );
#else // TOOLS_MEM_PLATFORM
    NodePoolSync * ret = new NodePoolSync( parent, sample, size, phase, align );
    *referencePool = ret;
    return std::move(ret);
#endif // TOOLS_MEM_PLATFORM
}

static AutoDispose<>
poolNodeThreadedNew(
    Pool ** referencePool,
    Pool & parent,
    impl::ResourceSample const & sample,
    size_t size,
    size_t phase = 0 )
{
#ifdef TOOLS_MEM_PLATFORM
    return poolMallocNew( referencePool, sample, size, phase );
#else // TOOLS_MEM_PLATFORM
    NodeSmallPool * ret = new NodeSmallPool( parent, sample, size, phase );
    *referencePool = ret;
    return std::move(ret);
#endif // TOOLS_MEM_PLATFORM
}

static AutoDispose<>
memoryWrapPoolNew(
    Pool ** referencePool,
    void * base,
    size_t length,
    size_t size )
{
    MemoryWrapPool * ret = new MemoryWrapPool( base, length, size );
    *referencePool = ret;
    return std::move(ret);
}

static bool
poolVerifyIsUnmap(
    void * site,
    size_t size )
{
    size_t i;
    if( ( ( size % 8 ) == 0 ) && ( ( reinterpret_cast< ptrdiff_t >( site ) & 7 ) == 0 )) {
        size /= 8;
        for( i=0; i!=size; ++i ) {
            if( static_cast< uint64 * >( site )[ i ] != VerifyPoolBase::verifyHeapFree8 ) {
                return false;
            }
        }
    } else if( ( ( size % 4 ) == 0 ) && ( ( reinterpret_cast< ptrdiff_t >( site ) & 3 ) == 0 )) {
        size /= 4;
        for( i=0; i!=size; ++i ) {
            if( static_cast< uint32 * >( site )[ i ] != VerifyPoolBase::verifyHeapFree4 ) {
                return false;
            }
        }
    } else {
        for( i=0; i!=size; ++i ) {
            if( static_cast< uint8 * >( site )[ i ] != VerifyPoolBase::verifyHeapFree ) {
                return false;
            }
        }
    }
    // looks unmapped to me
    return true;
}

static AutoDispose<>
poolBinaryNew(
    Pool ** referencePool,
    Pool & inner,
    impl::ResourceSample const & sample )
{
    BinaryPool * newPool = new BinaryPool( inner, sample );
    *referencePool = newPool;
    return std::move(newPool);
}

namespace tools
{
    namespace impl
    {
        void
        memset(
            void * site,
            int val,
            size_t size )
        {
            TOOLS_ASSERT( val < 256 );
            ::memset( site, val, size );
        }

        void
        outOfMemoryDie( void )
        {
            // We are well and truely out of memory.  We would like to call impl::resourceTraceDump(...) to
            // give us useful debugging information.  Alas that function needs to allocate in order to do the
            // dump.  We try to help it along by uncapping vsize (setrlimit).  If that fails, we go to more
            // extreeme measures and release vmem pool reserved pages.  If that fails, we don't try to output
            // memory usage stats.  We use stdout rather than log messaging on the theory that messaging
            // is likely to need more memory.  Stdout rather than stderr because it is more likely to be
            // captured.
            static unsigned entered = 0;
            unsigned prevNesting = atomicAdd( &entered, 1U );
            if( prevNesting == 0 ) {
                fprintf( stdout, "outOfMemoryDie() called, memory stats follow...\n" );
                platformUncapVsize();
            } else if( prevNesting == 1 ) {
                fprintf( stdout, "outOfMemoryDie() called recursively, dropping vmem pages to help the memory stats dump...\n" );
                platformReleaseMemory();
            } else {
                fprintf( stdout, "outOfMemoryDie() called with multiple recursion, cannot dump memory stats\n" );
            }
            if( prevNesting < 2 ) {
                impl::resourceTraceDump( impl::resourceTraceDumpPhaseAll, false, nullptr );
            }
            TOOLS_ASSERT( !"outOfMemoryDie() called" );
            abort();
        }

        bool
        regionIsUnmapped(
            void * site,
            size_t size )
        {
            size_t i;
            if( ( ( size % 8 ) == 0 ) && ( ( reinterpret_cast< ptrdiff_t >( site ) & 7 ) == 0 )) {
                uint64 * p = static_cast< uint64 * >( site );
                size /= 8;
                for( i = 0; i != size; ++i ) {
                    if( p[ i ] != VerifyPoolBase::verifyHeapFree8 ) {
                        return false;
                    }
                }
            } else if( ( ( size % 4 ) == 0 ) && ( ( reinterpret_cast< ptrdiff_t >( site ) & 3 ) == 0 )) {
                uint32 * p = static_cast< uint32 * >( site );
                size /= 4;
                for( i = 0; i != size; ++i ) {
                    if( p[ i ] != VerifyPoolBase::verifyHeapFree4 ) {
                        return false;
                    }
                }
            } else {
                uint8 * p = static_cast< uint8 * >( site );
                for( i = 0; i != size; ++i ) {
                    if( p[ i ] != VerifyPoolBase::verifyHeapFree ) {
                        return false;
                    }
                }
            }
            // looks like it is already unmapped
            return true;
        }

        bool
        regionIsPartiallyUnmapped(
            void * site,
            size_t size )
        {
            size_t i, numBytesD4;
            numBytesD4 = 0;
            if( ( ( size % 8 ) == 0 ) && ( ( reinterpret_cast< ptrdiff_t >( site ) & 7 ) == 0 )) {
                uint64 * p = static_cast< uint64 * >( site );
                size /= 8;
                for( i = 0; i != size; ++i ) {
                    if( p[ i ] == VerifyPoolBase::verifyHeapFree8 ) {
                        numBytesD4 += 8;
                    }
                }
            } else if( ( ( size % 4 ) == 0 ) && ( ( reinterpret_cast< ptrdiff_t >( site ) & 3 ) == 0 )) {
                uint32 * p = static_cast< uint32 * >( site );
                size /= 4;
                for( i = 0; i != size; ++i ) {
                    if( p[ i ] == VerifyPoolBase::verifyHeapFree4 ) {
                        numBytesD4 += 4;
                    }
                }
            } else {
                uint8 * p = static_cast< uint8 * >( site );
                for( i = 0; i != size; ++i ) {
                    if( p[ i ] == VerifyPoolBase::verifyHeapFree ) {
                        ++numBytesD4;
                    }
                }
            }
            return ( numBytesD4 >= ( size / 2 ));
        }

        void *
        safeMalloc(
            size_t size )
        {
            void * ret = malloc( size );
            if( !ret ) {
                outOfMemoryDie();
            }
            return ret;
        }

        bool
        leakProtect( void )
        {
            // TODO: make this go to configuration
            return true;
        }

        bool
        memoryPoison( void )
        {
            // TODO: make this go to configuration
            return Build::isDebug_;
        }

        bool
        memoryTrack( void )
        {
            // TODO: make this go to configuration
            return true;
        }

        unsigned
        memoryTrackingIntervalInherent( void )
        {
            // TODO: make this go to configuration
            return Build::isDebug_ ? 1 : 100;
        }

        unsigned
        memoryTrackingIntervalLifetime( void )
        {
            // TODO: make this go to configuration
            return 101;
        }

        unsigned
        memoryTrackingIntervalTemporal( void )
        {
            // TODO: make this go to configuration
            return Build::isDebug_ ? 1 : 100;
        }
    };  // impl namespace

    Affinity *
    staticServiceCacheInit( Affinity ***, Inherent *** )
    {
        static AffinityInherentMaster affinity( impl::affinityInstance< PlatformUntracked >(), nullptr, TOOLS_RESOURCE_SAMPLE_NAMED( 0, "inherent" ), 0, false );
        //if( impl::memoryTrack() ) {
        //    static TermDump term;
        //    static Affinity * result;
        //    static AutoDispose<> affinityDisp( affinityVerifyNew( impl::memTrackIntervalInherent(), &result, &affinity, nullptr, TOOLS_RESOURCE_SAMPLE_PARENT(affinity.sample_), nullptr, true ));
        //    return result;
        //} else {
            return &affinity;
        //}
    }

    Affinity *
    staticServiceCacheInit( Affinity ***, Monotonic *** )
    {
        // Infinite temporal scale.  Note: there is a possible race in fork().
        static Affinity * affinity;
        static AutoDispose<> affinityDisp( impl::affinityInstance< Temporal >().fork( &affinity ));
        return affinity;
    }

    Affinity *
    staticServiceCacheInit( Affinity ***, Temporal *** )
    {
#ifdef TOOLS_MEMORY_PLATFORM
        return &affinity_instance< Inherent >();
#else // TOOLS_MEMORY_PLATFORM
        static TemporalAffinity affinity( true, impl::affinityInstance< Inherent >(), impl::resourceTraceBuild( "temporal" ), impl::leakProtect() );
        //if( impl::memoryTrack() ) {
        //    static Affinity * result;
        //    static AutoDispose<> affinityDisp( affinityVerifyNew( impl::memTrackIntervalTemporal(), &result, &affinity, nullptr, affinity.trace_, nullptr ));
        //    return result;
        //} else {
            return &affinity;
        //}
#endif // TOOLS_MEMORY_PLATFORM
    }

    AutoDisposePair< Pool >
    poolVerifyNew(
        unsigned trackingInterval,
        Pool & inner,
        impl::ResourceTrace * mapTrace,
        size_t mapTraceElements = 1U,
        bool force = false )
    {
        TOOLS_ASSERT( trackingInterval == 1 ); // Pools track every operation
        if( force || impl::memoryTrack() ) {
            VerifyPool * newPool = new VerifyPool( trackingInterval, inner, mapTrace, mapTraceElements );
            return AutoDisposePair< Pool >( *newPool, newPool );
        }
        // No wrapping needed.
        return AutoDisposePair< Pool >( inner, nullDisposable() );
    }

    AutoDispose<>
    poolUniqueAddrVmemNew(
        Pool ** ref,
        impl::ResourceSample const & sample,
        size_t size,
        size_t phase,
        unsigned level )
    {
        VmemPoolUniqueAddr * ret = new VmemPoolUniqueAddr( sample, size, phase, level );
        *ref = ret;
        return std::move(ret);
    }

    Heap &
    heapHuge( void )
    {
        static HeapHugeImpl heapHuge_;
        return heapHuge_;
    }
};  // tools namespace

static AutoDispose< MemoryDumpTask >
memoryDumpTaskNew( ThreadScheduler & sched ) {
    return new MemoryDumpTask( sched );
}

static AutoDispose<>
affinityTemporalThreadLocalNew(
    Affinity ** affinity,
    Affinity & parent,
    impl::ResourceSample const & sample ) {
    bool leakProtect = impl::leakProtect();
    auto ret = new ThreadLocalTemporalAffinityFork( parent, impl::resourceTraceBuild( sample ), leakProtect );
    if( impl::memoryTrack() ) {
        return affinityVerifyNew( impl::memoryTrackingIntervalTemporal(), affinity, ret, ret, impl::resourceTraceBuild( sample ), nullptr );
    } else {
        *affinity = ret;
        return std::move(ret);
    }
}

// The maximum (inclusive baseline size to be included in this pool
static const size_t poolMaxElement[ tools::detail::CyclicPoolDesc::slabPoolsMax ] = {
    32U, 160U, 896U, 3840U, 16384U,
};

static size_t
poolFirstOfElement(
    size_t elementBytes ) {
    // This isn't offered for every allocation size.
    TOOLS_ASSERT( ( elementBytes > 0U ) && ( elementBytes < tools::detail::CyclicPoolDesc::cyclicBytesMax ));
    size_t unit;
    for( unit=0U; unit!=tools::detail::CyclicPoolDesc::slabPoolsMax; ++unit ) {
        if( elementBytes < poolMaxElement[ unit ]) {
            break;
        }
    }
    return unit;
}

// Specific pool instances
struct CyclicPoolUnit
{
    CyclicPoolUnit( Pool & parent, size_t unitBytes )
        : poolOwner_( poolNodeThreadedNew( &p_, parent, TOOLS_RESOURCE_SAMPLE_CALLER( unitBytes ), unitBytes ))
    {}

    Pool * p_;
    AutoDispose<> poolOwner_;
};

static const size_t poolUnits[ tools::detail::CyclicPoolDesc::slabPoolsMax ] = {
    208U, 1088U, 5376U, 22912U, 98304U,
};
// sample size 2MB, pool size 2MB
static Pool & cyclicMaster = impl::affinityInstance< Inherent >().pool( 2U * 1024U * 1024U, impl::ResourceSample( 2U * 1024U * 1024U, "cyclic allocator 2MB master backing slabs" ));
// sample size 0 (don't track, would lead to double-count with the master), pool size = 64k, pool align = 64k
static Pool * cyclicMinor;
static AutoDispose<> cyclicMinorLifetime( poolNodeSyncNew( &cyclicMinor, cyclicMaster, impl::ResourceSample( 0U, "cyclic allocator 64k minor backing slabs (from master backing slabs)" ), 65536U, 0U, 65536U ));
// The slab pools also run with thread local caching.
static CyclicPoolUnit unit0( *cyclicMinor, poolUnits[ 0U ]);
static CyclicPoolUnit unit1( *cyclicMinor, poolUnits[ 1U ]);
static CyclicPoolUnit unit2( cyclicMaster, poolUnits[ 2U ]);
static CyclicPoolUnit unit3( cyclicMaster, poolUnits[ 3U ]);
static CyclicPoolUnit unit4( cyclicMaster, poolUnits[ 4U ]);
// Anything that isn't the largest pool is unsaturated.
static size_t const slabPoolsSaturated = ( tools::detail::CyclicPoolDesc::slabPoolsMax - 1U );
// The tokens indicate where deallocation should occur. We don't retain smaller pools.
static Sentinel< tools::detail::CyclicSlab > tokenSuperSize[ slabPoolsSaturated ];

static size_t unitFromToken(
    tools::detail::CyclicSlab * sample )
{
    size_t ret;
    for( ret=0U; ret!=slabPoolsSaturated; ++ret ) {
        if( sample == &tokenSuperSize[ ret ]) {
            break;
        }
    }
    return ret;
}

static const SlabUnitDesc unitDescs[ tools::detail::CyclicPoolDesc::slabPoolsMax ] = {
    { 0U, false, 1U, static_cast< uint32 >( poolUnits[ 0U ] ), unit0.p_, &tokenSuperSize[ 0 ] },
    { 1U, false, 2U, static_cast< uint32 >( poolUnits[ 1U ] ), unit1.p_, &tokenSuperSize[ 1 ] },
    { 2U, false, 4U, static_cast< uint32 >( poolUnits[ 2U ] ), unit2.p_, &tokenSuperSize[ 2 ] },
    { 3U, false, 8U, static_cast< uint32 >( poolUnits[ 3U ] ), unit3.p_, &tokenSuperSize[ 3 ] },
    { 4U, true, 16U, static_cast< uint32 >( poolUnits[ 4U ] ), unit4.p_, },
};

// Layout the original links, in forward order, then do an allocation. Only the pointers are defined.
static void * cyclicSlabFormatAndAlloc(
    tools::detail::CyclicSlab * __restrict referenceSlab,
    size_t slabBytes,
    tools::detail::CyclicPoolDesc & user )
{
    // How many will fit?
    referenceSlab->allocMax_ = 1U;
    // Use standard inline allocators to format the slab, this is unusual but we expect burst allocations
    // by our users so two tight loops is better than one loose loop.
    AlignSpec spec;
    alignSpecOf( &spec, user.elementBytes_ + sizeof( tools::detail::CyclicSlab *[ 1 ] ), sizeof( tools::detail::CyclicSlab *[ 1 ] ), false );
    void * beginFree = referenceSlab + 1;
    void * endFree = reinterpret_cast< void * >( reinterpret_cast< uint8 * >( referenceSlab ) + slabBytes );
    // This is the aligned beginning, it must fit.
    beginFree = alignPlace( spec, beginFree, endFree );
    TOOLS_ASSERT( !!beginFree );
    *reinterpret_cast< tools::detail::CyclicSlab ** >( beginFree ) = referenceSlab;
    referenceSlab->allocHead_ = referenceSlab->toOffset( beginFree );
    while( void * nextFree = alignPlace( spec, static_cast< uint8 * >( beginFree ) + spec.placeBytes_, endFree )) {
        *reinterpret_cast< tools::detail::CyclicSlab ** >( nextFree ) = referenceSlab;
        referenceSlab->poke( beginFree, nextFree );
        beginFree = nextFree;
        ++referenceSlab->allocMax_;
    }
    // The last entry points to the top
    referenceSlab->poke( beginFree, referenceSlab );
    // We really want to hit this minimum
    TOOLS_ASSERT( referenceSlab->allocMax_ >= 4U );
    referenceSlab->freeCount_ = referenceSlab->allocMax_;
    return referenceSlab->map();
}

// Make sure we have a pool in srcPool.slabPools_[ unitLevel ], which will get used to allocate and free
// cyclic slabs.
static Pool *
tokenFaultPool(
    size_t unitLevel,
    tools::detail::CyclicPoolDesc & srcPool,
    StringId const & name )
{
    while( true ) {
        Pool * existing = atomicRead( &srcPool.slabPools_[ unitLevel ] );
        if( !!existing ) {
            // Once it becomes non-nullptr, that is stable
            return existing;
        }
        // Create a new pool. A PoolVerify wrapper (for memory tracking) on top of unitDescs[unitLevel].src
        // which is a NodeSmallPool with a fairly large allocation size (see poolUnits[]).
        size_t poolNbytes = unitDescs[ unitLevel ].src_->describe().size_;
        // Don't really need to embed the size in poolName since resource lines always show the size.
        impl::ResourceTrace  * trace = nullptr;
        if( impl::memoryTrack() ) {
            boost::format fmt( "%s cyclic slab" );
            fmt % name.c_str();
            trace = impl::resourceTraceBuild( 1, fmt.str().c_str(), poolNbytes );
            // Could pass a 4th parameter if we have a partent trace representing the cyclic master name.
        }
        // If memory tracking is on, poolVerifyNew really wraps the pool with a tracker, which needs later
        // disposing. If not, there is no wrapper and thus noting needs later disposing.
        AutoDisposePair< Pool > tmpPool( poolVerifyNew( 1, *unitDescs[ unitLevel ].src_, trace ));
        Pool * result;
        AutoDispose<> lifetime;
        result = tmpPool.release( &lifetime );
        if( atomicCas< Pool *, Pool * >( &srcPool.slabPools_[ unitLevel ], nullptr, result ) != nullptr ) {
            continue;  // drops lifetime
        }
        // Just successfully installed a pointer to the pool from unitDescs[ unitLevel ], which we don't
        // own. But we do own the pool wrapper for tracking. Store that. At this point we no longer need to
        // be atomic, this store can happen however it happens.
        TOOLS_ASSERT( !srcPool.slabPoolOwners_[ unitLevel ] );
        srcPool.slabPoolOwners_[ unitLevel ] = std::move( lifetime );
        return result;
    }
}

namespace tools {
    namespace detail {
        // This is the slow path.
        void *
        cyclicSlabPoolMap(
            CyclicSlabRootPointer * __restrict referenceSlabList,
            CyclicPoolDesc & desc,
            StringId const & name )
        {
            size_t iSlabAlloc;
            void * singletonAlloc;

            if( CyclicSlabRoot * __restrict current = referenceSlabList->other() ) {
                // We should only end up here when the 'next' entry has nothing free
                TOOLS_ASSERT( current->next_->freeCount_ == 0U );
                if( current->next_ != current ) {
                    TOOLS_ASSERT( unitFromToken( current->slabReturns_ ) == slabPoolsSaturated );
                    current->next_ = current->next_->next_;
                    if( !current->next_ && ( current->freeCount_ > 0U )) {
                        // TODO: hysteresis before selecting the root
                        current->next_ = current;
                    }
                    if( !!current->next_ ) {
                        // Pop the next entry from the list
                        TOOLS_ASSERT( current->next_->freeCount_ > 0U );
                        return current->next_->map();
                    }
                }
                size_t currentUnit = unitFromToken( current->slabReturns_ );
                if( currentUnit < slabPoolsSaturated ) {
                    // Detach this root and reallocate it in a larger space.
                    singletonAlloc = current->singleton_;
                    iSlabAlloc = currentUnit + 1ULL;
                    // Still growing and this is not detached, however we need the scale marker for when
                    // it is later freed.
                    current->next_ = current->slabReturns_;
                    current->slabReturns_ = nullptr;
                    current->singleton_ = nullptr;
                } else {
                    // Set this initially to nullptr before we refill
                    current->next_ = nullptr;
                    if( CyclicSlab * emptiest = current->slabReturns_ ) {
                        // We're going to recycle the slabs in the return list and discard the completely
                        // empty slabs. Then bubble the most empty to the front of the list.
                        TOOLS_ASSERT( current->freeCount_ == 0U );
                        current->slabReturns_ = emptiest->next_;
                        while( CyclicSlab * iSlab = current->slabReturns_ ) {
                            current->slabReturns_ = iSlab->next_;
                            if( iSlab->freeCount_ > emptiest->freeCount_ ) {
                                // bubble the emptiest to the top of the list
                                std::swap( emptiest, iSlab );
                            } else if( ( emptiest->freeCount_ == emptiest->allocMax_ ) && ( iSlab->freeCount_ == iSlab->allocMax_ )) {
                                // When we have multiple completely empty slabs, keep only one of them and
                                // return the rest to the parent allocator. This forms high watermark
                                // behavior.
                                desc.slabPools_[ slabPoolsSaturated ]->unmap( iSlab );
                                continue;
                            }
                            // Push back on the list
                            iSlab->next_ = current->next_;
                            current->next_ = iSlab;
                        }
                        // Push the emptiest on to the list
                        emptiest->next_ = current->next_;
                        current->next_ = emptiest;
                        // Re-sort and reuse it
                        return current->next_->map();
                    }
                    // No recycles pending, allocate a new slab
                    CyclicSlab * newSlab = static_cast< CyclicSlab * >( desc.slabPools_[ slabPoolsSaturated ]->map() );
                    newSlab->next_ = nullptr;
                    current->next_ = newSlab;
                    return cyclicSlabFormatAndAlloc( newSlab, unitDescs[ slabPoolsSaturated ].srcBytes_, desc );
                }
            } else {
                iSlabAlloc = desc.poolMin_;
                singletonAlloc = referenceSlabList->default();
            }
            // Figure out the right size for the first slab
            Pool * slabPool = tokenFaultPool( iSlabAlloc, desc, name );
            // slabPool is a NodeSmallPool with decently large alloc size (see poolUnits[]). When memory
            // tracking is on, allocations show up as a combination of 'name' and pool unit size.
            CyclicSlabRoot * newSlab = static_cast< CyclicSlabRoot * >( slabPool->map() );
            newSlab->next_ = newSlab;
            newSlab->singleton_ = singletonAlloc;
            newSlab->slabReturns_ = ( iSlabAlloc < slabPoolsSaturated ) ? unitDescs[ iSlabAlloc ].term_ : nullptr;
            *referenceSlabList = newSlab;
            return cyclicSlabFormatAndAlloc( newSlab, unitDescs[ iSlabAlloc ].srcBytes_ - ( sizeof( CyclicSlabRoot ) - sizeof( CyclicSlab )), desc );
        }

        // This is the slow path. We call then when trying to deallocate from a previously exhausted slab,
        // so all the rules for dispatching a slab inline are here.
        void
        cyclicSlabPoolUnmap(
            void * site,
            CyclicSlab * __restrict unmapSlab,
            CyclicSlabRoot * __restrict referenceSlabRoot,
            CyclicPoolDesc & srcPool )
        {
            if( unmapSlab != referenceSlabRoot->next_ ) {
                size_t slabUnit = unitFromToken( unmapSlab->next_ );
                if( slabUnit < slabPoolsSaturated ) {
                    // When this is both detached and unsaturated, we will try an run it down to zero
                    // allocations and release it.
                    if( --unmapSlab->allocMax_ == 0U ) {
                        // Yay!
                        srcPool.slabPools_[ slabUnit ]->unmap( static_cast< tools::detail::CyclicSlabRoot * >( unmapSlab ));
                    }
                    return;
                } else if( unmapSlab != referenceSlabRoot ) {
                    TOOLS_ASSERT( unitFromToken( unmapSlab ) == slabPoolsSaturated );
                    // This is saturated, we keep these in a list. We'll sort them before re-allocation.
                    unmapSlab->next_ = referenceSlabRoot->slabReturns_;
                    referenceSlabRoot->slabReturns_ = unmapSlab;
                }
            }
            // Reformat the head
            unmapSlab->poke( site, unmapSlab );
            unmapSlab->allocHead_ = unmapSlab->toOffset( site );
            unmapSlab->freeCount_ = 1U;
        }

        // Cleanup, called from the AllocatorCyclic destructor. Remember that, unlike most of the other
        // allocators, AllocatorCyclic has non-trivial state. This could very well execute at runtime,
        // rather than during static destruction.
        void
        cyclicSlabPoolFinalize(
            CyclicSlabRoot * __restrict referenceSlabRoot,
            CyclicPoolDesc & desc )
        {
            // Should not be here if there isn't a hint.
            TOOLS_ASSERT( !referenceSlabRoot->singleton_ );
            if( referenceSlabRoot->next_ != referenceSlabRoot ) {
                // The root is not in loopback 'mode', check its free list for slabs to free.
                while( tools::detail::CyclicSlab * slab = referenceSlabRoot->next_ ) {
                    referenceSlabRoot->next_ = slab->next_;
                    // Make sure all of the internal nodes are now free.
                    TOOLS_ASSERT( slab->freeCount_ == slab->allocMax_ );
                    desc.slabPools_[ slabPoolsSaturated ]->unmap( slab );
                }
            }
            size_t rootUnit = unitFromToken( referenceSlabRoot->slabReturns_ );
            if( rootUnit == slabPoolsSaturated ) {
                // Check for chained returns
                while( tools::detail::CyclicSlab * slab = referenceSlabRoot->slabReturns_ ) {
                    referenceSlabRoot->slabReturns_ = slab->next_;
                    // Make sure all of the nodes are now free.
                    TOOLS_ASSERT( slab->freeCount_ == slab->allocMax_ );
                    desc.slabPools_[ slabPoolsSaturated ]->unmap( slab );
                }
            }
            // Finally, delete the root itself
            TOOLS_ASSERT( referenceSlabRoot->freeCount_ == referenceSlabRoot->allocMax_ );
            desc.slabPools_[ rootUnit ]->unmap( referenceSlabRoot );
        }
    };  // detail namespace
};  // tools namespace

///////////////////
// BinaryPoolMaster
///////////////////

BinaryPoolMaster::BinaryPoolMaster(
    Pool & inner,
    impl::ResourceSample const & sample )
    : inner_( &inner )
{
    Pool::Desc innerDesc = inner.describe();
    TOOLS_ASSERT( ( innerDesc.align_ == innerDesc.size_ ) && ( innerDesc.phase_ == 0 ));
    std::fill( blocks_, blocks_ + tableSize, static_cast< BinaryBlock * >( nullptr ));
    desc_.align_ = desc_.size_ = innerDesc.size_ / 2U;
    desc_.phase_ = 0;
    impl::ResourceSample s = sample;
    s.size_ = desc_.size_;
    desc_.trace_ = impl::resourceTraceBuild( sample, innerDesc.trace_ );
}

BinaryPoolMaster::~BinaryPoolMaster( void )
{
    if( !impl::isAbnormalShutdown() ) {
        bool haveLeaks = false;
        for( size_t i=0; i!=tableSize; ++i ) {
            // Logging a memory leak
            if( !!blocks_[ i ] ) {
                haveLeaks = true;
                break;
            }
        }
        if( haveLeaks ) {
            impl::resourceTraceDump( desc_.trace_ );
        }
    }
}

Pool::Desc const &
BinaryPoolMaster::describe( void )
{
    return desc_;
}

void *
BinaryPoolMaster::map( void )
{
    size_t bias = 3;
    BinaryBlock * block = extract( &bias );
    if( !!block ) {
        BinaryBlock * next = block->next_;
        if( !!next ) {
            TOOLS_ASSERT( bias == BinaryPoolMaster::pointerBucket( next->base_ ));
            insertUnique( bias, next );
        }
        return block;
    }
    // If we get here, there were no cached moieties.  Make a new on from the parent
    // collection.
    BinaryBlock * bottom;
    void * top = BinaryPoolMaster::innerMap( &bottom, inner_, desc_.size_ );
    // Leave the bottom half in the free set
    insertUnique( BinaryPoolMaster::pointerBucket( top ), bottom );
    return top;
}

void
BinaryPoolMaster::unmap(
    void * site )
{
    TOOLS_ASSERT( ( reinterpret_cast< size_t >( site ) % desc_.size_ ) == 0 );
    BinaryBlock * block = static_cast< BinaryBlock * >( site );
    block->next_ = nullptr;
    block->base_ = reinterpret_cast< void * >( tools::roundDownPow2( reinterpret_cast< uintptr_t >( block ), desc_.size_ ));
    insertUnique( pointerBucket( block->base_ ), block );
}

BinaryBlock *
BinaryPoolMaster::extract(
    size_t * bucketBias )
{
    static_assert( ( tableSize & ( tableSize - 1 )) == 0, "bad tableSize" );  // ensure is power of 2
    size_t bias = *bucketBias;
    for( size_t i=0; i!=tableSize; ++i ) {
        size_t idx = ( i + bias ) & ( tableSize - 1U );
        if( !blocks_[ idx ] ) {
            continue;
        }
        BinaryBlock * block = atomicExchange< BinaryBlock *, BinaryBlock * >( blocks_ + idx, nullptr );
        if( !block ) {
            continue;
        }
        *bucketBias = idx;
        return block;
    }
    // None were found
    return nullptr;
}

void
BinaryPoolMaster::insertUnique(
    size_t bucket,
    BinaryBlock * next )
{
    // TODO: get rid of fasle sharing on blocks_ (add padding to make each element at least a cache line)
    while( !!atomicCas< BinaryBlock *, BinaryBlock * >( blocks_ + bucket, nullptr, next )) {
        // There are existing items in the list; perform a merge against the items in the next bucket.
        BinaryBlock * existing = atomicExchange< BinaryBlock *, BinaryBlock * >( blocks_ + bucket, nullptr );
        if( !existing ) {
            continue;
        }
        // Otherwise, merge by walking over the list
        BinaryBlock ** j = &next;
        while( !!*j ) {
            bool found = false;
            BinaryBlock ** k = &existing;
            while( !!*k ) {
                if( ( *j )->base_ != ( *k )->base_ ) {
                    k = &( *k )->next_;
                    continue;
                }
                void * toUnmap = ( *k )->base_;
                *k = ( *k )->next_;
                *j = ( *j )->next_;
                found = true;
                inner_->unmap( toUnmap );
                if( !*j ) {
                    break;
                }
            }
            if( !found ) {
                j = &( *j )->next_;
            }
        }
        *j = existing;
        // We should be at the end of j and at the end of k
        if( !next ) {
            TOOLS_ASSERT( *j == nullptr );
            // We freed the last block in this bucket, abort.
            return;
        }
    }
}

size_t
BinaryPoolMaster::pointerBucket(
    void * site )
{
    return static_cast< size_t >( impl::hashAny( site )) & ( tableSize - 1U );
}

void *
BinaryPoolMaster::innerMap(
    BinaryBlock ** bottomHalf,
    Pool * inner,
    size_t size )
{
    TOOLS_ASSERT( size * 2 == inner->describe().size_ );
    BinaryBlock * top = static_cast< BinaryBlock * >( inner->map() );
    BinaryBlock * bottom = reinterpret_cast< BinaryBlock * >( reinterpret_cast< uint8 * >( top ) + size );
    bottom->base_ = top;
    bottom->next_ = nullptr;
    *bottomHalf = bottom;
    return top;
}

////////////
// ProxyPool
////////////

ProxyPool::ProxyPool(
    Pool & inner,
    size_t rephase )
    : next_( nullptr )
    , desc_( inner.describe() )
    , inner_( &inner )
{
    TOOLS_ASSERT( desc_.phase_ == 0 );
    // Slide back one alignment unit and rephase
    desc_.phase_ = rephase;
    desc_.size_ -= phaseDiff();
}

Pool::Desc const &
ProxyPool::describe( void )
{
    return desc_;
}

void *
ProxyPool::map( void )
{
    return static_cast< uint8 * >( inner_->map() ) + phaseDiff();
}

void
ProxyPool::unmap(
    void * site )
{
    inner_->unmap( static_cast< uint8 * >( site ) - phaseDiff() );
}

bool
ProxyPool::operator==(
    ProxyPool const & right ) const
{
    return ( desc_.size_ == right.desc_.size_ ) &&
        ( desc_.phase_ == right.desc_.phase_ ) &&
        ( desc_.align_ == right.desc_.align_ );
}

bool
ProxyPool::operator==(
    AlignSpec const & right ) const
{
    return ( desc_.size_ == right.alignAlloc_ ) &&
        ( desc_.phase_ == right.phase_ ) &&
        ( desc_.align_ == right.alignBytes_ );
}

size_t
ProxyPool::phaseDiff( void ) const
{
    return ( desc_.align_ - ( desc_.phase_ & ( desc_.align_ - 1 )));
}

/////////////////////////////////////
// AffinityInherentBase::PoolInstance
/////////////////////////////////////

#ifdef WINDOWS_PLATFORM
AffinityInherentBase::PoolInstance::PoolInstance( void )
{
}

AffinityInherentBase::PoolInstance::PoolInstance( PoolInstance const & )
{
    TOOLS_ASSERT( !"Copy of PoolInstance is not supported" );
}

AffinityInherentBase::PoolInstance &
AffinityInherentBase::PoolInstance::operator=(
    PoolInstance const & )
{
    TOOLS_ASSERT( !"Assignment of PoolInstance is not supported" );
    return *this;
}
#endif // WINDOWS_PLATFORM

AffinityInherentBase::PoolInstance::PoolInstance(
    PoolInstance && r )
    : next_( r.next_ )
    , size_( r.size_ )
    , phase_( r.phase_ )
    , pool_( r.pool_ )
    , poolDispose_( std::move( r.poolDispose_ ))
{
    r.next_ = nullptr;
    r.size_ = 0U;
    r.phase_ = 0U;
    r.pool_ = nullptr;
}

AffinityInherentBase::PoolInstance &
AffinityInherentBase::PoolInstance::operator=(
    PoolInstance && r )
{
    next_ = r.next_;
    size_ = r.size_;
    phase_ = r.phase_;
    pool_ = r.pool_;
    poolDispose_ = std::move( r.poolDispose_ );
    r.next_ = nullptr;
    r.size_ = 0U;
    r.phase_ = 0U;
    r.pool_ = nullptr;
    return *this;
}

bool
AffinityInherentBase::PoolInstance::operator==(
    PoolInstance const & right ) const
{
    return ( size_ == right.size_ ) && ( phase_ == right.phase_ );
}

bool
AffinityInherentBase::PoolInstance::operator==(
    AlignSpec const & right ) const
{
    return ( size_ == right.size_ ) && ( phase_ == right.phase_ );
}

bool
AffinityInherentBase::PoolInstance::operator<(
    PoolInstance const & right ) const
{
    if( size_ == right.size_ ) {
        return phase_ > right.phase_;
    }
    return size_ < right.size_;
}

///////////////////////
// AffinityInherentBase
///////////////////////

AffinityInherentBase::AffinityInherentBase(
    Affinity & inner )
    : inner_( &inner )
{
    pagePools_.clear();
    linePools_.clear();
    proxyPools_.clear();
}

AffinityInherentBase::~AffinityInherentBase( void )
{
    // Shutdown the proxy pools
    shutdownPools();
}

Pool &
AffinityInherentBase::pool(
    size_t size,
    impl::ResourceSample const &,
    size_t phase )
{
    // Special-case the largest pools that are specifically request instead of going through a re-alignment
    // cycle. This should include (at least) all Temporal backing slab sizes we use. Presently this is:
    // 32K, 256K, and 2MB. All of which need to be aligned to their slab sizes.
    if( ( phase == 0 ) && ( ( size == ( 32U * 1024U )) || (size == ( 64U * 1024U )) ||
        ( size == ( 128U * 1024U )) || ( size == ( 256U * 1024U )) ||
        ( size == ( 512U * 1024U )) || ( size == ( 1024U * 1024 )) ||
        ( size == ( 2048U * 1024U )))) {
        // Make a synthetic alignment spec
        AlignSpec spec;
        spec.alignAlloc_ = size;
        spec.alignBytes_ = size;
        spec.model_ = alignModelPage;
        spec.phase_ = 0U;
        spec.placeBytes_ = size;
        spec.scale_ = alignScaleUnique;
        spec.size_ = size;
        return findPool( pagePools_, spec );
    }
    // Round the size up to the next word
    size = tools::roundUpPow2( size, sizeof( void * ));
    AlignSpec spec;
    alignSpecOf( &spec, size, phase, false );
    if( spec.scale_ == alignScaleLine ) {
        // For these, we always look for an exact match for phase and spec.
        return findPool( linePools_, spec );
    }
    // Correct the specification for large bucket sizes
    normalize( &spec );
    if( phase == 0 ) {
        // This should pad out to a 4k alignment
        spec.size_ = spec.alignAlloc_;
        return findPool( pagePools_, spec );
    }
    // Create a proxy to a standard pool
    return findProxy( spec );
}

void *
AffinityInherentBase::map(
    size_t size,
    impl::ResourceSample const & sample,
    size_t phase )
{
    AlignSpec spec;
    alignSpecOf( &spec, size + sizeof( Unmapper * ), phase + sizeof( Unmapper * ));
    void * site = nullptr;
    Unmapper * umap;
    if( spec.scale_ == alignScaleLine ) {
        // Redispatch to the inner allocator (small allocation
        umap = inner_;
        site = inner_->map( spec.alignAlloc_, sample );
    } else if( spec.alignAlloc_ > ( 2U * 1024U * 1024U )) {
        // Huge allocation, pass off to inner
        umap = inner_;
        site = inner_->map( spec.alignAlloc_, sample );
    } else {
        normalize( &spec );
        AlignSpec flatSpec = spec;
        flatten( &flatSpec );
        Pool * src = &pool( flatSpec.size_, sample, flatSpec.phase_ );
        // Redispatch to one of our pools, because the inner is assumed to handle
        // page scale allocations poorly.
        umap = src;
        site = src->map();
    }
    // TERRIBLE HACK!  Track the actual allocation size instead of that requested.
    const_cast< impl::ResourceSample & >( sample ).size_ = spec.alignAlloc_;
    Unmapper ** head = static_cast< Unmapper ** >( alignAlloc( spec, site ));
    *head = umap;
    return head + 1;
}

void
AffinityInherentBase::unmap(
    void * site )
{
    // Unwrap the allocation policy and alignment.
    Unmapper ** umap = static_cast< Unmapper ** >( site ) - 1;
    ( *umap )->unmap( unalignAlloc( umap ));
}

uint32
AffinityInherentBase::hashAlignSpec(
    AlignSpec const & spec )
{
    return HashAnyOf< AlignSpec >()( spec );
}

void
AffinityInherentBase::normalize(
    AlignSpec * spec )
{
    // Check if we need to make a size correction
    if( spec->scale_ == alignScaleUnique && !isPow2( spec->alignAlloc_ )) {
        // If unique, fix the alignment to the next power of two.  We are not too
        // worried about precision, we just want to prevent tunneling.
#if (__SIZEOF_POINTER__ == 8) || (_INTEGRAL_MAX_BITS == 64)
        spec->alignAlloc_ = roundToPow2( static_cast< uint64 >( spec->alignAlloc_ ));
#else
        spec->alignAlloc_ = roundToPow2( static_cast< uint32 > ( spec->alignAlloc_ ));
#endif
    }
}

void
AffinityInherentBase::flatten(
    AlignSpec * spec )
{
    if( spec->phase_ ) {
        spec->size_ = spec->alignAlloc_;
        spec->placeBytes_ = spec->alignAlloc_;
        spec->phase_ = 0U;
    }
}

bool
AffinityInherentBase::parentPoolOf(
    size_t * size,
    AlignScale scale,
    size_t poolSize )
{
    if( poolSize == ( 2U * 1024U * 1024U )) {
        return false;
    }
    *size = poolSize;
    if( scale == alignScaleUnique ) {
        *size = poolSize * 2U;
        return true;
    }
    if( scale == alignScaleLine ) {
        // A line scale size goes into the smallest unique pool because lesser
        // pools are not aligned.
        *size = 256U * 1024U;
        return true;
    }
    if( poolSize >= ( 128U * 1024U )) {
        *size = 2U * 1024U * 1024U;
    } else if( poolSize >= ( 64U * 1024U )) {
        *size = 1024U * 1024U;
    } else {
        *size = 512U * 1024U;
    }
    return true;
}

void
AffinityInherentBase::clearInstances(
    PoolInstance * inst )
{
    // Collect and sort the instance before delete.
    std::vector< PoolInstance > preDelete;
    PoolInstance * next;
    for( PoolInstance * i=inst; !!i; i=next ) {
        next = i->next_;
        if( !!i->poolDispose_ ) {
            preDelete.push_back( std::move( *i ));
        }
        delete i;
    }
    std::sort( preDelete.begin(), preDelete.end() );
    for( auto && del : preDelete ) {
        AutoDispose<> toDisp( std::move( del.poolDispose_ ));
    }
}

uint32
AffinityInherentBase::alignSpecProxyHash(
    AlignSpec const & spec )
{
    return tools::hashAnyBegin( spec ) % spec.alignAlloc_ % spec.phase_ % spec.alignBytes_;
}

Pool &
AffinityInherentBase::findProxy(
    AlignSpec const & spec )
{
    uint32 hash = alignSpecProxyHash( spec );
    ProxyPool * inst = proxyPools_.peek( hash, spec );
    if( !!inst ) {
        return *inst;
    }
    TOOLS_ASSERT( spec.scale_ != alignScaleLine );
    // At this scale, we expect an exact number of pages.
    TOOLS_ASSERT( ( spec.alignAlloc_ & ( 4096U - 1U )) == 0U );
    // Create the flat space we'll align into.
    AlignSpec src = spec;
    flatten( &src );
    // Keep these in case here's a race inserting a pool of this exact size.
    std::auto_ptr< ProxyPool > next( new ProxyPool( findPool( pagePools_, src ), spec.phase_ ));
    ProxyPool * inserted = proxyPools_.insert( hash, next.get() );
    if( inserted == next.get() ) {
        // Yay! Our instance was inserted!
        next.release();
    }
    return *inserted;
}

void
AffinityInherentBase::shutdownNonUniquePools( void )
{
    ProxyPool * next;
    for( ProxyPool * i=proxyPools_.detach(); !!i; i=next ) {
        next = i->next_;
        delete i;
    }
    clearInstances( linePools_.detach() );
    // Special-case the unique pools out of circulation.
    clearInstances( pagePools_.detachIf( IsNonUniquePool() ));
}

void
AffinityInherentBase::shutdownPools( void )
{
    shutdownNonUniquePools();
    clearInstances( pagePools_.detach() );
}

//////////////////////////////
// AffinityInherentThreadLocal
//////////////////////////////

AffinityInherentThreadLocal::AffinityInherentThreadLocal(
    AffinityInherentMaster & master,
    Affinity & inner,
    bool bind )
    : AffinityInherentBase( inner )
    , root_( bind ? &master.root_->bind() : &master )
    , master_( &master )
{
    std::fill( uniquePools_, uniquePools_ + binaryMasterSizeMax, static_cast< Pool * >( nullptr ));
}

AffinityInherentThreadLocal::~AffinityInherentThreadLocal( void )
{
    shutdownPools();
}

AutoDispose<>
AffinityInherentThreadLocal::newPool(
    Pool ** referencePool,
    AlignSpec const & spec )
{
    if( spec.scale_ == alignScaleUnique ) {
        if( spec.size_ == ( 2U * 1024U * 1024U )) {
            // Never keep a per-thread buffer of these
            *referencePool = &root_->pool( spec.size_ );
            return nullptr;
        }
        // The binary pools should be non-competetive
        BinaryMasterSize idx = binaryMasterSizeFromSize( spec.size_ );
        TOOLS_ASSERT( !uniquePools_[ idx ] );
        AutoDispose<> disp( poolBinaryLocalBufferNew( referencePool,
            *master_->binaryMasterOf( spec.size_ )));
        uniquePools_[ idx ] = *referencePool;
        return std::move( disp );
    }
    // Ordinary unsynchronized node allocator applied to one of the roots
    size_t parentSize = 0U;
    parentPoolOf( &parentSize, spec.scale_, spec.alignAlloc_ );
    return poolNodeNew( referencePool, ( master_ == master_->root_ ) ?
        pool( parentSize, TOOLS_RESOURCE_SAMPLE_CALLER( parentSize ), 0U ) :
        root_->pool( parentSize, TOOLS_RESOURCE_SAMPLE_CALLER( parentSize ) ),
        master_->sample_, spec.size_, spec.phase_ );
}

Affinity &
AffinityInherentThreadLocal::bind( void )
{
    // Already bound, but thanks for asking
    return *this;
}

AutoDispose<>
AffinityInherentThreadLocal::fork(
    Affinity ** referenceAffinity,
    impl::ResourceSample const & sample )
{
    // Move to a single-threaded implementation from the same root
    AffinityInherentForkBound * forked = new( sample ) AffinityInherentForkBound( *inner_,
        sample, *root_ );
    *referenceAffinity = forked;
    return std::move(forked);
}

/////////////////////////////////////
// AffinityInherentMaster::FanoutPool
/////////////////////////////////////

AffinityInherentMaster::FanoutPool::FanoutPool(
    AffinityInherentMaster & parent,
    StandardThreadLocalHandle< AffinityInherentThreadLocal > & local,
    BinaryMasterSize idx )
    : parent_( &parent )
    , threadLocal_( &local )
    , idx_( idx )
{
}

Pool::Desc const &
AffinityInherentMaster::FanoutPool::describe( void )
{
    return localPool().describe();
}

void *
AffinityInherentMaster::FanoutPool::map( void )
{
    return localPool().map();
}

void
AffinityInherentMaster::FanoutPool::unmap(
    void * site )
{
    peekLocalPool().unmap( site );
}

Pool &
AffinityInherentMaster::FanoutPool::localPool( void )
{
    AffinityInherentThreadLocal & local = **threadLocal_;
    Pool * pool = local.uniquePools_[ idx_ ];
    if( !pool ) {
        size_t size = sizeFromBinaryMasterSize( idx_ );
        pool = &local.pool( size, TOOLS_RESOURCE_SAMPLE_CALLER( size ), 0U );
        TOOLS_ASSERT( pool == local.uniquePools_[ idx_ ] );
    }
    return *pool;
}

Pool &
AffinityInherentMaster::FanoutPool::peekLocalPool( void )
{
    AffinityInherentThreadLocal * local = threadLocal_->peek();
    if( !local || !local->uniquePools_[ idx_ ] ) {
        size_t size = sizeFromBinaryMasterSize( idx_ );
        return *parent_->binaryMasterOf( size );
    }
    return *local->uniquePools_[ idx_ ];
}

/////////////////////////
// AffinityInherentMaster
/////////////////////////

AffinityInherentMaster::AffinityInherentMaster(
    Affinity & inner,
    Affinity * root,
    impl::ResourceSample const & sample,
    size_t poolMax,
    bool bind )
    : AffinityInherentBase( inner )
    , root_( root )
    , sample_( sample )
    , poolMaxBuffer_( poolMax )
    , threadLocal_( [ this, &inner, bind ]( AffinityInherentThreadLocal ** ref ) { return anyDisposableAllocNew< AllocStatic< Platform >>( ref, *this, inner, bind ); })
{
    if( !root ) {
        // self-rooted
        root_ = this;
    }
    // The threaded pools are specifically arranged to vector through a 16k pool into a 2MB pool. Only
    // allocations under 128 bytes use this as a partent.
    poolParentOwner_ = poolNodeSyncNew( &poolParent_, Affinity::pool( 2048U * 1024U ), TOOLS_RESOURCE_SAMPLE_CALLER( 0 ), 16384U, 0U, 16384U );
}

AffinityInherentMaster::~AffinityInherentMaster( void )
{
    // Shutdown non-unique pools, then the thread-local versions, then the masters.
    shutdownNonUniquePools();
    // The non-unique pools should have released all of their blocks back here. This will kill the reference
    // that this keeps on the 2MB pool.
    poolParentOwner_.reset();
}

Affinity &
AffinityInherentMaster::bind( void )
{
    return *threadLocal_;
}

AutoDispose<>
AffinityInherentMaster::fork(
    Affinity ** referenceAffinity,
    impl::ResourceSample const & sample )
{
    AffinityInherentMaster * forked = new( sample ) AffinityInherentMaster( *inner_, root_, sample, 16, true );
    *referenceAffinity = forked;
    return std::move(forked);
}

BinaryPoolMaster *
AffinityInherentMaster::binaryMasterOf(
    size_t size )
{
    BinaryMasterSize idx = binaryMasterSizeFromSize( size );
    BinaryPoolMaster * next = nullptr;
    BinaryPoolMaster * prev;
    do {
        prev = masters_[ idx ].get();
        if( !!prev ) {
            break;
        }
        if( !next ) {
            size_t parentSize = size * 2U;
            Pool & parentPool = ( parentSize == ( 2U * 1024U * 1024U )) ?
                root_->pool( parentSize ) : *binaryMasterOf( parentSize );
            next = new BinaryPoolMaster( parentPool, sample_ );
        }
    } while( masters_[ idx ].compareAndSwap( prev, next ) != prev );
    if( !!prev ) {
        if( !!next ) {
            delete next;
        }
        return prev;
    }
    return next;
}

AutoDispose<>
AffinityInherentMaster::newPool(
    Pool ** referencePool,
    AlignSpec const & spec )
{
    if( spec.scale_ == alignScaleUnique ) {
        if( spec.size_ == ( 2U * 1024U * 1024U )) {
            // Create the buffering pool.
            impl::ResourceSample sample = sample_;
            sample.size_ = spec.size_;
            return poolBufferNew( referencePool, inner_->pool( spec.size_ ), sample, poolMaxBuffer_ );
        }
        // We do per-thread pooling for these.  We return a proxy that will redirect to
        // the current thread.
        FanoutPool * newPool = new FanoutPool( *this, threadLocal_, binaryMasterSizeFromSize( spec.size_ ));
        *referencePool = newPool;
        return std::move(newPool);
    }
#ifdef TOOLS_NEW_PAGE_POOL
    // Other scales are implemented as ordinary sync node pools made against a super-block
    // in the root.
    if( ( spec.scale_ == alignScaleLine ) && ( spec.size < ( 4096U - 64U ))) {
        return poolUaVmemNew( referencePool, sample_, spec.size_, spec.phase_, 1U );
    }
#endif // TOOLS_NEW_PAGE_POOL
    if( spec.size_ < 128U ) {
        // This is a tiny allocation, create a threaded pool
        return poolNodeThreadedNew( referencePool, *poolParent_, sample_, spec.size_, spec.phase_ );
    }
    size_t parentSize = 0U;
    parentPoolOf( &parentSize, spec.scale_, spec.alignAlloc_ );
    return poolNodeSyncNew( referencePool, root_->pool( parentSize, 0U ), sample_, spec.size_, spec.phase_ );
}

/////////////////
// VerifyPoolBase
/////////////////

VerifyPoolBase::VerifyPoolBase(
    unsigned trackingInterval,
    Pool & inner,
    impl::ResourceTrace * mapTrace,
    size_t mapTraceElements )
    : inner_( &inner )
    , innerDesc_( inner.describe() )
    , mapTrace_( mapTrace )
    , mapTraceElements_( mapTraceElements )
    , trackingInterval_( trackingInterval )
{
    TOOLS_ASSERT( trackingInterval_ == 1 );  // we always inc/dec
    TOOLS_ASSERT( mapTrace_->interval() == 1 );
    TOOLS_ASSERT( mapTraceElements_ > 0U );
    TOOLS_ASSERT( innerDesc_.align_ > 0 );
    TOOLS_ASSERT( innerDesc_.size_ > 0 );
    TOOLS_ASSERT( innerDesc_.phase_ <= innerDesc_.size_ );
}

Pool::Desc const &
VerifyPoolBase::describe( void )
{
    return inner_->describe();
}

void *
VerifyPoolBase::map( void )
{
    void * ret = inner_->map();
    if( !ret ) {
        impl::outOfMemoryDie();
    }
    // Memory tracking and intervals:
    // poolMap() allocations currently all have the same ResourceTrace.  Not too concerned with CPU
    // overhead in ResourceTrace hash table lookups.  The only savings that we'd get from paying attention
    // to trackingInterval is a reduction in atomic inc/dec.  That may be worthwhile someday.
    //
    // In any event, it's currently up to whoever created the ResourceTrace to set its tracking interval.
    // At the moment, it's set by our caller, so we respect its tracking interval.
    TOOLS_ASSERT( trackingInterval_ == 1 );
    mapTrace_->inc( mapTraceElements_ );
    getTotalTrackedMemory() += mapTrace_->size() * mapTraceElements_; // typically larger than innerDesc_.size_
    TOOLS_ASSERT( (( reinterpret_cast< size_t >( ret ) + innerDesc_.phase_ ) % innerDesc_.align_ ) == 0 );
    if( impl::memoryPoison() ) {
        impl::memset( ret, verifyHeapAlloc, std::min( innerDesc_.size_, static_cast< size_t >( 65536U )));
    }
    return ret;
}

void
VerifyPoolBase::unmap(
    void * site )
{
    TOOLS_ASSERT( (( reinterpret_cast< size_t >( site ) + innerDesc_.phase_ ) % innerDesc_.align_ ) == 0 );
    if( impl::memoryPoison() ) {
        // This should detect double frees without too many false positives.  Assume the free space tracking
        // (if any) fits into a single word.
        TOOLS_ASSERT( ( innerDesc_.size_ == sizeof( void * )) || !impl::regionIsUnmapped( static_cast< void ** >( site ) + 1, innerDesc_.size_ - sizeof( void * )));
        impl::memset( site, verifyHeapFree, std::min( innerDesc_.size_, static_cast< size_t >( 65536U )));
    }
    getTotalTrackedMemory() -= mapTrace_->size() * mapTraceElements_; // larger than innerDesc_.size_
    mapTrace_->dec( mapTraceElements_ );
    inner_->unmap( site );
}

/////////////
// VerifyPool
/////////////

VerifyPool::VerifyPool(
    unsigned trackingInterval,
    Pool & inner,
    impl::ResourceTrace * mapTrace,
    size_t mapTraceElements )
    : VerifyPoolBase( trackingInterval, inner, mapTrace, mapTraceElements )
{
    TOOLS_ASSERT( trackingInterval == 1 );
}

/////////////////////
// VerifyAffinityBase
/////////////////////

VerifyAffinityBase::VerifyAffinityBase(
    unsigned trackingInterval,
    Affinity & inner,
    impl::ResourceTrace * target,
    unsigned volatile * forkOut,
    bool checkRt )
    : VerifyHeapBase< Affinity >( trackingInterval, inner, target, checkRt )
    , poolsLock_( monitorStaticNew( StringIdNull(), Monitor::PolicyAllowPriorityInversion ))
    , forks_( 0U )
    , forkRefs_( forkOut )
{
    if( !!forkRefs_ ) {
        atomicIncrement( forkRefs_ );
    }
}

VerifyAffinityBase::~VerifyAffinityBase( void )
{
    if( !!forkRefs_ ) {
        atomicDecrement( forkRefs_ );
    }
}

AutoDispose<>
VerifyAffinityBase::fork(
    Affinity ** referenceAffinity,
    impl::ResourceSample const & sample )
{
    impl::ResourceTrace * target = impl::resourceTraceBuild( sample, target_ );
    Affinity * forked;
    AutoDispose<> forkedDispose( std::move( inner_->fork( &forked, sample )));
    return affinityVerifyNew( trackingInterval_, referenceAffinity, forked, forkedDispose.get(), target,
        &forks_ );
}

Pool &
VerifyAffinityBase::pool(
    size_t size,
    impl::ResourceSample const & sample,
    size_t phase )
{
    Pool & innerPool = inner_->pool( size, sample, phase );
    unsigned const poolTrackingInterval = 1;  // Pools need to track every operation
    PoolId id = { size, phase, impl::resourceTraceBuild( poolTrackingInterval, sample, target_ ) };
    // If the VerifyPoolBase already exists, return that.
    // Note: the ResourceTrace is part of the search, not just the size, so it's possible tohave many
    // different 2MB pools each with distinct tracking, if desired.  Just supply a non-trivial
    // ResourceSample to get a distinct resource line.
    {
        AutoDispose<> l_( poolsLock_->enter() );
        auto i=pools_.find( id );
        if( i != pools_.end() ) {
            TOOLS_ASSERT( i->second.inner_ == &innerPool );
            return i->second;
        }
    }
    // Otherwise, create a new VerifyPoolBase
    VerifyPoolBase newPool( poolTrackingInterval, innerPool, id.trace_ );
    // Perform the insert under lock
    AutoDispose<> l_( poolsLock_->enter() );
    auto i = pools_.find( id );
    if( i != pools_.end() ) {
        // Race, item was added while we were getting the lock
        TOOLS_ASSERT( i->second.inner_ == &innerPool );
    } else {
        i = pools_.insert( PoolMap::value_type( id, newPool )).first;
    }
    return i->second;
}

//////////////////////
// VerifyAffinityBound
//////////////////////

VerifyAffinityBound::VerifyAffinityBound(
    unsigned trackingInterval,
    Affinity & inner,
    impl::ResourceTrace * target )
    : VerifyAffinityBase( trackingInterval, inner.bind(), target, nullptr )
{
}

Affinity &
VerifyAffinityBound::bind( void )
{
    // No further derivation
    return *this;
}

/////////////////
// VerifyAffinity
/////////////////

VerifyAffinity::VerifyAffinity(
    unsigned trackingInterval,
    Affinity & inner,
    Disposable * innerDispose,
    impl::ResourceTrace * target,
    unsigned volatile * forkRefs,
    bool checkRt )
    : VerifyAffinityBase( trackingInterval, inner, target, forkRefs, checkRt )
    , threadLocal_( [ trackingInterval, &inner, target ]( VerifyAffinityBound ** ref ) {
            return anyDisposableAllocNew< AllocStatic< Platform >>( ref, trackingInterval, inner, target );
        })
    , innerDispose_( innerDispose )
{
}

Affinity &
VerifyAffinity::bind( void )
{
    // Get the thread-local derivation
    return *threadLocal_;
}

/////////////////////
// VmemPoolUniqueAddr
/////////////////////

VmemPoolUniqueAddr::VmemPoolUniqueAddr(
    impl::ResourceSample const & sample,
    size_t size,
    size_t phase,
    unsigned /*level*/ )
    : lock_( monitorStaticNew() )
    , regionBegin_( nullptr )
    , regionEnd_( nullptr )
    , regionNext_( nullptr )
    , reserveNext_( nullptr )
{
    alignSpecOf( &spec_, size, phase, false );
    desc_.size_ = size;
    desc_.phase_ = phase;
    desc_.align_ = spec_.alignBytes_;
    impl::ResourceSample localSample = sample;
    localSample.size_ = vmemPageSize;
    desc_.trace_ = impl::resourceTraceBuild( localSample );
}

VmemPoolUniqueAddr::~VmemPoolUniqueAddr( void )
{
    unsigned refsLeft = static_cast< unsigned >( ( regionEnd_ - regionNext_ ) / vmemPageSize );
    if( refsLeft != 0 ) {
        TOOLS_ASSERT( regionBegin_->refs_ == refsLeft );
        impl::vmemPoolRelease( regionBegin_ );
    }
}

Pool::Desc const &
VmemPoolUniqueAddr::describe( void )
{
    return desc_;
}

void *
VmemPoolUniqueAddr::map( void )
{
    uint8 * pageHead;
    {
        AutoDispose<> l( lock_->enter() );
        if( regionNext_ == regionEnd_ ) {
            // Format the new region
            do {
                regionBegin_ = static_cast< RegionHead * >( impl::vmemPoolMap( reserveNext_ ));
                reserveNext_ += vmemRegionSize;
            } while( !regionBegin_ );
            regionBegin_->refs_ = ( vmemRegionSize / vmemPageSize ) - 1U;
            regionNext_ = reinterpret_cast< uint8 * >( regionBegin_ ) + vmemPageSize;
            regionEnd_ = reinterpret_cast< uint8 * >( regionBegin_ ) + vmemRegionSize;
            if( regionEnd_ > reserveNext_ ) {
                // The address was interpreted as a hint, but we got repositioned.  Accept the reposition
                // and move forward.
                reserveNext_ = regionEnd_;
            }
        }
        pageHead = regionNext_;
        *reinterpret_cast< RegionHead ** >( regionNext_ ) = regionBegin_;
        regionNext_ += vmemPageSize;
    }
    // Align the final value, because it's less than 4k.  This should always work.
    return alignPlace( spec_, pageHead + sizeof( RegionHead *[ 1 ] ), pageHead + vmemPageSize );
}

void
VmemPoolUniqueAddr::unmap( void * site )
{
    unmapAlloc( site );
}

void
VmemPoolUniqueAddr::unmapAlloc( void * site )
{
    RegionHead * region = regionOf( site );
    // TODO: verify overflow
    unsigned prevRefs;
    bool decommitted = false;
    do {
        prevRefs = region->refs_;
        if( prevRefs > 1U ) {
            if( !decommitted ) {
                impl::vmemPoolDecommit( pageOf( site ));
                decommitted = true;
            }
        }
    } while( atomicCas( &region->refs_, prevRefs, prevRefs - 1U ) != prevRefs );
    if( prevRefs == 1U ) {
        // release everything
        impl::vmemPoolRelease( region );
    }
}

///////////////
// HeapHugeImpl
///////////////

HeapHugeImpl::HeapHugeImpl( void )
{
    parentTrace_ = impl::resourceTraceBuild( "platform huge alloc" );
}

void *
HeapHugeImpl::map( size_t size, impl::ResourceSample const & sample, size_t phase )
{
    impl::ResourceTrace * trace = nullptr;
    if( impl::memoryTrack() ) {
        trace = impl::resourceTraceBuild( interval, sample, parentTrace_ );
        trace->inc();
    }
    size_t userSize = size - phase;
    userSize = roundUpPow2( userSize, pageSize );
    size_t adjustedPhase = phase + sizeof( Prefix );
    size_t headerSize = roundUpPow2( adjustedPhase, pageSize );
    void * allocAddr = impl::platformHugeAlloc( headerSize + userSize );
    TOOLS_ASSERT( phase <= headerSize );
    uint8 * addr = static_cast< uint8 * >( allocAddr ) + headerSize - phase;
    Prefix * prefix = reinterpret_cast< Prefix * >( addr - sizeof( Prefix ));
    prefix->trace_ = trace;
    prefix->mmappedAddr_ = allocAddr;
    prefix->mmappedSize_ = headerSize + userSize;
    return static_cast< void * >( addr );
}

void
HeapHugeImpl::unmap( void * site )
{
    Prefix * prefix = reinterpret_cast< Prefix * >( static_cast< uint8 * >( site ) - sizeof( Prefix ));
    if( impl::memoryTrack() ) {
        prefix->trace_->dec();
    }
    impl::platformHugeFree( prefix->mmappedAddr_, prefix->mmappedSize_ );
}

/////////////////
// MemoryDumpTask
/////////////////

MemoryDumpTask::MemoryDumpTask( ThreadScheduler & sched )
    : nesting_( 0 )
    , scheduler_( sched )
    , dumpIntervalNs_( 30 * TOOLS_NANOSECONDS_PER_SECOND )
{
    // TODO: get a better interval from config
}

void
MemoryDumpTask::execute( void )
{
    TOOLS_ASSERT( nesting_ == 1 );
    impl::resourceTraceDump( phase_, false, &storage_ );
    if( !!atomicDecrement( &nesting_ )) {
        TOOLS_ASSERT( !"Broken MemoryDumpTask nesting count" );
    }
}

void
MemoryDumpTask::checkUsage( uint64 curTime )
{
    if( dumpIntervalNs_ == 0 ) {
        return;  // disabled, do nothing
    }
    if( nesting_ > 0 ) {
        return;  // already spawned
    }
    if( !!atomicTryUpdate( &lastDumpNs_, [=]( uint64 * ref )->bool {
            if( curTime >= ( *ref + dumpIntervalNs_ )) {
                *ref = impl::getHighResTime();
                return true;
            }
            return false;
        })) {
        tryLaunch( impl::resourceTraceDumpPhasePeriodic );
        return;
    }
    // Check for new high water mark for memory.
    if( !!atomicTryUpdate( &lastDumpBytes_, []( uint64 * numBytes )->bool {
            // numBytes is the old value.  Here we compute a new desired value and return true.  False,
            // if there is no need to dump.
            uint64 cur = static_cast< uint64 >( getTotalTrackedMemory() ) /* TODO: + platformUntrackedMemory()*/;
            uint64 lastSize = *numBytes;
            uint64 thresh = std::min( lastSize + ( lastSize >> 3 ), lastSize + ( 512 * 1024 * 1024 ));
            bool run = ( cur >= thresh ) && ( cur >= ( 1024ULL * 1024 * 1024 ));
            if( run ) {
                *numBytes = cur;
            }
            return run;
        })) {
        tryLaunch( impl::resourceTraceDumpPhaseWatermark );
    }
}

void
MemoryDumpTask::tryLaunch( impl::ResourceTraceDumpPhase dumpPhase )
{
    unsigned oldNesting = atomicAdd< unsigned >( &nesting_, 1 );
    if( oldNesting > 0 ) {
        // spawn already pending
        return;
    }
    phase_ = dumpPhase;
    scheduler_.spawn( *this, scheduler_.defaultParam() );
}

////////////////////////////
// AffinityInherentForkBound
////////////////////////////

AffinityInherentForkBound::AffinityInherentForkBound(
    Affinity & inner,
    impl::ResourceSample const & sample,
    Affinity & root )
    : AffinityInherentBase( inner )
    , root_( &root )
    , sample_( sample )
{}

AutoDispose<>
AffinityInherentForkBound::newPool(
    Pool ** referencePool,
    AlignSpec const & spec )
{
    size_t parentSize;
    if( !parentPoolOf( &parentSize, spec.scale_, spec.alignAlloc_ )) {
        // This is a maximum size pool, pass the request through.
        // TODO: rebuffer a few allocations locally
        *referencePool = &root_->pool( spec.alignAlloc_ );
        return nullptr;
    }
    if( spec.scale_ == alignScaleUnique ) {
        // The largest scale uses a binary pool
        return poolBinaryNew( referencePool, root_->pool( parentSize ), sample_ );
    }
    // Otherwise, this is an ordinary node pool. As this type only exists on a single thread,
    // we can use the much faster poolNodeNew as opposed to poolNodeSyncNew.
    return poolNodeNew( referencePool, root_->pool( parentSize ), sample_, spec.size_,
        spec.phase_ );
}

Affinity &
AffinityInherentForkBound::bind( void )
{
    return *this;
}

AutoDispose<>
AffinityInherentForkBound::fork(
    Affinity ** referenceAffinity,
    impl::ResourceSample const & sample )
{
    // Make another one of these
    AffinityInherentForkBound * forkOf = new( sample ) AffinityInherentForkBound( *inner_, sample, *root_ );
    *referenceAffinity = forkOf;
    return std::move(forkOf);
}

/////////////////////////
// BinaryPoolThreadBuffer
/////////////////////////

BinaryPoolThreadBuffer::BinaryPoolThreadBuffer(
    BinaryPoolMaster * master )
    : master_( master )
    , inner_( master->inner_ )
    , parentSize_( master->desc_.size_ )
    , mapOf_( nullptr )
    , unmappedUsed_( 0 )
    , unmappedMax_( static_cast< unsigned >( ( 1024ULL * 1024ULL ) / master->desc_.size_ ))
{
    threadBias_ = static_cast< size_t >( HashAnyOf< BinaryPoolThreadBuffer >()( *this )) &
        ( BinaryPoolMaster::tableSize - 1U );
}

BinaryPoolThreadBuffer::~BinaryPoolThreadBuffer( void )
{
    if( !!mapOf_ ) {
        master_->unmap( mapOf_ );
    }
    // Pass everything back to the parent for reduction
    TOOLS_ASSERT( unmappedUsed_ <= unmappedMax_ );
    while( unmappedUsed_ != 0 ) {
        master_->unmap( unmapped_[ --unmappedUsed_ ]);
    }
}

Pool::Desc const &
BinaryPoolThreadBuffer::describe( void )
{
    return master_->desc_;
}

#ifdef WINDOWS_PLATFORM
#  pragma warning( disable : 4706 )
#endif // WINDOWS_PLATFORM
void *
BinaryPoolThreadBuffer::map( void )
{
    if( !!mapOf_ ) {
        // Unconditionally return the mapOf_.  This should help clustering slightly.
        void * ret = mapOf_;
        mapOf_ = nullptr;
        return ret;
    }
    TOOLS_ASSERT( unmappedUsed_ <= unmappedMax_ );
    if( unmappedUsed_ != 0 ) {
        // Return from our unmated pool.
        return unmapped_[ --unmappedUsed_ ];
    }
    // If this is an underflow, go to the parent to pull unmaps and pull a bucket of them.
    size_t bucketBias = threadBias_;
    // Refill to 3/4 full (or 1)
    unsigned unmappedTarget = std::max( unmappedMax_ * 3 / 4, 1U );
    TOOLS_ASSERT( unmappedUsed_ <= unmappedMax_ );
    while( unmappedUsed_ < unmappedTarget ) {
        BinaryBlock * block = master_->extract( &bucketBias );
        if( !block ) {
            // There were none to recycle
            break;
        }
        do {
            TOOLS_ASSERT( unmappedUsed_ < unmappedMax_ );
            unmapped_[ unmappedUsed_++ ] = block;
            if( unmappedUsed_ == unmappedMax_ ) {
                if( !!block->next_ ) {
                    // Return our leftovers to the bucket.  This only happens if we hit a
                    // hard lmit.
                    TOOLS_ASSERT( bucketBias ==
                        BinaryPoolMaster::pointerBucket( block->next_->base_ ));
                    master_->insertUnique( bucketBias, block->next_ );
                }
                break;
            }
        } while( !!( block = block->next_ ));
    }
    TOOLS_ASSERT( unmappedUsed_ <= unmappedMax_ );
    if( unmappedUsed_ != 0 ) {
        // Get one of the entries we just extracted
        return unmapped_[ --unmappedUsed_ ];
    }
    // We're completely out of entries.  Allocate a new one from the inner.
    return BinaryPoolMaster::innerMap( &mapOf_, inner_, parentSize_ );
}
#ifdef WINDOWS_PLATFORM
#  pragma warning( default : 4706 )
#endif // WINDOWS_PLATFORM

void
BinaryPoolThreadBuffer::unmap(
    void * site )
{
    TOOLS_ASSERT( ( reinterpret_cast< size_t >( site ) % parentSize_ ) == 0 );
    BinaryBlock * block = static_cast< BinaryBlock * >( site );
    BinaryBlock * base = reinterpret_cast< BinaryBlock * >( tools::roundDownPow2( reinterpret_cast< uintptr_t >( block ), parentSize_ * 2 ));
    if( base == mapOf_ ) {
        // Immediate round trip
        mapOf_ = nullptr;
        inner_->unmap( base );
        return;
    }
    TOOLS_ASSERT( unmappedUsed_ <= unmappedMax_ );
    for( unsigned i=0; i!=unmappedUsed_; ++i ) {
        if( unmapped_[ i ]->base_ == base ) {
            // This is a match
            unmapped_[ i ] = unmapped_[ --unmappedUsed_ ];
            inner_->unmap( base );
            return;
        }
    }
    TOOLS_ASSERT( unmappedUsed_ <= unmappedMax_ );
    if( unmappedUsed_ != unmappedMax_ ) {
        // Buffer this for later remate
        block->next_ = nullptr;
        block->base_ = base;
        unmapped_[ unmappedUsed_++ ] = block;
        return;
    }
    // TODO: Make this loop less horrible by allowing it to batch returns.
    TOOLS_ASSERT( unmappedUsed_ <= unmappedMax_ );
    while( ( unmappedUsed_ * 4 ) > unmappedMax_ ) {
        TOOLS_ASSERT( unmappedUsed_ <= unmappedMax_ );
        master_->unmap( unmapped_[ --unmappedUsed_ ]);
    }
    master_->unmap( site );
}

////////////////
// BufferingPool
////////////////

BufferingPool::BufferingPool(
    Pool & inner,
    impl::ResourceSample const & sample,
    size_t max )
    : inner_( &inner )
    , desc_( inner.describe() )
    , mapsLock_( monitorStaticNew( StringIdNull(), Monitor::PolicyAllowPriorityInversion ))
    , mapsUsed_( 0 )
    , mapsMax_( static_cast< unsigned >( max ))
{
    TOOLS_ASSERT( mapsMax_ > 0 );  // For 0, use BuffereingPoolNil instead.
    desc_.trace_ = impl::resourceTraceBuild( sample, desc_.trace_ );
}

Pool::Desc const &
BufferingPool::describe( void )
{
    return desc_;
}

void *
BufferingPool::map( void )
{
    // If one is already available, use that.
    {
        AutoDispose<> l_( mapsLock_->enter() );
        if( mapsUsed_ != 0 ) {
            return maps_[ --mapsUsed_ ];
        }
    }
    // Try to add a single entry to the available pool.
    void * ret = inner_->map();
    TOOLS_ASSERT( ( reinterpret_cast< size_t >( ret ) & desc_.align_ ) == 0 );
    {
        AutoDispose<> l_( mapsLock_->enter() );
        if( mapsUsed_ == mapsMax_ ) {
            // No room left to add this, so consume it now. This means many unmaps must have slipped in
            // while we were not holding the lock.  This should be very rare.
            return ret;
        }
        maps_[ mapsUsed_++ ] = ret;
    }
    // A single entry was added to the pool. Don't use it now (better performance later). Grab another page
    // from inner_.
    ret = inner_->map();
    TOOLS_ASSERT( ( reinterpret_cast< size_t >( ret ) & desc_.align_ ) == 0 );
    return ret;
}

void
BufferingPool::unmap(
    void * site )
{
    TOOLS_ASSERT( ( reinterpret_cast< size_t >( site ) & desc_.align_ ) == 0 );
    void * site2;
    {
        AutoDispose<> l_( mapsLock_->enter() );
        if( mapsUsed_ == mapsMax_ ) {
            // No room to store site in the pool. Free it, as well as one additoinal entry in order to
            // make some space in the pool.
            site2 = maps_[ --mapsUsed_ ];
            TOOLS_ASSERT( ( reinterpret_cast< size_t >( site ) & desc_.align_ ) == 0 );
        } else {
            maps_[ mapsUsed_++ ] = site;
            return;
        }
    }
    // If we get here, there was overflow
    inner_->unmap( site );
    inner_->unmap( site2 );
}

///////////////////
// BufferingPoolNil
///////////////////

BufferingPoolNil::BufferingPoolNil(
    Pool & inner,
    impl::ResourceSample const & sample )
    : inner_( &inner )
    , desc_( inner.describe() )
{
    desc_.trace_ = impl::resourceTraceBuild( sample, desc_.trace_ );
}

Pool::Desc const &
BufferingPoolNil::describe( void )
{
    return desc_;
}

void *
BufferingPoolNil::map( void )
{
    void * ret = inner_->map();
    TOOLS_ASSERT( ( reinterpret_cast< uintptr_t >( ret ) % desc_.align_ ) == 0 );
    return ret;
}

void
BufferingPoolNil::unmap(
    void * site )
{
    TOOLS_ASSERT( ( reinterpret_cast< size_t >( site ) % desc_.align_ ) == 0 );
    inner_->unmap( site );
}

///////////
// VmemPool
///////////

//VmemPool::VmemPool(
//    impl::ResourceSample const & sample,
//    size_t size,
//    size_t phase,
//    unsigned )
//    : lock_( impl::monitorPlatformNew() )
//    , regionBegin_( nullptr )
//    , regionEnd_( nullptr )
//    , regionNext_( nullptr )
//    , reserveNext_( nullptr )
//{
//    // We're setup to cycle this region
//    alignSpecOf( &spec_, size, phase, false );
//    desc_.size_ = size;
//    desc_.phase_ = phase;
//    desc_.align_ = spec_.alignBytes_;
//    impl::ResourceSample samp = sample;
//    samp.size_ = sizeVmemPage;
//    desc_.trace_ = impl::resourceTraceBuild( samp, nullptr );
//}
//
//VmemPool::~VmemPool( void )
//{
//    unsigned refsRemaining = static_cast< unsigned >( ( regionEnd_ - regionNext_ ) /
//        sizeVmemPage );
//    if( refsRemaining != 0 ) {
//        TOOLS_ASSERT( regionBegin_->refs_ == refsRemaining );
//        impl::vmemPoolRelease( regionBegin_ );
//    }
//}
//
//Pool::Desc const &
//VmemPool::describe( void )
//{
//    return desc_;
//}
//
//void *
//VmemPool::map( void )
//{
//    uint8 * pageHead;
//    {
//        AutoDispose<> l_( lock_->enter() );
//        if( regionNext_ == regionEnd_ ) {
//            // Format a new region
//            do {
//                regionBegin_ = static_cast< RegionHead * >( impl::vmemPoolMap( reserveNext_ ));
//                reserveNext_ += sizeVmemRegion;
//            } while( !regionBegin_ );
//            regionBegin_->refs_ = ( sizeVmemRegion / sizeVmemPage ) - 1U;
//            regionNext_ = reinterpret_cast< uint8 * >( regionBegin_ ) + sizeVmemPage;
//            regionEnd_ = reinterpret_cast< uint8 * >( regionBegin_ ) + sizeVmemRegion;
//            if( regionEnd_ > reserveNext_ ) {
//                // The address was interpreted as a hint, but we got repositioned.  Accept
//                // the repositioning and move forwards.
//                reserveNext_ = regionEnd_;
//            }
//        }
//        pageHead = regionNext_;
//        *reinterpret_cast< RegionHead ** >( regionNext_ ) = regionBegin_;
//        regionNext_ += sizeVmemPage;
//    }
//    // Align the final value. Because it's less than 4k, this should always work.
//    return alignPlace( spec_, pageHead + sizeof( RegionHead * ), pageHead + sizeVmemPage );
//}
//
//void
//VmemPool::unmap(
//    void * site )
//{
//    // We unmap only via the internal references, no locks
//    unmapAlloc( site );
//}
//
//void *
//VmemPool::pageOf(
//    void * site )
//{
//    return reinterpret_cast< void * >( reinterpret_cast< size_t >( site ) &
//        ~static_cast< size_t >( sizeVmemPage - 1U ));
//}
//
//VmemPool::RegionHead *
//VmemPool::regionOf(
//    void * site )
//{
//    return *reinterpret_cast< RegionHead ** >( reinterpret_cast< size_t >( site ) &
//        ~static_cast< size_t >( sizeVmemPage - 1U ));
//}
//
//void
//VmemPool::unmapAlloc(
//    void * site )
//{
//    RegionHead * region = regionOf( site );
//    // TODO: verify overflow
//    unsigned prevRefs;
//    bool decommitted = false;
//    do {
//        prevRefs = region->refs_;
//        if( prevRefs > 1U ) {
//            if( !decommitted ) {
//                impl::vmemPoolDecommit( pageOf( site ));
//                decommitted = true;
//            }
//        }
//    } while( atomicCas( &region->refs_, prevRefs, prevRefs - 1U ) != prevRefs );
//    if( prevRefs == 1U ) {
//        // Release the entire region
//        impl::vmemPoolRelease( region );
//    }
//}

///////////////
// NodePoolBase
///////////////

NodePoolBase::NodePoolBase(
    Pool & super,
    impl::ResourceSample const & sample,
    size_t size,
    size_t phase,
    size_t align )
    : super_( &super )
    , newBlock_( nullptr )
    , newMapBegin_( nullptr )
    , newMapEnd_( nullptr )
{
    Pool::Desc superDesc = super.describe();
    TOOLS_ASSERT( isPow2( superDesc.size_ ));
    TOOLS_ASSERT( superDesc.align_ == superDesc.size_ );
    TOOLS_ASSERT( superDesc.phase_ == 0 );
    superSize_ = superDesc.align_;
    TOOLS_ASSERT( superSize_ %  4096 == 0 );  // failing this we could not use leak-protect
    alignSpecOf( &spec_, size, phase, false );
    TOOLS_ASSERT( ( align == 0U ) || ( ( align >= spec_.alignBytes_ ) && ( ( align % spec_.alignBytes_ ) == 0U ) && ( align <= size )));
    if( align != 0U ) {
        spec_.alignBytes_ = align;
    }
    // Pool desc
    desc_.size_ = size;
    desc_.phase_ = phase;
    desc_.align_ = spec_.alignBytes_;
    impl::ResourceSample samp = sample;
    samp.size_ = spec_.alignAlloc_;
    desc_.trace_ = impl::resourceTraceBuild( samp, superDesc.trace_ );
}

NodePoolBase::~NodePoolBase( void )
{
    TOOLS_ASSERT( impl::isAbnormalShutdown() || freeBlocks_.empty() ||
        (!!newBlock_ && ( &freeBlocks_.back() == newBlock_ )));
    if( impl::leakProtect() ) {
        // Other classes that are built on top of this one may cause this to have entries in freeBlocks_,
        // if they are leak protected. Force clear in order to avoid assertion in the destructor.
        freeBlocks_.clear();
    }
    if( !newBlock_ ) {
        return;
    }
    // Deref the new slab's phantom reference
    TOOLS_ASSERT( impl::isAbnormalShutdown() || newBlock_->refs_ == 1U );
    if( newBlock_->is_linked() ) {
        newBlock_->unlink();
    }
    super_->unmap( newBlock_ );
}

Pool::Desc const &
NodePoolBase::describe( void )
{
    return desc_;
}

SuperBlock *
NodePoolBase::superBlockOf(
    void * site )
{
    return reinterpret_cast< SuperBlock * >( tools::roundDownPow2( reinterpret_cast< uintptr_t >( site ), superSize_ ));
}

SuperBlock *
NodePoolBase::firstFree( void )
{
    if( freeBlocks_.empty() ) {
        return nullptr;
    }
    return &freeBlocks_.back();
}

// We always have the notion of a superblock that we haven't yet filled. This function sets it.
//
// Perhaps counter-intuitively, we don't immediately make newBlock_->freeMap_ a full linked list of
// every item within the superblock, nor is newBlock_ added to freeBlocks_. That happens naturally
// over time, as frees happen. Arguably it's more efficient, though involves greater code complexity.
//
// Although freeMap_ starts off empty, we give the new superblock a reference count of 1 so that it
// does not get disposed before we've put anything in it. A similar trick is used in temporal.
void
NodePoolBase::acceptSuperBlock(
    void * block )
{
    TOOLS_ASSERT(!newBlock_);
    TOOLS_ASSERT(!newMapBegin_ && !newMapEnd_);
    newBlock_ = ::new( block ) SuperBlock;
    newBlock_->freeMap_ = nullptr;
    newBlock_->refs_ = 1U;
    // 64 bytes are reserved for the superblock header.
    static_assert(sizeof(SuperBlock) <= 64, "SuperBlock header is too large");
    // Start directly after the header.  This shouldn't be too polluting.
    newMapBegin_ = reinterpret_cast< uint8 * >( newBlock_ ) + 64U;
    newMapEnd_ = reinterpret_cast< uint8 * >( block ) + superSize_;
}

// Attempt to map out of existing user data.  Locking is left to the derived types.
void *
NodePoolBase::tryMapUser( void )
{
    SuperBlock * freeBlock = firstFree();
    if( !!freeBlock ) {
        void * user = freeBlock->freeMap_;
        TOOLS_ASSERT( superBlockOf( user ) == freeBlock );  // If this trips, the superblock contains a free item not within its own bounds.
        ++freeBlock->refs_;
        void * nextFreeItem = *reinterpret_cast< void ** >( user );
        freeBlock->freeMap_ = nextFreeItem;
        TOOLS_ASSERT( nextFreeItem != user );
        if( !freeBlock->freeMap_ ) {
            // Remove from the free list, superblock has no free items.
            freeBlock->unlink();
        } else {
            TOOLS_ASSERT( superBlockOf( nextFreeItem ) == freeBlock );  // If this trips, the superblock free list got corrupted.
        }
        return user;
    }
    // There are no superblocks with free items.  Allocate from the superblock that hasn't been filled
    // yet. This is moving to the right, similar to a temporal slab. But unlike temporal, we can (and
    // already tried to) allocate on top of an item that has already been freed.
    if( !newBlock_ ) {
        return nullptr;
    }
    // Otherwise, try and allocate from the new map range
    void * newFormat = alignPlace( spec_, newMapBegin_, newMapEnd_ );
    if( !!newFormat ) {
        ++newBlock_->refs_;
        newMapBegin_ = reinterpret_cast< uint8 * >( newFormat ) + spec_.placeBytes_;
        return newFormat;
    }
    // The superblock has filled to full. Further, we have no free items, or else we would have already
    // exited above. newBlock_->refs_ is maximal (roughly (superSize_ - slab header)/desc_.size_), but
    // that isn't assertable due to alignPlace(..) padding. Though we can assert a nullptr free map.
    TOOLS_ASSERT( newBlock_->refs_ > 1U );
    TOOLS_ASSERT( newBlock_->freeMap_ == nullptr );
    // Remove the synthetic ref that we set in acceptSuperBlock(...) to clear our notion of this block
    // being not filled yet. Then fail. The caller should respond by making a new superblock.
    --newBlock_->refs_;
    newBlock_ = nullptr;
    newMapBegin_ = newMapEnd_ = nullptr;
    // Although we weren't on the freeBlocks_ list if we got here, don't worry about this superblock
    // leaking. The next item freed (which there certainly should be given the high reference count)
    // will put it on the freeBlocks_ list.
    return nullptr;
}

// Unmap user data and return a superblock pointer if it was entirely dereferenced. Locking is left to the
// derived class. The code structure allos a derived class to easily wrap this call with a lock without
// having to also hold that lock while freeing a superblock.
void *
NodePoolBase::unmapUser(
    void * site )
{
    SuperBlock * block = superBlockOf( site );
    TOOLS_ASSERT( block->refs_ > 0U );  // If this trips, there is a double-free in a NodePoolBase superblock
    --block->refs_;
    if( block->refs_ == 0U ) {
        // There should have been something previously free; a one block SuperBlock isn't
        // too useful.
        TOOLS_ASSERT( !!block->freeMap_ );
        block->unlink();
        // While we could add 'site' to the list of free items. However, assuming the caller disposes the
        // superblock, its contents no longer matter.
        return block;
    }
    void * prevFree = block->freeMap_;
    TOOLS_ASSERT( !prevFree || ( superBlockOf( prevFree ) == block ));
    TOOLS_ASSERT( site != prevFree );
    *reinterpret_cast< void ** >( site ) = prevFree;
    block->freeMap_ = site;
    if( !prevFree ) {
        // First free in this super block, note it.
        freeBlocks_.push_front( *block );
    }
    return nullptr;
}

///////////
// NodePool
///////////

NodePool::NodePool(
    Pool & super,
    impl::ResourceSample const & sample,
    size_t size,
    size_t phase )
    : NodePoolBase( super, sample, size, phase )
{
    TOOLS_ASSERT( ( superSize_ % 4096 ) == 0 );  // If this trips we cannot use leak-protect.
}

void *
NodePool::map( void )
{
    void * ret = tryMapUser();
    if( !ret ) {
        void * superblock = super_->map();
        if( !superblock ) {
            return nullptr;
        }
        acceptSuperBlock( superblock );
        ret = tryMapUser();
        TOOLS_ASSERT( !!ret );
    }
    return ret;
}

void
NodePool::unmap(
    void * site )
{
    void * super = unmapUser( site );
    if( !!super ) {
        if( impl::leakProtect() ) {
            impl::leakAndProtect( super, superSize_, "NodePool" );
        } else {
            super_->unmap( super );
        }
    }
}

///////////////
// NodePoolSync
///////////////

NodePoolSync::NodePoolSync(
    Pool & super,
    impl::ResourceSample const & sample,
    size_t size,
    size_t phase,
    size_t align )
    : NodePoolBase( super, sample, size, phase, align )
    , lockFree_( monitorStaticNew( StringIdNull(), Monitor::PolicyAllowPriorityInversion ))
{
}

void *
NodePoolSync::map( void )
{
    void * ret;
    void * super = nullptr;
    {
        AutoDispose<> l_( lockFree_->enter() );
        ret = tryMapUser();
    }
    if( !!ret ) {
        return ret;
    }
    // Try to get an available superblock
    super = super_->map();
    if( !super ) {
        return nullptr;
    }
    // Since we dropped the lock for a while, there is a (small) chance that another thread has freed
    // something in that time. It doesn't hurt to be optimistic and retry.
    {
        AutoDispose<> l_( lockFree_->enter() );
        ret = tryMapUser();
        if( !ret ) {
            // Use the superblock that we allocated above
            acceptSuperBlock( super );
            ret = tryMapUser();
            TOOLS_ASSERT( !!ret );
            return ret;
        }
    }
    // Optimism for the win! We got something and don't need the superblock after all.
    TOOLS_ASSERT( !!super );
    super_->unmap( super );
    return ret;
}

void
NodePoolSync::unmap(
    void * site )
{
    void * super;
    {
        AutoDispose<> l_( lockFree_->enter() );
        super = unmapUser( site );
    }
    if( !!super ) {
        if( impl::leakProtect() ) {
            impl::leakAndProtect( super, superSize_, "NodePoolSync" );
        } else {
            super_->unmap( super );
        }
    }
}

////////////////
// SlabHeadSmall
////////////////

void
SlabHeadSmall::calcRefs( unsigned formatRefs )
{
    // If the number of references drops below this, the slab may get returned to the free pool. We'll
    // begin at 5/7, which may seeem high, but latency within the master is expected as well.
    lowFragRefs_ = ( formatRefs * 5U ) / 7U;
    TOOLS_ASSERT( lowFragRefs_ < formatRefs );
    if( ( formatRefs - lowFragRefs_ ) <= 15U ) {
        TOOLS_ASSERT( formatRefs >= 15 );
        lowFragRefs_ = ( formatRefs - 15U );
    }
    // The reuse refs is high because reuse cost is quite a bit cheaper. Though we don't want to oscillate
    // right on the edge.
    reuseRefs_ = ( formatRefs * 17U ) / 19U;
    TOOLS_ASSERT( reuseRefs_ < formatRefs );
    if( ( formatRefs - reuseRefs_ ) <= 3U ) {
        TOOLS_ASSERT( formatRefs >= 3 );
        reuseRefs_ = ( formatRefs - 3U );
    }
    // This must be true for the reuse method to function correctly.
    TOOLS_ASSERT( reuseRefs_ > lowFragRefs_ );
}

void
SlabHeadSmall::attach( unsigned threadRefs, bool isNew )
{
    TOOLS_ASSERT( isNew || ( ( lowFragRefs_ > 0U ) && ( reuseRefs_ > 0U )));
    atomicUpdate( &refs_, [ isNew, threadRefs ]( SlabHeadSmall::RefState prev )->SlabHeadSmall::RefState {
        prev.state_ = SlabHeadSmall::StateAttached;
        if( isNew ) {
            prev.refs_ = threadRefs;
        } else {
            prev.refs_+= threadRefs;
        }
        return prev;
    });
}

void
SlabHeadSmall::detach( NodeSmallPool * parent, unsigned threadRefs )
{
    TOOLS_ASSERT( ( lowFragRefs_ > 0U ) && ( reuseRefs_ > 0U ));
    // Reduce the refs.
    bool reuseSlab;
    atomicUpdate( &refs_, [ =, &reuseSlab ]( RefState prev )->RefState {
        TOOLS_ASSERT( prev.refs_ >= threadRefs );
        TOOLS_ASSERT( prev.state_ == SlabHeadSmall::StateAttached );
        prev.state_ = SlabHeadSmall::StateLowFrag;
        reuseSlab = false;
        prev.refs_ -= threadRefs;
        if( prev.refs_ <= lowFragRefs_ ) {
            reuseSlab = true;
            prev.state_ = StateFree;
        }
        return prev;
    });
    if( reuseSlab ) {
        // Return this to the master list of available slabs. Another thread will then be able to use it.
        unmapSlab( parent );
    }
}

FreeNode *
SlabHeadSmall::reuse( unsigned threadRefs, unsigned reuseThreadRefs )
{
    TOOLS_ASSERT( ( lowFragRefs_ > 0U ) && ( reuseRefs_ > 0U ));
    bool shouldReuse;
    atomicUpdate( &refs_, [ =, &shouldReuse ]( RefState prev )->RefState {
        TOOLS_ASSERT( prev.state_ == SlabHeadSmall::StateAttached );
        TOOLS_ASSERT( prev.refs_ >= threadRefs );
        prev.refs_ -= threadRefs;
        if( prev.refs_ <= reuseRefs_ ) {
            // Refs have been reduced enough to reuse.
            shouldReuse = true;
            prev.refs_ += reuseThreadRefs;
            return prev;
        }
        shouldReuse = false;
        TOOLS_ASSERT( prev.refs_ >= lowFragRefs_ );
        prev.state_ = SlabHeadSmall::StateLowFrag;
        return prev;
    });
    if( !shouldReuse ) {
        return nullptr;
    }
    TOOLS_ASSERT( !!frees_ );
    return tools::atomicExchange< FreeNode *, FreeNode * >( &frees_, nullptr );
}

void
SlabHeadSmall::unmapSlab( NodeSmallPool * parent )
{
    // Insert a free (or mostly free) slab onto the global list.
    parent->pushSlab( this );
}

void
SlabHeadSmall::unmap( NodeSmallPool * parent, void * site )
{
    // No ABA problem with this stack list since we never pop individual items, only the entire list as a whole.
    tools::atomicPush< FreeNode >( &frees_, static_cast< FreeNode * >( site ), &FreeNode::next_ );
    // Reduce the refs
    bool reuseSlab;
    atomicUpdate( &refs_, [ =, &reuseSlab ]( RefState prev )->RefState {
        TOOLS_ASSERT( prev.refs_ > 0U );
        reuseSlab = false;
        --prev.refs_;
        if( ( prev.state_ == SlabHeadSmall::StateLowFrag ) && ( prev.refs_ <= lowFragRefs_ )) {
            TOOLS_ASSERT( ( lowFragRefs_ > 0U ) && ( reuseRefs_ > 0U ));
            reuseSlab = true;
            prev.state_ = SlabHeadSmall::StateFree;
        }
        return prev;
    });
    if( reuseSlab ) {
        // Return slab to the master free list, so that it can be used in another thread.
        unmapSlab( parent );
    }
}

/////////////////
// NodeSmallLocal
/////////////////

NodeSmallLocal::NodeSmallLocal(
    NodeSmallPool * parent )
    : parent_( parent )
    , spec_( parent->spec_ )
    , slabSize_( parent->slabSize_ )
    , currentSlab_( nullptr )
    , currentFrees_( nullptr )
    , currentRefs_( 0U )
    , newMapBegin_( nullptr )
    , newMapEnd_( nullptr )
    , newMapNodes_( 0U )
{
    // The only reason this would get created is because we're going to map, so just do some allocation.
    allocSlab();
}

NodeSmallLocal::~NodeSmallLocal( void )
{
    // This only happens as threads shut down. As such it does not need to be all that optimized.
    while( void * i = tryMapCurrent() ) {
        currentSlab_->unmap( parent_, i );
    }
    currentSlab_->detach( parent_, currentRefs_ );
}

unsigned
NodeSmallLocal::maxRefs( void )
{
    return static_cast< unsigned >( slabSize_ / 8U );  // a guess is as good as anything
}

void
NodeSmallLocal::allocSlab( void )
{
    // Get a slab from the parent. It may or may not be a completely empty slab (isNew == true); slabs
    // that are mostly (but not completely) free move around alot in the small node allocator.
    bool isNew;
    currentSlab_ = allocSlabParent( &isNew );
    TOOLS_ASSERT( ( reinterpret_cast< uintptr_t >( currentSlab_ ) % slabSize_ ) == 0U );
    currentRefs_ = maxRefs();
    if( isNew ) {
        // Just got a completely empty slab. As with NodeSmallPool, we don't bother to initialize a
        // fully populated free list covering the entire slab; the free list gets filled over time, on
        // an explicit free. When the free list is empty, we allocate from the range [newMapBegin_,
        // newMapEnd_), which represents the never allocated range in this slab.
        currentSlab_->frees_ = nullptr;
        currentFrees_ = nullptr;
        newMapBegin_ = currentSlab_ + 1U;
        newMapEnd_ = reinterpret_cast< uint8 * >( currentSlab_ ) + slabSize_;
        newMapNodes_ = 0U;
    } else {
        // This slab is not completely free. It has already been filled once, so in this iteration we
        // are not going to make use of newMapBegin_/newMapEnd_; everything we can use is on the free list.
        newMapBegin_ = newMapEnd_ = nullptr;
        // Take the entire free list from the slab and store it local to this type. That way we claim the
        // entire contents of the list for this thread.
        currentFrees_ = atomicExchange< FreeNode *, FreeNode * >( &currentSlab_->frees_, nullptr );
    }
    currentSlab_->attach( currentRefs_, isNew );
}

void *
NodeSmallLocal::tryMapCurrent( void )
{
    // First, try to allocate from our thread local list. Though the slab may other free items due to
    // frees from other threads; currentFrees_ is the subset that are exclusive to us, and that we can
    // allocate from with no concurrency concerns.
    if( FreeNode * alloc = currentFrees_ ) {
        currentFrees_ = alloc->next_;
        --currentRefs_;
        return alloc;
    }
    // Second, if this slab hasn't yet filled up, use that space. This is similar to filling a temporal
    // slab, just moving from low to high addresses.
    if( newMapBegin_ == newMapEnd_ ) {
        return nullptr;
    }
    void * result = alignPlace( spec_, newMapBegin_, newMapEnd_ );
    if( !!result ) {
        --currentRefs_;
        ++newMapNodes_;
        // This should never actually overrun.
        TOOLS_ASSERT( newMapNodes_ < maxRefs() );
        newMapBegin_ = reinterpret_cast< uint8 * >( result ) + spec_.placeBytes_;
        TOOLS_ASSERT( newMapBegin_ <= newMapEnd_ );
    } else {
        // Allocation failed; throw out any remaining stuff at the end of the slab as they are not enough
        // to allocate an item.
        newMapBegin_ = newMapEnd_;
    }
    if( newMapBegin_ == newMapEnd_ ) {
        // Slab is now full.  Apply formatting metrics.
        currentSlab_->calcRefs( newMapNodes_ );
    }
    return result;
}

void *
NodeSmallLocal::map( void )
{
    while( true ) {
        if( void * currentNode = tryMapCurrent() ) {
            return currentNode;
        }
        unsigned targetRefs = maxRefs();
        // See if there are free nodes to recycle, or if the block is to be released back to the master.
        // If newFreeNodes is nullptr, the current was marked for release.
        if( FreeNode * newFreeNodes = currentSlab_->reuse( currentRefs_, targetRefs )) {
            currentFrees_ = newFreeNodes;
            currentRefs_ = targetRefs;
            continue;
        }
        // Get a new slab to draw from
        allocSlab();
    }
}

SlabHeadSmall *
NodeSmallLocal::allocSlabParent(
    bool * isNew )
{
    // Prefer to reuse an old slab.
    if( SlabHeadSmall * head = parent_->popSlab() ) {
        *isNew = false;
        return head;
    }
    // Get a new slab, at the expense of doing an actual allocation.
    *isNew = true;
    return reinterpret_cast< SlabHeadSmall * >( parent_->slabPool_->map() );
}

/////////////////////////////////////
// NodeSmallPoolBase::SuperUnmapQueue
/////////////////////////////////////

NodeSmallPoolBase::SuperUnmapQueue::SuperUnmapQueue(
    Unmapper & pool,
    unsigned size,
    StringId const & note )
    : slabPool_( &pool )
    , poolSize_( size )
    , leakProtectNote_( note )
    , superUnmaps_( nullptr )
    , passSuperUnmap_( false )
{
}

NodeSmallPoolBase::SuperUnmapQueue::~SuperUnmapQueue( void )
{
    while( SlabHeadSmall * i = superUnmaps_ ) {
        superUnmaps_ = i->nextSlab_;
        if( impl::leakProtect() ) {
            impl::leakAndProtect( i, poolSize_, leakProtectNote_ );
        } else {
            slabPool_->unmap( i );
        }
    }
}

uint32 const *
NodeSmallPoolBase::SuperUnmapQueue::operator()(
    SlabHeadSmall * slab )
{
    refs_ = atomicRead( &slab->refs_ ).refs_;
    if( refs_ == 0U ) {
        if( passSuperUnmap_ ) {
            slab->nextSlab_ = superUnmaps_;
            superUnmaps_ = slab;
            return nullptr;
        } else {
            passSuperUnmap_ = true;
        }
    }
    return &refs_;
}

////////////////////
// NodeSmallPoolBase
////////////////////

NodeSmallPoolBase::NodeSmallPoolBase(
    Pool & super )
    : slabPool_( &super )
    , freeQueueSlabs_( nullptr )
    , freeQueueLength_( 0U )
    , queueRefillLength_( 3U )
    , freeRefillLength_( 0U )
    , freeSlabsLock_( monitorStaticNew( StringIdNull(), Monitor::PolicyAllowPriorityInversion ))
    , refillSlabsLock_( monitorStaticNew( StringIdNull(), Monitor::PolicyAllowPriorityInversion ))
{
    // Ensure that we don't hit the platform node allocator (2MB).
    freeSlabs_.reserve( 128U );
    refillSlabs_.reserve( 128U );
}

NodeSmallPoolBase::~NodeSmallPoolBase( void )
{
    // Called after the thread local buffers are torn down, so everything in the lists should be fully
    // dereferenced.
    for( auto && slab : freeSlabs_ ) {
        // if slab.slab_->refs_->refs_ > 0, log a memory leak: Leak in a NodeSmallPoolBase freeSlabs_ slab
        // has reference count of (slab.slab_->refs_->refs_).  (A detailed leak message should follow.)
        if( atomicRead( &slab.slab_->refs_ ).refs_ == 0 ) {
            slabPool_->unmap( slab.slab_ );
        }
    }
    TOOLS_ASSERT( refillSlabs_.empty() );
    while( SlabHeadSmall * i = freeQueueSlabs_ ) {
        freeQueueSlabs_ = freeQueueSlabs_->nextSlab_;
        // if i->refs_->refs_ > 0, log a memory leak: Leak in NodeSmallPoolBase freeQueueSlabs_ slab
        // has a reference count of (i->refs_->refs_).  (A detailed leak message should follow.)
        if( atomicRead( &i->refs_ ).refs_ == 0 ) {
            slabPool_->unmap( i );
        }
    }
}

bool
NodeSmallPoolBase::tryRefill( void )
{
    // Accumulate slabs without any references here, but allow one through.
    SuperUnmapQueue superUnmaps( *slabPool_, static_cast< unsigned >( slabPool_->describe().size_ ), "NodeSmallPoolBase" );
    AutoDispose<> l_( refillSlabsLock_->enter( true ));
    if( !l_ ) {
        return false;
    }
    // Duplicated logic is to allow earliest possible exit
    if( freeQueueLength_ < queueRefillLength_ ) {
        // Not qualified to refill based on the queue length
        {
            AutoDispose<> l__( freeSlabsLock_->enter() );
            if( freeSlabs_.size() >= freeRefillLength_ ) {
                // Apparently also not qualified to refill based on free slabs length. Likely we are on
                // the losing side of a race for refill.
                return true;
            }
        }
        // This should always start empty because the refill lock cleaned up.
        TOOLS_ASSERT( refillSlabs_.empty() );
    } else {
        // This should always start empty because the refill lock cleaned up.
        TOOLS_ASSERT( refillSlabs_.empty() );
        AutoDispose<> l__( freeSlabsLock_->enter() );
        // Drain the best items into the refill set. Don't both derefing as we want this interval to be short.
        size_t freeRefillTransfer = freeRefillLength_;
        if( freeRefillTransfer < 3U ) {
            // Always leave three behind
            freeRefillTransfer = 3U;
        }
        if( freeSlabs_.size() > freeRefillTransfer ) {
            refillSlabs_.assign( freeSlabs_.begin() + static_cast< ptrdiff_t >( freeRefillTransfer ), freeSlabs_.end() );
            freeSlabs_.erase( freeSlabs_.begin() + static_cast< ptrdiff_t >( freeRefillTransfer ), freeSlabs_.end() );
        }
    }
    // Preprocess the entries in the vector, update their reference counts and remove excess emptry entries.
    auto slabOut = refillSlabs_.begin();
    for( auto && item : refillSlabs_ ) {
        if( uint32 const * refs = superUnmaps( item.slab_ )) {
            // update the sampled reference count
            slabOut->slabRefs_ = *refs;
            slabOut->slab_ = item.slab_;
            ++slabOut;
        }
        // otherwise we don't keep it
    }
    // Cut out anything we've already returned to the source because it was fully unmapped.
    refillSlabs_.erase( slabOut, refillSlabs_.end() );
    // Count everything we just removed
    size_t slabsPopped = 0U;
    SlabHeadSmall * freeSlabsPushed = atomicExchange< SlabHeadSmall *, SlabHeadSmall * >( &freeQueueSlabs_, nullptr );
    while( SlabHeadSmall * i = freeSlabsPushed ) {
        freeSlabsPushed = i->nextSlab_;
        ++slabsPopped;
        if( uint32 const * refs = superUnmaps( i )) {
            FreeSlab f;
            f.slab_ = i;
            f.slabRefs_ = *refs;
            refillSlabs_.push_back( f );
        }
    }
    // Count down references for what we just drew out of the list. This should reduce competition for
    // refills.
    atomicUpdate( &freeQueueLength_, [ = ]( unsigned prev )->unsigned {
        TOOLS_ASSERT( prev >= slabsPopped );
        return static_cast< unsigned >( prev - slabsPopped );
    });
    // Put them in order for drawing down
    std::sort( refillSlabs_.begin(), refillSlabs_.end() );
    {
        AutoDispose<> l__( freeSlabsLock_->enter() );
        // When the vector has been reduced to 1/7th of its original size, trigger a refill.
        freeRefillLength_ = static_cast< unsigned >( refillSlabs_.size() * 1U / 7U );
        // when the pending queue is 6/7s of the combined size, then refill from there (because  we're
        // probably looking at a deallocation pattern).
        size_t queueRefillNew = ( refillSlabs_.size() + freeSlabs_.size() ) * 6U / 7U;
        if( queueRefillNew < 3U ) {
            queueRefillNew = 3U;
        }
        queueRefillLength_ = static_cast< unsigned >( queueRefillNew );
        refillSlabs_.swap( freeSlabs_ );
    }
    // Take the former contents of the free slabs and put them back on the pending list, but don't check
    // for refill metrics while we're working on it. We want this thread to return.
    for( auto && item : refillSlabs_ ) {
        if( !!superUnmaps( item.slab_ )) {
            pushSlab( item.slab_, false );
        }
    }
    refillSlabs_.clear();
    return true;
}

void
NodeSmallPoolBase::pushSlab(
    SlabHeadSmall * slab,
    bool checkRefill )
{
    // Insert a (possibly only mostly) free slab onto the queue so that another thread can use it.
    atomicIncrement( &freeQueueLength_ );
    // For now we just push onto the list, in most cases we won't closely examine the relative lengths.
    atomicPush( &freeQueueSlabs_, slab, &SlabHeadSmall::nextSlab_ );
    if( !checkRefill || ( freeQueueLength_ < queueRefillLength_ )) {
        return;
    }
    // Check if we should do a refill based on the current unmaps.
    tryRefill();
}

SlabHeadSmall *
NodeSmallPoolBase::popSlab( void )
{
    // Try to return an old slab that may be only partially freed and put it onto the free slabs list. The
    // slab's free list will tell you what can be allocated. Worth repeating: unlike all of our other
    // allocators, this one can return a slab that is partially used and still may have some allocated
    // items on it. If we return nullptr the caller will need to allocate a new slab.
    
    // Only willing to return superUnmapsMax empty slabs during allocation.
    SlabHeadSmall * superUnmaps[ superUnmapsMax ];
    size_t superUnmapsUsed = 0U;
    SlabHeadSmall * ret = nullptr;
    bool shouldRefill = false;
    {
        AutoDispose<> l_( freeSlabsLock_->enter() );
        while( superUnmapsUsed != superUnmapsMax ) {
            if( freeSlabs_.empty() ) {
                break;
            }
            if( !!ret ) {
                superUnmaps[ superUnmapsUsed++ ] = ret;
            }
            ret = freeSlabs_.back().slab_;
            freeSlabs_.pop_back();
            if( atomicRead( &ret->refs_ ).refs_ > 0U ) {
                // not completely free
                break;
            }
        }
        shouldRefill = ( freeSlabs_.size() <= freeRefillLength_ );
    }
    // Try refill will check the parameters
    if( shouldRefill ) {
        if( tryRefill() && !ret ) {
            // We did something during refill, check if we should try for an existing entry
            AutoDispose<> l_( freeSlabsLock_->enter() );
            if( !freeSlabs_.empty() ) {
                ret = freeSlabs_.back().slab_;
                freeSlabs_.pop_back();
            }
        }
    }
    // If there were any pending unmaps do those now
    while( superUnmapsUsed > 0U ) {
        slabPool_->unmap( superUnmaps[ --superUnmapsUsed ]);
    }
    return ret;
}

////////////////
// NodeSmallPool
////////////////

NodeSmallPool::NodeSmallPool(
    Pool & super,
    impl::ResourceSample const & sample,
    size_t size,
    size_t phase )
    : NodeSmallPoolBase( super )
    , localPool_( [this]( NodeSmallLocal ** ref ) { return anyDisposableAllocNew< AllocStatic< Platform >>( ref, this ); })
{
    Pool::Desc superDesc = super.describe();
    TOOLS_ASSERT( isPow2( superDesc.size_ ));
    TOOLS_ASSERT( superDesc.align_ == superDesc.size_ );
    TOOLS_ASSERT( superDesc.phase_ == 0 );
    slabSize_ = superDesc.align_;
    TOOLS_ASSERT( ( slabSize_ % 4096 ) == 0 ); // Otherwise we cannot use leak protect
    alignSpecOf( &spec_, size, phase, false );
    desc_.size_ = size;
    desc_.phase_ = phase;
    desc_.align_ = spec_.alignBytes_;
    impl::ResourceSample localSample = sample;
    localSample.size_ = spec_.alignAlloc_;
    desc_.trace_ = impl::resourceTraceBuild( localSample, superDesc.trace_ );
}

Pool::Desc const &
NodeSmallPool::describe( void )
{
    return desc_;
}

void *
NodeSmallPool::map( void )
{
    return localPool_->map();
}

void
NodeSmallPool::unmap(
    void * site )
{
    return slabHeadOf( site )->unmap( this, site );
}

SlabHeadSmall *
NodeSmallPool::slabHeadOf(
    void * site )
{
    return reinterpret_cast< SlabHeadSmall * >( roundDownPow2( reinterpret_cast< uintptr_t >( site ), slabSize_ ));
}

/////////////////
// MemoryWrapPool
/////////////////

MemoryWrapPool::MemoryWrapPool(
    void * base,
    size_t length,
    size_t size )
    : base_( base )
    , length_( length )
    , allocSize_( size )
    , freeLock_( monitorNew() )
    , freeList_( nullptr )
{
    TOOLS_ASSERT( length_ > 0 );
    alignSpecOf( &spec_, allocSize_, 0, false );
    TOOLS_ASSERT( allocSize_ == spec_.alignAlloc_ );
    desc_.size_ = spec_.alignAlloc_;
    desc_.align_ = spec_.alignAlloc_;
    desc_.phase_ = 0U;
    desc_.trace_ = nullptr;
    nextChunk_ = static_cast< uint8 * >( base_ );
}

Pool::Desc const &
MemoryWrapPool::describe( void )
{
    return desc_;
}

void *
MemoryWrapPool::map( void )
{
    AutoDispose<> l_( freeLock_->enter() );
    if( !!freeList_ ) {
        void ** next = reinterpret_cast< void ** >( freeList_ );
        freeList_ = *next;
        return reinterpret_cast< void * >( next );
    }
    if( ( nextChunk_ + spec_.alignAlloc_ ) <= ( static_cast< uint8 * >( base_ ) + length_ )) {
        uint8 * alloc = nextChunk_;
        nextChunk_ = alloc + spec_.alignAlloc_;
        return static_cast< void * >( alloc );
    }
    // out of space
    return nullptr;
}

void
MemoryWrapPool::unmap(
    void * site )
{
    AutoDispose<> l_( freeLock_->enter() );
    if( ( static_cast< uint8 * >( site ) + spec_.alignAlloc_ ) == nextChunk_ ) {
        nextChunk_ = static_cast< uint8 * >( site );
        return;
    }
    // Add to the free list
    void ** next = reinterpret_cast< void ** >( site );
    *next = freeList_;
    freeList_ = site;
}

/////////////////
// CyclicPoolDesc
/////////////////

void
tools::detail::CyclicPoolDesc::format(
    size_t elementBytes )
{
    TOOLS_ASSERT( !IsNullOrEmptyStringId( name_ ));
    elementBytes_ = static_cast< uint32 >( elementBytes );
    // Calculate the lowest pool we'll be using
    poolMin_ = static_cast< uint8 >( poolFirstOfElement( elementBytes ));
}

/////////////
// BinaryPool
/////////////

BinaryPool::BinaryPool(
    Pool & inner,
    impl::ResourceSample const & sample )
    : master_( inner, sample )
{
}

Pool::Desc const &
BinaryPool::describe( void )
{
    return master_.desc_;
}

void *
BinaryPool::map( void )
{
    size_t bucketBias = 3;
    BinaryBlock * block = master_.extract( &bucketBias );
    if( !!block ) {
        BinaryBlock * next = block->next_;
        if( !!next ) {
            TOOLS_ASSERT( bucketBias == BinaryPoolMaster::pointerBucket( next->base_ ));
            master_.insertUnique( bucketBias, next );
        }
        return block;
    }
    // If we get here, there were no cached moieties.  Make a new one from the parent
    // collection.
    BinaryBlock * bottom;
    void * top = BinaryPoolMaster::innerMap( &bottom, master_.inner_,
        master_.desc_.size_ );
    // Leave the bottom half in the free set
    master_.insertUnique( BinaryPoolMaster::pointerBucket( top ), bottom );
    return top;
}

void
BinaryPool::unmap(
    void * site )
{
    master_.unmap( site );
}

/////////////
// MallocPool
/////////////

MallocPool::MallocPool(
    impl::ResourceSample const & sample,
    size_t size,
    size_t phase )
    : sample_( sample )
    , platform_( &impl::affinityInstance< Platform >() )
{
    alignSpecOf( &spec_, size, phase );
    // Alignment
    desc_.align_ = spec_.alignBytes_;
    desc_.size_ = size;
    desc_.phase_ = phase;
    impl::ResourceSample samp = sample;
    samp.size_ = desc_.size_;
    desc_.trace_ = impl::resourceTraceBuild( sample, nullptr );
}

Pool::Desc const &
MallocPool::describe( void )
{
    return desc_;
}

void *
MallocPool::map( void )
{
    return alignAlloc( spec_, platform_->map( spec_.alignAlloc_, sample_ ));
}

void
MallocPool::unmap(
    void * site )
{
    platform_->unmap( unalignAlloc( site ));
}

///////////
// SlabHead
///////////

SlabHead::SlabHead(
    unsigned inner,
    Pool * source,
    bool leakProtect,
    bool checkLifetime )
    : refs_( inner )
    , leakProtect_( leakProtect )
    , checkLifetime_( checkLifetime )
    , source_( source )
    , genesis_( 0 )
    , allocs_( nullptr )
{
    if( checkLifetime_ ) {
        genesis_ = impl::getHighResTime();
    }
}

size_t
SlabHead::getSlabSize( void )
{
    return source_->describe().size_;
}

void
SlabHead::setAllocsIfNull( AllocHeadCheckLifetime * head )
{
    TOOLS_ASSERT( checkLifetime_ );
    if( allocs_ == nullptr ) {
        allocs_ = head;
    }
}

void
SlabHead::unmapSlab( void )
{
    if( TOOLS_UNLIKELY( leakProtect_ )) {
        impl::leakAndProtect( this, getSlabSize(), leakProtectNoteSlab() );
    } else {
        source_->unmap( this );
    }
}

void
SlabHead::preUnmapCheck( void )
{
    TOOLS_ASSERT( refs_ == 0 );
#ifndef TOOLS_RELEASE
#  ifndef TOOLS_QA
    const uint64 now = impl::getHighResTime();
    const uint32 duration = static_cast< uint32 >( ( now - genesis_ ) / TOOLS_NANOSECONDS_PER_MILLISECOND );
#  endif // !TOOLS_QA
#endif // !TOOLS_RELEASE
    AllocHeadCheckLifetime * head = allocs_;
    for( ; !!head; head = head->getNext() ) {
        TOOLS_ASSERT( reinterpret_cast< uint8 * >( head ) > reinterpret_cast< uint8 * >( this ));
        TOOLS_ASSERT( reinterpret_cast< uint8 * >( head ) <= reinterpret_cast< uint8 * >( this ) + temporalSlabSizeLarge );
        TOOLS_ASSERT( head->parent_ == this );
        TOOLS_ASSERT( !head->getNext() || ( head->getNext() > head + 1 ));
        // signed, because delta-t might in certain rare occations be negative (VMs and the like).
        TOOLS_ASSERT( (sint32)head->timeMs_ <= (sint32)duration );
    }
}

void
SlabHead::lifetimeSkewCheck( AllocHeadCheckLifetime * node )
{
    // If the nodes allocation time is much higher than the average lifetimes of it's neighbors, then the
    // node has delayed frees for all other objects in the slab.  Since the temporal allocator does not
    // free anything in a slab until it frees everything, print a notice for possible investigation.
    uint64 othersTotalLifetime = 0;
    unsigned numOthers = 0;
    for( AllocHeadCheckLifetime * cur = allocs_; !!cur; cur = cur->getNext() ) {
        if( cur != node ) {
            // include neighbors but exclude ourselves in the calculation.
            othersTotalLifetime += cur->timeMs_;  // add delta-t
            ++numOthers;
        }
    }
    //impl::ResourceTrace * trace = node->trace_;
    uint32 ourLifetime = node->timeMs_;
    if( ( numOthers > 0 ) && ( ourLifetime > TOOLS_MILLISECONDS_PER_SECOND ) &&
        ( ourLifetime > ( 8 * ( othersTotalLifetime / numOthers )))) {
        // TODO: log a lifetime skew of ourLifetime ms > (othersTotalLifetime/numOthers) ms.
        // Name is trace->getName(), addr is trace->getSymbol(), trace->getSize() bytes.
        // For good measure we can log that the interval is impl::memoryTrackingIntervalLifetime().
    }
}

void
SlabHead::unmap( AllocHeadPlain * head )
{
    if( checkLifetime_ ) {
        static_cast< AllocHeadCheckLifetime * >( head )->onUnmap();
    }
    // As noted elsewhere, refs_ is artificially inflated by a large value until the slab overflows.  Until
    // then, we'll always early exit.  A nice trick to ensure we never free a slab without making full use
    // of it.  However, it does mean that if we free all blocks in the slab before it fills up, we miss
    // the chance to do the temporal check.
    const unsigned old = atomicAdd( &refs_, (unsigned)-1 );
    TOOLS_ASSERT( old != 0 );
    if( old > 1 ) {
        return;
    }
    if( checkLifetime_ ) {
        lifetimeSkewCheck( static_cast< AllocHeadCheckLifetime * >( head ));
        preUnmapCheck();
    }
    unmapSlab();
}

/////////////////////////
// AllocHeadCheckLifetime
/////////////////////////

AllocHeadCheckLifetime::AllocHeadCheckLifetime(
    SlabHead * parent,
    impl::ResourceSample const & sample,
    impl::ResourceTrace * trace )
    : CheckLifetimeData( impl::resourceTraceBuild( sample, trace ))
    , AllocHeadPlain( parent, sample, trace )
{
    uint64 now = impl::getHighResTime();
    TOOLS_ASSERT( now >= parent_->genesis_ );
    timeMs_ = static_cast< uint32 >( ( now - parent_->genesis_ ) / TOOLS_NANOSECONDS_PER_MILLISECOND );
}

void
AllocHeadCheckLifetime::setNext( AllocHeadCheckLifetime * node )
{
    // The alloc list is built one and never changed.
    TOOLS_ASSERT( !!node );
    TOOLS_ASSERT( nextOffset_ == 0 );
    uintptr_t byteOffset = reinterpret_cast< uintptr_t >( node ) - reinterpret_cast< uintptr_t >( parent_ );
    TOOLS_ASSERT( byteOffset < parent_->getSlabSize() );
    nextOffset_ = static_cast< uint32 >( byteOffset );
}

AllocHeadCheckLifetime *
AllocHeadCheckLifetime::getNext( void )
{
    if( nextOffset_ == 0 ) {
        return nullptr;
    }
    TOOLS_ASSERT( nextOffset_ <= parent_->getSlabSize() );
    uintptr_t ret = reinterpret_cast< uintptr_t >( parent_ ) + nextOffset_;
    return reinterpret_cast< AllocHeadCheckLifetime * >( ret );
}

void
AllocHeadCheckLifetime::onUnmap( void )
{
    const uint64 now = impl::getHighResTime();
    TOOLS_ASSERT( now >= parent_->genesis_ );
    // We would like to assert that now >= time, but the assert sometimes hits in VMs where getHighResTime
    // sometimes goes backwards (it shouldn't, but oh well).
    timeMs_ = static_cast< uint32 >( ( now - parent_->genesis_ ) / TOOLS_NANOSECONDS_PER_MILLISECOND ) - timeMs_;
}

///////////////
// TemporalBase
///////////////

TemporalBase::TemporalBase(
    Pool & pool,
    impl::ResourceTrace * trace,
    bool leakProtect )
    : runPool_( &pool )
    , runPoolDesc_( pool.describe() )
    , parentTrace_( trace )
    , slabHead_( nullptr )
    , slabUsed_( nullptr )
    , slabMax_( nullptr )
    , leakProtect_( leakProtect )
    , checkLifetimeSampler_( impl::memoryTrack() ? impl::memoryTrackingIntervalLifetime() : 0 )
    , slabHeadCheckLifetime_( false )
{
    // Check if the input dimensions might work
    TOOLS_ASSERT( runPoolDesc_.align_ == runPoolDesc_.size_ );
    TOOLS_ASSERT( !isInitialized() );
}

TemporalBase::~TemporalBase( void )
{
    if( !isInitialized() ) {
        return;
    } else if( release() ) {
        // The slab is empty, all objects were freed and destructed.
        runPool_->unmap( slabHead_ );
    } else {
        // There are still references to the slab.  Hopefully someone is still tearing things down.
    }
}

void
TemporalBase::attach(
    uint8 * site )
{
    slabHead_ = reinterpret_cast< SlabHead * >( site );
    slabMax_ = site + runPoolDesc_.size_;
    // Leave room for a cache line so the decrementing doesn't disturb any nearby
    // objects.
    static_assert( sizeof( SlabHead ) <= 64U, "SlabHead is too big" );
    slabUsed_ = site + 64U;
    // Starter refs so external users don't run down to zero and free the buffer.
    innerRefs_ = static_cast< unsigned >( runPoolDesc_.size_ );
    // Construction via placement new.
    ( void )::new( static_cast< void * >( slabHead_ )) SlabHead( innerRefs_, runPool_, leakProtect_, checkLifetimeSampler_.sample() );
    slabHeadCheckLifetime_ = slabHead_->checkLifetime_;
    if( slabHeadCheckLifetime_ ) {
        slabLast_ = nullptr;
    }
}

bool
TemporalBase::release( void )
{
    if( !isInitialized() ) {
        return false;
    }
    TOOLS_ASSERT( !!innerRefs_ );
    TOOLS_ASSERT( slabHead_->refs_ >= innerRefs_ );
    TOOLS_ASSERT( slabUsed_ > reinterpret_cast< uint8 * >( slabHead_ ));
    TOOLS_ASSERT( slabMax_ >= slabUsed_ );
    unsigned old = atomicAdd( &slabHead_->refs_, (unsigned)-(int)innerRefs_ );
    TOOLS_ASSERT( old >= innerRefs_ );
    return ( old == innerRefs_ );
}

void
TemporalBase::linkAllocationIn(
    AllocHeadPlain * )
{
}

void
TemporalBase::linkAllocationIn(
    AllocHeadCheckLifetime * node )
{
    if( !!slabLast_ ) {
        TOOLS_ASSERT( node >= ( slabLast_ + 1 ));
        slabLast_->setNext( node );
    }
    slabLast_ = node;
    slabHead_->setAllocsIfNull( node );
}

void *
TemporalBase::mapOnCurrentSlab(
    size_t size,
    impl::ResourceSample const & sample,
    size_t phase,
    size_t align )
{
    // On first execution slabHead_ is nullptr; however it doesn't matter which branch we take.
    if( slabHeadCheckLifetime_ ) {
        return mapOnCurrentSlabWithHead< AllocHeadCheckLifetime >( size, sample, phase, align );
    } else {
        return mapOnCurrentSlabWithHead< AllocHeadPlain >( size, sample, phase, align );
    }
}

void *
TemporalBase::map(
    size_t size,
    impl::ResourceSample const & sample,
    size_t phase,
    size_t align )
{
    void * place;
    while( true ) {
        place = mapOnCurrentSlab( size, sample, phase, align );
        if( TOOLS_LIKELY( !!place )) {
            return place;
        }
        // Slab overflow.  Release the inner ref counts that were held to ensure that the slab filled up
        // before it got freed.
        if( release() ) {
            // Everything in this slab is free, which is awesome because it means we can recycle this
            // slab.  Of note, we're reusing the slab without doing the lifetime skew check.  As such,
            // we don't have a valid AllocHead parameter to supply.  It turns out that is not so bad,
            // this path is only reached when the entire slab is free before overflow.  Given how fast
            // we fill slabs, that means this skew just won't happen very often.
            if( slabHeadCheckLifetime_ ) {
                slabHead_->preUnmapCheck();
            }
            if( impl::memoryPoison() ) {
                const uint8 sentinel = static_cast< uint8 >( '\xC4' );
                std::fill( reinterpret_cast< uint8 * >( slabHead_ ), slabMax_, sentinel );
            }
            attach( reinterpret_cast< uint8 * >( slabHead_ ));
        } else if ( !isInitialized() ) {
            // First allocation, we need a new slab.
            attach( static_cast< uint8 * >( runPool_->map() ));
        } else {
            // Slab still has >= 1 live items in it.  Temporal cannot reuse anything freed in the slab
            // until the entire slab is free.  So we have to allocate a new slab.
            attach( static_cast< uint8 * >( runPool_->map() ));
        }
    }
}

bool
TemporalBase::isInitialized( void ) const
{
    return ( slabHead_ != nullptr );
}

void
TemporalBase::unmapAny(
    void * site )
{
    // Threading note: TemporalBase is thread local.  Since frees can occur on any thread, this function
    // must stay away from TemporalBase, though it can use SlabHead.  Every block is preceeded by a
    // structure that includes a pointer to the slab head.  This is necessary because end users do not
    // know, and cannot bass in, the slab head.  We don't yet know whether there is only an AllocHeadPlain
    // or a full AllocHeadCheckLifetime, so we allow AllocHead to figure it out.
    AllocHeadPlain * head = reinterpret_cast< AllocHeadPlain * >( site ) - 1;
    head->unmap();
}

////////////////////
// TemporalPoolProxy
////////////////////

TemporalPoolProxy::TemporalPoolProxy(
    TemporalBase & base,
    Pool::Desc const & desc,
    impl::ResourceSample const & sample )
    : base_( &base )
    , desc_( desc )
    , sample_( sample )
{
}

Pool::Desc const &
TemporalPoolProxy::describe( void )
{
    return desc_;
}

void *
TemporalPoolProxy::map( void )
{
    return base_->map( desc_.size_, sample_, desc_.phase_, desc_.align_ );
}

void
TemporalPoolProxy::unmap(
    void * site )
{
    TemporalBase::unmapAny( site );
}

//////////////////////////////
// ThreadLocalTemporalAffinity
//////////////////////////////

ThreadLocalTemporalAffinity::ThreadLocalTemporalAffinity(
    Affinity & affinity,
    impl::ResourceTrace * trace,
    bool leakProtect )
    : parent_( &affinity )
    , parentPoolSample_( TOOLS_RESOURCE_SAMPLE_NAMED_T( sizeRun, "backing slab", trace ))
    , parentPool_( &parent_->pool( sizeRun, parentPoolSample_ ))
    , parentPoolDesc_( parentPool_->describe() )
    , parentPoolSmall_( &parent_->pool( sizeRunSmall, TOOLS_RESOURCE_SAMPLE_NAMED_T( sizeRunSmall, "backing slab", trace )))
    , parentPoolMedium_( &parent_->pool( sizeRunMedium, TOOLS_RESOURCE_SAMPLE_NAMED_T( sizeRunMedium, "backing slab", trace )))
    , parentTrace_( trace )
    , leakProtect_( leakProtect )
    , temporalHeapSmall_( *parentPoolSmall_, trace, leakProtect )
    , temporalHeapMedium_( *parentPoolMedium_, trace, leakProtect )
    , temporalHeapLarge_( *parentPool_, trace, leakProtect )
{
}

Affinity &
ThreadLocalTemporalAffinity::bind( void )
{
    // Already done
    return *this;
}

AutoDispose<>
ThreadLocalTemporalAffinity::fork(
    Affinity ** refferenceAffinity,
    impl::ResourceSample const & sample )
{
    // Convert the parent to a bound affinity
    ThreadLocalTemporalAffinityFork * aff = new ThreadLocalTemporalAffinityFork( *parent_, impl::resourceTraceBuild( sample ), leakProtect_ );
    *refferenceAffinity = aff;
    return std::move(aff);
}

Pool &
ThreadLocalTemporalAffinity::pool(
    size_t size,
    impl::ResourceSample const &,
    size_t phase )
{
    if( size >= parentPoolCutoff ) {
        // The parent pool should be sufficiently aligned.  So we don't have to worry
        // about fitting two things in the pool uneccessarily.
        return *parentPool_;
    }
    // TODO: Normalize the size
    PoolSpec spec( size, phase );
    auto i = pools_.find( spec );
    if( i != pools_.end() ) {
        return i->second;
    }
    // Get the alignment model
    AlignSpec align;
    alignSpecOf( &align, size, phase, false );
    Pool::Desc desc;
    desc.align_ = align.alignBytes_;
    desc.size_ = size;
    desc.phase_ = phase;
    desc.trace_ = parentPoolDesc_.trace_;
    auto & temporalHeap = ( size < 256 ) ? temporalHeapSmall_ :
        ( size < 16384 ) ? temporalHeapMedium_ : temporalHeapLarge_;
    i = pools_.insert( PoolMap::value_type( spec, TemporalPoolProxy( temporalHeap, desc, parentPoolSample_ ))).first;
    return i->second;
}

void *
ThreadLocalTemporalAffinity::map(
    size_t size,
    impl::ResourceSample const & sample,
    size_t phase )
{
    if( size < 256 ) {
        return temporalHeapSmall_.map( size, sample, phase, 64U );
    } else if( size < 16384 ) {
        void * ret = temporalHeapMedium_.map( size, sample, phase, ( ( size - phase ) & ( 4096U - 1U )) ? 64U : 4096U );
        // Don't mistake for an alignment that came from the parent pool.  If this trips, the check in unmap() is inherently buggy.
        TOOLS_ASSERT( ( reinterpret_cast< uintptr_t >( ret ) & ( sizeRun - 1U )) != 0 );
        return ret;
    } else if( size < parentPoolCutoff ) {
        void * ret = temporalHeapLarge_.map( size, sample, phase, 4096U );
        TOOLS_ASSERT( ( reinterpret_cast< uintptr_t >( ret ) & ( sizeRun - 1U )) != 0 );
        return ret;
    }
    // Fall through to the internal mapping.
    TOOLS_ASSERT( size <= sizeRun );
    // Paranoia checking.  If asserts are disabled, temporal cannot allocate anything larger than sizeRun.
    if( size > sizeRun ) {
        // TODO: make this integrate with logging
        printf( "Temporal allocation too large (%d > %d)\n", size, sizeRun );
        abort();
    }
    void * ret = parentPool_->map();
    // return something aligned to sizeRun, otherwise unmap() will do something wrong.
    TOOLS_ASSERT( !( reinterpret_cast< size_t >( ret ) & ( sizeRun - 1U )));
    return ret;
}

void
ThreadLocalTemporalAffinity::unmap(
    void * site )
{
    // This check may be fragile, a tiny allocation may be coincidentally aligned.  That's why map() has asserts.
    if( ( reinterpret_cast< uintptr_t >( site ) & ( sizeRun - 1U )) == 0 ) {
        // This was originally a 2MB allocation, give it back to the parent
        parentPool_->unmap( site );
    } else {
        TemporalBase::unmapAny( site );
    }
}

//////////////////////////////////
// TemporalAffinity::PoolProxyBase
//////////////////////////////////

TemporalAffinity::PoolProxyBase::PoolProxyBase(
    impl::ResourceSample const & sample )
    : sample_( sample )
{
}

Pool::Desc const &
TemporalAffinity::PoolProxyBase::describe( void )
{
    return desc_;
}

void *
TemporalAffinity::PoolProxyBase::map( void )
{
    ThreadLocalTemporalAffinity & l = **bound_;
    if( desc_.size_ < 256U ) {
        return l.temporalHeapSmall_.map( desc_.size_, sample_, desc_.phase_, desc_.align_ );
    } else if( desc_.size_ < 16384U ) {
        return l.temporalHeapMedium_.map( desc_.size_, sample_, desc_.phase_, desc_.align_ );
    }
    return l.temporalHeapLarge_.map( desc_.size_, sample_, desc_.phase_, 4096U );
}

void
TemporalAffinity::PoolProxyBase::unmap(
    void * site )
{
    TemporalBase::unmapAny( site );
}

///////////////////
// TemporalAffinity
///////////////////

TemporalAffinity::TemporalAffinity(
    bool forkParent,
    Affinity & parent,
    impl::ResourceTrace * trace,
    bool leakProtect )
    : trace_( trace )
    , bound_( ThreadLocalTemporalFactoryBase( *this ))
    , poolsLock_( monitorNew( 2U, Monitor::PolicyAllowPriorityInversion ))
    , leakProtect_( leakProtect )
{
    if( forkParent ) {
        parentDisp_ = parent.fork( &parent_ );
    } else {
        parent_ = &parent;
    }
}

TemporalAffinity::~TemporalAffinity( void )
{
}

Affinity &
TemporalAffinity::bind( void )
{
    return *bound_;
}

AutoDispose<>
TemporalAffinity::fork(
    Affinity ** referenceAffinity,
    impl::ResourceSample const & sample )
{
    TemporalAffinity * aff = new TemporalAffinity( false, *parent_,
        impl::resourceTraceBuild( sample, trace_ ), leakProtect_ );
    *referenceAffinity = aff;
    return std::move(aff);
}

Pool &
TemporalAffinity::pool(
    size_t size,
    impl::ResourceSample const & sample,
    size_t phase )
{
    if( size >= ThreadLocalTemporalAffinity::parentPoolCutoff ) {
        // The parent pool should be sufficiently aligned, so we don't have to worry
        // fitting two things in the pool unneccessarily.
        TOOLS_ASSERT( size <= ThreadLocalTemporalAffinity::sizeRun );
        return parent_->pool( size );
    }
    // TODO: normalize the size
    PoolSpec spec( size, phase );
    AutoDispose<> l_( poolsLock_->enter() );
    auto i = pools_.find( spec );
    if( i != pools_.end() ) {
        return i->second;
    }
    // Get the alignment model
    AlignSpec align;
    alignSpecOf( &align, size, phase, false );
    PoolProxyBase pool( sample );
    pool.bound_ = &bound_;
    pool.desc_.align_ = align.alignBytes_;
    pool.desc_.phase_ = phase;
    pool.desc_.trace_ = impl::resourceTraceBuild( TOOLS_RESOURCE_SAMPLE_CALLER( size ), trace_ );
    pool.desc_.size_ = size;
    return pools_.insert( PoolMap::value_type( spec, pool )).first->second;
}

void *
TemporalAffinity::map(
    size_t size,
    impl::ResourceSample const & sample,
    size_t phase )
{
    // Revector to the thread-local Temporal affinity.
    return bound_->map( size, sample, phase );
}

void
TemporalAffinity::unmap(
    void * site )
{
    if( ( reinterpret_cast< uintptr_t >( site ) &
        ( ThreadLocalTemporalAffinity::sizeRun - 1U )) == 0 ) {
        // This was originally a 2MB allocation, pass it back to the parent pool.
        bound_->unmap( site );
    } else {
        TemporalBase::unmapAny( site );
    }
}

/////////////////////////////////
// ThreadLocalTemporalFactoryBase
/////////////////////////////////

ThreadLocalTemporalFactoryBase::ThreadLocalTemporalFactoryBase(
    TemporalAffinity & outer )
    : outer_( &outer )
{
}

AutoDispose<>
ThreadLocalTemporalFactoryBase::operator()(
    ThreadLocalTemporalAffinity ** reference ) const
{
    return anyDisposableAllocNew< AllocStatic< Platform >>( reference, *outer_->parent_, outer_->trace_, outer_->leakProtect_ );
}

//////////////////////////////////
// ThreadLocalTemporalAffinityFork
//////////////////////////////////

ThreadLocalTemporalAffinityFork::ThreadLocalTemporalAffinityFork(
    Affinity & affinity,
    impl::ResourceTrace * trace,
    bool leakProtect )
    : ThreadLocalTemporalAffinity( affinity, trace, leakProtect )
{
}

///////////////
// ShutdownDump
///////////////

ShutdownDump::~ShutdownDump( void )
{
    impl::resourceTraceDump( impl::resourceTraceDumpPhaseAll, true, nullptr );
}

/////////////////////
// CyclicPoolDescImpl
/////////////////////

CyclicPoolDescImpl::CyclicPoolDescImpl(
    StringId const & name )
{
    name_ = name;
    elementBytes_ = 0U;
    poolMin_ = tools::detail::CyclicPoolDesc::slabPoolsMax;
    std::fill( slabPools_, slabPools_ + slabPoolsMax, static_cast< Pool * >( nullptr ));
}

//////////
// Statics
//////////

uint64 MemoryDumpTask::lastDumpNs_ = 0ULL;
uint64 MemoryDumpTask::lastDumpBytes_ = 0ULL;
static tools::RegisterFactoryRegistryFunctor< tools::detail::CyclicPoolDesc > registerCyclicPoolDesc(
    []( tools::detail::CyclicPoolDesc ** ref, tools::StringId const & name )->AutoDispose<> {
        CyclicPoolDescImpl * newItem = new CyclicPoolDescImpl( name );
        *ref = newItem;
        return std::move(newItem);
    });