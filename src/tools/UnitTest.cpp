#include "toolsprecompiled.h"

// #include <tools/Configuration.h>
// #include <tools/Notification.h>
#include <tools/Environment.h>
#include <tools/InterfaceTools.h>
#include <tools/Timing.h>
#include <tools/UnitTest.h>
#include <tools/WeakPointer.h>

#include <iterator>
#include <queue>
#include <unordered_map>
#include <vector>

#ifdef WINDOWS_PLATFORM
#  pragma warning( disable : 4365 )
#  pragma warning( disable : 4619 )
#endif // WINDOWS_PLATFORM
#include <boost/format.hpp>
#include <boost/regex.hpp>
#ifdef WINDOWS_PLATFORM
#  pragma warning( default : 4365 )
#  pragma warning( default : 4619 )
#endif // WINDOWS_PLATFORM

using namespace tools;

// CONFIGURE_STATIC( UnitTestConfig );
// CONFIGURE_FUNCTION( UnitTestConfig ) {
// }

namespace tools {
    namespace impl {
        void * threadLocalAlloc(void);
        void threadLocalSet(void *, void *);
        void threadLocalFree(void *);
    };  // impl namespace
};  // tools namespace

namespace {
	struct MockScheduler;
    struct TestEnvItf : TestEnv, Disposable {};
	struct TestImpl
		: Test
        , Notifiable<TestImpl>
	{
		TestImpl(Environment &, StringId const &);
        ~TestImpl(void);

		// Test
		void finalize_inner(AutoDispose<> &&) override;
		void sync(void) override;
		void resume(void) override;
		void progressTime(void) override;
		void progressTime(uint64) override;
		void fastForwardtime(uint64) override;
		void adjustPendingTimer(sint64) override;
		void skewWalltime(sint64) override;
		void endTimers(void) override;
		TestEnv & environment(void) override;
		Environment & trueEnvironment(void) override;
		AutoDispose<> & cloak(void) override;
        void run(NoDispose<Request> const &, RequestStatus &) override;
        void runAndAssertSuccess(AutoDispose<Request> &&) override;
        void runAndAssertSuccess(NoDispose<Request> const &) override;
        void runAndAssertError(AutoDispose<Request> &&) override;
        void runAndAssertError(NoDispose<Request> const &) override;
        void generatorNext(NoDispose<Generator> const &, unsigned) override;

        // local methods
        bool isMainThread(void);
        void firePendingTimers(uint64);
        void start(Thunk const &);
        void workerEntry(void);
        static void requestCompletion(void *, Error *);

		Environment & trueEnvironment_;
		StringId name_;
		AutoDispose<TestEnvItf> testEnvironment_;
		std::vector<AutoDispose<>, AllocatorAffinity<AutoDispose<>>> finalizes_;
		// TODO: add thread stuff
		bool terminated_;
		std::unique_ptr<MockScheduler> kernel_;
		PhantomPrototype & testPhantom_;
		AutoDispose<> testCloak_;
		void * tlsKey_;
        AutoDispose<Thread> thread_;
        AutoDispose<Monitor> threadControl_;
        AutoDispose<ConditionVar> resumeControl_;
        AutoDispose<ConditionVar> syncControl_;
        Thunk nextThreadThunk_;
	};

    struct TestEnvEntry
    {
        TestEnvEntry(void);
        TestEnvEntry(Unknown *, StringId const &);

        Unknown * service_;
        StringId name_;
    };

    struct ServiceFactoryDelegate
    {
        ServiceFactoryDelegate(void);
        ServiceFactoryDelegate(TestEnv::ServiceFactory, void *);

        NoDispose<Unknown> fire(NoDispose<TestEnv>);

        TestEnv::ServiceFactory factory_;
        void * param_;
    };

    struct TestEnvImpl
        : StandardDisposable<TestEnvImpl, TestEnvItf>
    {
        typedef std::unordered_map<StringId, TestEnvEntry, HashAnyOf<StringId>, std::equal_to<StringId>, AllocatorAffinity<std::pair<StringId, TestEnvEntry>>> ServiceMap;
        typedef std::unordered_map<StringId, ServiceFactoryDelegate, HashAnyOf<StringId>, std::equal_to<StringId>, AllocatorAffinity<std::pair<StringId, ServiceFactoryDelegate>>> FactoriesMap;
        typedef std::vector<StringId, AllocatorAffinity<StringId>> CreatingVec;

        TestEnvImpl(Environment &, TestImpl &);

        // Environment
        StringId const & name(void) override;
        Unknown * get(StringId const &) override;

        // TestEnv
        Test & getTest(void) override;
        void mock(StringId const &, NoDispose<Unknown>, bool = false) override;
        void setFactory(StringId const &, TestEnv::ServiceFactory, void *, StringId const & = StringIdNull()) override;
        void unmock(StringId const &) override;
        NoDispose<Unknown> unmockNow(StringId const &) override;
        NoDispose<Unknown> createReal(StringId const &) override;
        void stopUnmocked(NoDispose<Service>, unsigned = 0) override;
        void stopUnmockedNow(NoDispose<Service>) override;

        Environment & trueEnvironment_;
        TestImpl & test_;
        ServiceMap services_;
        FactoriesMap factories_;
        CreatingVec creating_;
    };

