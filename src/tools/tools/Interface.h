#pragma once

#include <tools/Meta.h>
#include <tools/String.h>
#include <tools/Tools.h>

#include <boost/mpl/as_sequence.hpp>
#include <boost/mpl/deref.hpp>
#include <boost/mpl/empty_sequence.hpp>
#include <boost/mpl/list.hpp>
#include <boost/mpl/next.hpp>
#include <memory>
#include <typeinfo>
#include <type_traits>

namespace tools {
    ///
    // This is a standard base class for discoverable interfaces at runtime. It contains
    // a getInterface function and a type-safe template helper that deduces the interface
    // name from the type.
    struct Unknown
    {
        ///
        // Return an interface by name - the name is identified by the TOOLS_TYPE_NAME macro. This
        // function returns nullptr if the interface is not found.
        virtual void * getInterface( StringId const & ) const throw() = 0;

        ///
        // Typed version of getInterface - returns nullptr if the interface was found.
        template< typename InterfaceT >
        inline InterfaceT * getInterface( void ) const throw()
        {
            return static_cast< InterfaceT * >( this->getInterface( nameOf< InterfaceT >() ) );
        }
    };

    ///
    // Disposable is the minimal object that can be disposed at some point in the future.
    // Unlike Referenced, this will not typically be used as a base class but as a simple
    // handle for something that can be disposed.  That is, this is a form of lifetime control.
    struct Disposable
    {
        virtual void dispose( void ) = 0;
    };

    ///
    // Given an interface, state that it is also disposable.  It is generally preferened
    // not to give peopel a lifetime controlling interface unless they legitimately can
    // make a statement about the lifetime of that object.  So this makes it slightly
    // easier to talk about those two aspects of a type.
    template< typename InterfaceT >
    struct IsDisposable
        : InterfaceT
        , tools::Disposable
    {};

    ///
    // Standard smart-pointer for Disposable items.  This is fairly compatable with
    // unique_ptr semantics.
    template<typename TypeT = tools::Disposable>
    struct AutoDispose
    {
        typedef TypeT * PointerT;
		class DisposeForbidden
			: public TypeT
		{
			template< typename SomeT, typename OtherT >
			DisposeForbidden(SomeT && param1, OtherT && param2)
				: TypeT(a, b)
			{} // This is here to prevent generating default constructor calls.

			   // Declaring this here (private) prevents people who have this type from calling it when not
			   // appropriate.
			virtual void dispose(void) = 0;
		};

