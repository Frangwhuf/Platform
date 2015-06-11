#pragma once

#include <tools/Interface.h>
#include <tools/Meta.h>
#include <tools/Tools.h>

#include <mmintrin.h>

namespace tools {
    namespace impl {
        TOOLS_API bool leakProtect( void );
    };  // impl namespace
    namespace detail {
        TOOLS_API uint64 physicalMemory( void );
        static TOOLS_FORCE_INLINE void prefetch(void * ptr) {
            _mm_prefetch(static_cast<char const *>(ptr), _MM_HINT_T0);
        }
        static TOOLS_FORCE_INLINE void unfetch(void * ptr) {
            _mm_prefetch(static_cast<char const *>(ptr), _MM_HINT_NTA);
        }
        TOOLS_API bool memoryValidate(void);
    };  // detail namespace

    ///
    // This interface accepts memory for unmapping, never to be used again.  This only
    // accepts memory from the same object (map is defined in a derived interface).
    struct Unmapper
    {
        // This memory is no longer in use.
        virtual void unmap( void * ) = 0;
    };

    ///
    // A pool returns fixed sized blocks with uniform characteristics.  It represents
    // both a specific source of memory (native machine facility) and the dimensions
    // for retreiving that memory.  Usually the system will provide blocks in only a
    // single size and library facilities can then be used to subdivide them into
    // application desireable sizes.
    //
    // Unlike common memory allocation functions, map does not accept a ResourceSample
    // but does report the ultimate source of memory in Pool::Desc (including the sample
    // where the pool itself was allocation).
    struct Pool
        : Unmapper
    {
        struct Desc
        {
            // Memory dimensions (all calculated).
            size_t size_;
            size_t align_;
            size_t phase_;
            impl::ResourceTrace * trace_;
        };

        // Return the attributes that may be expected from a map return.
        virtual Desc const & describe( void ) = 0;
        // Create a mapping.  This cannot fail and the memory must be passed back
        // into unmap before destroying the pool (in the case of system pools, that
        // would be before program termination).  Baring specific implementation
        // documentation, the contents of map data is undefined.
        virtual void * map( void ) = 0;
    };

    ///
    // A heap returns variable sized block of memory, and unlike a regular memory
    // heap, may not be destroyable and is more often used when there are loose
    // boundaries around allocations.  The heap can operate in a fixed dispatch
    // mode (via map/unmap pairs) or dynamic dispatch (via alloc/free) where free
    // is a global function that isn't required to know the specific heap that
    // gave the allocation.  Dynamic dispatch is most compatible as a drop-in for
    // malloc/free (or new/delete), or to implement trivial cases where the heap
    // pointer would have required storage anyway.
    //
    // Although a heap may be directly created, they are most often obtained from
    // an affinity that establishes a combination of heaps and pools.
    struct Heap
        : Unmapper
    {
        // Map a variable size of memory at a specific alignment.
        virtual void * map( size_t, impl::ResourceSample const &, size_t = 0 ) = 0;
        void *
        map(
            size_t size,
            size_t phase = 0 )
        {
            return map( size, TOOLS_RESOURCE_SAMPLE_CALLER( size ), phase );
        }
        // alloc cannot specify alignment because we're tweaking the size.
        void *
        alloc(
            size_t size,
            impl::ResourceSample const & sample,
            size_t phase = 0 )
        {
            Heap ** heapHead = reinterpret_cast< Heap ** >( map( size + sizeof( Heap *[1] ),
                sample, phase + sizeof( Heap *[1] )));
            *heapHead = this;
            return heapHead + 1;
        }
        void *
        alloc(
            size_t size,
            size_t phase = 0 )
        {
            return alloc( size, TOOLS_RESOURCE_SAMPLE_CALLER( size ), phase );
        }
        // Release some memory, regardless of which heap allocated it.
        static void
        free(
            void * site )
        {
            Heap ** heapHead = reinterpret_cast< Heap ** >( site ) - 1;
            (*heapHead)->unmap( heapHead );
        }
    };

    ///
    // An affinity represents a relationship between a set of maps and unmaps, independent
    // of the physical characteristics.  This is to allow isolation of resources in order
    // to reduce fragmentation and synchronization.
    //
    // The affinity returns Heaps and Pools for accessing memory of differing dimensions.
    //
    // Additionally, an affinity may be subclassed into local versions.  This is most
    // usefull for intensive memory operations, or to complement application level
    // synchronization.  Most derived implementations minimally perform buffering to
    // remove high frequency map/unmap cycles; and/or isolate a busy process from other
    // parts of the application.
    struct Affinity
        : Heap
    {
        // Return an affinity that is bound to the calling thread and has as few
        // indirections as possible.  Allocations and forks created from the bind
        // will exist until the thread is terminated.  After which, accessing the
        // affinity in any way, other then describing an existing Pool, is an error.
        //
        // Maps drawn from the bound affinity must be returned to the same Pool or
        // Heap within the context fo the same thread before the thread is terminated.
        // Failing to do so will cause memory to be leaked.
        //
        // If a fork occurs from a bin, it is considered a single-threaded affinity;
        // but is allowed to float among threads (when only access from one at a time).
        //
        // Bind might just return the same object, and multiple calls to bind will have
        // no further effect.
        virtual Affinity & bind( void ) = 0;
        // Create a derived affinity that contains many features of the parent, but
        // operates in its own temporal domain.  The Heap and Pools should be considered
        // completely unique.
        virtual AutoDispose<> fork( Affinity **, impl::ResourceSample const & ) = 0;
        AutoDispose<>
        fork(
            Affinity ** parent )
        {
            return std::move( fork( parent, TOOLS_RESOURCE_SAMPLE_CALLER( 0 )));
        }
        // Retrieve a Pool that maps blocks of at least the size specified.  Calling
        // describe afterwards will report the actual size.
        virtual Pool & pool( size_t, impl::ResourceSample const &, size_t = 0 ) = 0;
        Pool &
        pool(
            size_t size,
            size_t phase = 0 )
        {
            return pool( size, TOOLS_RESOURCE_SAMPLE_CALLER( size ), phase );
        }
    };