    struct ServiceCreationStack
    {
        ServiceCreationStack(TestEnvImpl::CreatingVec &, StringId const &);
        ~ServiceCreationStack(void);

        TestEnvImpl::CreatingVec & creating_;
        StringId name_;
    };

    struct TestManagementImpl
        : tools::unittest::impl::Management
        , tools::detail::StandardNoBindService<TestManagementImpl, boost::mpl::list<tools::unittest::impl::Management>::type>
    {
        TestManagementImpl(Environment &);

        // Management
        unsigned run(StringId const &) override;
        unsigned list(StringId const &) override;
        void executeSingle(StringId const &, tools::unittest::TestCase &) override;

        // local methods
        static bool acceptTest(StringId const &, StringId const &);

        Environment & env_;
    };

    struct MockTimerReq;
    struct MockTimerEntry
    {
        TOOLS_FORCE_INLINE MockTimerEntry(MockTimerReq & request, uint64 fireTime)
            : request_(&request)
            , fireTime_(fireTime)
        {}

        TOOLS_FORCE_INLINE bool operator<(MockTimerEntry const & r) const {
            return (fireTime_ < r.fireTime_);
        }

        MockTimerReq * request_;
        uint64 fireTime_;
    };

    struct MockScheduler
        : Timing
    {
        typedef std::priority_queue<MockTimerEntry, std::vector<MockTimerEntry, AllocatorAffinity<MockTimerEntry, Inherent>>> PendingQueue;

        MockScheduler(TestImpl &);
        ~MockScheduler(void);

        // Timing
        uint64 mark(void) override;
        uint64 mark(uint64) override;
        AutoDispose<Request> timer(uint64, uint64 *) override;

        TestImpl & parent_;
        uint64 time_;
        uint64 wallTime_;
        PendingQueue pendingRequests_;
    };

    struct MockTimerReq
        : StandardRequest<MockTimerReq>
        , Notifiable<MockTimerReq>
    {
        MockTimerReq(TestImpl &, MockScheduler &, uint64, uint64 *);

        // StandardRequest
        RequestStep start(void) override;

        // local methods
        void escape(void);
        void reenter(void);

        bool running_;
        bool cancel_;
        TestImpl & test_;
        MockScheduler & timing_;
        uint64 waitTime_;
        uint64 * outStartTime_;
        Thunk thunk_;
    };

    struct TestThreadLocal
        : AllocStatic<>
    {
        Test * test_;
    };

    struct UnmockStopper
    {
        virtual void stop(NoDispose<Service>, unsigned = 0) = 0;
        virtual void doStops(void) = 0;
        virtual void stopNow(NoDispose<Service>) = 0;
    };

    struct UnmockStopperImpl
        : UnmockStopper
        , tools::detail::StandardNoBindService<UnmockStopperImpl, boost::mpl::list<UnmockStopper>::type>
        , Completable<UnmockStopperImpl>
    {
        UnmockStopperImpl(Environment &);
        UnmockStopperImpl(Test &, NoDispose<TestEnv>);
        ~UnmockStopperImpl(void);

        // UnmockStopper
        void stop(NoDispose<Service>, unsigned) override;
        void doStops(void) override;
        void stopNow(NoDispose<Service>) override;

        // local methods
        void stopInner(NoDispose<Service>);
        void stopped(Error *);

        Test & test_;
        AutoDispose<Request> done_;
        AutoDispose<> trigger_;
        AutoDispose<Request> stop_;
        std::vector<NoDispose<Service>, AllocatorAffinity<NoDispose<Service>>> services_[2];
    };

    struct NullTestEnv
        : TestEnv
    {
        NullTestEnv(Test &);

        // Environment
        StringId const & name(void) override;
        Unknown * get(StringId const &) override;

        // TestEnv
        Test & getTest(void) override;
        void mock(tools::StringId const &, tools::NoDispose<tools::Unknown>, bool) override;
        void setFactory(tools::StringId const &, ServiceFactory, void *, tools::StringId const &override) override;
        void unmock(tools::StringId const &) override;
        tools::NoDispose<tools::Unknown> unmockNow(tools::StringId const &) override;
        tools::NoDispose<tools::Unknown> createReal(tools::StringId const &) override;
        void stopUnmocked(tools::NoDispose<tools::Service>, unsigned) override;
        void stopUnmockedNow(tools::NoDispose<tools::Service>) override;

        Test & test_;
    };

    struct NullTest
        : Test
    {
        NullTest(void);

        // Test
        void finalize_inner(AutoDispose<> &&) override;
        void sync(void) override;
        void resume(void) override;
        void progressTime(void) override;
        void progressTime(uint64) override;
        void fastForwardtime(uint64) override;
        void adjustPendingTimer(sint64) override;
        void skewWalltime(sint64) override;
        void endTimers(void) override;
        TestEnv & environment(void) override;
        Environment & trueEnvironment(void) override;
        AutoDispose<> & cloak(void) override;
        void run(NoDispose<Request> const &, RequestStatus &) override;
        void runAndAssertSuccess(AutoDispose<Request> &&) override;
        void runAndAssertSuccess(NoDispose<Request> const &) override;
        void runAndAssertError(AutoDispose<Request> &&) override;
        void runAndAssertError(NoDispose<Request> const &) override;
        void generatorNext(NoDispose<Generator> const &, unsigned = 5) override;

        NullTestEnv env_;
    };

