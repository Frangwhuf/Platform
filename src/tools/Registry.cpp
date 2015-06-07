#include "toolsprecompiled.h"

#include <tools/Algorithms.h>
#include <tools/Concurrency.h>
#include <tools/Environment.h>
#include <tools/Registry.h>

#include <algorithm>
#include <unordered_map>
#include <vector>

using namespace tools;

namespace tools {
    namespace impl {
        AutoDispose< Monitor > monitorPlatformNew( void );
    }; // impl namespace
}; // tools namespace

namespace {
    struct RegistryKey
        : Disposable
    {
        // Disposable
        void dispose( void ) {
            itf_ = nullptr;
        }

        // local methods
        bool operator==( RegistryKey const & c ) const {
            return ( typeName_ == c.typeName_ ) && ( serviceName_ == c.serviceName_ );
        }

        StringId serviceName_;
        StringId typeName_;
        void * volatile itf_;
        RegistryKey * nextService_;
        RegistryKey * nextMap_;
    };

    TOOLS_FORCE_INLINE uint32 defineHashAny( RegistryKey const & key, uint32 initial )
    {
        return impl::hashMix( impl::hashAny( key.typeName_ ), impl::hashMix( impl::hashAny( key.serviceName_ ), initial ));
    }

    inline bool
    isEnd( RegistryKey const & key ) {
        return !key.itf_;
    }

    inline void
    setEnd( RegistryKey & key ) {
        key.itf_ = nullptr;
    }

    struct RegistryEnumerationRoot
        : RegistryEnumeration
    {
        RegistryEnumerationRoot( StringId const & );

        // RegistryEnumeration
        void visit( Visitor & );

        // local methods
        void post( RegistryKey * );

        RegistryKey * volatile typeRoot_;
    };

    struct RegistryGlobal
    {
        static size_t const registryMapBucketsUsed = 65536U;

        RegistryGlobal( void );
        ~RegistryGlobal( void );

        RegistryKey * findService( RegistryKey const &, RegistryKey * );
        RegistryKey * volatile * bucketOf( RegistryKey const & );
        void * peekService( RegistryKey const &, FactoryRegistry ** );
        void insertService( RegistryKey * );
        void * pokeFactoryService( RegistryKey const &, AutoDispose<> &, void *, FactoryRegistry & );

        RegistryKey * volatile services_[ registryMapBucketsUsed ];
    };

    struct AutoRegisterImpl
        : AutoRegister
        , Disposable
    {
        typedef std::unordered_set< StringId > TypeSet;  // TODO: switch to HashAnyOf<..>

        AutoRegisterImpl( StringId const & );

        // AutoRegister
        AutoDispose<> insert( StringId const &, void * );

        // Disposable
        void dispose( void );

        StringId serviceName_;
        TypeSet types_;
    };

    struct AutoRegisterFactory
        : FactoryRegistry
    {
        AutoDispose<> link( void **, StringId const & );
    };

    struct AnyServiceImpl
        : AnyService
    {
        AnyServiceImpl( StringId const & );

        StringId describe( void );
        void * get( StringId const & );

        StringId typeName_;
    };
};  // anonymous namespace

///////////////////////
// Non-member functions
///////////////////////

static void
registryInitRegistryEnumerationRoot( void ) {
    static RegisterFactoryRegistry< RegistryEnumerationRoot, RegistryEnumerationRoot > registerRegistryEnumRoot_;
}

TOOLS_NO_INLINE static RegistryGlobal &
registryMapGlobalInit( void )
{
    static RegistryGlobal map_;
    return map_;
}

TOOLS_NO_INLINE static RegistryGlobal &
registryMapAutoInit( void )
{
    static RegistryGlobal & map_ = registryMapGlobalInit();
    static AutoRegisterFactory autoRegisterFactory_;
    static AutoDispose<> autoRegister_( registryInsert< FactoryRegistry, AutoRegister >( &autoRegisterFactory_ ));
    return map_;
}

static RegistryGlobal &
registryMapGlobal( void )
{
    static RegistryGlobal & map_ = registryMapAutoInit();
    return map_;
}

namespace tools {
    AutoDispose<>
    registryInsert( StringId const & serviceName, StringId const & typeName, void * val ) {
        TOOLS_ASSERT( !!val );
        RegistryKey * i = new RegistryKey;
        i->serviceName_ = serviceName;
        i->typeName_ = typeName;
        i->itf_ = val;
        registryMapGlobalInit().insertService( i );
        if( typeName != nameOf< RegistryEnumerationRoot >() ) {
            // Make sure the root enumeration service is initialized, but don't re-enter here if we're
            // already in the middle of it.
            registryInitRegistryEnumerationRoot();
        }
        // Only do a visitor insert when this isn't a factory, this saves us some rather challenging recursion.
        if( serviceName != nameOf< FactoryRegistry >() ) {
            registryFetch< RegistryEnumerationRoot >( serviceName )->post( i );
        }
        return std::move(i);
    }

    void *
    registryFetch( StringId const & serviceName, StringId const & typeName ) {
        RegistryKey k;
        k.serviceName_ = serviceName;
        k.typeName_ = typeName;
        RegistryGlobal & r = registryMapGlobal();
        FactoryRegistry * factory = nullptr;
        if( void * service = r.peekService( k, &factory )) {
            return service;
        }
        if( !factory ) {
            return nullptr;
        }
        void * newService;
        AutoDispose<> newServiceDisp( factory->link( &newService, typeName ));
        if( !newService ) {
            TOOLS_ASSERT( !newServiceDisp );
            return nullptr;
        }
        TOOLS_ASSERT( !!newServiceDisp );
        return r.pokeFactoryService( k, newServiceDisp, newService, *factory );
    }
};  // tools namespace

