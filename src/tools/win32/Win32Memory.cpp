#include "toolsprecompiled.h"

#include <tools/InterfaceTools.h>
#include <tools/Tools.h>

#include "Win32Tools.h"

#include <algorithm>

using namespace tools;

namespace {
    struct HeapInfo
    {
        enum : size_t {
            reservationUnits = 32,
        };

        HeapInfo( void );
        ~HeapInfo( void );

        void * popReserved( void );
        void pushReserved( void * );
        size_t getReservedSize( void ) const;
        size_t getSysAllocatedSize( void ) const;
        void * map( void );
        void unmap( void * );

        HANDLE heapProcess_;
        CRITICAL_SECTION largePageLock_;
        size_t largePageSize_;
        size_t largePageMax_;
        // Should we even try for large page allocations?
        bool largePageEnabled_;
        // Complete set of reservations we'll free at shutdown.
        void ** addrs_;
        unsigned addrsUsed_;
        unsigned addrsMax_;
        // Reserved addresses, which are used mainly for small pages that will
        // successfully decomit.
        void ** reserved_;
        unsigned reservedUsed_;
        unsigned reservedMax_;
    };

    // A large pool with normalized binding
    struct HeapInfoPoolProxy
        : Pool
    {
        HeapInfoPoolProxy( void );

        // Pool
        Pool::Desc & describe( void );
        void * map( void );
        void unmap( void * );

        HeapInfo * info_;
        Pool::Desc desc_;
    };

    struct Win32Affinity
        : Affinity
    {
        Win32Affinity( bool, HANDLE, impl::ResourceTrace * );

        // Affinity
        AutoDispose<> fork( Affinity **, impl::ResourceSample const & );
        Pool & pool( size_t, impl::ResourceSample const &, size_t );
        void * map( size_t, impl::ResourceSample const &, size_t );
        void unmap( void * );

        bool bound_;
        HANDLE heap_;
        impl::ResourceTrace * trace_;
        HeapInfoPoolProxy pool_;
    };

    struct AffinityBound
        : Win32Affinity
    {
        AffinityBound( HANDLE, impl::ResourceTrace * );
        Affinity & bind( void );
    };

    // This is not StandardDisposable<...> because we do not want to inherit an allocator.
    struct AffinityFork
        : Win32Affinity
        , Disposable
    {
        explicit AffinityFork( bool, impl::ResourceTrace * );
        ~AffinityFork( void );

        // Disposable
        void dispose( void );

        Affinity & bind( void );

        AffinityBound bound_;
    };

    struct AffinityGlobal
        : Win32Affinity
    {
        explicit AffinityGlobal( bool, impl::ResourceTrace * );
        Affinity & bind( void );
    };
};  // anonymous namespace

///////////////////////
// Non-member functions
///////////////////////

namespace tools
{
    namespace detail
    {
        uint64
        physicalMemory( void )
        {
            static bool init = false;
            static uint64 ret = 0ULL;
            if( !init ) {
                MEMORYSTATUSEX statex;
                statex.dwLength = sizeof( statex );
                GlobalMemoryStatusEx( &statex );
                ret = statex.ullTotalPhys;
                init = true;
            }
            TOOLS_ASSERT( ret != 0 );
            return ret;
        }
    };  // detail namespace
    namespace impl
    {
        void *
        vmemPoolMap(
            void * site )
        {
            return VirtualAlloc( site, 65536U, MEM_RESERVE | MEM_COMMIT,
                PAGE_READWRITE );
        }

        void
        vmemPoolDecommit(
            void * page )
        {
            VirtualFree( page, 4096U, MEM_DECOMMIT );
        }

        void
        vmemPoolRelease(
            void * site )
        {
            VirtualFree( site, 0, MEM_RELEASE );
        }

        inline bool
        memoryProtect(
            void * site,
            size_t size,
            bool canRead )
        {
            DWORD old = 0;
            DWORD flags = static_cast< DWORD >( canRead ? PAGE_READONLY : PAGE_NOACCESS );
            BOOL protect = VirtualProtect( site, size, flags, &old );
            TOOLS_ASSERT( old == PAGE_READWRITE );
            return !!protect;
        }