    static StandardThreadLocalHandle<TestThreadLocal> testLocal_;

    Test *
    testGetThreadLocal(void)
    {
        return testLocal_.get()->test_;
    }

    void
    testSetThreadLocal(Test * test)
    {
        testLocal_.get()->test_ = test;
    }

    static Test & nullTestNew(void)
    {
        static NullTest ret;
        return ret;
    }

    auto standardAutoRegister(AutoMock ***, UnmockStopper ***)->
        RegisterMock<UnmockStopper, UnmockStopperImpl>;
};  // anonymous namespace

///////////////////////
// Non-member Functions
///////////////////////

namespace {
    static NoDispose<Unknown>
    unmockFactory(
        NoDispose<TestEnv> env,
        void * param)
    {
        std::unique_ptr<StringId> id(static_cast<StringId *>(param));
        return env->createReal(*id);
    }

    static StringId
    composeFactoryName(
        StringId const & service,
        StringId const & requesting)
    {
        if (IsNullOrEmptyStringId(requesting)) {
            return service;
        }
        boost::format fmt("%s^%s");
        fmt % service % requesting;
        return fmt.str();
    }
}; // anonymous namespace

void
tools::unittest::impl::registerMockHelper(Test & test, NoDispose<Service> service)
{
    test.runAndAssertSuccess(service->start());
    test.environment().stopUnmocked(service);
}

///////////
// TestImpl
///////////

TestImpl::TestImpl(Environment & env, StringId const & name)
    : trueEnvironment_(env)
    , name_(name)
    , terminated_(false)
    , testPhantom_(phantomBindPrototype<PhantomUniversal>())
    , testCloak_(testPhantom_.select())
    , tlsKey_(tools::impl::threadLocalAlloc())
    , threadControl_(monitorNew())
    , resumeControl_(conditionVarNew())
    , syncControl_(conditionVarNew())
{
    testSetThreadLocal(this);
    testEnvironment_ = new TestEnvImpl(env, *this);
    tools::impl::threadLocalSet(tlsKey_, (void *)1);
    kernel_.reset(new MockScheduler(*this));
}

TestImpl::~TestImpl(void)
{
    // TOOLS_ASSERT(memoryValidate());
    testEnvironment_->get<UnmockStopper>()->doStops();
    if (!!thread_) {
        // Terminate the test thread
        terminated_ = true;
        resume();
        thread_->waitSync();  // wait for the test thread to exit
        syncControl_ = nullptr;
        resumeControl_ = nullptr;
        threadControl_ = nullptr;
        thread_ = nullptr;
    }
    // Run the finalizers. In case there are any dependancies, we should do this from newest to oldest. E.G.: backwards
    while (!finalizes_.empty()) {
        finalizes_.pop_back();
    }
    TOOLS_ASSERT(finalizes_.empty());
    testEnvironment_ = nullptr;
    tools::impl::threadLocalFree(tlsKey_);
    testCloak_ = nullptr;
    // TOOLS_ASSERT(memoryValidate());
}

void
TestImpl::finalize_inner(AutoDispose<> && toDisp)
{
    finalizes_.push_back(std::move(toDisp));
}

void
TestImpl::sync(void)
{
    TOOLS_ASSERT(!isMainThread());
    // This is entered from within the test thread in order to transfer control back to the main thread. This will
    // block until the main thread signals.
    {
        syncControl_->signal();
        AutoDispose<> l(threadControl_->enter());
        resumeControl_->wait();
    }
}

void
TestImpl::resume(void)
{
    if (!!thread_) {
        TOOLS_ASSERT(isMainThread());
        // This is entered from within the main thread in order to transfer control back to the test thread. This will
        // block until the test thread signals.
        {
            resumeControl_->signal();
            AutoDispose<> l(threadControl_->enter());
            syncControl_->wait();
        }
        testPhantom_.touch();
    }
}

void
TestImpl::progressTime(void)
{
    if (!kernel_->pendingRequests_.empty()) {
        // Find the next interesting edge in the pending queue
        MockScheduler::PendingQueue queueCopy = kernel_->pendingRequests_;
        uint64 activationTime = 0U;
        while (!queueCopy.empty()) {
            auto entry = queueCopy.top();
            TOOLS_ASSERT(entry.fireTime_ >= activationTime);
            activationTime = entry.fireTime_;
            queueCopy.pop();
        }
        firePendingTimers(activationTime);
    }
}

void
TestImpl::progressTime(uint64 delta)
{
    auto startTime = kernel_->time_;
    auto activationTime = startTime + delta;
    firePendingTimers(activationTime);
    if (kernel_->time_ < activationTime) {
        uint64 progress = static_cast<uint64>(activationTime - kernel_->time_);
        kernel_->time_ += progress;
        kernel_->wallTime_ += progress;
    }
}

void
TestImpl::fastForwardtime(uint64 delta)
{
    kernel_->time_ += delta;
    kernel_->wallTime_ += delta;
}

