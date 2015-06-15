#pragma once

#include <tools/MetaBase.h>
#include <tools/Tools.h>

#include <functional>
#ifdef WINDOWS_PLATFORM
#  include <intrin.h>
#  include <nmmintrin.h>
#elif defined(UNIX_PLATFORM)
#  if (defined(__x86_64__) || defined(__i386__))
// GCC compatible, x86/x86-64
#    include <x86intrin.h>
// TODO: this may now be depricated in favor of <immintrin.h>, look into this
#  else
#    error "What the hell are you? No idea where get intrinsics"
#  endif // GCC variants
#else
#  error "Don't know how to load intrinsics for this compiler/arch"
#endif // WINDOWS_PLATFORM

namespace tools {
    template< typename IntT >
    inline bool
    isPow2(
        IntT x )
    {
        return !( x & ( x - IntT( 1 )));
    }

    inline uint64
    roundToPow2(
        uint64 x )
    {
        --x;
        x |= ( x >> 32 );
        x |= ( x >> 16 );
        x |= ( x >> 8 );
        x |= ( x >> 4 );
        x |= ( x >> 2 );
        x |= ( x >> 1 );
        return x + 1;
    }

    inline uint32
    roundToPow2(
        uint32 x )
    {
        --x;
        x |= ( x >> 16 );
        x |= ( x >> 8 );
        x |= ( x >> 4 );
        x |= ( x >> 2 );
        x |= ( x >> 1 );
        return x + 1;
    }

    inline unsigned long long
    roundUpPow2(
        unsigned long long x,
        unsigned long long multiple )
    {
        TOOLS_ASSERT( isPow2( multiple ));
        const unsigned long long mask = ~( multiple - 1 );
        return ( x + multiple - 1 ) & mask;
    }

    inline unsigned long
    roundUpPow2(
        unsigned long x,
        unsigned long multiple )
    {
        TOOLS_ASSERT( isPow2( multiple ));
        const unsigned long mask = ~( multiple - 1 );
        return ( x + multiple - 1 ) & mask;
    }

    inline unsigned
    roundUpPow2(
        unsigned x,
        unsigned multiple )
    {
        TOOLS_ASSERT( isPow2( multiple ));
        const unsigned mask = ~( multiple - 1 );
        return ( x + multiple - 1 ) & mask;
    }

    inline unsigned long long
    roundDownPow2(
        unsigned long long x,
        unsigned long long multiple )
    {
        TOOLS_ASSERT( isPow2( multiple ));
        const unsigned long long mask = ~( multiple - 1 );
        return x & mask;
    }

    inline unsigned long
    roundDownPow2(
        unsigned long x,
        unsigned long multiple )
    {
        TOOLS_ASSERT( isPow2( multiple ));
        const unsigned long mask = ~( multiple - 1 );
        return x & mask;
    }

    inline unsigned
    roundDownPow2(
        unsigned x,
        unsigned multiple )
    {
        TOOLS_ASSERT( isPow2( multiple ));
        const unsigned mask = ~( multiple - 1 );
        return x & mask;
    }

    struct HashAnyInitStorage : SpecifyService< uint32 const > {};

    namespace impl {
#ifdef TOOLS_ARCH_X86
        TOOLS_FORCE_INLINE uint32 hashMix( uint8 v, uint32 initial )
        {
            return _mm_crc32_u8( initial, v );
        }

        TOOLS_FORCE_INLINE uint32 hashMix( sint8 v, uint32 initial )
        {
            return tools::impl::hashMix( static_cast< uint8 >( v ), initial );
        }

        TOOLS_FORCE_INLINE uint32 hashMix( bool v, uint32 initial )
        {
            return tools::impl::hashMix( static_cast< uint8 >( v ), initial );
        }

        TOOLS_FORCE_INLINE uint32 hashMix( uint16 v, uint32 initial )
        {
            return _mm_crc32_u16( initial, v );
        }

        TOOLS_FORCE_INLINE uint32 hashMix( sint16 v, uint32 initial )
        {
            return tools::impl::hashMix( static_cast< uint16 >( v ), initial );
        }

        TOOLS_FORCE_INLINE uint32 hashMix( uint32 v, uint32 initial )
        {
            return _mm_crc32_u32( initial, v );
        }

