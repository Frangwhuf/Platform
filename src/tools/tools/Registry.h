#pragma once

#include <tools/Interface.h>
#include <tools/InterfaceTools.h>
#include <tools/Meta.h>
#include <tools/String.h>
#include <tools/Tools.h>

namespace tools {
    // The dynamic type version will register services by name.  This will remain registered until the
    // returned disposable is disposed.
    TOOLS_API AutoDispose<> registryInsert( StringId const &, StringId const &, void * ) throw();

    // Fetch a registered item by name (or return nullptrptr if no such entry was found)
    TOOLS_API void * registryFetch( StringId const &, StringId const & ) throw();

    // Typesafe accessors
    template< typename ServiceT >
    typename tools::ServiceInterfaceOf< ServiceT >::Type * registryFetch( StringId const & type, ServiceT *** = 0 ) {
        return static_cast< typename tools::ServiceInterfaceOf< ServiceT >::Type * >( registryFetch( tools::nameOf< ServiceT >(), type ));
    }

    struct RegistryEnumeration
    {
        struct Visitor
        {
            virtual void visit( StringId const &, void * ) = 0;
        };

        virtual void visit( Visitor & ) = 0;
        template< typename ServiceT, typename FunctorT >
        void visit( FunctorT const &, ServiceT *** = 0 );
    };

    namespace detail {
        template< typename ServiceT, typename FunctorT >
        struct RegistryVisitorFunctor
            : RegistryEnumeration::Visitor
        {
            RegistryVisitorFunctor( FunctorT const & func )
                : func_( &func )
            {}
            void visit( StringId const & type, void * entry )
            {
                ( *func_ )( type, static_cast< typename tools::ServiceInterfaceOf< ServiceT >::Type * >( entry ));
            }

            // Don't copy the functor, as this is on the stack
            FunctorT const * func_;
        };
    };  // detail namespace

    template< typename ServiceT, typename FunctorT >
    inline void RegistryEnumeration::visit( FunctorT const & func, ServiceT *** )
    {
        tools::detail::RegistryVisitorFunctor< ServiceT, FunctorT > v( func );
        visit( v );
    }

    // Strip off the Reg wrapper to get the service type
    template< typename ServiceT >
    auto define__( tools::detail::ServiceTraits ***, tools::Reg< ServiceT > *** )
        -> typename ::tools::detail::TraitsOf< tools::detail::ServiceTraits, ServiceT >::Type;

    template< typename ServiceT, typename TypeT >
    inline typename tools::ServiceInterfaceOf< ServiceT >::Type *
    staticServiceCacheInit( tools::Reg< ServiceT > ***, TypeT *** ) {
        return static_cast< typename tools::ServiceInterfaceOf< ServiceT >::Type * >( registryFetch( tools::nameOf< ServiceT >(), tools::nameOf< TypeT >() ));
    }

    template< typename ServiceT, typename TypeT >
    inline typename tools::ServiceInterfaceOf< ServiceT >::Type *
    registryFetch( ServiceT *** = 0, TypeT *** = 0 ) {
        return tools::staticServiceCacheFetch< tools::Reg< ServiceT >, TypeT >();
    }

    template< typename ServiceT, typename TypeT >
    inline typename tools::ServiceInterfaceOf< ServiceT >::Type &
    registryRef( ServiceT *** = 0, TypeT *** = 0 ) {
        return tools::staticServiceCacheRef< tools::Reg< ServiceT >, TypeT >();
    }

    template< typename ServiceT, typename FunctorT >
    inline void registryVisit( FunctorT const & func ) {
        tools::registryFetch< RegistryEnumeration, ServiceT >()->template visit< ServiceT >( func );
    }

    template< typename ServiceT >
    inline AutoDispose<> registryInsert( StringId const & type, typename tools::ServiceInterfaceOf< ServiceT >::Type * val, ServiceT *** = 0 ) {
        return tools::registryInsert( tools::nameOf< ServiceT >(), type, static_cast< void * >( val ));
    }

    template< typename ServiceT, typename TypeT >
    inline AutoDispose<> registryInsert( typename tools::ServiceInterfaceOf< ServiceT >::Type * val, ServiceT *** = 0, TypeT *** = 0 ) {
        return tools::registryInsert( tools::nameOf< ServiceT >(), tools::nameOf< TypeT >(), static_cast< void * >( val ));
    }

    // Auto-registration for an exact instance.
    template< typename ServiceT, typename TypeT, typename ImplementationT >
    struct RegisterAny
        : ImplementationT
    {
        RegisterAny( void )
            : registration_( tools::registryInsert< ServiceT, TypeT >( static_cast< typename tools::ServiceInterfaceOf< ServiceT >::Type * >( static_cast< ImplementationT * >( this ))))
        {}

        tools::AutoDispose<> registration_;
    };

    namespace detail {
        struct FactoryInstance
        {
            AutoDispose<> instance_;
            AutoDispose<> insertion_;
            FactoryInstance * next_;
        };
    };  // detail namespace

    // Registry services can be synthesized when they aren't explicitly registered. This can be effective
    // when a service for a type can be based on another service that was previously registered.
    struct FactoryRegistry
    {
        TOOLS_API FactoryRegistry( void ) throw();
        TOOLS_API ~FactoryRegistry( void ) throw();

        virtual AutoDispose<> link( void **, StringId const & ) = 0;

        detail::FactoryInstance * volatile instances_;
    };

