#include "toolsprecompiled.h"

#include <tools/AtomicCollections.h>
#include <tools/Concurrency.h>
#include <tools/Threading.h>
#include <tools/WeakPointer.h>

using namespace tools;

////////
// Types
////////

namespace {
    struct PhantomSequence
        : AllocPool<PhantomSequence>
    {
        PhantomSequence( void );
        ~PhantomSequence( void );

        void deref( uint32 );
        void deref( uint32, PhantomSequence * volatile * );

        uint32 sequence_;
        unsigned volatile refs_;
        Weakling * volatile first_;  // Weakling to be operated on
        PhantomSequence * next_;  // Next phantom in sequence
    };

    struct PhantomSequenceRef
    {
        PhantomSequence * current_;  // List head (nullptr at refs == 0)
        uint32 next_;  // Next sequence
        uint16 refs_;  // Count of live threads
        bool live_;  // Accept inserts only if live and no derefs.
    };

    // Global tracking for phantom references. On reference, set a terminator sequence (generation). On
    // entry we increment the reference count.
    struct PhantomSequenceRoot
    {
        AtomicAny< PhantomSequenceRef, true > root_;
        PhantomSequence * volatile marshalled_;  // posted from some other class
    };

    struct PhantomSequenceLocal
    {
        PhantomSequenceLocal( PhantomSequenceRoot & );
        ~PhantomSequenceLocal( void );

        void derefLocalMain( uint32, PhantomSequence * );
        void derefLocal( uint32, PhantomSequence * );
        void derefMarshal( uint32, PhantomSequence * );
        void enter( void );
        void exit( bool );
        void touch( bool );
        void post( AutoDispose< Weakling > && );

        PhantomSequenceRoot * root_;
        uint32 cloakSeq_;
        unsigned entries_;
        // Thread-local allocation stash. When we allocate, but lose the race, we use this to avoid going
        // through the loop again.
        PhantomSequence * stash_;
    };

    struct PhantomUniversalBase
        : PhantomCloak
        , Disposable
        , PhantomPrototype
    {
        // PhantomCloak
        void finalize( AutoDispose< Weakling > && );
        bool isCloaked( void );

        // Disposable
        void dispose( void );

        // PhantomPrototype
        AutoDispose<> select( void );
        void touch( void );
    };

    struct PhantomRealTimeBase
        : PhantomCloak
        , Disposable
        , PhantomPrototype
    {
        // PhantomCloak
        void finalize( AutoDispose< Weakling > && );
        bool isCloaked( void );

        // Disposable
        void dispose( void );

        // PhantomPrototype
        AutoDispose<> select( void );
        void touch( void );
    };

    struct PhantomCloakLocal
        : PhantomUniversalBase
        , PhantomRealTimeBase
    {
        PhantomCloakLocal( void );

        // PhantomUniversalBase
        void universalFinalize( AutoDispose< Weakling > && );
        bool universalIsCloaked( void );
        void universalDispose( void );
        AutoDispose<> universalSelect( void );
        void universalTouch( void );

        // PhantomUniversalBase
        void realTimeFinalize( AutoDispose< Weakling > && );
        bool realTimeIsCloaked( void );
        void realTimeDispose( void );
        AutoDispose<> realTimeSelect( void );
        void realTimeTouch( void );

        PhantomSequenceLocal seqsUniversal_;
        PhantomSequenceLocal seqsRealTime_;
    };
};  // anonymous namespace

//////////
// Statics
//////////

static PhantomSequenceRoot universalRoot_;
static PhantomSequenceRoot realTimeRoot_;
static StandardThreadLocalHandle< PhantomCloakLocal > phantomCloakLocal_;

///////////////////////
// Non-member Functions
///////////////////////

PhantomCloak &
tools::definePhantomLocal( PhantomUniversal *** )
{
    return static_cast< PhantomUniversalBase & >( *phantomCloakLocal_ );
}

PhantomPrototype &
tools::definePhantomPrototype( PhantomUniversal *** )
{
    return static_cast< PhantomUniversalBase & >( *phantomCloakLocal_ );
}

