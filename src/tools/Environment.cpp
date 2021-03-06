#include "toolsprecompiled.h"

#include <tools/Algorithms.h>
#include <tools/Environment.h>
#include <tools/Async.h>
// #include <tools/Notification.h>

#include <unordered_map>
#include <vector>

#include <boost/mpl/list.hpp>

using namespace tools;

namespace {
    struct CycleTest
    {
        CycleTest( StringId const & );
        ~CycleTest( void );

        CycleTest * previous_;
        StringId serviceName_;
    };

    struct ThreadCycle
    {
        ThreadCycle( void ) : root_( nullptr ) {}

        CycleTest * volatile root_;
    };

  struct SimpleEnvironment
    : Environment
    , StandardDisposable< SimpleEnvironment >
  {
    typedef std::unordered_map< StringId, AutoDispose< Service > > ServiceMap;
    typedef std::vector< StringId > ServiceVec;

    SimpleEnvironment( StringId const & );
    ~SimpleEnvironment( void );

    // Environment
    StringId const & name( void );
    Unknown * get( StringId const & );

    ServiceMap services_;
    ServiceVec order_;
    StringId name_;
  };

  struct TwoStageEnvironment
    : Environment
    , impl::Service
    , StandardDisposable< TwoStageEnvironment >
    , StandardUnknown< TwoStageEnvironment, boost::mpl::list< impl::Service > >
  {
    typedef std::unordered_map< StringId, AutoDispose< tools::Service > > ServiceMap;
    typedef std::vector< StringId > ServiceVec;

    TwoStageEnvironment( StringId const & );
    ~TwoStageEnvironment( void );

    // Environment
    StringId const & name( void );
    Unknown * get( StringId const & );

    // impl::Service
    void bindEnv( Environment & ) throw();
    AutoDispose< Request > start( void ) throw();
    AutoDispose< Request > stop( void ) throw();

    ServiceMap services_;
    ServiceVec order_;
    StringId name_;
    bool allStopped_;
  };

  // static tools::notification::Category envCat( tools::StaticStringId( L"Environment" ), tools::StringIdNull() );
};  // anonymous namespace

static StandardThreadLocalHandle< ThreadCycle > cycleRoot_;

////////////
// CycleTest
////////////

CycleTest::CycleTest(
    StringId const & serviceName )
    : previous_( nullptr )
    , serviceName_( serviceName )
{
    auto root = cycleRoot_.get();
    auto loop = root->root_;
    while( !!loop ) {
        if( loop->serviceName_ == serviceName ) {
            // TODO: convert this to logging
            fprintf( stderr, "Environment::get() cycle detected!\n" );
            auto logLoop = root->root_;
            while( !!logLoop ) {
                fprintf( stderr, "\t%s\n", logLoop->serviceName_.c_str() );
                logLoop = logLoop->previous_;
            }
            return;
        }
        loop = loop->previous_;
    }
    previous_ = root->root_;
    auto old = atomicCas( &root->root_, previous_, this );
    TOOLS_ASSERT( old == previous_ );
}

CycleTest::~CycleTest( void )
{
    auto root = cycleRoot_.get();
    auto old = atomicCas( &root->root_, this, previous_ );
    TOOLS_ASSERT( old == this );  // if this fails something weird happened in the get stack
}

////////////////////
// SimpleEnvironment
////////////////////

SimpleEnvironment::SimpleEnvironment( StringId const & name )
  : name_( name )
{
}

SimpleEnvironment::~SimpleEnvironment( void )
{
  for( auto i=order_.rbegin(); i!=order_.rend(); ++i ) {
    auto item=services_.find( *i );
    TOOLS_ASSERT( item != services_.end() );
    AutoDispose< Request > req( item->second->stop() );
    if( !!req ) {
		AutoDispose< Error::Reference > err( runRequestSynchronously( req ));
		TOOLS_ASSERT( !err );
    }
    services_.erase( item );  // This should cause them to be disposed
  }
  TOOLS_ASSERT( services_.empty() );
  TOOLS_ASSERT(tools::detail::memoryValidate());
}

StringId const &
SimpleEnvironment::name( void )
{
  return name_;
}

Unknown *
SimpleEnvironment::get( StringId const & svc )
{
    CycleTest noCycles( svc );
    Unknown * ret = nullptr;
    auto item=services_.find( svc );
    if( item != services_.end() ) {
        ret = item->second.get();
    } else {
        impl::FactoryEnvironment * factory = registryFetch< impl::FactoryEnvironment >( svc );
        if( !factory ) {
            TOOLS_ASSERT( !"Unknown service" );
            return nullptr;
        }
        //impl::FactoryEnvironment::Desc const & desc = factory->describe();
        AutoDispose< Service > service( factory->factory( *this ));
        TOOLS_ASSERT( !!service );

        //AutoDispose<> l_( serviceLock->enter() );
        //item = services.find( svc );
        //if( item != services.end() ) {
        //    TOOLS_ASSERT( !!item->second );
        //    ret = item->second.get();
        //} else {
        // Since this is a single-stage environment, we start services as we create them
        AutoDispose< Request > srvStart( service->start() );
        if( !!srvStart ) {
	        AutoDispose< Error::Reference > err( runRequestSynchronously( srvStart ));
	        TOOLS_ASSERT( !err );
        }
        ret = service.get();
        services_[ svc ] = std::move( service );
        order_.push_back( svc );
        //}
    }
    return ret;
}

