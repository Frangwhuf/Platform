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

    namespace detail
    {
        struct RequestReactorCore
            : tools::Notifiable< RequestReactorCore >
            , tools::Completable< RequestReactorCore >
            , tools::Task
        {
            RequestReactorCore( void )
                : waitReq_( nullptr )
            {
            }

            // Task
            void
            execute( void )
            {
                reactorContinue();
            }

            // local methods
            void
            reactorFinishError( void )
            {
                //TOOLS_ASSERT( tools::lockLevelVerify( 0 ));
                TOOLS_ASSERT( !!finishError_ );
                tools::AutoDispose< tools::Error::Reference > err( std::move( finishError_ ));
                // err = std::move( tools::errorInfoSymbol( err, waitFrom_, "finished from" ));
                callerParam_.fire( err.get() );
            }
            void
            reactorFinish( void )
            {
                //TOOLS_ASSERT( tools::lockLevelVerify( 0 ));
                TOOLS_ASSERT( !finishError_ );
                callerParam_.fire();
            }
            void
            reactorWaitFinish( void )
            {
                TOOLS_ASSERT( !!waitReq_ );
                //TOOLS_ASSERT( tools::lockLevelVerify( 0 ));
                tools::Completion e;
                std::swap( e, callerParam_ );
                waitReq_->start( e );
            }
            void
            reactorWaitTryNotify(
                tools::Error * err )
            {
                TOOLS_ASSERT( !!nextError_ );
                TOOLS_ASSERT( !nextStep_ );
                waitReq_ = nullptr;
                if( !!err ) {
                    // Mix in some more debugging information in the error
                    //err = tools::errorInfoSymbol( err, nextError_, "continuation function" );
                }
                // Pass the error back to our user
                nextError_.fire( err );
                // evolve the state machine
                TOOLS_ASSERT( !!nextStep_ );
                nextStep_.fire();
            }
            void
            reactorTryWait( void )
            {
                TOOLS_ASSERT( !!waitReq_ );
                TOOLS_ASSERT( !!nextError_ );
                //TOOLS_ASSERT( tools::lockLevelVerify( 0 ));
                waitReq_->start( toCompletion< &RequestReactorCore::reactorWaitTryNotify >() );
            }
            void
            reactorWaitNotify(
                tools::Error * err )
            {
                TOOLS_ASSERT( !!nextThunk_ );
                TOOLS_ASSERT( !nextStep_ );
                waitReq_ = nullptr;
                if( !!err ) {
                    //finishError_ = std::move( tools::errorChainInfo( err, waitInfo_ ));
                    //finishError_ = std::move( tools::errorInfoSymbol( finishError_, nextThunk_, "continuation function" ));
                    finishError_ = std::move( err->ref() );
                    nextThunk_ = tools::Thunk();
                    nextStep_ = toThunk< &RequestReactorCore::reactorFinishError >();
                } else {
                    // Call back to our user
                    nextThunk_.fire();
                    TOOLS_ASSERT( !!nextStep_ );
                }
                // evolve the state machine
                nextStep_.fire();
            }
            void
            reactorWait( void )
            {
                TOOLS_ASSERT( !!waitReq_ );
                TOOLS_ASSERT( !!nextThunk_ );
                //TOOLS_ASSERT( tools::lockLevelVerify( 0 ));
                waitReq_->start(this->toCompletion< &RequestReactorCore::reactorWaitNotify >() );
            }
            void
            reactorContinue( void )
            {
#ifndef TOOLS_RELEASE
loop:
#endif // !TOOLS_RELEASE
                TOOLS_ASSERT( !waitReq_ );
                TOOLS_ASSERT( !!nextThunk_ || !!nextError_ ); // one must be set
                TOOLS_ASSERT( !nextThunk_ || !nextError_ ); // only one can be set
                TOOLS_ASSERT( !nextStep_ );
                //TOOLS_ASSERT( tools::lockLevelVerify( 0 ));
                // Pass control back to the user
                if( !!nextThunk_ ) {
                    nextThunk_.fire();
                } else {
                    nextError_.fire();
                }
                TOOLS_ASSERT( !!nextStep_ );
#ifndef TOOLS_RELEASE
                // Non-release builds do not seem to trigger the compiler to emit tail
                // call optimizations.  So we'll to our own version here.
                if( nextStep_ == toThunk< &RequestReactorCore::reactorContinue >() ) {
                    nextStep_ = tools::Thunk();
                    goto loop;
                }
#endif // !TOOLS_RELEASE
                // evolve the state machine
                nextStep_.fire();
            }

            tools::Completion callerParam_;  // result to the caller
            tools::Thunk nextStep_;     // for internal state machine
            tools::Completion nextError_;    // callback for next step
            tools::Thunk nextThunk_;    // callback for next step
            tools::Request * waitReq_;
            void * waitFrom_;
            //tools::ErrorInfo * waitInfo_;
            tools::AutoDispose< tools::Error::Reference > finishError_;  // completion error
        };
    };  // detail namespace

    template< typename ImplementationT, typename AffinityT = tools::Temporal, typename InterfaceT = tools::Request >
    struct StandardReactorRequest
        : tools::StandardDisposable< ImplementationT, InterfaceT, tools::AllocDynamic< AffinityT >>
        , tools::Notifiable< ImplementationT >
        , tools::Completable< ImplementationT >
    {
    private:
        typedef typename tools::Notifiable< ImplementationT >::ParamT NotifyParamT;
        typedef typename tools::Completable< ImplementationT >::ParamT ErrorNotifyParamT;
        detail::RequestReactorCore core;
    protected:
        template< void (ImplementationT::*ErrorFunctionT)( tools::Error * ) >
        void
        wait( tools::Request & req, typename ErrorNotifyParamT::template ImplementationMethod< ErrorFunctionT > *** = 0 )
        {
            tools::Completion e = tools::Completable< ImplementationT >::template toCompletion< ErrorFunctionT >();
            TOOLS_ASSERT( !core.callerParam_ );
            TOOLS_ASSERT( !core.nextThunk_ );
            TOOLS_ASSERT( !core.waitReq_ );
            TOOLS_ASSERT( !core.nextStep_ );
            core.callerParam_ = e;
            core.waitReq_ = &req;
            core.waitFrom_ = TOOLS_RETURN_ADDRESS();
            core.nextStep_ = core.toThunk< &detail::RequestReactorCore::reactorTryWait >();
        }
        template< void (ImplementationT::*ThunkFunctionT)( void ) >
        void
        wait( tools::Request & req,/* tools::ErrorInfo & info,*/ typename NotifyParamT::template ImplementationMethod< ThunkFunctionT > *** = 0 )
        {
            tools::Thunk t = tools::Notifiable< ImplementationT >::template toThunk< ThunkFunctionT >();
            TOOLS_ASSERT( !core.callerParam_ );
            TOOLS_ASSERT( !core.nextThunk_ );
            TOOLS_ASSERT( !core.waitReq_ );
            TOOLS_ASSERT( !core.nextStep_ );
            core.nextThunk_ = t;
            core.waitReq_ = &req;
            core.waitFrom_ = TOOLS_RETURN_ADDRESS();
            //core.waitInfo_ = &info;
            core.nextStep_ = core.toThunk< &detail::RequestReactorCore::reactorWait >();
        }
        template< void (ImplementationT::*ErrorFunctionT)( tools::Error * ) >
        void
        continuation( typename ErrorNotifyParamT::template ImplementationMethod< ErrorFunctionT > *** = 0 )
        {
            tools::Completion e = tools::Completable< ImplementationT >::template toCompletion< ErrorFunctionT >();
            TOOLS_ASSERT( !core.callerParam_ );
            TOOLS_ASSERT( !core.nextThunk_ );
            TOOLS_ASSERT( !core.waitReq_ );
            TOOLS_ASSERT( !core.nextStep_ );
            core.callerParam_ = e;
            core.nextStep_ = core.toThunk< &detail::RequestReactorCore::reactorContinue >();
        }
        template< void (ImplementationT::*ThunkFunctionT)( void ) >
        void
        continuation( typename NotifyParamT::template ImplementationMethod< ThunkFunctionT > *** = 0 )
        {
            tools::Thunk t = tools::Notifiable< ImplementationT >::template toThunk< ThunkFunctionT >();
            TOOLS_ASSERT( !core.callerParam_ );
            TOOLS_ASSERT( !core.nextThunk_ );
            TOOLS_ASSERT( !core.waitReq_ );
            TOOLS_ASSERT( !core.nextStep_ );
            core.callerParam_ = t;
            core.nextStep_ = core.toThunk< &detail::RequestReactorCore::reactorContinue >();
        }
        void
        finish(
            tools::Error * err )
        {
            TOOLS_ASSERT( !core.callerParam_ );
            TOOLS_ASSERT( !core.nextThunk_ );
            TOOLS_ASSERT( !core.waitReq_ );
            TOOLS_ASSERT( !core.nextStep_ );
            if( !!err ) {
                core.finishError_ = std::move( err->ref() );
                core.nextStep_ = core.toThunk< &detail::RequestReactorCore::reactorFinishError >();
            } else {
                core.nextStep_ = core.toThunk< &detail::RequestReactorCore::reactorFinish >();
            }
        }
        void
        finish(
            tools::Error & err )
        {
            finish( &err );
        }
        void
        finish( void )
        {
            finish( nullptr );
        }
        void
        waitFinish(
            tools::Request & req)
        {
            TOOLS_ASSERT( !core.callerParam_ );
            TOOLS_ASSERT( !core.nextThunk_ );
            TOOLS_ASSERT( !core.waitReq_ );
            TOOLS_ASSERT( !core.nextStep_ );
            core.waitReq_ = &req;
            core.nextStep_ = core.toThunk< &detail::RequestReactorCore::reactorWaitFinish >();
        }
    public:
        // Request
        void
        start(
            tools::Completion const & callback )
        {
            TOOLS_ASSERT( !core.callerParam_ );
            TOOLS_ASSERT( !core.nextError_ );
            TOOLS_ASSERT( !core.nextThunk_ );
            TOOLS_ASSERT( !core.nextStep_ );
            TOOLS_ASSERT( !core.waitReq_ );
            core.callerParam_ = callback;
            core.nextThunk_ = tools::Notifiable< ImplementationT >::template toThunk< &ImplementationT::start >();
            tools::ThreadScheduler::current().spawn( core );
        }
        virtual void
        start( void )
        {
            TOOLS_ASSERT( !"StandardReactorRequest - implementation must define start()" );
        }
        // TODO: maybe add some class new/delete operators for implementations with extension arrays
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
            void doComplete( Completion * __restrict refComplete, bool funcOnly )
            {
                // TODO: assert that there are no locks held
                TOOLS_ASSERT( !!completion_ );  // to use this, we really should have one of these
                if( funcOnly ) {
                    refComplete->func_ = completion_.func_;
                } else {
                    *refComplete = completion_;
                }
            }

            // On entry, if step != RequestStepFinish, then completion_ has been smashed with information
            // for the next step.
            //
            // The inbound Completion should point to the saved one on the stack.
            //
            // On exit (unless we are Finishing):
            //     Restore completion_ to the one that this request is to respond to.
            //     Store in refComplete the next step function
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

            RequestStep finish( void )
            {
#ifdef TOOLS_DEBUG
                completion_ = Completion();
#endif // TOOLS_DEBUG
                return tools::RequestStepFinish;
            }

            // During start(), or a notification function, this can be used to give
            // details about the step that was called.  Otherwise it contains who to
            // complete to.
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

            // Be terse on name length for debug symbols
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
                    Error * localOpt = opt;
                    {
                        ThisRequestBaseT * __restrict this_ = static_cast< ThisRequestBaseT * >( local.param_ );
                        // Save things
                        this_->doComplete( &local, false );
                        Error * moreLocalOpt = localOpt;
                        localOpt = nullptr;
                        this_->eval( runWaitF( static_cast< ImplementationT * __restrict >( this_ ), moreLocalOpt ), &local, &localOpt );
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
                    Error * localOpt = opt;
                    if( !localOpt ) {
                        ThisRequestBaseT * __restrict this_ = static_cast< ThisRequestBaseT * >( local.param_ );
                        // Save things
                        this_->doComplete( &local, false );
                        this_->eval( runContF( static_cast< ImplementationT * __restrict >( this_ )), &local, &localOpt );
                    } else {
                        local.func_ = &cCont< conter >::f_err;
                    }
                    // On to the next step!
                    local.fire( localOpt );
                }

                TOOLS_NO_INLINE static void f_err( void * param, Error * opt )
                {
                    static_cast< ThisRequestBaseT * >( param )->proxyError( opt, tools::nameOf< cCont< conter >>() );
                }

                static void startF( void * param, Error * opt )
                {
                    static_cast< Request * >( static_cast< void * >( opt ))->start( Completion( &cCont< conter >::f, param ));
                }
            };

            // Limit code generation by putting this outside the template(s).
            void proxyError( Error * opt, StringId const & context )
            {
                TOOLS_ASSERT( !!opt );
                // TODO: add context to the error to note the path it traveled through.
                // opt = errInfoAppend( opt, context );
                Completion local;
                // Capture the originals
                doComplete( &local, true );
#ifdef TOOLS_DEBUG
                TOOLS_ASSERT( completion_.func_ == local.func_ );
                completion_.func_ = nullptr;
#endif // TOOLS_DEBUG
                local.param_ = completion_.param_;
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
                this_->completion_.param_ = nullptr;
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
                this_->completion_.param_ = nullptr;
#endif // TOOLS_DEBUG
                AutoDispose< Error > localOpt( opt );
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
        void resume( tools::AutoDispose< Error::Reference > && e = nullptr, WaitParam< waiter > *** = 0 )
        {
            // TODO: assert that we are suspended
            ThisRequestBaseT::template cWait< waiter >::f(static_cast<void *>(static_cast<ThisRequestBaseT *>(this)), e.release());
        }

        void resumeFinish(Error * e = nullptr)
        {
            // TODO: assert that we are suspended
            Completion local = ThisRequestBaseT::completion_;
#ifdef TOOLS_DEBUG
            ThisRequestBaseT::completion_.param_ = nullptr;
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
            ThisRequestBaseT::eval( runRequestStart(), &local, &opt );
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
};  // tools namespace