		TOOLS_FORCE_INLINE AutoDispose(void) throw() : p_(nullptr) {}
		TOOLS_FORCE_INLINE AutoDispose(nullptr_t) throw() : p_(nullptr) {}
		template<typename AnyT>
		explicit TOOLS_FORCE_INLINE AutoDispose(AnyT * const & p) throw()
			: p_(p)
		{
			static_assert(std::is_base_of<TypeT, AnyT>::value, "AnyT must derive from TypeT");
		}
		template<typename AnyT>
		explicit TOOLS_FORCE_INLINE AutoDispose(AnyT * volatile const & p) throw()
			: p_(p)
		{
			static_assert(std::is_base_of<TypeT, AnyT>::value, "AnyT must derive from TypeT");
		}
		// This is different from the 'AnyT *' constructors because we wish to allow implicit conversion
		// from r-values. This makes sense as there can be no confusion of ownership in this case.
		template<typename AnyT>
		TOOLS_FORCE_INLINE AutoDispose(AnyT * && p) throw()
			: p_(p)
		{
			static_assert(std::is_base_of<TypeT, AnyT>::value, "AnyT must derive from TypeT");
		}
		template<typename AnyT>
		TOOLS_FORCE_INLINE AutoDispose(AnyT * const && p) throw()
			: p_(p)
		{
			static_assert(std::is_base_of<TypeT, AnyT>::value, "AnyT must derive from TypeT");
		}
		template<typename AnyT>
		TOOLS_FORCE_INLINE AutoDispose(AutoDispose<AnyT> && r) throw()
			: p_(r.p_)
		{
			static_assert(std::is_base_of<TypeT, AnyT>::value, "Contents of AutoDispose<AnyT> must derive from TypeT");
			// Move the contents
			r.p_ = nullptr;
		}
        ~AutoDispose(void) throw() {
            if( !!p_ ) {
                p_->dispose();
            }
#ifdef TOOLS_DEBUG
            // Ensure we crash if we try to access this after dispose.
            p_ = reinterpret_cast<PointerT>(static_cast<ptrdiff_t>(-1));
#endif // TOOLS_DEBUG
        }
		template<typename AnyT>
		TOOLS_FORCE_INLINE AutoDispose<TypeT> & operator=(AutoDispose<AnyT> && r) throw() {
			static_assert(std::is_base_of<TypeT, AnyT>::value, "Contents of AutoDipose<AnyT> must derive from TypeT");
			// This order of operations is specifically designed to naturally work in the face of assignment to self
			// or reenterancy on dispose.
			PointerT next = r.p_;
			r.p_ = nullptr;
			PointerT prev = this->p_;
			this->p_ = next;
			if (!!prev) {
				prev->dispose();
			}
			return *this;
		}
		TOOLS_FORCE_INLINE AutoDispose<TypeT> & operator=(nullptr_t) throw() {
			return operator=(AutoDispose<TypeT>());
		}
		template<typename AnyT>
		TOOLS_FORCE_INLINE AutoDispose<TypeT> & operator=(AnyT * const r) throw() {
			static_assert(std::is_base_of<TypeT, AnyT>::value, "AnyT must derive from TypeT");
			return operator=(AutoDispose<AnyT>(r));
		}
		template<typename AnyT>
		TOOLS_FORCE_INLINE bool operator==(AutoDispose<AnyT> const & r) const throw() {
			return (p_ == r.p_);
		}
		template<typename AnyT>
		TOOLS_FORCE_INLINE bool operator==(AnyT * const r) const throw() {
			return (p_ == r);
		}
		TOOLS_FORCE_INLINE bool operator==(nullptr_t) const throw() {
			return (p_ == nullptr);
		}
		TOOLS_FORCE_INLINE bool operator!(void) const throw() {
			return !p_;
		}
		TOOLS_FORCE_INLINE explicit operator bool(void) const throw() {
			return (p_ != nullptr);
		}
		TOOLS_FORCE_INLINE DisposeForbidden * operator->(void) const throw() {
			TOOLS_ASSERT(!!p_);
			return static_cast<DisposeForbidden *>(p_);
		}
		TOOLS_FORCE_INLINE TypeT & operator*(void) const throw() {
			TOOLS_ASSERT(!!p_);
			return *p_;
		}
		TOOLS_FORCE_INLINE void swap(AutoDispose< TypeT > & r) throw() {
			PointerT t = r.p_;
			r.p_ = p_;
			p_ = t;
		}
		TOOLS_FORCE_INLINE void swap(AutoDispose< TypeT > && r) throw() {
			PointerT t = r.p_;
			r.p_ = p_;
			p_ = t;
		}
		TOOLS_FORCE_INLINE PointerT get(void) const throw() {
			return p_;
		}
		TOOLS_FORCE_INLINE PointerT reset(void) {
            // This is structured to be safe in the face of reenterance on dispose.
			swap(AutoDispose<TypeT>());
		}
		TOOLS_FORCE_INLINE PointerT release(void) throw() {
			TOOLS_ASSERT(!!p_);
			PointerT p = p_;
			p_ = PointerT();
			return p;
		}
		TOOLS_FORCE_INLINE PointerT * getAddress(void) throw() {
			reset();
			return &p_;
		}
		TOOLS_FORCE_INLINE PointerT * atomicAccess(void) throw() {  // I hate that this exists
			return &p_;
		}
		//TOOLS_FORCE_INLINE void atomicSwap( AutoDispose< TypeT > & r ) {
        //    PointerT t = r.p_;
        //    r.p_ = tools::atomicExchange< PointerT >( &p_, t );
        //}
        //TOOLS_FORCE_INLINE bool atomicSwapIf( AutoDispose< TypeT > & r, PointerT o ) {
        //    PointerT n = r.p_;
        //    PointerT t = tools::atomicCas< PointerT >( &p_, o, n );
        //    if( t == o ) {
        //        r.p_ = t;
        //        return true;
        //    }
        //    return false;
        //}
        //TOOLS_FORCE_INLINE PointerT atomicRead( void ) const {
        //    return tools::atomicRead< PointerT >( &p_ );
        //}

