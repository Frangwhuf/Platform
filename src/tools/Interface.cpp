#include "toolsprecompiled.h"

#include <tools/Algorithms.h>
#include <tools/Concurrency.h>
#include <tools/Interface.h>
#include <tools/String.h>
#include <tools/Threading.h>
#include <tools/Tools.h>

#include <algorithm>
#include <numeric>
#include <vector>
#ifdef WINDOWS_PLATFORM
#  pragma warning( disable : 4365 )
#  pragma warning( disable : 4371 )
#  pragma warning( disable : 4571 )
#  pragma warning( disable : 4619 )
#endif // WINDOWS_PLATFORM
#include <boost/numeric/conversion/cast.hpp>
#include <boost/format.hpp>
#ifdef WINDOWS_PLATFORM
#  pragma warning( default : 4365 )
#  pragma warning( default : 4371 )
#  pragma warning( default : 4571 )
#  pragma warning( default : 4619 )
#endif // WINDOWS_PLATFORM

using namespace tools;
using boost::numeric_cast;

namespace tools {
    namespace impl {
        AutoDispose< Monitor > monitorPlatformNew( void );
        bool memoryTrack( void );
        void stringIdGetMemoryTracking( uint64 &, uint64 &, uint64 & );
        unsigned platformStackCount( void );
        size_t platformStackBytes( void );
        void logUntrackedMemory( void );
        void platformMallocStats( void );
    }; // impl namespace
}; // tools namespace

namespace {
    struct NullDisposable
        : Disposable
    {
        void dispose( void );
    };

    // The primary data structure used by memory tracking code. One of these per unique 'trace'. The only
    // allocation interface is resourceTraceBuild(), which allocates if needed, or returns a pointer to an
    // existing trace from the global hash table.
    struct ResourceTraceImpl
        : tools::impl::ResourceTrace
    {
        // Not derived from any AllocStatic<...>, not even AllocStatic< PlatformUntracked >. We do track
        // this structure's memory usage, but manually, to avoid recursion.
        ResourceTraceImpl( unsigned, impl::ResourceSample const &, impl::ResourceTrace * );

        // ResourceTrace
        StringId name( void ) const;
        size_t size( void ) const;
        void * symbol( void ) const;
        void inc( size_t );
        void dec( size_t );
        unsigned interval( void );

        // allocation
        void * operator new( size_t ) {
            resourceTraceImplsAllocated += 1;
            return impl::affinityInstance< PlatformUntracked >().alloc( sizeof( ResourceTraceImpl ));
        }
        void operator delete( void * site ) {
            resourceTraceImplsAllocated -= 1;
            impl::affinityInstance< PlatformUntracked >().free( site );
        }

        unsigned interval_;  // tracking interval
        ResourceTraceImpl * next_;  // hash table input
        // key
        impl::ResourceSample sample_;
        impl::ResourceTrace * target_; // "name -> target" in logging
        size_t volatile currAllocated_;
        StringId mutable name_;  // cached
        static ScalableCounter resourceTraceImplsAllocated;
    private:
        // Not impossible to implement, just really annoying
        void * operator new[]( size_t );
        void operator delete[]( void * );
    };
};  // anonymous namespace

static size_t const resourceTraceTableSize = 65536U;
static ResourceTraceImpl * volatile resourceTraces_[ resourceTraceTableSize ];
static size_t const resourceTraceDumpMinSize = 65536U;
ScalableCounter ResourceTraceImpl::resourceTraceImplsAllocated;

struct SymbolCache {
    SymbolCache(
        void * addr = NULL,
        StringId const & name = tools::StringIdNull(),
        unsigned offset = 0U )
        : addr_( addr )
        , name_( name )
        , offset_( offset )
    {
    }

    SymbolCache(
        SymbolCache const & right )
        : addr_( right.addr_ )
        , name_( right.name_ )
        , offset_( right.offset_ )
    {}

