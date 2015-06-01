#include "toolsprecompiled.h"

#include <tools/AsyncTools.h>

#include "AssetImpl.h"

#ifdef WINDOWS_PLATFORM
#  pragma warning( disable : 4365 )
#  pragma warning( disable : 4371 )
#  pragma warning( disable : 4571 )
#endif // WINDOWS_PLATFORM
#include <boost/format.hpp>
#ifdef WINDOWS_PLATFORM
#  pragma warning( default : 4365 )
#  pragma warning( default : 4371 )
#  pragma warning( default : 4571 )
#endif // WINDOWS_PLATFORM

using namespace tools;

////////
// Types
////////

namespace {
    struct NullLoaderImpl
        : StandardManualRequest< NullLoaderImpl, AllocStatic<>, AssetLoader >
    {
        NullLoaderImpl( void );

        // AssetLoader
        void * getInterface( StringId const & ) const throw();
        void start( void );

        uint64 dummy_;
    };
};  // anonymous namespace

///////////////////////
// Non-member Functions
///////////////////////

StringId const &
tools::AssetPathThis( void )
{
    static StringId ret( "." );
    return ret;
}

StringId const &
tools::AssetPathParent( void )
{
    static StringId ret( ".." );
    return ret;
}

/////////////////////////
// InternalAssetSingleton
/////////////////////////

// This is a system internal type and applies to every actually loaded singleton
// singleton::getInterface - if singleton_ and Ready/Reloadable, pass through to singleton
//   if Invalid/NotFound try to get at some appropriate ErrorAsset, else nullptr
//   need to support something to get 'this' for equality compares
// singleton::name - name of just this part of the hierarchy
// singleton::path - a full path from the root to this asset, could be used to load this
// singleton::== - is this asset the same as the other?  You know since they are singleton..
// singleton::newSubscription - register a thunk to be called whenever state changes.
//   This should be reusable components, one for the subscription list management, one that
//   wraps a variable.
// singleton::status - return the current value of state
// singleton::reload - If not Reloadable, return this. Get this type from the parent.  If
//   that fails, return load(path()).  IF the current child[name()] of parent is !Reloadable,
//   return that.  Otherwise, return parent.load(name()).
// singleton::load - If Loading, return a wrapper with the load path.  If Ready/Reloadable,
//   get AssetSingleton: fail get an error asset; otherwise return newChild().  IF Dying/
//   Invalid/NotFound return error asset.
// singleton::constructor - takes a AssetDataLoader (Request (for load complete) + Subscribable
//   (change)), and an Asset (parent).  The singleton refs itself immediately, then starts
//   the loader request, that ref can be released when the request completes.  The singleton
//   also gets a subscription.  If that delivers it targets moving to Reloadable, after which
//   it can drop the subscription.  Reloadable is a terminal state.  The request completing
//   with error will lead to Invalid/NotFound, interface on loader will tell which.  When the
//   request completes without error, the state is still Loading, pass a non-transfer ref of
//   the loader to a node factory provided to the constructor.  There is a general one in
//   the Registry, otherwise the parent AssetSingleton can provide one of it's own.
// singleton::dispose - If the ref count is about to transition to 0 AND there is a singleton_,
//   remove ourselves from our parent, if we are there, then release singleton_ passing
//   this to pendingDispose().  If there is no singleton_, drop the parent reference and
//   delete.  This is likely easiest to use an inner Refable for this and have the real outter
//   one only used for parent and for that inner one to release in the end.  Though this
//   would make an extra indirection for all users.  So if we make the outter the special one,
//   maybe that will be better.  Somewhere in here need to change state to Dying.  May also
//   want to transition to Dying if parent transitions to Dying.  In this way the root can
//   purge all of its assets at shutdown.
//
// Wrapper waits for parent to reach a loaded state, then internally asks it to load the path
//   forwarding calls to an inner if there is no error.  It never goes to Ready, only to
//   Reloaded.  reload will return the inner Asset.

InternalAssetSingleton::InternalAssetSingleton( AutoDispose< Asset > & parent, AutoDispose< AssetLoader > & loader, StringId const & name, AssetSingletonFactory & factory )
    : parent_( std::move( parent ))
    , loader_( std::move( loader ))
    , factory_( factory )
    , state_( detail::AssetStateLoading, taskPublisherNew() )
    //, childrenLock_( monitorPoolNew( this ))
    , childrenLock_( monitorNew() )
    , reloaded_( false )
    , name_( name )
    , refs_( 2U )
{
    parentSub_ = parent_->newSubscription( toThunk< &InternalAssetSingleton::parentUpdate >(), toThunk< &InternalAssetSingleton::dispose >() );
    if( !parentSub_ ) {
        dispose();  // release reference for parent subscription
    }
    parentUpdate();
    if( !loader_ ) {
        // No loader, the factory must already have what it needs
        loaded(nullptr);
    } else {
        Subscribable * subable = loader_->getInterface< Subscribable >();
        if( !!subable ) {
            atomicRef( &refs_ );
            subscription_ = subable->newSubscription( toThunk< &InternalAssetSingleton::onChanged >(), toThunk< &InternalAssetSingleton::dispose >() );
            if( !subscription_ ) {
                dispose();  // release the ref made for the subscription
            }
        }
        atomicRef( &refs_ );
        loader_->start( toCompletion< &InternalAssetSingleton::loaded >() );
    }
}