//////////
// Statics
//////////

static RegisterFactoryRegistryFunctor< RegistryEnumeration > registerRegistryEnum( []( RegistryEnumeration ** bound, StringId const & name )->AutoDispose<> {
    *bound = registryFetch< RegistryEnumerationRoot >( name );
    return nullDisposable();
});

static RegisterFactoryRegistry< AnyService, AnyServiceImpl > registerAnyService;

//////////////////////////
// RegistryEnumerationRoot
//////////////////////////

RegistryEnumerationRoot::RegistryEnumerationRoot( StringId const & )
    : typeRoot_( nullptr )
{
}

void
RegistryEnumerationRoot::visit( Visitor & v )
{
    RegistryKey * key = typeRoot_;
    while( RegistryKey * i = key ) {
        key = i->nextService_;
        if( void * itf = i->itf_ ) {
            v.visit( i->typeName_, itf );
        }
    }
}

void
RegistryEnumerationRoot::post( RegistryKey * key )
{
    atomicPush( &typeRoot_, key, &RegistryKey::nextService_ );
}

/////////////////
// RegistryGlobal
/////////////////

RegistryGlobal::RegistryGlobal( void )
{
    std::fill( services_, services_ + registryMapBucketsUsed, static_cast< RegistryKey * >( nullptr ));
}

RegistryGlobal::~RegistryGlobal( void )
{
    std::for_each( services_, services_ + registryMapBucketsUsed, []( RegistryKey * bucket )->void {
        while( RegistryKey * i = bucket ) {
            bucket = i->nextMap_;
            TOOLS_ASSERT( isEnd( *i ));
            delete i;
        }
    });
}

RegistryKey *
RegistryGlobal::findService( RegistryKey const & key, RegistryKey * bucket )
{
    while( RegistryKey * i = bucket ) {
        if( ( i->typeName_ == key.typeName_ ) && ( i->serviceName_ == key.serviceName_ ) && !isEnd( *i )) {
            return i;
        }
        bucket = i->nextMap_;
    }
    return nullptr;
}

RegistryKey * volatile *
RegistryGlobal::bucketOf( RegistryKey const & key )
{
    return &services_[ tools::HashAnyOf< RegistryKey >()( key ) % registryMapBucketsUsed ];
}

void *
RegistryGlobal::peekService( RegistryKey const & key, FactoryRegistry ** factory )
{
    if( RegistryKey * existing = findService( key, *bucketOf( key ))) {
        return existing->itf_;
    }
    RegistryKey factoryKey;
    factoryKey.serviceName_ = nameOf< FactoryRegistry >();
    factoryKey.typeName_ = key.serviceName_;
    if( RegistryKey * factoryReg = findService( factoryKey, *bucketOf( factoryKey ))) {
        *factory = static_cast< FactoryRegistry * >( factoryReg->itf_ );
    } else {
        *factory = nullptr;
    }
    return nullptr;
}

void
RegistryGlobal::insertService( RegistryKey * key )
{
    atomicPush( bucketOf( *key ), key, &RegistryKey::nextMap_ );
}

void *
RegistryGlobal::pokeFactoryService( RegistryKey const & key, AutoDispose<> & serviceDisp, void * service, FactoryRegistry & factory )
{
    RegistryKey * i = new RegistryKey;
    *i = key;
    i->itf_ = service;
    // Race to insert
    RegistryKey * ret;
    atomicTryUpdate( bucketOf( key ), [&]( RegistryKey *& ref )->bool {
        if( RegistryKey * existing = findService( key, ref )) {
            ret = existing;
            return false;
        }
        ret = i;
        i->nextMap_ = ref;
        ref = i;
        return true;
    });
    if( ret != i ) {
        // Collision
        serviceDisp.release();
        delete i;
    } else {
        // Insert a record as a factory created service
        detail::FactoryInstance * svc = new detail::FactoryInstance;
        svc->insertion_ = i;
        svc->instance_ = std::move( serviceDisp );
        atomicPush( &factory.instances_, svc, &detail::FactoryInstance::next_ );
        // Add to the visiting list
        registryFetch< RegistryEnumerationRoot >( key.serviceName_ )->post( i );
    }
    return ret->itf_;
}

///////////////////
// AutoRegisterImpl
///////////////////

AutoRegisterImpl::AutoRegisterImpl( StringId const & name )
    : serviceName_( name )
{
}

AutoDispose<>
AutoRegisterImpl::insert( StringId const & name, void * itf )
{
    if( types_.find( name ) != types_.end() ) {
        return nullDisposable();
    }
    types_.insert( name );
    return registryInsert( serviceName_, name, itf );
}

void
AutoRegisterImpl::dispose( void )
{
    // nothing, just leak it
}

//////////////////////
// AutoRegisterFactory
//////////////////////

AutoDispose<>
AutoRegisterFactory::link( void ** bound, StringId const & name )
{
    AutoRegisterImpl * impl = new AutoRegisterImpl( name );
    *bound = impl;
    return std::move(impl);
}

//////////////////
// FactoryRegistry
//////////////////

namespace tools {
    FactoryRegistry::FactoryRegistry( void )
        : instances_( nullptr )
    {
        registryMapGlobalInit();
    }

    FactoryRegistry::~FactoryRegistry( void )
    {
        while( detail::FactoryInstance * i = instances_ ) {
            instances_ = i->next_;
            delete i;
        }
    }
};  // tools namespace

/////////////////
// AnyServiceImpl
/////////////////

AnyServiceImpl::AnyServiceImpl( StringId const & name )
    : typeName_( name )
{
}

StringId
AnyServiceImpl::describe( void )
{
    return typeName_;
}

void *
AnyServiceImpl::get( StringId const & serviceName )
{
    return registryFetch( serviceName, typeName_ );
}
