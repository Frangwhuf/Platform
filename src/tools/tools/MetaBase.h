#pragma once

#include <tools/Tools.h>

#include <typeinfo>
#include <type_traits>

namespace tools {
    template< typename PointerT >
    inline bool isEnd( PointerT * p ) {
        return !p;
    }

    template< typename PointerT >
    inline void setEnd( PointerT ** p ) {
        *p = nullptr;
    }

    namespace detail {
        // Tag is used to identify classes with self-trait overrides
        template< typename TraitT >
        struct Tag
        {
        };

        // Use this as a base class when the answer doesn't depend on the final type
        template< typename TraitT, typename TraitDefT >
        struct TraitsSpecify
            : ::tools::detail::Tag< TraitT >
        {
            template< typename TypeT >
            struct TraitsApply
            {
                typedef TraitDefT Type;
            };
        };

        // Dispatch to find the traits of a type.
        template< typename TraitT, typename TypeT, bool from_base=std::is_base_of< ::tools::detail::Tag< TraitT >, TypeT >::value >
        struct TraitsOf;

        template< typename TraitT, typename TypeT >
        struct TraitsOf< TraitT, TypeT, true >
        {
            // Apply traits from the base class
            typedef typename TypeT::template TraitsApply< TypeT >::Type Type;
        };

        template< typename TraitT, typename TypeT >
        struct TraitsOf< TraitT, TypeT, false >
        {
            // It's off to ADL to see if we can figure this out.
            typedef decltype( define__( static_cast< TraitT *** >( 0 ), static_cast< TypeT *** >( 0 ))) DeclTypeT;
            typedef DeclTypeT Type;
        };

        template< typename TypeT, typename MemberT >
        size_t memberOffsetOf( MemberT TypeT::* memberPtr )
        {
            // TODO: turn this into better casts
            return (size_t)&(((TypeT *)(size_t)1124U)->*memberPtr)-1124U;
        }

        template< typename TypeT, typename MemberT >
        TypeT * itemFromMember( MemberT TypeT::* memberPtr, MemberT & member )
        {
            size_t offset = tools::detail::memberOffsetOf( memberPtr );
            return reinterpret_cast< TypeT * >( reinterpret_cast< uint8_t * >( &member ) - offset );
        }

        struct ServiceTraits
        {};

        template< typename TypeT >
        struct DefaultServiceTraits
        {
            typedef TypeT InterfaceT;
        };

        template< typename ServiceT >
        struct ServiceDefinition
        {
            typedef ServiceT InterfaceT;
        };
    }; // detail namespace

    // Registry services will be wrapped in this template
    template< typename ServiceT >
    struct Reg {};

    template< typename TypeT >
    auto define__( tools::detail::ServiceTraits ***, TypeT *** )
        ->typename tools::detail::DefaultServiceTraits< TypeT >;

    // Service name declaration with optional interface specification
    template< typename ServiceT >
    struct SpecifyService
        : tools::detail::TraitsSpecify< tools::detail::ServiceTraits, tools::detail::ServiceDefinition< ServiceT >>
    {};

    template< typename ServiceT >
    struct ServiceInterfaceOf
    {
        typedef typename tools::detail::TraitsOf< tools::detail::ServiceTraits, ServiceT >::Type::InterfaceT Type;
    };

    namespace detail {
        // Note: It does not make sense for there to be a default implementation for staticServiceCacheInit(),
        // as such, there isn't one. staticServiceCacheOnce can have an ADL overload if you want to do some
        // work when a new cache instance is created.
        template< typename ServiceT, typename TypeT >
        inline void staticServiceCacheOnce( typename tools::ServiceInterfaceOf< ServiceT >::Type *, ServiceT ***, TypeT *** )
        {}

        template< typename ServiceT, typename TypeT >
        struct StaticServiceCache
        {
            typedef typename tools::ServiceInterfaceOf< ServiceT >::Type InterfaceT;

            static InterfaceT ** storage( void ) throw() {
                static InterfaceT * storage_;
                return &storage_;
            }

            // _NOT_ inlined, so that we can establish a firewall.
            TOOLS_NO_INLINE static InterfaceT * init( void ) throw() {
                InterfaceT * __restrict new_ = staticServiceCacheInit( static_cast< ServiceT *** >( nullptr ), static_cast< TypeT *** >( nullptr ));
                if( !!new_ ) {
                    staticServiceCacheOnce( new_, static_cast< ServiceT *** >( nullptr ), static_cast< TypeT *** >( nullptr ));
                }
                *storage() = new_;
                return new_;
            }