    SymbolCache &
    operator=(
        SymbolCache const & right )
    {
        addr_ = right.addr_;
        name_ = right.name_;
        offset_ = right.offset_;
        return *this;
    }

    void * addr_;
    StringId name_;
    unsigned offset_;
};

static unsigned const symbolCacheSize_ = 4096U;
static SymbolCache symbolCacheHash_[ symbolCacheSize_ ];

///////////////////////
// Non-member functions
///////////////////////


////////////////////
// ResourceTraceImpl
////////////////////

ResourceTraceImpl::ResourceTraceImpl(
    unsigned interval,
    impl::ResourceSample const & res,
    impl::ResourceTrace * target )
    : interval_( interval )
    , next_( NULL )
    , sample_( res )
    , target_( target )
    , currAllocated_( 0 )
{
}

StringId
ResourceTraceImpl::name( void ) const
{
    if( TOOLS_LIKELY( !!name_ )) {
        return name_;
    }
    // Wait until the end to update name_, so that if two threads are calling name(), we still get the
    // expected result.
    StringId localName = sample_.name_;
    if( !localName ) {
        // No name supplied? Use the address.
        TOOLS_ASSERT( !!sample_.site_ );
        unsigned symOffset = 0;
        StringId mangledLocal = detail::symbolNameFromAddress( sample_.site_, &symOffset );
        localName = detail::platformDemangleSymbol( mangledLocal );
        if( symOffset > 0 ) {
            // TODO: delete the following
            //char offsetBuffer[ 40 ];
            //sprintf( offsetBuffer, "+0x%x", symOffset );  // TODO: convert to platform version of snprintf
            boost::format fmt( "%s+0x%x" );
            fmt % localName.c_str() % symOffset;
            localName = fmt.str();
        }
    }
    // name[baseName]
    if( !!sample_.parent_ ) {
        boost::format fmt( "%s[%s]" );
        fmt % localName.c_str() % sample_.parent_->name().c_str();
        localName = fmt.str();
    }
    // name->target
    if( !!target_ ) {
        boost::format fmt( "%s->%s" );
        fmt % localName.c_str() % target_->name().c_str();
        localName = fmt.str();
    }
    return name_ = localName;
}

size_t
ResourceTraceImpl::size( void ) const
{
    return sample_.size_;
}

void *
ResourceTraceImpl::symbol( void ) const
{
    return sample_.site_;
}

void
ResourceTraceImpl::inc(
    size_t count )
{
    TOOLS_ASSERT( interval_ != 0 );  // don't inc() a parent trace.
    atomicAdd< size_t >( &currAllocated_, static_cast< ptrdiff_t >( count ));
}

void
ResourceTraceImpl::dec(
    size_t count )
{
    TOOLS_ASSERT( interval_ != 0 );  // don't dec() a parent trace.
    // This assert tends to be the one that catches bugs w.r.t. multiple hash table entries for the same
    // logical item, or buggy hash functions.
    TOOLS_ASSERT( atomicRead( &currAllocated_ ) >= count );  // if this fires it is a double free
    atomicSubtract< size_t >( &currAllocated_, static_cast< ptrdiff_t >( count ));
}

unsigned
ResourceTraceImpl::interval( void )
{
    return interval_;
}

static AutoDispose< Monitor > &
symbolCacheLock()
{
    static AutoDispose< Monitor > symLock;
    if( !symLock ) {
        AutoDispose< Monitor > newLock( std::move( impl::monitorPlatformNew() ));
        if( !atomicCas< Monitor *, Monitor * >( symLock.atomicAccess(), NULL, newLock.get() )) {
            newLock.release();
        }
    }
    return symLock;
}