//////////////////////
// TwoStageEnvironment
//////////////////////

TwoStageEnvironment::TwoStageEnvironment( StringId const & name )
  : name_( name )
  , allStopped_( false )
{
}

TwoStageEnvironment::~TwoStageEnvironment()
{
  TOOLS_ASSERT( allStopped_ );
  for( auto i=order_.rbegin(); i!=order_.rend(); ++i ) {
    auto item=services_.find( *i );
    TOOLS_ASSERT( item != services_.end() );
    services_.erase( item );
  }
  TOOLS_ASSERT( services_.empty() );
  TOOLS_ASSERT(tools::detail::memoryValidate());
}

StringId const &
TwoStageEnvironment::name( void )
{
  return name_;
}

Unknown *
TwoStageEnvironment::get( StringId const & svc )
{
  CycleTest noCycles( svc );
  if( ServiceName< impl::Service >() == svc ) {
      return static_cast< Unknown * >( this );
  }
  Unknown * ret = nullptr;
  auto item=services_.find( svc );
  if( item != services_.end() ) {
    ret = item->second.get();
  } else {
      impl::FactoryEnvironment * factory = registryFetch< impl::FactoryEnvironment >( svc );
      if( !factory ) {
          TOOLS_ASSERT( !"Unknown service" );
          return nullptr;
      }
      //impl::FactoryEnvironment::Desc const & desc = factory->describe();
      AutoDispose< tools::Service > service( factory->factory( *this ));
      TOOLS_ASSERT( !!service );

      //AutoDispose<> l_( serviceLock->enter() );
      //item = services.find( svc );
      //if( item != services.end() ) {
      //    TOOLS_ASSERT( !!item->second );
      //    return item->second.get();
      //} else
      // For this kind of environment, services will be started as a seperate pass.
      ret = service.get();
      services_[ svc ] = std::move( service );
      order_.push_back( svc );
      //}
  }
  return ret;
}

void
TwoStageEnvironment::bindEnv(
    Environment & ) throw()
{
}

AutoDispose< Request >
TwoStageEnvironment::start( void ) throw()
{
  // TODO: implement this
  TOOLS_ASSERT( !"Not implemented" );
  return static_cast< Request * >( nullptr );
}

AutoDispose< Request >
TwoStageEnvironment::stop( void ) throw()
{
  // TODO: implement this
  TOOLS_ASSERT( !"Not implemented" );
  return static_cast< Request * >( nullptr );
}

namespace tools {
  ///////////////////////
  // NewSimpleEnvironment
  ///////////////////////
  Environment *
  NewSimpleEnvironment( AutoDispose<> & lifetime, StringId const & name )
  {
    AutoDispose< SimpleEnvironment > ret( new SimpleEnvironment( name ) );
    Environment * eret = ret.get();
    lifetime = std::move( ret );
    return eret;
  }

  /////////////////////////
  // NewTwoStageEnvironment
  /////////////////////////
  Environment *
  NewTwoStageEnvironment( AutoDispose<> & lifetime, StringId const & name )
  {
    AutoDispose< TwoStageEnvironment > ret( new TwoStageEnvironment( name ) );
    Environment * eret = ret.get();
    lifetime = std::move( ret );
    return eret;
  }
}; // namespace tools

#include <tools/UnitTest.h>
#if TOOLS_UNIT_TEST

namespace {
    struct EnvTestService {
        virtual bool haveStarted(void) = 0;
        virtual bool haveStopped(void) = 0;
    };

    struct EnvTestImpl
        : EnvTestService
        , tools::detail::StandardNoBindService< EnvTestImpl, boost::mpl::list< EnvTestService >::type >
    {
        EnvTestImpl(Environment &);

        // Service
        AutoDispose< Request > serviceStart(void);
        AutoDispose< Request > serviceStop(void);

        // EnvTestService
        bool haveStarted(void);
        bool haveStopped(void);

        bool started_;
        bool stopped_;
    };

    static RegisterEnvironment< EnvTestService, EnvTestImpl > regEnvTest;
}; // anonymous namespace

//////////////
// EnvTestImpl
//////////////

EnvTestImpl::EnvTestImpl(
    Environment & )
    : started_( false )
    , stopped_( false )
{
}

AutoDispose< Request >
EnvTestImpl::serviceStart( void )
{
    started_ = true;
    return static_cast< Request * >( nullptr );
}

AutoDispose< Request >
EnvTestImpl::serviceStop( void )
{
    stopped_ = true;
    return static_cast< Request * >( nullptr );
}

bool
EnvTestImpl::haveStarted( void )
{
    return started_;
}

bool
EnvTestImpl::haveStopped( void )
{
    return stopped_;
}

////////
// Tests
////////

TOOLS_TEST_CASE("Environment.empty", [](Test & test)
{
    TOOLS_ASSERTR(!IsNullOrEmptyStringId(test.environment().name()));
});

TOOLS_TEST_CASE("Environment.factory", [](Test & test)
{
    test.environment().unmock<EnvTestService>();
    auto svc = test.environment().get<EnvTestService>();
    TOOLS_ASSERTR(!!svc);
    TOOLS_ASSERTR(svc->haveStarted());
    TOOLS_ASSERTR(!svc->haveStopped());
});
#endif /* TOOLS_UNIT_TEST */