            static InterfaceT * __restrict get( void ) {
                InterfaceT * ret = *storage();
                if( TOOLS_LIKELY( !!ret )) {
                    return ret;
                }
                return init();
            }
        };
    };  // detail namespace

    template< typename ServiceT, typename TypeT >
    inline typename tools::ServiceInterfaceOf< ServiceT >::Type * __restrict staticServiceCacheFetch( ServiceT *** = 0, TypeT *** = 0 ) throw() {
        typename tools::ServiceInterfaceOf< ServiceT >::Type * ret = *tools::detail::StaticServiceCache< ServiceT, TypeT >::storage();
        if( TOOLS_LIKELY( !!ret )) {
            return ret;
        }
        return tools::detail::StaticServiceCache< ServiceT, TypeT >::init();
    }

    template< typename ServiceT, typename TypeT >
    inline typename tools::ServiceInterfaceOf< ServiceT >::Type & staticServiceCacheRef( ServiceT *** = 0, TypeT *** = 0 ) throw() {
        typename tools::ServiceInterfaceOf< ServiceT >::Type * ret = *tools::detail::StaticServiceCache< ServiceT, TypeT >::storage();
        TOOLS_ASSERT( !!ret );
        return *ret;
    }

    // Type sequence container
    template< size_t i >
    struct SequenceIndex
    {
        static size_t const value = i;
    };

    template< typename ForwardSequenceT >
    struct SequenceSize;

    namespace detail {
        struct Nil_ {};

        // Sequence possition
        template< typename SequenceT, size_t i >
        struct SequenceIter;

        // Iterator terminator
        struct SequenceIterEnd {};

        // Calculate the 'next' from the iterator, by default we just advance the position by one.
        template< typename SequenceT, size_t i >
        struct SequenceNextOrEnd
        {
            typedef typename std::conditional< i == tools::SequenceSize< SequenceT >::Value, SequenceIterEnd, SequenceIter< SequenceT, i >>::Type Type;
        };

        template< typename SequenceT >
        struct SequenceIter< SequenceT, 0U >
            : tools::SequenceIndex< 0U >
        {
            typedef typename SequenceT::T0 Type;
            typedef typename SequenceNextOrEnd< SequenceT, 1U >::Type Next;
        };

        template< typename SequenceT >
        struct SequenceIter< SequenceT, 1U >
            : tools::SequenceIndex< 1U >
        {
            typedef typename SequenceT::T1 Type;
            typedef typename SequenceNextOrEnd< SequenceT, 2U >::Type Next;
        };

        template< typename SequenceT >
        struct SequenceIter< SequenceT, 2U >
            : tools::SequenceIndex< 2U >
        {
            typedef typename SequenceT::T2 Type;
            typedef typename SequenceNextOrEnd< SequenceT, 3U >::Type Next;
        };

        template< typename SequenceT >
        struct SequenceIter< SequenceT, 3U >
            : tools::SequenceIndex< 3U >
        {
            typedef typename SequenceT::T3 Type;
            typedef typename SequenceNextOrEnd< SequenceT, 4U >::Type Next;
        };

        template< typename SequenceT >
        struct SequenceIter< SequenceT, 4U >
            : tools::SequenceIndex< 4U >
        {
            typedef typename SequenceT::T4 Type;
            typedef typename SequenceNextOrEnd< SequenceT, 5U >::Type Next;
        };

        template< typename SequenceT >
        struct SequenceIter< SequenceT, 5U >
            : tools::SequenceIndex< 5U >
        {
            typedef typename SequenceT::T5 Type;
            typedef typename SequenceNextOrEnd< SequenceT, 6U >::Type Next;
        };

        template< typename SequenceT >
        struct SequenceIter< SequenceT, 6U >
            : tools::SequenceIndex< 6U >
        {
            typedef typename SequenceT::T6 Type;
            typedef typename SequenceNextOrEnd< SequenceT, 7U >::Type Next;
        };

        template< typename SequenceT >
        struct SequenceIter< SequenceT, 7U >
            : tools::SequenceIndex< 7U >
        {
            typedef typename SequenceT::T7 Type;
            typedef typename SequenceNextOrEnd< SequenceT, 8U >::Type Next;
        };