		// These are only allowed in the trivial case.
        //AutoDispose( AutoDispose< TypeT > const & r ) {
        //    TOOLS_ASSERT( r.p_ == PointerT() );
        //    p_ = Type();
        //}
        //TOOLS_FORCE_INLINE AutoDispose< TypeT > & operator=( AutoDispose< TypeT > const & r ) {
        //    TOOLS_ASSERT( r.p_ == nullptr );
        //    reset();
        //    return *this;
        //}
		AutoDispose(AutoDispose<TypeT> const &) = delete;
		AutoDispose<TypeT> & operator=(AutoDispose<TypeT> const &) = delete;
    protected:
		// The following is needed to allow implicit conversion when moving from AutoDispose<derived> to
		// AutoDispose<base> without breaking encapsulation by introducing public helper methods.
		template<typename AnyT>
		friend struct AutoDispose;

        PointerT p_;
    };

    template< typename TypeT >
    inline bool isEnd( tools::AutoDispose< TypeT > const & ad ) {
        return !ad;
    }

    template< typename TypeT >
    inline void setEnd( tools::AutoDispose< TypeT > & ad ) {
        ad = nullptr;
    }

    TOOLS_API AutoDispose<> nullDisposable( void );

    template<typename TypeT>
    struct NoDispose
    {
        typedef typename tools::AutoDispose<TypeT>::DisposeForbidden DisposeForbidden;

        TOOLS_FORCE_INLINE NoDispose(void) : p_(nullptr) {}
        TOOLS_FORCE_INLINE NoDispose(nullptr_t) : p_(nullptr) {}
        TOOLS_FORCE_INLINE NoDispose(TypeT & p)
            : p_(&p)
        {}
        TOOLS_FORCE_INLINE NoDispose(TypeT volatile & p)
            : p_(&p)
        {}
        template<typename AnyT>
        TOOLS_FORCE_INLINE NoDispose(AutoDispose<AnyT> const & r)
            : p_(r.get())
        {
            static_assert(std::is_base_of<TypeT, AnyT>::value, "AnyT must derive from TypeT");
        }
        template<typename AnyT>
        TOOLS_FORCE_INLINE NoDispose(NoDispose<AnyT> const & r)
            : p_(r.p_)
        {
            static_assert(std::is_base_of<TypeT, AnyT>::value, "AnyT must derive from TypeT");
        }
        TOOLS_FORCE_INLINE NoDispose<TypeT> & operator=(nullptr_t) {
            p_ = nullptr;
            return *this;
        }
        TOOLS_FORCE_INLINE NoDispose<TypeT> & operator=(TypeT & p) {
            p_ = &p;
            return *this;
        }
        TOOLS_FORCE_INLINE NoDispose<TypeT> & operator=(TypeT volatile & p) {
            p_ = &p;
            return *this;
        }
        template<typename AnyT>
        TOOLS_FORCE_INLINE NoDispose<TypeT> & operator=(AutoDispose<AnyT> const & r) {
            static_assert(std::is_base_of<TypeT, AnyT>::value, "AnyT must derive from TypeT");
            p_ = r.get();
            return *this;
        }
        template<typename AnyT>
        TOOLS_FORCE_INLINE NoDispose<TypeT> & operator=(NoDispose<AnyT> const & r) {
            static_assert(std::is_base_of<TypeT, AnyT>::value, "AnyT must derive from TypeT");
            p_ = r.p_;
            return *this;
        }
        template<typename AnyT>
        TOOLS_FORCE_INLINE bool operator==(AnyT const * r) const {
            static_assert(std::is_base_of<TypeT, AnyT>::value, "AnyT must derive from TypeT");
            return (p_ == static_cast<TypeT const *>(r));
        }
        template<typename AnyT>
        TOOLS_FORCE_INLINE bool operator==(AutoDispose<AnyT> const & r) const {
            static_assert(std::is_base_of<TypeT, AnyT>::value, "AnyT must derive from TypeT");
            return (p_ == static_cast<TypeT *>(r.get()));
        }
        template<typename AnyT>
        TOOLS_FORCE_INLINE bool operator==(NoDispose<AnyT> const & r) const {
            static_assert(std::is_base_of<TypeT, AnyT>::value, "AnyT must derive from TypeT");
            return (p_ == static_cast<TypeT *>(r.p_));
        }
        template<typename AnyT>
        TOOLS_FORCE_INLINE operator AnyT *(void) const {
            static_assert(std::is_base_of<TypeT, AnyT>::value, "AnyT must derive from TypeT");
            return static_cast<DisposeForbidden *>(p_);
        }
        TOOLS_FORCE_INLINE DisposeForbidden * operator->(void) const {
            return static_cast<DisposeForbidden *>(p_);
        }
        TOOLS_FORCE_INLINE bool operator!(void) const {
            return !p_;
        }
        TOOLS_FORCE_INLINE TypeT & operator*(void) const {
            return *static_cast<DisposeForbidden *>(p_);
        }

