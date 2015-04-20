#include "toolsprecompiled.h"

#include <tools/Async.h>
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
};  // anonymous namespace

static StandardThreadLocalHandle< SyncLocal > syncLocal_;

///////////////////////
// Non-member functions
///////////////////////

AutoDispose< Error::Reference >
tools::runRequestSynchronously( AutoDispose< Request > const & req )
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