        TOOLS_FORCE_INLINE uint32 hashMix( sint32 v, uint32 initial )
        {
            return tools::impl::hashMix( static_cast< uint32 >( v ), initial );
        }

        TOOLS_FORCE_INLINE uint32 hashMix( uint64 v, uint32 initial )
        {
            return static_cast< uint32 >( _mm_crc32_u64( initial, v ));
        }

        TOOLS_FORCE_INLINE uint32 hashMix( sint64 v, uint32 initial )
        {
            return tools::impl::hashMix( static_cast< uint64 >( v ), initial );
        }
#endif // TOOLS_ARCH_X86
#ifdef TOOLS_ARCH_UNKNOWN
        struct HashState
        {
        protected:
            uint32 BobHashRot( uint32 x, uint8 k ) {
                return ( x << k ) | ( x >> ( 32 - k ));
            }
            void mix( void ) {
                uint32 a = premix_[ 0 ];
                uint32 b = premix_[ 1 ];
                uint32 c = premix_[ 2 ];
                a -= c;  a ^= BobHashRot( c, 4 );  c += b;
                b -= a;  b ^= BobHashRot( a, 6 );  a += c;
                c -= b;  c ^= BobHashRot( b, 8 );  b += a;
                a -= c;  a ^= BobHashRot( c, 16 );  c += b;
                b -= a;  b ^= BobHashRot( a, 19 );  a += c;
                c -= b;  c ^= BobHashRot( b, 4 );  b += a;
                premix_[ 0 ] = a;
                premix_[ 1 ] = b;
                premix_[ 2 ] = c;
            }
        public:
            HashState( void ) : offset_( 0 ) {
                premix_[ 0 ] = 0;  premix_[ 1 ] = 0;  premix_[ 2 ] = 0;
            }
            HashState( uint32 a, uint32 b, uint32 c ) : offset_( 0 ) {
                premix_[ 0 ] = a;  premix_[ 1 ] = b;  premix_[ 2 ] = c;
            }
            void accumulateUint32( uint32 a ) {
                TOOLS_ASSERT( offset_ < 3 );
                switch( offset_ ) {
                case 0:
                    premix_[ 0 ] += a;  offset_ = 1;
                    break;
                case 1:
                    premix_[ 1 ] += a;  offset_ = 2;
                    break;
                case 2:
                    premix_[ 2 ] += a;  mix();  offset_ = 0;
                    break;
                }
            }
            void accumulate2( uint32 a, uint32 b ) {
                TOOLS_ASSERT( offset_ < 3 );
                switch( offset_ ) {
                case 0:
                    premix_[ 0 ] += a;  premix_[ 1 ] += b;  offset_ = 2;
                    break;
                case 1:
                    premix_[ 1 ] += a;  premix_[ 2 ] += b; mix();  offset_ = 0;
                    break;
                case 2:
                    premix_[ 2 ] += a; mix(); premix_[ 0 ] += b;  offset_ = 1;
                    break;
                }
            }

            operator uint32( void ) {
                uint32 a = premix_[ 0 ];
                uint32 b = premix_[ 1 ];
                uint32 c = premix_[ 2 ];
                c ^= b;
                c -= BobHashRot( b, 14 );
                a ^= c;
                a -= BobHashRot( c, 11 );
                b ^= a;
                b -= BobHashRot( a, 25 );
                c ^= b;
                c -= BobHashRot( b, 16 );
                a ^= c;
                a -= BobHashRot( c, 4 );
                b ^= a;
                b -= BobHashRot( a, 14 );
                c ^= b;
                return static_cast< uint32 >( c - BobHashRot( b, 24 ));
            }

            uint32 premix_[3];
            uint32 offset_;
        };

        TOOLS_FORCE_INLINE uint32 hashMix( uint8 v, uint32 initial )
        {
            HashState h;
            h.accumulateUint32( initial );
            h.accumulateUint32( static_cast< uint32 >( v ) << 12 );
            return static_cast< uint32 >( h );
        }

        TOOLS_FORCE_INLINE uint32 hashMix( sint8 v, uint32 initial )
        {
            return tools::impl::hashMix( static_cast< uint8 >( v ), initial );
        }

