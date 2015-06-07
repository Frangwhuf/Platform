#pragma once

#include <tools/Memory.h>
#include <tools/Tools.h>
#include <tools/WeakPointer.h>

namespace tools
{
    // A lock-free non-blocking singly linked list. The elements are intrusive, with a required member for a
    // link to the next element as well as an unlink method.
    //
    // Implementation overview:
    //
    // The list maintains 'roots_' lists, which are linked by roleNext_. To handle element delete, there are two
    // internal lists (linked by roleUnlinked_):
    //    * extracted_[refs_.parity_]: recently removed elements (the 'parity slot')
    //    * extracted_[!refs_.parity_]: elements removed from the 'parity slot' (the 'linger slot')
    //
    // Element lifecycle:
    //    * element inserted (e.g.: by push(...))
    //    * element marked for delete (by setEnd(...))
    //    * element moved to 'parity slot' (by extract(...))
    //    * optionally: element moved to 'linger slot' (by extract(...), by changing parity_)
    //    * element actually deleted by call to extractFinalDispose(...)
    //
    // readRefs_ protects the 'parity slot', while readLingerRefs_ protects the 'linger slot'. It is safe to
    // dispose elements in the 'parity slot' when readRefs_ becomes zero. Likewise it is safe to dispose
    // elements in the 'linger slot' when readLingerRefs_ becomes zero.
    //
    // A reader holds a readRef_ while in progress. Given that there may always be an active reader, it may
    // be impossible to free elements in the 'parity slot'. In order to make forward progress, when the
    // 'linger slot' is empty, the extraction process may flip parity_ to move all 'parity slot' elements to
    // the 'linger slot'.
    //
    // When the extraction process changes parity_, active readers might alo access elements in the 'linger
    // slot', thus all reader references are copied to linger references. Once a reader becomes inactive, it
    // is able to determine if it should release a linger reference based on if parity_ has changed. It is
    // worth noting that parity_ cannot change more than once over the lifetime of a single reader. This can
    // be enforced by not allowing parity_ to change while any linger references are held.
    //
    // Given linger references are released so long as parity_ does not change, eventually all active readers
    // will complete and the extraction process will be able to make forward progress freeing elements in the
    // 'linger slot'.

    // Base type for AtomicList lists. This is generally simplier than rolling your own.
    template<typename AnyT, typename RoleT>
    struct AtomicListLink
    {
        AnyT * nextLink_;
    };

    struct LinkRoleNext {};
    struct LinkRoleUnlinked {};

    // List elements are disposed in the future.
    template<typename AnyT>
    struct AtomicListDispose
    {
        void extractFinalDispose(void) {
            delete static_cast<AnyT *>(this);
        }
    };

    // Base type for AtomicList
    template<typename AnyT>
    struct AtomicListBase
        : AtomicListLink<AnyT, LinkRoleNext>
        , AtomicListLink<AnyT, LinkRoleUnlinked>
        , AtomicListDispose<AnyT>
    {};

    template<typename RoleT, typename AnyT>
    TOOLS_FORCE_INLINE AnyT ** atomicLinkOf(AnyT & a, RoleT *** = nullptr)
    {
        return &static_cast<AtomicListLink<AnyT, RoleT> *>(&a)->nextLink_;
    }

    namespace detail {
        struct AtomicListRefs
        {
            enum ClaimState : uint16 {
                UNCLAIMED,       // No extract in progress
                CLAIMED,         // Extrack in progress
                CLAIMEDMOREWORK, // Someone tried to claim the extration while it was already claimed
            };

            uint16 readRefs_; // Number of readers in the current epoch. Note: there can only be one ref/thread, so this should have enough room.
            uint16 readLingerRefs_; // Number of readers lingering in the alternate list. Refs cannot be added here, so this will eventually reach 0. At which point extracted elements can be disposed.
            bool parity_; // The epoch marches forward one bit at a time, there is on ly one extraction active at a tiem.
            ClaimState extractClaimed_; // If some user has claimed being the extractor, when it is done the parity epoch will advance.
        };

