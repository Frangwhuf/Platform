#pragma once

#include <tools/Memory.h>
#include <tools/Async.h>
#include <tools/Threading.h>

namespace tools {
    template< typename ImplementationT, typename AllocatorT = tools::AllocStatic< tools::Temporal >, typename InterfaceT = tools::Request >
    struct StandardManualRequest
        : tools::StandardDisposable< ImplementationT, InterfaceT, AllocatorT >
        , tools::Completable< ImplementationT >
    {
        static_assert( std::is_same< tools::Request, InterfaceT >::value || std::is_base_of< tools::Request, InterfaceT >::value,
            "InterfaceT must derive from tools::Request" );
        typedef typename tools::Completable< ImplementationT >::ParamT NotifyT;

        void
        finish( void )
        {
            // TODO: assert no lock held
            TOOLS_ASSERT( !!notify_ );
            notify_.fire();
        }
        void
        finish( tools::Error & err )
        {
            // TODO: assert no lock held
            TOOLS_ASSERT( !!notify_ );
            notify_.fire( &err );
        }
        Completion
        finishDetach( void )
        {
            TOOLS_ASSERT( !!notify_ );
            tools::Completion notify;
            std::swap( notify, notify_ );
            return notify;
        }
        template< void (ImplementationT::*FuncT)( Error * ) >
        void
        call(
            tools::Request & r,
            typename NotifyT::template ImplementationMethod< FuncT > *** = 0)
        {
            // TODO: assert no lock held
            TOOLS_ASSERT( !!notify_ );
            r.start( tools::Completable< ImplementationT >::template toCompletion< FuncT >() );
        }
        void
        callFinish(
            tools::Request & r )
        {
            // TODO: assert no lock held
            TOOLS_ASSERT( !!notify_ );
            tools::Completion notify;
            std::swap( notify, notify_ );
            r.start( notify );
        }
        bool
        amStarted( void ) {
            return !!notify_;
        }

        // Request
        virtual void start( void ) = 0;  // The implementation type must implement this
        void
        start(
            tools::Completion const & notify )
        {
            TOOLS_ASSERT( !notify_ );
            notify_ = notify;
            start();
        }
#ifdef TOOLS_DEBUG
    protected:
        ~StandardManualRequest() {
            TOOLS_ASSERT(!notify_);
        }
#endif // TOOLS_DEBUG
    private:
        Completion notify_;
    };

    enum RequestStep
    {
        RequestStepNext,  // As zero, this can be a placeholder for 'no action queued'
        RequestStepFinish,
        RequestStepFinishError,
        RequestStepContinue,
        RequestStepWait
    };

    namespace detail {
        struct RequestCore
        {
            RequestCore( void )
            {
            }

            ~RequestCore( void )
            {
                TOOLS_ASSERT( !completion_ );
            }

            void doStart( Completion const & complete )
            {
                TOOLS_ASSERT( !completion_ );
                TOOLS_ASSERT( !!complete );
                // TODO: assert that there are no locks held
                // Nothing else to do, as the completion is already on the stack.
            }

            // Save the real completion on the stack
            void doComplete( Completion * __restrict refComplete )
            {
                // TODO: assert that there are no locks held
                TOOLS_ASSERT( !!completion_ );  // to use this, we really should have one of these
                *refComplete = completion_;
#ifdef TOOLS_DEBUG
                // Clear the completion, ready to accep the next step.
                completion_ = Completion();
#endif // TOOLS_DEBUG
            }

            // On entry, if step != RequestStepFinish, then completion_ has been smashed with information
            // for the next step.
            //
            // The inbound Completion should point to the saved one on the stack.
            //
            // On exit (unless we are Finishing):
            //     Restore completion_ to the one that this request is to respond to.
            //     Store in refComplete the next step function, this. refErr contains the param for the next
            //         step function (e.g.: the req *).
            // If Finishing:
            //     no change
            void eval( RequestStep step, Completion * __restrict refComplete, Error ** __restrict refErr )
            {
                TOOLS_ASSERT( !*refErr );
                TOOLS_ASSERT( step != tools::RequestStepNext );
                if( step != tools::RequestStepFinish ) {
                    *refErr = static_cast< Error * >( completion_.param_ );
                    // Extract the notification function as we are not yet done
                    std::swap( refComplete->func_, completion_.func_ );
                    completion_.param_ = refComplete->param_;
                    refComplete->param_ = static_cast< void * >( this );
                } else {
                    TOOLS_ASSERTD( !completion_ );
                }
            }

