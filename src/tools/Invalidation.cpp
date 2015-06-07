#include "toolsprecompiled.h"

#include <tools/AsyncTools.h>
#include <tools/InterfaceTools.h>
#include <tools/Invalidation.h>
#include <tools/Threading.h>
#include <tools/Tools.h>

#include <vector>

using namespace tools;

////////
// Types
////////

namespace {
    struct PubBase;

	struct SubscrItem
		: AllocStatic<>
	{
        enum : unsigned {
            CallingFlag = 0x1U,
            DisposedFlag = 0x2U,
            DirtyFlag = 0x4U,
            InitializingFlag = 0x8U,
            AllocatedFlag = 0x10U,
        };

        SubscrItem( PubBase *, Thunk const &, Thunk const &, unsigned );
        ~SubscrItem( void );

        PubBase * parent_;
		Thunk thunk_;
        Thunk dead_;
		unsigned flags_;
		SubscrItem * volatile next_;
	};

	struct PubBase
		: Publisher
	{
		PubBase( void );
		~PubBase( void );

		// Publisher
		AutoDispose<> newSubscription( Thunk const &, Thunk const & );
		void invalidate( void );

        // local methods
        void prune( void );
        virtual void makeCall( SubscrItem * ) = 0;
        virtual void makeDeadCall( SubscrItem * ) = 0;

		SubscrItem * volatile head_;
		unsigned volatile capacity_;
		unsigned volatile size_;
        unsigned volatile dead_;
	};

	struct SubItemDisp
		: StandardDisposable< SubItemDisp >
	{
		SubItemDisp( SubscrItem * );
		~SubItemDisp( void );

        SubscrItem * node_;
	};

	struct SimplePubImpl
		: StandardDisposable< SimplePubImpl, PubBase >
	{
        // local methods
        void makeCall( SubscrItem * );
        void makeDeadCall( SubscrItem * );
	};

	struct TaskPubImpl
		: PubBase
        , AllocStatic<>
	{
        TaskPubImpl( void );

        // Publisher
        void dispose( void );

        // local methods
        void makeCall( SubscrItem * );
        void makeDeadCall( SubscrItem * );

        unsigned volatile refs_;
	};

	struct TaskPubTask
		: Task
		, AllocStatic<>
	{
		TaskPubTask( SubscrItem *, bool );

		// Task
		void execute( void );

		SubscrItem * node_;
        bool deadCall_;
	};

    struct ReqAdaptor
        : Notifiable< ReqAdaptor >
        , StandardManualRequest< ReqAdaptor >
    {
        ReqAdaptor( Subscribable &, Thunk const & );
        ~ReqAdaptor( void );

        // Request
        void start( void );

        // local methods
        void onDeath( void );

        AutoDispose<> subscription_;
    };
};  // anonymous namespace

namespace tools {
	AutoDispose< Publisher >
	simplePublisherNew( impl::ResourceSample const & sample )
	{
		return new( sample ) SimplePubImpl();
	}

	AutoDispose< Publisher >
	taskPublisherNew( impl::ResourceSample const & sample )
	{
		return new( sample ) TaskPubImpl();
	}

    AutoDispose< Request >
    subscriptionRequestAdaptorNew( Subscribable & sub, Thunk const & thunk, impl::ResourceSample const & sample )
    {
        return new( sample ) ReqAdaptor( sub, thunk );
    }
};  // tools namespace

/////////////
// SubscrItem
/////////////

SubscrItem::SubscrItem( PubBase * p, Thunk const & t, Thunk const & d, unsigned f )
    : parent_( p )
    , thunk_( t )
    , dead_( d )
    , flags_( f )
    , next_( nullptr )
{
}

SubscrItem::~SubscrItem( void )
{
    TOOLS_ASSERT( flags_ == 0U );
}

//////////
// PubBase
//////////

PubBase::PubBase( void )
	: head_( nullptr )
	, capacity_( 0U )
	, size_( 0U )
    , dead_( 0U )
{
}

PubBase::~PubBase( void )
{
    prune();
	TOOLS_ASSERT( size_ == 0U );
	SubscrItem * current = head_;
	while( current != nullptr ) {
		SubscrItem * next = current->next_;
		delete current;
		current = next;
	}
}