#ifdef WINDOWS_PLATFORM
#  pragma warning( disable : 4706 )
#endif // WINDOWS_PLATFORM
StringId
detail::symbolNameFromAddress(
    void * site,
    unsigned * offset )
{
    static StringId unknown( StaticStringId( "[unk]" ));
    size_t hash = ( reinterpret_cast< size_t >( site ) >> 3 ) & ( symbolCacheSize_ - 1U );
    size_t endHash = hash + 8;
    for( size_t iHash=hash; iHash!=endHash; ++iHash ) {
        // Locate the potential slot with wrap-around
        SymbolCache c = symbolCacheHash_[ iHash & ( symbolCacheSize_ - 1U ) ];
        if( IsNullOrEmptyStringId( c.name_ )) {
            // No entry
            break;
        }
        if( c.addr_ == site ) {
            if( !!offset ) {
                *offset = c.offset_;
            }
            return c.name_;
        }
    }
    // If we get here, we need to do a hash lookup
    AutoDispose<> l_( symbolCacheLock()->enter() );
    unsigned innerOffset = 0;
    StringId ret = tools::detail::platformSymbolNameFromAddress( site, &innerOffset );
    if( IsNullOrEmptyStringId( ret )) {
        ret = unknown;
    }
    // First try and do a friendly collide
    size_t iHash;
    for( iHash=hash; iHash!=endHash; ++iHash ) {
        // Locate the potential slot with wrap-around
        SymbolCache * c = symbolCacheHash_ + ( iHash & ( symbolCacheSize_ - 1U ));
        if( IsNullOrEmptyStringId( c->name_ )) {
            // Found a non-colliding spec
            c->addr_ = site;
            c->name_ = ret;
            c->offset_ = innerOffset;
            break;
        }
    }
    if( iHash == endHash ) {
        // Do an unfriendly collide
        SymbolCache c( site, ret, innerOffset );
        symbolCacheHash_[ hash ] = c;
    }
    if( !!offset ) {
        *offset = innerOffset;
    }
    return ret;
}
#ifdef WINDOWS_PLATFORM
#  pragma warning( default : 4706 )
#endif // WINDOWS_PLATFORM

AutoDispose<>
tools::nullDisposable( void )
{
    static NullDisposable null_;
    return &null_;
}

/////////////////////
TOOLS_DEBUG_OPT_BEGIN
/////////////////////

static uint32
resourceSampleHash(
    unsigned interval,
    impl::ResourceSample const & sample,
    impl::ResourceTrace * target )
{
    // Hash only one of {site, name}. Prefer site, but use name if site is NULL.
    TOOLS_ASSERT( !!sample.site_ || !!sample.name_ );
    // We use use hashAnyBegin(x, 0), not hashAnyBegin(x). The former initializes the hash to a number (0),
    // the later initializes the hash to a hash of the type name 'ResourceSample'. Getting the hash of the
    // type name invoves some StringId work, which in the future may also become tracked memory. We need
    // to avoid infinite recursion in that case. Since the type name is not an interesting component of
    // this hash, and we're adding pleanty of other useful fields to the hash, zero works fine.
    if( !!sample.site_ ) {
        return impl::hashMix( interval, impl::hashMix( target, impl::hashMix( sample.parent_,
            impl::hashMix( sample.size_, impl::hashAny( sample.site_ )))));
    } else {
        return impl::hashMix( interval, impl::hashMix( target, impl::hashMix( sample.parent_,
            impl::hashMix( sample.size_, impl::hashAny( sample.name_ )))));
    }
}

static ResourceTraceImpl *
resourceSampleBucketPeek(
    unsigned interval,
    impl::ResourceSample const & sample,
    ResourceTraceImpl * addrBucket,
    impl::ResourceTrace * target )
{
#ifdef TOOLS_DEBUG
    const uint32 verifyBucket = resourceSampleHash( interval, sample, target ) % resourceTraceTableSize;
#endif // TOOLS_DEBUG
    for(; !!addrBucket; addrBucket = addrBucket->next_ ) {
        // This assert is a little pricey, but has caught some bugs
        TOOLS_ASSERT( resourceSampleHash( addrBucket->interval_, addrBucket->sample_, addrBucket->target_ ) % resourceTraceTableSize == verifyBucket );
        if( ( addrBucket->sample_ == sample ) && ( addrBucket->target_ == target ) && ( addrBucket->interval_ == interval )) {
            return addrBucket;
        }
    }
    return NULL;
}