        // Base class that contains the type independant implementation, which is mostly state manipulation.
        struct AtomicListStates
        {
            ~AtomicListStates(void) {
                if (Build::isDebug_) {
                    AtomicListRefs local(atomicRead(&refs_));
                    TOOLS_ASSERT(local.readRefs_ == 0U);
                    TOOLS_ASSERT(local.readLingerRefs_ == 0U);
                    TOOLS_ASSERT(local.extractClaimed_ == AtomicListRefs::UNCLAIMED);
                }
            }
            // Enter as a reader and return the parity
            bool refReader(void) {
                bool parity = false;
                atomicUpdate(&refs_, [&parity](AtomicListRefs old)->AtomicListRefs {
                    parity = old.parity_;
                    ++old.readRefs_;
                    return old;
                });
                return parity;
            }
            // Returns true if the list is now dirty (parity from refReader must be resubmitted so that we can tell
            // if we've witnessed a rollover, when markDirty is true the list will be unconditionally dirty).
            bool derefReader(bool readerParity) {
                bool dirty = false;
                atomicUpdate(&refs_, [=, &dirty](AtomicListRefs old)->AtomicListRefs {
                    dirty = false;
                    // The most basic form of deref
                    TOOLS_ASSERT(old.readRefs_ > 0U);
                    --old.readRefs_;
                    if (readerParity != old.parity_) {
                        // We've aquired a lingering reference, which we must deref.
                        TOOLS_ASSERT(old.readLingerRefs_ > 0U);
                        --old.readLingerRefs_;
                        if (old.readLingerRefs_ == 0U) {
                            // We're dirty because we should clean the 'linger slot'.
                            dirty = true;
                        }
                    }
                    return old;
                });
                return dirty;
            }
            // Claim things for extraction, returning true if successful. Passing true for rescan indicates that
            // an existing extractor will make another pass at extraction before unclaiming. This will do the work
            // that this claimant was attempting.
            //
            // Note: extracting is only useful if the list is dirty. That is, has items to be removed or items in
            // the 'parity slot'/'linger slot'.
            //
            // Values returned via arguments are only valid when true is returned.
            bool claimExtract(bool rescan, bool & refClaimParity, bool & refCanDispose, bool & refCanDisposeLinger) {
                bool claim = false;
                atomicTryUpdate(&refs_, [&](AtomicListRefs & refOld)->bool {
                    typedef tools::detail::AtomicListRefs::ClaimState ClaimState;
                    if (refOld.extractClaimed_ != ClaimState::UNCLAIMED) {
                        claim = false;
                        if (rescan && (refOld.extractClaimed_ == ClaimState::CLAIMED)) {
                            refOld.extractClaimed_ = ClaimState::CLAIMEDMOREWORK;
                            return true;
                        } else {
                            return false;
                        }
                    }
                    claim = true;
                    refClaimParity = refOld.parity_;
                    refCanDispose = (refOld.readRefs_ == 0U);
                    refCanDisposeLinger = (refOld.readLingerRefs_ == 0U);
                    refOld.extractClaimed_ = ClaimState::CLAIMED;
                    return true;
                });
                return claim;
            }
            // Attempt to 'unclaim'. Return false if there is more work that could be done. In which case the
            // caller should loop. Otherwise, release the claim, return true, and report if we are still dirty.
            //
            // Note: the list is (still) dirty if there are elements to be removed, elements in the 'parity
            // slot', or elements in the 'linger slot'. All relevant elements should be able to be removed,
            // however we may not be able to free the items in the 'parity slot' or 'linger slot' if the
            // matching reference counts are > 0. This is why we can successfully unclaim but still be dirty.
            bool unclaim(bool hasExtracted, bool hasExtractedLinger, bool & refCanDispose, bool & refCanDisposeLinger, bool & refStillDirty) {
                bool unclaimed = false;
                atomicTryUpdate(&refs_, [&](AtomicListRefs & refOld)->bool {
                    typedef tools::detail::AtomicListRefs::ClaimState ClaimState;
                    TOOLS_ASSERT(refOld.extractClaimed_ != ClaimState::UNCLAIMED);
                    unclaimed = false;
                    refCanDispose = (hasExtracted && (refOld.readRefs_ == 0U));
                    refCanDisposeLinger = (hasExtractedLinger && (refOld.readLingerRefs_ == 0U));
                    if (refOld.extractClaimed_ == ClaimState::CLAIMEDMOREWORK) {
                        // Loop again, extract more
                        refOld.extractClaimed_ = ClaimState::CLAIMED;
                        return true;
                    }
                    if (refCanDispose || refCanDisposeLinger) {
                        // Always prefer to dispose when that is possible
                        return false;
                    }
                    unclaimed = true;
                    // Looks like we might be able to unclaim!
                    refOld.extractClaimed_ = ClaimState::UNCLAIMED;
                    if (hasExtracted && (refOld.readRefs_ > 0U) && (refOld.readLingerRefs_ == 0U)) {
                        TOOLS_ASSERT(!hasExtractedLinger);
                        // There exist concurrent readers preventing us from disposing the extracted elements,
                        // however the 'linger slot' is available. So copy those read references of to the
                        // 'linger slot'.
                        refOld.parity_ = !refOld.parity_;
                        refOld.readLingerRefs_ = refOld.readRefs_;
                        refStillDirty = true;
                    } else {
                        // We're still dirty if another slot is not empty. Scan again at another time.
                        refStillDirty = (hasExtracted || hasExtractedLinger);
                    }
                    return true;
                });
                return unclaimed;
            }

            AtomicAny<detail::AtomicListRefs> refs_;
        };
    }; // detail namespace

