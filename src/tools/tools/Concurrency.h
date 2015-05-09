#pragma once

#include <tools/Interface.h>
#include <tools/InterfaceTools.h>
#include <tools/Tools.h>

#ifdef WINDOWS_PLATFORM
#  include <intrin.h>
#  pragma intrinsic(_InterlockedCompareExchangePointer)
#  pragma intrinsic(_InterlockedExchangeAdd64)
#endif // WINDOWS_PLATFORM

#include <boost/utility.hpp>

namespace tools {
    namespace impl {
        // Compute an upper bound power of 2
        template< size_t N, size_t power, bool isLess >
        struct AtomicRoundPower;

        template< size_t N, size_t pow >
        struct AtomicRoundPower< N, pow, false >
        {
            static const size_t power_ = tools::impl::AtomicRoundPower< N, ( 2 * pow ), ( N <= ( 2 * pow )) >::power_;
        };

        template< size_t N, size_t pow >
        struct AtomicRoundPower< N, pow, true >
        {
            static const size_t power_ = pow;
        };

        template< size_t N >
        struct AtomicPowerOf2
        {
            static const size_t power_ = tools::impl::AtomicRoundPower< N, 1, ( N <= 1 ) >::power_;
        };

        template< int width >
        struct AtomicTrait
        {
            template< typename TypeT >
            struct Read
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT * const )
                {
                    static_assert( sizeof( TypeT ) == 0, "Unsupported width or architecture for atomic read" );
                    return static_cast< TypeT const >( 0 );
                }
            };

            template< typename TypeT >
            struct Set
            {
                TOOLS_FORCE_INLINE static void op( TypeT volatile * const, TypeT const )
                {
                    static_assert( sizeof( TypeT ) == 0, "Unsupported width or architecture for atomic set" );
                }
            };

            // Returns the old value
            template< typename TypeT >
            struct Cas
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const, TypeT const, TypeT const )
                {
                    static_assert( sizeof( TypeT ) == 0, "Unsupported width or architecture for atomic CAS" );
                    return static_cast< TypeT const >( 0 );
                }
            };

            // Returns the old value
            template< typename TypeT >
            struct Exchange
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const, TypeT const )
                {
                    static_assert( sizeof( TypeT ) == 0, "Unsupported width or architecture for atomic exchange" );
                    return static_cast< TypeT const >( 0 );
                }
            };

            // Returns the old value
            template< typename TypeT >
            struct Add
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const, TypeT const )
                {
                    static_assert( sizeof( TypeT ) == 0, "Unsupported width or architecture for atomic add" );
                    return static_cast< TypeT const >( 0 );
                }
            };

            // Returns the old value
            template< typename TypeT >
            struct Subtract
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const, TypeT const )
                {
                    static_assert( sizeof( TypeT ) == 0, "Unsupported width or architecture for atomic subtract" );
                    return static_cast< TypeT const >( 0 );
                }
            };

            // Return true if result is not '0'
            template< typename TypeT >
            struct Increment
            {
                TOOLS_FORCE_INLINE static bool const op( TypeT volatile * const )
                {
                    static_assert( sizeof( TypeT ) == 0, "Unsupported width or architecture for atomic increment" );
                    return false;
                }
            };

            // Return true if result is not '0'
            template< typename TypeT >
            struct Decrement
            {
                TOOLS_FORCE_INLINE static bool const op( TypeT volatile * const )
                {
                    static_assert( sizeof( TypeT ) == 0, "Unsupported width or architecture for atomic decrement" );
                    return false;
                }
            };

            // Returns the old value
            template< typename TypeT >
            struct And
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const, TypeT const )
                {
                    static_assert( sizeof( TypeT ) == 0, "Unsupported width or architecture for atomic AND" );
                    return static_cast< TypeT const >( 0 );
                }
            };

            // Returns the old value
            template< typename TypeT >
            struct Or
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const, TypeT const )
                {
                    static_assert( sizeof( TypeT ) == 0, "Unsupported width or architecture for atomic OR" );
                    return static_cast< TypeT const >( 0 );
                }
            };

            template< typename TypeT >
            struct Swap
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT const )
                {
                    static_assert( sizeof( TypeT ) == 0, "Unsupported width or architecture for byte swap" );
                    return static_cast< TypeT const >( 0 );
                }
            };

            template< typename TypeT, typename StorageT >
            struct GenericRead
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT * const site )
                {
                    static_assert( sizeof( TypeT ) <= sizeof( StorageT ), "AtomicTrait for type is too narrow" );
                    // Most of this is to make the compiler silent, and generate exactly the one instruction we
                    // want. The individual steps are:
                    //
                    // 1) remove CV-qual, an in-compiler only operation
                    // 2) reinterpret as the storage type, an in-compiler only operation
                    // 3) mark it volatile, so the compiler doesn't think it can use an already read copy
                    // 4) de-reference the pointer, this is the only thing that generates code
                    // 5) cast result back to non-CV-qual 'user' type, an in-compiler only operation
                    //
                    // In non-release builds the compiler generates a couple extra moves for reasons that
                    // are unclear. While this is non-ideal, non-release build performance is always questionable.
                    // It probably should not be surprising the compiler does extra 'stuff' in those modes.
                    return static_cast< typename std::remove_cv< TypeT >::type const >(          // 5
                        *                                                                        // 4
                        const_cast< StorageT const volatile * const >(                           // 3
                        reinterpret_cast< StorageT * const >(                                    // 2
                        const_cast< typename std::remove_cv< TypeT >::type * const >( site )))); // 1
                }
            };

            template< typename TypeT, typename StorageT >
            struct GenericSet
            {
                TOOLS_FORCE_INLINE static void op( TypeT volatile * const site, TypeT const newValue )
                {
                    static_assert( sizeof( TypeT ) <= sizeof( StorageT ), "AtomicTrait for type is too narrow" );
                    // Most of this is to make the compiler silent, and generate exactly the one instruction we
                    // want. The individual steps are:
                    //
                    // (for the left-hand side)
                    // 1) remove CV-qual, an in-compiler only operation
                    // 2) reinterpret as the storage type, an in-compiler only operation
                    // 3) mark it volatile, so the compiler thinks it must write the value when set
                    // 4) de-reference the pointer, in conjunction with 6 this is the only thing that generates code
                    // (for the right-hand side)
                    // 5) convert the new value to the storage type, an in-compiler only operation
                    // (operator)
                    // 6) do the assignment, in conjunction with 4 this is the only thing that generates code
                    //
                    // In non-release builds the compiler generates a couple extra moves for reasons that
                    // are unclear. While this is non-ideal, non-release build performance is always questionable.
                    // It probably should not be surprising the compiler does extra 'stuff' in those modes.
                    *                                                                          // 4
                        const_cast< StorageT volatile * const >(                               // 3
                        reinterpret_cast< StorageT * const >(                                  // 2
                        const_cast< typename std::remove_cv< TypeT >::type * const >( site ))) // 1
                        =                                                                      // 6
                        static_cast< StorageT const >( newValue );                             // 5
                }
            };

#ifdef __GNUC__
            template< typename TypeT, typename StorageT >
            struct GenericCas
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const oldValue, TypeT const newValue )
                {
                    static_assert( sizeof( TypeT ) <= sizeof( StorageT ), "AtomicTrait for type  is too narrow" );
                    return static_cast< TypeT const >( __sync_val_compare_and_swap(
                        reinterpret_cast< StorageT volatile * const >( site ),
                        static_cast< StorageT const >( oldValue ),
                        static_cast< StorageT const >( newValue )));
                }
            };

            template< typename TypeT, typename StorageT >
            struct GenericCas< TypeT *, StorageT >
            {
                TOOLS_FORCE_INLINE static TypeT * const op( TypeT * volatile * const site, TypeT * const oldValue, TypeT * const newValue )
                {
                    static_assert( sizeof( TypeT * ) <= sizeof( StorageT ), "AtomicTrait for type is too narrow" );
                    return reinterpret_cast< TypeT * const >( __sync_val_compare_and_swap(
                        reinterpret_cast< void * volatile * const >( site ),
                        reinterpret_cast< void * const >( oldValue ),
                        reinterpret_cast< void * const >( newValue )));
                }
            };

            template< typename TypeT, typename StorageT >
            struct GenericExchange
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const newValue )
                {
                    static_assert( sizeof( TypeT ) <= sizeof( StorageT ), "AtomicTrait for type is too narrow" );
                    return static_cast< TypeT const >( __sync_lock_test_and_set(
                        reinterpret_cast< StorageT volatile * const >( site ),
                        static_cast< StorageT const >( newValue )));
                }
            };

            template< typename TypeT, typename StorageT >
            struct GenericExchange
            {
                TOOLS_FORCE_INLINE static TypeT * const op( TypeT * volatile * const site, TypeT * const newValue )
                {
                    static_assert( sizeof( TypeT * ) <= sizeof( StorageT ), "AtomicTrait for type is too narrow" );
                    return reinterpret_cast< TypeT * const >( __sync_locK_test_and_set(
                        reinterpret_cast< void * volatile * const >( site ),
                        reinterpret_cast< void * const >( newValue )));
                }
            };

            template< typename TypeT, typename StorageT >
            struct GenericExchange< TypeT *, StorageT >
            {
                TOOLS_FORCE_INLINE static TypeT * const op( TypeT * volatile * const site, TypeT * const newValue )
                {
                    static_assert( sizeof( TypeT * ) <= sizeof( StorageT ), "AtomicTrait for type is too narrow" );
                    return reinterpret_cast< Typet * const >( __sync_lock_test_and_set(
                        reinterpret_cast< void * volatile * const >( site ),
                        reinterpret_cast< void * const >( newValue )));
                }
            };

            template< typename TypeT, typename StorageT >
            struct GenericAdd
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const diff )
                {
                    static_assert( sizeof( TypeT ) <= sizeof( StorageT ), "AtomicTrait for type is too narrow" );
                    return static_cast< TypeT const >( __sync_fetch_and_add(
                        reinterpret_cast< StorageT volatile * const >( site ),
                        static_cast< StorageT const >( diff )));
                }
            };

            template< typename TypeT, typename StorageT >
            struct GenericSubtract
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const diff )
                {
                    static_assert( sizeof( TypeT ) <= sizeof( StorageT ), "AtomicTrait for type is too narrow" );
                    return static_cast< TypeT const >( __sync_fetch_and_sub(
                        reinterpret_cast< StorageT volatile * const >( site ),
                        static_cast< StorageT const >( diff )));
                }
            };

            template< typename TypeT, typename StorageT >
            struct GenericIncrement
            {
                TOOLS_FORCE_INLINE static bool const op( TypeT volatile * const site )
                {
                    static_assert( sizeof( TypeT ) <= sizeof( StorageT ), "AtomicTrait for type is too narrow" );
                    return !! __sync_add_and_fetch(
                        reinterpret_cast< StorageT volatile * const >( site ),
                        static_cast< StorageT const >( 1U ));
                }
            };

            template< typename TypeT, typename StorageT >
            struct GenericDecrement
            {
                TOOLS_FORCE_INLINE static bool const op( TypeT volatile * const site )
                {
                    static_assert( sizeof( TypeT ) <= sizeof( StorageT ), "AtomicTrait for type is too narrow" );
                    return !! __sync_sub_and_fetch(
                        reinterpret_cast< StorageT volatile * const>( site ),
                        static_cast< StorageT const >( 1U ));
                }
            };

            template< typename TypeT, typename StorageT >
            struct GenericAnd
            {
                TOOLS_FORCE_INCLUDE static StorageT const op( TypeT volatile * const site, TypeT const mask )
                {
                    static_assert( sizeof( TypeT ) <= sizeof( StorageT ), "AtomicTrait for type is too narrow" );
                    return __sync_fetch_and_and(
                        reinterpret_cast< StorageT volatile * const >( site ),
                        static_cast< StorageT const >( mask ));
                }
            };

            template< typename TypeT, typename StorageT >
            struct GenericOr
            {
                TOOLS_FORCE_INCLUDE static StorageT const op( TypeT volatile * const site, TypeT const mask )
                {
                    static_assert( sizeof( TypeT ) <= sizeof( StorageT ), "AtomicTrait for type is too narrow" );
                    return __sync_fetch_and_or(
                        reinterpret_cast< StorageT volatile * const >( site ),
                        static_cast< StorageT const >( mask ));
                }
            };