    ///
    // Top-level allocation affinities.
    struct Platform {};  // Effectively platform new/delete/alloc/free
    struct PlatformUntracked {};  // Like it sounds.
    struct Inherent {};  // Standard non-temporal allocatoin.  That is, objects that
                         // have a longer (or unpredictable) lifetime.
    struct Monotonic {};
    struct Temporal {};  // Allocations that have similar (short) lifetime.  This is
                         // probably the most common affinity, followed by Inherent.

    TOOLS_API Affinity * staticServiceCacheInit( Affinity ***, Platform *** );
    TOOLS_API Affinity * staticServiceCacheInit( Affinity ***, PlatformUntracked *** );
    TOOLS_API Affinity * staticServiceCacheInit( Affinity ***, Inherent *** );
    TOOLS_API Affinity * staticServiceCacheInit( Affinity ***, Monotonic *** );
    TOOLS_API Affinity * staticServiceCacheInit( Affinity ***, Temporal *** );

    namespace impl {
        template< typename AffinityT >
        inline Affinity & affinityInstance( AffinityT *** = 0 ) {
            return *tools::staticServiceCacheFetch< Affinity, AffinityT >();
        }

        template< typename AffinityT >
        inline Affinity & affinityRef( AffinityT *** = 0 ) {
            return tools::staticServiceCacheRef< Affinity, AffinityT >();
        }

        TOOLS_API void memset( void *, int, size_t );
        TOOLS_API void outOfMemoryDie( void );
        TOOLS_API bool regionIsUnmapped( void *, size_t );
        TOOLS_API bool regionIsPartiallyUnmapped( void *, size_t );
        TOOLS_API void * safeMalloc( size_t );
    };  // impl namespace
    TOOLS_API AutoDispose<> poolUniqueAddrVmemNew( Pool **, impl::ResourceSample const &, size_t, size_t, unsigned );
    TOOLS_API Heap & heapHuge( void );

    struct AllocTag {};

    // Bind to a static affinity that cannot be overriden.  This would be used typically
    // for components that control their own lifetime precisely.
    template< typename AffinityT = tools::Inherent >
    struct AllocStatic
        : AllocTag
    {
        void *
        operator new(
            size_t size,
            tools::impl::ResourceSample const & sample )
        {
            return tools::impl::affinityInstance< AffinityT >().map( size, sample );
        }
        void *
        operator new(
            size_t size )
        {
            return operator new( size, TOOLS_RESOURCE_SAMPLE_CALLER( size ));
        }
        void
        operator delete(
            void * site )
        {
            tools::impl::affinityRef< AffinityT >().unmap( site );
        }
        void
        operator delete(
            void * site,
            tools::impl::ResourceSample const & )
        {
            operator delete( site );
        }
    private:
        // Disallowed only because they are annoying. They could be implemented if needed.
        void * operator new[]( size_t );
        void operator delete[]( void * );
    };

    // Bind to a dynamic affinity (with a default).  The caller can pass in a new
    // affinity, and the allocation will occur via the alloc/free cycle instead of
    // map/unmap.
    template< typename AffinityT = tools::Inherent >
    struct AllocDynamic
        : AllocTag
    {
        void *
        operator new(
            size_t size,
            tools::Heap & heap,
            tools::impl::ResourceSample const & sample )
        {
            return heap.alloc( size, sample );
        }
        void *
        operator new(
            size_t size,
            tools::impl::ResourceSample const & sample )
        {
            return operator new( size, tools::impl::affinityInstance< AffinityT >(), sample );
        }
        void *
        operator new(
            size_t size,
            tools::Heap & heap )
        {
            return operator new( size, heap, TOOLS_RESOURCE_SAMPLE_CALLER( size ));
        }
        void *
        operator new(
            size_t size )
        {
            return operator new( size, tools::impl::affinityInstance< AffinityT >(),
                TOOLS_RESOURCE_SAMPLE_CALLER( size ));
        }
        void
        operator delete(
            void * site )
        {
            tools::Heap::free( site );
        }
        void
        operator delete(
            void * site,
            tools::impl::ResourceSample const & )
        {
            operator delete( site );
        }
        void
        operator delete(
            void * site,
            tools::Heap & )
        {
            operator delete( site );
        }
        void
        operator delete(
            void * site,
            tools::Heap &,
            tools::impl::ResourceSample const & )
        {
            operator delete( site );
        }
        static tools::Heap &
        dynamicHeapOf(
            void * site )
        {
            // Similar to what we do in Heap, we find the heap from the pointer.
            tools::Heap ** heapHead = reinterpret_cast< tools::Heap ** >( site ) - 1;
            return *( *heapHead );
        }
    private:
        // Disallowed only because they are annoying. They could be implemented if needed.
        void * operator new[]( size_t );
        void operator delete[]( void * );
    };