        inline void
        memoryUnprotect(
            void * site,
            size_t size )
        {
            DWORD old = 0;
            VirtualProtect( site, size, PAGE_READWRITE, &old );
        }

        void
        leakAndProtect(
            void * site,
            size_t size,
            StringId const & )
        {
            memoryProtect( site, size, false );
            // TODO: find the Win equivalent of MADV_DONTNEED
        }

        void
        platformCapVsize(
            uint64 )
        {
            // This is a no-op on windows, which doesn't have setrlimit()
        }

        void
        platformReleaseMemory( void )
        {
            // This is a no-op on windows.
        }

        void
        platformUncapVsize( void )
        {
            // This is a no-op on windows, which doesn't have setrlimit()
        }

        uint64
        platformVsizeCap( void )
        {
            return std::numeric_limits< uint64 >::max();
        }

        void *
        platformHugeAlloc(
            size_t size )
        {
            void * ret = VirtualAlloc( nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );
            if( ret ) {
                outOfMemoryDie();
            }
            return ret;
        }

        void
        platformHugeFree(
            void * site,
            size_t )
        {
#ifdef TOOLS_DEBUG
            bool success = !!
#endif // TOOLS_DEBUG
                VirtualFree( site, 0, MEM_RELEASE );
            TOOLS_ASSERT( success );
        }
    };  // impl namespace

    Affinity *
    staticServiceCacheInit( Affinity ***, Platform *** )
    {
        static AffinityGlobal affinity(false, nullptr);
        return &affinity;
    }

    Affinity *
    staticServiceCacheInit( Affinity ***, PlatformUntracked *** )
    {
        // Presently this is the same.
        return &impl::affinityInstance< Platform >();
    }
};  // tools namesapce

static Affinity &
affinityGlobalBound( void )
{
    static AffinityGlobal globalBound_( true, nullptr );
    return globalBound_;
}

static HeapInfo &
globalInfo( void )
{
    static HeapInfo info_;
    return info_;
}

bool
tools::detail::memoryValidate(void)
{
    return (!!HeapValidate(globalInfo().heapProcess_, 0, nullptr));
}

static bool
enablePrivileges(
    size_t largePageSize,
    size_t & largePageMax )
{
    TOKEN_PRIVILEGES privs;
    HANDLE token;
    BOOL ret = OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES,
        &token );
    bool enabled = false;
    if( !ret ) {
        return false;
    }
    ret = LookupPrivilegeValue( nullptr, SE_LOCK_MEMORY_NAME, &privs.Privileges[ 0 ].Luid );
    if( !ret ) {
        return false;
    }
    privs.PrivilegeCount = 1;
    privs.Privileges[ 0 ].Attributes = SE_PRIVILEGE_ENABLED;
    ret = AdjustTokenPrivileges( token, FALSE, &privs, sizeof( TOKEN_PRIVILEGES ), nullptr, nullptr );
    if( !!ret ) {
        DWORD err = GetLastError();
        if( err != ERROR_SUCCESS ) {
            SetLastError( err );
            // maybe log this
        } else {
            enabled = true;
        }
    }
    CloseHandle( token );
    // Figure out the amount of memory and try to claim all of it
    {
        SIZE_T workingMax, workingMin;
        ULONGLONG physicalMemory;
        GetPhysicallyInstalledSystemMemory( &physicalMemory );
        // Our maximum is within a GB of the machine total
        workingMax = ( physicalMemory - ( 1024U * 1024U )) * 1024ULL;
        // The minimum is a GB lower.
        workingMin = ( workingMax < ( 2ULL * 1024ULL * 1024ULL * 1024ULL ) ?
            ( 1024ULL * 1024ULL * 1024ULL ) : ( workingMax - ( 1024ULL * 1024ULL * 1024ULL )));
        ret = SetProcessWorkingSetSizeEx( GetCurrentProcess(), workingMin,
            workingMax, QUOTA_LIMITS_HARDWS_MIN_ENABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE );
        // Set the maximum number of large pages to our working set.
        largePageMax = (( workingMin == ( 1024ULL * 1024ULL * 1024ULL )) ?
            ( 512ULL * 1024ULL * 1024ULL ) : ( workingMin - ( 1024ULL * 1024ULL * 1024ULL ))) /
            largePageSize;
    }
    if( ret ) {
        SIZE_T workingMin, workingMax;
        DWORD workingFlags;
        ret = GetProcessWorkingSetSizeEx( GetCurrentProcess(), &workingMin,
            &workingMax, &workingFlags );
        if( !!ret ) {
            // May want to log some of this
            ;
        }
    }
    return enabled;
}