#endif // __GNUC __
        };

        template<>
        struct AtomicTrait< 1 >
        {
            typedef uint8 Storage;
            static unsigned const width_ = 1U;

            template< typename TypeT >
            struct Read
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT * const site )
                {
                    static_assert( sizeof( TypeT ) == 1, "AtomicTrait for type is too narrow" );
                    // Values smaller than int are atomic reads, so long as they don't cross a cache line.
                    // The the best of my understanding a single byte cannot cross a cache line. Nor can I
                    // figure out how a single byte can get torn.
                    return ::tools::impl::AtomicTrait< 0 >::template GenericRead< TypeT, Storage >::op( site );
                }
            };

            template< typename TypeT >
            struct Set
            {
                TOOLS_FORCE_INLINE static void op( TypeT volatile * const site, TypeT const newValue )
                {
                    static_assert( sizeof( TypeT ) == 1, "AtomicTrait for type is too narrow" );
                    // On x86, normal memory writes are always exposed in program order, provided the compiler
                    // emits the following as a single instruction.
                    ::tools::impl::AtomicTrait< 0 >::template GenericSet< TypeT, Storage >::op( site, newValue );
                }
            };

            // Returns the old value
            template< typename TypeT >
            struct Cas
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const oldValue, TypeT newValue )
                {
                    static_assert( sizeof( TypeT ) == 1, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return static_cast< TypeT const >( _InterlockedCompareExchange8(
                        reinterpret_cast< char volatile * const >( site ),
                        static_cast< char const >( newValue ),
                        static_cast< char const >( oldValue )));
#elif defined(__GNUC__)
                    return ::tools::impl::AtomicTrait< 0 >::template GenericCas< TypeT, Storage >::op( site, oldValue, newValue );
#endif // WINDOWS_PLATFORM
                }
            };

#if defined(WINDOWS_PLATFORM)
            template< typename TypeT >
            TOOLS_FORCE_INLINE static Storage const innerExchange( TypeT volatile * const site, TypeT const newValue )
            {
                static_assert( sizeof( TypeT ) == 1, "AtomicTrait for type is too narrow" );
                return static_cast< Storage const >( _InterlockedExchange8(
                    reinterpret_cast< char volatile * const >( site ),
                    static_cast< char const >( newValue )));
            }

            // Returns the old value
            template< typename TypeT >
            struct Exchange
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const newValue )
                {
                    return static_cast< TypeT const >( innerExchange< TypeT >( site, newValue ));
                }
            };

            // Returns the old value
            template<>
            struct Exchange< bool >
            {
                TOOLS_FORCE_INLINE static bool const op( bool volatile * const site, bool const newValue )
                {
                    return !! innerExchange< bool >( site, newValue );
                }
            };
#elif defined(__GNUC__)
            template< typename TypeT >
            struct Exchange
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const newValue )
                {
                    static_assert( sizeof( TypeT ) == 1, "AtomicTrait for type is too narrow" );
                    return ::tools::impl::AtomicTrait< 0 >::template GenericExchange< TypeT, Storage >::op( site, newValue );
                }
            };
#endif // WINDOWS_PLATFORM

            // Returns the old value
            template< typename TypeT >
            struct Add
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const diff )
                {
                    static_assert( sizeof( TypeT ) == 1, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return static_cast< TypeT const >( _InterlockedExchangeAdd8(
                        reinterpret_cast< char volatile * const >( site ),
                        static_cast< char const >( diff )));
#elif defined(__GNUC__)
                    return ::tools::impl::AtomicTrait< 0 >::template GenericAdd< TypeT, Storage >::op( site, diff );
#endif // WINDOWS_PLATFORM
                }
            };

            // Returns the old value
            template< typename TypeT >
            struct Subtract
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const diff )
                {
                    static_assert( sizeof( TypeT ) == 1, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return static_cast< TypeT const >( _InterlockedExchangeAdd8(
                        reinterpret_cast< char volatile * const >( site ),
                        static_cast< char const >( diff ) * -1));
#elif defined(__GNUC__)
                    return::tools::impl::AtomicTrait< 0 >::template GenericSubtract< TypeT, Storage >::op( site, diff );
#endif // WINDOWS_PLATFORM
                }
            };

            // Return true if result is not '0'
            template< typename TypeT >
            struct Increment
            {
                TOOLS_FORCE_INLINE static bool const op( TypeT volatile * const site )
                {
                    static_assert( sizeof( TypeT ) == 1, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return ( _InterlockedExchangeAdd8(
                        reinterpret_cast< char volatile * const >( site ),
                        static_cast< char const >( 1 )) != static_cast< char const >( -1 ));
#elif defined(__GNUC__)
                    return ::tools::impl::AtomicTrait< 0 >::template GenericIncrement< TypeT, Storage >::op( site );
#endif // WINDOWS_PLATFORM
                }
            };

            // Return true if result is not '0'
            template< typename TypeT >
            struct Decrement
            {
                TOOLS_FORCE_INLINE static bool const op( TypeT volatile * const site )
                {
                    static_assert( sizeof( TypeT ) == 1, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return ( _InterlockedExchangeAdd8(
                        reinterpret_cast< char volatile * const >( site ),
                        static_cast< char const >( -1 )) != 0x01 );
#elif defined(__GNUC__)
                    return ::tools::impl::AtomicTrait< 0 >::template GenericDecrement< TypeT, Storage >::op( site );
#endif // WINDOWS_PLATFORM
                }
            };

            template< typename TypeT >
            TOOLS_FORCE_INLINE static Storage const innerAnd( TypeT volatile * const site, TypeT const mask )
            {
                static_assert( sizeof( TypeT ) == 1, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                return static_cast< Storage const >( _InterlockedAnd8(
                    reinterpret_cast< char volatile * const >( site ), static_cast< char const >( mask )));
#elif defined(__GNUC__)
                return ::tools::impl::AtomicTrait< 0 >::template GenericAnd< TypeT, Storage >::op( site, mask );
#endif // WINDOWS_PLATFORM
            }

            // Returns the old value
            template< typename TypeT >
            struct And
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const mask )
                {
                    return static_cast< TypeT const >( innerAnd( site, mask ));
                }
            };

            template< typename TypeT >
            TOOLS_FORCE_INLINE static Storage const innerOr( TypeT volatile * const site, TypeT const mask )
            {
                static_assert( sizeof( TypeT ) == 1, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                return static_cast< Storage const >( _InterlockedOr8(
                    reinterpret_cast< char volatile * const >( site ), static_cast< char const >( mask )));
#elif defined(__GNUC__)
                return ::tools::impl::AtomicTrait< 0 >::template GenericOr< TypeT, Storage >::op( site, mask );
#endif // WINDOWS_PLATFORM
            }

            // Returns the old value
            template< typename TypeT >
            struct Or
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const mask )
                {
                    return static_cast< TypeT const >( innerOr( site, mask ));
                }
            };

            template< typename TypeT >
            struct Swap
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT const site )
                {
                    static_assert( sizeof( TypeT ) == 1, "AtomicTrait for type is too narrow" );
                    return site; // no op
                }
            };
        };

        template<>
        struct AtomicTrait< 1 >::Read< bool >
        {
            TOOLS_FORCE_INLINE static bool const op( bool * const site )
            {
                // One byte cannot possibly be torn. Right?
                return *const_cast< bool const volatile * const >( site );
            }
        };

        template<>
        struct AtomicTrait< 1 >::Set< bool >
        {
            TOOLS_FORCE_INLINE static void op( bool volatile * const site, bool const newValue )
            {
                // On x86, normal memory writes are always exposed in program order, provided the compiler
                // emits the following in a single instruction. Which the actual assignment should be.
                *site = newValue;
            }
        };

#ifdef WINDOWS_PLATFORM
        // Returns the old value
        template<>
        struct AtomicTrait< 1 >::Cas< bool >
        {
            TOOLS_FORCE_INLINE static bool const op( bool volatile * const site, bool const oldValue, bool const newValue )
            {
                return !! _InterlockedCompareExchange8(
                    reinterpret_cast< char volatile * const >( site ),
                    static_cast< char const >( newValue ),
                    static_cast< char const >( oldValue ));
            }
        };
#endif // WINDOWS_PLATFORM

        template<>
        struct AtomicTrait< 1 >::Add< bool >
        {
            template< typename AnyT >
            TOOLS_FORCE_INLINE static bool const op( bool volatile * const, AnyT const )
            {
                static_assert( sizeof( AnyT ) == 0, "Atomic add for bool unimplemented" );
                return false;
            }
        };

        template<>
        struct AtomicTrait< 1 >::Subtract< bool >
        {
            template< typename AnyT >
            TOOLS_FORCE_INLINE static bool const op( bool volatile * const, AnyT const )
            {
                static_assert( sizeof( AnyT ) == 0, "Atomic subtract for bool unimplemented" );
                return false;
            }
        };

        template<>
        struct AtomicTrait< 1 >::Increment< bool >
        {
            template< typename AnyT >
            TOOLS_FORCE_INLINE static bool const op( AnyT volatile * const )
            {
                static_assert( sizeof( AnyT ) == 0, "Atomic increment for bool unimplemented" );
                return false;
            }
        };

        template<>
        struct AtomicTrait< 1 >::Decrement< bool >
        {
            template< typename AnyT >
            TOOLS_FORCE_INLINE static bool const op( AnyT volatile * const )
            {
                static_assert( sizeof( AnyT ) == 0, "Atomic decrement for bool unimplemented" );
                return false;
            }
        };

        template<>
        struct AtomicTrait< 1 >::And< bool >
        {
            TOOLS_FORCE_INLINE static bool const op( bool volatile * const site, bool const mask )
            {
                return !! ::tools::impl::AtomicTrait< 1 >::innerAnd( site, mask );
            }
        };

        template<>
        struct AtomicTrait< 1 >::Or< bool >
        {
            TOOLS_FORCE_INLINE static bool const op( bool volatile * const site, bool const mask )
            {
                return !! ::tools::impl::AtomicTrait< 1 >::innerOr( site, mask );
            }
        };

        template<>
        struct AtomicTrait< 2 >
        {
            typedef uint16 Storage;
            static unsigned const width_ = 2U;

            template< typename TypeT >
            struct Read
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT * const site )
                {
                    static_assert( sizeof( TypeT ) > 1, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 2, "AtomicTrait for type is too narrow" );
                    // Values smaller than int are atomic reads, so long as they don't cross a cache line.
                    TOOLS_ASSERTD( reinterpret_cast< uintptr_t const >( site ) % 2 == 0 );
                    return ::tools::impl::AtomicTrait< 0 >::template GenericRead< TypeT, Storage >::op( site );
                }
            };

            template< typename TypeT >
            struct Set
            {
                TOOLS_FORCE_INLINE static void op( TypeT volatile * const site, TypeT const newValue )
                {
                    static_assert( sizeof( TypeT ) > 1, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 2, "AtomicTrait for type is too narrow" );
                    TOOLS_ASSERTD( reinterpret_cast< uintptr_t const >( site ) % 2 == 0 );
                    // On x86, normal memory writes are always exposed in program order, provided the compiler
                    // emits the following as a single instruction.
                    ::tools::impl::AtomicTrait< 0 >::template GenericSet< TypeT, Storage >::op( site, newValue );
                }
            };

            // Returns the old value
            template< typename TypeT >
            struct Cas
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const oldValue, TypeT newValue )
                {
                    static_assert( sizeof( TypeT ) > 1, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 2, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return static_cast< TypeT const >( _InterlockedCompareExchange16(
                        reinterpret_cast< short volatile * const >( site ),
                        static_cast< short const >( newValue ),
                        static_cast< short const >( oldValue )));
#elif defined(__GNUC__)
                    return ::tools::impl::AtomicTrait< 0 >::template GenericCas< TypeT, Storage >::op( site, oldValue, newValue );
#endif // WINDOWS_PLATFORM
                }
            };

            // Returns the old value
            template< typename TypeT >
            struct Exchange
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const newValue )
                {
                    static_assert( sizeof( TypeT ) > 1, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 2, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return static_cast< TypeT const >( _InterlockedExchange16(
                        reinterpret_cast< short volatile * const >( site ),
                        static_cast< short const >( newValue )));
#elif defiend(__GNUC__)
                    return ::tools::impl::AtomicTrait< 0 >::template GenericExchange< TypeT, Storage >::op( site, newValue );
#endif // WINDOWS_PLATFORM
                }
            };

            // Returns the old value
            template< typename TypeT >
            struct Add
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const diff )
                {
                    static_assert( sizeof( TypeT ) > 1, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 2, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return static_cast< TypeT const >( _InterlockedExchangeAdd16(
                        reinterpret_cast< short volatile * const >( site ),
                        static_cast< short const >( diff )));
#elif defined(__GNUC__)
                    return ::tools::impl::AtomicTrait< 0 >::template GenericAdd< TypeT, Storage >::op( site, diff );
#endif // WINDOWS_PLATFORM
                }
            };

            // Returns the old value
            template< typename TypeT >
            struct Subtract
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const diff )
                {
                    static_assert( sizeof( TypeT ) > 1, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 2, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return static_cast< TypeT const >( _InterlockedExchangeAdd16(
                        reinterpret_cast< short volatile * const >( site ),
                        static_cast< short const >( diff ) * -1));
#elif defined(__GNUC__)
                    return::tools::impl::AtomicTrait< 0 >::template GenericSubtract< TypeT, Storage >::op( site, diff );
#endif // WINDOWS_PLATFORM
                }
            };

            // Return true if result is not '0'
            template< typename TypeT >
            struct Increment
            {
                TOOLS_FORCE_INLINE static bool const op( TypeT volatile * const site )
                {
                    static_assert( sizeof( TypeT ) > 1, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 2, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return !! _InterlockedIncrement16( reinterpret_cast< char volatile * const >( site ));
#elif defined(__GNUC__)
                    return ::tools::impl::AtomicTrait< 0 >::template GenericIncrement< TypeT, Storage >::op( site );
#endif // WINDOWS_PLATFORM
                }
            };

            // Return true if result is not '0'
            template< typename TypeT >
            struct Decrement
            {
                TOOLS_FORCE_INLINE static bool const op( TypeT volatile * const site )
                {
                    static_assert( sizeof( TypeT ) > 1, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 2, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return !! _InterlockedDecrement16( reinterpret_cast< short volatile * const >( site ));
#elif defined(__GNUC__)
                    return ::tools::impl::AtomicTrait< 0 >::template GenericDecrement< TypeT, Storage >::op( site );
#endif // WINDOWS_PLATFORM
                }
            };

            // Returns the old value
            template< typename TypeT >
            struct And
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const mask )
                {
                    static_assert( sizeof( TypeT ) > 1, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 2, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return static_cast< TypeT const >( _InterlockedAnd16(
                        reinterpret_cast< short volatile * const >( site ),
                        static_cast< short const >( mask )));
#elif defined(__GNUC__)
                    return ::tools::impl::AtomicTrait< 0 >::template GenericAnd< TypeT, Storage >::op( site, mask );
#endif // WINDOWS_PLATFORM
                }
            };

            // Returns the old value
            template< typename TypeT >
            struct Or
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const mask )
                {
                    static_assert( sizeof( TypeT ) > 1, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 2, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return static_cast< TypeT const >( _InterlockedOr16(
                        reinterpret_cast< short volatile * const >( site ),
                        static_cast< short const >( mask )));
#elif defined(__GNUC__)
                    return ::tools::impl::AtomicTrait< 0 >::template GenericOr< TypeT, Storage >::op( site, mask );
#endif // WINDOWS_PLATFORM
                }
            };

            template< typename TypeT >
            struct Swap
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT const site )
                {
                    static_assert( sizeof( TypeT ) > 1, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 2, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return static_cast< TypeT const >( _byteswap_ushort( static_cast< Storage const >( site )));
#elif defined(__GNUC__)
                    return static_cast< TypeT const >( ( static_cast< Storage const >( site ) >> 8 ) | ( static_cast< Storage const >( site ) << 8 ));
#endif // WINDOWS_PLATFORM
                }
            };
        };

        template<>
        struct AtomicTrait< 4 >
        {
            typedef uint32 Storage;
            static unsigned const width_ = 4U;

            template< typename TypeT >
            struct Read
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT * const site )
                {
                    static_assert( sizeof( TypeT ) > 2, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 4, "AtomicTrait for type is too narrow" );
                    // Values smaller than int are atomic reads, so long as they don't cross a cache line.
                    TOOLS_ASSERTD( reinterpret_cast< uintptr_t const >( site ) % 4 == 0 );
                    return ::tools::impl::AtomicTrait< 0 >::template GenericRead< TypeT, Storage >::op( site );
                }
            };