            // Functions to help get stack unwinding in debug builds.
            //
            // In debug builds, tail-call optimization is generally disabled. So this would
            // consume potentially a great many more stack frames. The reactor loop is used
            // in debug builds to execute back-to-back conts without also consuming additional
            // stack frames.
            //
            // The reactor loops is only used to conts. All other 'next step' functions will
            // be executed directly.
            //
            // The implementation for this is unpleasant. The reactor needs to know if the
            // next implementation entry is the next back-to-back cont (as opposed to wait<..>
            // or finish). To record this, a pointer to the flag is stored in the completion_
            // function.
            //
            // The reactor loop calls the completion_ with the optErr pointing to the flag.
            // In order to distinguish this from an actual Error * and a flag pointer, the
            // least significant bit is set to 1 for flag pointers.
            //
            // Lastly, the completion_ function uses the existance of the flag pointer to know
            // that it's in a reactor loop. In which case it will return instead of starting
            // a new reactor loop.
            static TOOLS_FORCE_INLINE uint64 * contSeenFromError(Error * err)
            {
#ifdef TOOLS_DEBUG
                ptrdiff_t ptr = reinterpret_cast<ptrdiff_t>(err);
                if ((ptr & 0x1) != 0) {
                    return reinterpret_cast<uint64 *>(ptr - 1);
                }
#endif // TOOLS_DEBUG
                return nullptr;
            }

#ifdef TOOLS_DEBUG
            static TOOLS_FORCE_INLINE Error * contSeenToErr(uint64 * seen)
            {
                return reinterpret_cast<Error *>(reinterpret_cast<ptrdiff_t>(seen) | 0x1);
            }

            void doContReactor(Completion real, uint64 * seen)
            {
                // The reactor is on the stack if seen is !nullptr
                if (!!seen) {
                    *seen = 1;
                    return;
                }
                // Begin a new reactor loop
                uint64 localSeen;
                do {
                    localSeen = 0U;
                    Completion::FunctionT userFunc = completion_.func_;
                    TOOLS_ASSERT(!completion_.param_);
                    completion_ = real;
                    userFunc(static_cast<void *>(this), contSeenToErr(&localSeen));
                } while (localSeen != 0);
            }
#endif // TOOLS_DEBUG

            RequestStep finish( void )
            {
                TOOLS_ASSERT(!completion_);
                return tools::RequestStepFinish;
            }

            // The majority of the time, this contains who to complete to. However,
            // during start(), or a notification function, this can be used to give
            // details about the step that was called. During a call to a notification
            // function, the real value of this is saved on the stack.
            Completion completion_;
        };