    // This is for types that have dynamic extention arrays.  This allocator makes
    // it easier to manage the memory for them, and allocate them of the right size.
    template< typename TailT, typename AffinityT = tools::Inherent >
    struct AllocTail
        : tools::AllocTag
    {
        void *
        operator new(
            size_t size,
            size_t tailElements,
            tools::impl::ResourceSample const & sample )
        {
            return tools::impl::affinityInstance< AffinityT >().map(
                size + ( sizeof( TailT ) * tailElements ), sample );
        }
        void *
        operator new(
            size_t size,
            size_t tailElements = 0 )
        {
            return operator new( size, tailElements, TOOLS_RESOURCE_SAMPLE_CALLER( size ));
        }
        void
        operator delete(
            void * site )
        {
            tools::impl::affinityRef< AffinityT >().unmap( site );
        }
        void
        operator delete(
            void * site,
            size_t )
        {
            operator delete( site );
        }
        void
        operator delete(
            void * site,
            size_t,
            tools::impl::ResourceSample const & )
        {
            operator delete( site );
        }
    private:
        // Disallowed only because they are annoying. They could be implemented if needed.
        void * operator new[]( size_t );
        void operator delete[]( void * );
    };

    template< typename TailT, typename AffinityT = tools::Inherent >
    struct AllocTailDynamic
        : tools::AllocTag
    {
        void *
        operator new(
            size_t size,
            size_t tailElements,
            tools::Heap & heap,
            tools::impl::ResourceSample const & sample )
        {
            size_t sizeAlloc = size + sizeof( TailT[ 1 ]) * tailElements;
            if( ( sizeof( TailT[ 1 ]) % sizeof( void * )) != 0U ) {
                // Make sure the tail will align to word size
                if( ( sizeAlloc % sizeof( void * )) != 0U ) {
                    sizeAlloc += sizeof( void * ) - ( sizeAlloc % sizeof( void * ));
                }
            } else {
                TOOLS_ASSERT( ( sizeAlloc % sizeof( void * )) == 0U );
            }
            return heap.alloc( sizeAlloc, sample );
        }
        void *
        operator new(
            size_t size,
            size_t tailElements,
            tools::impl::ResourceSample const & sample )
        {
            return operator new( size, tailElements, tools::impl::affinityInstance< AffinityT >(), sample );
        }
        void *
        operator new(
            size_t size,
            size_t tailElements,
            tools::Heap & heap )
        {
            return operator new( size, tailElements, heap, TOOLS_RESOURCE_SAMPLE_CALLER( size ));
        }
        void *
        operator new(
            size_t size,
            size_t tailElements = 0 )
        {
            return operator new( size, tailElements, tools::impl::affinityInstance< AffinityT >(),
                TOOLS_RESOURCE_SAMPLE_CALLER( size ));
        }
        void
        operator delete(
            void * site )
        {
            tools::Heap::free( site );
        }
        void
        operator delete(
            void * site,
            size_t )
        {
            operator delete( site );
        }
        void
        operator delete(
            void * site,
            size_t,
            tools::Heap & )
        {
            operator delete( site );
        }
        void
        operator delete(
            void * site,
            size_t,
            tools::impl::ResourceSample const & )
        {
            operator delete( site );
        }
        void
        operator delete(
            void * site,
            size_t,
            tools::Heap &,
            tools::impl::ResourceSample const & )
        {
            operator delete( site );
        }
    private:
        // Disallowed only because they are annoying. They could be implemented if needed.
        void * operator new[]( size_t );
        void operator delete[]( void * );
    };

    // An allocator for types that should never be dynamically allocated on their own. They can be created
    // on the stack, included inside other types, used as value types of STL maps, etc, but they cannot be
    // allocated directly via new().
    class AllocProhibited
        : public AllocTag
    {
        void * operator new( size_t );  // never defined
        void operator delete( void * );  // never defined
    };

    // A null allocator for use when there are multiple base types trying to provide
    // an allocation policy.
    struct AllocNull
        : AllocTag
    {};

    // Pool that should be attached to a specific object type. Somewhat unusually, map() is static, but
    // there are enough template parameters to get a specific pool. Mostly syntactic sugar around
    // affinityInstance< AffinityT >().pool( sizeof( ElementT )), but with better memory tracking given
    // that we use a specific type name in the resource sample.
    namespace detail {
        template< typename ElementT, typename AffinityT >
        struct ElementPool
        {
            static tools::Pool * volatile *
            storage( void )
            {
                static tools::Pool * global;
                return const_cast< tools::Pool * volatile * >( &global );
            }
            static tools::Pool &
            init( void )
            {
                // Return a pool for ElementT sized items from our affinity. Memory tracking and 0xC4/0xD4
                // are present if the affinity uses VerifyAffinity.
                tools::Affinity & aff = tools::impl::affinityInstance< AffinityT >();
                // Useful name for memory tracking
                // TODO: fix infinite loop
                // tools::StringId name = tools::nameOf< ElementPool< ElementT, AffinityT >>();
                tools::Pool * ret = &aff.pool( std::max( sizeof( size_t ), sizeof( ElementT )), impl::ResourceSample( sizeof( ElementT ), /*name.c_str()*/"ElementPool< ElementT, AffinityT >" ));
                tools::atomicCas< tools::Pool *, tools::Pool * >( storage(), nullptr, ret );
                return **storage();
            }
            TOOLS_NO_INLINE static void *
            initMap( void )
            {
                return init().map();
            }
            TOOLS_NO_INLINE static void
            initTouch( void )
            {
                init();
            }
            static void
            touch( void )
            {
                // does it exist?
                if( !*storage() ) {
                    initTouch();
                }
            }
            // Does not assume init() has already been called.
            static void *
            map( void )
            {
                if( tools::Pool * ret = *storage() ) {
                    return ret->map();
                }
                return initMap();
            }
            // Assumes init() has already been called.
            static void *
            mapUncached( void )
            {
                return ( *storage() )->map();
            }
            // Assumes init() has been called, which it should have if map() has been called.
            static void
            unmap( void * site )
            {
                ( *storage() )->unmap( site );
            }
        };
    };  // detail namespace