//#if (defined(WINDOWS_PLATFORM) && (_INTEGRAL_MAX_BITS == 32)) || (defined(__GNUC__) && (__SIZEOF_POINTER__ == 4))
            template<typename TypeT>
            struct Read< TypeT * >
            {
                TOOLS_FORCE_INLINE static TypeT * const op( TypeT ** const site )
                {
                    static_assert( sizeof( TypeT * ) == 4, "AtomicTrait for pointer types must be 32-bit" );
                    static_assert( sizeof( TypeT * ) == sizeof( void * ), "AtomicTrait for pointer does not match pointer size" );
                    // Values smaller than int are atomic reads, so long as they don't cross a cache line.
                    TOOLS_ASSERTD( reinterpret_cast< uintptr_t const >( site ) % 4 == 0 );
                    return *const_cast< TypeT * const volatile * const >( site );
                }
            };
//#endif // 32-bit pointers

            template< typename TypeT >
            struct Set
            {
                TOOLS_FORCE_INLINE static void op( TypeT volatile * const site, TypeT const newValue )
                {
                    static_assert( sizeof( TypeT ) > 2, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 4, "AtomicTrait for type is too narrow" );
                    TOOLS_ASSERTD( reinterpret_cast< uintptr_t const >( site ) % 4 == 0 );
                    // On x86, normal memory writes are always exposed in program order, provided the compiler
                    // emits the following as a single instruction.
                    ::tools::impl::AtomicTrait< 0 >::template GenericSet< TypeT, Storage >::op( site, newValue );
                }
            };

//#if (defined(WINDOWS_PLATFORM) && (_INTEGRAL_MAX_BITS == 32)) || (defined(__GNUC__) && (__SIZEOF_POINTER__ == 4))
            template< typename TypeT >
            struct Set< TypeT * >
            {
                TOOLS_FORCE_INLINE static void op( TypeT * volatile * const site, TypeT * const newValue )
                {
                    static_assert( sizeof( TypeT * ) == 4, "AtomicTrait for pointer types must be 32-bit" );
                    static_assert( sizeof( TypeT * ) == sizeof( void * ), "AtomicTrait for pointer does not match pointer size" );
                    // Values smaller than int are atomic reads, so long as they don't cross a cache line.
                    TOOLS_ASSERTD( reinterpret_cast< uintptr_t const >( site ) % 4 == 0 );
                    *site = newValue;
                }
            };
//#endif // 32-bit pointers

#if defined(WINDOWS_PLATFORM)
            template< typename TypeT >
            struct innerCas
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const oldValue, TypeT const newValue )
                {
                    return static_cast< TypeT const >( _InterlockedCompareExchange(
                        reinterpret_cast< long volatile * const >( site ),
                        static_cast< long const >( newValue ),
                        static_cast< long const >( oldValue )));
                }
            };

//#  if (_INTEGRAL_MAX_BITS == 32)
            template< typename TypeT >
            struct innerCas< TypeT * >
            {
                TOOLS_FORCE_INLINE static TypeT * const op( TypeT * volatile * const site, TypeT * const oldValue, TypeT * const newValue )
                {
                    return static_cast< TypeT * const >( _InterlockedCompareExchangePointer(
                        reinterpret_cast< void * volatile * const >( site ), newValue, oldValue ));
                }
            };
//#  endif // 32-bit pointers
#elif defined(__GNUC__)
            template< typename TypeT >
            struct innerCas
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const oldValue, TypeT const newValue )
                {
                    return ::tools::impl::AtomicTrait< 0 >::template GenericCas< TypeT, Storage >::op( site, oldValue, newValue );
                }
            };
#endif // WINDOWS_PLATFORM

            // Returns the old value
            template< typename TypeT >
            struct Cas
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const oldValue, TypeT const newValue )
                {
                    static_assert( sizeof( TypeT ) > 2, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 4, "AtomicTrait for type is too narrow" );
                    return innerCas< TypeT >::op( site, oldValue, newValue );
                }
            };

//#if (defined(WINDOWS_PLATFORM) && (_INTEGRAL_MAX_BITS == 32)) || (defined(__GNUC__) && (__SIZEOF_POINTER__ == 4))
            // Returns the old value
            template< typename TypeT >
            struct Cas< TypeT * >
            {
                TOOLS_FORCE_INLINE static TypeT * const op( TypeT * volatile * const site, TypeT * const oldValue, TypeT * const newValue )
                {
                    static_assert( sizeof( TypeT * ) == 4, "AtomicTrait for pointer types must be 32-bit" );
                    static_assert( sizeof( TypeT * ) == sizeof( void * ), "AtomicTrait for pointer does not match pointer size" );
                    return innerCas< TypeT * >::op( site, oldValue, newValue );
                }
            };
//#endif // 32-bit pointers

#if defined(WINDOWS_PLATFORM)
            template< typename TypeT >
            struct innerExchange
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const newValue )
                {
                    return static_cast< TypeT const >( _InterlockedExchange(
                        reinterpret_cast< long volatile * const >( site ),
                        static_cast< long const >( newValue )));
                }
            };

//#  if (_INTEGRAL_MAX_BITS == 32)
            template< typename TypeT >
            struct innerExchange< TypeT * >
            {
                TOOLS_FORCE_INLINE static TypeT * const op( TypeT * volatile * const site, TypeT * const newValue )
                {
                    return static_cast< TypeT * const >( _InterlockedExchangePointer(
                        reinterpret_cast< void * volatile * const >( site ), newValue ));
                }
            };
//#  endif // 32-bit pointers
#elif defined(__GNUC__)
            template< typename TypeT >
            struct innerExchange
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const newValue )
                {
                    return ::tools::impl::AtomicTrait< 0 >::template GenericExchange< TypeT, Storage >::op( site, newValue );
                }
            };
#endif // WINDOWS_PLATFORM

            // Returns the old value
            template< typename TypeT >
            struct Exchange
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const newValue )
                {
                    static_assert( sizeof( TypeT ) > 2, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 4, "AtomicTrait for type is too narrow" );
                    return innerExchange< TypeT >::op( site, newValue );
                }
            };

//#if (defined(WINDOWS_PLATFORM) && (_INTEGRAL_MAX_BITS == 32)) || (defined(__GNUC__) && (__SIZEOF_POINTER__ == 4))
            // Returns the old value
            template< typename TypeT >
            struct Exchange< TypeT * >
            {
                TOOLS_FORCE_INLINE static TypeT * const op( TypeT * volatile * const site, TypeT * const newValue )
                {
                    static_assert( sizeof( TypeT * ) == 4, "AtomicTrait for pointer types must be 32-bit" );
                    static_assert( sizeof( TypeT * ) == sizeof( void * ), "AtomicTrait for pointer does not match pointer size" );
                    return innerExchange< TypeT * >::op( site, newValue );
                }
            };
//#endif // 32-bit pointers

            // Returns the old value
            template< typename TypeT >
            struct Add
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const diff )
                {
                    static_assert( sizeof( TypeT ) > 2, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 4, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return static_cast< TypeT const >( _InterlockedExchangeAdd(
                        reinterpret_cast< long volatile * const >( site ),
                        static_cast< long const >( diff )));
#elif defined(__GNUC__)
                    return ::tools::impl::AtomicTrait< 0 >::template GenericAdd< TypeT, Storage >::op( site, diff );
#endif // WINDOWS_PLATFORM
                }
            };

//#if (defined(WINDOWS_PLATFORM) && (_INTEGRAL_MAX_BITS == 32)) || (defined(__GNUC__) && (__SIZEOF_POINTER__ == 4))
            // Returns the old value
            template< typename TypeT >
            struct Add< TypeT * >
            {
                TOOLS_FORCE_INLINE static TypeT * const op( TypeT * volatile * const site, TypeT * const diff )
                {
                    static_assert( sizeof( TypeT * ) == 4, "AtomicTrait for pointer types must be 32-bit" );
                    static_assert( sizeof( TypeT * ) == sizeof( void * ), "AtomicTrait for pointer does not match pointer size" );
                    static_assert( sizeof( TypeT ) == 0, "atomic add for pointer types unimplemented" );
//#if defined(WINDOWS_PLATFORM)
//                    return static_cast< TypeT const >( _InterlockedExchangeAdd(
//                        reinterpret_cast< long volatile * const >( site ),
//                        static_cast< long const >( diff )));
//#elif defined(__GNUC__)
//                    return ::tools::impl::AtomicTrait< 0 >::template GenericAdd< TypeT, Storage >::op( site, diff );
//#endif // WINDOWS_PLATFORM
                    return static_cast< TypeT * const >( nullptr );
                }
            };