void
TestImpl::adjustPendingTimer(sint64 delta)
{
    MockScheduler::PendingQueue queueCopy;
    kernel_->pendingRequests_.swap(queueCopy);
    while (!queueCopy.empty()) {
        auto entry = queueCopy.top();
        entry.fireTime_ = std::max<uint64>(kernel_->time_, entry.fireTime_ + delta);
        kernel_->pendingRequests_.push(entry);
        queueCopy.pop();
    }
}

void
TestImpl::skewWalltime(sint64 delta)
{
    kernel_->wallTime_ += delta;
}

void
TestImpl::endTimers(void)
{
    std::vector<MockTimerReq *, AllocatorAffinity<MockTimerReq *>> fire;
    while (!kernel_->pendingRequests_.empty()) {
        auto entry = kernel_->pendingRequests_.top();
        kernel_->pendingRequests_.pop();
        fire.push_back(entry.request_);
    }
    for (auto && request : fire) {
        request->cancel_ = true;
        start(request->thunk_);
    }
}

TestEnv &
TestImpl::environment(void)
{
    return *testEnvironment_;
}

Environment &
TestImpl::trueEnvironment(void)
{
    return trueEnvironment_;
}

AutoDispose<> &
TestImpl::cloak(void)
{
    return testCloak_;
}

void
TestImpl::run(
    NoDispose<Request> const & req,
    RequestStatus & status)
{
    status.started_ = true;
    status.notified_ = false;
    status.err_ = nullptr;
    req->start(Completion(requestCompletion, reinterpret_cast<Error *>(&status)));
}

void
TestImpl::runAndAssertSuccess(
    AutoDispose<Request> && req)
{
    if (!!req) {
        RequestStatus status;
        run(std::move(req), status);
        status.success();
    }
}

void
TestImpl::runAndAssertSuccess(
    NoDispose<Request> const & req)
{
    if (!!req) {
        RequestStatus status;
        run(req, status);
        status.success();
    }
}

void
TestImpl::runAndAssertError(
    AutoDispose<Request> && req)
{
    if (!!req) {
        RequestStatus status;
        run(std::move(req), status);
        status.error();
    }
}

void
TestImpl::runAndAssertError(
    NoDispose<Request> const & req)
{
    if (!!req) {
        RequestStatus status;
        run(req, status);
        status.error();
    }
}

void
TestImpl::generatorNext(
    NoDispose<Generator> const & generator,
    unsigned numAsyncs)
{
    if (!!generator) {
        unsigned count = 0;
        while (!generator->next()) {
            TOOLS_ASSERTR(count++ < numAsyncs);
            runAndAssertSuccess(generator);
        }
    }
}

bool
TestImpl::isMainThread(void)
{
    return !!tools::impl::threadLocalGet(tlsKey_);
}

void
TestImpl::firePendingTimers(uint64 activationTime)
{
    while (!kernel_->pendingRequests_.empty()) {
        auto entry = kernel_->pendingRequests_.top();
        if (entry.fireTime_ <= activationTime) {
            kernel_->pendingRequests_.pop();
            if (kernel_->time_ < entry.fireTime_) {
                uint64 movement = static_cast<uint64>(entry.fireTime_ - kernel_->time_);
                fastForwardtime(movement);
            }
            start(entry.request_->thunk_);
        } else {
            break;
        }
    }
}

void
TestImpl::start(Thunk const & thunk)
{
    TOOLS_ASSERT(isMainThread());
    if (!thread_) {
        thread_ = trueEnvironment_.get<Threading>()->fork(name_, toThunk<&TestImpl::workerEntry>());
    }
    {
        AutoDispose<> l(threadControl_->enter());
        TOOLS_ASSERT(!nextThreadThunk_);
        TOOLS_ASSERT(!!thunk);
        nextThreadThunk_ = thunk;
    }
    resumeControl_->signal();
    {
        AutoDispose<> l(threadControl_->enter());
        syncControl_->wait();
    }
}

void
TestImpl::workerEntry(void)
{
    // assertThreadAnnotation(ThreadTypeApplication, nullptr);
    {
        AutoDispose<> l(threadControl_->enter());
        do {
            if (!!nextThreadThunk_) {
                Thunk local = nextThreadThunk_;
                nextThreadThunk_ = Thunk();
                {
                    l = nullptr;
                    AutoDispose<> p(phantomTryBindPrototype<PhantomUniversal>());
                    local.fire();
                    // TODO: poll global capture
                    l = threadControl_->enter();
                }
            } else {
                // Have run out of work
                l = nullptr;
                syncControl_->signal();
                l = threadControl_->enter();
                resumeControl_->wait();
            }
        } while (!terminated_);
        TOOLS_ASSERT(!nextThreadThunk_); // There should be no pending work
    }
    syncControl_->signal();
}

void
TestImpl::requestCompletion(
    void * param,
    Error * err)
{
    RequestStatus * status = reinterpret_cast<RequestStatus *>(param);
    status->notified_ = true;
    if (!!err) {
        status->err_ = err->ref();
    } else {
        status->err_ = nullptr;
    }
}

///////////////
// TestEnvEntry
///////////////

TestEnvEntry::TestEnvEntry(void)
    : service_(nullptr)
{
}

TestEnvEntry::TestEnvEntry(Unknown * service, StringId const & name)
    : service_(service)
    , name_(name)
{
}