        // The majority of the basic request implementation is here.
        template< typename ImplementationT >
        struct RequestBase_
            : RequestCore
        {
            typedef RequestBase_< ImplementationT > ThisRequestBaseT;
            // Callback types we support
            typedef RequestStep ( ImplementationT::* WaitFunc )( Error * );
            typedef RequestStep ( ImplementationT::* ContFunc )( void );
            typedef void (ImplementationT::* SuspendFunc)();

            // Be terse on name length for debug symbols. This is for implementation callbacks which take
            // Error *.
            template< WaitFunc waiter >
            struct cWait
            {
                TOOLS_NO_INLINE static RequestStep runWaitF( ImplementationT * __restrict this_, Error * e )
                {
                    return ( this_->* waiter )( e );
                }

                static void f( void * param_, Error * opt )
                {
                    Completion local;
                    local.param_ = param_;
                    auto seen = ThisRequestBaseT::contSeenFromError(opt);
                    Error * localOpt = (!!seen) ? nullptr : opt;
                    {
                        ThisRequestBaseT * __restrict this_ = static_cast< ThisRequestBaseT * >( local.param_ );
                        // Save things
                        this_->doComplete( &local );
                        Error * moreLocalOpt = localOpt;
                        localOpt = nullptr;
                        RequestStep step = runWaitF(static_cast<ImplementationT * __restrict>(this_), moreLocalOpt);
#ifdef TOOLS_DEBUG
                        if (step == RequestStepContinue) {
                            return this_->doContReactor(local, seen);
                        }
#endif // TOOLS_DEBUG
                        this_->eval( step, &local, &localOpt );
                    }
                    // On to the next step!
                    local.fire( localOpt );
                }

                // Revector to starting the request
                static void startF( void * param, Error * opt )
                {
                    static_cast< Request * >( static_cast< void * >( opt ))->start( Completion( &cWait< waiter >::f, param ));
                }
            };

            // Be terse on name length for debug symbols. This is for implementation callback which take no
            // parameters.
            template< ContFunc conter >
            struct cCont
            {
                TOOLS_NO_INLINE static RequestStep runContF( ImplementationT * __restrict this_ )
                {
                    return ( this_->* conter )();
                }

                static void f( void * param, Error * opt )
                {
                    Completion local;
                    local.param_ = param;
                    auto seen = ThisRequestBaseT::contSeenFromError(opt);
                    Error * localOpt = (!!seen) ? nullptr : opt;
                    if( !localOpt ) {
                        ThisRequestBaseT * __restrict this_ = static_cast< ThisRequestBaseT * >( local.param_ );
                        // Save things
                        this_->doComplete( &local );
                        RequestStep step = runContF(static_cast<ImplementationT * __restrict>(this_));
#ifdef TOOLS_DEBUG
                        if (step == RequestStepContinue) {
                            return this_->doContReactor(local, seen);
                        }
#endif // TOOLS_DEBUG
                        this_->eval( step, &local, &localOpt );
                    } else {
                        local.func_ = &cCont< conter >::fErr;
                    }
                    // On to the next step!
                    local.fire( localOpt );
                }

                TOOLS_NO_INLINE static void fErr( void * param, Error * opt )
                {
                    static_cast< ThisRequestBaseT * >( param )->proxyError( opt, tools::nameOf< cCont< conter >>() );
                }

                static void startF( void * param, Error * opt )
                {
                    static_cast< Request * >( static_cast< void * >( opt ))->start( Completion( &cCont< conter >::f, param ));
                }
            };

            // Limit code generation by putting this outside the template(s).
            void proxyError( Error * opt, StringId const & /*context*/ )
            {
                TOOLS_ASSERT( !!opt );
                // TODO: add context to the error to note the path it traveled through.
                // opt = errInfoAppend( opt, context );
                Completion local;
                // Capture the originals
                doComplete( &local );
                // Time to act
                local.fire( opt );
            }

            TOOLS_NO_INLINE Error * errInfoAppend( Error * opt, StringId const & context = tools::StringIdNull() )
            {
                // TODO: implement this
                TOOLS_ASSERT( !"Not implemented" );
                return opt;
            }

            static void waitFinishStart( void * param, Error * opt )
            {
                ThisRequestBaseT * __restrict this_ = static_cast< ThisRequestBaseT * >( param );
                Completion local = this_->completion_;
#ifdef TOOLS_DEBUG
                this_->completion_.func_ = nullptr;
#endif // TOOLS_DEBUG
                static_cast< Request * >( static_cast< void * >( opt ))->start( local );
            }

            template< SuspendFunc suspender >
            static void suspend( void * param, Error * )
            {
                ThisRequestBaseT * __restrict this_ = static_cast< ThisRequestBaseT * >(param);
                // TODO: assert that we aren't already suspended
                (static_cast<ImplementationT * __restrict>(this_)->*suspender)();
            }

            // This is for cleaning up the Error after it is reported to the user.
            static void notifyError( void * param, Error * opt )
            {
                TOOLS_ASSERT( !!opt );
                ThisRequestBaseT * __restrict this_ = static_cast< ThisRequestBaseT * >( param );
                // TODO: add additional err information to track what stages the error has passed through
                // opt = this_->errInfoAppend( opt, StringIdNull() );
                Completion local = this_->completion_;
#ifdef TOOLS_DEBUG
                this_->completion_.func_ = nullptr;
#endif // TOOLS_DEBUG
                AutoDispose<Error::Reference> localOpt(static_cast<Error::Reference *>(opt));
                local.fire( localOpt.get() );
            }
        };
    };  // detail namespace

