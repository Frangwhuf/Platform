#pragma once

#include <tools/AsyncTools.h>
#include <tools/Interface.h>
#include <tools/InterfaceTools.h>
#include <tools/Registry.h>
#include <tools/String.h>
#include <tools/Threading.h>
#include <tools/Tools.h>

#include <boost/mpl/as_sequence.hpp>
#include <boost/mpl/deref.hpp>
#include <boost/mpl/empty_sequence.hpp>
#include <boost/mpl/list.hpp>
#include <boost/mpl/next.hpp>

// This is internal to service declaration
template< typename ServiceT >
struct InterfaceFromService {
    typedef ServiceT type;
};

namespace tools {
    struct Environment;

    namespace impl {
        struct Service
        {
            virtual void bindEnv( tools::Environment & ) throw() = 0;
            virtual AutoDispose< Request > start( void ) throw() = 0;
            virtual AutoDispose< Request > stop( void ) throw() = 0;
        };
    };  // impl namespace

    // Compute the name of a service
    template< typename ServiceT >
    inline StringId const & ServiceName( ServiceT *** = nullptr ) {
        // ADL implements service name
        return nameOf< ServiceT >();
    }

    struct Service
        : Disposable
        , Unknown
        , impl::Service
    {
    };

    ///
    // This is the environment, and it represents the foundation of role virtualization and
    // the source of all tools that are not process global.  It contains references to objects
    // that are considered to be global to that role.
    //
    // The tools (services) are factoried by the environment on demand.  Services initialize
    // and terminate in two stages.  First is construction, during which time a service should
    // get references to other services on which it depends.  This allows the environment to
    // determine which order to terminate the services.  If the environment itself supports
    // two stage initialization, all factoried services will then undergo their second stage
    // initialization (start()) when the environment itself goes through its second stange
    // initialization.  If the environment does not support second stage initialization, then
    // services are second stage initialized after factory construction.
    //
    // The lifetime of the environment is managed outside of its interface, and is likely
    // described by the factory that creates it.
    struct Environment
    {
        virtual StringId const & name( void ) = 0;
        virtual Unknown * get( StringId const & ) = 0;

        template< typename ServiceT >
        typename tools::ServiceInterfaceOf< ServiceT >::Type * get( ServiceT *** = nullptr ) {
            Unknown * ret = get( tools::ServiceName< ServiceT >() );
            return !!ret ? ret->getInterface< typename tools::ServiceInterfaceOf< ServiceT >::Type >() : nullptr;
        }

        template< typename ServiceT, typename ItfT >
        ItfT * get( ServiceT *** = nullptr, ItfT *** = nullptr ) {
            Unknown * ret = get( tools::ServiceName< ServiceT >() );
            return !!ret ? ret->getInterface< ItfT >() : nullptr;
        }
    };

    ///
    // Returns a new instance of an environment object.  This environment is initially empty.  Any
    // service factoried by this environment will go through both stages of initialization as part
    // of being factoried.
    //
    // The AutoDispose passed in to this function is filled with a Disposable that controls the
    // lifetime of the returned environment.  When disposed, all factoried services will undergo
    // both stages of their termination in reverse order to how they were factoried.
    TOOLS_API Environment * NewSimpleEnvironment( AutoDispose<> &, StringId const & = StringIdEmpty() );

    ///
    // Returns a new instance of an environment object.  This environment is initially empty.  Any
    // service factoried by this environment will not go through its second stage of initialization
    // until the environment itself goes through its second stange of initialization.  This environment
    // will respond to fetching impl::Service to give access to it's second stage initialization and
    // termination.
    //
    // The AutoDispose passed in to this function is filled with a Disposable that controls the
    // lifetime of the returned environment.  Before this can be disposed, the owner must complete the
    // second stage termination (stop()) on the environment.  Failing to do so will at the least
    // assert.  During second stage termination, all services will undergo their second stage
    // termination in reverse order to how they were initialized.  During dispose, all services will
    // be disposed in reverse order to how they were factoried.
    TOOLS_API Environment * NewTwoStageEnvironment( AutoDispose<> &, StringId const & = StringIdEmpty() );