/////////////////////////
// ServiceFactoryDelegate
/////////////////////////

ServiceFactoryDelegate::ServiceFactoryDelegate(void)
    : factory_(nullptr)
    , param_(nullptr)
{}

ServiceFactoryDelegate::ServiceFactoryDelegate(
    TestEnv::ServiceFactory factory,
    void * param)
    : factory_(factory)
    , param_(param)
{}

NoDispose<Unknown>
ServiceFactoryDelegate::fire(
    NoDispose<TestEnv> env)
{
    return factory_(env, param_);
}

//////////////
// TestEnvImpl
//////////////

TestEnvImpl::TestEnvImpl(Environment & env, TestImpl & test)
    : trueEnvironment_(env)
    , test_(test)
{
}

StringId const &
TestEnvImpl::name(void)
{
    static StringId ret("test");
    return ret;
}

Unknown *
TestEnvImpl::get(StringId const & name)
{
    StringId requesting(!creating_.empty() ? creating_.back() : StringIdNull());
    if (!IsNullOrEmptyStringId(requesting)) {
        StringId requestName = composeFactoryName(name, requesting);
        auto serviceIter = services_.find(requestName);
        if (serviceIter != services_.end()) {
            return serviceIter->second.service_;
        }
        auto factory = factories_.find(requestName);
        if (factory != factories_.end()) {
            ServiceCreationStack stack(creating_, requestName);
            if (!test_.isMainThread()) {
                // TODO: log this
                fprintf(stderr, "instantiating %s not on the main thread\n", requestName.c_str());
                TOOLS_ASSERT(!"Instanting service not on main thread");
            }
            auto service = factory->second.fire(*this);
            TOOLS_ASSERT(!!service);
            services_[name] = TestEnvEntry(&*service, name);
            return &*service;
        }
    }
    {
        auto serviceIter = services_.find(name);
        if (serviceIter != services_.end()) {
            TOOLS_ASSERT(!!serviceIter->second.service_);
            return serviceIter->second.service_;
        }
    }
    {
        if (!test_.isMainThread()) {
            // TODO: log this
            fprintf(stderr, "instantiating %s not on the main thread\n", name.c_str());
            TOOLS_ASSERT(!"Instantiating service not on main thread");
        }
        if (std::find(creating_.begin(), creating_.end(), name) != creating_.end()) {
            // TODO: log this
            fprintf(stderr, "Service dependency loop while creating %s\n", name.c_str());
            for (auto && svc : creating_) {
                // TODO: log this
                fprintf(stderr, "  creating %s\n", svc.c_str());
            }
            TOOLS_ASSERT(!"Service dependency loop");
        }
    }
    ServiceCreationStack stack(creating_, name);
    {
        auto factory = factories_.find(name);
        if (factory != factories_.end()) {
            if (!test_.isMainThread()) {
                // TODO: log this
                fprintf(stderr, "instantiating %s not on the main thread\n", name.c_str());
                TOOLS_ASSERT(!"Instanting service not on main thread");
            }
            auto service = factory->second.fire(*this);
            TOOLS_ASSERT(!!service);
            services_[name] = TestEnvEntry(&*service, name);
            return &*service;
        }
    }
    // Check the registry for an auto mock
    {
        auto registryAutoMock = registryFetch<tools::AutoMock>(name);
        if (!!registryAutoMock) {
            registryAutoMock->factory(test_, *this);
            auto serviceIter = services_.find(name);
            if (serviceIter == services_.end()) {
                // TODO: log this
                fprintf(stderr, "TestEnvImpl::get() - registry auto mock failed to factory '%s'\n", name.c_str());
                TOOLS_ASSERT(!"TestEnvImpl::get - registry auto mock failed to factory requested service");
                return nullptr;
            }
            TOOLS_ASSERT(!!serviceIter->second.service_);
            return serviceIter->second.service_;
        }
    }
    // Check if the service is inheritable from the true environment
    {
        auto factory = registryFetch<tools::impl::FactoryEnvironment>(name);
        if (!!factory) {
            // This may be inheritable from the true environment, pass it through.
            if (factory->describe().inherritable_) {
                auto * newService = trueEnvironment_.get(name);
                services_[name] = TestEnvEntry(newService, factory->describe().interfaceName_);
                return newService;
            }
        }
    }
    // TODO: log this
    fprintf(stderr, "TestEnvImpl::get - service '%s' not found and no appropriate factories available\n", name.c_str());
    TOOLS_ASSERT(!"TestEnvImpl::get - could not find service");
    return nullptr;
}

Test &
TestEnvImpl::getTest(void)
{
    return test_;
}

void
TestEnvImpl::mock(
    StringId const & name,
    NoDispose<Unknown> itf,
    bool overwrite)
{
    TOOLS_ASSERT(registryFetch<tools::impl::FactoryEnvironment>(name));
    TOOLS_ASSERT(overwrite || (services_.find(name) == services_.end()));
    services_[name] = TestEnvEntry(&*itf, name);
}

void
TestEnvImpl::setFactory(
    StringId const & name,
    TestEnv::ServiceFactory func,
    void * param,
    StringId const & requesting)
{
    StringId factoryName(composeFactoryName(name, requesting));
    TOOLS_ASSERT(services_.find(name) == services_.end());
    TOOLS_ASSERT(factories_.find(factoryName) == factories_.end());
    factories_[factoryName] = ServiceFactoryDelegate(func, param);
}