    // Bind to a static affinity that allocates fixed sized objects of sizeof( ImplementationT ). Can be
    // used for individual 'new' allocations, but not variable sized ones like vectors (which will trigger
    // an assert). Currently, inherent AllocPool has good performance for high volume < 128 byte objects
    // (NodeSmallPool), but has lock problems for larger objects (NodePoolSync).
    template< typename ImplementationT, typename AffinityT = tools::Inherent >
    struct AllocPool
        : tools::AllocTag
    {
        void *
        operator new( size_t size )
        {
            TOOLS_ASSERT( size == sizeof( ImplementationT ));
            return tools::detail::ElementPool< ImplementationT, AffinityT >::map();
        }
        void
        operator delete( void * site )
        {
            tools::detail::ElementPool< ImplementationT, AffinityT >::unmap( site );
        }
    };

    // Usage guide for AllocatorAffinity<..>:
    // Derive an object from AllocStatic<...> to give memory tracking when using new/delete. But that does
    // not do anything for STL collections, which don't use new/delete internally. To get memory tracking
    // for an STL collection, use AllocatorAffinity<...>, which fits into the STL 'allocator' template
    // parameter.
    //
    // Examples:
    //    std::vector< size_t, AllocatorAffinity< size_t >> FooVec;            // Inherent (default)
    //    std::vector< size_t, AllocatorAffinity< size_t, Inherent >> FooVec;  // Inherent
    //    std::vector< size_t, AllocatorAffinity< size_t, Temporal >> FooVec;  // Temporal
    //    std::map< key, value, AllocatorAffinity< std::pair< key const, value >>> FooMap;      // Inherent (default)
    //    std::map< key, value, AllocatorAffinity< std::pair< key const, value >, Inherent >>;  // Inherent
    //    std::map< key, value, AllocatorAffinity< std::pair< key const, value >, Temporal >>;  // Temporal
    //
    // std::vector -
    //    We can't do memory tracking of individual allocations that are larger than 2MB (see VmemPool, etc)
    //    and by default that's how large we allow an STL vector to grow. It is possible to override this
    //    by setting the maxBytes template argument to 0 on Inherent or Platform affinities. However,
    //    memory may not be tracked, and this does not work with Temporal or Monotonic which will still
    //    have the 2MB upper limit.
    //
    // std::map/std::set -
    //    Since map/set allocates individual nodes for its RB-tree, so it never hits the 2MB limit.
    //
    // std::unordered_map/std::unordered_set - 
    //    For very large hash tables, we can run into the 2MB limit as the number of buckets grows to keep
    //    the average load factor down. It may be tempting to cap the number of buckets to ensure we do not
    //    go over the 2MB limit. Remember that these collections are typically structured as a vector of
    //    linked lists. The vector part, which is the hash buckets, is subject to the 2MB limit. However,
    //    remember that limiting the bucket vector in this way can have horrible effects on hash table
    //    performance, turning searches more and more into O(n) linked list walks. Map/set may be a better
    //    choice if you are running up against this.

    // As another safety valve, you can use the Jumbo wrapper to remove the 2MB limit for allocation.
    template< typename AffinityT >
    struct Jumbo
    {};

    namespace impl {
        template< typename AffinityT >
        struct AffinityAllocatorTraits
        {
            // Default is just a hair under 2MB (to account for allocation header, etc).
            typedef AffinityT Type;
            enum : size_t {
                maxBytes = ((2U * 1024U * 1024U) - 4096U),
            };
        };

        template< typename AffinityT >
        struct AffinityAllocatorTraits< tools::Jumbo< AffinityT >>
        {
            typedef AffinityT Type;
            enum : size_t {
                maxBytes = 0U
            };
        };
    };  // impl namespace

    // This is compatable with allocators for STL types.  By default, enforce a maximum
    // allocation size of 2MB.  If you want to disable size checks, set maxBytes to 0.
    template< typename ElementT, typename AffinityTraitsT = tools::Inherent >
    struct AllocatorAffinity
        : std::allocator< ElementT >
    {
        enum : size_t {
            maxBytes = tools::impl::AffinityAllocatorTraits< AffinityTraitsT >::maxBytes
        };
        // Adapters
        typedef typename tools::impl::AffinityAllocatorTraits< AffinityTraitsT >::Type AffinityT;
        typedef tools::detail::ElementPool< ElementT, AffinityT > PoolT;

        // This is stateless, so everything works on nothing.
        typedef std::true_type propagate_on_container_copy_assignment;
        typedef std::true_type propagate_on_container_move_assignment;
        typedef std::true_type propagate_on_container_swap;

        AllocatorAffinity( void )
            : array_( false )
        {
            // Ensure that we have memory.
            PoolT::touch();
        }
        AllocatorAffinity( AllocatorAffinity const & c )
            : array_( c.array_ )
        {
        }
        template< typename OtherElementT >
        AllocatorAffinity( AllocatorAffinity< OtherElementT, AffinityTraitsT > const & )
            : array_( false )
        {
            PoolT::touch();
        }
        template< typename OtherElementT, typename OtherAffinityTraitsT >
        AllocatorAffinity< ElementT, AffinityTraitsT > &
        operator=(
            AllocatorAffinity< OtherElementT, OtherAffinityTraitsT > const & c )
        {
            // No assignment to really speak of
            if( sizeof( OtherElementT ) == sizeof( ElementT )) {
                array_ = c.array_;
            }
            return *this;
        }
        AllocatorAffinity< ElementT, AffinityTraitsT > select_on_container_copy_construction() const
        {
            // return this allocator
            return *this;
        }
        // rebind is a compatability type, so its naming does not follow our conventions.
        template< typename OtherElementT >
        struct rebind
        {
            typedef AllocatorAffinity< OtherElementT, AffinityTraitsT > other;
        };
        typename std::allocator< ElementT >::pointer
        allocate(
            typename std::allocator< ElementT >::size_type count,
            void const * = nullptr )
        {
            TOOLS_ASSERT( count != 0 );
            if( count == 1U ) {
                return static_cast< typename std::allocator< ElementT >::pointer >( PoolT::map() );
            } else if( !array_ ) {
                array_ = true;
            }
            size_t allocSize = sizeof( ElementT ) * count;
            if( ( sizeof( ElementT ) % sizeof( size_t[ 1 ])) != 0U ) {
                // For unaligned sizes, check if the final size is aligned. This should be compiled out
                // for larger sizes. Check twice as the compiler may not correctly figure out a multiply.
                if( ( allocSize & ( sizeof( size_t[ 1 ] ) - 1U )) != 0U ) {
                    // Round up to the next word
                    allocSize = ( allocSize & ~static_cast< size_t >( sizeof( size_t[ 1 ]) - 1U )) + sizeof( size_t[ 1 ]);
                }
            }
            return static_cast< typename std::allocator< ElementT >::pointer >(
                tools::impl::affinityRef< AffinityT >().map( allocSize, tools::impl::ResourceSample( allocSize,
                    tools::nameOf< AllocatorAffinity< ElementT, AffinityTraitsT >>().c_str() )));
        }
        void
        deallocate(
            typename std::allocator< ElementT >::pointer site,
            typename std::allocator< ElementT >::size_type count )
        {
            TOOLS_ASSERT( count != 0 );
            tools::Unmapper * u = *PoolT::storage();
            if( count != 1U ) {
                u = &tools::impl::affinityRef< AffinityT >();
            }
            u->unmap( site );
        }
        typename std::allocator< ElementT >::size_type max_size( void ) const
        {
            if( ( maxBytes != 0 ) && array_ ) {
                return ( maxBytes / sizeof( ElementT ));
            }
            return std::allocator< ElementT >::max_size();
        }

        bool array_;
    };