static impl::ResourceSample
resourceSampleAlignSize(
    impl::ResourceSample const & sample )
{
    impl::ResourceSample ret = sample;
    if( ret.size_ < 128 ) {
        // Tiny sizes are left alone.
        return ret;
    }
    if( ret.size_ < 16384 ) {
        // Small sizes are aligned to 64 bytes
        ret.size_ = ( ret.size_ + 63U ) & ~numeric_cast< size_t >( 63U );
    } else if( ret.size_ < ( 256U * 1024U )) {
        // Medium sizes are aligned to 4k
        ret.size_ = ( ret.size_ + 4095U ) & ~numeric_cast< size_t >( 4095U );
    } else {
        // Large sizes are aligned to 64k
        ret.size_ = ( ret.size_ + 65535U ) & ~numeric_cast< size_t >( 65535U );
    }
    return ret;
}

impl::ResourceTrace *
impl::resourceTraceBuild(
    impl::ResourceSample const & sample,
    impl::ResourceTrace * target )
{
    // Interval of 0, which should prevent inc/dec.
    return impl::resourceTraceBuild( 0U, sample, target );
}

impl::ResourceTrace *
impl::resourceTraceBuild(
    unsigned interval,
    ResourceSample const & sample,
    ResourceTrace * target )
{
    // Must provide either a name or an address, which we use in searching.  Some subtlety: if name_ is
    // initially NULL, but address isn't, then the trace's name gets updated later at runtime (see name()).
    // But if we are provided with a non-NULL name to start with, we will use that.
    if( !sample.name_ ) {
        TOOLS_ASSERT( !!sample.site_ );
    } else {
        TOOLS_ASSERT( !sample.site_ );
    }
    // Butchering the size has downsides, but at least one upside: creating fewer trace impls by collapsing
    // ones with the same name and only slightly different size.
    impl::ResourceSample localSample = resourceSampleAlignSize( sample );
    const uint32 hash = resourceSampleHash( interval, sample, target ); // must do after size adjustment
    ResourceTraceImpl * foundRoot;
    // Keep track of this in case we're retrying
    ResourceTraceImpl * allocatedRoot = NULL;
    atomicTryUpdate( &resourceTraces_[ hash % resourceTraceTableSize ], [ =, &foundRoot, &allocatedRoot ]( ResourceTraceImpl ** ref )->bool {
        if( TOOLS_UNLIKELY( !!allocatedRoot )) {
            delete allocatedRoot;
            allocatedRoot = NULL;
        }
        foundRoot = resourceSampleBucketPeek( interval, sample, *ref, target );
        if( !!foundRoot ) {
            return false;
        }
        allocatedRoot = new ResourceTraceImpl( interval, sample, target );
        allocatedRoot->next_ = *ref;
        foundRoot = allocatedRoot;
        *ref = allocatedRoot;
        return true;
    });
    return foundRoot;
}

impl::ResourceTrace *
impl::resourceTraceBuild(
    unsigned interval,
    StringId const & name,
    size_t nbytes,
    ResourceTrace * target )
{
    TOOLS_ASSERT( !!name );
    // Because we're lazy, we want reuse as much code as possible. In this case we have to cook up a dummy
    // ResourceSample in order to do this. The only field of the ResourceSample that really gets used is
    // size_.
    StringId localName = name;
    impl::ResourceSample dummy( nbytes, name );
    ResourceTraceImpl * result = static_cast< ResourceTraceImpl * >( impl::resourceTraceBuild( interval, dummy, target ));
    if( !!target ) {
        boost::format fmt( "%s->%s" );
        fmt % name.c_str() % target->name().c_str();
        localName = fmt.str();
    }
    result->name_ = localName;
    return result;
}