        template< typename SequenceT >
        struct SequenceIter< SequenceT, 8U >
            : tools::SequenceIndex< 8U >
        {
            typedef typename SequenceT::T8 Type;
            typedef typename SequenceNextOrEnd< SequenceT, 9U >::Type Next;
        };

        template< typename SequenceT >
        struct SequenceIter< SequenceT, 9U >
            : tools::SequenceIndex< 9U >
        {
            typedef typename SequenceT::T9 Type;
            typedef typename SequenceNextOrEnd< SequenceT, 10U >::Type Next;
        };

        template< typename SequenceT >
        struct SequenceIter< SequenceT, 10U >
            : tools::SequenceIndex< 10U >
        {
            typedef typename SequenceT::T10 Type;
            typedef typename SequenceNextOrEnd< SequenceT, 11U >::Type Next;
        };

        template< typename SequenceT >
        struct SequenceIter< SequenceT, 11U >
            : tools::SequenceIndex< 11U >
        {
            typedef typename SequenceT::T11 Type;
            typedef typename SequenceNextOrEnd< SequenceT, 12U >::Type Next;
        };

        template< typename SequenceT >
        struct SequenceIter< SequenceT, 12U >
            : tools::SequenceIndex< 12U >
        {
            typedef typename SequenceT::T12 Type;
            typedef typename SequenceNextOrEnd< SequenceT, 13U >::Type Next;
        };

        template< typename SequenceT >
        struct SequenceIter< SequenceT, 13U >
            : tools::SequenceIndex< 13U >
        {
            typedef typename SequenceT::T13 Type;
            typedef typename SequenceNextOrEnd< SequenceT, 14U >::Type Next;
        };

        template< typename SequenceT >
        struct SequenceIter< SequenceT, 14U >
            : tools::SequenceIndex< 14U >
        {
            typedef typename SequenceT::T14 Type;
            typedef typename SequenceNextOrEnd< SequenceT, 15U >::Type Next;
        };

        template< typename SequenceT >
        struct SequenceIter< SequenceT, 15U >
            : tools::SequenceIndex< 15U >
        {
            typedef typename SequenceT::T15 Type;
            typedef typename SequenceNextOrEnd< SequenceT, 16U >::Type Next;
        };

        template< typename SequenceT >
        struct SequenceIter< SequenceT, 16U >
            : tools::SequenceIndex< 16U >
        {
            typedef typename SequenceT::T16 Type;
            typedef SequenceIterEnd Next;
        };
    };  // detail namespace

    template< typename T_0 = tools::detail::Nil_, typename T_1 = tools::detail::Nil_,
        typename T_2 = tools::detail::Nil_, typename T_3 = tools::detail::Nil_,
        typename T_4 = tools::detail::Nil_, typename T_5 = tools::detail::Nil_,
        typename T_6 = tools::detail::Nil_, typename T_7 = tools::detail::Nil_,
        typename T_8 = tools::detail::Nil_, typename T_9 = tools::detail::Nil_,
        typename T_10 = tools::detail::Nil_, typename T_11 = tools::detail::Nil_,
        typename T_12 = tools::detail::Nil_, typename T_13 = tools::detail::Nil_,
        typename T_14 = tools::detail::Nil_, typename T_15 = tools::detail::Nil_,
        typename T_16 = tools::detail::Nil_ >
    struct Seq
    {
        static size_t const size_ = 17U;
        typedef T_0 T0;
        typedef T_1 T1;
        typedef T_2 T2;
        typedef T_3 T3;
        typedef T_4 T4;
        typedef T_5 T5;
        typedef T_6 T6;
        typedef T_7 T7;
        typedef T_8 T8;
        typedef T_9 T9;
        typedef T_10 T10;
        typedef T_11 T11;
        typedef T_12 T12;
        typedef T_13 T13;
        typedef T_14 T14;
        typedef T_15 T15;
        typedef T_16 T16;
    };

    template<>
    struct Seq< tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_>
    {
        static size_t const size_ = 0U;

        template< typename TypeT >
        struct DoPushBack
        {
            typedef tools::Seq< TypeT > Type;
        };
    };

    typedef tools::Seq<> SequenceEmpty;

    template< typename T_0 >
    struct Seq< T_0, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_>
    {
        static size_t const size_ = 1U;

        typedef T_0 T0;

        template< typename TypeT >
        struct DoPushBack
        {
            typedef tools::Seq< T_0, TypeT > Type;
        };
    };