void
InternalAssetSingleton::dispose( void )
{
    unsigned oldVal, newVal;
    do {
        oldVal = refs_;
        TOOLS_ASSERT( oldVal != 0U );
        newVal = oldVal - 1U;
    } while( atomicCas( &refs_, oldVal, newVal ) != oldVal );
    if(( newVal == 1U ) && !!parent_ ) {
        AutoDispose< Asset > dispParent( std::move( parent_ ));
        if( !!singleton_ ) {
            // We add a ref here so that when the parent releases its reference, our lifetime
            // is completely in the hands of the singleton
            atomicRef( &refs_ );
            AutoDispose<> handoff( this );
            singleton_->pendingDispose( handoff );
        }
    } else if( newVal == 0U ) {
        delete this;
    }
}

void *
InternalAssetSingleton::getInterface( StringId const & itf ) const throw()
{
    void * ret = nullptr;
    if( itf == nameOf< Asset >() ) {
        ret = static_cast< Asset * >( const_cast< InternalAssetSingleton * >( this ));
    } else if( itf == nameOf< AssetSingleton >() ) {
        ret = singleton_.get();
    } else {
        auto state = state_.get();
        if( !!singleton_ && (( state == detail::AssetStateReady ) || ( state == detail::AssetStateReloadable ))) {
            ret = singleton_->getInterface( itf );
        }
    }
    // TODO: do something about error state(s)
    return ret;
}

AutoDispose< Asset >
InternalAssetSingleton::load( StringId const & relpath ) throw()
{
    if( relpath == AssetPathThis() ) {
        // want another copy of this
        if( state_.get() == detail::AssetStateDying ) {
            return nullptr;
        }
        atomicRef( &refs_ );
        return this;
    }
    switch( state_.get() ) {
    case detail::AssetStateLoading:
        {
            atomicRef( &refs_ );
            AutoDispose< Asset > asset( this );
            return new InternalAssetFullLoadWrapper( asset, relpath, factory_ );
        }
    case detail::AssetStateReady:
        // TODO: need to handle full paths in some way
        {
            std::string nameStr( name_.c_str() );
            std::string relpathStr( relpath.c_str() );
            AutoDispose<> l( childrenLock_->enter() );
            Asset * child = nullptr;
            StringId residuePath;
            for( auto && element : children_ ) {
                if( element.first == relpath ) {
                    // full match!  Woo!
                    child = element.second.get();
                    residuePath = AssetPathThis();
                    break;
                } else {
                    std::string childStr( element.first.c_str() );
                    auto loc = relpathStr.find( element.first.c_str() );
                    if( loc != 0 ) {
                        // This child matches the front of the path we're looking for, explore this further.
                        child = element.second.get();
                        residuePath = relpathStr.substr( element.first.length() );
                        break;
                    }
                }
            }
            if( !child || ( child->status() == detail::AssetStateReloadable )) {
                // Did not find child in map, make one
                TOOLS_ASSERT( !!singleton_ );
                StringId match, childres;
                AutoDispose< AssetLoader > loader( singleton_->newChild( relpath, match, childres ));
                AutoDispose< Asset > thisWrap( new InternalAssetParentWrapper( *this, match ));
                if( IsNullOrEmptyStringId( childres )) {
                    AutoDispose< Asset > newSingleton( new InternalAssetSingleton( thisWrap, loader, match, factory_ ));
                    Asset * single = newSingleton.get();
                    children_[ match ] = std::move( newSingleton );
                    return single->load( AssetPathThis() );
                }
                InternalAssetSingleton * single = new InternalAssetSingleton( thisWrap, loader, match, factory_ );
                AutoDispose< Asset > newSingleton( single );
                AutoDispose< Asset > handOff( single->ref().release() );
                AutoDispose< Asset > terminal( new InternalAssetFullLoadWrapper( handOff, childres, factory_ ));
                TOOLS_ASSERT( !!terminal );
                children_[ match ] = std::move( newSingleton );
                return terminal->load( AssetPathThis() );
            }
            // Found child in map, and it's not reloadable, so use it to get a new reference
            return child->load( residuePath );
        }
    case detail::AssetStateReloadable:
        if( !!parent_ ) {
            boost::format fmt( "%s%s" );
            fmt % name_ % relpath;
            return parent_->load( fmt.str() );
        }
        break;
    case detail::AssetStateDying:
        // We're on our way out.
        return nullptr;
    case detail::AssetStateNotFound:
    case detail::AssetStateInvalid:
        atomicRef( &refs_ );
        return this;
    default:
        break;
    }
    return nullptr;
}

AutoDispose< Asset >
InternalAssetSingleton::reload( void ) throw()
{
    auto state = state_.get();
    if(( state == detail::AssetStateDying ) || !parent_ ) {
        return nullptr;
    }
    if( state != detail::AssetStateReloadable ) {
        return this;
    }
    TOOLS_ASSERT( !!parent_ );
    return parent_->load( name_ );
}

detail::AssetState
InternalAssetSingleton::status( void ) throw()
{
    return state_.get();
}

AutoDispose<>
InternalAssetSingleton::newSubscription( Thunk const & thunk, Thunk const & dead )
{
    return state_.newSubscription( thunk, dead );
}

bool
InternalAssetSingleton::operator==( AssetControl const & r ) throw()
{
    return ( r.getInterface< Asset >() == Unknown::getInterface< Asset >() );
}

