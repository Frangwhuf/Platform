#pragma once

/* #include <tools/Memory.h> */
#include <tools/Environment.h>
#include <tools/InterfaceTools.h>
#include <tools/String.h>
#include <tools/Tools.h>

#ifdef WINDOWS_PLATFORM
#  pragma warning( disable : 4365 )
#  pragma warning( disable : 4371 )
#  pragma warning( disable : 4548 )
#  pragma warning( disable : 4571 )
#endif // WINDOWS_PLATFORM
#include <boost/format.hpp>
#ifdef WINDOWS_PLATFORM
#  pragma warning( default : 4365 )
#  pragma warning( default : 4371 )
#  pragma warning( default : 4548 )
#  pragma warning( default : 4571 )
#endif // WINDOWS_PLATFORM

#include <list>
#include <tuple>

namespace tools {
    struct Test;
	struct TestEnv : Environment
    {
        typedef tools::NoDispose<tools::Unknown>(*ServiceFactory)(tools::NoDispose<TestEnv>, void *);

        template<typename ServiceT>
        struct SetFactoryTrampoline
            : tools::StandardDisposable<SetFactoryTrampoline<ServiceT>>
        {
            SetFactoryTrampoline(std::function<tools::NoDispose<ServiceT>(tools::NoDispose<TestEnv>)> & func)
                : func_(func)
            {}

            static tools::NoDispose<tools::Unknown> runFactory(tools::NoDispose<TestEnv> env, void * param)
            {
                auto this_ = static_cast<SetFactoryTrampoline<ServiceT> *>(param);
                return this_->func_(env);
            }

            std::function<tools::NoDispose<ServiceT>(tools::NoDispose<TestEnv>)> func_;
        };

        virtual Test & getTest(void) = 0;
        virtual void mock(tools::StringId const &, tools::NoDispose<tools::Unknown>, bool = false) = 0;
        template<typename ServiceT, typename AnyT>
        void 
        mock(tools::NoDispose<AnyT> mocked, bool override = false, ServiceT *** = 0, AnyT *** = 0)
        {
            mock(tools::nameOf<ServiceT>(), mocked, override);
        }
        virtual void setFactory(tools::StringId const &, ServiceFactory, void *, tools::StringId const & = tools::StringIdNull()) = 0;
        template<typename ServiceT>
        void
        setFactory(std::function<tools::NoDispose<typename tools::ServiceInterfaceOf<ServiceT>::Type> (tools::NoDispose<TestEnv>)> const & sfunc, ServiceT *** = 0)
        {
            typedef typename tools::ServiceInterfaceOf<ServiceT>::Type InterfaceT;
            auto trampoline = this->getTest()->finalize(new SetFactoryTrampoline<InterfaceT>(sf));
            setFactory(tools::nameOf<ServiceT>(), &SetFactoryTrampoline<ServiceT>::runFactory, &*trampoline);
        }
        virtual void unmock(tools::StringId const &) = 0;
        template<typename ServiceT>
        void 
        unmock(ServiceT *** = 0)
        {
            unmock(tools::nameOf<ServiceT>());
        }
        virtual tools::NoDispose<tools::Unknown> unmockNow(tools::StringId const &) = 0;
        template<typename ServiceT>
        typename tools::ServiceInterfaceOf<ServiceT>::Type *
        unmockNow(ServiceT *** = 0)
        {
            return unmockNow(tools::nameOf<ServiceT>())->getInterface<typename tools::ServiceInterfaceOf<ServiceT>::Type>();
        }
        virtual tools::NoDispose<tools::Unknown> createReal(tools::StringId const &) = 0;
        template<typename ServiceT>
        tools::NoDispose<typename tools::ServiceInterfaceOf<ServiceT>::Type>
        createReal(ServiceT *** = 0)
        {
            return static_cast<typename tools::ServiceInterfaceOf<ServiceT>::Type &>(createReal(tools::nameOf<ServiceT>()));
        }
        virtual void stopUnmocked(tools::NoDispose<tools::Service>, unsigned = 0) = 0;
        virtual void stopUnmockedNow(tools::NoDispose<tools::Service>) = 0;
    };
	struct Test
	{
        struct RequestStatus
            : AllocStatic<>
        {
            TOOLS_FORCE_INLINE RequestStatus(void) : started_(false), notified_(false) {}

            TOOLS_FORCE_INLINE void success(void) const {
                TOOLS_ASSERTR(started_);
                TOOLS_ASSERTR(notified_ && "Request may have gone asyc");
                TOOLS_ASSERTR(!err_);
            }
            TOOLS_FORCE_INLINE void error(void) const {
                TOOLS_ASSERTR(started_);
                TOOLS_ASSERTR(notified_ && "Request may have gone async");
                TOOLS_ASSERTR(!!err_);
            }
            TOOLS_FORCE_INLINE void unnotified(void) const {
                TOOLS_ASSERTR(started_);
                TOOLS_ASSERTR(!notified_);
            }

            bool started_;
            bool notified_;
            AutoDispose<Error::Reference> err_;
        };
    protected:
        virtual void finalize_inner(AutoDispose<> &&) = 0;
    public:

		virtual void sync(void) = 0;
		virtual void resume(void) = 0;
		virtual void progressTime(void) = 0;
		virtual void progressTime(uint64) = 0;
		virtual void fastForwardtime(uint64) = 0;
		virtual void adjustPendingTimer(sint64) = 0;
		virtual void skewWalltime(sint64) = 0;
		virtual void endTimers(void) = 0;
		virtual TestEnv & environment(void) = 0;
		virtual Environment & trueEnvironment(void) = 0;
		virtual AutoDispose<> & cloak(void) = 0;

        template<typename AnyT>
        TOOLS_FORCE_INLINE NoDispose<AnyT>
        finalize(AutoDispose<AnyT> && disp)
        {
            NoDispose<AnyT> ret(disp);
            this->finalize_inner(std::move(disp));
            return ret;
        }
        template<typename AnyT>
        TOOLS_FORCE_INLINE NoDispose<AnyT>
            finalize(AnyT * && disp)
        {
            NoDispose<AnyT> ret(*disp);
            this->finalize_inner(tools::AutoDispose<>(disp));
            return ret;
        }

        virtual void run(NoDispose<Request> const &, RequestStatus &) = 0;
        TOOLS_FORCE_INLINE void run(AutoDispose<Request> && req, RequestStatus & status) {
            run(this->finalize(std::move(req)), status);
        }
        virtual void runAndAssertSuccess(AutoDispose<Request> &&) = 0;
        virtual void runAndAssertSuccess(NoDispose<Request> const &) = 0;
        virtual void runAndAssertError(AutoDispose<Request> &&) = 0;
        virtual void runAndAssertError(NoDispose<Request> const &) = 0;

        virtual void generatorNext(NoDispose<Generator> const &, unsigned = 5) = 0;
	};

    struct AutoMock
    {
        virtual void factory(Test & test, NoDispose<TestEnv> subEnv) = 0;
    };

	namespace unittest {
		struct TestCase
		{
			virtual void run(tools::Test &) = 0;
		};

		struct DisposableTestCase : TestCase, Disposable {};

		namespace impl {
            struct Management
            {
                virtual unsigned run(StringId const &) = 0;
                virtual unsigned list(StringId const &) = 0;
                virtual void executeSingle(StringId const &, TestCase &) = 0;
            };

            template<typename ServiceT>
            struct RegisterReal
                : tools::AutoMock
            {
                // AutoMock
                void factory(tools::Test &, tools::NoDispose<tools::TestEnv> subEnv) override
                {
                    subEnv->unmockNow<ServiceT>();
                }
            };

            TOOLS_API void registerMockHelper(Test &, NoDispose<Service>);
		}; // impl namespace
	}; // unittest namespace
    template<typename ServiceT, typename ImplementationT>
    struct RegisterMock
        : AutoMock
    {
        // AutoMock
        void factory(Test & test, NoDispose<TestEnv> subEnv) override
        {
            auto impl = test.finalize(new ImplementationT(test, subEnv));
            subEnv->mock<ServiceT>(impl);
            tools::unittest::impl::registerMockHelper(test, impl);
        }
    };

    template<typename ServiceT>
    auto standardAutoRegister(AutoMock ***, ServiceT ***)->
        tools::unittest::impl::RegisterReal<ServiceT>;

    template<typename ServiceT>
    TOOLS_FORCE_INLINE void registryAutoDepends(AutoMock ***, ServiceT ***)
    {
        // no dependencies
    }

