#include "toolsprecompiled.h"

#include <tools/AtomicCollections.h>
#include <tools/Async.h>
#include <tools/AsyncTools.h>
#include <tools/Threading.h>
#include <tools/WeakPointer.h>

using namespace tools;

////////
// Types
////////

namespace {
    struct SyncLocal
        : AllocStatic<>
    {
        SyncLocal( void );

        AutoDispose< ConditionVar > cvar_;
        AutoDispose< Monitor > monitor_;
    };

    struct SyncThunk
        : Completable< SyncThunk >
    {
        SyncThunk( void );
        void completed( Error * );

        SyncLocal * local_;
        AutoDispose< Error::Reference > error_;
        bool volatile complete_;
    };

    struct MultiRequestOwnerImpl;

    struct ListEntry
        : AtomicListBase<ListEntry>
        , AllocStatic<>
    {
        ListEntry(MultiRequestOwnerImpl & parent, AutoDispose<Request> && inner)
            : parent_(parent)
            , inner_(std::move(inner))
        {
            TOOLS_ASSERT(!!inner_);
        }

        MultiRequestOwnerImpl & parent_;
        AutoDispose<Request> inner_;
    };

    TOOLS_FORCE_INLINE bool isEnd(ListEntry const & entry)
    {
        return !entry.inner_;
    }

    TOOLS_FORCE_INLINE void setEnd(ListEntry & entry)
    {
        entry.inner_ = nullptr;
    }

    struct CompletionFanoutReq
        : StandardRequest<CompletionFanoutReq>
        , Completable<CompletionFanoutReq>
    {
        CompletionFanoutReq(CompletionFanout &, AutoDispose<Request> &&, bool = false);

        // StandardRequest
        RequestStep start(void) override;

        // local methods
        void joinInner(void);
        void fanoutReenter(Error *);
        RequestStep fanoutCompleted(Error *);
        RequestStep innerCompleted(Error *);

        CompletionFanout & fanout_;
        AutoDispose<Request> inner_;
        bool forceStartOnError_;
    };

    struct MultiRequestOwnerImpl
        : StandardDisposable<MultiRequestOwnerImpl, MultiRequestOwner>
    {
        enum : unsigned {
            extractInterval = 8,
        };
        struct State
        {
            bool closed_; // true IFF no new requests are allowed to be started
            uint16 doneCount_; // Extract only runs once per extractInterval completions. This lowers CPU utilization when there are many requests, trading off for memory.
            uint32 running_; // number of in-flight Requests
        };

        MultiRequestOwnerImpl(void) = default;
        ~MultiRequestOwnerImpl(void);

        // MultiRequestOwner
        void start(AutoDispose<Request> &&) override;
        AutoDispose<Request> maybeStart(AutoDispose<Request> &&) override;
        AutoDispose<Request> stop(void) override;

        // local methods
        static void done(void *, Error *);
        void done(ListEntry *);

        AtomicList<ListEntry> list_;
        AtomicAny<State> state_;
        CompletionFanout fanout_;
    };

    // This is more complex than it might otherwise be because the user may dispose of the request before
    // starting it. When that happens, dispose() can race with the trigger.
    struct TriggerRequest
        : StandardRequest<TriggerRequest, AllocStatic<Inherent>>
    {
        struct State
        {
            bool triggered_;
            bool started_;
            bool disposed_;
            // bool pad_[5];  // for Valgrind complaints about uninitialized memory
        };
        struct Trigger
            : Disposable
        {
            Trigger(TriggerRequest & parent) : parent_(parent) {}

            // Disposable
            void dispose(void) override {
                bool doFinish = false;
                bool doDispose = false;
                atomicUpdate(&parent_.state_, [&](State & old)->State {
                    TriggerRequest::State next = old;
                    TOOLS_ASSERT(!next.triggered_);
                    next.triggered_ = true;
                    doFinish = next.started_;
                    doDispose = next.disposed_;
                    return next;
                });
                if (doFinish) {
                    parent_.resumeFinish();
                }
                if (doDispose) {
                    delete &parent_;
                }
            }

            TriggerRequest & parent_;
        };

        TriggerRequest(void) : trigger_(*this) {}

        // StandardRequest
        RequestStep start(void) override {
            return suspend<&TriggerRequest::escapedStart>();
        }

        // local methods
        void escapedStart(void) {
            bool doFinish = false;
            atomicUpdate(&state_, [&](State & old)->State {
                TriggerRequest::State next = old;
                TOOLS_ASSERT(!old.started_);
                next.started_ = true;
                doFinish = next.triggered_;
                return next;
            });
            if (doFinish) {
                resumeFinish();
            }
        }

        AtomicAny<State> state_;
        Trigger trigger_;
    };
};  // anonymous namespace

