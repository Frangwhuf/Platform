#include "toolsprecompiled.h"

#include <tools/AsyncTools.h>

#include "TimingImpl.h"

#include <algorithm>
#include <vector>
#include <boost/cast.hpp>

using namespace tools;
using boost::numeric_cast;

////////
// Types
////////

namespace {
    struct TimerQueueImpl;

    struct TimerReq
        : StandardManualRequest< TimerReq >
    {
        TimerReq( TimerQueueImpl &, uint64, uint64 *, void * );

        // Request
        void start();

        TimerQueueImpl * parent_;
        uint64 delta_;
        uint64 * startTime_;
        TimerReq * next_;
        uint64 due_;
        void * caller_;
    };

    struct TimerQueueImpl
        : StandardDisposable< TimerQueueImpl, TimerQueue >
    {
        typedef std::vector< TimerReq *, AllocatorAffinity< TimerReq * >> TimerVec;

        TimerQueueImpl( Thunk const & );
        ~TimerQueueImpl( void );

        // TimerQueue
        AutoDispose< Request > timer( uint64, uint64 *, void * );
        uint64 eval( uint64 * );

        // local methods
        bool draw( TimerReq * );
        void post( TimerReq & );

        Thunk thunk_;
        TimerReq * volatile pending_;
        TimerVec sleeping_;
        TimerReq claim_;
    };
};  // anonymous namespace

///////////////////////
// Non-member Functions
///////////////////////

AutoDispose< TimerQueue >
tools::impl::timerQueueNew(
    Thunk const & thunk )
{
    return new TimerQueueImpl( thunk );
}

///////////
// TimerReq
///////////

TimerReq::TimerReq(
    TimerQueueImpl & p,
    uint64 d,
    uint64 * s,
    void * c )
    : parent_( &p )
    , delta_( d )
    , startTime_( s )
    , caller_( c )
{
}

void
TimerReq::start()
{
    uint64 now = tools::impl::getHighResTime();
    due_ = now + delta_;
    if( !!startTime_ ) {
        *startTime_ = now;
    }
    parent_->post( *this );
}

/////////////////
// TimerQueueImpl
/////////////////

TimerQueueImpl::TimerQueueImpl(
    Thunk const & thunk )
    : thunk_( thunk )
    , pending_( NULL )
    , claim_( *this, 0, NULL, NULL )
{
    claim_.next_ = NULL;
    pending_ = &claim_;
}

TimerQueueImpl::~TimerQueueImpl( void )
{
    AutoDispose< Error::Reference > err;
    // Clear out all pending timers
    while( !!pending_ && pending_ != &claim_ ) {
        TimerReq * toCancel = pending_;
        pending_ = toCancel->next_;
        toCancel->next_ = NULL;
        if( !err ) {
            err.reset( errorCancelNew() );
        }
        toCancel->finish( *err );
    }
    // Cancel timers from those most about to fire, to those more distant.
    std::for_each(sleeping_.rbegin(), sleeping_.rend(), [&]( TimerReq * r )->void {
        if( !err ) {
            err.reset( errorCancelNew() );
        }
        r->finish( *err );
    });
}

AutoDispose< Request >
TimerQueueImpl::timer(
    uint64 delay,
    uint64 * start,
    void * caller )
{
    return new TimerReq( *this, delay, start, caller );
}

uint64
TimerQueueImpl::eval(
    uint64 * napTime )
{
    uint64 now, retry;
    do {
        // By default retry after 7 seconds
        retry = 7ULL * TOOLS_NANOSECONDS_PER_SECOND;
        // Claim ownership of the eval loop immediately to prevent other threads from pushing
        // spurious wakeups.
        TimerReq * queue = atomicExchange( &pending_, &claim_ );
        // Copy, then sort, the claims.
        bool added = draw( queue );
        bool activate = false;
        // Allow up to 50 us of slop.
        now = tools::impl::getHighResTime();
        uint64 activateTime = now + ( 50U * TOOLS_MILLISECONDS_PER_SECOND );
        if( !sleeping_.empty() ) {
            do {
                TimerReq * soonest = sleeping_.back();
                if( soonest->due_ > activateTime ) {
                    retry = std::min( retry, numeric_cast< uint64 >( ( soonest->due_ - activateTime ) +
                        ( 100U * TOOLS_MILLISECONDS_PER_SECOND )));
                    break;                  
                }
                activate = true;
                sleeping_.pop_back();
                // Fire this timer
                uint64 duration = tools::impl::getHighResTime();
                //void * caller = soonest->caller_;
                soonest->finish();
                duration = tools::impl::getHighResTime() - duration;
                // if( duration > warningTime ) {
                //     // TODO: log this as taking a long time
                // }
            } while( !sleeping_.empty() );
        }
        if( activate || added ) {
            // If we succeeded, try and find some more work.
            continue;
        }
        // if( ( activateTime - lastMemDump ) > memDump ) {
        //     // TODO: do a memory dump
        // }
    } while( atomicCas< TimerReq *, TimerReq * >( &pending_, &claim_, NULL ) != &claim_ );
    // By the time we get here, we have computed the time until next execution.
    if( !!napTime ) {
        *napTime = ( now + retry );
    }
    return retry;
}