    protected:
        // The following is needed to allow implicit conversion when moving from AutoDispose<derived> to
        // AutoDispose<base> without breaking encapsulation by introducing public helper methods.
        template<typename AnyT>
        friend struct NoDispose;

        TypeT * p_;
    };

    // An AutoDisposePair couples a pointer to a thing and an external lifetime control for that thing.
    template< typename InterfaceT, typename AutoDispT = tools::AutoDispose<> >
    struct AutoDisposePair
    {
        typedef InterfaceT * PointerT;
        typedef InterfaceT & ReferenceT;
        typedef tools::AutoDisposePair< InterfaceT, AutoDispT > ThisT;

        AutoDisposePair( void ) : p_( nullptr ) {}
        AutoDisposePair( ReferenceT p, AutoDispT && life )
            : p_( &p )
            , lifetime_( std::move( life ))
        {
            TOOLS_ASSERT( !!p_ == !!lifetime_ );
        }
        AutoDisposePair( ReferenceT p, typename AutoDispT::PointerT life )
            : p_( &p )
            , lifetime_( life )
        {
            TOOLS_ASSERT( !!p_ == !!lifetime_ );
        }
        AutoDisposePair( ReferenceT p, AutoDispT & life )
            : p_( &p )
            , lifetime_( std::move( life ))
        {
            TOOLS_ASSERT( !!p_ == !!lifetime_ );
        }
        template< typename OtherT >
        AutoDisposePair( OtherT && other )
            : p_( other )
            , lifetime_( std::move( other ))
        {
            TOOLS_ASSERT( !!p_ == !!lifetime_ );
        }
        AutoDisposePair( ThisT && r )
            : p_( r.p_ )
            , lifetime_( std::move( r.lifetime_ ))
        {
            r.p_ = nullptr;
            TOOLS_ASSERT( !!p_ == !!lifetime_ );
            TOOLS_ASSERT( !!r.p_ == !!r.lifetime_ );
        }
        // only 'copy' empty AutoDisposePairs<>
        AutoDisposePair( ThisT const & r )
            : p_( nullptr )
        {
            TOOLS_ASSERT( !r.p_ );
        }
        ~AutoDisposePair( void )
        {
#ifdef TOOLS_DEBUG
            // poison the pointer to make a crash if it gets used
            p_ = reinterpret_cast< InterfaceT * >( static_cast< ptrdiff_t >( -1 ));
#endif // TOOLS_DEBUG
        }
        TOOLS_FORCE_INLINE PointerT operator->( void ) const
        {
            TOOLS_ASSERT( !!p_ );
            return p_;
        }
        TOOLS_FORCE_INLINE ReferenceT operator*( void ) const
        {
            TOOLS_ASSERT( !!p_ );
            return *p_;
        }
        AutoDispT const & lifetime( void ) const
        {
            TOOLS_ASSERT( !!p_ );
            return lifetime_;
        }
        void swap( ThisT & r )
        {
            PointerT t = r.p_;
            r.p_ = p_;
            p_ = t;
            AutoDispT tl( std::move( r.lifetime_ ));
            r.lifetime_ = std::move( lifetime_ );
            lifetime_ = std::move( tl );
        }
        void swap( ThisT && r )
        {
            PointerT t = r.p_;
            r.p_ = p_;
            p_ = t;
            AutoDispT tl( std::move( r.lifetime_ ));
            r.lifetime_ = std::move( lifetime_ );
            lifetime_ = std::move( tl );
        }
        TOOLS_FORCE_INLINE PointerT get( void ) const
        {
            return p_;
        }
        TOOLS_FORCE_INLINE bool operator!( void ) const
        {
            return !p_;
        }
        TOOLS_FORCE_INLINE void reset( void )
        {
            p_ = nullptr;
            lifetime_.reset();
        }
        TOOLS_FORCE_INLINE void reset( ReferenceT p, AutoDispT && life )
        {
            TOOLS_ASSERT( !!life );
            p_ = &p;
            lifetime_ = std::move( life );
        }
        TOOLS_FORCE_INLINE void reset( ReferenceT p, typename AutoDispT::PointerT life )
        {
            TOOLS_ASSERT( !!life );
            p_ = &p;
            lifetime_.reset( life );
        }
        TOOLS_FORCE_INLINE ThisT & operator=( ThisT && r )
        {
            PointerT t = r.p_;
            r.p_ = nullptr;
            p_ = t;
            lifetime_ = std::move( r.lifetime_ );
            return *this;
        }
        TOOLS_FORCE_INLINE ThisT & operator=( ThisT const & r )
        {
            TOOLS_ASSERT( !r );
            reset();
            return *this;
        }
        PointerT release( AutoDispT * tgt )
        {
            TOOLS_ASSERT( !!p_ && !!lifetime_ );
            PointerT p = p_;
            p_ = PointerT();
            *tgt = std::move( lifetime_ );
            return p;
        }
        template< typename OtherPtrT >
        bool operator==( AutoDisposePair< InterfaceT, OtherPtrT > const & r ) const
        {
            return ( p_ == r.p_ );
        }
    private:
        PointerT p_;
        AutoDispT lifetime_;
    };