static StandardThreadLocalHandle< SyncLocal > syncLocal_;

///////////////////////
// Non-member functions
///////////////////////

AutoDispose< Error::Reference >
tools::runRequestSynchronously( NoDispose< Request > const & req )
{
    SyncThunk thunk;
    {
        AutoDispose<> p_( phantomTryBindPrototype< PhantomUniversal >() );
        req->start( thunk.toCompletion< &SyncThunk::completed >() );
    }
    {
        AutoDispose<> l_( thunk.local_->monitor_->enter() );
        while( !thunk.complete_ ) {
            thunk.local_->cvar_->wait();
            // TODO: consider adding a timeout
        }
    }
    return std::move( thunk.error_ );
}

AutoDispose<MultiRequestOwner>
tools::multiRequestOwnerNew(void)
{
    return new MultiRequestOwnerImpl();
}

AutoDispose<Request> tools::triggerRequestNew(
    AutoDispose<> & trigger)
{
    auto ret = new TriggerRequest();
    trigger = &ret->trigger_;
    return std::move(ret);
}

////////////
// SyncLocal
////////////

SyncLocal::SyncLocal( void )
    : cvar_( conditionVarNew() )
    , monitor_(cvar_->monitorNew() )
{
}

////////////
// SyncThunk
////////////

SyncThunk::SyncThunk( void )
    : local_( syncLocal_.get() )
    , complete_( false )
{
}

void
SyncThunk::completed( Error * err )
{
    TOOLS_ASSERT( !!local_ );
    {
        AutoDispose<> l_( local_->monitor_->enter() );
        TOOLS_ASSERT( !complete_ );
        TOOLS_ASSERT( !error_ );
        if( !!err ) {
            error_ = err->ref();
        }
        complete_ = true;
    }
    local_->cvar_->signal(true);
}

///////////////////
// CompletionFanout
///////////////////

AutoDispose<Request>
CompletionFanout::maybeWrap(
    AutoDispose<Request> && request)
{
    if (hasCompleted()) {
        return std::move(request);
    }
    return ThreadScheduler::current().bind(AutoDispose<Request>(new CompletionFanoutReq(*this, std::move(request))));
}

//////////////////////
// CompletionFanoutReq
//////////////////////

CompletionFanoutReq::CompletionFanoutReq(
    CompletionFanout & fanout,
    AutoDispose<Request> && inner,
    bool startOnErr)
    : fanout_(fanout)
    , inner_(std::move(inner))
    , forceStartOnError_(startOnErr)
{}

RequestStep
CompletionFanoutReq::start(void)
{
    return suspend<&CompletionFanoutReq::joinInner>();
}

void
CompletionFanoutReq::joinInner(void)
{
    fanout_.join(toCompletion<&CompletionFanoutReq::fanoutReenter>());
}

void
CompletionFanoutReq::fanoutReenter(
    Error * err)
{
    resume<&CompletionFanoutReq::fanoutCompleted>(err);
}

RequestStep
CompletionFanoutReq::fanoutCompleted(
    Error * err)
{
    if (!!err && !forceStartOnError_) {
        inner_ = nullptr;
        return finish(*err);
    }
    return maybeWait<&CompletionFanoutReq::innerCompleted>(inner_);
}

RequestStep
CompletionFanoutReq::innerCompleted(
    Error * err)
{
    inner_ = nullptr;
    if (!err) {
        return finish();
    }
    return finish(*err);
}

////////////////////////
// MultiRequestOwnerImpl
////////////////////////

MultiRequestOwnerImpl::~MultiRequestOwnerImpl(void)
{
    list_.extract(false);
}

void
MultiRequestOwnerImpl::start(
    AutoDispose<Request> && request)
{
    atomicTryUpdate(&state_, [](State & old)->bool {
        TOOLS_ASSERT(!old.closed_);
        ++old.running_;
        return true;
    });
    Request & req = *request;
    ListEntry * entry = new ListEntry(*this, std::move(request));
    list_.push(entry);
    req.start(Completion(&MultiRequestOwnerImpl::done, entry));
}