    // A standard implementation of Request.  This offers a very small memory footprint and minimal
    // CPU overhead, while giving good stack traces and providing tools that help enforce the semantics.
    //
    // The implementation must implement 'RequestStep start( void )', which is the entry point for the request.
    // The implementation may also implement requestErrorInfo to provide extended information about the
    // request should there be an error.
    template< typename ImplementationT, typename AllocT = tools::AllocStatic< /* tools::Temporal */ >, typename InterfaceT = tools::Request >
    struct StandardRequest
        : tools::StandardDisposable< ImplementationT, InterfaceT, AllocT >
        , tools::detail::RequestBase_< ImplementationT >
    {
        typedef typename tools::detail::RequestBase_< ImplementationT >::ContFunc ContFunc;
        typedef typename tools::detail::RequestBase_< ImplementationT >::WaitFunc WaitFunc;
        typedef typename tools::detail::RequestBase_< ImplementationT >::SuspendFunc SuspendFunc;
        typedef typename tools::detail::RequestBase_< ImplementationT >::ThisRequestBaseT ThisRequestBaseT;

        template< ContFunc cc > struct ContParam {};
        template< WaitFunc cc > struct WaitParam {};
        template< SuspendFunc cc > struct SuspendParam {};

        RequestStep waitFinish( Request & req )
        {
            TOOLS_ASSERT(!ThisRequestBaseT::completion_);
            ThisRequestBaseT::completion_ = Completion( &ThisRequestBaseT::waitFinishStart, static_cast< void * >( &req ));
            return RequestStepWait;
        }

        template<typename RequestContainerT>
        RequestStep maybeWaitFinish(RequestContainerT const & req)
        {
            static_assert(std::is_base_of<tools::Request, typename std::decay<decltype(*req)>::type>::value, "RequestContainterT must dereference to a type derived from req");
            if (!!req) {
                return waitFinish(*req);
            }
            return finish();
        }

        using tools::detail::RequestCore::finish;

        RequestStep finish( Error & e )
        {
            TOOLS_ASSERT(!ThisRequestBaseT::completion_);
            ThisRequestBaseT::completion_ = Completion( &ThisRequestBaseT::notifyError, static_cast< void * >( e.ref().get() ));
            return RequestStepFinishError;
        }

        RequestStep finish( tools::AutoDispose< Error::Reference > && e )
        {
            if (!e) {
                return finish();
            }
            TOOLS_ASSERT(!ThisRequestBaseT::completion_);
            ThisRequestBaseT::completion_ = Completion( &ThisRequestBaseT::notifyError, static_cast< void * >( e.release() ));
            return RequestStepFinishError;
        }

        template< ContFunc conter >
        RequestStep cont( ContParam< conter > *** = 0 )
        {
            TOOLS_ASSERT(!ThisRequestBaseT::completion_);
            ThisRequestBaseT::completion_ = Completion( &ThisRequestBaseT::template cCont< conter >::f, nullptr );
            return RequestStepContinue;
        }

        template< WaitFunc waiter >
        RequestStep cont( WaitParam< waiter > *** = 0 )
        {
            TOOLS_ASSERT(!ThisRequestBaseT::completion_);
            ThisRequestBaseT::completion_ = Completion( &ThisRequestBaseT::template cWait< waiter >::f, nullptr );
            return RequestStepContinue;
        }

        template< ContFunc conter >
        RequestStep wait( Request & req, ContParam< conter > *** = 0 )
        {
            TOOLS_ASSERT(!ThisRequestBaseT::completion_);
            ThisRequestBaseT::completion_ = Completion( &ThisRequestBaseT::template cCont< conter >::startF, static_cast< void * >( &req ));
            return RequestStepWait;
        }

        template< WaitFunc waiter >
        RequestStep wait( Request & req, WaitParam< waiter > *** = 0 )
        {
            TOOLS_ASSERT(!ThisRequestBaseT::completion_);
            ThisRequestBaseT::completion_ = Completion( &ThisRequestBaseT::template cWait< waiter >::startF, static_cast< void * >( &req ));
            return RequestStepWait;
        }

        template< ContFunc conter, typename RequestContainerT >
        RequestStep maybeWait( RequestContainerT const & req, ContParam< conter > *** = 0 )
        {
            static_assert(std::is_base_of<tools::Request, typename std::decay<decltype(*req)>::type>::value, "RequestContainerT must dereference to a type derived from Request");
            if( !!req ) {
                return wait< conter >( *req );
            }
            return cont< conter >();
        }

        template< WaitFunc waiter, typename RequestContainerT >
        RequestStep maybeWait( RequestContainerT const & req, WaitParam< waiter > *** = 0 )
        {
            static_assert(std::is_base_of<tools::Request, typename std::decay<decltype(*req)>::type>::value, "RequestContainerT must dereference to a type derived from Request");
            if( !!req ) {
                return wait< waiter >( *req );
            }
            return cont< waiter >();
        }

        // Exit the reactor and call the suspend function. this function should arrange for the request to be
        // resumed. Be warned, it is _always_ a (bad) race condition to have an empty suspend function.
        template< SuspendFunc suspender >
        RequestStep suspend( SuspendParam< suspender > *** = 0 )
        {
            TOOLS_ASSERT(!ThisRequestBaseT::completion_);
            ThisRequestBaseT::completion_ = Completion( &ThisRequestBaseT::template suspend< suspender >, nullptr );
            return RequestStepWait;
        }

        // Resume a suspended reactor, returning into the implementation at conter/waiter. This will function as though
        // a virtual inner request had notified (with an optional Error).
        template< ContFunc conter >
        void resume( Error * e = nullptr, ContParam< conter > *** = 0 )
        {
            // TODO: assert that we are suspended
            ThisRequestBaseT::template cCont< conter >::f(static_cast<void *>(static_cast<ThisRequestBaseT *>(this)), e);
        }

        template< ContFunc conter >
        void resume( tools::AutoDispose< Error::Reference > && e, ContParam< conter > *** = 0 )
        {
            // TODO: assert that we are suspended
            ThisRequestBaseT::template cCont< conter >::f(static_cast<void *>(static_cast<ThisRequestBaseT *>(this)), e.release());
        }

        template< WaitFunc waiter >
        void resume( Error * e = nullptr, WaitParam< waiter > *** = 0 )
        {
            // TODO: assert that we are suspended
            ThisRequestBaseT::template cWait< waiter >::f(static_cast<void *>(static_cast<ThisRequestBaseT *>(this)), e);
        }

        template< WaitFunc waiter >
        void resume( tools::AutoDispose< Error::Reference > && e, WaitParam< waiter > *** = 0 )
        {
            // TODO: assert that we are suspended
            ThisRequestBaseT::template cWait< waiter >::f(static_cast<void *>(static_cast<ThisRequestBaseT *>(this)), e.release());
        }

        void resumeFinish(Error * e = nullptr)
        {
            // TODO: assert that we are suspended
            Completion local = ThisRequestBaseT::completion_;
#ifdef TOOLS_DEBUG
            ThisRequestBaseT::completion_.func_ = nullptr;
#endif // TOOLS_DEBUG
            local.fire(e);
        }

        Error * requestErrorInfo( Error * err )
        {
            // Do nothing by default
            return err;
        }

        virtual RequestStep start( void ) = 0;
    private:
        TOOLS_NO_INLINE RequestStep runRequestStart( void )
        {
            return static_cast< ImplementationT * __restrict >( this )->start();
        }

        // Request
        void start( Completion const & notify ) override
        {
            // Preserve stack variables for debugability.
            Completion local = notify;
            ThisRequestBaseT::doStart( local );
            Error * opt = nullptr;
            RequestStep step = runRequestStart();
#ifdef TOOLS_DEBUG
            if (step == RequestStepContinue) {
                return this->doContReactor(local, nullptr);
            }
#endif // TOOLS_DEBUG
            ThisRequestBaseT::eval( step, &local, &opt );
            // All set, do something
            local.fire( opt );
        }
    };