    template< typename InterfaceT, typename AutoDispT >
    inline bool isEnd( tools::AutoDisposePair< InterfaceT, AutoDispT > const & adp ) {
        return !adp;
    }

    template< typename InterfaceT, typename AutoDispT >
    inline void setEnd( tools::AutoDisposePair< InterfaceT, AutoDispT > & adp ) {
        adp.release();
    }

    // A flagged pointer has a special sentinel value when it is isEnd(), otherwise it is just a pointer.
    template< typename AnyT >
    struct FlagPointer
    {
        AnyT * operator->( void ) const volatile {
            return reinterpret_cast< AnyT * >( reinterpret_cast< ptrdiff_t >( p_ ) & ~static_cast< ptrdiff_t >( 1 ));
        }
        AnyT & operator*( void ) const volatile {
            return *(operator->());
        }
        AnyT * get( void ) const volatile {
            return operator->();
        }
        // This returns the pointer when you are absolutely certain it is not flagged.
        AnyT * getNotEnd( void ) const volatile {
            TOOLS_ASSERT( ( reinterpret_cast< ptrdiff_t >( p_ ) & static_cast< ptrdiff_t >( 1 )) == 0 );
            return p_;
        }
        // This does not check the flag.
        bool operator!( void ) const volatile {
            return !( reinterpret_cast< ptrdiff_t >( p_ ) & ~static_cast< ptrdiff_t >( 1 ));
        }
        bool operator==( FlagPointer< AnyT > const volatile & c ) const volatile {
            return ( p_ == c.p_ );
        }
        bool operator!=( FlagPointer< AnyT > const volatile & c ) const volatile {
            return ( p_ != c.p_ );
        }
        void reset( AnyT * p, bool flag = false ) volatile {
            p_ = reinterpret_cast< AnyT * >( reinterpret_cast< ptrdiff_t >( p ) | static_cast< ptrdiff_t >( flag ));
        }
        void reset( FlagPointer< AnyT > const volatile & c ) volatile {
            p_ = c.p_;
        }
        void reset( FlagPointer< AnyT > const & c ) volatile {
            p_ = c.p_;
        }
        void reset( FlagPointer< AnyT > && c ) volatile {
            AnyT * t = c.p_;
            c.p_ = p_;
            p_ = t;
        }
        //FlagPointer< AnyT > & operator=( FlagPointer< AnyT > const & c ) volatile {
        //    TOOLS_ASSERT( p_ == nullptr );
        //    p_ = c.p_;
        //    return *this;
        //}
        static FlagPointer< AnyT > make( AnyT * p = nullptr, bool flag = false ) {
            FlagPointer< AnyT > ret;
            ret.reset( p, flag );
            return ret;
        }
        AnyT * p_;
    };