void
TestEnvImpl::unmock(
    StringId const & name)
{
    setFactory(name, unmockFactory, new StringId(name));
}

NoDispose<Unknown>
TestEnvImpl::unmockNow(
    StringId const & name)
{
    auto factory = registryFetch<tools::impl::FactoryEnvironment>(name);
    if (!factory) {
        // TODO: log this
        fprintf(stderr, "Cannot find factory for %s\n", name.c_str());
        TOOLS_ASSERT(!"Cannot find factory");
    }
    auto service = test_.finalize(factory->factory(*this));
    TOOLS_ASSERT(!!service);
    mock(name, service);
    // TODO: log this
    // start unmock for %s, name.c_str()
    test_.runAndAssertSuccess(service->start());
    stopUnmocked(service);
    return service;
}

NoDispose<Unknown>
TestEnvImpl::createReal(
    StringId const & name)
{
    auto factory = registryFetch<tools::impl::FactoryEnvironment>(name);
    if (!factory) {
        // TODO: log this
        fprintf(stderr, "Cannot find factory for '%s'\n", name.c_str());
        TOOLS_ASSERTR(!"Cannot find factory");
    }
    auto service = test_.finalize(factory->factory(*this));
    test_.runAndAssertSuccess(service->start());
    stopUnmocked(service);
    return service;
}

void
TestEnvImpl::stopUnmocked(
    NoDispose<Service> service,
    unsigned level)
{
    Environment::get<UnmockStopper>()->stop(service, level);
}

void
TestEnvImpl::stopUnmockedNow(
    NoDispose<Service> service)
{
    Environment::get<UnmockStopper>()->stopNow(service);
}

///////////////////////
// ServiceCreationStack
///////////////////////

ServiceCreationStack::ServiceCreationStack(
    TestEnvImpl::CreatingVec & creating,
    StringId const & name)
    : creating_(creating)
    , name_(name)
{
    creating_.push_back(name);
}

ServiceCreationStack::~ServiceCreationStack(void)
{
    TOOLS_ASSERT(!creating_.empty() && (creating_.back() == name_));
    creating_.pop_back();
}

/////////////////////
// TestManagementImpl
/////////////////////

TestManagementImpl::TestManagementImpl(Environment & env)
    : env_(env)
{
}

unsigned
TestManagementImpl::run(StringId const & filter)
{
    unsigned numRun = 0U;
    registryVisit<tools::unittest::TestCase>([&](StringId const & name, tools::unittest::TestCase * testCase) {
        if (TestManagementImpl::acceptTest(filter, name)) {
            executeSingle(name, *testCase);
            atomicIncrement(&numRun);
        }
    });
    return numRun;
}

unsigned
TestManagementImpl::list(StringId const & filter)
{
    unsigned numRun = 0U;
    registryVisit<tools::unittest::TestCase>([&](StringId const & name, tools::unittest::TestCase *) {
        if (TestManagementImpl::acceptTest(filter, name)) {
            // TODO: log this
            fprintf(stdout, "%s\n", name.c_str());
            atomicIncrement(&numRun);
        }
    });
    return numRun;
}

void
TestManagementImpl::executeSingle(StringId const & name, tools::unittest::TestCase & testCase)
{
    {
        TestImpl test(env_, name);
        // TODO: log this
        fprintf(stdout, "[%s] begin\n", name.c_str());
        // Setup an error handler
        // currentTestName = name;
        // Actually run the test
        testCase.run(test);
        TOOLS_ASSERTR(tools::detail::memoryValidate());
    }
    TOOLS_ASSERTR(tools::detail::memoryValidate());
    // TODO: log this
    fprintf(stdout, "[%s] end\n", name.c_str());
    // currentTestName = StringIdNull();
}

bool
TestManagementImpl::acceptTest(StringId const & filter, StringId const & name)
{
    bool ret = (IsNullOrEmptyStringId(filter) || (filter == name));
    if (!ret) {
        // TODO: test prefix match
        // TODO: test regex match
    }
    return ret;
}

////////////////
// MockScheduler
////////////////

MockScheduler::MockScheduler(TestImpl & parent)
    : parent_(parent)
    , time_(887U)
    , wallTime_(400U * 86400U * TOOLS_NANOSECONDS_PER_SECOND)
{
}

MockScheduler::~MockScheduler(void)
{
    TOOLS_ASSERT(pendingRequests_.empty());
}

uint64
MockScheduler::mark(void)
{
    // Help ensure that time marches forward, no matter how slowly
    ++time_;
    ++wallTime_;
    return time_;
}

uint64
MockScheduler::mark(uint64 mark)
{
    // Help ensure that time marches forward, no matter how slowly
    ++time_;
    ++wallTime_;
    return static_cast<uint64>(time_ - mark);
}

AutoDispose<Request>
MockScheduler::timer(uint64 waitTime, uint64 * outStartTime)
{
    return new MockTimerReq(parent_, *this, waitTime, outStartTime);
}

///////////////
// MockTimerReq
///////////////