    template< typename ImplementationT, typename AllocatorT = tools::AllocStatic<>, typename InterfaceT = tools::Generator >
    struct StandardManualGenerator
        : tools::StandardManualRequest< ImplementationT, AllocatorT, InterfaceT >
    {
        virtual bool next( void )
        {
            TOOLS_ASSERT( !"StandardManualGenerator: Must define next()" );
            return false;
        }
    };

    template< typename ImplementationT, typename AllocatorT = tools::AllocStatic<>, typename InterfaceT = tools::Generator >
    struct StandardSynchronousGenerator
        : tools::StandardDisposable< ImplementationT, InterfaceT, AllocatorT >
    {
        // Generator
        void start( tools::Completion const & param )
        {
            TOOLS_ASSERT( !"StandardSynchronousGenerator: This should never go async." );
            param();
        }
        bool next( void )
        {
            generatorNext();
            return true;
        }

        // new methods
        virtual void generatorNext( void )
        {
            TOOLS_ASSERT( !"StandardSynchronousGenerator: Must define generatorNext()" );
        }
    };

    typedef RequestStep StreamStep;

    // A standard implementation of a Generator.  Memory and CPU overhead are minimal.  This implementation
    // presents a somewhat inside-out model for computation of a Generator.  The implementation must implement
    // the function 'StreamStep first( void )'.  This function is called one the first call to next(),
    // this can proceed through however many steps or function calls until it arives at a result value. At
    // that point it should return with a call to some flavor of succeed, templated on the next function
    // to call.  The behaviors of next() and start() in the Generator interface are managed entirely by
    // the implementation helper based on the implementations calls to wait<...> and succeed<...>.
    //
    // Be certain to set the bound variable(s) to a valid result before calling one of the succeed variants.
    //
    // One notable additional helper is succeedEos().  Returning the result of this method will cause the
    // implementation helper to never call into the implementation again.  It does not, however, set the
    // bound variable(s) to EOS.  It has no way of knowing what or how many of them there are.
    template< typename ImplementationT, typename AllocatorT = tools::AllocStatic<>, typename InterfaceT = tools::Generator >
    class StandardGenerator
        : public tools::StandardDisposable< ImplementationT, InterfaceT, AllocatorT >
    {
        typedef StreamStep ( ImplementationT::* ContFunc )( void );
        template< ContFunc cc > struct ContParam {};
        struct ContPair {
            ContFunc next_;
            tools::Request * wait_;
        };
    protected:
        // Configure this to return true to the user.  On the subsequent calls to next(), call the
        // provided method.
        template< ContFunc conter >
        StreamStep succeed( ContParam< conter > *** = 0 )
        {
            TOOLS_ASSERT( !completion_ );
            stepable_.next_ = conter;
            return RequestStepFinish;
        }

        // Go async on a request.  A well behaved user of a stream will call start() after getting false
        // back from next().  The provided request will be run when start() is called.  After completion,
        // a subsequent call to next(), will transfer execution to the provided method.
        //
        // At present, there is no mechanism for intercepting errors.  Any failures will be passed to the
        // user of this stream.  Behavior of this implementation helper is undefined after an error.
        template< ContFunc conter >
        StreamStep wait( Request & req, ContParam< conter > *** = 0 )
        {
            TOOLS_ASSERT( !completion_ );
            stepable_.next_ = conter;
            stepable_.wait_ = &req;
            return RequestStepWait;
        }

        template< ContFunc conter >
        StreamStep maybeWait( Request * req, ContParam< conter > *** = 0 )
        {
            TOOLS_ASSERT( !completion_ );
            stepable_.next_ = conter;
            stepable_.wait_ = req;
            return !req ? RequestStepContinue : RequestStepWait;
        }

        template< ContFunc conter >
        StreamStep cont( ContParam< conter > *** = 0 )
        {
            TOOLS_ASSERT( !completion_ );
            stepable_.next_ = conter;
            return RequestStepContinue;
        }

        // A succeed variant that will forever more return true on calls to next().  The implementation
        // needs to set the bound variable(s) to the end sentinal before calling this method.
        StreamStep succeedEos( void )
        {
            TOOLS_ASSERT( !completion_ );
            stepable_.next_ = &ImplementationT::succeedEos;
            return RequestStepFinish;
        }

        // Potentially useful on seek/rewind operations provided by the implementation.  A subsequent
        // call to next() will call the provided method.
        template< ContFunc conter >
        void reset( ContParam< conter > *** = 0 )
        {
            stepable_.next_ = conter;
            stepable_.wait_ = nullptr;
        }

        // This is called at start of next().  The implementation can override this to permit some streams
        // to detect when they are seeking.
        ContFunc streamReset( void )
        {
            return nullptr;
        }

        // The following can be useful in streams where the producer is asynchronous to the consumer.

        // Cause next() to return false.  For well behaved users, this will cause them to call start()
        // shortly after.  As such it provides a mechanism to force things to go async.
        //
        // For this to really be meaningful, the implementation should also override streamStart().
        StreamStep async( void )
        {
            TOOLS_ASSERT( !completion_ );
            return RequestStepWait;
        }

        // This will be called when start gets called, but with no pending request.  This is only useful
        // when an implementation overrides it, and some mechanism within it calls async().
        void streamStart( void )
        {
            TOOLS_ASSERT( !"startStream() is expected to be overriden by the implementation type." );
        }

        template< ContFunc conter >
        void finish( Error * err = nullptr, ContParam< conter > *** = 0 )
        {
            Completion local = completion_;
            stepable_.next_ = conter;
            stepable_.wait_ = nullptr;
            local.fire( err );
        }
    public:
        void start( Completion const & notify )
        {
            // TODO: assert that no locks are held
            Request * req = stepable_.wait_;
            stepable_.wait_ = nullptr;
            if( !req ) {
                TOOLS_ASSERT( !completion_ );
                completion_ = notify;
                static_cast< ImplementationT * >( this )->streamStart();
            } else {
                TOOLS_ASSERT( !!completion_ );
                req->start( notify );
            }
        }

        bool next( void )
        {
            ContFunc f = static_cast< ImplementationT * >( this )->streamReset();
            if( /* TOOLS_UNLIKELY */ !!f ) {
                stepable_.wait_ = nullptr;
                stepable_.next_ = f;
            } else if( /* TOOLS_UNLIKELY */ !!stepable_.wait_ ) {
                // There is still a pending request, keep returning false.
                return false;
            }
            // TOOD: assert lock level
            StreamStep s;
            do {
                TOOLS_ASSERT( !!completion_ );
#ifdef TOOLS_DEBUG
                ContFunc f = stepable_.next_;
                stepable_.next_ = nullptr;
                s = ( static_cast< ImplementationT * >( this )->*f )();
#else // TOOLS_DEBUG
                s = ( static_cast< ImplementationT * >( this )->*( stepable_.next_ ))();
#endif // TOOLS_DEBUG
            } while( s == RequestStepContinue );
            TOOLS_ASSERT( ( s == RequestStepFinish ) || ( s == RequestStepWait ));
            return ( s == RequestStepFinish );
        }

        virtual StreamStep first( void ) = 0;  // Implementation is expected to implement this

        StandardGenerator( void )
        {
            stepable_.next_ = &ImplementationT::first;
            stepable_.wait_ = nullptr;
        }
    private:
        union {
            tools::Completion completion_;
            ContPair stepable_;
        };
    };