    template< typename ImplT, typename SequenceT=boost::mpl::empty_sequence, typename ItfT=tools::Service, typename AllocT = tools::AllocStatic<> >
    struct StandardService
        : StandardUnknown< ImplT, SequenceT, StandardDisposable< ImplT, ItfT, AllocNull >, AllocT >
    {
        static_assert( std::is_same< tools::Service, ItfT >::value || std::is_base_of< tools::Service, ItfT >::value, "ItfT must derive from tools::Service" );

        StandardService( void )
            : serviceThreadScheduler_( nullptr )
        {
        }

        // These should be implemented optionall by the implementation type.
        AutoDispose< Request > serviceStart( void ) {
            return nullptr;
        }
        AutoDispose< Request > serviceStop( void ) {
            return nullptr;
        }
    private:
        struct StartRequest
            : StandardRequest< StartRequest >
        {
            StartRequest( ImplT & parent )
                : parent_( parent )
            {}

            // StandardRequest
            RequestStep start( void )
            {
                inner_.reset( parent_.serviceStart() );
                return maybeWaitFinish( inner_.get() );
            }

            ImplT & parent_;
            AutoDispose< Request > inner_;
        };

        struct StopRequest
            : StandardRequest< StopRequest >
        {
            StopRequest( ImplT & parent )
                : parent_( parent )
            {}

            // StandardRequest
            RequestStep start( void )
            {
                inner_.reset( parent_.serviceStop() );
                return maybeWaitFinish( inner_.get() );
            }

            ImplT & parent_;
            AutoDispose< Request > inner_;
        };
    public:
        // These provide a concrete implementation for the methods from the Service interface
        void bindEnv( tools::Environment & env ) throw() {
            serviceThreadScheduler_ = env.get< tools::TaskScheduler >();
        }
        AutoDispose< Request > start() throw() {
            if( !serviceThreadScheduler_ ) {
                return new StartRequest( *static_cast< ImplT * >( this ));
            }
            return serviceThreadScheduler_->proxy( AutoDispose< Request >( new StartRequest( *static_cast< ImplT * >( this ))), impl::affinityInstance<Temporal>(), serviceThreadScheduler_->defaultParam() );
        }
        AutoDispose< Request > stop() throw() {
            if( !serviceThreadScheduler_ ) {
                return new StopRequest( *static_cast< ImplT * >( this ));
            }
            return serviceThreadScheduler_->proxy( AutoDispose< Request >( new StopRequest( *static_cast< ImplT * >( this ))), impl::affinityInstance<Temporal>(), serviceThreadScheduler_->defaultParam() );
        }

        ThreadScheduler *   serviceThreadScheduler_;
    };

    namespace detail {
        template< typename ImplT, typename SequenceT=boost::mpl::empty_sequence, typename ItfT=tools::Service, typename AllocT = tools::AllocStatic<> >
        struct StandardNoBindService
            : StandardUnknown< ImplT, SequenceT, StandardDisposable< ImplT, ItfT, AllocNull >, AllocT >
        {
            static_assert( std::is_same< tools::Service, ItfT >::value || std::is_base_of< tools::Service, ItfT >::value, "ItfT must derive from tools::Service" );

            StandardNoBindService( void )
            {
            }

            // These should be implemented optionall by the implementation type.
            AutoDispose< Request > serviceStart( void ) {
                return nullptr;
            }
            AutoDispose< Request > serviceStop( void ) {
                return nullptr;
            }
        private:
            struct StartRequest
                : StandardRequest< StartRequest >
            {
                StartRequest( ImplT & parent )
                    : parent_( parent )
                {}

                // StandardRequest
                RequestStep start( void )
                {
                    inner_.reset( parent_.serviceStart() );
                    return maybeWaitFinish( inner_.get() );
                }

                ImplT & parent_;
                AutoDispose< Request > inner_;
            };

            struct StopRequest
                : StandardRequest< StopRequest >
            {
                StopRequest( ImplT & parent )
                    : parent_( parent )
                {}

                // StandardRequest
                RequestStep start( void )
                {
                    inner_.reset( parent_.serviceStop() );
                    return maybeWaitFinish( inner_.get() );
                }

                ImplT & parent_;
                AutoDispose< Request > inner_;
            };
        public:
            // These provide a concrete implementation for the methods from the Service interface
            void bindEnv( tools::Environment & ) throw() {
            }
            AutoDispose< Request > start() throw() {
                return new StartRequest( *static_cast< ImplT * >( this ));
            }
            AutoDispose< Request > stop() throw() {
                return new StopRequest( *static_cast< ImplT * >( this ));
            }
        };
    };  // detail namespace
    namespace impl {
        struct FactoryEnvironment
        {
            struct Desc
            {
                enum : unsigned {
                    defaultServicePhase = 1U,
                };

                TOOLS_FORCE_INLINE Desc( StringId const & name, bool inherit, unsigned phase = defaultServicePhase )
                    : interfaceName_( name )
                    , inherritable_( inherit )
                    , phase_( phase )
                {}

                StringId interfaceName_;
                bool inherritable_;
                unsigned phase_;  // of start/stop
            };

            virtual Desc const & describe( void ) = 0;
            virtual tools::Service * factory(Environment &) = 0;
        };
    };  // impl namespace

    // Bind a service implementation to its interface.
    template< typename ServiceT, typename ImplementationT >
    struct RegisterEnvironment
        : tools::impl::FactoryEnvironment
    {
        RegisterEnvironment( bool inherritable = false, unsigned phase = tools::impl::FactoryEnvironment::Desc::defaultServicePhase )
            : desc_( tools::nameOf< typename tools::ServiceInterfaceOf< ServiceT >::Type >(), inherritable, phase )
            , registration_( tools::registryInsert< tools::impl::FactoryEnvironment, ServiceT >( this ))
        {}

        // impl::FactoryEnvironment
        tools::impl::FactoryEnvironment::Desc const &
        describe( void )
        {
            return desc_;
        }

        tools::Service *
        factory( tools::Environment & env )
        {
            ImplementationT * impl = new ImplementationT( env );
            tools::Service * ret = static_cast< tools::Service * >( impl );
            ret->bindEnv( env );
            return ret;
        }

        tools::impl::FactoryEnvironment::Desc desc_;
        tools::AutoDispose<> registration_;
    };
}; // namespace tools