    template< typename T_0, typename T_1 >
    struct Seq< T_0, T_1,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_>
    {
        static size_t const size_ = 2U;

        typedef T_0 T0;
        typedef T_1 T1;

        template< typename TypeT >
        struct DoPushBack
        {
            typedef tools::Seq< T_0, T_1, TypeT > Type;
        };
    };

    template< typename T_0, typename T_1, typename T_2 >
    struct Seq< T_0, T_1, T_2, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_>
    {
        static size_t const size_ = 3U;

        typedef T_0 T0;
        typedef T_1 T1;
        typedef T_2 T2;

        template< typename TypeT >
        struct DoPushBack
        {
            typedef tools::Seq< T_0, T_1, T_2, TypeT > Type;
        };
    };

    template< typename T_0, typename T_1, typename T_2, typename T_3 >
    struct Seq< T_0, T_1, T_2, T_3,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_>
    {
        static size_t const size_ = 4U;

        typedef T_0 T0;
        typedef T_1 T1;
        typedef T_2 T2;
        typedef T_3 T3;

        template< typename TypeT >
        struct DoPushBack
        {
            typedef tools::Seq< T_0, T_1, T_2, T_3,
                TypeT > Type;
        };
    };

    template< typename T_0, typename T_1, typename T_2, typename T_3,
        typename T_4 >
    struct Seq< T_0, T_1, T_2, T_3,
        T_4, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_>
    {
        static size_t const size_ = 5U;

        typedef T_0 T0;
        typedef T_1 T1;
        typedef T_2 T2;
        typedef T_3 T3;
        typedef T_4 T4;

        template< typename TypeT >
        struct DoPushBack
        {
            typedef tools::Seq< T_0, T_1, T_2, T_3,
                T_4, TypeT > Type;
        };
    };

    template< typename T_0, typename T_1, typename T_2, typename T_3,
        typename T_4, typename T_5 >
    struct Seq< T_0, T_1, T_2, T_3,
        T_4, T_5, tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_>
    {
        static size_t const size_ = 6U;

        typedef T_0 T0;
        typedef T_1 T1;
        typedef T_2 T2;
        typedef T_3 T3;
        typedef T_4 T4;
        typedef T_5 T5;

        template< typename TypeT >
        struct DoPushBack
        {
            typedef tools::Seq< T_0, T_1, T_2, T_3,
                T_4, T_5, TypeT > Type;
        };
    };

    template< typename T_0, typename T_1, typename T_2, typename T_3,
        typename T_4, typename T_5, typename T_6, typename T_7 >
    struct Seq< T_0, T_1, T_2, T_3,
        T_4, T_5, T_6, T_7,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_>
    {
        static size_t const size_ = 8U;

        typedef T_0 T0;
        typedef T_1 T1;
        typedef T_2 T2;
        typedef T_3 T3;
        typedef T_4 T4;
        typedef T_5 T5;
        typedef T_6 T6;
        typedef T_7 T7;

        template< typename TypeT >
        struct DoPushBack
        {
            typedef tools::Seq< T_0, T_1, T_2, T_3,
                T_4, T_5, T_6, T_7,
                TypeT > Type;
        };
    };

    template< typename T_0, typename T_1, typename T_2, typename T_3,
        typename T_4, typename T_5, typename T_6, typename T_7,
        typename T_8 >
    struct Seq< T_0, T_1, T_2, T_3,
        T_4, T_5, T_6, T_7,
        T_8, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_>
    {
        static size_t const size_ = 9U;

        typedef T_0 T0;
        typedef T_1 T1;
        typedef T_2 T2;
        typedef T_3 T3;
        typedef T_4 T4;
        typedef T_5 T5;
        typedef T_6 T6;
        typedef T_7 T7;
        typedef T_8 T8;

        template< typename TypeT >
        struct DoPushBack
        {
            typedef tools::Seq< T_0, T_1, T_2, T_3,
                T_4, T_5, T_6, T_7,
                T_8, TypeT > Type;
        };
    };