PhantomCloak &
tools::definePhantomLocal( PhantomRealTime *** )
{
    return static_cast< PhantomRealTimeBase & >( *phantomCloakLocal_ );
}

PhantomPrototype &
tools::definePhantomPrototype( PhantomRealTime *** )
{
    return static_cast< PhantomRealTimeBase & >( *phantomCloakLocal_ );
}

//////////////////
// PhantomSequence
//////////////////

PhantomSequence::PhantomSequence( void )
    : first_( nullptr )
{
}

PhantomSequence::~PhantomSequence( void )
{
    Weakling * next = first_;
    while( Weakling * weak = next ) {
        next = weak->weaklingNext_;
        AutoDispose<> disp( weak );
    }
}

void
PhantomSequence::deref(
    uint32 final )
{
    PhantomSequence * next = this;
    while( PhantomSequence * seq = next ) {
        TOOLS_ASSERT( atomicRead( &seq->refs_ ) > 0U );
        next = ( seq->sequence_ != final ) ? seq->next_ : nullptr;
        if( !atomicDeref( &seq->refs_ )) {
            delete seq;
        }
    }
}

void
PhantomSequence::deref(
    uint32 final,
    PhantomSequence * volatile * requeue )
{
    PhantomSequence * next = this;
    while( PhantomSequence * seq = next ) {
        TOOLS_ASSERT( atomicRead( &seq->refs_ ) > 0U );
        next = ( seq->sequence_ != final ) ? seq->next_ : nullptr;
        if( !atomicDeref( &seq->refs_ )) {
            atomicPush( requeue, seq, &PhantomSequence::next_ );
        }
    }
}

///////////////////////
// PhantomSequenceLocal
///////////////////////

PhantomSequenceLocal::PhantomSequenceLocal(
    PhantomSequenceRoot & root )
    : root_( &root )
    , entries_( 0U )
    , stash_( nullptr )
{
}

PhantomSequenceLocal::~PhantomSequenceLocal( void )
{
    TOOLS_ASSERT( entries_ == 0U );
    if( !!stash_ ) {
        delete stash_;
    }
}

void
PhantomSequenceLocal::derefLocalMain(
    uint32 uncloakSeq,
    PhantomSequence * start )
{
    start->deref( uncloakSeq );
}

void
PhantomSequenceLocal::derefLocal(
    uint32 uncloakSeq,
    PhantomSequence * start )
{
    if( !!start ) {
        derefLocalMain( uncloakSeq, start );
    }
    if( !!root_->marshalled_ ) {
        PhantomSequence * next = atomicExchange( &root_->marshalled_, static_cast< PhantomSequence * >( nullptr ));
        while( PhantomSequence * demarshal = next ) {
            next = demarshal->next_;
            TOOLS_ASSERT( atomicRead( &demarshal->refs_ ) == 0U );
            delete demarshal;
        }
    }
}

void
PhantomSequenceLocal::derefMarshal(
    uint32 uncloakSeq,
    PhantomSequence * start )
{
    if( !start ) {
        return;
    }
    start->deref( uncloakSeq, &root_->marshalled_ );
}

void
PhantomSequenceLocal::enter( void )
{
    ++entries_;
    atomicTryUpdate( &root_->root_, [=]( PhantomSequenceRef * ref )->bool {
        cloakSeq_ = ref->next_;
        ++ref->refs_;
        return true;
    });
}

void
PhantomSequenceLocal::exit(
    bool marshal )
{
    PhantomSequence * start;
    atomicTryUpdate( &root_->root_, [=, &start]( PhantomSequenceRef * ref )->bool {
        start = ( ref->next_ != cloakSeq_ ) ? ref->current_ : nullptr;
        ref->live_ = false;
        TOOLS_ASSERT( ref->refs_ > 0U );
        --ref->refs_;
        return true;
    });
    uint32 uncloakSeq = cloakSeq_;
    if( marshal ) {
        derefMarshal( uncloakSeq, start );
    } else {
        derefLocal( uncloakSeq, start );
    }
    TOOLS_ASSERT( entries_ > 0U );
    --entries_;
}