    template<typename AnyT, size_t rootsUsed = 1U>
    struct AtomicList
    {
        TOOLS_FORCE_INLINE AtomicList(void) {
            memset(const_cast<void *>(static_cast<void volatile *>(roots_)), 0, sizeof(AnyT *) * rootsUsed);
            memset(const_cast<void *>(static_cast<void volatile *>(extracted_)), 0, sizeof(AnyT *) * 2);
        }
        TOOLS_FORCE_INLINE ~AtomicList(void) {
            if (Build::isDebug_) {
                for (auto i = 0U; i != rootsUsed; ++i) {
                    TOOLS_ASSERT(!roots_[i]);
                }
            }
            while (AnyT * i = extracted_[0U]) {
                extracted_[0U] = *atomicLinkOf<LinkRoleUnlinked>(*i);
                i->extractFinalDispose();
            }
            while (AnyT * i = extracted_[1U]) {
                extracted_[1U] = *atomicLinkOf<LinkRoleUnlinked>(*i);
                i->extractFinalDispose();
            }
        }
        // Visit each element in the list (a snapshot of its state). If this return true, the list is dirty.
        template<typename FuncT>
        TOOLS_FORCE_INLINE bool forEach(size_t root, FuncT && func) {
            TOOLS_ASSERT(root < rootsUsed);
            if (!roots_[root]) {
                // The list is empty, do nothing
                return false;
            }
            tools::impl::prefetch(roots_[root]);
            // Reserve the read reference
            bool parity = states_.refReader();
            // Do the internal iteration
            AnyT * next;
            for (AnyT * i = roots_[root]; !!i; i = next) {
                next = *atomicLinkOf<LinkRoleNext>(*i);
                tools::impl::prefetch(next);
                if (!func(*i)) {
                    // early exit
                    break;
                }
            }
            return states_.derefReader(parity);
        }
        template<typename FuncT>
        TOOLS_FORCE_INLINE bool forEach(FuncT && func) {
            TOOLS_ASSERT(rootUsed == 1U);
            return forEach(0U, func);
        }
        // Append a new entry to the list
        TOOLS_FORCE_INLINE void push(size_t root, AnyT * a) {
            atomicUpdate(&roots_[root], [=](AnyT * old)->AnyT * {
                *atomicLinkOf<LinkRoleNext>(*a) = old;
                return a;
            });
        }
        TOOLS_FORCE_INLINE void push(AnyT * a) {
            TOOLS_ASSERT(rootsUsed == 1U);
            push(0U, a);
        }
        // Push a new element that depends on all other elements in the list. Since there may exist a race to insert
        // new elements, this will continue to rescan the list until the root is stable. The caller provides a functor
        // that takes a referent to the current root as input. The caller may reset the root pointer and return a value
        // indicating that they would like the value to be linked in. The signature for this functor is:
        //
        //   [...](AnyT ** ref)->bool
        //
        // Returning true signals that we should try to re-assign the root. Otherwise no assignment happens.
        template<typename FuncT>
        TOOLS_FORCE_INLINE bool pushForEach(size_t root, FuncT && func) {
            // Reserve the read reference
            bool parity = states_.refReader();
            // The tail root is the extent we've observed.
            AnyT * tail = nullptr;
            atomicTryUpdate(&roots_[root], [&](AnyT *& old)->bool {
                AnyT * newRoot = old;
                tail = old;
                bool ret = func(&newRoot);
                if (ret) {
                    TOOLS_ASSERT(!!newRoot);
                    if (newRoot != root) {
                        *atomicLinkOf<LinkRoleNext>(*newRoot) = root;
                        root = newRoot;
                    }
                }
                return ret;
            });
            return states_.derefReader(parity);
        }
        // Internal method
        TOOLS_FORCE_INLINE void disposeParity(bool parity) {
            if (!extracted_[parity]) {
                return; // Nothing to do
            }
            // We are the final iterator over the elements in this epoch. Extract them and allow the parity to advance.
            AnyT * extracted = atomicExchange(&extracted_[parity], nullptr);
            // Dispose everything we can.
            while (AnyT * i = extracted) {
                extracted = *atomicLinkOf<LinkRoleUnlinked>(*i);
                i->extractFinalDispose();
            }
        }
        TOOLS_FORCE_INLINE void pushExtracted(bool parity, AnyT * a) {
            atomicUpdate(&extracted_[parity], [=](AnyT * old)->AnyT * {
                *atomicLinkOf<LinkRoleUnlinked>(*a) = old;
                return a;
            });
        }
        // Extract marked elements and push into the appropriate queue.
        TOOLS_FORCE_INLINE void extractParity(bool parity) {
            for (auto i = 0U; i != rootsUsed; ++i) {
                // Handle the root element, which may race with an insertion.
                AnyT * rootExtract = nullptr;
                while (atomicTryUpdate(&roots_[i], [&](AnyT *& refRoot)->bool {
                    rootExtract = refRoot;
                    bool ret = !!rootExtract;
                    if (ret) {
                        ret = isEnd(*rootExtract);
                        if (ret) {
                            refRoot = *atomicLinkOf<LinkRoleNext>(*rootExtract);
                        }
                    }
                    return ret;
                })) {
                    // The root was removed. Record this and check if the new root can also be extracted.
                    pushExtracted(parity, rootExtract);
                }
                if (!rootExtract) {
                    // Nothing more to do
                    continue;
                }
                // All remaining elements can be accessed more simply as there cannot be a race with push.
                AnyT ** next = atomicLinkOf<LinkRoleNext>(*rootExtract);
                while (AnyT * element = *next) {
                    if (isEnd(*element)) {
                        *next = *atomicLinkOf<LinkRoleNext>(*element);
                        pushExtracted(parity, element);
                    } else {
                        next = atomicLinkOf<LinkRoleNext>(*element);
                    }
                }
            }
        }
        // Extract elements if we're dirty and there are no other extractions in flight. This will return true if
        // the list is still dirty.
        // 
        // When rescan is true, and there is another extraction in flight, this call will ask to make an additional
        // pass over the list to try to find elements to extract. Rescan does not affect the return value.
        TOOLS_FORCE_INLINE bool extract(bool rescan) {
            bool claimed = false;
            bool canDispose = false;
            bool canDisposeLinger = false;
            if (!states_.claimExtract(rescan, claimed, canDispose, canDisposeLinger)) {
                // Did not claim, list is still dirty
                return true;
            }
            bool dirty = false;
            do {
                if (canDisposeLinger) {
                    disposeParity(!claimed);
                }
                if (canDispose) {
                    disposeParity(claimed);
                }
                extractParity(claimed);
            } while (!states_.unclaim(!!extracted_[claimed], !!extracted_[!claimed], canDispose, canDisposeLinger, dirty));
            return dirty;
        }