    // CompletionFanout fires a collection of Completions when fire() is called, which is only allowed to happen
    // once per instance. New Completions are registered either with join() or, preferably, wrapRequest(). Join()
    // may fire the Completion synchronously if fire() has already been called.
    struct CompletionFanout
        : AllocProhibited
    {
        enum : uint64 {
            completed = 1,
        };
        struct Node
            : AllocStatic<>
        {
            Node(Completion const & func) : next_(nullptr), func_(func) {}

            Node * next_;
            Completion func_;
        };

        TOOLS_FORCE_INLINE CompletionFanout(void)
            : pending_(nullptr)
        {}
        TOOLS_FORCE_INLINE ~CompletionFanout(void) {
            TOOLS_ASSERT(!pending_ || hasCompleted());
        }
        TOOLS_FORCE_INLINE bool hasCompleted(void) const {
            return (reinterpret_cast<uint64>(atomicRead(&pending_)) == completed);
        }
        TOOLS_FORCE_INLINE void join(Completion const & completion) {
            auto add = new Node(completion);
            if (!atomicTryUpdate(&pending_, [add](Node ** node)->bool {
                bool ret = (*node != reinterpret_cast<Node *>(completed));
                if (ret) {
                    add->next_ = *node;
                    *node = add;
                }
                return ret;
            })) {
                delete add;
                completion(err_.get());
            }
        }
        TOOLS_FORCE_INLINE void fire(AutoDispose<Error::Reference> && err) {
            TOOLS_ASSERT(!hasCompleted());
            if (!!err) {
                err_ = err->ref();
            }
            Node * local = atomicExchange(&pending_, reinterpret_cast<Node *>(completed));
            while (!!local) {
                local->func_.fire(err_.get());
                auto next = local->next_;
                delete local;
                local = next;
            };
        }
        TOOLS_API AutoDispose<Request> maybeWrap(AutoDispose<Request> && = nullptr);

        Node * pending_;
        AutoDispose<Error::Reference> err_;
    };