AutoDispose<>
PubBase::newSubscription( Thunk const & thunk, Thunk const & dead )
{
	if( !thunk ) {
		// no one to tell, so don't bother
		return static_cast< Disposable * >( nullptr );
	}
	SubItemDisp * ret = nullptr;
	while( true ) {
		if( size_ == capacity_ ) {
			// all nodes in use, make a new one
			SubscrItem * item = new SubscrItem( this, thunk, dead, SubscrItem::AllocatedFlag );
			ret = new SubItemDisp( item );
			SubscrItem * oldHead;
			do {
				oldHead = head_;
				item->next_ = head_;
			} while( atomicCas( &head_, oldHead, item ) != oldHead );
			atomicIncrement( &size_ );
			atomicIncrement( &capacity_ );
			break;
		}
		// try to find one to use
		SubscrItem * volatile current = head_;
		while( current != nullptr ) {
			unsigned flags = atomicRead( &current->flags_ );
			if( flags == 0U ) {
				// Disposed but not being called, perfect
				if( atomicCas( &current->flags_, flags, ( SubscrItem::AllocatedFlag | SubscrItem::InitializingFlag ) ) == flags ) {
					ret = new SubItemDisp( current );
                    current->thunk_ = thunk;
                    current->dead_ = dead;
					atomicIncrement( &size_ );
                    // clear the initializing flag, we should be the only one modifying this now
                    atomicTryUpdate( &current->flags_, []( unsigned * prev )->bool {
                        bool ret = (( *prev & SubscrItem::InitializingFlag ) != 0U );
                        if( ret ) {
                            *prev = *prev & ~SubscrItem::InitializingFlag;
                        }
                        return ret;
                    });
					break;
				}
			}
			current = current->next_;
		}
		if( !!ret ) {
			break;
		}
	}
	return std::move(ret);
}

void
PubBase::invalidate( void )
{
	SubscrItem * volatile current = head_;
	while( current != nullptr ) {
        unsigned oldFlags, newFlags;
        oldFlags = atomicRead( &current->flags_ );
        bool done = false;
        do {
            if( ( oldFlags & SubscrItem::AllocatedFlag ) == 0U ) {
                // not in use, move on
                break;
            }
            if( ( oldFlags & ( SubscrItem::CallingFlag | SubscrItem::DisposedFlag | SubscrItem::InitializingFlag )) == 0U ) {
                // not currently being called or initialized, let's try to set the calling flag
                newFlags = oldFlags | SubscrItem::CallingFlag;
                if( atomicCas( &current->flags_, oldFlags, newFlags ) == oldFlags ) {
                    // If this test failed, something else is going on this this node and it can be ignored
                    this->makeCall( current );
                }
                done = true;
            } else {
                // try to mark this node as dirty
                do {
                    if( ( oldFlags & ( SubscrItem::DisposedFlag | SubscrItem::InitializingFlag )) != 0U ) {
                        done = true;
                        break;
                    } else if( ( oldFlags & SubscrItem::CallingFlag ) == 0U ) {
                        break;  // try to get calling again
                    }
                    newFlags = oldFlags | SubscrItem::DirtyFlag;
                } while( atomicCas( &current->flags_, oldFlags, newFlags ) != oldFlags );
            }
        } while( !done );
		// on to the next target
		current = current->next_;
	}
    this->prune();
}

void
PubBase::prune( void )
{
    SubscrItem * volatile current = head_;
    while( current != nullptr ) {
        if( dead_ == 0 ) {
            return;
        }
        unsigned oldFlags, oldAltFlags, newFlags;
        oldFlags = oldAltFlags = newFlags = ( SubscrItem::AllocatedFlag | SubscrItem::DisposedFlag );
        newFlags |= SubscrItem::CallingFlag;
        oldAltFlags |= SubscrItem::DirtyFlag;
        if( atomicCas( &current->flags_, oldFlags, newFlags ) == oldFlags ) {
            this->makeDeadCall( current );
        } else if( atomicCas( &current->flags_, oldAltFlags, newFlags ) == oldAltFlags ) {
            this->makeDeadCall( current );
        }
        current = current->next_;
    }
}

//////////////
// SubItemDisp
//////////////

SubItemDisp::SubItemDisp( SubscrItem * n )
	: node_( n )
{
}

SubItemDisp::~SubItemDisp( void )
{
    PubBase * base = node_->parent_;
    TOOLS_ASSERT( !!base );
    unsigned oldFlags, newFlags;
    do {
        oldFlags = atomicRead( &node_->flags_ );
        newFlags = oldFlags | SubscrItem::DisposedFlag;
        TOOLS_ASSERT( ( oldFlags & ( SubscrItem::AllocatedFlag | SubscrItem::InitializingFlag | SubscrItem::DisposedFlag )) == SubscrItem::AllocatedFlag );
    } while( atomicCas( &node_->flags_, oldFlags, newFlags ) != oldFlags );
    atomicIncrement( &node_->parent_->dead_ );
    base->prune();  // kick off releasing things
}