        TOOLS_FORCE_INLINE uint32 hashMix( bool v, uint32 initial )
        {
            return tools::impl::hashMix( static_cast< uint8 >( v ), initial );
        }

        TOOLS_FORCE_INLINE uint32 hashMix( uint16 v, uint32 initial )
        {
            HashState h;
            h.accumulateUint32( initial );
            h.accumulateUint32( static_cast< uint32 >( v ) << 8 );
            return static_cast< uint32 >( h );
        }

        TOOLS_FORCE_INLINE uint32 hashMix( sint16 v, uint32 initial )
        {
            return tools::impl::hashMix( static_cast< uint16 >( v ), initial );
        }

        TOOLS_FORCE_INLINE uint32 hashMix( uint32 v, uint32 initial )
        {
            HashState h;
            h.accumulateUint32( initial );
            h.accumulateUint32( v );
            return static_cast< uint32 >( h );
        }

        TOOLS_FORCE_INLINE uint32 hashMix( sint32 v, uint32 initial )
        {
            return tools::impl::hashMix( static_cast< uint32 >( v ), initial );
        }

        TOOLS_FORCE_INLINE uint32 hashMix( uint64 v, uint32 initial )
        {
            HashState h;
            h.accumulateUint32( initial );
            h.accumulate2( static_cast< uint32 >( v & 0xFFFFFFFF ), static_cast< uint32 >( v >> 32 ) );
            return static_cast< uint32 >( h );
        }

        TOOLS_FORCE_INLINE uint32 hashMix( sint64 v, uint32 initial )
        {
            return tools::impl::hashMix( static_cast< uint64 >( v ), initial );
        }
#endif // TOOLS_ARCH_UNKNOWN

        // Make your own defineHashAnyInit if you need to have custom initialization for your type
        template< typename AnyT >
        TOOLS_FORCE_INLINE uint32 defineHashAnyInit( AnyT *** )
        {
            // I use typeid(...).name() rather than NameOf because I don't want to involve StringId here.
            return tools::impl::hashMix( static_cast< uint64 >( reinterpret_cast< ptrdiff_t >( typeid( AnyT ).name() )), static_cast< uint32 >( 0x6CA99934U ));
        }

        // Some constant defaults for common types to save some time/CPU
        TOOLS_FORCE_INLINE uint32 defineHashAnyInit( uint64 *** )
        {
            return static_cast< uint32 >( 0xE6FAEA19U );
        }

        TOOLS_FORCE_INLINE uint32 defineHashAnyInit( sint64 *** )
        {
            return static_cast< uint32 >( 0x37167F14U );
        }

        TOOLS_FORCE_INLINE uint32 defineHashAnyInit( uint32 *** )
        {
            return static_cast< uint32 >( 0xF86EB4E7U );
        }

        TOOLS_FORCE_INLINE uint32 defineHashAnyInit( sint32 *** )
        {
            return static_cast< uint32 >( 0xE57A202EU );
        }

        template< typename AnyT >
        TOOLS_FORCE_INLINE uint32 dispatchHashAnyInit( AnyT *** mangled )
        {
            return defineHashAnyInit( mangled );
        }
    };  // impl namespace

    template< typename AnyT >
    TOOLS_FORCE_INLINE uint32 const * staticServiceCacheInit( HashAnyInitStorage ***, AnyT *** mangled )
    {
        // The following two lines are only thread safe when separate. Do _NOT_ combine them.
        static uint32 initialValue;
        initialValue = tools::impl::dispatchHashAnyInit( mangled );
        return &initialValue;
    }

    // Default initial value for hashing, this is a hash of the normalized typename
    template< typename AnyT >
    TOOLS_FORCE_INLINE uint32 hashAnyInit( AnyT *** = 0 )
    {
        // Decay the type to remove CV qualifiers and references.
        return *staticServiceCacheFetch< HashAnyInitStorage, typename std::decay< AnyT >::type >();
    }

    namespace impl {
        // Compute a hash for any value. See notes below about defineHashAny for how to extend this
        // mechanism.
        template< typename AnyT >
        TOOLS_FORCE_INLINE uint32 hashAny( AnyT const &, uint32 );

        template< typename AnyT >
        TOOLS_FORCE_INLINE uint32 hashAny( AnyT const & a )
        {
            return tools::impl::hashAny( a, tools::hashAnyInit< AnyT >() );
        }