AutoDispose<Request>
MultiRequestOwnerImpl::maybeStart(
    AutoDispose<Request> && request)
{
    bool stopped = false;
    atomicTryUpdate(&state_, [&stopped](State & old)->bool {
        stopped = old.closed_;
        if (!stopped) {
            ++old.running_;
        }
        return true;
    });
    if (!stopped) {
        Request & req = *request;
        ListEntry * entry = new ListEntry(*this, std::move(request));
        list_.push(entry);
        req.start(Completion(&MultiRequestOwnerImpl::done, entry));
        TOOLS_ASSERT(!request);
    }
    return std::move(request);
}

AutoDispose<Request>
MultiRequestOwnerImpl::stop(void)
{
    bool fire = false;
    atomicTryUpdate(&state_, [&fire](State & old)->bool {
        TOOLS_ASSERT(!old.closed_);
        old.closed_ = true;
        fire = (old.running_ == 0U);
        return true;
    });
    if (fire) {
        // All Requests have completed, fire synchronously
        return nullptr;
    }
    // There are still Requests in-flight.
    return ThreadScheduler::current().fork(fanout_.maybeWrap());
}

void
MultiRequestOwnerImpl::done(
    void * impl,
    Error *)
{
    ListEntry * entry = static_cast<ListEntry *>(impl);
    entry->parent_.done(entry);
}

void
MultiRequestOwnerImpl::done(
    ListEntry * entry)
{
    setEnd(*entry);
    bool extract = false;
    atomicTryUpdate(&state_, [&extract](State & old)->bool {
        ++old.doneCount_;
        if ((extract = (old.doneCount_ >= extractInterval))) {
            old.doneCount_ = 0;
        }
        return true;
    });
    if (extract) {
        list_.extract(false);
    }
    bool fire = false;
    atomicTryUpdate(&state_, [&fire](State & old)->bool {
        TOOLS_ASSERT(old.running_ != 0U);
        --old.running_;
        fire = ((old.running_ == 0U) && old.closed_);
        return true;
    });
    if (fire) {
        // All Requests completed -> fire -> safe to dispose this.
        fanout_.fire(nullptr);
    }
}

//////////
// Testing
//////////

#include <tools/UnitTest.h>
#if TOOLS_UNIT_TEST

namespace {
    struct SuspendReq
        : StandardRequest<SuspendReq>
    {
        RequestStep start(void) {
            return suspend<&SuspendReq::suspend1>();
        }

        void suspend1(void) {
            resume<&SuspendReq::noError>();
        }

        RequestStep noError(void) {
            return suspend<&SuspendReq::suspend2>();
        }

        void suspend2(void) {
            resume<&SuspendReq::withError>();
        }

        RequestStep withError(Error * e) {
            TOOLS_ASSERTR(!e);
            return suspend<&SuspendReq::suspend3>();
        }

        void suspend3(void) {
            AutoDispose<Error::Reference> err(errorCancelNew());
            resume<&SuspendReq::withError2>(err.get());
        }

        RequestStep withError2(Error * e) {
            TOOLS_ASSERTR(!!e);
            return suspend<&SuspendReq::suspend4>();
        }

        void suspend4(void) {
            return resumeFinish();
        }
    };

    struct SuspendErrorReq
        : StandardRequest<SuspendErrorReq>
    {
        RequestStep start(void) {
            return suspend<&SuspendErrorReq::suspend1>();
        }

        void suspend1(void) {
            AutoDispose<Error::Reference> err(errorCancelNew());
            resume<&SuspendErrorReq::noError>(err.get());
        }

        RequestStep noError(void) {
            return finish();
        }
    };

    struct BackToBackCont
        : StandardRequest<BackToBackCont>
    {
        BackToBackCont(void) : idx_(0U) {}

        RequestStep start(void) {
            if (++idx_ > 1000000U) {
                return finish();
            }
            return cont<&BackToBackCont::start>();
        }

        unsigned idx_;
    };

    struct StartCheck
        : StandardRequest<StartCheck>
    {
        StartCheck(bool & started) : started_(started) {}

        // StandardRequest
        RequestStep start(void) override {
            started_ = true;
            return finish();
        }

        bool & started_;
    };
}; // anonymous namespace