        AnyT * volatile roots_[rootsUsed]; // current list member (may contain unresolved dirty elements and intermittent extracted items).
        tools::detail::AtomicListStates states_;
        AnyT * volatile extracted_[2U]; // Extracted elements are placed in one of these lists. they alternate when there is contention among readers.
    };

    // Phantom-based singly linked list and hash table. Be aware, that because these use phantom implementations
    // there is likely to be higher than ideal memory load, because the phantom nodes are not deleted until
    // all threads have returned to idle.

    template< typename ElementT, typename PhantomT = StandardPhantom< ElementT >>
    struct StandardPhantomSlistElement
        : PhantomT
    {
        StandardPhantomSlistElement(void)
        {
            slistNext_.reset( /*tools::FlagPointer< ElementT >::make()*/ nullptr);
        }

        tools::FlagPointer< ElementT > volatile slistNext_;
    };

    template< typename ElementT >
    inline void
        phantomSlistMarkForRemove(
            ElementT * ref)
    {
        atomicTryUpdate(phantomSlistNext(*ref), [](tools::FlagPointer< ElementT > & link)->bool {
            if (isEnd(link)) {
                return false;
            }
            setEnd(link);
            return true;
        });
    }

    template< typename ElementT >
    inline tools::FlagPointer< ElementT > volatile *
        phantomSlistNext(
            ElementT & elem)
    {
        return &elem.slistNext_;
    }

    // This slist implementation is safe to read without any memory writes (which would defeat caching) or
    // locks.  Modification of the list use FlagPointer<...> to mark a next pointer as read-only before
    // doing the remove. Threads race actually delete pointers pending removal during any ordinary update.
    // In this way the slist achieves _eventual consistency_, even in the presence of real-time threads.
    //
    // Elements can only exist in a single list during their lifetime, after which they must be destroyed
    // (via the phantom mechanism). Elements are only removed in newest-to-oldest order. Should correctness
    // require a different order, the caller should encode this into the elements to express this difference.
    //
    // This slist implementation has an order of elements. This is used internally to detect overlapping
    // elements and short-circuit searches.
    template< typename ElementT, typename PhantomT >
    struct PhantomSlist
    {
        typedef tools::FlagPointer< ElementT > LinkType;
        typedef ElementT ElementType;
        typedef PhantomT PhantomType;

        PhantomSlist(void)
            : root_(LinkType::make())
        {}
        ~PhantomSlist(void)
        {
            TOOLS_ASSERT(!root_);  // Should have been cleared previously
        }

        bool
            operator!(void) const
        {
            return !root_;
        }

        // Real-only list access. This generally skips removed items as much as is reasonable. The passed in
        // function should have the signature:
        //    (ElementT const &)->bool
        // Returning false, will stop iteration. This method returns a count of elements visited.
        template< typename VisitF >
        unsigned
            peek(
                VisitF const & func) const
        {
            TOOLS_ASSERT(tools::phantomVerifyIsCloaked< PhantomT >());
            // Not allowed to remove the root
            TOOLS_ASSERT(!isEnd(root_));
            unsigned ret = 0U;
            if (ElementT * elem = root_.getNotEnd()) {
                do {
                    ++ret;
                    if (!func(*elem)) {
                        break;
                    }
                    elem = tools::phantomSlistNext(*elem)->get();
                } while (!!elem);
            }
            return ret;
        }

        // Empty the list, returning a count of the number of elements removed.
        size_t
            clear(void)
        {
            if (!*this) {
                // Nothing to remove;
                return 0U;
            }
            LinkType root = tools::atomicExchange(&root_, LinkType::make());
            if (!root) {
                return 0U;
            }
            TOOLS_ASSERT(tools::phantomVerifyIsCloaked< PhantomT >());
            size_t ret = 0U;
            PhantomCloak & phantoms = phantomLocal< PhantomT >();
            while (ElementT * elem = root.get()) {
                ElementT * next;
                tools::atomicTryUpdate(tools::phantomSlistNext(*elem), [=, &next](LinkType & ref)->bool {
                    next = ref.get();
                    if (!isEnd(ref)) {
                        setEnd(ref);
                        return true;
                    }
                    return false;
                });
                // Send element off to the land of wind and ghosts
                phantoms.finalize(tools::AutoDispose< tools::Weakling >(elem));
                ++ret;
                root.reset(next);
            }
            return ret;
        }