StringId
InternalAssetSingleton::name( void ) throw()
{
    return name_;
}

StringId
InternalAssetSingleton::path( void ) throw()
{
    if( !!parent_ ) {
        boost::format fmt( "%s%s" );
        fmt % parent_->path() % name_;
        return fmt.str();
    }
    return name_;
}

AutoDispose< InternalAssetSingleton >
InternalAssetSingleton::ref( void ) const throw()
{
    atomicIncrement( &refs_ );
    return const_cast< InternalAssetSingleton * >( this );
}

void
InternalAssetSingleton::loaded( Error * err )
{
    if( !err ) {
        singleton_ = factory_.factory( loader_, name_ );
        {
            if( state_.get() != detail::AssetStateDying ) {
                if( reloaded_ ) {
                    state_.set( detail::AssetStateReloadable );
                } else {
                    state_.set( detail::AssetStateReady );
                }
            }
        }
    } else {
        {
            // TODO: set appropriate error info
            state_.set( detail::AssetStateNotFound );
        }
    }
    if( !!loader_ ) {
        dispose();  // deref for the loading request
    }
}

void
InternalAssetSingleton::onChanged( void )
{
    AutoDispose<> toDisp( std::move( subscription_ ));
    {
        auto state = state_.get();
        if( state == detail::AssetStateDying ) {
            return;
        } else if( state == detail::AssetStateLoading ) {
            reloaded_ = true;
        } else {
            state_.set( detail::AssetStateReloadable );
        }
    }
}

void
InternalAssetSingleton::parentUpdate( void )
{
    if( !parent_ ) {
        return;
    }
    switch( parent_->status() ) {
    case detail::AssetStateLoading:
    case detail::AssetStateReady:
        break;  // Nothing really to do with this information
    case detail::AssetStateReloadable:
        switch( state_.get() ) {
        case detail::AssetStateLoading:
        case detail::AssetStateReloadable:
        case detail::AssetStateDying:
            break;
        case detail::AssetStateReady:
            if( !loader_ ) {
                // If we don't have a loader, this sould be our only sign of reload
                state_.set( detail::AssetStateReloadable );
            }
            break;
        case detail::AssetStateNotFound:
        case detail::AssetStateInvalid:
            {
                state_.set( detail::AssetStateReloadable );
            }
            break;
        default:
            break;
        }
        break;
    case detail::AssetStateDying:
        {  // Dispose whatever we can and set ourselves to Dying
            AutoDispose< Asset > dispParent( std::move( parent_ ));
            AutoDispose< AssetSingleton > dispSingleton( std::move( singleton_ ));
            AutoDispose<> dispSubscription( std::move( subscription_ ));
            AutoDispose<> dispParentSub( std::move( parentSub_ ));
            state_.set( detail::AssetStateDying );
        }
        break;
    case detail::AssetStateNotFound:
        {
            state_.set( detail::AssetStateNotFound );  // if our parent is busted, we should be too
        }
        break;
    case detail::AssetStateInvalid:
        {
            state_.set( detail::AssetStateInvalid );  // if our parent is busted, we should be too
        }
        break;
    default:
        break;
    }
}

///////////////////////////
// InternalAssetPerInstance
///////////////////////////

// This is a system internal type and applies to every per-instance type.
// per_inst::getInterface - if instance_ and Ready or Reloadable, pass through to instance
//    if Invalid/NotFound pass through to singleton (if present), else nullptr
//    need to support something to get the inner singleton asset
// per_inst::name - name of just this part of the heirarchy
// per_inst::path - a full path from the root to this asset, could be used to load this
// per_inst::== - are the singleton assets the same, do this via getInterface
// per_inst::newSubscription - register a thunk to be called whenever state changes.
//    This should be reusable components, one for the subscription list management, one that
//    wraps a variable
// per_inst::status - return the current value of state
// per_inst::reload - set state to Loading, replace the singleton with the most current
//    (discarding the current inner subscription), get a new inner subscription, call the
//    state update thunk, return this
// per_inst::load - new per_inst around asset returned by singleton->load, let it evolve.
//    In order for this not itself to be a request, this returned asset needs to be a hybrid
//    wrapper, not the final singleton(s) that will be created.  Internally it waits for
//    its current eval to be Ready/Reloadable, then uses newChild to get the next part.  Wait
//    on that, repeat.  Until you get to the requested terminal child.  These inner assets
//    are true system singletons.  This wrapper asset will transition directly from Loading
//    to Reloadable.  Reloading it will collapse the wrapper to the current version of the
//    terminal singleton.
// per_inst::constructor - given the singleton it is an instance of.  Get subscription and
//    call delivery  func.
//
// state evolution in per_inst, indent to singleton new state
// Loading (initial)
//   Loading - nothing
//   Ready -
//     if can't get AssetSingleton from singleton -> Invalid
//     call newInstance(..., this->instance), if return a request wait on that request then..
//       no intermediate instance pointer, because there isn't one to replace
//     if err -> correct error state
//     otherwise -> Ready and call state update (this logic)
//   Reloadable -
//     if can't get AssetSingleton; drop subscription, swap to most current singleton, get
//       new subscription, call state update (this logic)
//     call newInstance(..., this->instance), if return a request wait on that request then..
//     if err -> drop subscription, swap to most current singleton, get new subscription,
//       call state update (this logic)
//     otherwise -> Reloadable
//   Dying - drop subscription, release singleton and instance, -> Dying
//   NotFound - -> NotFound
//   Invalid - -> Invalid
// Ready
//   Loading - not a valid transition
//   Ready - nothing
//   Reloadable -> Reloadable
//   Dying - drop subscription, release singleton and instance, -> Dying
//   NotFound - not a valid transition
//   Invalid - not a valid transition
// Reloadable
//   Loading - not a valid transition
//   Ready - not a valid transition
//   Reloadable - nothing
//   Dying - drop subscription, release singleton and instance, -> Dying
//   NotFound - not a valid transition
//   Invalid - not a valid transition
// Dying - N/A subscriptions dropped
// NotFound
//   Loading - not a valid transition
//   Ready - not a valid transition
//   Reloadable -> Reloadable
//   Dying - drop subscription, release singleton and instance, -> Dying
//   NotFound - nothing
//   Invalid - not a valid transition
// Invalid
//   Loading - not a valid transition
//   Ready - not a valid transition
//   Reloadable -> Reloadable
//   Dying - drop subscription, release singleton and instance, -> Dying
//   NotFound - not a valid transition
//   Invalid - nothing
//
// Wrapper waits for parent to reach a loaded state, then internally asks it to load the path
//   forwarding calls to an inner if there is no error.  It never goes to Ready, only to
//   Reloaded.  reload will return the inner Asset.