TOOLS_TEST_CASE("Request.suspend", [](Test & test)
{
    test.runAndAssertSuccess(new SuspendReq());
    test.runAndAssertError(new SuspendErrorReq());
});

TOOLS_TEST_CASE("Request.stackUnroll", [](Test & test)
{
    test.runAndAssertSuccess(new BackToBackCont());
});

TOOLS_TEST_CASE("MultiRequestOwner.basic", [](Test &)
{
    AutoDispose<MultiRequestOwner> owner(multiRequestOwnerNew());
    unsigned syncCompletes = 0U;
    // Single live request at a time
    owner->start(simpleLambdaRequestNew([&]()->void {
        ++syncCompletes;
    }));
    owner->start(simpleLambdaRequestNew([&]()->void {
        ++syncCompletes;
    }));
    // Multiple live requests in-flight
    AutoDispose<> trigger1, trigger2, trigger3;
    owner->start(triggerRequestNew(trigger1));
    owner->start(triggerRequestNew(trigger2));
    owner->start(triggerRequestNew(trigger3));
    // Finish them out of insert order
    trigger2 = nullptr;
    trigger3 = nullptr;
    trigger1 = nullptr;
    // One more single live request
    owner->start(simpleLambdaRequestNew([&]()->void {
        ++syncCompletes;
    }));
    TOOLS_ASSERTR(syncCompletes == 3);
});

TOOLS_TEST_CASE("MultiRequestOwner.stop", [](Test & test)
{
    AutoDispose<MultiRequestOwner> owner(multiRequestOwnerNew());
    // Multiple live requests in-flight
    AutoDispose<> trigger1, trigger2, trigger3;
    owner->start(triggerRequestNew(trigger1));
    owner->start(triggerRequestNew(trigger2));
    owner->start(triggerRequestNew(trigger3));
    Test::RequestStatus stat;
    test.run(owner->stop(), stat);
    stat.unnotified();
    trigger2 = nullptr;
    stat.unnotified();
    trigger3 = nullptr;
    stat.unnotified();
    trigger1 = nullptr;
    stat.success();
    // Now safe to dispose the old one
    owner = multiRequestOwnerNew();
    // with no running requests, this should complete immediately
    test.runAndAssertSuccess(owner->stop());
    owner = multiRequestOwnerNew();
    owner->start(triggerRequestNew(trigger1));
    trigger1 = nullptr;
    test.runAndAssertSuccess(owner->stop());
});

TOOLS_TEST_CASE("MultiRequestOwner.maybeStart", [](Test & test)
{
    AutoDispose<MultiRequestOwner> owner(multiRequestOwnerNew());
    AutoDispose<> trigger1, trigger2;
    TOOLS_ASSERTR(!owner->maybeStart(triggerRequestNew(trigger1)));
    TOOLS_ASSERTR(!owner->maybeStart(triggerRequestNew(trigger2)));
    Test::RequestStatus stat;
    test.run(owner->stop(), stat);
    bool started = false;
    TOOLS_ASSERTR(!!owner->maybeStart(new StartCheck(started)));
    TOOLS_ASSERTR(!started);
    stat.unnotified();
    trigger2 = nullptr;
    stat.unnotified();
    trigger1 = nullptr;
    stat.success();
    owner = multiRequestOwnerNew();
    test.runAndAssertSuccess(owner->stop());
    TOOLS_ASSERTR(!!owner->maybeStart(new StartCheck(started)));
    TOOLS_ASSERTR(!started);
});

TOOLS_TEST_CASE("TriggerRequet", [](Test & test)
{
    AutoDispose<> trigger;
    AutoDispose<Request> inner;
    // First start the req, then trigger it
    inner = triggerRequestNew(trigger);
    Test::RequestStatus stat;
    test.run(inner, stat);
    stat.unnotified();
    trigger = nullptr;
    stat.success();
    // Next trigger the req, then start it
    inner = triggerRequestNew(trigger);
    trigger = nullptr;
    test.runAndAssertSuccess(inner);
    // Next dispose the req without starting it
    inner = triggerRequestNew(trigger);
    inner = nullptr;
    trigger = nullptr;
});

#endif // TOOLS_UNIT_TEST