        // Conditionally push an element to the front of the list. The provided predicate may read the next
        // element and modify the target to ensure it sorts earlier. The signature for the predicate is:
        //     (LinkType &)->bool
        // The parameter will be nullptr if the list is empty. If the predicate returns false, the insert is
        // aborted. The method returns a bool indicating if the insert happened.
        template< typename PredicateF >
        bool
            push(
                ElementT * elem,
                PredicateF const & pred)
        {
            TOOLS_ASSERT(tools::phantomVerifyIsCloaked< PhantomT >());
            return atomicTryUpdate(&root_, [=](LinkType & ref)->bool {
                TOOLS_ASSERT(!isEnd(ref));
                ElementT * nextElem = ref.get();
                if (!pred(ref)) {
                    return false;
                }
                tools::phantomSlistNext(*elem)->reset(nextElem);
                ref->reset(elem);
                return true;
            });
        }

        // Unconditionally push element to front of list
        void
            push(
                ElementT * elem)
        {
            push(elem, [=](LinkType * root)->bool {
                root->reset(elem);
                return true;
            });
        }

        // Walk the list, applying a transformation as we go, doing maintainence as we go. The signature
        // of the transformation function is:
        //     (ElementT * prev, ElementT * & current)->bool
        // 'prev' will be nullptr for the first element of the list. The transform is allowed to reset 'current'.
        // If the provided 'current' is nullptr, this is the end of the list. Returning false terminates the
        // transform. If the the transformation inserts an element, the current node will visited again.
        // If 'current' is unchanged, the transform advances to the next node. This method returns a count
        // of the number of elements removed.
        template< typename TransformF >
        size_t
            transform(
                TransformF const & func)
        {
            TOOLS_ASSERT(tools::phantomVerifyIsCloaked< PhantomT >());
            size_t ret = 0U;
            LinkType volatile * transIter = &root_;
            ElementT * prev = nullptr;
            for (;; ) {
                ElementT * extracted;
                ElementT * inserted;
                LinkType volatile * transReset;
                ElementT * transPrevReset;
                atomicTryUpdate(transIter, [=, &extracted, &inserted, &transReset, &transPrevReset](LinkType & ref)->bool {
                    extracted = nullptr;
                    inserted = nullptr;
                    if (isEnd(ref)) {
                        // current position was removed, start over
                        transReset = &this->root_;
                        transPrevReset = nullptr;
                        return false;
                    }
                    // By default don't move forward
                    transReset = transIter;
                    transPrevReset = prev;
                    ElementT * sample = ref.get();
                    if (!!sample) {
                        // Is this node removed?
                        LinkType volatile * next = tools::phantomSlistNext(*sample);
                        if (isEnd(*next)) {
                            // Remove this one
                            extracted = sample;
                            ref.reset(next->get());
                            return true;
                        }
                    }
                    if (!func(prev, sample)) {
                        // Terminate transformation
                        transReset = nullptr;
                        return false;
                    } else if (sample == ref.get()) {
                        // No change
                        if (!sample) {
                            // All done
                            transReset = nullptr;
                        } else if (isEnd(*tools::phantomSlistNext(*sample))) {
                            // Marked for remove
                            extracted = sample;
                            ref.reset(tools::phantomSlistNext(*sample)->get());
                            return true;
                        } else {
                            // Advance iteration
                            transReset = tools::phantomSlistNext(*sample);
                            transPrevReset = sample;
                        }
                        return false;
                    }
                    // Transformation function altered the current entry.
                    TOOLS_ASSERT(!isEnd(*tools::phantomSlistNext(*sample)));
                    // Inset at current
                    tools::phantomSlistNext(*sample)->reset(ref.get());
                    ref->reset(sample);
                    inserted = sample;
                    transReset = tools::phantomSlistNext(*sample);
                    transPrevReset = sample;
                    return true;
                });
                if (!transReset) {
                    TOOLS_ASSERT(!extracted && !inserted);
                    break;
                }
                transIter = transReset;
                prev = transPrevReset;
                if (!!extracted) {
                    TOOLS_ASSERT(!inserted);
                    ++ret;
                    tools::phantomLocal< PhantomT >().finalize(tools::AutoDispose< tools::Weakling >(extracted));
                } else if (!!inserted) {
                    // Non-binding callback to make sure we announce the insert. If it returns false,
                    // we early exit. Otherwise, we loop.
                    TOOLS_ASSERT(inserted == prev);
                    ElementT * next = transIter->get();
                    if (!func(prev, next)) {
                        break;
                    }
                    // loop
                }
            }
            return ret;
        }

        // Remove any element for which a predicate function returns true. A given element may be passed
        // to the predicate more than once, as this will restart any time an element is removed. This
        // method returns a count of the elements removed. The signature of the predicate is:
        //     (ElementT const &)->bool
        template< typename PredicateF >
        size_t
            removeIf(
                PredicateF const & func)
        {
            if (!*this) {
                return 0U;
            }
            return transform([&](ElementT * prev, ElementT *& next)->bool {
                if (ElementT * elem = next) {
                    if (func(*elem)) {
                        tools::phantomSlistMarkForRemove(elem);
                    }
                    return true;
                }
                return false;
            });
        }

