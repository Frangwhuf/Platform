#include "../toolsprecompiled.h"

#include <tools/Concurrency.h>
#include <tools/InterfaceTools.h>
#include <tools/Memory.h>
#include <tools/Tools.h>

#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <algorithm>
#include <condition_variable>
#include <map>
#include <numeric>
#include <unordered_map>
#include <vector>

#include <boost/cast.hpp>

using boost::numeric_cast;

using namespace tools;

///////////////
// Evil Globals
///////////////

// Total bytes allocated by the 'tracked' functions.
size_t volatile trackedUntracked = 0;
size_t trackedPad[ 7 ];
// If true, do stack trace based tracking of 'untracked' memory. Very expensive in memory and CPU.
// This should be made into something controlled by Configuration. While this is NULL, this tracking
// is disabled.
bool const * trackComplete = NULL;
// This should be made into something controlled by Configuration. May need to change TrackedUntrackedStackTrace.
unsigned const stackLevels = 40;
__thread unsigned insideTrackedMalloc;

////////
// Types
////////

namespace {
    struct VmemPool
        : Pool
    {
        VmemPool( void );
        ~VmemPool( void );
        VmemPool( VmemPool const & ) = delete;
        VmemPool & operator=( VmemPool const & ) = delete;

        // Pool
        Pool::Desc const & describe( void );
        void * map( void );
        void unmap( void * );

        // local methods
        size_t getSysAllocatedSize( void ) const;
        size_t getSysAllocatedSizeLargePages( void ) const;
        void releaseReserved( void );
        size_t getReservedSize( void ) const;
        void initDesc( void );
        void * popReserved( void );
        void pushReserved( void * );

        size_t largePageSize_;
        Pool::Desc desc_;
        // Reserve 32 units at a time
        static const size_t reservationUnits = 32;
        // Complete set of reservations we'll free at shutdown
        std::vector< void * > addrs_;
        unsigned volatile largePagesAllocated_;
        // Reserved addresses (used mainly for small pages that will successfully decommit)
        std::vector< void * > reserved_;
        bool reservedUnmapped_;
        // Can't use tools::ConditionVariable or tools::Monitor as they need to allocate, which may
        // be re-enterent.  But now C++ has equivalents.
        std::condition_variable availablePageCond_;
        std::mutex availablePagelock_;
        // If this is true, don't free any of the 2 MB pages, only mprotect. This is a surprisingly
        // effective method of use-after-free/write-after-free detection. Of course, this will also
        // rapidly lead to OOM. This is controlled by the 'leak-protect' (boolean) configuration.
        bool leakProtect_;
        bool forceSmallPages_;

        unsigned volatile numMmappers_;
    };

    struct AffinityMalloc;

    struct AffinityMallocPool
        : Pool
    {
        AffinityMallocPool( AffinityMalloc *, Pool::Desc & );

        // Pool
        Pool::Desc const & describe( void );
        void * map( void );
        void unmap( void * );

        AffinityMalloc * parent_;
        Pool::Desc desc_;
    };

    struct AffinityMalloc
        : Affinity
        , Disposable
    {
        typedef std::map< size_t, AffinityMallocPool > PoolMap;
        AffinityMalloc( bool );
        AffinityMalloc( AffinityMalloc const & ) = delete;
        AffinityMalloc & operator=( AffinityMalloc const & ) = delete;

        // Disposable
        void dispose( void );

        // Affinity
        Affinity & bind( void );
        AutoDispose<> fork( Affinity **, impl::ResourceSample const & );
        Pool & pool( size_t, impl::ResourceSample const &, size_t );
        void * map( size_t, impl::ResourceSample const &, size_t );
        void unmap( void * );

        bool untracked_;
        VmemPool * vmem_;
        PoolMap pools_;
    };

    struct TrackedUntrackedStackTrace
    {
        TOOLS_FORCE_INLINE bool operator==( TrackedUntrackedStackTrace const & right ) const {
            return ( bufferItems_ == right.bufferItems_ ) && std::equal( buffer_, buffer_ + bufferItems_, right.buffer_ );
        }

        void * buffer_[ stackLevels ];
        unsigned bufferItems_;
    };

    struct DeepTrackedUntrackedEntity
    {
        TrackedUntrackedStackTrace stackTrace_;
        size_t size_;
    };

    // AllocatorAffinity<...> with PlatformUntracked is too heavyweigh for use here. Allocate() on it can
    // end up doing a tracked untracked allocation:
    //   allocate -> NameOf<...> -> ... -> platformDemangleTypeInfo -> new StringId
    // This may cause deadlock because code below needs to assume that nothing it does will lead to
    // additional tracked untracked allocations. To that end we make our own allocator that is more
    // specialized than AllocatorAffinity< T, PlatformUntracked >. _Really_ do not use this anywhere else.
    // It is horrible. Nothing allocated by this allocator is tracked at all. At all.
    template< typename TypeT >
    struct BlindAllocator
        : private std::allocator< TypeT >  // just so we can get some typedefs
    {
        typedef typename std::allocator< TypeT >::value_type value_type;
        typedef typename std::allocator< TypeT >::size_type size_type;
        typedef typename std::allocator< TypeT >::difference_type difference_type;
        typedef typename std::allocator< TypeT >::pointer pointer;
        typedef typename std::allocator< TypeT >::const_pointer const_pointer;
        typedef typename std::allocator< TypeT >::reference reference;
        typedef typename std::allocator< TypeT >::const_reference const_reference;

        BlindAllocator( void ) {}
        template< typename OtherT > BlindAllocator( BlindAllocator< OtherT > const & ) {}
        BlindAllocator( BlindAllocator const & ) {}

        typename std::allocator< TypeT >::pointer
        allocate(
            typename std::allocator< TypeT >::size_type count,
            void const * hint = NULL )
        {
            void * ret = untrackedMalloc( count * sizeof( TypeT ));
            return static_cast< pointer >( ret );
        }

        void
        deallocate(
            typename std::allocator< TypeT >::pointer site,
            typename std::allocator< TypeT >::size_type count )
        {
            TOOLS_ASSERT( count != 0 );
            untrackedFree( site );
        }

        template< typename OtherT >
        struct rebind
        {
            typedef BlindAllocator< OtherT > other;
        };

        void
        construct(
            pointer site,
            const_reference val )
        {
            (void)::new( site ) TypeT( val );
        }

        void
        destroy(
            pointer site )
        {
            site->~value_type();
        }

        size_type
        max_size( void ) const
        {
            return std::allocator< TypeT >::max_size();
        }
    };