void
PhantomSequenceLocal::touch(
    bool marshal )
{
    TOOLS_ASSERT( entries_ > 0U );
    PhantomSequence * start;
    uint32 recloakSeq;
    atomicTryUpdate( &root_->root_, [=, &start, &recloakSeq]( PhantomSequenceRef * ref )->bool {
        recloakSeq = ref->next_;
        TOOLS_ASSERT( ref->refs_ > 0U );
        if( recloakSeq == cloakSeq_ ) {
            start = nullptr;
            if( !ref->live_ ) {
                // no change
                return false;
            }
        } else {
            // If we read a real value we must modify the AtomicAny to guarantee it wasn't torn during
            // transfer.
            start = ref->current_;
        }
        ref->live_ = false;
        return true;
    });
    uint32 uncloakSeq = cloakSeq_;
    cloakSeq_ = recloakSeq;  // reset to cover re-entrant call
    if( marshal ) {
        derefMarshal( uncloakSeq, start );
    } else {
        derefLocal( uncloakSeq, start );
    }
}

void
PhantomSequenceLocal::post(
    AutoDispose< Weakling > && weakling )
{
    if( !weakling ) {
        return;
    }
    TOOLS_ASSERT( entries_ > 0U );
    PhantomSequence * seqPost;
    atomicTryUpdate( &root_->root_, [=, &seqPost]( PhantomSequenceRef * ref )->bool {
        if( ref->live_ && ( ref->next_ != cloakSeq_ )) {
            seqPost = ref->current_;
            // Because we are reading a > 64-bit AtomicAny<...>, to guarentee no tearing, we must do the update.
            return true;
        }
        // New element
        if( !stash_ ) {
            stash_ = new PhantomSequence();
        }
        seqPost = stash_;
        seqPost->next_ = ref->current_;
        seqPost->refs_ = ref->refs_;
        seqPost->sequence_ = ref->next_;
        ref->live_ = true;
        ++ref->next_;  // overflow doesn't matter for this
        ref->current_ = seqPost;
        return true;
    });
    if( seqPost == stash_ ) {
        // Just used our stash
        stash_ = nullptr;
    }
    atomicPush( &seqPost->first_, weakling.release(), &Weakling::weaklingNext_ );
}

///////////////////////
// PhantomUniversalBase
///////////////////////

void
PhantomUniversalBase::finalize(
    AutoDispose< Weakling > && weakling )
{
    static_cast< PhantomCloakLocal * >( this )->universalFinalize( std::move( weakling ));
}

bool
PhantomUniversalBase::isCloaked( void )
{
    return static_cast< PhantomCloakLocal * >( this )->universalIsCloaked();
}

void
PhantomUniversalBase::dispose( void )
{
    static_cast< PhantomCloakLocal * >( this )->universalDispose();
}

AutoDispose<>
PhantomUniversalBase::select( void )
{
    return static_cast< PhantomCloakLocal * >( this )->universalSelect();
}

void
PhantomUniversalBase::touch( void )
{
    static_cast< PhantomCloakLocal * >( this )->universalTouch();
}

//////////////////////
// PhantomRealTimeBase
//////////////////////

void
PhantomRealTimeBase::finalize(
    AutoDispose< Weakling > && weakling )
{
    static_cast< PhantomCloakLocal * >( this )->realTimeFinalize( std::move( weakling ));
}

bool
PhantomRealTimeBase::isCloaked( void )
{
    return static_cast< PhantomCloakLocal * >( this )->realTimeIsCloaked();
}

void
PhantomRealTimeBase::dispose( void )
{
    static_cast< PhantomCloakLocal * >( this )->realTimeDispose();
}

AutoDispose<>
PhantomRealTimeBase::select( void )
{
    return static_cast< PhantomCloakLocal * >( this )->realTimeSelect();
}

void
PhantomRealTimeBase::touch( void )
{
    static_cast< PhantomCloakLocal * >( this )->realTimeTouch();
}

////////////////////
// PhantomCloakLocal
////////////////////