        // Insert an element into the list. Its position in the list is controlled by a comparator. This
        // method can also conditionally remove all nodes that are 'equivalent' that follow the new one.
        // The signature of the comparator is:
        //     (ElementT const & elem, ElementT const & current)->bool
        // This can be thought of as semantically equivalent to >.
        template< typename PredicateF >
        size_t
            insert(
                ElementT * elem,
                bool replace,
                PredicateF const & comp)
        {
            bool complete = false;
            bool entered = false;
            return transform([&](ElementT * prev, ElementT *& ref)->bool {
                if (!prev) {
                    entered = false;  // We were reset
                } else if (prev == elem) {
                    if (!replace) {
                        return false;
                    }
                    // All done
                    complete = true;
                    entered = true;
                }
                if (!complete) {
                    // Is this where we want to insert?
                    if (!ref || !comp(*ref, *elem)) {
                        // Insert here
                        ref = elem;
                    }
                    // Nope, move to the next
                    return true;
                }
                // Should only get here if doing replace
                TOOLS_ASSERT(replace);
                if (!ref || comp(*elem, *ref)) {
                    // This is an element greater than the new one, nothing more needs removing.
                    return false;
                }
                // Remove after our insert
                if (entered) {
                    tools::phantomSlistMarkForRemove(ref);
                }
                return true;
            });
        }

        // Insert an element. Its position in the list is controlled by a comparator. Then all following
        // nodes matching a different predicate are removed.
        // The signature for the comparator is:
        //     (ElementT const & elem, ElementT const & current)->bool
        // This can be thought of as semantically equivalent to >. The removal predicate has the signature:
        //     (ElementT const &)->bool
        template< typename PredicateF, typename ComparatorF >
        size_t
            replace(
                ElementT * elem,
                PredicateF const & rem,
                ComparatorF const & comp)
        {
            bool complete = false;
            bool entered = false;
            return transform([&](ElementT * prev, ElementT *& ref)->bool {
                if (!prev) {
                    entered = false;  // We were reset
                } else if (prev == elem) {
                    complete = true;
                    entered = true;
                }
                if (!complete) {
                    // Is this where we want to insert?
                    if (!ref || !comp(*ref, *elem)) {
                        // Insert here
                        ref = elem;
                    }
                    // Nope, move to the next
                    return true;
                }
                if (!ref || comp(*elem, *ref)) {
                    // we're past any overlap
                    return false;
                }
                // Remove things after the insert
                if (entered && rem(*ref)) {
                    tools::phantomSlistMarkForRemove(ref);
                }
                return true;
            });
        }

        // Assuming the user desires to have at most one element matching a given predicate, rather than
        // modifying the element in place we allocate a new element. This can frequently be simplier
        // for maintaining consistancy. This new element will be the result of a given function. If
        // the replacement fails (due to a race), the function is given another opportunity to evaluate
        // what it wants to do. Once inserted, the new value will be used to determine equivalence. The
        // signature for the update function is:
        //     (ElementT *&)->bool
        // The parameter will be nullptr at the end of the list. Not modifying the parameter indicates no
        // update. Setting the parameter to nullptr, indicates that the element is to be removed. Setting
        // the parameter to anything else indicates a change in the element. If the replace fails, this
        // new element will be finalized. Returning false indicates that the desired change was made.
        // The signature for the equivalence function is:
        //     (ElementT const &)->bool
        // Returning true indicates that the given element is equivalent to the update and should be removed.
        // Equivalent elements are assumed to be contiguous in the list.
        template< typename UpdateF, typename PredicateF >
        void
            update(
                UpdateF const & gen,
                PredicateF const & equiv)
        {
            TOOLS_ASSERT(tools::phantomVerifyIsCloaked< PhantomT >());
            LinkType volatile * iter = &root_;
            for (;; ) {
                LinkType prevLink = derefCanonicalize(&iter);
                ElementT * prev = prevLink.get();
                ElementT * next = prev;
                if (!gen(next)) {
                    // generator is all done
                    return;
                }
                if (next == prev) {
                    // No change
                    if (!prev) {
                        // At end of list
                        return;
                    }
                    // advance
                    iter = tools::phantomSlistNext(*prev);
                    continue;
                }
                // This element is to be replaced.
                if (!!next) {
                    tools::phantomSlistNext(*next)->reset(prev);
                    if (atomicCas(iter, prevLink, LinkType::make(next)) == prevLink) {
                        // Update succeeded
                        replaceAfterIf(prev, equiv);
                        break;
                    }
                    // It is safe to assume the new element was never visible to any other code, so we
                    // can finalize it immediately.
                    tools::AutoDispose<> finalize(static_cast< tools::Weakling * >(next));
                } else {
                    // There are multiple steps to removing the current element.
                    //   1) Ensure every replacable element after this is marked as removed. There may be
                    //      another operation we are racing with, and that may not have completed doing its
                    //      removes.
                    //   2) Mark this element as removed (this can actually be allowed to fail)
                    //   3) Update the link to remove this element. This step must succeed.
                    // Because there may be races, we do the remove immediately to serve as the update
                    // even and guarantee stability.
                    LinkType volatile * nextLink = tools::phantomSlistNext(*prev);
                    replaceAfterIf(nextLink->get(), equiv);
                    atomicTryUpdate(nextLink, [](LinkType & ref)->bool {
                        bool ret = !isEnd(ref);
                        if (ret) {
                            setEnd(ref);
                        }
                        return ret;
                    });
                    // Lastly do the remove, forming a stable state transition.
                    if (atomicCas(iter, prevLink, LinkType::make(nextLink->get())) == prevLink) {
                        tools::phantomLocal< PhantomT >().finalize(tools::AutoDispose< tools::Weakling >(prev));
                        break;
                    }
                }
                // Start over
                iter = &root_;
            }
            // Canonicalize
            while (ElementT * elem = derefCanonicalize(&iter).get()) {
                iter = tools::phantomSlistNext(*elem);
            }
        }
    private:
        // Deref a link iterator unless the element has been flagged for removal. In that case, try to
        // remove it and advance the iterator. If the iterator is, itself, marked for removal, reset.
        LinkType
            derefCanonicalize(
                LinkType volatile ** __restrict ref)
        {
            LinkType prevUpdate;
            prevUpdate.reset(**ref);
            for (;; ) {
                // Qualify what the link is pointing at
                if (isEnd(prevUpdate)) {
                    // This link is marked as removed, reset back to the root.
                    TOOLS_ASSERT(*ref != &root_);
                    *ref = &root_;
                    prevUpdate.reset(**ref);
                }
                ElementT * prev = prevUpdate.getNotEnd();
                if (!prev) {
                    break;
                }
                // Make sure we're not inserting before a removed element
                LinkType nextUpdate;
                nextUpdate.reset(*tools::phantomSlistNext(*prev));
                if (!isEnd(nextUpdate)) {
                    break;
                }
                nextUpdate.reset(atomicCas(*ref, prevUpdate, LinkType::make(nextUpdate.get())));
                if (nextUpdate == prevUpdate) {
                    // This was an exact removal from the list for this element. Finalize it and loop.
                    tools::phantomLocal< PhantomT >().finalize(tools::AutoDispose< tools::Weakling >(prev));
                } else {
                    prevUpdate = nextUpdate;
                }
                // reconsider the current iterator
            }
            return prevUpdate;
        }