    // A FlagPointer has a special value when it is isEnd(), otherwise it is just a pointer
    template<>
    struct FlagPointer< void >
    {
        void * operator->( void ) const {
            return reinterpret_cast< void * >( reinterpret_cast< ptrdiff_t >( p_ ) & ~static_cast< ptrdiff_t >( 1 ));
        }
        void * get( void ) const {
            return operator->();
        }
        // This returns the pointer when you are absolutely certain is is not flagged.
        void * getNotend( void ) const {
            TOOLS_ASSERT( ( reinterpret_cast< ptrdiff_t >( p_ ) & static_cast< ptrdiff_t >( 1 )) == 0 );
            return p_;
        }
        // This does not check the flag.
        bool operator!( void ) const {
            return !( reinterpret_cast< ptrdiff_t >( p_ ) & ~static_cast< ptrdiff_t >( 1 ));
        }
        void reset( void * p, bool flag = false ) {
            p_ = reinterpret_cast< void * >( reinterpret_cast< ptrdiff_t >( p ) | static_cast< ptrdiff_t >( flag ));
        }
        void * p_;
    };

    template< typename AnyT >
    inline bool isEnd( tools::FlagPointer< AnyT > const volatile & fp ) {
        return ( reinterpret_cast< size_t >( fp.p_ ) & static_cast< size_t >( 1 )) ? true : false;
    }
    template< typename AnyT >
    inline void setEnd( tools::FlagPointer< AnyT > volatile & ref ) {
        ref.reset( ref.get(), true );
    }