InternalAssetPerInstance::InternalAssetPerInstance( AutoDispose< Asset > & singleton )
    : singleton_( std::move( singleton ))
    , state_( detail::AssetStateLoading, taskPublisherNew() )
    , refs_( 1U )
{
    atomicRef( &refs_ );
    subscription_ = singleton_->newSubscription( toThunk< &InternalAssetPerInstance::singletonUpdate >(), toThunk< &InternalAssetPerInstance::dispose >() );
    if( !subscription_ ) {
        dispose();
    }
    singletonUpdate();
}

void
InternalAssetPerInstance::dispose( void )
{
    if( !atomicDeref( &refs_ )) {
        delete this;
    }
}

void *
InternalAssetPerInstance::getInterface( StringId const & itf ) const throw()
{
    void * ret = nullptr;
    if( itf == nameOf< Asset >() ) {
        ret = singleton_.get();
    } else {
        auto state = state_.get();
        if( !!instance_ && (( state == detail::AssetStateReady ) || ( state == detail::AssetStateReloadable ))) {
            ret = instance_->getInterface( itf );
        }
    }
    // TODO: handle error states
    return ret;
}

AutoDispose< Asset >
InternalAssetPerInstance::load( StringId const & relpath ) throw()
{
    AutoDispose< Asset > inner( singleton_->load( relpath ));
    return new InternalAssetPerInstance( inner );
}

AutoDispose< Asset >
InternalAssetPerInstance::reload( void ) throw()
{
    subscription_.release();  // drop the old subscription
    state_.set( detail::AssetStateLoading );
    instance_.release();
    singleton_ = singleton_->load( AssetPathThis() );
    atomicRef( &refs_ );
    subscription_ = singleton_->newSubscription( toThunk< &InternalAssetPerInstance::singletonUpdate >(), toThunk< &InternalAssetPerInstance::dispose >() );
    if( !!subscription_ ) {
        dispose();
    }
    singletonUpdate();
    return ref().release();
}

detail::AssetState
InternalAssetPerInstance::status( void ) throw()
{
    return state_.get();
}

AutoDispose<>
InternalAssetPerInstance::newSubscription( Thunk const & thunk, Thunk const & dead )
{
    return state_.newSubscription( thunk, dead );
}

bool
InternalAssetPerInstance::operator==( AssetControl const & r ) throw()
{
    return ( r.getInterface< Asset >() == singleton_.get() );
}

StringId
InternalAssetPerInstance::name( void ) throw()
{
    TOOLS_ASSERT( !!singleton_ );
    return singleton_->name();
}

StringId
InternalAssetPerInstance::path( void ) throw()
{
    TOOLS_ASSERT( !!singleton_ );
    return singleton_->path();
}