    // A container that locklessly holds multiple concurrent requests. A single AtomicList is
    // used to store all the Requests that are still in-flight. One benefit of this type is that
    // it allows fire-and-forget behavior, while still giving a place to find in-flight Requests
    // in the debugger. Completed Requests are disposed synchronously in the notification. List
    // nodes are batched for disposal. This does mean that notification requires a list traversal,
    // and so may incurr performance problems for high frequency Requests.
    struct MultiRequestOwner
        : tools::Disposable
    {
        // Start the given Request synchronously, so long as stop() has not been called.
        virtual void start(tools::AutoDispose<tools::Request> &&) = 0;
        // Start the given Request if stop() has not been called, otherwise pass the Request back.
        virtual tools::AutoDispose<tools::Request> maybeStart(tools::AutoDispose<tools::Request> &&) = 0;
        // Return a Request that completes when all current requests have completed. Once this Request
        // completes, it is safe to dispose this object.
        virtual tools::AutoDispose<tools::Request> stop(void) = 0;
    };

    TOOLS_API tools::AutoDispose<MultiRequestOwner> multiRequestOwnerNew(void);

    namespace detail {
        template<typename FuncT, typename AllocT = tools::AllocDynamic<tools::Inherent>>
        struct LambdaRequest
            : tools::StandardRequest<LambdaRequest<FuncT, AllocT>, AllocT>
        {
            LambdaRequest(FuncT && func) : func_(std::move(func)) {}

            // StandardRequest
            tools::RequestStep start(void) override {
                // TODO: assert no locks held
                tools::AutoDispose<tools::Error::Reference> err;
                inner_ = func_(err);
                if (!!err) {
                    return this->finish(std::move(err));
                }
                return this->maybeWaitFinish(inner_);
            }

            FuncT func_;
            tools::AutoDispose<tools::Request> inner_;
        };
    }; // detail namespace