impl::ResourceTrace *
impl::resourceTraceBuild(
    StringId const & name,
    ResourceTrace * target )
{
    TOOLS_ASSERT( !!name );
    return impl::resourceTraceBuild( 0U, name, 0, target );
}

namespace {
    static bool
    shouldDump(
        ResourceTraceImpl & res,
        impl::ResourceTraceDumpPhase phase )
    {
        if( ( res.sample_.size_ == 0 ) || ( res.currAllocated_ == 0 ) || ( phase == impl::resourceTraceDumpPhaseInitial )) {
            return false;
        }
        TOOLS_ASSERT( res.interval_ > 0 );
        size_t synthAllocated = res.currAllocated_ * res.interval_;
        size_t numBytes = res.sample_.size_ * synthAllocated;
        return ( phase == impl::resourceTraceDumpPhaseAll ) || (res.sample_.size_ >= 16384 ) ||
            ( numBytes >= 65536 ) || ( synthAllocated >= 256 );
    }
};  // anonymous namespace

///////////////////
TOOLS_DEBUG_OPT_END
///////////////////

void
impl::resourceTraceDump(
    impl::ResourceTrace * trace )
{
    for( size_t i=0; i!=resourceTraceTableSize; ++i ) {
        for( ResourceTraceImpl * j=resourceTraces_[ i ]; !!j; j=j->next_ ) {
            if( j->target_ != trace ) {
                continue;
            }
            StringId name = detail::symbolNameFromAddress( j->symbol() );
            if( j->currAllocated_ > 0 ) {
                // TODO: log this
                fprintf( stderr, "leak\t%d\t%d\t%s", j->currAllocated_, j->size(), name.c_str() );
            }
        }
    }
}