void
InternalAssetPerInstance::singletonUpdate( void )
{
    switch( state_.get() ) {
    case detail::AssetStateLoading:
        switch( singleton_->status() ) {
        case detail::AssetStateLoading:
            // no change
            return;
        case detail::AssetStateReady:
            {
                AssetSingleton * semantic = singleton_->getInterface< AssetSingleton >();
                if( !semantic ) {
                    state_.set( detail::AssetStateInvalid );
                    // TODO: set error info
                    return;
                }
                TOOLS_ASSERT( !instanceReq_ );
                target_ = detail::AssetStateReady;
                instanceReq_ = semantic->newInstance( instance_ );
                if( !instanceReq_ ) {
                    instanceCompleted( nullptr );
                } else {
                    instanceReq_->start( toCompletion< &InternalAssetPerInstance::instanceCompleted >() );
                }
            }
            break;
        case detail::AssetStateReloadable:
            {
                AssetSingleton * semantic = singleton_->getInterface< AssetSingleton >();
                if( !semantic ) {
                    subscription_.release();
                    singleton_ = singleton_->reload();
                    atomicRef( &refs_ );
                    subscription_ = singleton_->newSubscription( toThunk< &InternalAssetPerInstance::singletonUpdate >(), toThunk< &InternalAssetPerInstance::dispose >() );
                    if( !subscription_ ) {
                        dispose();
                    }
                    singletonUpdate();
                    return;
                }
                target_ = detail::AssetStateReloadable;
                instanceReq_ = semantic->newInstance( instance_ );
                if( !instanceReq_ ) {
                    instanceCompleted( nullptr );
                } else {
                    instanceReq_->start( toCompletion< &InternalAssetPerInstance::instanceCompleted >() );
                }
            }
            break;
        case detail::AssetStateDying:
            {
                subscription_.release();
                instance_.release();
                singleton_.release();
                state_.set( detail::AssetStateDying );
            }
            break;
        case detail::AssetStateNotFound:
            state_.set( detail::AssetStateNotFound );
            break;
        case detail::AssetStateInvalid:
            state_.set( detail::AssetStateInvalid );
            break;
        default:
            TOOLS_ASSERT( !"Unknown Asset state." );
            // TODO: transition to Invalid with error information about bad state
            break;
        }
        break;
    case detail::AssetStateReady:
        switch( singleton_->status() ) {
        case detail::AssetStateLoading:
            TOOLS_ASSERT( !"Invalid Asset transition from Ready to Loading" );
            state_.set( detail::AssetStateInvalid );
            // TODO: transition to Invalid with error information about bad state
            break;
        case detail::AssetStateReady:
            // no change
            return;
        case detail::AssetStateReloadable:
            state_.set( detail::AssetStateReloadable );
            break;
        case detail::AssetStateDying:
            {
                subscription_.release();
                instance_.release();
                singleton_.release();
                state_.set( detail::AssetStateDying );
            }
            break;
        case detail::AssetStateNotFound:
            TOOLS_ASSERT( !"Invalid Asset transition from Ready to NotFound" );
            state_.set( detail::AssetStateInvalid );
            // TODO: transition to Invalid with error information about bad state
            break;
        case detail::AssetStateInvalid:
            TOOLS_ASSERT( !"Invalid Asset transition from Ready to Invalid" );
            state_.set( detail::AssetStateInvalid );
            // TODO: transition to Invalid with error information about bad state
            break;
        default:
            TOOLS_ASSERT( !"Unknown Asset state." );
            // TODO: transition to Invalid with error information about bad state
            break;
        }
        break;
    case detail::AssetStateReloadable:
        switch( singleton_->status() ) {
        case detail::AssetStateLoading:
            TOOLS_ASSERT( !"Invalid Asset transition from Reloadable to Loading" );
            state_.set( detail::AssetStateInvalid );
            // TODO: transition to Invalid with error information about bad state
            break;
        case detail::AssetStateReady:
            TOOLS_ASSERT( !"Invalid Asset transition from Reloadable to Ready" );
            state_.set( detail::AssetStateInvalid );
            // TODO: transition to Invalid with error information about bad state
            break;
        case detail::AssetStateReloadable:
            // no change
            return;
        case detail::AssetStateDying:
            {
                subscription_.release();
                instance_.release();
                singleton_.release();
                state_.set( detail::AssetStateDying );
            }
            break;
        case detail::AssetStateNotFound:
            TOOLS_ASSERT( !"Invalid Asset transition from Reloadable to NotFound" );
            state_.set( detail::AssetStateInvalid );
            // TODO: transition to Invalid with error information about bad state
            break;
        case detail::AssetStateInvalid:
            TOOLS_ASSERT( !"Invalid Asset transition from Reloadable to Invalid" );
            state_.set( detail::AssetStateInvalid );
            // TODO: transition to Invalid with error information about bad state
            break;
        default:
            TOOLS_ASSERT( !"Unknown Asset state." );
            // TODO: transition to Invalid with error information about bad state
            break;
        }
        break;
    case detail::AssetStateDying:
        return;
    case detail::AssetStateNotFound:
        switch( singleton_->status() ) {
        case detail::AssetStateLoading:
            TOOLS_ASSERT( !"Invalid Asset transition from NotFound to Loading" );
            state_.set( detail::AssetStateInvalid );
            // TODO: transition to Invalid with error information about bad state
            break;
        case detail::AssetStateReady:
            TOOLS_ASSERT( !"Invalid Asset transition from NotFound to Ready" );
            state_.set( detail::AssetStateInvalid );
            // TODO: transition to Invalid with error information about bad state
            break;
        case detail::AssetStateReloadable:
            state_.set( detail::AssetStateReloadable );
            break;
        case detail::AssetStateDying:
            {
                subscription_.release();
                instance_.release();
                singleton_.release();
                state_.set( detail::AssetStateDying );
            }
            break;
        case detail::AssetStateNotFound:
            // no change
            return;
        case detail::AssetStateInvalid:
            TOOLS_ASSERT( !"Invalid Asset transition from NotFound to Invalid" );
            state_.set( detail::AssetStateInvalid );
            // TODO: transition to Invalid with error information about bad state
            break;
        default:
            TOOLS_ASSERT( !"Unknown Asset state." );
            // TODO: transition to Invalid with error information about bad state
            break;
        }
        break;
    case detail::AssetStateInvalid:
        switch( singleton_->status() ) {
        case detail::AssetStateLoading:
            TOOLS_ASSERT( !"Invalid Asset transition from Invalid to Loading" );
            state_.set( detail::AssetStateInvalid );
            // TODO: transition to Invalid with error information about bad state
            break;
        case detail::AssetStateReady:
            TOOLS_ASSERT( !"Invalid Asset transition from Invalid to Ready" );
            state_.set( detail::AssetStateInvalid );
            // TODO: transition to Invalid with error information about bad state
            break;
        case detail::AssetStateReloadable:
            state_.set( detail::AssetStateReloadable );
            break;
        case detail::AssetStateDying:
            {
                subscription_.release();
                instance_.release();
                singleton_.release();
                state_.set( detail::AssetStateDying );
            }
            break;
        case detail::AssetStateNotFound:
            TOOLS_ASSERT( !"Invalid Asset transition from Invalid to NotFound" );
            state_.set( detail::AssetStateInvalid );
            // TODO: transition to Invalid with error information about bad state
            break;
        case detail::AssetStateInvalid:
            // no change
            return;
        default:
            TOOLS_ASSERT( !"Unknown Asset state." );
            // TODO: transition to Invalid with error information about bad state
            break;
        }
        break;
    default:
        TOOLS_ASSERT( !"Unknown Asset state." );
        // TODO: transition to Invalid with error information about bad state
        break;
    }
}