//#endif // 32-bit pointers

            // Returns the old value
            template< typename TypeT >
            struct Subtract
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const diff )
                {
                    static_assert( sizeof( TypeT ) > 2, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 4, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return static_cast< TypeT const >( _InterlockedExchangeAdd(
                        reinterpret_cast< long volatile * const >( site ),
                        static_cast< long const >( diff ) * -1L));
#elif defined(__GNUC__)
                    return ::tools::impl::AtomicTrait< 0 >::template GenericSubtract< TypeT, Storage >::op( site, diff );
#endif // WINDOWS_PLATFORM
                }
            };

//#if (defined(WINDOWS_PLATFORM) && (_INTEGRAL_MAX_BITS == 32)) || (defined(__GNUC__) && (__SIZEOF_POINTER__ == 4))
            // Returns the old value
            template< typename TypeT >
            struct Subtract< TypeT * >
            {
                TOOLS_FORCE_INLINE static TypeT * const op( TypeT * volatile * const site, TypeT * const diff )
                {
                    static_assert( sizeof( TypeT * ) == 4, "AtomicTrait for pointer types must be 32-bit" );
                    static_assert( sizeof( TypeT * ) == sizeof( void * ), "AtomicTrait for pointer does not match pointer size" );
                    static_assert( sizeof( TypeT ) == 0, "atomic subtract for pointer types unimplemented" );
//#if defined(WINDOWS_PLATFORM)
//                    return static_cast< TypeT const >( _InterlockedExchangeAdd(
//                        reinterpret_cast< long volatile * const >( site ),
//                        static_cast< long const >( diff ) * -1L));
//#elif defined(__GNUC__)
//                    return ::tools::impl::AtomicTrait< 0 >::template GenericSubtract< TypeT, Storage >::op( site, diff );
//#endif // WINDOWS_PLATFORM
                    return static_cast< TypeT * const >( nullptr );
                }
            };
//#endif // 32-bit pointers

            // Returns true if result is not '0'
            template< typename TypeT >
            struct Increment
            {
                TOOLS_FORCE_INLINE static bool const op( TypeT volatile * const site )
                {
                    static_assert( sizeof( TypeT ) > 2, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 4, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return !! _InterlockedIncrement( reinterpret_cast< long volatile * const >( site ));
#elif defined(__GNUC__)
                    return ::tools::impl::AtomicTrait< 0 >::template GenericIncrement< TypeT, Storage >::op( site );
#endif // WINDOWS_PLATFORM
                }
            };

//#if (defined(WINDOWS_PLATFORM) && (_INTEGRAL_MAX_BITS == 32)) || (defined(__GNUC__) && (__SIZEOF_POINTER__ == 4))
            // Returns true if result is not '0'
            template< typename TypeT >
            struct Increment< TypeT * >
            {
                TOOLS_FORCE_INLINE static bool const op( TypeT * volatile * const site )
                {
                    static_assert( sizeof( TypeT * ) == 4, "AtomicTrait for pointer types must be 32-bit" );
                    static_assert( sizeof( TypeT * ) == sizeof( void * ), "AtomicTrait for pointer does not match pointer size" );
                    static_assert( sizeof( TypeT ) == 0, "atomic increment for pointer types unimplemented" );
//#if defined(WINDOWS_PLATFORM)
//                    return !! _InterlockedIncrement( reinterpret_cast< long volatile * const >( site ));
//#elif defined(__GNUC__)
//                    return ::tools::impl::AtomicTrait< 0 >::template GenericIncrement< TypeT, Storage >::op( site );
//#endif // WINDOWS_PLATFORM
                    return false;
                }
            };
//#endif // 32-bit pointers

            // Returns true if result is not '0'
            template< typename TypeT >
            struct Decrement
            {
                TOOLS_FORCE_INLINE static bool const op( TypeT volatile * const site )
                {
                    static_assert( sizeof( TypeT ) > 2, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 4, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return !! _InterlockedDecrement( reinterpret_cast< long volatile * const >( site ));
#elif defined(__GNUC__)
                    return ::tools::impl::AtomicTrait< 0 >::template GenericDecrement< TypeT, Storage >::op( site );
#endif // WINDOWS_PLATFORM
                }
            };

//#if (defined(WINDOWS_PLATFORM) && (_INTEGRAL_MAX_BITS == 32)) || (defined(__GNUC__) && (__SIZEOF_POINTER__ == 4))
            // Returns true if result is not '0'
            template< typename TypeT >
            struct Decrement< TypeT * >
            {
                TOOLS_FORCE_INLINE static bool const op( TypeT * volatile * const site )
                {
                    static_assert( sizeof( TypeT * ) == 4, "AtomicTrait for pointer types must be 32-bit" );
                    static_assert( sizeof( TypeT * ) == sizeof( void * ), "AtomicTrait for pointer does not match pointer size" );
                    static_assert( sizeof( TypeT ) == 0, "atomic decrement for pointer types unimplemented" );
//#if defined(WINDOWS_PLATFORM)
//                    return !! _InterlockedDecrement( reinterpret_cast< long volatile * const >( site ));
//#elif defined(__GNUC__)
//                    return ::tools::impl::AtomicTrait< 0 >::template GenericDecrement< TypeT, Storage >::op( site );
//#endif // WINDOWS_PLATFORM
                    return false;
                }
            };
//#endif // 32-bit pointers

            // Returns the old value
            template< typename TypeT >
            struct And
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const mask )
                {
                    static_assert( sizeof( TypeT ) > 2, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 4, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return static_cast< TypeT const >( _InterlockedAnd(
                        reinterpret_cast< long volatile * const >( site ),
                        static_cast< long const >( mask )));
#elif defined(__GNUC__)
                    return ::tools::impl::AtomicTrait< 0 >::template GenericAnd< TypeT, Storage >::op( site, mask );
#endif // WINDOWS_PLATFORM
                }
            };

//#if (defined(WINDOWS_PLATFORM) && (_INTEGRAL_MAX_BITS == 32)) || (defined(__GNUC__) && (__SIZEOF_POINTER__ == 4))
            // Returns the old value
            template< typename TypeT >
            struct And< TypeT * >
            {
                TOOLS_FORCE_INLINE static TypeT * const op( TypeT * volatile * const site, TypeT * const mask )
                {
                    static_assert( sizeof( TypeT * ) == 4, "AtomicTrait for pointer types must be 32-bit" );
                    static_assert( sizeof( TypeT * ) == sizeof( void * ), "AtomicTrait for pointer does not match pointer size" );
                    static_assert( sizeof( TypeT ) == 0, "atomic AND for pointer types unimplemented" );
//#if defined(WINDOWS_PLATFORM)
//                    return static_cast< TypeT const >( _InterlockedAnd(
//                        reinterpret_cast< long volatile * const >( site ),
//                        static_cast< long const >( mask )));
//#elif defined(__GNUC__)
//                    return ::tools::impl::AtomicTrait< 0 >::template GenericAnd< TypeT, Storage >::op( site, mask );
//#endif // WINDOWS_PLATFORM
                    return static_cast< TypeT * const >( nullptr );
                }
            };
//#endif // 32-bit pointers

            // Returns the old value
            template< typename TypeT >
            struct Or
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const mask )
                {
                    static_assert( sizeof( TypeT ) > 2, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 4, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return static_cast< TypeT const >( _InterlockedOr(
                        reinterpret_cast< long volatile * const >( site ),
                        static_cast< long const >( mask )));
#elif defined(__GNUC__)
                    return ::tools::impl::AtomicTrait< 0 >::template GenericOr< TypeT, Storage >::op( site, mask );
#endif // WINDOWS_PLATFORM
                }
            };

//#if (defined(WINDOWS_PLATFORM) && (_INTEGRAL_MAX_BITS == 32)) || (defined(__GNUC__) && (__SIZEOF_POINTER__ == 4))
            // Returns the old value
            template< typename TypeT >
            struct Or< TypeT * >
            {
                TOOLS_FORCE_INLINE static TypeT * const op( TypeT * volatile * const site, TypeT * const mask )
                {
                    static_assert( sizeof( TypeT * ) == 4, "AtomicTrait for pointer types must be 32-bit" );
                    static_assert( sizeof( TypeT * ) == sizeof( void * ), "AtomicTrait for pointer does not match pointer size" );
                    static_assert( sizeof( TypeT ) == 0, "atomic OR for pointer types unimplemented" );
//#if defined(WINDOWS_PLATFORM)
//                    return static_cast< TypeT const >( _InterlockedOr(
//                        reinterpret_cast< long volatile * const >( site ),
//                        static_cast< long const >( mask )));
//#elif defined(__GNUC__)
//                    return ::tools::impl::AtomicTrait< 0 >::template GenericOr< TypeT, Storage >::op( site, mask );
//#endif // WINDOWS_PLATFORM
                    return static_cast< TypeT * const >( nullptr );
                }
            };
//#endif // 32-bit pointers

            TOOLS_FORCE_INLINE static Storage const innerSwap( Storage const site )
            {
#if defined(WINDOWS_PLATFORM)
                return static_cast< Storage const >( _byteswap_ulong( site ));
#elif defined(__GNUC__)
                return static_cast< Storage const >( __builtin_bswap32( site ));
#endif // WINDOWS_PLATFORM
            }

            template< typename TypeT >
            struct Swap
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT const site )
                {
                    static_assert( sizeof( TypeT ) > 2, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 4, "AtomicTrait for type is too narrow" );
                    return static_cast< TypeT const >( innerSwap( static_cast< Storage const >( site )));
                }
            };

//#if (defined(WINDOWS_PLATFORM) && (_INTEGRAL_MAX_BITS == 32)) || (defined(__GNUC__) && (__SIZEOF_POINTER__ == 4))
            template< typename TypeT >
            struct Swap< TypeT * >
            {
                TOOLS_FORCE_INLINE static TypeT * const op( TypeT * const site )
                {
                    static_assert( sizeof( TypeT * ) == 4, "AtomicTrait for pointer types must be 32-bit" );
                    static_assert( sizeof( TypeT * ) == sizeof( void * ), "AtomicTrait for pointer does not match pointer size" );
                    return reinterpret_cast< TypeT * const >( innerSwap( reinterpret_cast< Storage const >( site )));
                }
            };
//#endif // 32-bit pointers
        };

        template<>
        struct AtomicTrait< 8 >
        {
            typedef uint64 Storage;
            static unsigned const width_ = 8U;

            template< typename TypeT >
            struct Read
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT * const site )
                {
                    static_assert( sizeof( TypeT ) > 4, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 8, "AtomicTrait for type is too narrow" );
                    // Values smaller than int are atomic reads, so long as they don't cross a cache line.
                    TOOLS_ASSERTD( reinterpret_cast< uintptr_t const >( site ) % 8 == 0 );
                    return ::tools::impl::AtomicTrait< 0 >::template GenericRead< TypeT, Storage >::op( site );
                }
            };

//#if (defined(WINDOWS_PLATFORM) && (_INTEGRAL_MAX_BITS == 64)) || (defined(__GNUC__) && (__SIZEOF_POINTER__ == 8))
            template<typename TypeT>
            struct Read< TypeT * >
            {
                TOOLS_FORCE_INLINE static TypeT * const op( TypeT ** const site )
                {
                    static_assert( sizeof( TypeT * ) == 8, "AtomicTrait for pointer types must be 64-bit" );
                    static_assert( sizeof( TypeT * ) == sizeof( void * ), "AtomicTrait for pointer does not match pointer size" );
                    // Values smaller than int are atomic reads, so long as they don't cross a cache line.
                    TOOLS_ASSERTD( reinterpret_cast< uintptr_t const >( site ) % 8 == 0 );
                    return *const_cast< TypeT * const volatile * const >( site );
                }
            };
//#endif // 64-bit pointers

            template< typename TypeT >
            struct Set
            {
                TOOLS_FORCE_INLINE static void op( TypeT volatile * const site, TypeT const newValue )
                {
                    static_assert( sizeof( TypeT ) > 4, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 8, "AtomicTrait for type is too narrow" );
                    TOOLS_ASSERTD( reinterpret_cast< uintptr_t const >( site ) % 8 == 0 );
                    // On x86, normal memory writes are always exposed in program order, provided the compiler
                    // emits the following as a single instruction.
                    ::tools::impl::AtomicTrait< 0 >::template GenericSet< TypeT, Storage >::op( site, newValue );
                }
            };