	struct RegisterTestFunctor
	{
		// The actual inner implementation for a test case.
		template<typename TestBodyT>
		struct TestCaseImplementation
			: tools::StandardDisposable<TestCaseImplementation<TestBodyT>, tools::unittest::DisposableTestCase, tools::AllocStatic<Platform>>
		{
			explicit TestCaseImplementation(TestBodyT const & f)
				: func_(f)
			{}

			void run(tools::Test & test) override
			{
				func_(test);
			}

			TestBodyT func_;
		};

		// Helper templates used to unpack parameter tuples as function call parameters.
		// An excellent reference is http://stackoverflow.com/questions/7858817/unpacking-a-tuple-to-call-a-matching-function-pointer
		
		// First the sequence type.
		template<int ...>
		struct Sequence {};

		// Next a generator type for the sequence.
		template<int N, int ...S>
		struct GeneratorSequence : GeneratorSequence<N - 1, N - 1, S...> {};
		template<int ...S>
		struct GeneratorSequence<0, S...>
		{
			typedef Sequence<S...> Type;
		};

		// An implementation for a parameterized test case.
		template<typename TestBodyT, typename... ParameterT>
		struct ParameterizedTestCaseImplementation
			: tools::StandardDisposable<ParameterizedTestCaseImplementation<TestBodyT, ParameterT...>, tools::unittest::DisposableTestCase, tools::AllocStatic<Platform>>
		{
			ParameterizedTestCaseImplementation(TestBodyT func, std::tuple<ParameterT...> parameters)
				: parameters_(parameters)
				, func_(func)
			{}

			// Call func_ with the test and the parameter pack for that instance
			template<int... S>
			void call(tools::Test & test, Sequence<S...>)
			{
				func_(test, std::get<S>(parameters_) ...);
			}

			void run(tools::Test & test) override
			{
				call(test, typename GeneratorSequence<sizeof...(ParameterT)>::Type());
			}

			// Parameters for the test case(s)
			std::tuple<ParameterT...> parameters_;
			TestBodyT func_;
		};

		// Inner registration for parameterized unit tests. This will register each inner test configuration under
		// the name: name/index
		template<typename GeneratorT, typename TestBodyT>
		struct ParameterizedTestRegistration
			: tools::StandardDisposable<ParameterizedTestRegistration<GeneratorT, TestBodyT>, tools::Disposable, tools::AllocStatic<Platform>>
		{
			ParameterizedTestRegistration(tools::StringId const & name, GeneratorT gen, TestBodyT func)
			{
                boost::format fmt("%s/%d");
				int idx = 0;
				while (!gen.empty()) {
					auto param = gen.next();
                    fmt % name % idx;
					tools::AutoDisposePair<tools::unittest::TestCase> test(nextCase(func, param));
					tools::AutoDispose<> registration(tools::registryInsert(tools::nameOf<tools::unittest::TestCase>(), fmt.str().c_str(), test.get()));
					tests_.push_back(std::move(test));
					registrations_.push_back(std::move(registration));
					++idx;
				}
			}

			template<typename... ParameterT>
			tools::AutoDisposePair<tools::unittest::TestCase>
			nextCase(TestBodyT const & func, std::tuple<ParameterT...> params)
			{
				return tools::AutoDisposePair<tools::unittest::TestCase>(new ParameterizedTestCaseImplementation<TestBodyT, ParameterT...>(func, params));
			}

			std::list<tools::AutoDisposePair<tools::unittest::TestCase>, tools::AllocatorAffinity<tools::AutoDisposePair<tools::unittest::TestCase>, Platform>> tests_;
			std::list<tools::AutoDispose<>, tools::AllocatorAffinity<tools::AutoDispose<>, Platform>> registrations_;
		};

		struct DisabledTestCaseImplementation
			: tools::StandardDisposable<DisabledTestCaseImplementation, tools::unittest::DisposableTestCase, tools::AllocStatic<Platform>>
		{
			DisabledTestCaseImplementation(RegisterTestFunctor & parent, char const * const reason)
				: parent_(parent)
				, reason_(reason)
			{}

			void run(tools::Test &) override
			{
				// TODO: log that parent.name_ is disabled for reason_
                fprintf(stderr, "Test '%s' disabled because: %s\n", parent_.name_.c_str(), reason_);
			}

			RegisterTestFunctor & parent_;
			char const * const reason_;
		};