AutoDispose< InternalAssetPerInstance >
InternalAssetPerInstance::ref( void ) const throw()
{
    atomicRef( &refs_ );
    return const_cast< InternalAssetPerInstance * >( this );
}

void
InternalAssetPerInstance::instanceCompleted(
    Error * err )
{
    {
        AutoDispose< Request > disp( std::move( instanceReq_ ));
        if( !!err ) {
            state_.set( detail::AssetStateInvalid );
            // TODO: add some deeper error information
            return;
        }
        if( !instance_ ) {
            state_.set( detail::AssetStateInvalid );
            // TODO: add some deeper error information
            return;
        }
        state_.set( target_ );
    }
    singletonUpdate();
}

/////////////////////////////
// InternalAssetParentWrapper
/////////////////////////////

InternalAssetParentWrapper::InternalAssetParentWrapper( InternalAssetSingleton & asset, StringId const & name )
    : asset_( asset.ref() )
    , name_( name )
{
}

InternalAssetParentWrapper::~InternalAssetParentWrapper( void )
{
    AutoDispose<> l( asset_->childrenLock_->enter() );
    auto iter = asset_->children_.find( name_ );
    if( iter != asset_->children_.end() ) {
        asset_->children_.erase( iter );
    }
}

void
InternalAssetParentWrapper::dispose( void )
{
    delete this;
}

void *
InternalAssetParentWrapper::getInterface( StringId const & itf ) const throw()
{
    return asset_->getInterface( itf );
}

AutoDispose< Asset >
InternalAssetParentWrapper::load( StringId const & relpath ) throw()
{
    return asset_->load( relpath );
}

AutoDispose< Asset >
InternalAssetParentWrapper::reload( void ) throw()
{
    return asset_->reload();
}

detail::AssetState
InternalAssetParentWrapper::status( void ) throw()
{
    return asset_->status();
}

AutoDispose<>
InternalAssetParentWrapper::newSubscription( Thunk const & thunk, Thunk const & dead )
{
    return asset_->newSubscription( thunk, dead );
}

bool
InternalAssetParentWrapper::operator==( AssetControl const & r ) throw()
{
    return asset_->operator==( r );
}

StringId
InternalAssetParentWrapper::name( void ) throw()
{
    return asset_->name();
}

StringId
InternalAssetParentWrapper::path( void ) throw()
{
    return asset_->path();
}

///////////////////////////////
// InternalAssetFullLoadWrapper
///////////////////////////////

InternalAssetFullLoadWrapper::InternalAssetFullLoadWrapper( AutoDispose< Asset > & parent, StringId const & residue, AssetSingletonFactory & factory )
    : parent_( std::move( parent ))
    , residuePath_( residue )
    , factory_( factory )
    , publisher_( taskPublisherNew() )
    , refs_( 2U )
{
    subscription_ = parent_->newSubscription( toThunk< &InternalAssetFullLoadWrapper::stateUpdate >(), toThunk< &InternalAssetFullLoadWrapper::dispose >() );
    if( !subscription_ ) {
        dispose();  // no subscription, no reference
    }
    stateUpdate();
}

void
InternalAssetFullLoadWrapper::dispose( void )
{
    if( !atomicDeref( &refs_ )) {
        delete this;
    }
}

void *
InternalAssetFullLoadWrapper::getInterface( StringId const & itf ) const throw()
{
    void * ret = nullptr;
    if( itf == nameOf< Asset >() ) {
        ret = static_cast< Asset * >( const_cast< InternalAssetFullLoadWrapper * >( this ));
    } else if( !!asset_ ) {
        ret = asset_->getInterface( itf );
    }
    return ret;
}

AutoDispose< Asset >
InternalAssetFullLoadWrapper::load( StringId const & relpath ) throw()
{
    if( relpath == AssetPathThis() ) {
        // want another copy of 'this'
        if( parent_->status() == detail::AssetStateDying ) {
            return nullptr;
        }
        if( !asset_ ) {
            atomicRef( &refs_ );
            return this;
        }
        return asset_->load( relpath );
    }
    if (!!asset_) {
        return asset_->load(relpath);
    }
    AutoDispose< Asset > thisRef( this->ref() );
    return new InternalAssetFullLoadWrapper( thisRef, relpath, factory_ );
}