        TOOLS_FORCE_INLINE uint32 defineHashAny( bool v, uint32 initial )
        {
            return hashMix( v, initial );
        }

        TOOLS_FORCE_INLINE uint32 defineHashAny( uint8 v, uint32 initial )
        {
            return hashMix( v, initial );
        }

        TOOLS_FORCE_INLINE uint32 defineHashAny( sint8 v, uint32 initial )
        {
            return hashMix( v, initial );
        }

        TOOLS_FORCE_INLINE uint32 defineHashAny( uint16 v, uint32 initial )
        {
            return hashMix( v, initial );
        }

        TOOLS_FORCE_INLINE uint32 defineHashAny( sint16 v, uint32 initial )
        {
            return hashMix( v, initial );
        }

        TOOLS_FORCE_INLINE uint32 defineHashAny( uint32 v, uint32 initial )
        {
            return hashMix( v, initial );
        }

        TOOLS_FORCE_INLINE uint32 defineHashAny( sint32 v, uint32 initial )
        {
            return hashMix( v, initial );
        }

        TOOLS_FORCE_INLINE uint32 defineHashAny( uint64 v, uint32 initial )
        {
            return hashMix( v, initial );
        }

        TOOLS_FORCE_INLINE uint32 defineHashAny( sint64 v, uint32 initial )
        {
            return hashMix( v, initial );
        }

        template< typename AnyT >
        TOOLS_FORCE_INLINE uint32 defineHashAny( AnyT * v, uint32 initial )
        {
            return tools::impl::hashAny( static_cast< uint64 >( reinterpret_cast< ptrdiff_t >( v )), initial );
        }

        // This indirection is for ADL shenanigans
        template< typename AnyT >
        TOOLS_FORCE_INLINE uint32 dispatchHashAny( AnyT const & a, uint32 initial )
        {
            // ADL to the correct implementation
            return defineHashAny( a, initial );
        }

        template< typename AnyT >
        TOOLS_FORCE_INLINE uint32 hashAny( AnyT const & a, uint32 initial )
        {
            return tools::impl::dispatchHashAny( a, initial );
        }

        struct HashAccum
        {
            TOOLS_FORCE_INLINE HashAccum( uint32 initial )
                : current_( initial )
            {}

            TOOLS_FORCE_INLINE operator uint32( void ) const
            {
                return current_;
            }

            template< typename AnyT >
            HashAccum const & operator%( AnyT const & mix ) const
            {
                current_ = hashAny( mix, current_ );
                return *this;
            }

            template< typename AnyT >
            HashAccum const & operator%=( AnyT const & mix ) const
            {
                current_ = hashAny( mix, current_ );
                return *this;
            }

            uint32 mutable current_;
        };
    };  // impl namespace

    // This is the preferred template for hashing, especially for things like unordered_set and
    // unordered_map.  When possible, this should try to use native CRC (or other CPU hashing
    // function in hardware) whenever possible. Never, _ever_ use std::hash<...>. Some implementations
    // return identity for integer types (including pointers), including GCC at least through 4.5.2.
    //
    // This is extended by defining the 'defineHashAny' function for your type, and use the overloaded
    // modulo (%) operator to mix fields into the hash.  For example:
    //   TOOLS_FORCE_INLINE uint32 defineHashAny( MyType const & t, uint32 initial )
    //   {
    //       return tools::impl::hashAnyBegin( t, initial ) % t.field1 % t.field2 % t.field3;
    //   }
    //
    // Remember, you need to define 'defineHashAny' in the same namespace as your custom type so that
    // ADL finds it correctly.
    template< typename AnyT >
    struct HashAnyOf
        : std::unary_function< AnyT const &, uint32 >
    {
        TOOLS_FORCE_INLINE uint32 operator()( AnyT const & a ) const
        {
            return tools::impl::hashAny( a );
        }
    };

    // Helper for defineHashAny implementations.
    template< typename AnyT >
    TOOLS_FORCE_INLINE tools::impl::HashAccum hashAnyBegin( AnyT const &, uint32 initial = hashAnyInit< AnyT >() )
    {
        return tools::impl::HashAccum( initial );
    }
}; // namespace tools