    uint32 defineHashAny( TrackedUntrackedStackTrace const & trace, uint32 initial )
    {
        return std::accumulate(trace.buffer_, trace.buffer_ + trace.bufferItems_, impl::hashMix( trace.bufferItems_, initial ), []( uint32 left, void * right )->uint32 {
            return impl::hashMix( right, left );
        });
    }

    struct AllocCounts
    {
        TOOLS_FORCE_INLINE AllocCounts( uint64 na, uint64 nb ) : numAllocations_( na ), numBytes_( nb ) {}

        uint64 numAllocations_;
        uint64 numBytes_;
    };

    struct TrackedItems
    {
        TrackedItems( void );
        ~TrackedItems( void );
        TrackedItems( TrackedItems const & ) = delete;
        TrackedItems & operator=( TrackedItems const & ) = delete;

        void insert( uintptr_t, DeepTrackedUntrackedEntity const & );
        void erase( uintptr_t );
        void collect( std::vector< std::pair< TrackedUntrackedStackTrace, AllocCounts >, BlindAllocator< std::pair< TrackedUntrackedStackTrace, AllocCounts >>> & );

        std::mutex trackedItemsLock_;
        // Using BlindAllocator<> guarantees we go directly to untrackedMalloc, not trackedMalloc
        std::unordered_map< uintptr_t, DeepTrackedUntrackedEntity, HashAnyOf< uintptr_t >, std::equal_to< uintptr_t >, BlindAllocator< std::pair< uintptr_t const, DeepTrackedUntrackedEntity >>> trackedItems_;
        std::unordered_map< TrackedUntrackedStackTrace, AllocCounts, HashAnyOf< TrackedUntrackedStackTrace >, std::equal_to< TrackedUntrackedStackTrace >, BlindAllocator< std::pair< TrackedUntrackedStackTrace const, uint64 >>> trackedStackTraces_;
        bool destructed_; // Hack allert! This is used to stop insert/erase during shutdown.
    };

    static TrackedItems trackedItems;
};  // anonymous namespace

///////////////////////
// Non-member functions
///////////////////////

uint64
tools::detail::physicalMemory( void )
{
    static bool init = false;
    static uint64 ret = 0;
    if( !init ) {
        uint64_t localRet = sysconf( _SC_PHYS_PAGES );
        localRet *= sysconf( _SC_PAGESIZE );
        ret = localRet;
        init = true;
    }
    TOOLS_ASSERT( ret != 0 );
    return ret;
}

extern "C" void * __libc_calloc( size_t, size_t );

namespace tools {
    namespace impl {
        void *
        vmemPoolMap(
            void * site )
        {
            // TODO: add ComplainTimer< 100 > here
            return mmap( site, 65536U, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
        }

        void
        vmemPoolDecommit(
            void * page )
        {
            // TODO: add ComplainTimer< 100 > here
            // TODO: early release this
            mprotect( page, 4096U, PROT_NONE );
        }

        void
        vmemPoolRelease(
            void * site )
        {
            // TODO: add ComplainTimer< 100 > here
            munmap( site, 65536U );
        }

        void
        platformCapVsize(
            uint64 bytes )
        {
            uint64 current = platformVsizeCap();
            if( current < bytes ) {
                // The cap is already set lower.
                // TODO: log this
                return;
            }
            if( bytes > current) {
                // TODO: log that we may be setting (bytes) above the max (current)
            }
            rlimit lim;
            lim.rlim_cur = bytes;
            if( 0 != setrlimit( RLIMIT_AS, &lim )) {
                fprintf( stderr, "Could not cap vsize at %llu (err %d)\n", bytes, errno );
                // TODO: log failed setrlimit
            }
        }

        void
        platformReleaseMemory( void )
        {
            // TODO: consider logging this
            fprintf( stderr, "Dropping memory from VmemPool" );
            VmemPool & pool = globalVmemPool();
            pool.releaseReserved();
            fprintf( stderr, "\n" );
        }

        void
        platformUncapVsize( void )
        {
            // Attempt to uncap virtual memory usage. This gets called when we are dying because of OOM,
            // as a mechanism to try to get though the OOM handling code. Should this fail, don't log
            // anything (which may allocate).
            rlimit lim;
            if( 0 != getrlimit( RLIMIT_AS, &lim )) {
                return;
            }
            lim.rlim_cur = lim.rlim_max;  // no point in trying to set past the maximum
            (void)setrlimit( RLIMIT_AS, &lim );
        }

        uint64
        platformVsizeCap( void )
        {
            rlimit lim;
            if( 0 != getrlimit( RLIMIT_AS, &lim )) {
                fprintf( stderr, "Could not get vsize cap (err %d)\n", errno );
                // TODO: log failed getrlimit
            }
            return lim.rlim_cur;
        }

        void *
        platformHugeAlloc(
            size_t size )
        {
            // size is assumed to be rounded up to page size, but not 'huge page size'. Thus we don't
            // have MAP_HUGETLB.
            void * ret = mmap( NULL, size, PROT_READ | PROT_WRITE, MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0 );
            if( ret == MAP_FAILED ) {
                outOfMemoryDie();
            }
            return ret;
        }

        void
        platformHugeFree(
            void * site,
            size_t size )
        {
            int ret = munmap( site, size );
            TOOLS_ASSERT( !ret );
        }
    };  // impl namespace