bool
TimerQueueImpl::draw(
    TimerReq * queue )
{
    bool added = false;
    for(;;) {
        for( TimerReq * queueNext; !!queue && queue != &claim_; queue = queueNext ) {
            queueNext = queue->next_;
            // This needs to be reset immediately
            queue->next_ = NULL;
            sleeping_.push_back( queue );
            added = true;
        }
        if( pending_ == &claim_ ) {
            break;
        }
        queue = atomicExchange( &pending_, &claim_ );
    }
    if( added ) {
        std::sort( sleeping_.begin(), sleeping_.end(), []( TimerReq * l, TimerReq * r )->bool {
            return l->due_ > r->due_;
        });
    }
    return added;
}

void
TimerQueueImpl::post(
    TimerReq & r )
{
    if( !atomicPush( &pending_, &r, &TimerReq::next_ )) {
        if( !!thunk_ ) {
	    thunk_.fire();
	}
    }
}

#include <tools/UnitTest.h>
#if TOOLS_UNIT_TEST
//////////
// Testing
//////////

namespace {
    struct TimingTestImpl
    {
        TimingTestImpl( void );

        // tests
        void doTrivialTest( void );
        void doWaitTest( void );
        void doTrivialMarkTest( void );
        void doMarkWaitTest( void );

        Environment * env_;
        AutoDispose<> envLifetime_;
    };
};  // anonymous namespace

TOOLS_UNIT_TEST_METHOD( TimingTestImpl, doTrivialTest );
TOOLS_UNIT_TEST_METHOD( TimingTestImpl, doWaitTest );
TOOLS_UNIT_TEST_METHOD( TimingTestImpl, doTrivialMarkTest );
TOOLS_UNIT_TEST_METHOD( TimingTestImpl, doMarkWaitTest );

/////////////////
// TimingTestImpl
/////////////////

TimingTestImpl::TimingTestImpl( void )
{
    env_ = NewSimpleEnvironment( envLifetime_ );
}

void
TimingTestImpl::doTrivialTest( void )
{
    Timing * timeSvc = env_->get< Timing >();
    TOOLS_ASSERTR( !!timeSvc );
}

void
TimingTestImpl::doWaitTest( void )
{
    Timing * timeSvc = env_->get< Timing >();
    TOOLS_ASSERTR( !!timeSvc );
    {
        AutoDispose< Request > req( timeSvc->timer( TOOLS_NANOSECONDS_PER_MILLISECOND ));
        AutoDispose< Error::Reference > err( runRequestSynchronously( req ));
        TOOLS_ASSERTR( !err );
    }
}

void
TimingTestImpl::doTrivialMarkTest( void )
{
    Timing * timeSvc = env_->get< Timing >();
    TOOLS_ASSERTR( !!timeSvc );
    uint64 mark = timeSvc->mark();
    TOOLS_ASSERTR( mark != 0 );
    unsigned count = 0;
    bool changed = false;
    while (!changed && (count < 10000)) {
        if( timeSvc->mark() != mark ) {
            changed = true;
        }
        ++count;
    }
    TOOLS_ASSERTR( changed );
    uint64 delta = timeSvc->mark( mark );
    TOOLS_ASSERTR( delta != 0 );
}

void
TimingTestImpl::doMarkWaitTest( void )
{
    Timing * timeSvc = env_->get< Timing >();
    TOOLS_ASSERTR( !!timeSvc );
    {
        AutoDispose< Request > req( timeSvc->timer( TOOLS_NANOSECONDS_PER_MILLISECOND ));
        uint64 mark = timeSvc->mark();
        TOOLS_ASSERTR( mark != 0 );
        AutoDispose< Error::Reference > err( runRequestSynchronously( req ));
        uint64 delta = timeSvc->mark( mark );
        TOOLS_ASSERTR( !err );
        TOOLS_ASSERTR( delta >= TOOLS_NANOSECONDS_PER_MILLISECOND );
    }
}

#endif // TOOLS_UNIT_TEST