    // Factory a Request that executes a given lambda when the request is started. The lambda should have
    // a signature compatible with:
    //     [....](AutoDispose<Error::Reference> &)->AutoDispose<Request>
    //
    // If calling the lambda results in an Error, the returned Request will synchronously complete with that Error.
    // Otherwise, if calling the lambda results in a Request, that request will be waited on before completing. In
    // all other cases, this will complete synchronously without error.
    template<typename FuncT>
    tools::AutoDispose<tools::Request> lambdaRequestNew(FuncT && func, tools::Affinity & heap = tools::impl::affinityInstance<tools::Inherent>()) {
        return new(heap) tools::detail::LambdaRequest<FuncT>(std::move(func));
    }

    // An alternate factory for lambda Requests that don't need the full interface. The lambda should be compatible
    // with:
    //     [....]()->void
    template<typename FuncT>
    tools::AutoDispose<tools::Request> simpleLambdaRequestNew(FuncT && func, tools::Affinity & heap = tools::impl::affinityInstance<tools::Inherent>()) {
        // TODO: used generalized capture to move the func into this lambda
        return tools::lambdaRequestNew([=](tools::AutoDispose<tools::Error::Reference> &)->tools::AutoDispose<tools::Request> {
            func();
            return nullptr;
        }, heap);
    }

    TOOLS_API tools::AutoDispose<tools::Request> triggerRequestNew(tools::AutoDispose<> &);
};  // tools namespace