void
impl::resourceTraceDump(
    impl::ResourceTraceDumpPhase phase,
    bool assertNoAlloc,
    std::vector< impl::ResourceTraceSum, AllocatorAffinity< impl::ResourceTraceSum, Platform >> * storage )
{
    if( phase != resourceTraceDumpPhaseInitial ) {
        // start frame
    }
    if( phase == resourceTraceDumpPhasePeriodic ) {
        // Log periodic dump starting
    } else if( phase == resourceTraceDumpPhaseWatermark ) {
        // log new high watermark starting
    } else if( phase == resourceTraceDumpPhaseAll ) {
        // log full dump starting
    }
    unsigned goalSize = 0;
    if( impl::memoryTrack() ) {
        for( size_t i=0; i!=resourceTraceTableSize; ++i ) {
            for( ResourceTraceImpl * j=resourceTraces_[ i ]; !!j; j=j->next_ ) {
                if( shouldDump( *j, phase )) {
                    ++goalSize;
                }
            }
        }
        // Given that the above is racy, give some headroom
        goalSize += 10;
    }
    std::vector< ResourceTraceSum, AllocatorAffinity< ResourceTraceSum, Platform >> localStorage;
    std::vector< ResourceTraceSum, AllocatorAffinity< ResourceTraceSum, Platform >> & sum = ( !storage ? localStorage : *storage );
    sum.clear();
    if( sum.capacity() < goalSize ) {
        sum.reserve( goalSize );
    }
    size_t total = 0;
    size_t elided = 0;
    for( size_t i=0; i!=resourceTraceTableSize; ++i ) {
        for( ResourceTraceImpl * j=resourceTraces_[ i ]; !!j; j=j->next_ ) {
            size_t count = j->currAllocated_ * j->interval_;
            size_t size = j->sample_.size_;
            size_t bytes = size * count;
            if( !shouldDump( *j, phase )) {
                // expect to drop most ( > 3/4) of all entries
                elided += bytes;
                continue;
            }
            sum.push_back( ResourceTraceSum( bytes, size, count, j->name_ ));
        }
    }
    // Tracking of the traces is handled elsewhere. As such they don't count towards assertNoAlloc.
    if( !assertNoAlloc ) {
        size_t count = static_cast< uint64 >( ResourceTraceImpl::resourceTraceImplsAllocated );
        size_t size = sizeof( ResourceTraceImpl );
        size_t bytes = size * count;
        static StringId trackingName = StaticStringId( "ResourceTraceImpls used by internal memory tracking" );
        sum.push_back( ResourceTraceSum( bytes, size, count, trackingName ));
    }
    // StringId tracking, which is also ignored when assertNoAlloc
    if( !assertNoAlloc ) {
        uint64 totalIds;
        uint64 totalStaticIds;
        uint64 totalIdBytes;
        impl::stringIdGetMemoryTracking( totalIds, totalStaticIds, totalIdBytes );
        static StringId stringIdTrackingName = StaticStringId( "StringIds" );
        static StringId staticStringIdTrackingName = StaticStringId( "Static StringIds" );
        sum.push_back( ResourceTraceSum( totalIdBytes, totalIdBytes / totalIds, totalIds, stringIdTrackingName ));
        sum.push_back( ResourceTraceSum( totalStaticIds, 1, totalStaticIds, staticStringIdTrackingName ));
    }
    // Tracking of flight-data-recorders. When assertNoAlloc, exactly 1 is allowed to be allocated (for
    // the main thread).
    unsigned fdrBufferCount;
    uint64 fdrPerBufferBytes;
    impl::globalFdr()->memoryTracking( fdrBufferCount, fdrPerBufferBytes );
    if( !assertNoAlloc || ( fdrBufferCount > 1 )) {
        static StringId fdrTrackingName = StaticStringId( "FDR buffers" );
        sum.push_back( ResourceTraceSum( fdrBufferCount * fdrPerBufferBytes, fdrPerBufferBytes, fdrBufferCount, fdrTrackingName ));
    }
    // Stack tracking. Given the stack for the main thread is untracked, we don't need to make special allowances.
    unsigned stackCount = impl::platformStackCount();
    size_t stackBytes = impl::platformStackBytes();
    if( ( stackCount > 0 ) || ( stackBytes > 0 )) {
        static StringId stackTrackingName = StaticStringId( "Thread stacks" );
        sum.push_back( ResourceTraceSum( stackBytes, ( stackCount == 0 ) ? 0 : ( stackBytes / stackCount ), stackCount, stackTrackingName ));
    }
    // This is expensive. It also doesn't really work during shutdown.
    if( !assertNoAlloc ) {
        // TODO: make this also configurable
        impl::logUntrackedMemory();
    }
    std::sort(sum.begin(), sum.end());
    if( elided > 0 ) {
        // TODO: log amount elided
    }
    total = std::accumulate(sum.begin(), sum.end(), total, [](size_t left, ResourceTraceSum const & right)->size_t {
        // TODO: log right->{total, size, count, name}
        return left + right.total_;
    });
    total += elided;
    // This represents an ideal view of tracked memory. That means we don't have a window into fragmentation
    // (internal or external), opportunistic buffering within types, etc.
    // TODO: log total, platformVsize, platformRsize, platformPoolMemory
    if( impl::memoryTrack() ) {
        // TODO: log platformUntractedMemory
    }
    // TODO: log platformSystemMemory, platformSystemLargePages
#ifdef TOOLS_DEBUG
    if( impl::memoryTrack() ) {
        impl::platformMallocStats();
    }
#endif // TOOLS_DEBUG
    if( assertNoAlloc && !sum.empty() ) {
        if( impl::leakProtect() ) {
            // TODO: log 'memory leak'
        } else {
            TOOLS_ASSERTR( !"memory leak" );
        }
    }
    if( phase != resourceTraceDumpPhaseInitial ) {
        // exit frame
    }
}

/////////////////
// NullDisposable
/////////////////

void
NullDisposable::dispose( void )
{
    // Nothing to do, this is static
}
