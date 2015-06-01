#pragma once

#include <tools/Interface.h>
#include <tools/Memory.h>

namespace tools {
    template< typename ImplementationT, typename SequenceT=boost::mpl::empty_sequence,
        typename IterfaceT=tools::Unknown, typename AllocT = tools::AllocStatic<> >
    struct StandardUnknown
        : IterfaceT
        , AllocT
    {
        void * getInterfaceDynamic( StringId const & ) const throw() {
            return nullptr;
        }
    protected:
        template< typename IteratorT >
        void * getInterfaceImpl( StringId const & typeName, IteratorT * ) const throw() {
            if (typeName == tools::nameOf< typename boost::mpl::deref< IteratorT >::type >() ) {
                return static_cast< typename boost::mpl::deref< IteratorT >::type * >( const_cast< ImplementationT * >( static_cast< ImplementationT const * >( this )));
            }
            return getInterfaceImpl( typeName, static_cast< typename boost::mpl::next< IteratorT >::type * >( nullptr ));
        }

        // This terminates the iteration.
        void * getInterfaceImpl( StringId const & typeName, typename boost::mpl::end< SequenceT >::type * ) const throw() {
            return static_cast< ImplementationT const * >( this )->getInterfaceDynamic( typeName );
        }
    public:
        void * getInterface( StringId const & typeName ) const throw() {
            return getInterfaceImpl( typeName, static_cast< typename boost::mpl::begin< SequenceT >::type * >( nullptr ) );
        }
    };

    ///
    // A standard implementation of Disposable.  Given that most implementations just 'delete this',
    // then how about we stop making people type that.
    template< typename ImplementationT, typename DisposeT = tools::Disposable,
        typename AllocT = tools::AllocStatic<> >
    struct StandardDisposable
        : DisposeT
        , AllocT
    {
        void dispose( void ) { delete static_cast< ImplementationT * >( this ); }
    };
    template< typename ImplementationT, typename InterfaceT >
    struct StandardIsDisposable
        : tools::StandardDisposable< ImplementationT, tools::IsDisposable< InterfaceT > >
    {};

	// A delegate is a crystalization of a callable function/method.  The target function
	// can be a member or non-member function that expects to take a context parameter and
	// optionally another parameter.  This is primarilly created by helpers like: Delegatable,
	// Notifiable, Completable, etc.
	template< typename ParameterT = void, typename ReturnT = void >
	struct Delegate {
		typedef ReturnT (*FunctionT)( void *, ParameterT );

		Delegate( void )
			: func_( nullptr )
			, param_( nullptr )
		{}
		Delegate( FunctionT func, void * param )
			: func_( func )
			, param_( param )
		{}
		bool
		operator!( void ) const
		{
			return !func_;
		}
		bool
		operator==( Delegate< ParameterT, ReturnT > const & r ) const
		{
			return ( func_ == r.func_ ) && ( param_ == r.param_ );
		}
		void
		operator()( ParameterT p = ParameterT() ) const
		{
			return func_( param_, p );
		}
		void
		fire( ParameterT p = ParameterT() )
		{
			FunctionT f = func_;
			void * param = param_;
			func_ = nullptr;
			param_ = nullptr;
			f( param, p );
		}

		FunctionT func_;
		void * param_;
	};

	template< typename ReturnT >
	struct Delegate< void, ReturnT > {
		typedef ReturnT (*FunctionT)( void * );

		Delegate( void )
			: func_( nullptr )
			, param_( nullptr )
		{}
		Delegate( FunctionT func, void * param )
			: func_( func )
			, param_( param )
		{}
		bool
		operator!( void ) const
		{
			return !func_;
		}
		bool
		operator==( Delegate< void, ReturnT > const & r ) const
		{
			return ( func_ == r.func_ ) && ( param_ == r.param_ );
		}
		void
		operator()( void ) const
		{
			return func_( param_ );
		}
		void
		fire( void )
		{
			FunctionT f = func_;
			void * param = param_;
			func_ = nullptr;
			param_ = nullptr;
			f( param );
		}

		FunctionT func_;
		void * param_;
	};

	typedef Delegate<> Thunk;