    namespace detail {
        struct CyclicSlab
        {
            CyclicSlab( void )
                : allocHead_( 0U )
                , allocMax_( 0U )
                , freeCount_( 0U )
            {}
            // Convert between offset (8-byte granularity) and pointers, always relative to a specific slab.
            void * toPtr( uint16 offset )
            {
                void * ret = reinterpret_cast< uint64 * >( this ) + offset;
                TOOLS_ASSERT( toOffset( ret ) == offset );
                return ret;
            }
            uint16 toOffset( void * site )
            {
                TOOLS_ASSERT( site >= reinterpret_cast< void * >( this ));
                TOOLS_ASSERT( site < static_cast< void * >( reinterpret_cast< uint64 * >( this ) + 65535 ));
                TOOLS_ASSERT( ( site == this ) || ( reinterpret_cast< CyclicSlab ** >( site )[ 0 ] == this ));
                return static_cast< uint16 >( reinterpret_cast< uint64 * >( site ) - reinterpret_cast< uint64 * >( this ));
            }
            void * bind( void * site )
            {
                CyclicSlab ** ref = reinterpret_cast< CyclicSlab ** >( site );
                TOOLS_ASSERT( toOffset( site ) > 0U );
                return reinterpret_cast< void * >( ref + 1 );
            }
            // Extract the slab and adjust the pointer to the top of its memory.
            static CyclicSlab * __restrict unbind( void ** __restrict ref )
            {
                CyclicSlab ** head = reinterpret_cast< CyclicSlab ** >( *ref ) - 1;
                *ref = reinterpret_cast< void * >( head );
                TOOLS_ASSERT( ( *head )->toOffset( *ref ) > 0U );
                return *head;
            }
            // When this is unbound memory, peek/poke to update the internal linked list. We use a range
            // in the user area as opposed to the previous item which might be a different cache line,
            // just in case. But we always pass around pointers at the top of the user block. These might
            // be malformed, but that will be alright.
            void * peek( void * site )
            {
                TOOLS_ASSERT( reinterpret_cast< CyclicSlab ** >( site )[ 0 ] == this );
                return reinterpret_cast< void * >( reinterpret_cast< CyclicSlab ** >( site )[ 1 ]);
            }
            void poke( void * site, void * next )
            {
                TOOLS_ASSERT( reinterpret_cast< CyclicSlab ** >( site )[ 0 ] == this );
                reinterpret_cast< CyclicSlab ** >( site )[ 1 ] = reinterpret_cast< CyclicSlab * >( next );
            }
            // This must always succeed
            void * map( void )
            {
                TOOLS_ASSERT( freeCount_ > 0 );
                --freeCount_;
                void * unbound = toPtr( allocHead_ );
                // When the slab is formatted (or recycled), the last entry is setup to point to itself so
                // it's a valid value.
                allocHead_ = toOffset( peek( unbound ));
                // Make sure the whole thing is formatted correctly.
                TOOLS_ASSERT( ( ( freeCount_ == 0 ) && ( allocHead_ == 0 )) || ( ( freeCount_ > 0 ) && ( allocHead_ > 0 )));
                return bind( unbound );
            }
            // The fast allocation path is taken whenever there isn't much thinking required. It's inline,
            // whenever this doesn't succeed we go through a longer (slower, non-inline, non-code-gen)
            // path.
            bool tryMap( void ** ref )
            {
                if( allocHead_ > 0U ) {
                    *ref = map();
                    return true;
                }
                return false;
            }
            // I won't return true if this is the very first unmap, because there are likely to be extra
            // conditions. Assume the caller did unbind.
            bool tryUnmap( void * site )
            {
                TOOLS_ASSERT( toOffset( site ) != 0U );
                if( allocHead_ > 0U ) {
                    TOOLS_ASSERT( freeCount_ > 0U );
                    TOOLS_ASSERT( freeCount_ < allocMax_ );
                    ++freeCount_;
                    // link this in
                    poke( site, toPtr( allocHead_ ));
                    allocHead_ = toOffset( site );
                }
                return false;
            }