    template< typename T_0, typename T_1, typename T_2, typename T_3,
        typename T_4, typename T_5, typename T_6, typename T_7,
        typename T_8, typename T_9 >
    struct Seq< T_0, T_1, T_2, T_3,
        T_4, T_5, T_6, T_7,
        T_8, T_9, tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_>
    {
        static size_t const size_ = 10U;

        typedef T_0 T0;
        typedef T_1 T1;
        typedef T_2 T2;
        typedef T_3 T3;
        typedef T_4 T4;
        typedef T_5 T5;
        typedef T_6 T6;
        typedef T_7 T7;
        typedef T_8 T8;
        typedef T_9 T9;

        template< typename TypeT >
        struct DoPushBack
        {
            typedef tools::Seq< T_0, T_1, T_2, T_3,
                T_4, T_5, T_6, T_7,
                T_8, T_9, TypeT > Type;
        };
    };

    template< typename T_0, typename T_1, typename T_2, typename T_3,
        typename T_4, typename T_5, typename T_6, typename T_7,
        typename T_8, typename T_9, typename T_10 >
    struct Seq< T_0, T_1, T_2, T_3,
        T_4, T_5, T_6, T_7,
        T_8, T_9, T_10, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_>
    {
        static size_t const size_ = 11U;

        typedef T_0 T0;
        typedef T_1 T1;
        typedef T_2 T2;
        typedef T_3 T3;
        typedef T_4 T4;
        typedef T_5 T5;
        typedef T_6 T6;
        typedef T_7 T7;
        typedef T_8 T8;
        typedef T_9 T9;
        typedef T_10 T10;

        template< typename TypeT >
        struct DoPushBack
        {
            typedef tools::Seq< T_0, T_1, T_2, T_3,
                T_4, T_5, T_6, T_7,
                T_8, T_9, T_10, TypeT > Type;
        };
    };

    template< typename T_0, typename T_1, typename T_2, typename T_3,
        typename T_4, typename T_5, typename T_6, typename T_7,
        typename T_8, typename T_9, typename T_10, typename T_11 >
    struct Seq< T_0, T_1, T_2, T_3,
        T_4, T_5, T_6, T_7,
        T_8, T_9, T_10, T_11,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_>
    {
        static size_t const size_ = 12U;

        typedef T_0 T0;
        typedef T_1 T1;
        typedef T_2 T2;
        typedef T_3 T3;
        typedef T_4 T4;
        typedef T_5 T5;
        typedef T_6 T6;
        typedef T_7 T7;
        typedef T_8 T8;
        typedef T_9 T9;
        typedef T_10 T10;
        typedef T_11 T11;

        template< typename TypeT >
        struct DoPushBack
        {
            typedef tools::Seq< T_0, T_1, T_2, T_3,
                T_4, T_5, T_6, T_7,
                T_8, T_9, T_10, T_11,
                TypeT > Type;
        };
    };

    template< typename T_0, typename T_1, typename T_2, typename T_3,
        typename T_4, typename T_5, typename T_6, typename T_7,
        typename T_8, typename T_9, typename T_10, typename T_11,
        typename T_12 >
    struct Seq< T_0, T_1, T_2, T_3,
        T_4, T_5, T_6, T_7,
        T_8, T_9, T_10, T_11,
        T_12, tools::detail::Nil_,
        tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_>
    {
        static size_t const size_ = 13U;

        typedef T_0 T0;
        typedef T_1 T1;
        typedef T_2 T2;
        typedef T_3 T3;
        typedef T_4 T4;
        typedef T_5 T5;
        typedef T_6 T6;
        typedef T_7 T7;
        typedef T_8 T8;
        typedef T_9 T9;
        typedef T_10 T10;
        typedef T_11 T11;
        typedef T_12 T12;

        template< typename TypeT >
        struct DoPushBack
        {
            typedef tools::Seq< T_0, T_1, T_2, T_3,
                T_4, T_5, T_6, T_7,
                T_8, T_9, T_10, T_11,
                T_12, TypeT > Type;
        };
    };

    template< typename T_0, typename T_1, typename T_2, typename T_3,
        typename T_4, typename T_5, typename T_6, typename T_7,
        typename T_8, typename T_9, typename T_10, typename T_11,
        typename T_12, typename T_13 >
    struct Seq< T_0, T_1, T_2, T_3,
        T_4, T_5, T_6, T_7,
        T_8, T_9, T_10, T_11,
        T_12, T_13, tools::detail::Nil_, tools::detail::Nil_,
        tools::detail::Nil_>
    {
        static size_t const size_ = 14U;

        typedef T_0 T0;
        typedef T_1 T1;
        typedef T_2 T2;
        typedef T_3 T3;
        typedef T_4 T4;
        typedef T_5 T5;
        typedef T_6 T6;
        typedef T_7 T7;
        typedef T_8 T8;
        typedef T_9 T9;
        typedef T_10 T10;
        typedef T_11 T11;
        typedef T_12 T12;
        typedef T_13 T13;

        template< typename TypeT >
        struct DoPushBack
        {
            typedef tools::Seq< T_0, T_1, T_2, T_3,
                T_4, T_5, T_6, T_7,
                T_8, T_9, T_10, T_11,
                T_12, T_13, TypeT > Type;
        };
    };