PhantomCloakLocal::PhantomCloakLocal( void )
    : seqsUniversal_( universalRoot_ )
    , seqsRealTime_( realTimeRoot_ )
{
}

void
PhantomCloakLocal::universalFinalize(
    AutoDispose< Weakling > && weakling )
{
    seqsUniversal_.post( std::move( weakling ));
}

bool
PhantomCloakLocal::universalIsCloaked( void )
{
    return ( seqsUniversal_.entries_ > 0U );
}

void
PhantomCloakLocal::universalDispose( void )
{
    seqsUniversal_.exit( false );
}

AutoDispose<>
PhantomCloakLocal::universalSelect( void )
{
    TOOLS_ASSERT( seqsUniversal_.entries_ == 0U );
    TOOLS_ASSERT( seqsRealTime_.entries_ == 0U );
    seqsUniversal_.enter();
    return static_cast< PhantomUniversalBase * >( this );
}

void
PhantomCloakLocal::universalTouch( void )
{
    seqsUniversal_.touch( false );
}

void
PhantomCloakLocal::realTimeFinalize(
    AutoDispose< Weakling > && weakling )
{
    seqsRealTime_.post( std::move( weakling ));
}

bool
PhantomCloakLocal::realTimeIsCloaked( void )
{
    return ( seqsRealTime_.entries_ > 0U );
}

void
PhantomCloakLocal::realTimeDispose( void )
{
    seqsUniversal_.exit( true );
    seqsRealTime_.exit( false );
}

AutoDispose<>
PhantomCloakLocal::realTimeSelect( void )
{
    TOOLS_ASSERT( seqsUniversal_.entries_ == 0U );
    TOOLS_ASSERT( seqsRealTime_.entries_ == 0U );
    seqsUniversal_.enter();
    seqsRealTime_.enter();
    return static_cast< PhantomRealTimeBase * >( this );
}

void
PhantomCloakLocal::realTimeTouch( void )
{
    seqsUniversal_.touch( true );
    seqsRealTime_.touch( false );
}

#include <tools/UnitTest.h>
#if TOOLS_UNIT_TEST

namespace {
    struct TestWeakling
        : Weakling
        , AllocStatic<>
    {
        TestWeakling(bool * d) : disposed_(d) {}

        // Weakling
        void dispose(void) override {
            PhantomCloak & p(phantomLocal<PhantomUniversal>());
            TOOLS_ASSERTR(p.isCloaked());
            TOOLS_ASSERTR(!*disposed_);
            *disposed_ = true;
        }

        bool * disposed_;
    };

    struct TestPhantomElement
        : StandardPhantomSlistElement< TestPhantomElement, StandardPhantom< TestPhantomElement, AllocPool< TestPhantomElement >>>
    {
        TestPhantomElement( uint64 i ) : id_( i ) {}

        uint64 id_;
    };

    inline uint64 const &
    keyOf( TestPhantomElement const & v )
    {
        return v.id_;
    }
};  // anonymous namespace

TOOLS_TEST_CASE("Weakling", [](Test & test)
{
    PhantomCloak & phantom(phantomLocal<PhantomUniversal>());
    bool disposed = false;
    AutoDispose<> & testCloak = test.cloak();
    TestWeakling weakling(&disposed);
    phantom.finalize(&weakling);
    TOOLS_ASSERTR(!disposed);
    testCloak = nullptr;
    TOOLS_ASSERTR(disposed);
    // Just in case the test environment needs a cloak for shutdown, let's give it a new one.
    testCloak = phantomBindPrototype<PhantomUniversal>().select();
});