            // I'm next in the linked list. This is either a link or a token indicating the size.
            CyclicSlab * next_;
            // We continue after this with ordinary pointers, the allocations are sorted (0 is not
            // available). When we're out of allocations, we unroll and sort the frees. When we go
            // non-empty, allocHead_ = 0U will indicate that we should be skipped in the linked list.
            uint16 allocHead_;  // offset at 8-byte granularity
            // Maximum entries within the slab. Allows us to know when they're complete.
            uint16 allocMax_;
            // Count of free entries. This is used to bubble the next slab to the front of the list.
            uint16 freeCount_;
        };

        struct CyclicSlabBase
        {
            CyclicSlabBase( void )
                : singleton_( nullptr )
                , slabReturns_( nullptr )
            {}

            // If I replace a singleton, it is stored here until it is released, then never used again.
            void * singleton_;
            // Where slabs are freed, they end here. The terminator for this list isn't quite nullptr, it's a
            // token indicating the actual size.
            CyclicSlab * slabReturns_;
        };

        // The smallest entry is a 'root' that contains a couple of additional pointers. This one will stay
        // around for a while. The members are in an alternate base class so the slab is still lined up
        // with the allocations. For the root (and only the root) all allocations are passed into 'next'
        // which may or may not point back to itself.
        struct CyclicSlabRoot
            : CyclicSlabBase
            , CyclicSlab
        {};

        // Storage for the pools that will be setup per-user, but there aren't very many and the only
        // difference is the starting point. Jump up by 4x at a time which should be about the right
        // curve. This is an interface class, see CyclicPoolDescImpl.
        struct CyclicPoolDesc
        {
            // There are 5 pools. Begin at the one that fits 4x the number of elements and move up through
            // the pools, increasing scale by 4x each step.
            enum : size_t {
                slabPoolsMax = 5U,
                cyclicBytesMax = 16384U,
            };

            // Format the descriptor. We'll assume some raciness, this must be called before its first use.
            TOOLS_API void format( size_t );

            // Add in the type name for a little less branching and distinct sizes.
            StringId name_;
            uint32 elementBytes_;
            uint8 poolMin_;  // First pool to use.
            // Pools that we will allocate from for various CyclicSlab sizes. These are memory tracked
            // with type specific slab names. The inner pools are the global type generic unitDescs[].src_.
            Pool * volatile slabPools_[ slabPoolsMax ];
            // If memory tracking is on, we need to dispose a PoolVerify wrapper later on (held here). If
            // not, this is a dummy owned which does nothing.
            AutoDispose<> slabPoolOwners_[ slabPoolsMax ];
        };

        template< typename TypeT >
        inline void staticServiceCacheOnce( CyclicPoolDesc * desc, tools::Reg< CyclicPoolDesc > ***, TypeT *** )
        {
            static_assert( sizeof( TypeT ) < CyclicPoolDesc::cyclicBytesMax, "This element exceeds the maximum supported size (16k)" );
            // This format call must be internally reentrant
            desc->format( sizeof( TypeT ));
        }

        // For use by the various manipulation functions
        typedef tools::AlternatePointer< void, tools::detail::CyclicSlabRoot > CyclicSlabRootPointer;

        // parameters are per container, global, and for memory tracking (in order).
        TOOLS_API void * cyclicSlabPoolMap( CyclicSlabRootPointer * __restrict, CyclicPoolDesc &, StringId const & );
        // root is percontainer, where desc is global
        TOOLS_API void cyclicSlabPoolUnmap( void *, CyclicSlab * __restrict, CyclicSlabRoot * __restrict, CyclicPoolDesc & );
        // If we used any pool content, return it here
        TOOLS_API void cyclicSlabPoolFinalize( CyclicSlabRoot * __restrict, CyclicPoolDesc & );
    };  // detail namespace

    // This helper object uses the cyclic pools for object allocation. The integration is 'choppy', don't
    // use this until that gets better worked out. Both the allocations and frees must be single threaded.
    // Also the object must use AllocCyclic.
    //
    // TODO: memory tracking? Probably via affinityVerifyNew(), or manual ResourceTraces (+inc/dec). Not
    // entirely clear yet where to put the tracking though.
    template< typename ElementT >
    struct AllocCyclicRoot
    {
        static_assert( sizeof( ElementT ) < tools::detail::CyclicPoolDesc::cyclicBytesMax, "This element is too large for Cyclic" );
        typedef tools::detail::CyclicSlab CyclicSlab;
        typedef tools::detail::CyclicSlabRoot CyclicSlabRoot;
        typedef tools::detail::CyclicSlabRootPointer CyclicSlabRootPointer;
        typedef tools::detail::CyclicPoolDesc CyclicPoolDesc;

        AllocCyclicRoot( void ) {
            head_.reset( static_cast< void * >( nullptr ));
            name();
        }
        ~AllocCyclicRoot( void ) {
            if( CyclicSlabRoot * h = head_.other() ) {
                // lots more cleanup
                tools::detail::cyclicSlabPoolFinalize( h, slabPools() );
            }
        }