AutoDispose< Asset >
InternalAssetFullLoadWrapper::reload( void ) throw()
{
    if( !!asset_ ) {
        return asset_->reload();
    }
    if( parent_->status() == detail::AssetStateDying ) {
        return nullptr;
    }
    return this;
}

detail::AssetState
InternalAssetFullLoadWrapper::status( void ) throw()
{
    if( !!asset_ ) {
        return asset_->status();
    }
    switch( parent_->status() ) {
    case detail::AssetStateLoading:
    case detail::AssetStateReady:
    case detail::AssetStateReloadable:
        return detail::AssetStateLoading;
    case detail::AssetStateDying:
        return detail::AssetStateDying;
    case detail::AssetStateNotFound:
        return detail::AssetStateNotFound;
    case detail::AssetStateInvalid:
    default:
        break;
    }
    return detail::AssetStateInvalid;
}

AutoDispose<>
InternalAssetFullLoadWrapper::newSubscription( Thunk const & thunk, Thunk const & dead )
{
    return publisher_->newSubscription(thunk, dead);
}

bool
InternalAssetFullLoadWrapper::operator==( AssetControl const & r ) throw()
{
    return ( r.getInterface< Asset >() == Unknown::getInterface< Asset >() );
}

StringId
InternalAssetFullLoadWrapper::name( void ) throw()
{
    if( !!asset_ ) {
        return asset_->name();
    }
    return residuePath_;
}

StringId
InternalAssetFullLoadWrapper::path( void ) throw()
{
    if( !!asset_ ) {
        return asset_->path();
    }
    boost::format fmt( "%s%s" );
    fmt % parent_->path() % residuePath_;
    return fmt.str();
}

void
InternalAssetFullLoadWrapper::stateUpdate( void )
{
    bool makeChild = false;
    bool releaseChild = false;
    switch( parent_->status() ) {
    case detail::AssetStateLoading:
        // No change
        break;
    case detail::AssetStateReady:
    case detail::AssetStateReloadable:
        makeChild = true;
        break;
    case detail::AssetStateDying:
    case detail::AssetStateNotFound:
    case detail::AssetStateInvalid:
    default:
        releaseChild = true;
        break;
    }
    if( makeChild ) {
        TOOLS_ASSERT( !!parent_ );
        AutoDispose< Asset > newChild( parent_->load( residuePath_ ));
        AutoDispose<> newChildSub;
        if( !!newChild ) {
            atomicRef( &refs_ );
            newChildSub = newChild->newSubscription( toThunk< &InternalAssetFullLoadWrapper::childInvalidate >(), toThunk< &InternalAssetFullLoadWrapper::dispose >() );
            if( !newChildSub ) {
                // No subscription, no reference
                atomicDeref( &refs_ );
            }
            asset_.swap( newChild );
            childSubscription_.swap( newChildSub );
            childInvalidate();  // this will invalidate publisher_
        } else {
            publisher_->invalidate();
        }
    } else {
        if( releaseChild ) {
            childSubscription_.reset();
            asset_.reset();
        }
        publisher_->invalidate();  // any change should be communicated
    }
}

AutoDispose< Asset >
InternalAssetFullLoadWrapper::ref( void ) const throw()
{
    atomicRef( &refs_ );
    return const_cast< InternalAssetFullLoadWrapper * >( this );
}

void
InternalAssetFullLoadWrapper::childInvalidate( void )
{
    if( !asset_ ) {
        return;
    }
    //switch( asset_->status() ) {
    //case detail::AssetStateLoading:
    //case detail::AssetStateReady:
    //case detail::AssetStateReloadable:
    //case detail::AssetStateDying:
    //case detail::AssetStateNotFound:
    //case detail::AssetStateInvalid:
    //default:
    //    break;
    //}
    publisher_->invalidate();  // We are a proxy for this asset, so forward this invalidation
}

/////////////////
// NullLoaderImpl
/////////////////

NullLoaderImpl::NullLoaderImpl( void )
{
}

void *
NullLoaderImpl::getInterface( StringId const & itf ) const throw()
{
    if( nameOf< NullLoaderImpl >() == itf ) {
        return const_cast< NullLoaderImpl * >(this);
    }
    return nullptr;
}

void
NullLoaderImpl::start( void )
{
    finish();
}

#include <tools/UnitTest.h>
#if TOOLS_UNIT_TEST

struct TestAssetRoot;

struct TestAssetFactory
    : AssetSingletonFactory
{
    TestAssetFactory( void );
    ~TestAssetFactory( void );

    // AssetSingletonFactory
    AutoDispose< AssetSingleton > factory( AutoDispose< AssetLoader > const &, StringId const & );

    unsigned volatile singletons_;
    TestAssetRoot * root_;
};

struct TestAssetRoot
    : Asset
{
    TestAssetRoot( void );
    ~TestAssetRoot( void );

    // Asset
    void dispose( void );
    void * getInterface( StringId const & ) const throw();
    AutoDispose< Asset > load( StringId const & ) throw();
    AutoDispose< Asset > reload( void ) throw();
    detail::AssetState status( void ) throw();
    AutoDispose<> newSubscription( Thunk const &, Thunk const & );
    bool operator==( AssetControl const & ) throw();
    StringId name( void ) throw();
    StringId path( void ) throw();

    // Local methods
    void setState( detail::AssetState );

    AutoDispose< Publisher > publisher_;
    detail::AssetState state_;
    AutoDispose< Asset > root_;
    TestAssetFactory factory_;
    unsigned volatile refs_;
};