    Affinity *
    staticServiceCacheInit( Affinity ***, Platform *** )
    {
        static AffinityMalloc affinity( false );
        return &affinity;
    }

    Affinity *
    staticServiceCacheInit( Affinity ***, PlatformUntracked *** )
    {
        static AffinityMalloc affinity( true );
        return &affinity;
    }

    void *
    untrackedMalloc(
        size_t size )
    {
        typedef void * ( *MallocT )( size_t );
        static MallocT const localMalloc = reinterpret_cast< MallocT >( dlsym( RTLD_NEXT, "malloc" ));
        return localMalloc( size );
    }

    void
    untrackedFree(
        void * site )
    {
        typedef void ( *FreeT )( void * );
        static FreeT const localFree = reinterpret_cast< FreeT >( dlsym( RTLD_NEXT, "free" ));
        localFree( site );
    }

    void *
    untrackedMmap(
        void * site,
        size_t size,
        int prot,
        int flags,
        int fd,
        off_t offset )
    {
        typedef void * ( *MmapT )( void *, size_t, int, int, int, off_t );
        static MmapT const localMmap = reinterpret_cast< MmapT >( dlsym( RTLD_NEXT, "mmap" ));
        return localMmap( site, size, prot, flags, fd, offset );
    }

    int
    untrackedMunmap(
        void * site,
        size_t size )
    {
        typedef int ( *MunmapT )( void *, size_t );
        static MunmapT const localMunmap = reinterpret_cast< MunmapT >( dlsym( RTLD_NEXT, "munmap" ));
        return localMunmap( site, size );
    }

    int
    untrackedMadvise(
        void * site,
        size_t size,
        int advice )
    {
        typedef int ( *MadviseT )( void *, size_t, int );
        static MadviseT const localMadvise = reinterpret_cast< MadviseT >( dlsym( RTLD_NEXT, "madvise" ));
        return localMadvise( site, size, advice );
    }

    int
    untrackedMprotect(
        void const * site,
        size_t size,
        int prot )
    {
        typedef int ( *MprotectT )( void const *, size_t, int );
        static MprotectT const localMprotect = reinterpret_cast< MprotectT >( dlsym( RTLD_NEXT, "mprotect" ));
        return localMprotect( site, size, prot );
    }

    void *
    untrackedRealloc(
        void * site,
        size_t size )
    {
        typedef void * ( *ReallocT )( void *, size_t );
        static ReallocT const localRealloc = reinterpret_cast< ReallocT >( dlsym( RTLD_NEXT, "realloc" ));
        return localRealloc( site, size );
    }

    void *
    untrackedCalloc(
        size_t count,
        size_t size )
    {
        typedef void * ( *CallocT )( size_t, size_t );
        // We need to cheat here, because dlsym needs calloc
        static CallocT const localCalloc = __libc_calloc;
        return localCalloc( count, size );
    }

    void
    untrackedCfree(
        void * site )
    {
        typedef void ( *CfreeT )( void * );
        static CfreeT const localCfree = reinterpret_cast< CfreeT >( dlsym( RTLD_NEXT, "cfree" ));
        localCfree( site );
    }

    void *
    untrackedMemalign(
        size_t align,
        size_t size )
    {
        typedef void * ( *MemalignT )( size_t, size_t );
        static MemalignT const localMemalign = reinterpret_cast< MemalignT >( dlsym( RTLD_NEXT, "memalign" ));
        return localMemalign( align, size );
    }

    void *
    untrackedValloc(
        size_t size )
    {
        typedef void * ( *VallocT )( size_t );
        static VallocT const localValloc = reinterpret_cast< VallocT >( dlsym( RTLD_NEXT, "valloc" ));
        return localValloc( size );
    }

    void *
    untrackedPvalloc(
        size size )
    {
        typedef void * ( *PvallocT )( size_t );
        static PvallocT const localPvalloc = reinterpret_cast< PvallocT >( dlsym( RTLD_NEXT, "pvalloc" ));
        return localPvalloc( size );
    }

    int
    untrackedPosixMemalign(
        void ** ref,
        size_t align,
        size_t size )
    {
        typedef int ( *PosixMemalignT )( void **, size_t, size_t );
        static PosixMemalignT const localPosixMemalign = reinterpret_cast< PosixMemalignT >( dlsym( RTLD_NEXT, "posix_memalign" ));
        return localPosixMemalign( ref, align, size );
    }