///////////
// HeapInfo
///////////

HeapInfo::HeapInfo( void )
{
    heapProcess_ = GetProcessHeap();
    // Enable locking pages, and by extension large pages.
    largePageSize_ = GetLargePageMinimum();
    InitializeCriticalSection( &largePageLock_ );
    largePageEnabled_ = enablePrivileges( largePageSize_, largePageMax_ );
    // Try and size these so they're pretty unlikely to be recycled.
    addrsUsed_ = 0;
    addrsMax_ = static_cast< unsigned >( largePageMax_ / reservationUnits );
    addrs_ = static_cast< void ** >( HeapAlloc( heapProcess_, 0, sizeof( void * ) * addrsMax_ ));
    // Assume about a quarter of the pages will be unused.
    reservedUsed_ = 0;
    reservedMax_ = static_cast< unsigned >( largePageMax_ / 4U );
    reserved_ = static_cast< void ** >( HeapAlloc( heapProcess_, 0, sizeof( void * ) * reservedMax_ ));
}

HeapInfo::~HeapInfo( void )
{
    DeleteCriticalSection( &largePageLock_ );
    std::for_each( addrs_, addrs_ + addrsUsed_, []( void * site ){ VirtualFree( site, 0, MEM_RELEASE ); });
    HeapFree( heapProcess_, 0, reserved_ );
    HeapFree( heapProcess_, 0, addrs_ );
}

void *
HeapInfo::popReserved( void )
{
    // must be under the lock.
    if( reservedUsed_ != 0 ) {
        return reserved_[ --reservedUsed_ ];
    }
    return nullptr;
}

void
HeapInfo::pushReserved(
    void * site )
{
    if( reservedUsed_ == reservedMax_ ) {
        reservedMax_ *= 2;
        reserved_ = static_cast< void ** >( HeapReAlloc( heapProcess_, 0, reserved_,
            sizeof( void * ) * reservedMax_ ));
    }
    reserved_[ reservedUsed_++ ] = site;
}

size_t
HeapInfo::getReservedSize( void ) const
{
    return reservedUsed_ * largePageSize_;
}

size_t
HeapInfo::getSysAllocatedSize( void ) const
{
    return addrsUsed_ * largePageSize_ * reservationUnits;
}

#pragma warning ( disable : 4706 )
void *
HeapInfo::map( void )
{
    void * site;
    EnterCriticalSection( &largePageLock_ );
    while( !( site = popReserved() )) {
        // Yeah this is ugly.  Unpleasant lock manipulation.
        LeaveCriticalSection( &largePageLock_ );
            site = VirtualAlloc( nullptr, largePageSize_ * reservationUnits,
                MEM_RESERVE, PAGE_NOACCESS );
            void * endPtr = reinterpret_cast< uint8 * >( site ) +
                ( largePageSize_ * reservationUnits );
        EnterCriticalSection( &largePageLock_ );
        if( addrsUsed_ == addrsMax_ ) {
            addrsMax_ *= 2;
            addrs_ = static_cast< void ** >( HeapReAlloc( heapProcess_, 0, addrs_,
                sizeof( void * ) * reservedMax_ ));
        }
        addrs_[ addrsUsed_++ ] = site;
        // Now align site
        size_t offset = reinterpret_cast< size_t >( site ) & ( largePageSize_ - 1U );
        if( offset != 0 ) {
            site = reinterpret_cast< uint8 * >( site ) + ( largePageSize_ - offset );
        }
        endPtr = reinterpret_cast< void * >( reinterpret_cast< size_t >( endPtr ) &
            ~static_cast< size_t >( largePageSize_ - 1U ));
        // Effectively do an unmap of everything, which we will pull out again at the
        // top of the loop.
        while( reinterpret_cast< uint8 * >( site ) < reinterpret_cast< uint8 * >( endPtr )) {
            pushReserved( site );
            site = reinterpret_cast< uint8 * >( site ) + largePageSize_;
#ifdef TOOLS_DEBUG
            // Add a 2 MB buffer for bounds checking
            site = reinterpret_cast< uint8 * >( site ) + largePageSize_;
#endif // TOOLS_DEBUG
        }
    }
    LeaveCriticalSection( &largePageLock_ );
    VirtualAlloc( site, largePageSize_, MEM_COMMIT, PAGE_READWRITE );
    return site;
}
#pragma warning ( default : 4706 )

