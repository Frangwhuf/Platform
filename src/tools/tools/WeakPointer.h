#pragma once

#include <tools/Concurrency.h>
#include <tools/Interface.h>
#include <tools/Memory.h>

namespace tools {
    // A weak pointer waiting to be disposed.  Dispose will happen when there are no known remaining
    // references.
    struct Weakling : Disposable
    {
        Weakling * weaklingNext_;
    };

    // Cloak the current thread with a particular phantom prototype. This may defer disposal of Weaklings.
    struct PhantomPrototype
    {
        // Cloak the current thread in this prototype.
        virtual AutoDispose<> select( void ) = 0;
        // A fast version of dispose/re-cloak on the current thread. Only valid when the prototype is applied.
        virtual void touch( void ) = 0;
    };

    // Defer work until there are no threads cloaked with this instance. Any thread participating in
    // phantom weak pointers must run within a cloak in order to defer processing. Typically this is
    // used to defer deallocation of memory until we can be certain no thread could be using that memory.
    // This is a form of protection against ABA-style bugs without reference counting.
    //
    // While reference counting can be more stable than using Monitors, they can also have significantly
    // longer path lengths when traversing complex high-churn shared data structures.
    //
    // Phantoms can also be used for objects that are single-owner/multi-user where the owner may destroy
    // the entity at any time.  Much like with reference counting, but without explicit reference/dereference.
    // It is useful, with this patern, to have a 'disposed' flag in the entity to deflect new (incoming)
    // implicit references.
    //
    // Overall, if the overwhelming majority of accesses to an entity are read (rather than modify/write),
    // it is significantly faster to use an implicit system-wide lock.
    struct PhantomCloak
    {
        // Enqueue to be operated on once it can be proven no thread could observe it.
        virtual void finalize( AutoDispose< Weakling > && ) = 0;
        // Test if there is already a cloak on the current thread for this kind fo prototype.
        virtual bool isCloaked( void ) = 0;
    };

    // Fetch the phantom of a given type for this thread.
    template< typename PhantomT >
    inline PhantomCloak &
    phantomLocal( PhantomT *** phantom = 0 )
    {
        return definePhantomLocal( phantom );
    }

    // Create a prototype on the current thread for a given policy. A thread may only have one active
    // policy at a time.
    template< typename PhantomT >
    inline PhantomPrototype & phantomBindPrototype( PhantomT *** phantom = 0 )
    {
        return definePhantomPrototype( phantom );
    }
    template< typename PhantomT >
    inline AutoDispose<> phantomTryBindPrototype( PhantomT *** phantom = 0 )
    {
        return !phantomLocal( phantom ).isCloaked() ? phantomBindPrototype( phantom ).select() : nullptr;
    }

    // All (participating) threads must return to idle to prove there are no outstanding references on
    // the Weakling. The Weakling will be disposed on whatever the last (participating) thread that
    // returns to idle, regardless of the thread type it was finalized on.
    struct PhantomUniversal {};

    TOOLS_API PhantomCloak & definePhantomLocal( PhantomUniversal *** );
    TOOLS_API PhantomPrototype & definePhantomPrototype( PhantomUniversal *** );

    // For phantoms that are only on real-time threads.
    struct PhantomRealTime {};

    TOOLS_API PhantomCloak & definePhantomLocal( PhantomRealTime *** );
    TOOLS_API PhantomPrototype & definePhantomPrototype( PhantomRealTime *** );

    // Verify the current thread is cloaked.  If not it is likely any phantom protected structure is
    // unstable.
    template< typename PhantomT >
    inline bool
    phantomVerifyIsCloaked( PhantomT *** phantom = 0 )
    {
        return phantomLocal( phantom ).isCloaked();
    }

    template< typename ImplementationT, typename AllocT = tools::AllocStatic<>, typename InterfaceT = Weakling >
    struct StandardPhantom
        : InterfaceT
        , AllocT
    {
        static_assert( std::is_base_of< AllocTag, AllocT >::value, "AllocT must be an allocation policy (e.g.: AllocStatic, AllocDynamic, AllocTail, AllocPool, ..." );
        static_assert( std::is_same< Weakling, InterfaceT >::value || std::is_base_of< Weakling, InterfaceT >::value, "InterfaceT must derive from tools::Weakling" );

        // Weakling
        void
        dispose( void )
        {
            static_cast< ImplementationT * >( this )->phantomDispose();
        }

        // A default implementation.
        virtual void phantomDispose( void )
        {
            delete static_cast< ImplementationT * >( this );
        }
    };
};  // tools namespace