		template<typename TestBodyT>
		RegisterTestFunctor(char const * const name, TestBodyT const & func)
			: name_(name)
		{
			// Check if this is a duplicate test case
			void * prev = tools::registryFetch(tools::nameOf<tools::unittest::TestCase>(), name_);
			TOOLS_ASSERT(!prev);
            tools::AutoDispose<tools::unittest::DisposableTestCase> newTest(new TestCaseImplementation<TestBodyT>(func));
            tools::unittest::TestCase & ref = *newTest;
			test_ = tools::AutoDisposePair<tools::unittest::TestCase>(ref, std::move(newTest));
			registration_ = tools::registryInsert(tools::nameOf<tools::unittest::TestCase>(), name_, test_.get());
		}

		template<typename TestBodyT>
		RegisterTestFunctor(char const * const name, char const * const disable, TestBodyT const &)
			: name_(name)
		{
			// Check if this is a duplicate test case
			void * prev = tools::registryFetch(tools::nameOf<tools::unittest::TestCase>(), name_);
			TOOLS_ASSERT(!prev);
            tools::AutoDispose<tools::unittest::DisposableTestCase> newTest(new DisabledTestCaseImplementation(*this, disable));
            tools::unittest::TestCase & ref = *newTest;
			test_ = tools::AutoDisposePair<tools::unittest::TestCase>(ref, std::move(newTest));
			registration_ = tools::registryInsert(tools::nameOf<tools::unittest::TestCase>(), name_, test_.get());
		}

		typedef void (*NonMemberTestFunc)(tools::Test &);

		RegisterTestFunctor(char const * const name, NonMemberTestFunc func)
			: name_(name)
		{
			// Check if this is a duplicate test case
			void * prev = tools::registryFetch(tools::nameOf<tools::unittest::TestCase>(), name_);
			TOOLS_ASSERT(!prev);
            tools::AutoDispose<tools::unittest::DisposableTestCase> newTest(new TestCaseImplementation<NonMemberTestFunc>(func));
            tools::unittest::TestCase & ref = *newTest;
			test_ = tools::AutoDisposePair<tools::unittest::TestCase>(ref, std::move(newTest));
			registration_ = tools::registryInsert(tools::nameOf<tools::unittest::TestCase>(), name_, test_.get());
		}

		RegisterTestFunctor(char const * const name, char const * const disable, NonMemberTestFunc)
			: name_(name)
		{
			// Check if this is a duplicate test case
			void * prev = tools::registryFetch(tools::nameOf<tools::unittest::TestCase>(), name_);
			TOOLS_ASSERT(!prev);
            tools::AutoDispose<tools::unittest::DisposableTestCase> newTest(new DisabledTestCaseImplementation(*this, disable));
            tools::unittest::TestCase & ref = *newTest;
			test_ = tools::AutoDisposePair<tools::unittest::TestCase>(ref, std::move(newTest));
			registration_ = tools::registryInsert(tools::nameOf<tools::unittest::TestCase>(), name_, test_.get());
		}

		// Registration entry point for parameterized test cases. The test body is registered multiple times, once
		// for each set of parameter resulting from a generation type.
		// 
		// Parameter generation type must have the following:
		//    - a typedef ParameterT that gives the type of the parameter
		//    - a method 'bool empty() const' -- true if al values have been generated
		//    - a method 'ParameterT next()' the yields the next value
		//    - the generation type must be copyable
		//
		// Some usage examples:
		//    TOOLS_TEST_CASE("my.test", TestParamValues(1)(2)(3)(5)(8), [](Test & test, int fib) { ... });
		//    TOOLS_TEST_CASE("some.test", TestParamValues(1, 1.0)(2, 2.0)(3, 3.0)(5, 5.0)(8, 8.0),
		//       [](Test & test, int fid, double fid2) { ... });
		//    TOOLS_TEST_CASE("another", TestParamValues({1, 2, 3, 5, 8}), [](Test & test, int fib) { ... });
		//
		// Other generators can be created to expand combinatorics of values. For example:
		//    TOOLS_TEST_CASE("many", TestParamCombine({1, 2, 3}, {5, 6}), [](Test & test, int a, int b) { ... });
		//    - this would generate 6 instances of the test with the values:
		//       (1, 5), (2, 5), (3, 5), (1, 6), (2, 6), (3, 6)
		template<typename ParameterGeneratorT, typename TestBodyT>
		RegisterTestFunctor(char const * const name, ParameterGeneratorT generator, TestBodyT const & func)
			: name_(name)
		{
			registration_ = new ParameterizedTestRegistration<ParameterGeneratorT, TestBodyT>(name_, generator, func);
		}

