#pragma once

#include <tools/Concurrency.h>
#include <tools/InterfaceTools.h>
#include <tools/Async.h>

namespace tools {
    // Abstract interface for things that can be invalidated.
    struct Invalidatable
    {
        virtual void invalidate( void ) = 0;
    };

    // The abstract interface for something that can be subscribed to, providing invalidation
    // callbacks on possible changes.  Subscription does not guarentee that the value/state
    // has actually changed, only that it may have changed.  That is, whatever the usage site
    // thought it knew about the value/state, is no longer valid.
    struct Subscribable {
        // Start a new subscription.  Whenever the value/state may have changed, the provided
        // thunk will be called.  To stop receiving invalidations, dispose the returned
        // Disposable.  The implementation should not initiate a call to the provided thunk
        // once the returned Disposable has completed disposing  (returned to the caller).
        // Though any call that had already been initiated will still be made.  As such it is
        // best keep the work in the thunk simple and short, and to try to have the host of
        // the thunk live until all threads have returned to idle.
        virtual AutoDispose<> newSubscription( Thunk const &, Thunk const & ) = 0;
    };
    
    // Abstract interface for a lifetime-controlled entity that provides a particular subscription,
    // as well as a mechanism for triggering invalidation of all subscribers.
    struct Publisher
        : Disposable
        , Subscribable
        , Invalidatable
    {};

    // A simple, but always safe implementation of Publisher.
    TOOLS_API AutoDispose< Publisher > simplePublisherNew( impl::ResourceSample const & );
    inline AutoDispose< Publisher > simplePublisherNew( void )
    {
        return tools::simplePublisherNew( TOOLS_RESOURCE_SAMPLE_CALLER( 0 ) );
    }

    // An implementation of Publisher that deisvers its subscriptions on tasks.  Likely
    // delivering many in parallel.
    TOOLS_API AutoDispose< Publisher > taskPublisherNew( impl::ResourceSample const & );
    inline AutoDispose< Publisher > taskPublisherNew( void )
    {
        return tools::taskPublisherNew( TOOLS_RESOURCE_SAMPLE_CALLER( 0 ) );
    }

    // A lifetime helper for subscriptions
    template< typename AffinityT = tools::Inherent >
    struct SubscriptionLifetime
        : tools::Disposable
        , tools::Notifiable< SubscriptionLifetime< AffinityT >>
        , tools::AllocStatic< AffinityT >
    {
        SubscriptionLifetime( Subscribable & sub, Thunk const & thunk )
	    : subscription_( sub.newSubscription( thunk, this->template toThunk< &SubscriptionLifetime::onDeath >() ))
        {}

        // Disposable
        void
        dispose( void )
        {
            if( !!subscription_ ) {
                thunk_ = Thunk();
                tools::AutoDispose<> toDisp( std::move( subscription_ ));
                return;
            }
            delete this;
        }
    protected:
        // local methods
        void
        onChange( void )
        {
            if( !!thunk_ ) {
                thunk_();
            }
        }
        void
        onDeath( void )
        {
            TOOLS_ASSERT( !subscription_ );
            dispose();
        }
    private:
        tools::AutoDispose<> subscription_;
        Thunk thunk_;
    };

    TOOLS_API AutoDispose< Request > subscriptionRequestAdaptorNew( Subscribable &, Thunk const &, impl::ResourceSample const & );
    inline AutoDispose< Request > subscriptionRequestAdaptorNew( Subscribable & sub, Thunk const & thunk )
    {
        return tools::subscriptionRequestAdaptorNew( sub, thunk, TOOLS_RESOURCE_SAMPLE_CALLER( 0 ));
    }

    // A standard container for a variable that can report its changes.
    template< typename ItemT, typename AllocT = tools::AllocStatic<> >
    struct SubscribableItem
        : tools::StandardDisposable< SubscribableItem< ItemT, AllocT >, tools::Publisher, AllocT >
    {
        SubscribableItem( AutoDispose< Publisher > && pub = tools::simplePublisherNew() )
            : item_( ItemT() )
            , publisher_( std::move( pub ))
        {}
        SubscribableItem( ItemT & i, AutoDispose< Publisher > && pub = tools::simplePublisherNew() )
            : item_( std::move( i ))
            , publisher_( std::move( pub ))
        {}
        SubscribableItem( ItemT const & i, AutoDispose< Publisher > && pub = tools::simplePublisherNew() )
            : item_( i )
            , publisher_( std::move( pub ))
        {}

        // Publisher
        AutoDispose<> newSubscription( Thunk const & thunk, Thunk const & dead )
        {
            return publisher_->newSubscription( thunk, dead );
        }
        void invalidate( void )
        {
            publisher_->invalidate();
        }

        // local methods
        void set( ItemT & i )
        {
            item_ = std::move( i );
            publisher_->invalidate();
        }
        void set( ItemT const & i )
        {
            item_ = i;
            publisher_->invalidate();
        }
        ItemT get( void ) const {
            return item_;
        }
    protected:
        ItemT item_;
        AutoDispose< Publisher > publisher_;
    };

    // A standard atomic container for a variable that can report its changes.
    template< typename ItemT, typename AllocT = tools::AllocStatic<> >
    struct AtomicSubscribableItem
        : tools::StandardDisposable< AtomicSubscribableItem< ItemT, AllocT >, tools::Publisher, AllocT >
    {
        AtomicSubscribableItem( tools::AutoDispose< tools::Publisher > && pub = tools::simplePublisherNew() )
            : item_( ItemT() )
            , publisher_( std::move( pub ))
        {}
        AtomicSubscribableItem( ItemT & i, tools::AutoDispose< tools::Publisher > && pub = tools::simplePublisherNew() )
            : item_( std::move( i ))
            , publisher_( std::move( pub ))
        {}
        AtomicSubscribableItem( ItemT const & i, tools::AutoDispose< tools::Publisher > && pub = tools::simplePublisherNew() )
            : item_( i )
            , publisher_( std::move( pub ))
        {}

        // Publisher
        AutoDispose<> newSubscription( Thunk const & thunk, Thunk const & dead )
        {
            return publisher_->newSubscription( thunk, dead );
        }
        void invalidate( void )
        {
            publisher_->invalidate();
        }

        // local methods
        void set( ItemT const & i )
        {
            tools::atomicUpdate( &item_, [i]( ItemT const & )->ItemT {
                return i;
            });
            publisher_->invalidate();
        }
        ItemT get( void ) const
        {
            return tools::atomicRead( &item_ );
        }
    protected:
        tools::AtomicAny< ItemT > item_;
        tools::AutoDispose< tools::Publisher > publisher_;
    };
};  // namespace tools