////////////////
// SimplePubImpl
////////////////

void
SimplePubImpl::makeCall( SubscrItem * item )
{
    unsigned oldFlags, newFlags;
    // Loop here so that if something changes while we are calling,
    // we can track that
    do {
        oldFlags = atomicRead( &item->flags_ );
        if( ( oldFlags & SubscrItem::DirtyFlag ) != 0U ) {
            // First check for, and clear the dirty flag
            do {
                oldFlags = atomicRead( &item->flags_ );
                newFlags = ( oldFlags & ~SubscrItem::DirtyFlag );
            } while( atomicCas( &item->flags_, oldFlags, newFlags ) != oldFlags );
            oldFlags = newFlags;
        }
        if( ( oldFlags & SubscrItem::DisposedFlag ) == 0U ) {
            // still valid, call
            item->thunk_();
            newFlags = ( oldFlags & ~( SubscrItem::CallingFlag | SubscrItem::DirtyFlag ));
        } else {
            newFlags = ( oldFlags & ~( SubscrItem::CallingFlag ));
        }
    } while( atomicCas( &item->flags_, oldFlags, newFlags ) != oldFlags );
}

void
SimplePubImpl::makeDeadCall( SubscrItem * item )
{
    if( !!item->dead_ ) {
        item->dead_();
    }
    atomicDecrement( &dead_ );
    atomicUpdate( &item->flags_, []( unsigned )->unsigned { return 0U; });
    atomicDecrement( &size_ );
}

//////////////
// TaskPubImpl
//////////////

TaskPubImpl::TaskPubImpl( void )
    : refs_( 1U )
{
}

void
TaskPubImpl::dispose( void )
{
    if( !atomicDeref( &refs_ )) {
        delete this;
    }
}

void
TaskPubImpl::makeCall( SubscrItem * item )
{
    atomicRef( &refs_ );  // hold a ref until the task completes
    Task * task = new TaskPubTask( item, false );
    ThreadScheduler::current().spawn( *task, ThreadScheduler::current().defaultParam() );
}

void
TaskPubImpl::makeDeadCall( SubscrItem * item )
{
    atomicRef( &refs_ );  // hold a ref until the task completes
    Task * task = new TaskPubTask( item, true );
    ThreadScheduler::current().spawn( *task, ThreadScheduler::current().defaultParam() );
}

//////////////
// TaskPubTask
//////////////

TaskPubTask::TaskPubTask( SubscrItem * node, bool dead )
	: node_( node )
    , deadCall_( dead )
{
}

void
TaskPubTask::execute( void )
{
    // The node is already maked Calling, so we mostly just need to do that
    if( deadCall_ ) {
        if( !!node_->dead_ ) {
            node_->dead_();
        }
        atomicDecrement( &node_->parent_->dead_ );
        atomicUpdate( &node_->flags_, []( unsigned )->unsigned { return 0U; });
        atomicDecrement( &node_->parent_->size_ );
    } else {
        unsigned oldFlags, newFlags;
        // Loop here so that we catch if a node gets marked dirty
        // while we are calling.
        do {
            oldFlags = atomicRead( &node_->flags_ );
            if( ( oldFlags & SubscrItem::DirtyFlag ) != 0U ) {
                // First check for, and clear the dirty flag
                do {
                    oldFlags = atomicRead( &node_->flags_ );
                    newFlags = ( oldFlags & ~SubscrItem::DirtyFlag );
                } while( atomicCas( &node_->flags_, oldFlags, newFlags ) != oldFlags );
                oldFlags = newFlags;
            }
            if( ( oldFlags & SubscrItem::DisposedFlag ) == 0U ) {
                // still valid, call
                node_->thunk_();
                newFlags = ( oldFlags & ~( SubscrItem::CallingFlag | SubscrItem::DirtyFlag ));
            } else {
                newFlags = ( oldFlags & ~( SubscrItem::CallingFlag ));
            }
            // If this fails, something happened while we were calling
        } while( atomicCas( &node_->flags_, oldFlags, newFlags ) != oldFlags );
    }
    node_->parent_->dispose();  // deref
	delete this;
}

/////////////
// ReqAdaptor
/////////////

ReqAdaptor::ReqAdaptor( Subscribable & sub, Thunk const & thunk )
    : subscription_( sub.newSubscription( thunk, toThunk< &ReqAdaptor::onDeath >() ))
{
}

ReqAdaptor::~ReqAdaptor( void )
{
    TOOLS_ASSERT( !subscription_ );
}

void
ReqAdaptor::start( void )
{
    subscription_.release();
}

void
ReqAdaptor::onDeath( void )
{
    finish();
}