    // This is a pointer that may be more than one type, safely tested for the current type and providing
    // visitation for a particular type. Asserts at runtime that access to the type is the type expected.
    // Both types are FlagPointers, and the flags will agree on the mode. This is mostly useful for very,
    // _very_ memory tight structures. Note: this could probably be safely extended to more types presented
    // in a type list (because 64-bit is 8-byte aligned, so we have 3 'spare' bits for 8 options.
    template< typename DefaultAnyT, typename OtherAnyT >
    struct AlternatePointer
    {
        explicit AlternatePointer( DefaultAnyT * defP = nullptr ) {
            defaultPtr_.reset( defP );
        }
        explicit AlternatePointer( OtherAnyT * otherP ) {
            otherPtr_.reset( otherP, true );
        }
        bool operator!( void ) const {
            return !defaultPtr_;  // They're the same.
        }
        bool operator==( AlternatePointer< DefaultAnyT, OtherAnyT > const & c ) const {
            return ( defaultPtr_ == c.defaultPtr_ );
        }
        bool operator!=( AlternatePointer< DefaultAnyT, OtherAnyT > const & c ) const {
            return ( defaultPtr_ != c.defaultPtr_ );
        }
        bool operator<( AlternatePointer< DefaultAnyT, OtherAnyT > const & c ) const {
            return ( defaultPtr_ < c.defaultPtr_ );
        }
        bool operator==( DefaultAnyT * c ) const {
            return ( !isEnd( defaultPtr_ ) && ( defaultPtr_.getNotEnd() == c ));
        }
        bool operator!=( DefaultAnyT * c ) const {
            return ( isEnd( defaultPtr_ ) || ( defaultPtr_.getNotEnd() != c ));
        }
        bool operator==( OtherAnyT * c ) const {
            return ( other() == c );
        }
        bool operator!=( OtherAnyT * c ) const {
            return ( other() != c );
        }
        AlternatePointer< DefaultAnyT, OtherAnyT > const & operator=( DefaultAnyT * c ) {
            defaultPtr_.reset( c );
            return *this;
        }
        AlternatePointer< DefaultAnyT, OtherAnyT > const & operator=( OtherAnyT * c ) {
            otherPtr_.reset( c, true );
            return *this;
        }
        DefaultAnyT * default( void ) {
            DefaultAnyT * ret = defaultPtr_.p_;
            return !isEnd( defaultPtr_ ) ? ret : nullptr;
        }
        OtherAnyT * other( void ) {
            OtherAnyT * ret = otherPtr_.get();
            return isEnd( otherPtr_ ) ? ret : nullptr;
        }
        void reset( DefaultAnyT * defP ) {
            defaultPtr_.reset( defP );
        }
        void reset( OtherAnyT * otherP ) {
            otherPtr_.reset( otherP, true );
        }
        // This version is slightly faster. There is no check for nullptr on top of the flag check.
        bool as( OtherAnyT ** ref ) const {
            *ref = otherPtr_.get();
            return ( *ref != otherPtr_.p_ );
        }
        bool as( DefaultAnyT ** ref ) const {
            *ref = defaultPtr_.p_;
            return !isEnd( defaultPtr_ );
        }
        // Keep extra type information to aid the debugger.
        union {
            tools::FlagPointer< DefaultAnyT > defaultPtr_;
            tools::FlagPointer< OtherAnyT > otherPtr_;
        };
    };

    template< typename DefaultAnyT, typename OtherAnyT >
    inline void swap( tools::AlternatePointer< DefaultAnyT, OtherAnyT > & left, tools::AlternatePointer< DefaultAnyT, OtherAnyT > & right ) {
        auto tmp = left.defaultPtr_.p_;  // same because of union.
        left.defaultPtr_.p_ = right.defaultPtr_.p_;
        right.defaultPtr_.p_ = tmp;
    }

    // A Sentinel is a placeholder value that is guaranteed to be a unique address within the heap. As such
    // it can be used as a special 'alternate' value that cannot be confused with an actual value of any
    // kind.
    template< typename InterfaceT, size_t size = sizeof( int ) >
    struct Sentinel
    {
        TOOLS_PURE_INLINE InterfaceT * operator&( void ) const {
            return const_cast< InterfaceT * >( reinterpret_cast< InterfaceT const * >( &storage_[ 0 ]));
        }
    private:
        char storage_[ size ];
    };
};  // namespace tools

#ifdef WINDOWS_PLATFORM
#  include <intrin.h>
#  pragma intrinsic(_ReturnAddress)
#  define TOOLS_RETURN_ADDRESS() (_ReturnAddress())
#endif // WINDOWS_PLATFORM
#ifdef UNIX_PLATFORM
#  define TOOLS_RETURN_ADDRESS() (__builtin_return_address(0))
#endif // UNIX_PLATFORM