struct TestAssetSingleton
    : StandardDisposable< TestAssetSingleton, AssetSingleton >
{
    TestAssetSingleton( TestAssetFactory &, StringId const & );
    ~TestAssetSingleton( void );

    // AssetSingleton
    void * getInterface( StringId const & ) const throw();
    AutoDispose< Request > newInstance( AutoDispose< AssetInstance > & );
    AutoDispose< AssetLoader > newChild( StringId const &, StringId & match, StringId & residue );
    void pendingDispose( AutoDispose<> & );

    TestAssetFactory & factory_;
    StringId name_;
};

///////////////////
// TestAssetFactory
///////////////////

TestAssetFactory::TestAssetFactory( void )
    : singletons_( 0U )
    , root_( nullptr )
{
}

TestAssetFactory::~TestAssetFactory( void )
{
    TOOLS_ASSERT( singletons_ == 0U );
}

AutoDispose< AssetSingleton >
TestAssetFactory::factory( AutoDispose< AssetLoader > const & loader, StringId const & name )
{
    TOOLS_ASSERT( !!loader->getInterface< NullLoaderImpl >() );
    atomicIncrement( &singletons_ );
    return new TestAssetSingleton( *this, name );
}

////////////////
// TestAssetRoot
////////////////

TestAssetRoot::TestAssetRoot( void )
    : publisher_( taskPublisherNew() )
    , state_( detail::AssetStateLoading )
    , refs_( 1U )
{
    factory_.root_ = this;
}

TestAssetRoot::~TestAssetRoot( void )
{
    // TODO: anything to check here?
}

void
TestAssetRoot::dispose( void )
{
    if( !atomicDeref( &refs_ )) {
        delete this;
    }
}

void *
TestAssetRoot::getInterface( StringId const & ) const throw()
{
    return nullptr;
}

AutoDispose< Asset >
TestAssetRoot::load( StringId const & relpath ) throw()
{
    if( !root_ ) {
        atomicRef( &refs_ );
        AutoDispose< AssetLoader > loader( new NullLoaderImpl() );
        AutoDispose< Asset > parent( this );
        root_ = new InternalAssetSingleton( parent, loader, name(), factory_ );
        TOOLS_ASSERT( !!root_ );
    }
    return root_->load( relpath );
}

AutoDispose< Asset >
TestAssetRoot::reload( void ) throw()
{
    atomicRef( &refs_ );
    return this;
}

detail::AssetState
TestAssetRoot::status( void ) throw()
{
    return state_;
}

AutoDispose<>
TestAssetRoot::newSubscription( Thunk const & thunk, Thunk const & dead )
{
    TOOLS_ASSERT( state_ != detail::AssetStateDying );
    return publisher_->newSubscription( thunk, dead );
}

bool
TestAssetRoot::operator==( AssetControl const & other ) throw()
{
    return static_cast< AssetControl * >( this ) == &other;
}

StringId
TestAssetRoot::name( void ) throw()
{
    return StringIdEmpty();
}

StringId
TestAssetRoot::path( void ) throw()
{
    return name();
}

void
TestAssetRoot::setState( detail::AssetState st )
{
    state_ = st;
    publisher_->invalidate();
}

/////////////////////
// TestAssetSingleton
/////////////////////

TestAssetSingleton::TestAssetSingleton( TestAssetFactory & factory, StringId const & name )
    : factory_( factory )
    , name_( name )
{
}

TestAssetSingleton::~TestAssetSingleton( void )
{
    atomicDecrement( &factory_.singletons_ );
}

void *
TestAssetSingleton::getInterface( StringId const & ) const throw()
{
    return nullptr;
}

AutoDispose< Request >
TestAssetSingleton::newInstance( AutoDispose< AssetInstance > & inst )
{
    // TODO: sometimes return a request, sometimes return not this
    inst = this;
    return static_cast< Request * >( nullptr );
}

AutoDispose< AssetLoader >
TestAssetSingleton::newChild( StringId const & relpath, StringId & match, StringId & residue )
{
    auto begin = relpath.c_str();
    auto end = begin + relpath.length();
    // For the test assets, we use ' ' as path seperators
    auto first = std::find( begin, end, ' ' );
    if( first == end ) {
        match = relpath;
        residue = StringIdNull();
    } else {
        ++first;
        match = StringId( begin, (first - begin ));
        residue = StringId( first );
    }
    return new NullLoaderImpl();
}

void
TestAssetSingleton::pendingDispose( AutoDispose<> & disp )
{
    AutoDispose<> toDisp( std::move( disp ));
}

////////
// Tests
////////

void
TestTestAssetRootCreate( void )
{
    AutoDispose< TestAssetRoot > root( new TestAssetRoot() );
}

TOOLS_UNIT_TEST_FUNCTION( TestTestAssetRootCreate );

void
TestSingleAssetCreate( void )
{
    AutoDispose< TestAssetRoot > root( new TestAssetRoot() );
    {
        AutoDispose< Asset > asset( root->load( "foo" ));
        TOOLS_ASSERT( !!asset );
        TOOLS_ASSERT( asset->status() == detail::AssetStateReady );
    }
    root->setState( detail::AssetStateDying );
}

TOOLS_UNIT_TEST_FUNCTION( TestSingleAssetCreate );

#endif // TOOLS_UNIT_TEST