	namespace impl {
		template< typename ImplementationT, typename ParameterT = void, typename ReturnT = void >
		struct AsDelegate {
			typedef typename tools::Delegate< ParameterT, ReturnT >::FunctionT FunctionT;
			template< ReturnT ( ImplementationT::*MethodT )( ParameterT ) >
			struct ImplementationMethod {
				static ReturnT
				func( void * param, ParameterT p )
				{
					return ( static_cast< ImplementationT * >( param )->*MethodT )( p );
				}
			};
			template< ReturnT ( ImplementationT::*MethodT )( ParameterT ) >
			static FunctionT
			toFunc( ImplementationMethod< MethodT > *** = 0 )
			{
				return &ImplementationMethod< MethodT >::func;
			}
			// Param helper
			template< typename ThisT >
			static void *
			toParam( ThisT * param )
			{
				return static_cast< void * >( static_cast< ImplementationT * >( param ));
			}
		};

		template< typename ImplementationT, typename ReturnT >
		struct AsDelegate< ImplementationT, void, ReturnT > {
			typedef typename tools::Delegate< void, ReturnT >::FunctionT FunctionT;
			template< ReturnT ( ImplementationT::*MethodT )( void ) >
			struct ImplementationMethod {
				static ReturnT
				func( void * param )
				{
					return ( static_cast< ImplementationT * >( param )->*MethodT )();
				}
			};
			template< ReturnT ( ImplementationT::*MethodT )( void ) >
			static FunctionT
			toFunc( ImplementationMethod< MethodT > *** = 0 )
			{
				return &ImplementationMethod< MethodT >::func;
			}
			// Param helper
			template< typename ThisT >
			static void *
			toParam( ThisT * param )
			{
				return static_cast< void * >( static_cast< ImplementationT * >( param ));
			}
		};
	};  // impl namespace

	// A convenience template for creating callback delegates
	template< typename ImplementationT, typename ParameterT = void, typename ReturnT = void >
	struct Delegatable {
	protected:
		typedef tools::impl::AsDelegate< ImplementationT, ParameterT, ReturnT > ParamT;
		typedef tools::Delegate< ParameterT, ReturnT > DelegateT;
	public:
		template< ReturnT ( ImplementationT::*MethodT)( ParameterT ) >
		DelegateT
		toDelegate( typename ParamT::template ImplementationMethod< MethodT > *** = 0 )
		{
			return DelegateT( ParamT::template toFunc< MethodT >(),
				ParamT::template toParam< Delegatable< ImplementationT, ParameterT, ReturnT >>( this ));
		}
	};

	// specialization for no parameter version
	template< typename ImplementationT, typename ReturnT >
	struct Delegatable< ImplementationT, void, ReturnT > {
	protected:
		typedef tools::impl::AsDelegate< ImplementationT, void, ReturnT > ParamT;
		typedef tools::Delegate< void, ReturnT > DelegateT;
	public:
		template< ReturnT ( ImplementationT::*MethodT)( void ) >
		DelegateT
		toDelegate( typename ParamT::template ImplementationMethod< MethodT > *** = 0 )
		{
			return DelegateT( ParamT::template toFunc< MethodT >(),
				ParamT::template toParam< Delegatable< ImplementationT, void, ReturnT >>( this ));
		}
	};

    ///
    // A convenience template for creating callback thunks.
	template< typename ImplementationT >
	struct Notifiable
		: tools::Delegatable< ImplementationT >
	{
		template< void ( ImplementationT::*ThunkT )( void ) >
		tools::Thunk
		toThunk( void )
		{
			return this->template toDelegate< ThunkT >();
		}
	};

    namespace detail {
        template< typename ImplementationT, typename AllocT = tools::AllocStatic<> >
        struct EmplaceDisposable
            : StandardDisposable< EmplaceDisposable< ImplementationT, AllocT >, tools::Disposable, AllocT >
        {
            EmplaceDisposable( void )
            {
            }

            template< typename Param0T >
            EmplaceDisposable( Param0T && p0 )
                : obj_( std::forward< Param0T >( p0 ))
            {
            }

            template< typename Param0T, typename Param1T >
            EmplaceDisposable( Param0T && p0, Param1T && p1 )
                : obj_( std::forward< Param0T >( p0 ), std::forward< Param1T >( p1 ))
            {
            }

            template< typename Param0T, typename Param1T, typename Param2T >
            EmplaceDisposable( Param0T && p0, Param1T && p1, Param2T && p2 )
                : obj_( std::forward< Param0T >( p0 ), std::forward< Param1T >( p1 ), std::forward< Param2T >( p2 ))
            {
            }

            ImplementationT obj_;
        };
    };  // detail namespace

    template< typename ImplementationT >
    inline tools::AutoDispose<> anyDisposableNew( ImplementationT ** ref )
    {
        auto ret = new tools::detail::EmplaceDisposable< ImplementationT >();
        *ref = &ret->obj_;
        return tools::AutoDispose<>( ret );
    }