namespace tools {
  namespace impl {
      struct ResourceTrace
      {
          virtual StringId name( void ) const = 0;
          virtual size_t size( void ) const = 0;
          virtual void * symbol( void ) const = 0;
          virtual void inc( size_t = 1U ) = 0;
          virtual void dec( size_t = 1U ) = 0;
          virtual unsigned interval( void ) = 0;
      };

      struct ResourceSample
      {
          ResourceSample( size_t size, void * site, ResourceTrace * parent = nullptr )
              : size_( size )
              , site_( site )
              , name_( nullptr )
              , parent_( parent )
          {}
          ResourceSample( size_t size, char const * name, void * site = nullptr, ResourceTrace * parent = nullptr )
              : size_( size)
              , site_( site )
              , name_( name )
              , parent_( parent )
          {}
          TOOLS_NO_INLINE ResourceSample( size_t size, ResourceTrace * parent = nullptr )
              : size_( size )
              , site_( TOOLS_RETURN_ADDRESS() )
              , name_( nullptr )
              , parent_( parent )
          {}

          bool operator==( ResourceSample const & c ) {
              return ( size_ == c.size_ ) && ( site_ == c.site_ ) && ( name_ == c.name_ ) && ( parent_ == c.parent_ );
          }

          size_t size_;  // Allocation size
          void * site_;  // Function/method that is the source of the allocation.
          char const * name_;
          ResourceTrace * parent_;  // Parent allocation (if this allocation is contained
                                    // in another).  This may be nullptr for a top-level
                                    // allocation.
      };

      TOOLS_FORCE_INLINE uint32 defineHashAny( ResourceSample const & samp )
      {
          auto ret = tools::hashAnyBegin( samp ) % samp.site_ % static_cast< uint32 >( samp.size_ & 0xFFFFFFFFU );
          if( !!samp.parent_ ) {
              ret %= samp.parent_;
#if (__SIZEOF_POINTER__ == 8) || (_INTEGRAL_MAX_BITS == 64)
              ret %= static_cast< uint32 >( ( samp.size_ >> 32 ) & 0xFFFFFFFFU );
#endif // 64-bit test
          }
          return ret;
      }

      enum ResourceTraceDumpPhase
      {
          resourceTraceDumpPhaseInitial,
          resourceTraceDumpPhasePeriodic,
          resourceTraceDumpPhaseWatermark,
          resourceTraceDumpPhaseAll,
      };

      TOOLS_API ResourceTrace * resourceTraceBuild( ResourceSample const &, ResourceTrace * = nullptr );
      TOOLS_API ResourceTrace * resourceTraceBuild( unsigned interval, ResourceSample const &, ResourceTrace * = nullptr );
      TOOLS_API ResourceTrace * resourceTraceBuild( unsigned interval, StringId const &, size_t nbytes, ResourceTrace * = nullptr );
      TOOLS_API ResourceTrace * resourceTraceBuild( StringId const &, ResourceTrace * = nullptr );
      TOOLS_API void resourceTraceDump( ResourceTrace * );
  };  // impl namespace
}; // namespace tools

// Create a sample from a caller and size.  We require a macro for this because the
// return address can't be determined across a function call.
#define TOOLS_RESOURCE_SAMPLE_CALLER( sz ) tools::impl::ResourceSample( sz, TOOLS_RETURN_ADDRESS() )
#define TOOLS_RESOURCE_SAMPLE_CALLER_T( sz, t ) tools::impl::ResourceSample( sz, TOOLS_RETURN_ADDRESS(), t )
#define TOOLS_RESOURCE_SAMPLE_HERE( sz ) tools::impl::ResourceSample( sz )
#define TOOLS_RESOURCE_SAMPLE_NAMED( sz, name ) tools::impl::ResourceSample( sz, name, nullptr, nullptr )
#define TOOLS_RESOURCE_SAMPLE_NAMED_T( sz, name, t ) tools::impl::ResourceSample( sz, name, nullptr, t )