MockTimerReq::MockTimerReq(TestImpl & test, MockScheduler & timing, uint64 waitTime, uint64 * outStartTime)
    : running_(false)
    , cancel_(false)
    , test_(test)
    , timing_(timing)
    , waitTime_(waitTime)
    , outStartTime_(outStartTime)
{
}

RequestStep
MockTimerReq::start()
{
    return suspend<&MockTimerReq::escape>();
}

void
MockTimerReq::escape(void)
{
    TOOLS_ASSERT(!running_);
    running_ = true;
    thunk_ = toThunk<&MockTimerReq::reenter>();
    auto fireTime = timing_.time_ + waitTime_;
    timing_.pendingRequests_.push(MockTimerEntry(*this, fireTime));
    if (!!outStartTime_) {
        *outStartTime_ = timing_.time_;
    }
}

void
MockTimerReq::reenter(void)
{
    TOOLS_ASSERT(running_);
    running_ = false;
    AutoDispose< Error::Reference > err;
    if (cancel_) {
        err = errorCancelNew();
    }
    resumeFinish(err.get());
}

////////////////////
// UnmockStopperImpl
////////////////////

UnmockStopperImpl::UnmockStopperImpl(
    Environment &)
    : test_(nullTestNew())
{
    TOOLS_ASSERT(!"Should never contruct UnmockStopperImpl via standard environment factory");
}

UnmockStopperImpl::UnmockStopperImpl(
    Test & test,
    NoDispose<TestEnv>)
    : test_(test)
{
}

UnmockStopperImpl::~UnmockStopperImpl(void)
{
    TOOLS_ASSERT(services_[0].empty());
    TOOLS_ASSERT(services_[1].empty());
}

void
UnmockStopperImpl::stop(
    NoDispose<Service> service,
    unsigned level)
{
    TOOLS_ASSERT((level == 0) || (level == 1));
    services_[level].push_back(service);
}

void
UnmockStopperImpl::doStops(void)
{
    for (unsigned level = 2; level != 0; --level) {
        for (auto iter = services_[level - 1].rbegin(); iter != services_[level - 1].rend(); ++iter) {
            stopInner(*iter);
        }
        services_[level - 1].clear();
    }
    // Cancel any outstanding timers
    test_.endTimers();
}

void
UnmockStopperImpl::stopNow(
    NoDispose<Service> service)
{
    for (unsigned level = 2; level != 0; --level) {
        for (auto iter = services_[level - 1].begin(); iter != services_[level - 1].end(); ++iter) {
            if (*iter == service) {
                stopInner(*iter);
                services_[level - 1].erase(iter);
                return;
            }
        }
    }
    TOOLS_ASSERT(!"Failed to find service to stop");
}

void
UnmockStopperImpl::stopInner(
    NoDispose<Service> service)
{
    stop_ = service->stop();
    if (!!stop_) {
        done_ = triggerRequestNew(trigger_);
        stop_->start(toCompletion<&UnmockStopperImpl::stopped>());
        // If stop() goes async, then either it has gone async on some other service (which is an error, BTW) or on
        // a timer. So in that case fire timers now. TODO: consider making the timer service cancel early.
        if (!!stop_) {
            test_.progressTime();
        }
        // Sleep now in order to support tests which use the real scheduler.
        runRequestSynchronously(*done_);
    }
}

void
UnmockStopperImpl::stopped(
    Error *)
{
    stop_ = nullptr;
    trigger_ = nullptr;
}

//////////////
// NullTestEnv
//////////////

NullTestEnv::NullTestEnv(Test & test)
    : test_(test)
{
}

StringId const &
NullTestEnv::name(void)
{
    TOOLS_ASSERT(!"NullTestEnv::name should never be called");
    return StringIdNull();
}

Unknown *
NullTestEnv::get(StringId const &)
{
    TOOLS_ASSERT(!"NullTestEnv::get should never be called");
    return nullptr;
}

Test &
NullTestEnv::getTest(void)
{
    TOOLS_ASSERT(!"NullTestEnv::getTest should never be called");
    return test_;
}

void
NullTestEnv::mock(StringId const &, NoDispose<Unknown>, bool)
{
    TOOLS_ASSERT(!"NullTestEnv::mock should never be called");
}

void
NullTestEnv::setFactory(StringId const &, ServiceFactory, void *, StringId const &)
{
    TOOLS_ASSERT(!"NullTestEnv::setFactory should never be called");
}

void
NullTestEnv::unmock(StringId const &)
{
    TOOLS_ASSERT(!"NullTestEnv::unmock should never be called");
}

NoDispose<Unknown>
NullTestEnv::unmockNow(StringId const &)
{
    TOOLS_ASSERT(!"NullTestEnv::unmockNow should never be called");
    return nullptr;
}

NoDispose<Unknown>
NullTestEnv::createReal(StringId const &)
{
    TOOLS_ASSERT(!"NullTestEnv::createReal should never be called");
    return nullptr;
}

void
NullTestEnv::stopUnmocked(NoDispose<Service>, unsigned)
{
    TOOLS_ASSERT(!"NullTestEnv::stopUnmocked should never be called");
}

void
NullTestEnv::stopUnmockedNow(NoDispose<Service>)
{
    TOOLS_ASSERT(!"NullTestEnv::stopUnmockedNow should never be called");
}