//#if (defined(WINDOWS_PLATFORM) && (_INTEGRAL_MAX_BITS == 64)) || (defined(__GNUC__) && (__SIZEOF_POINTER__ == 8))
            template< typename TypeT >
            struct Set< TypeT * >
            {
                TOOLS_FORCE_INLINE static void op( TypeT * volatile * const site, TypeT * const newValue )
                {
                    static_assert( sizeof( TypeT * ) == 8, "AtomicTrait for pointer types must be 64-bit" );
                    static_assert( sizeof( TypeT * ) == sizeof( void * ), "AtomicTrait for pointer does not match pointer size" );
                    // Values smaller than int are atomic reads, so long as they don't cross a cache line.
                    TOOLS_ASSERTD( reinterpret_cast< uintptr_t const >( site ) % 8 == 0 );
                    *site = newValue;
                }
            };
//#endif // 64-bit pointers

#if defined(WINDOWS_PLATFORM)
            template< typename TypeT >
            struct innerCas
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const oldValue, TypeT const newValue )
                {
                    return static_cast< TypeT const >( _InterlockedCompareExchange64(
                        reinterpret_cast< long long volatile * const >( site ),
                        static_cast< long long const >( newValue ),
                        static_cast< long long const >( oldValue )));
                }
            };

//#  if (_INTEGRAL_MAX_BITS == 64)
            template< typename TypeT >
            struct innerCas< TypeT * >
            {
                TOOLS_FORCE_INLINE static TypeT * const op( TypeT * volatile * const site, TypeT * const oldValue, TypeT * const newValue )
                {
                    return static_cast< TypeT * const >( _InterlockedCompareExchangePointer(
                        reinterpret_cast< void * volatile * const >( site ), newValue, oldValue ));
                }
            };
//#  endif // 64-bit pointers
#elif defined(__GNUC__)
            template< typename TypeT >
            struct innerCas
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const oldValue, TypeT const newValue )
                {
                    return ::tools::impl::AtomicTrait< 0 >::template GenericCas< TypeT, Storage >::op( site, oldValue, newValue );
                }
            };
#endif // WINDOWS_PLATFORM

            // Returns the old value
            template< typename TypeT >
            struct Cas
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const oldValue, TypeT const newValue )
                {
                    static_assert( sizeof( TypeT ) > 4, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 8, "AtomicTrait for type is too narrow" );
                    return innerCas< TypeT >::op( site, oldValue, newValue );
                }
            };

//#if (defined(WINDOWS_PLATFORM) && (_INTEGRAL_MAX_BITS == 64)) || (defined(__GNUC__) && (__SIZEOF_POINTER__ == 8))
            // Returns the old value
            template< typename TypeT >
            struct Cas< TypeT * >
            {
                TOOLS_FORCE_INLINE static TypeT * const op( TypeT * volatile * const site, TypeT * const oldValue, TypeT * const newValue )
                {
                    static_assert( sizeof( TypeT * ) == 8, "AtomicTrait for pointer types must be 64-bit" );
                    static_assert( sizeof( TypeT * ) == sizeof( void * ), "AtomicTrait for pointer does not match pointer size" );
                    return innerCas< TypeT * >::op( site, oldValue, newValue );
                }
            };
//#endif // 64-bit pointers

#if defined(WINDOWS_PLATFORM)
            template< typename TypeT >
            struct innerExchange
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const newValue )
                {
                    return static_cast< TypeT const >( _InterlockedExchange64(
                        reinterpret_cast< long long volatile * const >( site ),
                        static_cast< long long const >( newValue )));
                }
            };

//#  if (_INTEGRAL_MAX_BITS == 64)
            template< typename TypeT >
            struct innerExchange< TypeT * >
            {
                TOOLS_FORCE_INLINE static TypeT * const op( TypeT * volatile * const site, TypeT * const newValue )
                {
                    return static_cast< TypeT * const >( _InterlockedExchangePointer(
                        reinterpret_cast< void * volatile * const >( site ), newValue ));
                }
            };
//#  endif // 64-bit pointers
#elif defined(__GNUC__)
            template< typename TypeT >
            struct innerExchange
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const newValue )
                {
                    return ::tools::impl::AtomicTrait< 0 >::template GenericExchange< TypeT, Storage >::op( site, newValue );
                }
            };
#endif // WINDOWS_PLATFORM

            // Returns the old value
            template< typename TypeT >
            struct Exchange
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const newValue )
                {
                    static_assert( sizeof( TypeT ) > 4, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 8, "AtomicTrait for type is too narrow" );
                    return innerExchange< TypeT >::op( site, newValue );
                }
            };

//#if (defined(WINDOWS_PLATFORM) && (_INTEGRAL_MAX_BITS == 64)) || (defined(__GNUC__) && (__SIZEOF_POINTER__ == 8))
            // Returns the old value
            template< typename TypeT >
            struct Exchange< TypeT * >
            {
                TOOLS_FORCE_INLINE static TypeT * const op( TypeT * volatile * const site, TypeT * const newValue )
                {
                    static_assert( sizeof( TypeT * ) == 8, "AtomicTrait for pointer types must be 64-bit" );
                    static_assert( sizeof( TypeT * ) == sizeof( void * ), "AtomicTrait for pointer does not match pointer size" );
                    return innerExchange< TypeT * >::op( site, newValue );
                }
            };
//#endif // 64-bit pointers

            // Returns the old value
            template< typename TypeT >
            struct Add
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const diff )
                {
                    static_assert( sizeof( TypeT ) > 4, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 8, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return static_cast< TypeT const >( _InterlockedExchangeAdd64(
                        reinterpret_cast< long long volatile * const >( site ),
                        static_cast< long long const >( diff )));
#elif defined(__GNUC__)
                    return ::tools::impl::AtomicTrait< 0 >::template GenericAdd< TypeT, Storage >::op( site, diff );
#endif // WINDOWS_PLATFORM
                }
            };

//#if (defined(WINDOWS_PLATFORM) && (_INTEGRAL_MAX_BITS == 64)) || (defined(__GNUC__) && (__SIZEOF_POINTER__ == 8))
            // Returns the old value
            template< typename TypeT >
            struct Add< TypeT * >
            {
                TOOLS_FORCE_INLINE static TypeT * const op( TypeT * volatile * const site, TypeT * const diff )
                {
                    static_assert( sizeof( TypeT * ) == 8, "AtomicTrait for pointer types must be 64-bit" );
                    static_assert( sizeof( TypeT * ) == sizeof( void * ), "AtomicTrait for pointer does not match pointer size" );
                    static_assert( sizeof( TypeT ) == 0, "atomic add for pointer types unimplemented" );
//#if defined(WINDOWS_PLATFORM)
//                    return static_cast< TypeT const >( _InterlockedExchangeAdd64(
//                        reinterpret_cast< long long volatile * const >( site ),
//                        static_cast< long long const >( diff )));
//#elif defined(__GNUC__)
//                    return ::tools::impl::AtomicTrait< 0 >::template GenericAdd< TypeT, Storage >::op( site, diff );
//#endif // WINDOWS_PLATFORM
                    return static_cast< TypeT * const >( nullptr );
                }
            };
//#endif // 64-bit pointers

            // Returns the old value
            template< typename TypeT >
            struct Subtract
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const diff )
                {
                    static_assert( sizeof( TypeT ) > 4, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 8, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return static_cast< TypeT const >( _InterlockedExchangeAdd64(
                        reinterpret_cast< long long volatile * const >( site ),
                        static_cast< long long const >( diff ) * -1L));
#elif defined(__GNUC__)
                    return ::tools::impl::AtomicTrait< 0 >::template GenericSubtract< TypeT, Storage >::op( site, diff );
#endif // WINDOWS_PLATFORM
                }
            };

//#if (defined(WINDOWS_PLATFORM) && (_INTEGRAL_MAX_BITS == 64)) || (defined(__GNUC__) && (__SIZEOF_POINTER__ == 8))
            // Returns the old value
            template< typename TypeT >
            struct Subtract< TypeT * >
            {
                TOOLS_FORCE_INLINE static TypeT * const op( TypeT * volatile * const site, TypeT * const diff )
                {
                    static_assert( sizeof( TypeT * ) == 8, "AtomicTrait for pointer types must be 64-bit" );
                    static_assert( sizeof( TypeT * ) == sizeof( void * ), "AtomicTrait for pointer does not match pointer size" );
                    static_assert( sizeof( TypeT ) == 0, "atomic subtract for pointer types unimplemented" );
//#if defined(WINDOWS_PLATFORM)
//                    return static_cast< TypeT const >( _InterlockedExchangeAdd64(
//                        reinterpret_cast< long long volatile * const >( site ),
//                        static_cast< long long const >( diff ) * -1L));
//#elif defined(__GNUC__)
//                    return ::tools::impl::AtomicTrait< 0 >::template GenericSubtract< TypeT, Storage >::op( site, diff );
//#endif // WINDOWS_PLATFORM
                    return static_cast< TypeT * const >( nullptr );
                }
            };
//#endif // 64-bit pointers

            // Returns true if result is not '0'
            template< typename TypeT >
            struct Increment
            {
                TOOLS_FORCE_INLINE static bool const op( TypeT volatile * const site )
                {
                    static_assert( sizeof( TypeT ) > 4, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 8, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return !! _InterlockedIncrement64( reinterpret_cast< long long volatile * const >( site ));
#elif defined(__GNUC__)
                    return ::tools::impl::AtomicTrait< 0 >::template GenericIncrement< TypeT, Storage >::op( site );
#endif // WINDOWS_PLATFORM
                }
            };

//#if (defined(WINDOWS_PLATFORM) && (_INTEGRAL_MAX_BITS == 64)) || (defined(__GNUC__) && (__SIZEOF_POINTER__ == 8))
            // Returns true if result is not '0'
            template< typename TypeT >
            struct Increment< TypeT * >
            {
                TOOLS_FORCE_INLINE static bool const op( TypeT * volatile * const site )
                {
                    static_assert( sizeof( TypeT * ) == 8, "AtomicTrait for pointer types must be 64-bit" );
                    static_assert( sizeof( TypeT * ) == sizeof( void * ), "AtomicTrait for pointer does not match pointer size" );
                    static_assert( sizeof( TypeT ) == 0, "atomic increment for pointer types unimplemented" );
//#if defined(WINDOWS_PLATFORM)
//                    return !! _InterlockedIncrement64( reinterpret_cast< long long volatile * const >( site ));
//#elif defined(__GNUC__)
//                    return ::tools::impl::AtomicTrait< 0 >::template GenericIncrement< TypeT, Storage >::op( site );
//#endif // WINDOWS_PLATFORM
                    return false;
                }
            };
//#endif // 64-bit pointers

            // Returns true if result is not '0'
            template< typename TypeT >
            struct Decrement
            {
                TOOLS_FORCE_INLINE static bool const op( TypeT volatile * const site )
                {
                    static_assert( sizeof( TypeT ) > 4, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 8, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return !! _InterlockedDecrement64( reinterpret_cast< long long volatile * const >( site ));
#elif defined(__GNUC__)
                    return ::tools::impl::AtomicTrait< 0 >::template GenericDecrement< TypeT, Storage >::op( site );
#endif // WINDOWS_PLATFORM
                }
            };

//#if (defined(WINDOWS_PLATFORM) && (_INTEGRAL_MAX_BITS == 64)) || (defined(__GNUC__) && (__SIZEOF_POINTER__ == 8))
            // Returns true if result is not '0'
            template< typename TypeT >
            struct Decrement< TypeT * >
            {
                TOOLS_FORCE_INLINE static bool const op( TypeT * volatile * const site )
                {
                    static_assert( sizeof( TypeT * ) == 8, "AtomicTrait for pointer types must be 64-bit" );
                    static_assert( sizeof( TypeT * ) == sizeof( void * ), "AtomicTrait for pointer does not match pointer size" );
                    static_assert( sizeof( TypeT ) == 0, "atomic decrement for pointer types unimplemented" );
//#if defined(WINDOWS_PLATFORM)
//                    return !! _InterlockedDecrement64( reinterpret_cast< long long volatile * const >( site ));
//#elif defined(__GNUC__)
//                    return ::tools::impl::AtomicTrait< 0 >::template GenericDecrement< TypeT, Storage >::op( site );
//#endif // WINDOWS_PLATFORM
                    return false;
                }
            };
//#endif // 64-bit pointers

            // Returns the old value
            template< typename TypeT >
            struct And
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const mask )
                {
                    static_assert( sizeof( TypeT ) > 4, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 8, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return static_cast< TypeT const >( _InterlockedAnd64(
                        reinterpret_cast< long long volatile * const >( site ),
                        static_cast< long long const >( mask )));
#elif defined(__GNUC__)
                    return ::tools::impl::AtomicTrait< 0 >::template GenericAnd< TypeT, Storage >::op( site, mask );
#endif // WINDOWS_PLATFORM
                }
            };

//#if (defined(WINDOWS_PLATFORM) && (_INTEGRAL_MAX_BITS == 64)) || (defined(__GNUC__) && (__SIZEOF_POINTER__ == 8))
            // Returns the old value
            template< typename TypeT >
            struct And< TypeT * >
            {
                TOOLS_FORCE_INLINE static TypeT * const op( TypeT * volatile * const site, TypeT * const mask )
                {
                    static_assert( sizeof( TypeT * ) == 8, "AtomicTrait for pointer types must be 64-bit" );
                    static_assert( sizeof( TypeT * ) == sizeof( void * ), "AtomicTrait for pointer does not match pointer size" );
                    static_assert( sizeof( TypeT ) == 0, "atomic AND for pointer types unimplemented" );
//#if defined(WINDOWS_PLATFORM)
//                    return static_cast< TypeT const >( _InterlockedAnd64(
//                        reinterpret_cast< long long volatile * const >( site ),
//                        static_cast< long long const >( mask )));
//#elif defined(__GNUC__)
//                    return ::tools::impl::AtomicTrait< 0 >::template GenericAnd< TypeT, Storage >::op( site, mask );
//#endif // WINDOWS_PLATFORM
                    return static_cast< TypeT * const >( nullptr );
                }
            };
//#endif // 64-bit pointers

            // Returns the old value
            template< typename TypeT >
            struct Or
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const site, TypeT const mask )
                {
                    static_assert( sizeof( TypeT ) > 4, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 8, "AtomicTrait for type is too narrow" );
#if defined(WINDOWS_PLATFORM)
                    return static_cast< TypeT const >( _InterlockedOr64(
                        reinterpret_cast< long long volatile * const >( site ),
                        static_cast< long long const >( mask )));
#elif defined(__GNUC__)
                    return ::tools::impl::AtomicTrait< 0 >::template GenericOr< TypeT, Storage >::op( site, mask );
#endif // WINDOWS_PLATFORM
                }
            };