		// This is the entrypoint to execute the test(s) from this registration.
		void run(tools::Environment & env)
		{
            env.get<tools::unittest::impl::Management>()->executeSingle(name_, *test_);
		}

		tools::StringId name_;
		tools::AutoDisposePair<tools::unittest::TestCase> test_;
		tools::AutoDispose<> registration_;
	};

	namespace detail
	{
		// A test parameter generator that results in a sequence of values.
		template<typename... ParamT>
		struct TestParameterValueProvider
		{
			typedef TestParameterValueProvider<ParamT...> ThisT;
			typedef std::tuple<ParamT...> ParameterT;

			bool empty(void) const
			{
				return values_.empty();
			}

			ParameterT next(void)
			{
				auto param = values_.front();
				values_.pop_front();
				return param;
			}

			// call operator used to add new values to the collection
			ThisT & operator()(ParamT... param)
			{
				values_.emplace_back(param...);
				return *this;
			}

			std::list<ParameterT> values_;
		};

		// A test parameter generator that results in no value.
		template<typename... ParamT>
		struct TestParameterEmpty
		{
			typedef std::tuple<ParamT...> ParameterT;

			bool empty(void) const
			{
				return true;
			}

			ParameterT next(void)
			{
				ParameterT defaultParameter;
				TOOLS_ASSERT(!"This should never be called");
				return defaultParameter;
			}
		};

		// A test parameter generator that results in a combinatoric mix of two value sequences.
		template<typename Param1T, typename Param2T>
		struct TestParameterCombinationProvider
		{
			typedef std::tuple<Param1T, Param2T> ParameterT;

			TestParameterCombinationProvider(std::initializer_list<Param1T> gen1, std::initializer_list<Param2T> gen2)
				: generator1_(gen1)
				, generator2_(gen2)
				, index_(0)
			{}

			bool empty(void) const
			{
				return generator1_.empty() || generator2_.empty() || (index_ >= (generator1_.size() * generator2_.size()));
			}

			ParameterT next(void)
			{
				TOOLS_ASSERT(!empty());
				ParameterT current = std::make_tuple(generator1_[index_ % generator1_.size()], generator2_[index_ / generator1_.size()]);
				++index_;
				return current;
			}

			typename std::vector<Param1T> generator1_;
			typename std::vector<Param2T> generator2_;
			size_t index_;
		};
	}; // detail namespace

	template<typename... ParamT>
	detail::TestParameterValueProvider<ParamT...>
	testParamValues(ParamT... params)
	{
		auto provider = tools::detail::TestParameterValueProvider<ParamT...>();
		provider(params...);
		return provider;
	}

	template<typename ParamT>
	detail::TestParameterValueProvider<ParamT>
	testParamValues(std::initializer_list<ParamT> params)
	{
		auto provider = tools::detail::TestParameterValueProvider<ParamT>();
		for (auto && param : params) {
			provider(param);
		}
		return provider;
	}

	template<typename Param1T, typename Param2T>
	detail::TestParameterCombinationProvider<Param1T, Param2T>
	testParamCombine(std::initializer_list<Param1T> generator1, std::initializer_list<Param2T> generator2)
	{
		return tools::detail::TestParameterCombinationProvider<Param1T, Param2T>(generator1, generator2);
	}
}; // tools namespace

// Unit test registration should take one of the following forms:
//   For normal/simple test registration
//   TOOLS_TEST_CASE("some.test.name.hierarchy", [](tools::Test &)->void { ... });
//
//   For any disabled test
//   TOOLS_TEST_CASE("some.test.name.hierarchy", "reason for being disabled", testLambda);
//
//   For any parameterized test, more on this below
//   TOOLS_TEST_CASE("some.test.name.hierarchy", parameterization, testLambda);
#define TOOLS_TEST_CASE static tools::RegisterTestFunctor TOOLS_UNIQUE_LOCAL(__test_case_)