    template< typename T_0, typename T_1, typename T_2, typename T_3,
        typename T_4, typename T_5, typename T_6, typename T_7,
        typename T_8, typename T_9, typename T_10, typename T_11,
        typename T_12, typename T_13, typename T_14 >
    struct Seq< T_0, T_1, T_2, T_3,
        T_4, T_5, T_6, T_7,
        T_8, T_9, T_10, T_11,
        T_12, T_13, T_14, tools::detail::Nil_,
        tools::detail::Nil_>
    {
        static size_t const size_ = 15U;

        typedef T_0 T0;
        typedef T_1 T1;
        typedef T_2 T2;
        typedef T_3 T3;
        typedef T_4 T4;
        typedef T_5 T5;
        typedef T_6 T6;
        typedef T_7 T7;
        typedef T_8 T8;
        typedef T_9 T9;
        typedef T_10 T10;
        typedef T_11 T11;
        typedef T_12 T12;
        typedef T_13 T13;
        typedef T_14 T14;

        template< typename TypeT >
        struct DoPushBack
        {
            typedef tools::Seq< T_0, T_1, T_2, T_3,
                T_4, T_5, T_6, T_7,
                T_8, T_9, T_10, T_11,
                T_12, T_13, T_14, TypeT > Type;
        };
    };

    template< typename T_0, typename T_1, typename T_2, typename T_3,
        typename T_4, typename T_5, typename T_6, typename T_7,
        typename T_8, typename T_9, typename T_10, typename T_11,
        typename T_12, typename T_13, typename T_14, typename T_15 >
    struct Seq< T_0, T_1, T_2, T_3,
        T_4, T_5, T_6, T_7,
        T_8, T_9, T_10, T_11,
        T_12, T_13, T_14, T_15,
        tools::detail::Nil_>
    {
        static size_t const size_ = 16U;

        typedef T_0 T0;
        typedef T_1 T1;
        typedef T_2 T2;
        typedef T_3 T3;
        typedef T_4 T4;
        typedef T_5 T5;
        typedef T_6 T6;
        typedef T_7 T7;
        typedef T_8 T8;
        typedef T_9 T9;
        typedef T_10 T10;
        typedef T_11 T11;
        typedef T_12 T12;
        typedef T_13 T13;
        typedef T_14 T14;
        typedef T_15 T15;

        template< typename TypeT >
        struct DoPushBack
        {
            typedef tools::Seq< T_0, T_1, T_2, T_3,
                T_4, T_5, T_6, T_7,
                T_8, T_9, T_10, T_11,
                T_12, T_13, T_14, T_15,
                TypeT > Type;
        };
    };

    template< typename ForwardSequenceT >
    struct SequenceBegin;

    template< typename T_0, typename T_1, typename T_2, typename T_3,
        typename T_4, typename T_5, typename T_6, typename T_7,
        typename T_8, typename T_9, typename T_10, typename T_11,
        typename T_12, typename T_13, typename T_14, typename T_15,
        typename T_16 >
    struct SequenceBegin< tools::Seq< T_0, T_1, T_2, T_3, T_4, T_5, T_6, T_7, T_8, T_9, T_10, T_11, T_12, T_13, T_14, T_15, T_16 >>
    {
        typedef tools::detail::SequenceIter< tools::Seq< T_0, T_1, T_2, T_3, T_4, T_5, T_6, T_7, T_8, T_9, T_10, T_11, T_12, T_13, T_14, T_15, T_16 >, 0U > Type;
    };

    template< typename TypeT >
    struct SequenceBegin
    {
        typedef typename tools::SequenceBegin< tools::Seq< TypeT >>::Type Type;
    };

    template<>
    struct SequenceBegin< SequenceEmpty >
    {
        typedef tools::detail::SequenceIterEnd Type;
    };

    template< typename TypeT >
    struct SequenceEnd
    {
        typedef tools::detail::SequenceIterEnd Type;
    };