//#if (defined(WINDOWS_PLATFORM) && (_INTEGRAL_MAX_BITS == 64)) || (defined(__GNUC__) && (__SIZEOF_POINTER__ == 8))
            // Returns the old value
            template< typename TypeT >
            struct Or< TypeT * >
            {
                TOOLS_FORCE_INLINE static TypeT * const op( TypeT * volatile * const site, TypeT * const mask )
                {
                    static_assert( sizeof( TypeT * ) == 8, "AtomicTrait for pointer types must be 64-bit" );
                    static_assert( sizeof( TypeT * ) == sizeof( void * ), "AtomicTrait for pointer does not match pointer size" );
                    static_assert( sizeof( TypeT ) == 0, "atomic OR for pointer types unimplemented" );
//#if defined(WINDOWS_PLATFORM)
//                    return static_cast< TypeT const >( _InterlockedOr64(
//                        reinterpret_cast< long long volatile * const >( site ),
//                        static_cast< long long const >( mask )));
//#elif defined(__GNUC__)
//                    return ::tools::impl::AtomicTrait< 0 >::template GenericOr< TypeT, Storage >::op( site, mask );
//#endif // WINDOWS_PLATFORM
                    return static_cast< TypeT * const >( nullptr );
                }
            };
//#endif // 64-bit pointers

            TOOLS_FORCE_INLINE static Storage const innerSwap( Storage const site )
            {
#if defined(WINDOWS_PLATFORM)
                return static_cast< Storage const >( _byteswap_uint64( site ));
#elif defined(__GNUC__)
                return static_cast< Storage const >( __builtin_bswap64( site ));
#endif // WINDOWS_PLATFORM
            }

            template< typename TypeT >
            struct Swap
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT const site )
                {
                    static_assert( sizeof( TypeT ) > 4, "AtomicTrait for type is too wide" );
                    static_assert( sizeof( TypeT ) <= 8, "AtomicTrait for type is too narrow" );
                    return static_cast< TypeT const >( innerSwap( static_cast< Storage const >( site )));
                }
            };

//#if (defined(WINDOWS_PLATFORM) && (_INTEGRAL_MAX_BITS == 64)) || (defined(__GNUC__) && (__SIZEOF_POINTER__ == 8))
            template< typename TypeT >
            struct Swap< TypeT * >
            {
                TOOLS_FORCE_INLINE static TypeT * const op( TypeT * const site )
                {
                    static_assert( sizeof( TypeT * ) == 8, "AtomicTrait for pointer types must be 64-bit" );
                    static_assert( sizeof( TypeT * ) == sizeof( void * ), "AtomicTrait for pointer does not match pointer size" );
                    return reinterpret_cast< TypeT * const >( innerSwap( reinterpret_cast< Storage const >( site )));
                }
            };
//#endif // 64-bit pointers
        };

#if defined(WINDOWS_PLATFORM)
        __declspec( align( 16 )) struct AtomicLongStorage
        {
            int64_t volatile data[ 2U ];

            TOOLS_FORCE_INLINE void init( void )
            {
                data[ 0U ] = data[ 1U ] = 0U;
            }

            TOOLS_FORCE_INLINE bool const operator==( AtomicLongStorage const & v ) const
            {
                return ( data[ 0U ] == v.data[ 0U ] ) && ( data[ 1U ] == v.data[ 1U ] );
            }

            TOOLS_FORCE_INLINE bool const operator!=( AtomicLongStorage const & v ) const
            {
                return ( data[ 0U ] != v.data[ 0U ] ) || ( data[ 1U ] != v.data[ 1U ] );
            }
        };
#elif defined(__GNUC__)
        struct AtomicLongStorage
        {
            __int128_t volatile data;

            TOOLS_FORCE_INLINE init( void )
            {
                data = 0U;
            }

            TOOLS_FORCE_INLINE bool const operator==( AtomicLongStorage const & v ) const
            {
                return ( data == v.data );
            }

            TOOLS_FORCE_INLINE bool const operator!=( AtomicLongStorage const & v ) const
            {
                return ( data != v.data );
            }
        };