    // Below we put the allocator type first because the implementation type should be derivable from the
    // parameter.
    template< typename AllocT, typename ImplementationT >
    inline tools::AutoDispose<> anyDisposableAllocNew( ImplementationT ** ref, AllocT *** = 0 )
    {
        auto ret = new tools::detail::EmplaceDisposable< ImplementationT, AllocT >();
        *ref = &ret->obj_;
        return std::move( tools::AutoDispose<>( ret ));
    }

    template< typename ImplementationT, typename Param0T >
    inline tools::AutoDispose<> anyDisposableNew( ImplementationT ** ref, Param0T && p0 )
    {
        auto ret = new tools::detail::EmplaceDisposable< ImplementationT >( std::forward< Param0T >( p0 ));
        *ref = &ret->obj_;
        return tools::AutoDispose<>( ret );
    }

    // Below we put the allocator type first because the implementation type should be derivable from the
    // parameters.
    template< typename AllocT, typename ImplementationT, typename Param0T >
    inline tools::AutoDispose<> anyDisposableAllocNew( ImplementationT ** ref, Param0T && p0, AllocT *** = 0 )
    {
        auto ret = new tools::detail::EmplaceDisposable< ImplementationT, AllocT >( std::forward< Param0T >( p0 ));
        *ref = &ret->obj_;
        return std::move( tools::AutoDispose<>( ret ));
    }

    template< typename ImplementationT, typename Param0T, typename Param1T >
    inline tools::AutoDispose<> anyDisposableNew( ImplementationT ** ref, Param0T && p0, Param1T && p1 )
    {
        auto ret = new tools::detail::EmplaceDisposable< ImplementationT >( std::forward< Param0T >( p0 ), std::forward< Param1T >( p1 ));
        *ref = &ret->obj_;
        return tools::AutoDispose<>( ret );
    }

    // Below we put the allocator type first because the implementation type should be derivable from the
    // parameters.
    template< typename AllocT, typename ImplementationT, typename Param0T, typename Param1T >
    inline tools::AutoDispose<> anyDisposableAllocNew( ImplementationT ** ref, Param0T && p0, Param1T && p1, AllocT *** = 0 )
    {
        auto ret = new tools::detail::EmplaceDisposable< ImplementationT, AllocT >( std::forward< Param0T >( p0 ), std::forward< Param1T >( p1 ));
        *ref = &ret->obj_;
        return std::move( tools::AutoDispose<>( ret ));
    }

    template< typename ImplementationT, typename Param0T, typename Param1T, typename Param2T >
    inline tools::AutoDispose<> anyDisposableNew( ImplementationT ** ref, Param0T && p0, Param1T && p1, Param2T && p2 )
    {
        auto ret = new tools::detail::EmplaceDisposable< ImplementationT >( std::forward< Param0T >( p0 ), std::forward< Param1T >( p1 ), std::forward< Param2T >( p2 ));
        *ref = &ret->obj_;
        return tools::AutoDispose<>( ret );
    }

    // Below we put the allocator type first because the implementation type should be derivable from the
    // parameters.
    template< typename AllocT, typename ImplementationT, typename Param0T, typename Param1T, typename Param2T >
    inline tools::AutoDispose<> anyDisposableAllocNew( ImplementationT ** ref, Param0T && p0, Param1T && p1, Param2T && p2, AllocT *** = 0 )
    {
        auto ret = new tools::detail::EmplaceDisposable< ImplementationT, AllocT >( std::forward< Param0T >( p0 ), std::forward< Param1T >( p1 ), std::forward< Param2T >( p2 ));
        *ref = &ret->obj_;
        return std::move( tools::AutoDispose<>( ret ));
    }

    namespace impl {
      struct ResourceTraceSum
          : AllocStatic< Platform >
      {
          ResourceTraceSum( size_t total, size_t size, size_t count, StringId const & name )
              : total_( total ), size_( size ), count_( count ), name_( name )
          {}

          inline bool operator<( ResourceTraceSum const & other ) const {
              return total_ < other.total_;
          }

          size_t total_;  // should always be size_ * count_. Kept independantly for performance.
          size_t size_;
          size_t count_;
          StringId name_;
      };

      // The caller is made to pass in storage to minimize the possibility of allocation while inside this.
      // The previous contents should be considered lost. If no storage is passed (nullptr), this function will
      // allocate its own.
      TOOLS_API void resourceTraceDump(ResourceTraceDumpPhase, bool, std::vector< ResourceTraceSum, AllocatorAffinity< ResourceTraceSum, Platform >> * );
    };  // impl namespace
};  // namespace tools