        // Starting from current element, mark all elements in like as end if they pass the given predicate.
        template< typename PredicateF >
        void
            replaceAfterIf(
                ElementT * elem,
                PredicateF const & pred)
        {
            while (ElementT * rem = elem) {
                if (!pred(*rem)) {
                    return;
                }
                atomicTryUpdate(tools::phantomSlistNext(*rem), [&elem](LinkType & ref)->bool {
                    elem = ref.get();
                    if (!isEnd(ref)) {
                        setEnd(ref);
                        return true;
                    }
                    return false;
                });
            }
        }

        // Head of the list, never set to end
        LinkType volatile root_;
    };

    // Phantom hash map, is similar to unordered_map, implemented with phantom slist. Unlike unordered_map,
    // phantom hash map the slist under each bucket is ordered. This improves the efficiency of many
    // operations, especially in dense maps. This also alllows the same implementation to support
    // unordered_multimap. The default number of buckets is 131072, this is larger than most applications
    // require. Be aware that this also represents a 2MB allocation per phantom hash map. Phantom hash map
    // requires a function to convert from the element type to the key type. A typical signature for this
    // function is:
    //     (ElementT const &)->KeyT const &
    // Though the 'const &' on the return is optional. This function must be named keyOf and defined in
    // the same namespace of ElementT as we find it via ADL.
    template< typename ElementT, typename KeyT, typename PhantomT, size_t bucketsUsed = 131072U >
    struct PhantomHashMap
    {
        typedef tools::PhantomSlist< ElementT, PhantomT > BucketT;

        PhantomHashMap(void)
            : hashInit_(tools::hashAnyInit< KeyT >())
        {}

        size_t
            clear(void)
        {
            return std::accumulate(buckets_, buckets_ + bucketsUsed, static_cast< size_t >(0U), [](size_t left, BucketT & right)->size_t {
                return left + right.clear();
            });
        }

        // Real-only map access. This generally skips removed items as much as is reasonable. The passed in
        // function should have the signature:
        //    (ElementT const &)->bool
        // Returning false, will stop iteration only on that bucket.
        template< typename VisitorF >
        void
            forEach(
                VisitorF const & visitor) const
        {
            std::for_each(buckets_, buckets_ + bucketsUsed, [&](BucketT const & b)->void {
                b.peek(visitor);
            });
        }

        // Iterate the map, applying the given function to every element with a key matching the given. The
        // visitor function has the signature:
        //     (ElementT &)->bool
        // Returning false stops the iteration. This method returns a count of elements visited.
        template< typename VisitorF >
        unsigned
            forEachKey(
                KeyT const & __restrict key,
                VisitorF const & visitor)
        {
            return buckets_[tools::impl::hashAny(key, hashInit_) % bucketsUsed].peek([&key, &visitor](ElementT & __restrict elem)->bool {
                KeyT const & __restrict elemKeyRef = keyOf(elem);
                return (elemKeyRef < key) || !((key < elemKeyRef) || !visitor(elem));
            });
        }

        // Remove all elements matching a given key. This method returns a count of elements removed, which
        // may include elements not matching the key (differed removal).
        size_t
            removeKey(
                KeyT const & key)
        {
            return buckets_[tools::impl::hashAny(key, hashInit_) % bucketsUsed].removeIf([&](ElementT & elem)->bool {
                return (keyOf(elem) == key);
            });
        }