TOOLS_TEST_CASE("PhantomHashMap.update.inesrt", [](Test &)
{
    PhantomHashMap< TestPhantomElement, uint64, PhantomUniversal, 1 > phMap;
    // Verify insertion doesn't remove items
    size_t i = 0;
    while( i < 10 ) {
        uint64 key = ( 2 * i ) + 1;
        phMap.find( key, [&]( TestPhantomElement const * ref )->void {
            TOOLS_ASSERTR( ref == nullptr );
        });
        phMap.update( key, [&]( TestPhantomElement *& ref )->void {
            TOOLS_ASSERTR( ref == nullptr );
            ref = new TestPhantomElement( key );
        });
        ++i;
    }
    i = 0;
    while( i < 10 ) {
        uint64 key = 2 * i;
        phMap.find( key, [&]( TestPhantomElement const * ref )->void {
            TOOLS_ASSERTR( ref == nullptr );
        });
        phMap.update( key, [&]( TestPhantomElement *& ref )->void {
            TOOLS_ASSERTR( ref == nullptr );
            ref = new TestPhantomElement( key );
        });
        ++i;
    }
    // Validate the state of the map
    uint32 masks = 0U;
    uint32 maskAll = 0xFFFFFU;
    phMap.forEach( [&]( TestPhantomElement const & elem )->bool {
        uint32 mask = static_cast< uint32 >( 1U ) << elem.id_;
        TOOLS_ASSERTR( ( mask & masks ) == 0 );
        masks |= mask;
        return true;
    });
    TOOLS_ASSERTR( maskAll == masks );
    phMap.clear();
});

TOOLS_TEST_CASE("PhantomHashMap.update.insert.duplicate", [](Test &)
{
    PhantomHashMap< TestPhantomElement, uint64, PhantomUniversal, 1> phMap;
    // Verify insertion doesn't remove items
    size_t i = 0;
    while( i < 12 ) {
        uint64 key = i;
        phMap.find( key, [&]( TestPhantomElement const * ref )->void {
            TOOLS_ASSERTR( ref == nullptr );
        });
        phMap.update( key, [&]( TestPhantomElement *& ref )->void {
            TOOLS_ASSERTR( ref == nullptr );
            ref = new TestPhantomElement( key );
        });
        ++i;
    }
    // Replace all elements with new ones
    i = 0;
    while( i < 12 ) {
        uint64 key = i;
        phMap.find( key, [&]( TestPhantomElement const * ref )->void {
            TOOLS_ASSERTR( ref != nullptr );
        });
        phMap.update( key, [&]( TestPhantomElement *& ref )->void {
            TOOLS_ASSERTR( ref != nullptr );
            ref = new TestPhantomElement( key );
        });
        ++i;
    }
    // Validate the state of the map
    uint32 masks = 0U;
    uint32 maskAll = 0xFFFU;
    phMap.forEach( [&]( TestPhantomElement const & elem )->bool {
        uint32 mask = static_cast< uint32 >( 1U ) << elem.id_;
        TOOLS_ASSERTR( ( mask & masks ) == 0 );
        masks |= mask;
        return true;
    });
    TOOLS_ASSERTR( maskAll == masks );
    phMap.clear();
});

TOOLS_TEST_CASE("PhantomHashMap.update.remove", [](Test &)
{
    PhantomHashMap< TestPhantomElement, uint64, PhantomUniversal, 1> phMap;
    size_t i = 0;
    while( i < 12 ) {
        uint64 key = i;
        phMap.find( key, [&]( TestPhantomElement const * ref )->void {
            TOOLS_ASSERTR( ref == nullptr );
        });
        phMap.update( key, [&]( TestPhantomElement *& ref )->void {
            TOOLS_ASSERTR( ref == nullptr );
            ref = new TestPhantomElement( key );
        });
        ++i;
    }
    // Make 2 passes, should observe no difference
    size_t j = 2;
    while( j-- > 0 ) {
        i = 0;
        while( i < 6 ) {
            uint64 key = i * 2;
            phMap.update( key, [&]( TestPhantomElement *& ref )->void {
                ref = nullptr;
            });
            ++i;
        }
        uint32 masks = 0U;
        uint32 maskAll = 0xAAAU;
        phMap.forEach( [&]( TestPhantomElement const & elem )->bool {
            uint32 mask = static_cast< uint32 >( 1U ) << elem.id_;
            TOOLS_ASSERTR( ( mask & masks ) == 0 );
            masks |= mask;
            return true;
        });
        TOOLS_ASSERTR( maskAll == masks );
    }
    phMap.clear();
});

#endif /* TOOLS_UNIT_TEST */