    void
    leakAndProtect(
        void * site,
        size_t size,
        StringId const & note )
    {
        TOOLS_ASSERT( reinterpret_cast< uintptr_t >( site ) % 4096 == 0 );
        TOOLS_ASSERT( size % 4096 == 0 );
        // TODO: log protecting range site to (site + size - 1), size bytes, note
        if( mprotect( site, size, PROT_NONE ) != 0 ) {
            int err = errno;
            // TODO: log failed to protect range site to (site + size - 1), err
            return;
        }
        if( madvise( site, size, MADV_DONTNEED ) != 0 ) {
            int err = errno;
            // TODO: log failed protect advise site to (site + size - 1), err
            return;
        }
    }

    static void *
    bigAllocation(
        size_t size,
        bool tryLarge,
        bool & gotLarge )
    {
        void * ret = MAP_FAILED;
        gotLarge = false;
        if( tryLarge ) {
            {
                // TODO: reinstate this
                // ComplainTimer< 100 > t( logSink_, "VmemPool::mmap (large pages)");
                ret = untrackedMmap( NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE | MAP_PRIVATE, -1, 0 );
            }
            if( p == MAP_FAILED ) {
                // This is actually expected (thus not an error) if all of the large pages are in use.
                int e = errno;
                // TODO: log cannot mmap (size / 1MB) MB using large pages, err. Retrying with small pages.
            } else {
                gotLarge = true;
                // TODO: log successfully mmapped (size / 1MB) MB using large pages.
                if( madvise( ret, size, MADV_DONTFORK ) < 0 ) {
                    // TODO: log couldn't set DONTFORK on large page
                    ;
                }
            }
        }
        if( ret == MAP_FAILED ) {
            // TODO: reinstate this
            // ComplainTimer< 100 > t( logSink_, "VmemPool::mmap (small pages)" );
            ret = untrackedMmap( NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_POPULATE | MAP_PRIVATE, -1, 0 );
        }
        if( ret == MAP_FAILED ) {
            impl::outOfMemoryDie();
        } else if( madvise( ret, size, MADV_DONTFORK ) < 0 ) {
            // TODO: could couldn't set DONTFORK on small page
            ;
        }
        return ret;
    }
};  // tools namespace

static VmemPool &
globalVmemPool( void )
{
    static VmemPool pool_;
    return pool_;
}

static Affinity &
affinityMallocGlobalBound( void )
{
    static AffinityMallocGlobal affinity_( true );
    return affinity_;
}

static size_t
getHugePageSize( void )
{
    int fd;
    int len;
    char buf[ 4096 ];
    char * p;
    fd = open( "/proc/meminfo", O_RDONLY | O_CLOEXEC );
    TOOLS_ASSERT( fd >= 0 );
    len = read( fd, buf, sizeof buf );
    TOOLS_ASSERT( len >= 0 );
    TOOLS_ASSERT( len < static_cast< int >( sizeof buf ));
    buf[ len ] = '\0';
    close( fd );
    p = strstr( buf, "Hugepagesize:" );
    TOOLS_ASSERT( !!p );
    p += strlen( "Hugepagesize:" );
    size_t ret = strtol( p, NULL, 10 ) * 1024;
    // x86 normal page size is 4 KB, large page size is 2 MB. Sandybridge introduced 1 GB massive
    // pages. However we don't want to use those for slabs as that will have absurd amounts of
    // fragmentation. So we aim for something between 4 KB and 2 MB.
    TOOLS_ASSERT( ( ret >= 4096 ) && ( ret <= ( 2 * 1024 * 1024 )));
    return ret;
}

// The GCC attribute alias(fn) tells GCC to emit an extra symbol that points at fn. Which is to say, for
// a declaration of the form:
//   void foo() alias(bar)
// there will be two sybols in the symbol table; 'foo' and 'bar' that both point to the same address
// (specifically the address of bar).

#define ALIAS( fn ) __attribute__ (( alias ( #fn )))

// Here we use ALIAS to take over malloc, free, realloc, etc. These functions are just wrappers over the
// libc versions in order to do memory tracking. Pointers to the libc versions are obtained using
// dlsym( RTLD_NEXT, ... ).
extern "C" {
    int trackedMadvise( void *, size_t, int ) __THROW;
    int madvise( void *, size_t, int ) __THROW ALIAS( tracked_madvise );
    int trackedMprotect( void const *, size_t, int ) __THROW;
    int mprotect( void const *, size_t, int ) __THROW ALIAS( tracked_mprotect );
    void * trackedMmap( void *, size_t, int, int, int, off_t ) __THROW;
    void * mmap( void *, size_t, int, int, int, off_t ) __THROW ALIAS( tracked_mmap );
    int trackedMunmap( void *, size_t ) __THROW;
    int munmap( void *, size_t ) __THROW ALIAS( tracked_munmap );
    void * trackedMalloc( size_t ) __THROW;
    void * malloc( size_t ) __THROW ALIAS( tracked_malloc );
    void trackedFree( void * ) __THROW;
    void free( void * ) __THROW ALIAS( tracked_free );
    void * trackedRealloc( void *, size_t ) __THROW;
    void * realloc( void *, size_t ) __THROW ALIAS( tracked_realloc );
    void * trackedCalloc( size_t, size_t ) __THROW;
    void * calloc( size_t, size_t ) __THROW ALIAS( tracked_calloc );
    void trackedCfree( void * ) __THROW;
    void cfree( void * ) __THROW ALIAS( tracked_cfree );
    void * trackedMemalign( size_t, size_t ) __THROW;
    void * memalign( size_t, size_t ) __THROW ALIAS( tracked_memalign );
    void * trackedValloc( size_t ) __THROW;
    void * valloc( size_t ) __THROW ALIAS( tracked_valloc );
    void * trackedPvalloc( size_t ) __THROW;
    void * pvalloc( size_t ) __THROW ALIAS( tracked_pvalloc );
    int trackedPosicMemalign( void **, size_t, size_t ) __THROW;
    int posix_memalign( void **, size_t, size_t ) __THROW ALIAS( tracked_posix_memalign );
};

// 'Untracked' memory is allocations made outside of the affinity system, including:
//   1) Use of 'platform' allocator
//   2) When an STL collection is made without AllocatorAffinity<...>
//   3) If calls are made to malloc/calloc/etc explicitly
//
// Tracked memory is logged in great detail periodically. Untracked memory is only known in a very
// approximate way. Namely the total of all untracked memory, with no individual object size breakdown
// or function that did the allocation. The following functions play some dso games by intercepting
// calls to key functions (e.g.: malloc()), do some book keeping work, then call the native implementation.
//
// With this, calls to malloc() will end up at trackedMalloc(), where we will do book keeping on the
// allocation. Calling untrackedMalloc() will just call the native malloc(). This later is used by
// the tracking system to prevent double-counting allocations.

int
trackedMadvise(
    void * site,
    size_t size,
    int advice ) __THROW
{
    // This is called by pthread teardown. This is disabled by default, change the condition to enable it.
    // Though understand that this can happen during unit tests.
    if( false ) {
        // TODO: log the call
        logStackTrace( false, false );
    }
    return untrackedMadvise( site, size, advice );
}

int
trackedMprotect(
    void const * site,
    size_t size,
    int prot ) __THROW
{
    // This is called by pthread create. This is disabled by default, change the condition to enable it.
    if( false ) {
        // TODO: log the call
        logStackTrace( false, false );
    }
    return untrackedMprotect( site, size, prot );
}

void *
trackedMmap(
    void * site,
    size_t size,
    int prot,
    int flags,
    int fd,
    off_t offset ) __THROW
{
    // This is called by pthread create. This is disabled by default, change the condition to enable it.
    if( false ) {
        // TODO: log the call
        logStackTrace( false, false );
    }
    return untrackedMmap( site, size, prot, flags, fd, offset );
}

int
trackedMunmap(
    void * site,
    size_t size ) __THROW
{
    // This is disabled by default, change the condition to enable it.
    if( false ) {
        // TODO: log the call
        logStackTrace( false, false );
    }
    return untrackedMunmap( site, size );
}

bool
getDeepTracking( void )
{
    if( !trackComplete ) {
        return false;
    }
    return *trackComplete;
}

void *
trackedMalloc(
    size_t size ) __THROW
{
    void * site = untrackedMalloc( size );
    if( true ) {  // TODO: this should be under the control of Configuration
        atomicAdd( &trackedUntracked, malloc_usable_size( site ));
        if( getDeepTracking() ) {
            if( ++insideTrackedMalloc == 0 ) {
                DeepTrackedUntrackedEntity entity;
                entity.size_ = size;
                entity.stackTrace_.bufferItems_ = backtrace( entity.stackTrace_.buffer_, stackLevels );
                trackedItems.insert( reinterpret_cast< uintptr_t >( site ), entity );
            }
            TOOLS_ASSERT( insideTrackedMalloc > 0 );
            --insideTrackedMalloc;
        }
    }
    return site;
}

void
trackedFree(
    void * site ) __THROW
{
    if( true ) {  // TODO: this should be under the control of Configuration
        atomicSubtract( &trackedUntracked, malloc_usable_size( site ));
        if( getDeepTracking() && !!site ) {
            trackedItems.erase( reinterpret_cast< uintptr_t >( site ));  // ok if it doesn't exist
        }
    }
    untrackedFree( site );
}

void *
trackedRealloc(
    void * site,
    size_t size ) __THROW
{
    if( true && !!site ) {  // TODO: this should be under the control of Configuration
        atomicSubtract( &trackedUntracked, malloc_usable_size( site ));
    }
    void * ret = untrackedRealloc( site, size );
    // While the contents of 'site' are now invalid, we can still use it's address to look things up.
    if( true && getDeepTracking() ) {  // TODO: this should be under the control of Configuration
        if( !!site ) {
            trackedItems.erase( reinterpret_cast< uintptr_t >( site ));
        }
        if( !!ret && ( insideTrackedMalloc++ == 0 )) {
            DeepTrackedUntrackedEntity entity;
            entity.size_ = size;
            entity.stackTrace_.bufferItems_ = backtrace( entity.stackTrace_.buffer_, stackLevels );
            trackedItems.insert( reinterpret_cast< uintptr_t >( ret ), entity );
            TOOLS_ASSERT( insideTrackedMalloc > 0 );
            --insideTrackedMalloc;
        }
    }
    if( true ) {  // TODO: this should be under the control of Configuration
        atomicAdd( &trackedUntracked, malloc_usable_size( ret ));
    }
    return ret;
}

void *
trackedCalloc(
    size_t count,
    size_t size ) __THROW
{
    void * ret = untrackedCalloc( count, size );
    if( true ) {  // TODO: this should be under the control of Configuration
        atomicAdd( &trackedUntracked, malloc_usable_size( ret ));
        if( getDeepTracking() ) {
            if( insideTrackedMalloc++ > 0 ) {
                DeepTrackedUntrackedEntity entity;
                entity.size_ = size;
                entity.stackTrace_.bufferItems_ = backtrace( entity.stackTrace_.buffer_, stackLevels );
                trackedItems.insert( reinterpret_cast< uintptr_t >( ret ), entity );
            }
            TOOLS_ASSERT( insideTrackedMalloc > 0 );
            --insideTrackedMalloc;
        }
    }
    return ret;
}

void
trackedCfree(
    void * site ) __THROW
{
    if( true ) {  // TODO: this should be under control of Configuration
        atomicSubtract( &trackedUntracked, malloc_usable_size( site ));
    }
    untrackedCfree( site );
}

void *
trackedMemalign(
    size_t align,
    size_t size ) __THROW
{
    void * ret = untrackedMemalign( align, size );
    if( true ) {  // TODO: this should be under control of Configuration
        atomicAdd( &trackedUntracked, malloc_usable_size( ret ));
    }
    return ret;
}

void *
trackedValloc(
    size_t size ) __THROW
{
    void * ret = untrackedValloc( size );
    if( true ) {  // TODO: this should be under control of Configuration
        atomicAdd( &trackedUntracked, malloc_usable_size( ret ));
    }
    return ret;
}

void
trackedPvalloc(
    size_t size ) __THROW
{
    void * ret = untrackedPvalloc( size );
    if( true ) {  // TODO: this should be under control of Configuration
        atomicAdd( &trackedUntracked, malloc_usable_size( ret ));
    }
    return ret;
}

int
trackedPosixMemalign(
    void ** ref,
    size_t align,
    size_t size ) __THROW
{
    int ret = untrackedPosixMemalign( ref, align, size );
    if( true ) {  // TODO: this should be under control of Configuration
        atomicAdd( &trackedUntracked, malloc_usable_size( *ref ));
    }
    return ret;
}

///////////
// VmemPool
///////////

VmemPool::VmemPool( void )
    : largePageSize_( getHugePageSize() )
    , largePagesAllocated_( 0 )
    , reservedUnmapped_( false )
    , leakProtect_( false )  // TODO: get this from Config
    , forceSmallPages_( false )  // TODO: get this from Config
    , numMmappers_( 0 )
{
    ssize_t workingMax;
    ssize_t workingMin;
    ssize_t physicalMemory;

    physicalMemory = detail::physicalMemory();
    // Our maximum is within a GB of the machine total.
    workingMax = physicalMemory - ( 1024LL * 1024LL * 1024LL);
    // The minimum is a GB lower.
    workingMin = ( workingMax < ( 2LL * 1024LL * 1024LL * 1024LL )) ?
        ( 1024LL * 1024LL * 1024LL ) :
        ( workingMax - ( 1024LL * 1024LL * 1024LL ));
    // Set the maximum number of large pages to below our working set
    size_t largePagesMax = (( workingMin == ( 1024LL * 1024LL * 1024LL )) ?
                      ( 512LL * 1024LL * 1024LL ) :
                      ( workingMin - ( 1024LL * 1024LL * 1024LL ))) /
        largePageSize_;
    // Try to arrange it so that this is unlikely to recycle
    addrs_.reserve( largePagesMax / reservationUnits );
    // Assume that roughly 1/4th of the pages will go unused
    reserved_.reserve( largePagesMax / 4U );
    initDesc();
    if( leakProtect_ ) {
        fprintf( stdout, "leak-protect enabled -- uses MMU to check for reads or writes after free, "
            "but will cause an out of memory, or at least a virtual memory explosion.\n" );
    }
}

VmemPool::~VmemPool( void )
{
    // Though the entries in reserved_ were aligned (and padded), we keep the original mmap addresses in
    // addrs_, and that is what we will unmap here.  If releaseReserved() has already been called, we're
    // munmapping some addresses twice. However this is not, of itself, an error.
    std::for_each( addrs_.begin(), addrs_.end(), [&]( void * site )->void {
        untrackedMunmap( site, largePageSize_ * reservationUnits );
    });
}

Pool::Desc const &
VmemPool::describe( void )
{
    if( !desc_.trace_ ) {
        desc_.trace_ = impl::resourceTraceBuild( StaticStringId( "VmemPool backing pages" ));
    }
    return desc_;
}

void *
VmemPool::map( void )
{
    // TODO: put this in
    // ComplainTimer< 100 > t( logSink, "VmemPool::map" );
    {
        std::unique_lock< std::mutex > l( availablePagelock_ );
        if( reservedUnmapped_ ) {
            // Another thread is already dying, and thus unmapped all of reaserved_'s pages. Don't
            // hand out any of those, which will cause immediate page faults that are indistinguishable
            // from memory corruptions. As such it is better to just abort.
            //
            // What remains unknown, does calling abort() while in the middle of crashing in some other
            // thread cause core files to be corrupted? A present I dno't have evidence either way.
            // However there may be alternatives:
            //   1) releaseReserved() is there so that core files are a managable size. We could stop
            //      doing that.
            //   2) hang here, while everything crashes around us. This seems questionable at best.
            
            // TODO: log something useful
            abort();
        }
        for( ;; ) {
            void * site = popReserved();
            if( !!site ) {
                return site;
            }
            if( atomicCas( &numMmappers_, 0U, 1U ) == 0U ) {
                // Won the race for doing a 'big' allocation. This will go into the kernel and is likely
                // to be slow.
                break;
            }
            // Another thread is already doing a 'big' allocation. Wait for it to finish (and/or for some
            // other thread to do a VmemPool::unmap()). There is no reason to do an mmap here if another
            // thread already is for a couple reasons:
            //   1) It will lead to a spike in memory consumption (we map 64 MB at a time, which is
            //      sufficient for many threads).
            //   2) The Linux kernel already serialzing mmap. Adding more only adds the time those later
            //      ones take.
            //   3) Patience is a virtue. If another thread does a VmemPool::unmap before we win the mmap
            //      race. Which means that we can recycle a 2 MB page before an existing mmap completes.
            availablePageCond_.wait( l );  // drops the lock while we wait.
        }
    }
    // Well, we are left with no choice. We have to go to the kernel to get more memory. After doing so,
    // we will add several pages to the free list and wake up any waiting threads.  The lock is dropped
    // during the mmap. Logically the lock/condvar combination protects the vector of free pages, so a
    // naive view says dropping the lock will have thundering herd problems. However, the atomicCas
    // prevents this. If we didn't drop the lock during a 'big' allocation, we would block VmemPool::unmap
    // from adding a recycled page to the free list and waking up waiting threads.
    bool gotLargePages;
    size_t numBytes = largePageSize_ * reservationUnits;
    void * site = bigAllocation( numBytes, !forceSmallPages_, gotLargePages );
    void * endPtr = reinterpret_cast< uint8 * >( site ) + numBytes;
    if( gotLargePages ) {
        atomicAdd( &largePagesAllocated_, reservationUnits );
    }
    void * ret = NULL;
    {
        std::unique_lock< std::mutex > l( availablePageLock_ );
        addrs_.push_back( site );
        // allign site
        site = reinterpret_cast< void * >( roundUpPow2( reinterpret_cast< uintptr_t >( site ), largePageSize_ ));
        endPtr = reinterpret_cast< void * >( roundDownPow2( reinterpret_cast< uintptr_t >( endPtr ), largePageSize_ ));
        // We get the first page. Only fair since we're doing most of the work.
        ret = site;
        site = reinterpret_cast< uint8 * >( site ) + largePageSize_;
        // The rest are put into the free list.
        while( site != endPtr ) {
            pushReserved( site );
            site = reinterpret_cast< uint8 * >( site ) + largePageSize_;
        }
    }
    atomicSet( &numMmappers_, 0U );  // Allow other threads to race for mmap. Though that shouldn't happen
                                     // until the pages we just added are all consumed.
    // Signal waiting threads that there are likely new pages available. We have already dropped the lock,
    // which is appropriate for condvars.
    availablePageCond_.notify_all();
    return ret;
}

void
VmemPool::unmap(
    void * site )
{
    // Add to reserved_, then signal waiting threads (if any). In order to make the push safe, we need
    // to take the lock. Important to note, the lock is never held during 'big' allocations, as that is
    // very expensive and probably slow.

    // TODO: return this
    // ComplainTimer< 100 > t( logSink_, "VmemPool::unmap" );
    if( TOOLS_UNLIKELY( leakProtect_ )) {
        leakAndProtect( site, largePageSize_, StaticStringId( "VmemPool" ));
        return;
    }
    {
        std::unique_lock< std::mutex > l( availablePageLock_ );
        pushReserved( site );
    }
    availablePageCond_.notify_one();  // We only added one page, so only wake up one thread.
}

size_t
VmemPool::getSysAllocatedSize( void ) const
{
    return addrs_.size() * largePageSize_ * reservationUnits;
}

size_t
VmemPool::getSysAllocatedSizeLargePages( void ) const
{
    return largePagesAllocated_ * largePageSize_;
}

void
VmemPool::releaseReserved( void )
{
    // reserved_ entries were not directly mmaped, the real mmaped addresses are in addrs_. So this
    // 'mismatch' between mmap and unmap is gross. However it is not an error to munmap() a region twice.

    // TODO: reinstate this
    // ComplainTimer< 100 > t( logSink_, "VmemPool::releaseReserved" )
    std::unique_lock< std::mutex > l( availablePageLock_ );
    std::for_each( reserved_.begin(), reserved_.end(), [&]( void * site )->void {
        untrackedMunmap( site, largePageSize_ );
    });
    reservedUnmapped_ = true;
}

size_t
VmemPool::getReservedSize( void ) const
{
    return reserved_.size() * largePageSize_;
}

void
VmemPool::initDesc( void )
{
    // Initialize the description
    desc_.size_ = largePageSize_;
    desc_.align_ = largePageSize_;
    desc_.phase_ = 0U;
    desc_.trace_ = NULL;
}

void *
VmemPool::popReserved( void )
{
    // Lock must be held
    TOOLS_ASSERT( !reservedUnmapped_ );
    if( !reserved_.empty() ) {
        void * ret = reserved_.back();
        reserved_.pop_back();
        return ret;
    }
    return NULL;
}

void
VmemPool::pushReserved(
    void * site )
{
    // lock must be held
    reserved_.push_back( site );
}

/////////////////
// AffinityMalloc
/////////////////

AffinityMalloc::AffinityMalloc(
    bool untracked )
    : untracked_( untracked )
    , vmem_( &globalVmemPool() )
{
}

void
AffinityMalloc::dispose( void )
{
    delete this;
}

Affinity &
AffinityMalloc::bind( void )
{
    // This doesn't really do anything useful, but must be implemented.
    // TODO: consider returning a thread-local malloc, if one exists
    return *this;
}

AutoDispose<>
AffinityMalloc::fork(
    Affinity ** referenceAffinity,
    impl::ResourceSample const & )
{
    // This doesn't really do anything useful, but must be implemented.
    AffinityMalloc * ret = new AffinityMalloc( untracked_ );
    *referenceAffinity = ret;
    return ret;
}

Pool &
AffinityMalloc::pool(
    size_t size,
    impl::ResourceSample const & sample,
    size_t phase )
{
    TOOLS_ASSERT( phase == 0 );
    if( size == ( 2ULL * 1024ULL * 1024ULL )) {
        // This is exactly what VmemPool is for.
        return *vmem_;
    }
    // Return a fake pool which implements map() as a call to malloc with fixed size.  If this
    // gets called alot, we need to consider if the caller is really behaving correctly.  This
    // is done rather than returning *vmem_ because doing so would turn every pool map into
    // 2MB, regardless of 'size', this is sub-optimal.
    TOOLS_ASSERT( size < ( 2ULL * 1024ULL * 1024ULL ));
    auto iter = pools_.find( size );
    if( iter != pools_.end() ) {
        return iter->second;
    }
    Pool::Desc d;
#if (__SIZEOF_POINTER__ == 4)
    d.align_ = 4;  // The only thing malloc can guarentee.  (32-bit)
#else  // 32-bit
    d.align_ = 8;  // The only thing malloc can guarantee.  (64-bit)
#endif  // 32-bit
    d.size_ = size;
    d.phase_ = phase;
    d.trace_ = impl::resourceTraceBuild( sample );
    iter = pools_.insert(typename PoolMap::value_type( size, AffinityMallocPool( this, d ))).first;
    return iter->second;
}

void *
AffinityMalloc::map(
    size_t size,
    impl::ResourceSample const &,
    size_t phase )
{
#ifdef TOOLS_DEBUG
    // TODO: re-enable this
    // ComplainTimer<50> t( logSink_, "AffinityMalloc::map()" );
#endif // TOOLS_DEBUG
    if( untracked_ ) {
        // similar to safe_malloc(), but untracked
        void * ret = untrackedMalloc( size );
        if( !!ret ) {
            return ret;
        }
        impl::outOfMemoryDie();
        return NULL;
    }
    // tracked allocation
    return impl::safeMalloc( size );
}

void
AffinityMalloc::unmap(
    void * site )
{
#ifdef TOOLS_DEBUG
    // TODO: re-enable this
    // ComplainTimer<50> t( logSink_, "AffinityMalloc::map()" );
#endif // TOOLS_DEBUG
    if( untracked_ ) {
        untrackedFree( site );
    }
    return ::free( site );
}

/////////////////////
// AffinityMallocPool
/////////////////////

AffinityMallocPool::AffinityMallocPool(
    AffinityMalloc * parent,
    Pool::Desc & desc )
    : parent_( parent )
    , desc_( desc )
{
}

Pool::Desc const &
AffinityMallocPool::describe( void )
{
    return desc_;
}

void *
AffinityMallocPool::map( void )
{
    return parent_->map(desc_.size_, TOOLS_RESOURCE_SAMPLE_CALLER( desc_.size_ ), desc_.phase_ );
}

void
AffinityMallocPool::unmap(
    void * site )
{
    parent_->unmap( site );
}

///////////////
// TrackedItems
///////////////

TrackedItems::TrackedItems( void )
    : destructed_( false )
{
}

TrackedItems::~TrackedItems( void )
{
    destructed_ = true;
}

void
TrackedItems::insert(
    uintptr_t site,
    DeepTrackedUntrackedEntity const & item )
{
    if( destructed_ ) {
        return;
    }
    std::unique_lock< std::mutex > l( trackedItemsLock_ );
    trackedItems_.insert( std::make_pair( site, item ));
    auto ins = trackedStackTraces_.insert( std::make_pair( item.stackTrace_, AllocCounts( 1, item.size_ )));
    if( !ins.second ) {
        ++ins.first->second.numAllocations_;
        ins.first->second.numBytes_ += item.size_;
    }
}

void
TrackedItems::erase(
    uintptr_t site )
{
    if( destructed_ ) {
        return;
    }
    std::unique_lock< std::mutex > l( trackedItemsLock_ );
    auto iter = trackedItems_.find( site );
    if( iter == trackedItems_.end() ) {
        return;  // shouldn't happen, but not cause for alarm
    }
    TrackedUntrackedStackTrace const & trace = iter->second.stackTrace_;
    size_t size = iter->second.size_;
    auto iterst = trackedStackTraces_.find( trace );
    TOOLS_ASSERT( iterst != trackedStackTraces_.end() );
    TOOLS_ASSERT( iterst->second.numAllocations_ >= 1 );
    TOOLS_ASSERT( iterst->second.numBytes_ >= size );
    --iterst->second.numAllocations_;
    iterst->second.numBytes_ -= size;
    trackedItems_.erase( iter );
}

void
TrackedItems::collect(
    std::vector< std::pair< TrackedUntrackedStackTrace, AllocCounts >, BlindAllocator< std::pair< TrackedUntrackedStackTrace, AllocCounts >>> & vec )
{
    if( destructed_ ) {
        return;
    }
    std::unique_lock< std::mutex > l( trackedItemsLock_ );
    vec.reserve( trackedStackTraces_.size() );
    std::for_each( trackedStackTraces_.begin(), trackedStackTraces_.end(), [ &vec ]( std::pair< TrackedUntrackedStackTrace const, AllocCounts > & v )->void {
        vec.push_back( v );
    });
}