        void name( void ) {
            if( impl::memTracking() ) {
                name_ = tools::nameOf< AllocCyclicRoot< ElementT >>();
            }
        }
        void * allocate( void ) {
            if( CyclicSlabRoot * h = head_.other() ) {
                // Immediately traverse to next because it may be pointing somewhere much larger and more
                // likely to succeed.
                void * p;
                if( h->next_->tryMap( &p )) {
                    // This is the most likely outcome.
                    return p;
                }
            } else {
                // Make sure this type is faulted in.
                tools::registryFetch< tools::detail::CyclicPoolDesc, ElementT >();
            }
            // We have to take the slow path. This is here because it is less likely than the startup.
            return tools::detail::cyclicSlabPoolMap( &head_, slabPools(), name_ );
        }
        void deallocate( void * site ) {
            TOOLS_ASSERT( !!head_.other() );
            // We might not be freeing to the head. This doesn't matter, the pointer will tell us.
            CyclicSlab * __restrict ptrSlab = CyclicSlab::unbind( &site );
            if( ptrSlab->tryUnmap( site )) {
                return;
            }
            // We have to take the slow path.
            tools::detail::cyclicSlabPoolUnmap( site, ptrSlab, head_.other(), slabPools() );
        }
        static CyclicPoolDesc & slabPools( void ) {
            return tools::registryRef< tools::detail::CyclicPoolDesc, ElementT >();
        }

        // Never use the optimization for the first singleton, so it is mutable only in one direction.
        // However, the 'other' pointer is used to make the API cleaner.
        CyclicSlabRootPointer head_;
        StringId name_;
    };