#endif // WINDOWS_PLATFORM

        template<>
        struct AtomicTrait< 16U >
        {
            typedef AtomicLongStorage Storage;
            static unsigned const width_ = 16U;

            template< typename TypeT >
            struct Read
            {
                TOOLS_FORCE_INLINE static Storage const op( Storage * const site )
                {
TOOLS_WARNINGS_SAVE
TOOLS_WARNINGS_DISABLE_UNINITIALIZED
                    // It is alright that ret is uninitialized. Just doing  a CAS to get an atomic read.
                    Storage ret;
#if defined(WINDOWS_PLATFORM)
                    _InterlockedCompareExchange128( site->data, ret.data[ 1U ], ret.data[ 0U ],
                        const_cast< int64_t * const >( ret.data ));
#elif defined(__GNUC__)
#  if defined(TOOLS_DEBUG)
                    ret.init();  // make valgrind happy as dumb as it is.
#  endif // TOOLS_DEBUG
                    ret.data = __sync_val_compare_and_swap( &site->data, ret.data, ret.data );
#endif // WINDOWS_PLATFORM
                    return ret;
TOOLS_WARNINGS_RESTORE
                }
            };

            template< typename TypeT >
            struct Set
            {
                TOOLS_FORCE_INLINE static void op( Storage volatile * const site, Storage const & newValue )
                {
                    Storage oldValue;
                    do {
                        oldValue = *const_cast< Storage * const >( site );
#if defined(WINDOWS_PLATFORM)
                    } while( ! _InterlockedCompareExchange128( site->data, newValue.data[ 1U ], newValue.data[ 0U ],
                        const_cast< int64_t * const >( oldValue.data )));
#elif defined(__GNUC__)
                    } while( ! __sync_bool_compare_and_swap( &site->data, oldValue.data, newValue.data ));
#endif // WINDOWS_PLATFORM
                }
            };

            template< typename TypeT >
            struct Cas
            {
                TOOLS_FORCE_INLINE static Storage const op( Storage volatile * const site, Storage oldValue, Storage const & newValue )
                {
#if defined(WINDOWS_PLATFORM)
                    _InterlockedCompareExchange128( site->data, newValue.data[ 1U ], newValue.data[ 0U ],
                        const_cast< int64_t * const >( oldValue.data ));
                    return oldValue;
#elif defined(__GNUC__)
                    Storage ret;
                    ret.data = __sync_val_compare_and_swap( &site->data, oldValue.data, newValue.data );
                    return ret;
#endif // WINDOWS_PLATFORM
                }
            };

            template< typename TypeT >
            struct Exchange
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const, TypeT const newValue )
                {
                    static_assert( sizeof( TypeT ) == 0, "atomic exchange for 128-bit types unimplemented" );
                    return newValue;
                }
            };

            template< typename TypeT >
            struct Add
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const, TypeT const diff )
                {
                    static_assert( sizeof( TypeT ) == 0, "atomic add for 128-bit types unimplemented" );
                    return diff;
                }
            };

            template< typename TypeT >
            struct Subtract
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const, TypeT const diff )
                {
                    static_assert( sizeof( TypeT ) == 0, "atomic subtract for 128-bit types unimplemented" );
                    return diff;
                }
            };

            template< typename TypeT >
            struct Increment
            {
                TOOLS_FORCE_INLINE static bool const op( TypeT volatile * const )
                {
                    static_assert( sizeof( TypeT ) == 0, "atomic increment for 128-bit types unimplemented" );
                    return false;
                };
            };

            template< typename TypeT >
            struct Decrement
            {
                TOOLS_FORCE_INLINE static bool const op( TypeT volatile * const )
                {
                    static_assert( sizeof( TypeT ) == 0, "atomic decrement for 128-bit types unimplemented" );
                    return false;
                };
            };

            template< typename TypeT >
            struct And
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const, TypeT const mask )
                {
                    static_assert( sizeof( TypeT ) == 0, "atomic AND for 128-bit types unimplemented" );
                    return mask;
                }
            };

            template< typename TypeT >
            struct Or
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT volatile * const, TypeT const mask )
                {
                    static_assert( sizeof( TypeT ) == 0, "atomic OR for 128-bit types unimplemented" );
                    return mask;
                }
            };

            template< typename TypeT >
            struct Swap
            {
                TOOLS_FORCE_INLINE static TypeT const op( TypeT const site )
                {
                    static_assert( sizeof( TypeT ) == 0, "byte swap for 128-bit types unimplemented" );
                    return site;
                }
            };
        };
    };  // impl namespace

    template< typename TypeT >
    TOOLS_FORCE_INLINE typename std::remove_cv< TypeT >::type const atomicRead( TypeT * const site )
    {
        return ::tools::impl::AtomicTrait< sizeof( TypeT ) >::template Read< typename std::remove_cv< TypeT >::type >::op(
            const_cast< typename std::remove_cv< TypeT >::type * >( site ));
    }

    template< typename TypeT, typename OtherT >
    TOOLS_FORCE_INLINE void atomicSet( TypeT volatile * const site, OtherT const newValue )
    {
        ::tools::impl::AtomicTrait< sizeof( TypeT ) >::template Set< typename std::remove_cv< TypeT >::type >::op( site,
            static_cast< TypeT const >( newValue ));
    }

    // Returns the actual old value.
    template< typename TypeT, typename OtherT >
    TOOLS_FORCE_INLINE TypeT const atomicCas( TypeT volatile * const site, OtherT const oldValue, OtherT const newValue = static_cast< OtherT const >( 0 ))
    {
        return ::tools::impl::AtomicTrait< sizeof( TypeT ) >::template Cas< TypeT >::op( site, static_cast< TypeT const >( oldValue ),
            static_cast< TypeT const >( newValue ));
    }
    //template< typename AnyT, typename OtherT, typename SomeT>
    //TOOLS_FORCE_INLINE tools::FlagPointer< AnyT > atomicCas( tools::FlagPointer< AnyT > volatile * const site, OtherT * oldValue, SomeT * newValue ) {
    //    tools::FlagPointer<AnyT> ret;
    //    ret.p_ = *::tools::atomicCas( &site->p_, static_cast< AnyT * >( oldValue ), static_cast< AnyT * >( newValue ));
    //    return ret;
    //}
    template< typename AnyT >
    TOOLS_FORCE_INLINE tools::FlagPointer< AnyT > atomicCas( tools::FlagPointer< AnyT > volatile * const site, tools::FlagPointer< AnyT > oldValue, tools::FlagPointer< AnyT > const & newValue ) {
        oldValue.p_ = ::tools::atomicCas( &site->p_, oldValue.p_, newValue.p_ );
        return oldValue;
    }

    // Returns the old value
    template< typename TypeT, typename OtherT >
    TOOLS_FORCE_INLINE TypeT const atomicExchange( TypeT volatile * const site, OtherT const newValue )
    {
        return ::tools::impl::AtomicTrait< sizeof( TypeT ) >::template Exchange< TypeT >::op( site, static_cast< TypeT const >( newValue ));
    }
    template< typename AnyT >
    TOOLS_FORCE_INLINE tools::FlagPointer< AnyT > atomicExchange( tools::FlagPointer< AnyT > volatile * const site, tools::FlagPointer< AnyT > const & newValue ) {
        tools::FlagPointer< AnyT > ret;
        ret.p_ = atomicExchange( &site->p_, newValue.p_ );
        return ret;
    }

    // Returns the old value
    template< typename TypeT, typename DiffT >
    TOOLS_FORCE_INLINE TypeT atomicAdd( TypeT volatile * const site, DiffT const diff )
    {
        return ::tools::impl::AtomicTrait< sizeof( TypeT ) >::template Add< TypeT >::op( site, static_cast< TypeT const >( diff ));
    }

    // Returns the old value
    template< typename TypeT, typename DiffT >
    TOOLS_FORCE_INLINE TypeT atomicSubtract( TypeT volatile * const site, DiffT const diff )
    {
        return ::tools::impl::AtomicTrait< sizeof( TypeT ) >::template Subtract< TypeT >::op( site, static_cast< TypeT const >( diff ));
    }

    // Returns true iff result is not '0'
    template< typename TypeT >
    TOOLS_FORCE_INLINE bool const atomicIncrement( TypeT volatile * const site )
    {
        return ::tools::impl::AtomicTrait< sizeof( TypeT ) >::template Increment< TypeT >::op( site );
    }

    // Returns true if result is not '0'
    template< typename TypeT >
    TOOLS_FORCE_INLINE bool const atomicDecrement( TypeT volatile * const site )
    {
        return ::tools::impl::AtomicTrait< sizeof( TypeT ) >::template Decrement< TypeT >::op( site );
    }

    // Returns the old value
    template< typename TypeT, typename OtherT >
    TOOLS_FORCE_INLINE TypeT const atomicAnd( TypeT volatile * const site, OtherT const mask )
    {
        return ::tools::impl::AtomicTrait< sizeof( TypeT ) >::template And< TypeT >::op( site, static_cast< TypeT const >( mask ));
    }

    // Returns the old value
    template< typename TypeT, typename OtherT >
    TOOLS_FORCE_INLINE TypeT const atomicOr( TypeT volatile * const site, OtherT const mask )
    {
        return ::tools::impl::AtomicTrait< sizeof( TypeT ) >::template Or< TypeT >::op( site, static_cast< TypeT const >( mask ));
    }

    // Returns the value with all bytes flipped first-to-last
    template< typename TypeT >
    TOOLS_FORCE_INLINE TypeT const byteSwap( TypeT const site )
    {
        return ::tools::impl::AtomicTrait< sizeof( TypeT ) >::template Swap< TypeT >::op( site );
    }

    static TOOLS_FORCE_INLINE void
    atomicRef(
        unsigned volatile * const site )
    {
        TOOLS_ASSERT( *site != 0 );
        TOOLS_ASSERT( ( *site & 0xFF000000U ) == 0 );  // Really?  You hae more then
                                                       // 4.2 billion references?  Or
                                                       // is it more likely something
                                                       // went wrong.
        ::tools::atomicIncrement( site );
    }

    static TOOLS_FORCE_INLINE bool const
    atomicDeref(
        unsigned volatile * const site )
    {
        TOOLS_ASSERT( *site != 0 );
        TOOLS_ASSERT( ( *site & 0xFF000000U ) == 0 );  // Really?  You hae more then
                                                       // 4.2 billion references?  Or
                                                       // is it more likely something
                                                       // went wrong.
        return ::tools::atomicDecrement( site );
    }

    // Continue to compute and try to apply a new value until it takes.
    // The passed in functor should take TypeT (ref, const, const ref)
    // as a parameter and return the new value based on that.
    template< typename TypeT, typename FuncT >
    TOOLS_FORCE_INLINE TypeT atomicUpdate( TypeT volatile * const site, FuncT && f )
    {
        for( ;; ) {
            TypeT prevValue = *site;
            if( ::tools::atomicCas( site, prevValue, static_cast< TypeT const >( f( prevValue ))) == prevValue ) {
                return prevValue;
            }
        }
    }

    // The passed in functor should take a TypeT reference, which
    // on entry contains the previous/current value. The functor
    // should decide if an update should be performed against this
    // value. If so the new value should be placed into the parameter
    // and the functor should return true. If not, the functor should
    // return false. The second variation returns the old/previous
    // value.
    template< typename TypeT, typename FuncT >
    TOOLS_FORCE_INLINE bool const atomicTryUpdate( TypeT volatile * const site, FuncT && f )
    {
        for( ;; ) {
            TypeT prevValue = *site;
            TypeT nextValue = prevValue;
            bool ret = f( &nextValue );
            if( ret ) {
                if( ::tools::atomicCas( site, prevValue, nextValue ) != prevValue ) {
                    continue;
                }
            }
            return ret;
        }
    }
    template< typename AnyT, typename FuncT >
    TOOLS_FORCE_INLINE AnyT * const atomicUpdate( tools::FlagPointer< AnyT > volatile * const site, FuncT && f )
    {
        tools::FlagPointer< AnyT > nextValue;
        for( ;; ) {
            tools::FlagPointer< AnyT > prevValue( site->p_ );
            nextValue.reset( f( prevValue ));
            if( ::tools::atomicCas( &site->p_, prevValue.p_, nextValue.p_ ) == prevValue.p_ ) {
                return prevValue.p_;
            }
        }
    }
    template< typename AnyT, typename FuncT >
    TOOLS_FORCE_INLINE bool const atomicTryUpdate( tools::FlagPointer< AnyT > volatile * const site, FuncT && f )
    {
        // f should be of the form 'bool f(FlagPointer< AnyT > &)'
        tools::FlagPointer< AnyT > prev;
        tools::FlagPointer< AnyT > next;
        for( ;; ) {
            prev.p_ = site->p_;
            next.p_ = prev.p_;
            bool ret = f( next );
            if( ret ) {
                if( ::tools::atomicCas( &site->p_, prev.p_, next.p_ ) != prev.p_ ) {
                    continue;
                }
            }
            return ret;
        }
    }

    // Atomically push an element to the front of a singley linked list, returning the previous front.
    // There is not matching atomicPop(...), as this is likely to have CAS ABA problems. The safest way
    // to pop from a list is to atomicSwap(...) the entire list, then pop from that list which is now
    // local.
    template< typename TypeT >
    TOOLS_FORCE_INLINE TypeT * /*const*/ atomicPush( TypeT * volatile * /*const*/ site, TypeT * /*const*/ element, TypeT * TypeT::* /*const*/ next ) {
        return atomicUpdate( site, [ element, next ]( TypeT * previous )->TypeT * {
            ( element->*next ) = previous;
            return element;
        });
    }

    template< typename ItemT >
    struct atomicPointer
    {
        atomicPointer( void )
            : pointer_( nullptr )
        {}
        ~atomicPointer( void )
        {
            delete pointer_;
        }
        bool
        operator!( void ) const
        {
            return !pointer_;
        }
        ItemT *
        operator->( void ) const
        {
            return pointer_;
        }
        ItemT &
        operator*( void ) const
        {
            return *pointer_;
        }
        ItemT *
        compareAndSwap(
            ItemT * oldValue,
            ItemT * newValue = nullptr )
        {
            return tools::atomicCas( &pointer_, oldValue, newValue );
        }
        ItemT *
        get( void ) const
        {
            return pointer_;
        }
    private:
        ItemT * volatile pointer_;
    };

    template< typename AnyT, bool understandSlow = false >
    struct AtomicAny
    {
        typedef typename ::tools::impl::AtomicTrait< ::tools::impl::AtomicPowerOf2< sizeof( AnyT ) >::power_ > Trait;
        typedef typename Trait::Storage Storage;

        template< typename UserT, typename StorageT, bool is128 >
        union Pair;

        template< typename UserT, typename StorageT >
        union Pair< UserT, StorageT, false >
        {
            UserT any_;
            StorageT bits_;

            explicit TOOLS_FORCE_INLINE Pair( void )
                : bits_( 0U )
            {}

            explicit TOOLS_FORCE_INLINE Pair( StorageT const v )
                : bits_( v )
            {}

            explicit TOOLS_FORCE_INLINE Pair( UserT const & v )
                : any_( v )
            {}

            explicit TOOLS_FORCE_INLINE Pair( Pair const & v )
                : bits_( v.bits_ )
            {}

            TOOLS_FORCE_INLINE Pair & operator=( UserT const & v )
            {
                any_ = v;
                return *this;
            }
        };

        template< typename UserT, typename StorageT >
        union Pair< UserT, StorageT, true >
        {
            UserT any_;
            StorageT bits_;

            explicit TOOLS_FORCE_INLINE Pair( void )
            {
                bits_.init();
            }

            explicit TOOLS_FORCE_INLINE Pair( StorageT const & v )
                : bits_( v )
            {}

            explicit TOOLS_FORCE_INLINE Pair( UserT const & v )
                : any_( v )
            {}

            explicit TOOLS_FORCE_INLINE Pair( Pair const & v )
                : bits_( v.bits_ )
            {}

            TOOLS_FORCE_INLINE Pair & operator=( UserT const & v )
            {
                any_ = v;
                return *this;
            }
        };

        template< typename SomeT >
        union Pair< SomeT, SomeT, false >
        {
            SomeT any_, bits_;

            explicit TOOLS_FORCE_INLINE Pair( void )
                : bits_( 0U )
            {}

            explicit TOOLS_FORCE_INLINE Pair( SomeT const v )
                : bits_( v )
            {}

            explicit TOOLS_FORCE_INLINE Pair( Pair const & v )
                : bits_( v.bits_ )
            {}

            TOOLS_FORCE_INLINE Pair & operator =( SomeT const v )
            {
                bits_ = v;
                return *this;
            }
        };

        typedef Pair< typename std::remove_cv< AnyT >::type, Storage, Trait::width_ == 16 > PairT;

        static_assert( sizeof( PairT ) <= 16, "AnyT must have a containing atomic size (including padding)" );

        TOOLS_FORCE_INLINE AtomicAny( void )
            : any_()
        {}
        TOOLS_FORCE_INLINE AtomicAny( AnyT const & c )
            : any_( c )
        {}
        TOOLS_FORCE_INLINE AnyT const localRead( void ) const
        {
            return PairT( ::tools::atomicRead( &any_.bits_ )).any_;
        }
        TOOLS_FORCE_INLINE void localSet( AnyT const & s )
        {
            ::tools::atomicSet( &any_.bits_, PairT( s ).bits_ );
        }
        template< typename FuncT >
        TOOLS_FORCE_INLINE AnyT const localUpdate( FuncT && f )
        {
            static_assert( ( Trait::width_ < 16U ) || understandSlow, "128-bit atomic updates using atomicRead may be slower than expected" );
            PairT nextValue;
            for( ;; ) {
                PairT prevValue( ::tools::atomicRead( &any_.bits_ ));
                nextValue = f( prevValue.any_ );
                if( ::tools::atomicCas( &any_.bits_, prevValue.bits_, nextValue.bits_ ) == prevValue.bits_ ) {
                    return prevValue.any_;
                }
            }
        }
        template< typename FuncT >
        TOOLS_FORCE_INLINE AnyT const localUpdateUnsafe( FuncT && f )
        {
            PairT nextValue;
            for( ;; ) {
                PairT prevValue( &any_.bits_ );
                nextValue = f( prevValue.any_ );
                if( ::tools::atomicCas( &any_.bits_, prevValue.bits_, nextValue.bits_ ) == prevValue.bits_ ) {
                    return prevValue.any_;
                }
            }
        }
        template< typename FuncT >
        TOOLS_FORCE_INLINE bool const localTryUpdate( FuncT && f )
        {
            static_assert( ( Trait::width_ < 16U ) || understandSlow, "128-bit atomic updates using atomicRead may be slower than expected" );
            for( ;; ) {
                PairT prevValue( ::tools::atomicRead( &any_.bits_ ));
                PairT nextValue( prevValue );
                bool ret = f( &nextValue.any_ );
                if( ret ) {
                    if( ::tools::atomicCas( &any_.bits_, prevValue.bits_, nextValue.bits_ ) != prevValue.bits_ ) {
                        continue;
                    }
                }
                return ret;
            }
        }
        template< typename FuncT >
        TOOLS_FORCE_INLINE bool const localTryUpdateUnsafe( FuncT && f )
        {
            for( ;; ) {
                PairT prevValue( &any_.bits_ );
                PairT nextValue( prevValue );
                bool ret = f( &nextValue.any_ );
                if( ret ) {
                    if( ::tools::atomicCas( &any_.bits_, prevValue.bits_, nextValue.bits_ ) != prevValue.bits_ ) {
                        continue;
                    }
                }
                return ret;
            }
        }

        PairT any_;
    };

    template< typename AnyT, bool understandV >
    TOOLS_FORCE_INLINE AnyT const atomicRead( AtomicAny< AnyT, understandV > * const site )
    {
        return site->localRead();
    }

    template< typename AnyT, bool understandV >
    TOOLS_FORCE_INLINE AnyT const atomicRead( AtomicAny< AnyT, understandV > const * const site )
    {
        return site->localRead();
    }

    template< typename AnyT, typename OtherT, bool understandV >
    TOOLS_FORCE_INLINE void atomicSet( AtomicAny< AnyT, understandV > * const site, OtherT const & newValue )
    {
        site->localSet( newValue );
    }

    template< typename AnyT, typename FuncT, bool understandV >
    TOOLS_FORCE_INLINE AnyT const atomicUpdate( AtomicAny< AnyT, understandV > * const site, FuncT && func )
    {
        return site->localUpdate( std::forward< FuncT >( func ));
    }

    template< typename AnyT, typename FuncT, bool understandV >
    TOOLS_FORCE_INLINE AnyT const atomicUpdateUnsafe( AtomicAny< AnyT, understandV > * const site, FuncT && func )
    {
        return site->localUpdateUnsafe( std::forward< FuncT >( func ));
    }

    template< typename AnyT, typename FuncT, bool understandV >
    TOOLS_FORCE_INLINE bool const atomicTryUpdate( AtomicAny< AnyT, understandV > * const site, FuncT && func )
    {
        return site->localTryUpdate( std::forward< FuncT >( func ));
    }

    template< typename AnyT, typename FuncT, bool understandV >
    TOOLS_FORCE_INLINE bool const atomicTryUpdateUnsafe( AtomicAny< AnyT, understandV > * const site, FuncT && func )
    {
        return site->localTryUpdateUnsafe( std::forward< FuncT >( func ));
    }

    ///
    // Referenced is the minimal interface of an object that maintains a reference count.
    // When the reference count transitions to 0 the object can delete itself.  When a
    // particular user releases it's handle that user should not access the object any more.
    //
    // This template is meant to be used as a base type for the 'host' type that is to be
    // refable.
    template< typename HostT >
    struct Referenced
    {
        struct Reference
            : HostT
            , tools::Disposable
        {};

        virtual tools::AutoDispose< Reference > ref( void ) const throw() = 0;
    };

    ///
    // A standard implementation of Referenced that will delete 'this' (cast to ImplT) when
    // the reference count transitions to 0.
    template< typename ImplementationT, typename ReferenceT,
        typename AllocT = tools::AllocStatic<> >
    struct StandardReferenced
        : ReferenceT::Reference
        , AllocT
    {
        StandardReferenced( void )
            : refs_( 1 )
        {}
        ~StandardReferenced( void )
        {
            TOOLS_ASSERT( refs_ == 0 );
        }
        void
        dispose( void )
        {
            if( !atomicDeref( &refs_ )) {
                delete static_cast< ImplementationT const * >( this );
            }
        }
        tools::AutoDispose< typename ReferenceT::Reference >
        ref( void ) const throw()
        {
            atomicRef( &refs_ );
            return static_cast< typename ReferenceT::Reference * >(
                static_cast< ImplementationT * >(
                const_cast< StandardReferenced * >( this )));
        }
    protected:
        unsigned mutable volatile refs_;
    private:
        // Cannot be copied or assigned.
        StandardReferenced( StandardReferenced const & ) {}
        StandardReferenced & operator=( StandardReferenced const & ) { return *this; }
    };
    template< typename ImplementationT, typename InterfaceT,
        typename AllocT = tools::AllocStatic<> >
    struct StandardIsReferenced
        : tools::StandardReferenced< ImplementationT, tools::Referenced< InterfaceT >,
            AllocT >
    {};
    ///
    // A standard implementation of Referenced that does not delete itself when the reference goes
    // to 0.  This is useful for static and stack-based instances.
    template< typename ImplementationT, typename ReferenceT >
    struct StandardStaticReferenced
        : ReferenceT::Reference
        , boost::noncopyable
    {
        StandardStaticReferenced( void )
            : refs_( 1 )
        {}
        ~StandardStaticReferenced( void )
        {
            TOOLS_ASSERT( refs_ == 0 );
        }
        void
        dispose( void )
        {
            tools::atomicDecrement( &refs_ );
        }
        tools::AutoDispose< typename ReferenceT::Reference >
        ref( void ) const throw()
        {
            tools::atomicIncrement( &refs_ );
            return static_cast< typename ReferenceT::Reference * >(
                static_cast< ImplementationT * >(
                const_cast< StandardStaticReferenced * >( this )));
        }
    protected:
        unsigned mutable volatile refs_;
    };

    // A mutual exclusion lock of otherwise unspecified type
    struct Monitor
        : Disposable
    {
        enum Policy {
            PolicyStrict, // Real-time threads are not allowed to aquire these locks (default).
            PolicyAllowRt, // Real-time threads are allowed to aquire these locks. However, real-time and non-real-time threads are not allowed to aquire the same lock. This is to prevent priority inversion.
            PolicyAllowPriorityInversion // Any thread may aquire these locks.
        };

        // Enter the excusion/lock.  This returns a Disposable that will exit
        // the exclusion/lock when disposed.  If tryOnly is true, enter may
        // return nullptr if the thread cannot immediately enter the exclusion/lock.
        virtual AutoDispose<> enter( impl::ResourceSample const &, bool tryOnly = false ) = 0;

        // Check if this monitor is aquired by the current thread. This may not be implemented by all
        // implementations, and may sometimes return false positives. Use at your own risk.
        virtual bool isAquired( void ) = 0;

        AutoDispose<> enter( bool tryOnly = false ) {
            return enter( TOOLS_RESOURCE_SAMPLE_CALLER( 0 ), tryOnly );
        }
    };

    // A reader-writer lock
    struct RwMonitor
        : Monitor
    {
        // Enter the lock in read mode.  Write mode will use the Monitor interface.
        virtual AutoDispose<> enterShared( impl::ResourceSample const &, bool tryOnly = false ) = 0;

        AutoDispose<> enterShared( bool tryOnly = false ) {
            return enterShared( TOOLS_RESOURCE_SAMPLE_CALLER( 0 ), tryOnly );
        }
    };

    // An event.
    struct Event
        : Disposable
    {
        // Wait until the event is posted.
        virtual void wait( void ) = 0;
        // Signal that the event has triggered
        virtual void post( void ) = 0;
    };

    // A condition variable.
    struct ConditionVar
        : Disposable
    {
        // Create a Monitor bound to this condition variable.  This, or one of its
        // peers, must be engaged before the condition variable may be signalled.
        // ConditionVar monitors are always level 0 and cannot be nested.
        virtual AutoDispose< Monitor > monitorNew( impl::ResourceSample const & ) = 0;
        // Wait at most until the condition variable is signalled.  The active
        // monitor will be released while waiting and re-entered before returning.
        virtual void wait( void ) = 0;
        // Signal at least one waiting monitor; if all is true, signal all monitors
        // currently waiting on this condition variable.  To prevent busy wakeups,
        // it's best if no monitor is held while calling signal.
        virtual void signal( bool all = false ) = 0;

        AutoDispose< Monitor > monitorNew( void ) {
            return monitorNew( TOOLS_RESOURCE_SAMPLE_CALLER( 0 ) );
        }
    };

    // Create a new simple monitor.  The level limits the monitors that can be concurrently
    // entered.  A level 0 monitor means it is the only monitor that may be active on the
    // current thread.  Monitors are not re-enterant.
    TOOLS_API AutoDispose< Monitor > monitorNew( impl::ResourceSample const &, unsigned level = 0U, tools::Monitor::Policy policy = tools::Monitor::PolicyStrict );
    // Create a new monitor from a pool, based on a specific container object address.
    // The monitor is shared and level 0, however there's a lot of them so the possibility
    // of accidental contention is low on average.
    TOOLS_API AutoDispose< Monitor > monitorPoolNew( void * );
    // A light weight monitor that still reports contention, though without the dependencies that make
    // a fully tracked monitor expensive.
    TOOLS_API AutoDispose< Monitor > monitorStaticNew( impl::ResourceSample const &, StringId const & = tools::StringIdEmpty(), Monitor::Policy = Monitor::PolicyStrict );

    inline AutoDispose< Monitor > monitorNew( unsigned level = 0U )
    {
        return tools::monitorNew( TOOLS_RESOURCE_SAMPLE_CALLER( level ), level );
    }

    // Create a simple read-write monitor using thread-local read monitors.  The RwMonitor
    // may use a range of levels within, thus the supplied level will be the lower bound.
    // The upper bound isn't specified and is not under caller control.  When taken in
    // shared mode, the effective level may be higher.
    TOOLS_API AutoDispose< RwMonitor > rwMonitorNew( impl::ResourceSample const &, Monitor::Policy );

    inline AutoDispose< RwMonitor > rwMonitorNew( Monitor::Policy policy = Monitor::PolicyStrict ) {
        return rwMonitorNew( TOOLS_RESOURCE_SAMPLE_CALLER( 0 ), policy );
    }

    inline AutoDispose< Monitor > monitorStaticNew( StringId const & stereotype = tools::StringIdEmpty(), Monitor::Policy policy = Monitor::PolicyStrict ) {
        return monitorStaticNew( TOOLS_RESOURCE_SAMPLE_CALLER( 0 ), stereotype, policy );
    }

    // Create a simple event.
    TOOLS_API AutoDispose< Event > eventNew( impl::ResourceSample const & );

    inline AutoDispose< Event > eventNew( void ) {
        return eventNew( TOOLS_RESOURCE_SAMPLE_CALLER( 0 ));
    }

    // Create a simple condition variable.
    TOOLS_API AutoDispose< ConditionVar > conditionVarNew( impl::ResourceSample const & );

    inline AutoDispose< ConditionVar > conditionVarNew( void ) {
        return conditionVarNew( TOOLS_RESOURCE_SAMPLE_CALLER( 0 ) );
    }

	// TODO: move this into a project private header
    namespace impl
    {
        struct ConditionVarLock
        {
            // Current condition variable
            ConditionVar * cvar_;
            // Active monitor
            Monitor * monitor_;
            // Active lock
            Disposable * lock_;
        };

        struct HungThreadDetector
            : Disposable
        {
            virtual void arm( void ) = 0;
            virtual void disarm( void ) = 0;
            virtual bool enabled( void ) = 0;
            virtual void noteExecBegin( uint64 ) = 0;
            virtual void noteExecFinish( void ) = 0;
            virtual void timerFire( uint64 ) = 0;
        };

        TOOLS_API AutoDispose< HungThreadDetector > platformHungThreadDetectorNew( StringId const &, uint64, uint64, uint64 );
    };  // impl namespace

    namespace detail
    {
        TOOLS_API bool setThreadIsRealtime( bool = true );
        TOOLS_API bool threadIsRealtime( void );
    };  // detail namespace
}; // namespace tools