    template< typename ForwardSequenceT, size_t i >
    struct SequenceAt;

    template< typename T_0, typename T_1, typename T_2, typename T_3,
        typename T_4, typename T_5, typename T_6, typename T_7,
        typename T_8, typename T_9, typename T_10, typename T_11,
        typename T_12, typename T_13, typename T_14, typename T_15,
        typename T_16, size_t i >
    struct SequenceAt< tools::Seq< T_0, T_1, T_2, T_3, T_4, T_5, T_6, T_7, T_8, T_9, T_10, T_11, T_12, T_13, T_14, T_15, T_16 >, i >
    {
        typedef typename tools::detail::SequenceIter< tools::Seq< T_0, T_1, T_2, T_3, T_4, T_5, T_6, T_7, T_8, T_9, T_10, T_11, T_12, T_13, T_14, T_15, T_16 >, i >::Type Type;
    };

    template< typename MutableSequenceT, typename TypeT >
    struct SequencePushBack;

    template< typename T_0, typename T_1, typename T_2, typename T_3,
        typename T_4, typename T_5, typename T_6, typename T_7,
        typename T_8, typename T_9, typename T_10, typename T_11,
        typename T_12, typename T_13, typename T_14, typename T_15,
        typename T_16, typename TypeT >
    struct SequencePushBack< tools::Seq< T_0, T_1, T_2, T_3, T_4, T_5, T_6, T_7, T_8, T_9, T_10, T_11, T_12, T_13, T_14, T_15, T_16 >, TypeT >
    {
        typedef typename  tools::Seq< T_0, T_1, T_2, T_3, T_4, T_5, T_6, T_7, T_8, T_9, T_10, T_11, T_12, T_13, T_14, T_15, T_16 >::template DoPushBack< TypeT >::Type Type;
    };

    template< typename T_0, typename T_1, typename T_2, typename T_3,
        typename T_4, typename T_5, typename T_6, typename T_7,
        typename T_8, typename T_9, typename T_10, typename T_11,
        typename T_12, typename T_13, typename T_14, typename T_15,
        typename T_16 >
    struct SequenceSize< tools::Seq< T_0, T_1, T_2, T_3, T_4, T_5, T_6, T_7, T_8, T_9, T_10, T_11, T_12, T_13, T_14, T_15, T_16 >>
    {
        static size_t const value = tools::Seq< T_0, T_1, T_2, T_3, T_4, T_5, T_6, T_7, T_8, T_9, T_10, T_11, T_12, T_13, T_14, T_15, T_16 >::size_;
    };

    template< typename SequenceT, typename FunctorT >
    struct Accumulate
    {
        template< typename IteratorT, typename ValueT >
        struct ApplyIter;

        template< typename ValueT >
        struct ApplyIter< typename tools::SequenceEnd< SequenceT >::Type, ValueT >
        {
            typedef ValueT Type;
        };

        template< typename IteratorT, typename ValueT >
        struct ApplyIter
        {
            typedef typename ApplyIter< typename IteratorT::Next, typename FunctorT::template apply< ValueT, typename IteratorT::Type >::Type >::Type Type;
        };

        typedef typename ApplyIter< typename tools::SequenceBegin< SequenceT >::Type::Next, typename tools::SequenceBegin< SequenceT >::Type::Type >::Type Type;
    };

    template< typename FunctorT >
    struct Accumulate< tools::SequenceEmpty, FunctorT >
    {
        typedef tools::detail::Nil_ Type;
    };

    template< typename SequenceOrTypeT >
    struct EvalTypes
    {
        typedef typename tools::SequenceEnd< SequenceOrTypeT >::Type IterEndT;

        template< typename VisitorT >
        TOOLS_FORCE_INLINE static void next( VisitorT * __restrict v, IterEndT *** ) {
            // terminator
        }

        template< typename VisitorT, typename IteratorT >
        TOOLS_FORCE_INLINE static void next( VisitorT * __restrict v, IteratorT *** ) {
            ( *v )( IteratorT::Value, static_cast< typename IteratorT::Type *** >( 0 ));
            next( v, static_cast< IteratorT::Next *** >( 0 ));
        }

        template< typename VisitorT >
        TOOLS_FORCE_INLINE static void bind( VisitorT & v ) {
            next( &v, static_cast< typename tools::SequenceBegin< SequenceOrTypeT >::Type *** >( 0 ));
        }
    };
}; // namespace tools