///////////
// NullTest
///////////

NullTest::NullTest(void)
    : env_(*this)
{}

void
NullTest::finalize_inner(AutoDispose<> &&)
{
    TOOLS_ASSERT(!"NullTest::finalize_inner should never be called");
}

void
NullTest::sync(void)
{
    TOOLS_ASSERT(!"NullTest::sync should never be called");
}

void
NullTest::resume(void)
{
    TOOLS_ASSERT(!"NullTest::resume should never be called");
}

void
NullTest::progressTime(void)
{
    TOOLS_ASSERT(!"NullTest::progressTime should never be called");
}

void
NullTest::progressTime(uint64)
{
    TOOLS_ASSERT(!"NullTest::progressTime should never be called");
}

void
NullTest::fastForwardtime(uint64)
{
    TOOLS_ASSERT(!"NullTest::fastForwardtime should never be called");
}

void
NullTest::adjustPendingTimer(sint64)
{
    TOOLS_ASSERT(!"NullTest::adjustPendingTimer should never be called");
}

void
NullTest::skewWalltime(sint64)
{
    TOOLS_ASSERT(!"NullTest::skewWalltime should never be called");
}

void
NullTest::endTimers(void)
{
    TOOLS_ASSERT(!"NullTest::endTimers should never be called");
}

TestEnv &
NullTest::environment(void)
{
    TOOLS_ASSERT(!"NullTest::environment should never be called");
    return env_;
}

Environment &
NullTest::trueEnvironment(void)
{
    TOOLS_ASSERT(!"NullTest::trueEnvironment should never be called");
    return env_;
}

AutoDispose<> &
NullTest::cloak(void)
{
    static AutoDispose<> ret;
    TOOLS_ASSERT(!"NullTest::cloak should never be called");
    return ret;
}

void
NullTest::run(NoDispose<Request> const &, RequestStatus &)
{
    TOOLS_ASSERT(!"NullTest::run should never be called");
}

void
NullTest::runAndAssertSuccess(AutoDispose<Request> &&)
{
    TOOLS_ASSERT(!"NullTest::runAndAssertSuccess should never be called");
}

void
NullTest::runAndAssertSuccess(NoDispose<Request> const &)
{
    TOOLS_ASSERT(!"NullTest::runAndAssertSuccess should never be called");
}

void
NullTest::runAndAssertError(AutoDispose<Request> &&)
{
    TOOLS_ASSERT(!"NullTest::runAndAssertError should never be called");
}

void
NullTest::runAndAssertError(NoDispose<Request> const &)
{
    TOOLS_ASSERT(!"NullTest::runAndAssertError should never be called");
}

void
NullTest::generatorNext(NoDispose<Generator> const &, unsigned)
{
    TOOLS_ASSERT(!"NullTest::generatorNext should never be called");
}

///////////////////////
// Static registrations
///////////////////////

static RegisterEnvironment< tools::unittest::impl::Management, TestManagementImpl > regManagement;
static RegisterEnvironment< UnmockStopper, UnmockStopperImpl > regStopper;
static RegisterAuto<AutoMock, UnmockStopper> regUnmock;

// Do we have unit tests for the unit test framework?  Of course we do!
#if TOOLS_UNIT_TEST

TOOLS_TEST_CASE("unittest.trivial.registration", [](Test &)
{
    TOOLS_ASSERTR(true);
});

TOOLS_TEST_CASE("unittest.parameterized.singleValue", testParamValues(1)(2)(3)(4)(5), [](Test &, int n)
{
    TOOLS_ASSERTR(n >= 1);
    TOOLS_ASSERTR(n <= 5);
});

TOOLS_TEST_CASE("unittest.parameterized.singleValue.initializerList", testParamValues({ 1, 2, 3, 4, 5 }), [](Test &, int n)
{
    TOOLS_ASSERTR(n >= 1);
    TOOLS_ASSERTR(n <= 5);
});

TOOLS_TEST_CASE("unittest.parameterized.multiValue", testParamValues(1, "1")(2, "2")(3, "3"), [](Test &, int n, StringId const & s)
{
    boost::format fmt("%d");
    fmt % n;
    TOOLS_ASSERT(s == fmt.str().c_str());
});

TOOLS_TEST_CASE("unittest.parameterized.emptyParameter", tools::detail::TestParameterEmpty<int, StringId>(), [](Test &, int, StringId const &)
{
    TOOLS_ASSERT(!"Body of unittest.parameterized.emptyParameter should never be called");
});

TOOLS_TEST_CASE("unittest.parameterized.combine", testParamCombine<int, StringId>({ 1, 2, 3, 4, 5 }, { "a", "b" }), [](Test &, int n, StringId const & s)
{
    TOOLS_ASSERTR(n >= 1);
    TOOLS_ASSERTR(n <= 5);
    TOOLS_ASSERT((s == "a") || (s == "b"));
});

TOOLS_TEST_CASE("unittest.parameterized.combine.empty", testParamCombine<int, StringId>({ 1, 2, 3, 4, 5 }, std::initializer_list<StringId>{}), [](Test &, int, StringId const &)
{
    TOOLS_ASSERT(!"Body of unittest.parameterized.combine.empty should never be called");
});

#endif /* TOOLS_UNIT_TEST != 0 */