    // Allocator optimized for node containers (i.e.: list, map, set, unordered_map, unordered_set).
    // Unlike AllocatorAffinity<>, this allocator contains significant state. Internally we will use
    // AllocatorAffinity<> for non-node allocations.
    //
    // Locks are not used, since STL containers must already be lock protected. Further, unlike
    // NodeSmallPool and Temporal, we don't even have all that many thread local accesses. While these
    // aren't all that slow, it is good to avoid them anyway. Also, the simplest allocation paths are
    // inlined in the allocator, so the fast path should be as fast as possible.
    //
    // This allocator exposes some high watermark behavior. If a container only grows, floats around a
    // certain level, or is not prone to one-time spikes; this allocator will give the best performance.
    //
    // The affinity parameter does not affect the source of the nodes themselves. The nodes are allocated
    // from slabs in a Temporal-like manner. The affinity parameter only affects non-node allocations.
    // (E.g.: the bucket vector in unordered collections.)
    //
    // Memory tracking and protection (0xC4/0xD4) note: Tracking is not trivial to implement in this class.
    // A related allocator, AllocatorAffinity<>, does all allocations (pool or heap) from an affinity.
    // This affinity was already wrapped in VerifyAffinity (if applicable), so it has memory tracking and
    // protection. We currently implement cyclic memory tracking in two places. These overlap, so it is
    // double counting, but seeing both numbers is helpful when looking for memory bloat. The two sites
    // where we add memory tracking are:
    //   1) 2MB backing slabs from Inherent.
    //   2) Cyclic slabs that are allocated out of NodeSmallPool pools (which we have in 5 different sizes,
    //      see unitDescs[]), which are themselves backed by the 2MB slabs from 1).
    //
    // We do not currently implement tracking on each allocate(). It is not trivial because there is no
    // affinity nearby. So there is no chance to leverage affinityVerifyNew(). But it can be done manually,
    // giving tracking at the ideal granularity of allocations, as it is done everywhere else.
    //
    // Some AllocatorCyclic allocations (the non-node ones) are passed through to AllocatorAffinity<>, so
    // they get tracked.
    template< typename ElementT, typename AffinityTraitsT = Inherent >
    struct AllocatorCyclic
        : AllocatorAffinity< ElementT, AffinityTraitsT >
    {
        typedef AllocatorAffinity< ElementT, AffinityTraitsT > BaseT;
        typedef AllocatorCyclic< ElementT, AffinityTraitsT > AllocatorThisT;
        typedef tools::detail::CyclicSlab CyclicSlab;
        typedef tools::detail::CyclicSlabRoot CyclicSlabRoot;
        typedef tools::detail::CyclicSlabRootPointer CyclicSlabRootPointer;
        typedef tools::detail::CyclicPoolDesc CyclicPoolDesc;

        // Integration with future allocator_traits implementation, harmless for pre-C++x11

        // These say that even though the allocators are not equal, we containers can move and swap, but
        // not copy.
        typedef std::false_type propagate_on_container_copy_assignment;
        typedef std::true_type propagate_on_container_move_assignment;
        typedef std::true_type propagate_on_container_swap_assignment;

        template< typename OtherElementT >
        struct rebind {
            typedef AllocatorCyclic< OtherElementT, AffinityTraitsT > other;
        };

        AllocatorCyclic( void ) {
            name();
        }
        AllocatorCyclic( AllocatorThisT const & c )
            : BaseT( c )
        {
            c.head_.reset( static_cast< void * >( nullptr ));
            name();
        }
        // Don'treally have any other parameters
        template< typename OtherElementT >
        AllocatorCyclic( AllocatorCyclic< OtherElementT, AffinityTraitsT > const & c )
            : BaseT( c )
        {
            name();
        }
        ~AllocatorCyclic( void ) {
            if( sizeof( ElementT ) < tools::detail::CyclicPoolDesc::cyclicBytesMax ) {
                TOOLS_ASSERT( !head_.default() );
                if( CyclicSlabRoot * h = head_.other() ) {
                    tools::detail::cyclicSlabPoolFinalize( h, slabPools() );
                }
            } else {
                TOOLS_ASSERT( !head_ );
            }
        }

        void name( void ) {
            if( impl::memTracking() ) {
                name_ = tools::nameOf< AllocatorThisT >();
            }
        }

        AllocatorThisT select_on_container_copy_construction( void ) const {
            // In allocators, copy is move and default construct is copy.
            return AllocatorThisT();
        }

        // For allocators, assignment is actually move, copies are first passed through
        // select_on_container_copy_construction. Neat, eh?
        AllocatorThisT & operator=( AllocatorThisT & c ) {
            head_ = c.head_;
            c.head_.reset( static_cast< void * >( nullptr ));
            return *this;
        }

        // A more complex exception case.
        TOOLS_NO_INLINE typename std::allocator< ElementT >::pointer allocate_except( typename std::allocator< ElementT >::size_type count, void const * hint ) {
            if( ( sizeof( ElementT ) < tools::detail::CyclicPoolDesc::cyclicBytesMax ) && ( count == 1U )) {
                if( !head_ ) {
                    // Still warming up. Need at least two concurrent node allocations to activate.
                    auto ptrAlloc = BaseT::allocate( count, hint ); // head_ allocated with AllocatorAffinity<>? Make sure we are consistent when we delete it.
                    head_.reset( static_cast< void * >( ptrAlloc ));
                    return ptrAlloc;
                }
                if( CyclicSlabRoot * h = head_.other() ) {
                    // Make sure we are only here because the allocation failed.
                    TOOLS_ASSERT( !h->next_->tryMap( nullptr ));  // assertion side effects are probably already here.
                }
                // Go to the even more complex path. This is here because it is less likely than the startup.
                return static_cast< typename std::allocator< ElementT >::pointer >( tools::detail::cyclicSlabPoolMap( &head_, *tools::registryFetch< tools::detail::CyclicPoolDesc, ElementT >(), name_ ));
            }
            // Not a node-sized allocation. Just use the base, AllocatorAffinity.
            return BaseT::allocate( count, hint );
        }

        // Regular
        typename std::allocator< ElementT >::pointer allocate( typename std::allocator< ElementT >::size_type count, void const * hint = nullptr ) {
            CyclicSlabRoot * h;
            // for node allocators, this should be statically determined and instanced.
            if( ( sizeof( ElementT ) < tools::detail::CyclicPoolDesc::cyclicBytesMax ) && ( count == 1U ) && head_.as( &h )) {
                // Immediately traverse to next_ in this case as it is likely to be pointing somewhere
                // larger and more likely to succeed.
                void * ptr;
                if( h->next_->tryMap( &ptr )) {
                    // This is the most likely path
                    return static_cast< typename std::allocator< ElementT >::pointer >( ptr );
                }
            }
            // All other cases translate to a function call.
            return allocate_except( count, hint );
        }

        // A more complex exception case.
        TOOLS_NO_INLINE void deallocate_except( typename std::allocator< ElementT >::pointer site, typename std::allocator< ElementT >::size_type count ) {
            if( ( sizeof( ElementT ) < tools::detail::CyclicPoolDesc::cyclicBytesMax ) && ( count == 1U )) {
                if( CyclicSlabRoot * h = head_.other() ) {
                    if( site != h->singleton_ ) {
                        // But we might not be freeing to the head_. This is alright, the pointer will tell
                        // us.
                        void * ptr = site;
                        CyclicSlab * __restrict ptrSlab = CyclicSlab::unbind( &ptr );
                        TOOLS_ASSERT( !ptrSlab->tryUnmap( ptr ));
                        // Go to the even more complex path.
                        tools::detail::cyclicSlabPoolUnmap( ptr, ptrSlab, h, slabPools() );
                        return;
                    } else {
                        h->singleton_ = nullptr;
                    }
                } else {
                    // The only way we should be here is because we are releasing the lone singleton. Reset
                    // that singleton and fall through.
                    TOOLS_ASSERT( head_.default() == site );
                    head_ = static_cast< void * >( nullptr );
                }
            }
            // Not a node-sized deallocation. Use our base AllocatorAffinity<>.
            BaseT::deallocate( ptr, count );
        }

        // Regular path
        void deallocate( typename std::allocator< ElementT >::pointer site, typename std::allocator< ElementT >::size_type count ) {
            // This branch should be statically determined.
            CyclicSlabRoot * h;
            if( ( sizeof( ElementT ) < tools::detail::CyclicPoolDesc::cyclicBytesMax ) && ( count == 1U ) && head_.as( &h ) && ( site != h->singleton_ )) {
                // We might not be freeing to the head_. That is alright, the pointer will tell us.
                void * ptr = site;
                CyclicSlab * __restrict ptrSlab = CyclicSlab::unbind( &ptr );
                if( ptrSlab->tryUnmap( ptr )) {
                    return;
                }
            }
            // All other cases go through the non-inline exception path.
            deallocate_except( site, count );
        }

        static CyclicPoolDesc & slabPools( void ) {
            return tools::registryRef< tools::detail::CyclicPoolDesc, ElementT >();
        }

        // When this is a void type, it is the first node. When it is cyclic slab root, we are committed.
        // For the first singleton, we don't do any special allocation. This allows vector types to pass
        // through transparently if deployment goes wider.
        CyclicSlabRootPointer mutable head_;
        StringId name_;
    };

    // Unconditionally these allocators are never equal, so we can copy.
    template< typename ElementT, typename OtherElementT, typename AffinityT >
    inline bool operator==( AllocatorCyclic< ElementT, AffinityT > const &, AllocatorCyclic< OtherElementT, AffinityT > const & ) {
        return false;
    }
    template< typename ElementT, typename OtherElementT, typename AffinityT >
    inline bool operator!=( AllocatorCyclic< ElementT, AffinityT > const &, AllocatorCyclic< OtherElementT, AffinityT > const & ) {
        return true;
    }
    template< typename ElementT, typename AffinityT >
    inline void swap( AllocatorCyclic< ElementT, AffinityT > & left, AllocatorCyclic< ElementT, AffinityT > & right ) {
        std::swap( left.array_, right.array_ );
        swap( left.head_, right.head_ );
        std::swap( left.name_, right.name_ );
    }
}; // namespace tools