    // AutoRegister permits generative services to be registered as a consequence of querying common
    // components instead of requiring explicit registration for every service/type combination. In this
    // way is counter to FactoryRegistry.
    //
    // A service that wishes to enable auto registration can use the standardAutoRegister(...) function
    // to return a template functor (containing a call operator) that will return a factory type that will
    // be inserted into the autoRegister service and create new types as needed.
    //
    // To create a dependency call the autoRegister(...) function at any time to touch a static
    // registration structure.
    //
    // Auto-registers are applied after explicit insertions, but before factory creation. Lick factory
    // instantiations, they are subject to phantom creations. Only the first instance inserted into
    // AutoRegister will stick.
    struct AutoRegister
    {
        virtual AutoDispose<> insert( StringId const &, void * ) = 0;
    };

    template< typename ServiceT, typename TypeT >
    struct ResolveAutoRegister;

    namespace detail {
        // The factory that will complete the dynamic linkage with the AutoRegister.
        template< typename ServiceT, typename TypeT >
        struct AutoFactoryRegister
        {
            TOOLS_NO_INLINE static typename tools::ServiceInterfaceOf< ServiceT >::Type * storage( void ) throw() {
                typedef typename tools::ResolveAutoRegister< ServiceT, TypeT >::Type ImplementationT;
                static ImplementationT impl_;
                return &impl_;
            }
        };
    };  // detail namespace

    template< typename ServiceT, typename TypeT >
    TOOLS_NO_INLINE inline void registryAutoInsert( ServiceT *** = 0, TypeT *** = 0 ) {
        static tools::AutoDispose<> register_;
        if( !register_ ) {
            registryAutoDepends< ServiceT, TypeT >();
            if( !register_ ) {
                register_.reset( tools::registryFetch< AutoRegister, ServiceT >()->insert( tools::nameOf< TypeT >(), tools::detail::AutoFactoryRegister< ServiceT, TypeT >::storage() ));
            }
        }
    }

    namespace detail {
        template< typename ServiceT >
        struct RegisterAutoType
        {
            template< typename TypeT >
            TOOLS_NO_INLINE void operator()( size_t, TypeT *** ) {
                tools::registryAutoInsert< ServiceT, TypeT >();
            }
        };

        template< typename TypeSeqT >
        struct RegisterAutoService
        {
            template< typename ServiceT >
            TOOLS_NO_INLINE void operator()( size_t, ServiceT *** ) {
                tools::detail::RegisterAutoType< ServiceT > v;
                tools::EvalTypes< TypeSeqT >::bind( v );
            }
        };
    };  // detail namespace

    template< typename ServiceSeqT, typename TypeSeqT >
    struct RegisterAuto
    {
        RegisterAuto( void ) {
            tools::detail::RegisterAutoService< TypeSeqT > v;
            tools::EvalTypes< ServiceSeqT >::bind( v );
        }
    };

    template< typename ServiceT, typename TypeT >
    struct ResolveAutoRegister
    {
        typedef decltype( standardAutoRegister< ServiceT, TypeT >() ) DeclTypeT;
        typedef DeclTypeT Type;
    };

    // A trivial factory for when we can generate whatever we need from a type name.
    template< typename ServiceT, typename ImplementationT >
    struct RegisterFactoryRegistry
    {
        struct Instance
            : tools::StandardDisposable< Instance, tools::Disposable, tools::AllocStatic< Platform >>
        {
            Instance( StringId const & name )
                : impl_( name )
            {}

            ImplementationT impl_;
        };

        struct Factory
            : tools::FactoryRegistry
        {
            tools::AutoDispose<> link( void ** service, StringId const & name ) {
                Instance * new_ = new Instance( name );
                *service = static_cast< typename tools::ServiceInterfaceOf< ServiceT >::Type * >( &new_->impl_ );
                return std::move(new_);
            }
        };

        tools::RegisterAny< FactoryRegistry, ServiceT, Factory > inner_;
    };

    // A service factory functor for single statement service factory definition. The functor must have
    // the prototype:
    //     []( ServiceInterfaceT ** boundItf, StringId const & name )->AutoDispose<>
    template< typename ServiceT >
    struct RegisterFactoryRegistryFunctor
    {
        template< typename FactoryT >
        struct FactoryImpl
            : tools::FactoryRegistry
            , tools::StandardDisposable< FactoryImpl< FactoryT >, tools::Disposable, tools::AllocStatic<Platform>>
        {
            FactoryImpl( FactoryT const & func )
                : func_( func )
            {}

            tools::AutoDispose<> link( void ** bound, StringId const & name ) {
                // Trampoline to local variable to ensure single assignment.
                typename tools::ServiceInterfaceOf< ServiceT >::Type * itf = nullptr;
                tools::AutoDispose<> new_( func_( &itf, name ));
                *bound = itf;
                return new_;
            }

            FactoryT func_;
        };

        template< typename FactoryT >
        RegisterFactoryRegistryFunctor( FactoryT const & func )
        {
            FactoryImpl< FactoryT > * new_ = new FactoryImpl< FactoryT >( func );
            factory_ = new_;
            registration_ = tools::registryInsert< tools::FactoryRegistry, ServiceT >( new_ );
        }

        tools::AutoDispose<> factory_;
        tools::AutoDispose<> registration_;
    };

    // This inverts the relationship between a service and type by presenting a bundle of services with
    // an implied type, rather than the other way around.  nullptr is returned when there is no service
    // present.  In general this isn't faster than doing a dual lookup, but it is likely much more
    // convenient.
    struct AnyService
    {
        virtual StringId describe( void ) = 0;
        virtual void * get( StringId const & ) = 0;
        template< typename ServiceT >
        typename tools::ServiceInterfaceOf< ServiceT >::Type * get( ServiceT *** = 0 ) {
            return static_cast< typename tools::ServiceInterfaceOf< ServiceT >::Type * >( get( tools::nameOf< ServiceT >() ));
        }
    };
}; // namespace tools