        // Remove all elements that both match a given key and for which a given predicate returns true. The
        // signature of this predicate is:
        //     (ElementT &)->bool
        // This method returns a count of elements removed, which may include elements not matching the key
        // (differed removal).
        template< typename VisitorF >
        size_t
            removeIf(
                KeyT const & key,
                VisitorF const & pred)
        {
            return buckets_[tools::impl::hashAny(key, hashInit_) % bucketsUsed].removeIf([&](ElementT & elem)->bool {
                bool ret = false;
                if (keyOf(elem) == key) {
                    ret = func(elem);
                }
                return ret;
            });
        }

        // Remove all elements, across all buckets, for which a given predicate returns true. The signature
        // of this predicate is:
        //     (ElementT &)->vool
        // This method returns a count of elements removed, which may include elements not matching the key
        // (differed removal).
        template< typename VisitorF >
        size_t
            removeIf(
                VisitorF const & visitor)
        {
            return std::accumulate(buckets_, buckets_ + bucketsUsed, static_cast< size_t >(0U), [&](size_t left, BucketT & right)->size_t {
                return left + b.removeIf(visitor);
            });
        }

        // Remove a specific element (if present). This method returns a count of elements removed.
        size_t
            remove(
                ElementT * rem)
        {
            return buckets_[tools::impl::hashAny(keyOf(*rem), hashInit_) % bucketsUsed].removeIf([&](ElementT & elem)->bool {
                return (&elem == rem);
            });
        }

        // Unconditionally insert an element. If replace is true, all existing equivalent elements are
        // removed. Otherwise the equivalent items are left in place.
        void
            insert(
                ElementT * elem,
                bool replace = false)
        {
            buckets_[tools::impl::hashAny(keyOf(*elem), hashInit_) % bucketsUsed].insert(elem, replace, [&](ElementT const & left, ElementT const & right)->bool {
                return (keyOf(left) < keyOf(right));
            });
        }

        // Unconditionally insert an element, replacing any equivalent items indicated by a given predicate.
        // The signature of the predicate is:
        //     (ElementT const &)->bool
        template< typename PredicateF >
        void
            replace(
                ElementT * elem,
                PredicateF const & pred)
        {
            buckets_[tools::impl::hashAny(keyOf(*elem), hashInit_) % bucketsUsed].replace(elem, pred, [&](ElementT const & left, ElementT const & right)->bool {
                return (keyOf(left) < keyOf(right));
            });
        }

        // Assuming the user desires to have at most one element matching a given key, rather than modifying
        // the element in place we allocate a new element. This can frequently be simplier for maintining
        // consistancy. This new element will be the result of a given function. The function will be
        // presented with the current element for the key. If this is nullptr, no current element exists. The
        // function may choose to return the same element, indicating no change. It can also return nullptr,
        // indicating the element is to be removed. Or lastly, it can create a new element, indicating an
        // update. If the replacement fails (due to a race), the function will be given another opportunity
        // to evaluate what it wants to do. The signature of the function is:
        //     (ElementT *&)->void
        template< typename UpdateF >
        void
            update(
                KeyT const & key,
                UpdateF const & gen)
        {
            buckets_[tools::impl::hashAny(key, hashInit_) % bucketsUsed].update([&](ElementT *& ref)->bool {
                ElementT * prev = ref;
                if (!!prev) {
                    if (keyOf(*prev) < key) {
                        // too early in the list
                        return true;
                    }
                    if (key < keyOf(*prev)) {
                        ref = nullptr;
                        prev = nullptr;
                    }
                }
                gen(ref);
                // If this update is being removed, trigger an update
                return (ref != prev);
            }, [&](ElementT const & ref)->bool {
                return !(key < keyOf(ref));
            });
        }

        // Visit each element with a given function. This is more strict than forEach as elements marked
        // for removal are not enumerated. The signature for the function is:
        //     (ElementT const &)->void
        template< typename VisitorF >
        void
            forEachUnique(
                VisitorF const & func)
        {
            ElementT * __restrict prev;
            auto wrap = [&](ElementT & elem)->bool {
                if (!!prev && !(keyOf(*prev) < keyOf(elem))) {
                    // This is a repeat
                    return true;
                }
                prev = &elem;
                func(elem);
                return true;
            };
            std::for_each(buckets_, buckets_ + bucketsUsed, [&](BucketT const & b)->void {
                prev = nullptr;  // new bucket, thus no overlap
                b.peek(wrap);
            });
        }

        // Visit elements matching a given key. The signature of the visitor is:
        //     (ElementT *)->void
        // If no element matches, nullptr will be passed to the visitor.
        template< typename VisitF >
        void
            find(
                KeyT const & key,
                VisitF const & visitor)
        {
            ElementT * elem = nullptr;
            buckets_[tools::impl::hashAny(key, hashInit_) % bucketsUsed].peek([&](ElementT & __restrict test)->bool {
                KeyT const & __restrict refKey = keyOf(test);
                bool ret = (refKey < key);
                if (ret) {
                    return ret;
                }
                if (!(key < refKey)) {
                    elem = &test;
                }
                return ret;
            });
            visitor(elem);
        }

        uint32 hashInit_;
        BucketT buckets_[bucketsUsed];
    };
}; // tools namespace