void
HeapInfo::unmap(
    void * site )
{
    VirtualFree( site, largePageSize_, MEM_DECOMMIT );
    EnterCriticalSection( &largePageLock_ );
        pushReserved( site );
    LeaveCriticalSection( &largePageLock_ );
}

////////////////////
// HeapInfoPoolProxy
////////////////////

HeapInfoPoolProxy::HeapInfoPoolProxy( void )
    : info_( &globalInfo() )
{
    desc_.size_ = info_->largePageSize_;
    desc_.align_ = info_->largePageSize_;
    desc_.phase_ = 0U;
    desc_.trace_ = nullptr;
}

Pool::Desc &
HeapInfoPoolProxy::describe( void )
{
    if( !desc_.trace_ ) {
        static impl::ResourceSample poolSample( 2U * 1024U * 1024U,
            reinterpret_cast< void * >( &globalInfo ) );
        desc_.trace_ = impl::resourceTraceBuild( 1, poolSample );
    }
    return desc_;
}

void *
HeapInfoPoolProxy::map( void )
{
    return info_->map();
}

void
HeapInfoPoolProxy::unmap(
    void * site )
{
    info_->unmap( site );
}

////////////////
// Win32Affinity
////////////////

Win32Affinity::Win32Affinity(
    bool bound,
    HANDLE heap,
    impl::ResourceTrace * trace )
    : bound_( bound )
    , heap_( heap )
    , trace_( trace )
    , pool_()
{
}

AutoDispose<>
Win32Affinity::fork(
    Affinity ** parent,
    impl::ResourceSample const & sample )
{
    if( !trace_ ) {
        static impl::ResourceSample global_( 0U, static_cast< void * >( nullptr ), nullptr );
        trace_ = impl::resourceTraceBuild( 1, global_ );
    }
    AffinityFork * ret = new AffinityFork( bound_, impl::resourceTraceBuild( 1, sample, trace_ ));
    *parent = ret;
    return std::move(ret);
}

Pool &
Win32Affinity::pool(
    size_t size,
    impl::ResourceSample const &,
    size_t phase )
{
    TOOLS_ASSERT( size <= pool_.info_->largePageSize_ );
    // Phases are not supported
    TOOLS_ASSERT( phase == 0 );
    return pool_;
}

void *
Win32Affinity::map(
    size_t size,
    impl::ResourceSample const &,
    size_t )
{
    // We don't honor phases in platform
    return HeapAlloc( heap_, 0, size );
}

void
Win32Affinity::unmap(
    void * site )
{
    HeapFree( heap_, 0, site );
}

////////////////
// AffinityBound
////////////////

AffinityBound::AffinityBound(
    HANDLE heap,
    impl::ResourceTrace * trace )
    : Win32Affinity( true, heap, trace )
{
}

Affinity &
AffinityBound::bind( void )
{
    // This is already bound.
    return *this;
}

///////////////
// AffinityFork
///////////////

AffinityFork::AffinityFork(
    bool bound,
    impl::ResourceTrace * trace )
    : Win32Affinity( bound,
        HeapCreate( HEAP_GENERATE_EXCEPTIONS | ( bound ? HEAP_NO_SERIALIZE : 0U ), 0, 0 ),
        trace )
    , bound_( heap_, trace )
{
}

AffinityFork::~AffinityFork( void )
{
    HeapDestroy( heap_ );
}

void
AffinityFork::dispose( void )
{
    delete this;
}

Affinity &
AffinityFork::bind( void )
{
    return bound_;
}

/////////////////
// AffinityGlobal
/////////////////

AffinityGlobal::AffinityGlobal(
    bool bound,
    impl::ResourceTrace * trace )
    : Win32Affinity( bound, globalInfo().heapProcess_, trace )
{
}

Affinity &
AffinityGlobal::bind( void )
{
    return affinityGlobalBound();
}
