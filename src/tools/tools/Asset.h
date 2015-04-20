#pragma once

#include <tools/Environment.h>
#include <tools/InterfaceTools.h>
#include <tools/Invalidation.h>
#include <tools/String.h>
#include <tools/Tools.h>

#include <boost/mpl/as_sequence.hpp>
#include <boost/mpl/deref.hpp>
#include <boost/mpl/empty_sequence.hpp>
#include <boost/mpl/list.hpp>
#include <boost/mpl/next.hpp>

// The asset system has a few goals:
//   *) only load the bits for an asset once per revision of that asset.
//   *) provide a mechanism to implement semantic understanding of any particular asset type.
//   *) provide any given usage site with a unique instance through which to access the asset.
//
// for the first point, the asset system will create an maintain a singleton for each loaded
// revision of an asset.  In general an asset does not have to be a filesystem object, it only
// need to be a blob of data that can be uniquely identified via something that behaves like
// a filesystem path or URL.  Assets are believed to be vaguely hierarchical, where parent
// assets understand how to find and load their immediate children.  This extends beyond
// directories and files, and can include thinsg like sub-assets of files (for example files/
// directories within a tar/archieve, or materials/meshes within a 3D model, etc).  Assets
// may even be entirely proceedural, as long as the asset system knows how to factory them
// (see ExistentAssets).
//
// If an asset supports monitoring it's persistant location for changes, then the implementation
// of that asset type should support update invalidation.  When the persistant location
// changes, the status of any instances of that asset will update to reflect this information.
// Usage sites are not requiered to reload to the most current state of the asset, the older
// state will remain available as long as there are instances using it.  For each revision of an
// asset, the system creates a singleton to manage that revision of that asset.  This singleton
// is never exposed to a usage site.  Rather an instance type, which holds a reference on the
// singleton, is given to usage sites.  These instances semantically contain per-instance
// dynamic state of an asset, where the singleton semantically contains static state for an
// asset.  For example, consider an animation asset for a 3D model.  The set of matricies that
// represent a particular frame in the animation is fairly specific to a particular model and
// where it is in that animation.  As such that is probably best represented as per-instance
// data.  However, decoding the animation from its persistant form to something that is easy
// to compute with is work that can be shared by every user of that animation.  As such that
// makes a good candidate for singleton state.  Not all asset types will have both kinds of
// state.  For example XML assets would likely only have singleton state, containing the parsed
// XML document.
//
// This leads naturally into the second point.  When implementing an asset type, it needs to be
// possible to code the semantics of both the singleton and per-instance behaviors.  The singleton
// implementation is typically created after the data has loaded into memory, where it is then
// resposible for parsing the data of the asset, as well as factorying per-instance data and
// child assets (if any).  Implementation of singleton semantics must implement AssetSingleton,
// and per-instance semantics must implement AssetInstance.  If there are no per-instance semantics
// the AssetSingleton implementation can return itself as the instance data and the system will
// behave correctly.  Associating the semantic implementation with a particular kind of asset is
// done though static registration when there is a way to determine this association prior to
// loading the bits.  A more complex association factory is needed when this is not the case.
//
// Editing of assets is controlled through a seperate interface (AssetEdit), if a particular
// asset type provides this interface via Unknown::getInterface, then that asset supports being
// edited.  There may be multiple instances of AssetEdit types created for an asset at the same
// time.  There is minimal synchronization between these instances.  The system only promises
// that commits of edits will appear to any users of the asset in the order they are committed.

namespace tools {
    namespace detail {
        enum AssetState : unsigned {
            // This asset has not yet finished loading, subscribe to be told when it may be ready.
            AssetStateLoading,
            // This asset is loaded and ready for use.
            AssetStateReady,
            // This asset is loaded and ready, but there is a newer version that could be loaded.
            // Use reload to get the newer version.
            AssetStateReloadable,
            // This asset is being torn-down.  If you get a delivery to your subscription and the
            // asset is in this state you are required to release your reference and dispose your
            // subscription.  Generally a user should never see this.
            // TODO: make this impossible to observe, then delete it.  Any interface that requires
            // a specific action on the user is likely to fail.
            AssetStateDying,
            // There was no asset found at the specified location.  The ErrorAsset may have more
            // information.
            AssetStateNotFound,
            // This asset is, in some way, invalid (e.g.: parsing or validation errors while loading).
            // The ErrorAsset may contain more information about the details of what went wrong.
            AssetStateInvalid
        };
    };  // detail namespace

    struct Asset;

    // The AssetControl interface contains operations that apply to all asset types.  Both
    // the instance provided to usage sites as well as the internal singleton implement this
    // interface.  The load stat (returned by AssetControl::state()) reports what the current
    // status of the asset is, and if you can expect to get data from it.  Changes to this
    // status can be observed by registering an invalidation thunk with AssetControl::newSubscription.
    //
    // When you with to refer to the current asset (for example to make a copy for some other
    // usage site), use an asset path of AssetPathThis (".").
    //
    // The helper newAssetLoadRequest manages the common case of waiting on the loading state
    // machine.  This request will complete only when the asset has finished loading (or reached
    // an error).  Though if a usage site wants to support reload logic, it is likely that
    // a helper such as AutoAsset<...> will be more usefull.  This encapsulates a standard
    // implementation of the full asset state machine.
    //
    // Should an error occur during (re)load, the asset state will become either AssetStateInvalid
    // or AssetStateNotFound (depending on the nature of the error).  In this case an ErrorAsset
    // can be retrieved via Unknown::getInterface.  This will contain information relevant to
    // whatever went wrong in loading the asset.  In either case the asset will continue to function
    // as if it had loaded, allowing the user to replace the broken asset (possibly via AssetEdit)
    // to repair the problem and trigger another relead.
    //
    // If the implementation provides Invalidatable, then an AssetSingleton or AssetInstance can
    // flag when the persistant content of the asset may have changed.  This may also be triggered
    // when an AssetEdit commits.
    struct AssetControl
        : Unknown
        , Subscribable
    {
        // Load an asset by name/path relative to this asset.  The returned Asset may not be
        // finished loading, use AssetControl::sttaus to determine this.
        virtual AutoDispose< Asset > load( StringId const & ) throw() = 0;
        // Return an Asset representing the most current revision of this asset.  As with
        // AssetControl::load, the returned asset may not be done loading.  The asset singleton
        // will return a new instance, where the per-instance implementation will return the
        // same pointer with it's internals shifted to the new data.  Any asset types with
        // internal dependencies will also be reloaded.  E.g.: reloading a surface will also
        // cause the materials that depend on it to be reloaded.
        virtual AutoDispose< Asset > reload( void ) throw() = 0;
        // Return the current state of this asset.  See AssetState for details of what the
        // possible values mean.  Use AssetControl::newSubscription to observe changes to
        // this state.
        virtual detail::AssetState status( void ) throw() = 0;
        // Test if the given asset is logically equivalent to this one.
        virtual bool operator==( AssetControl const & ) throw() = 0;
        // The name of this asset.
        virtual StringId name( void ) throw() = 0;
        // The path to this asset.
        virtual StringId path( void ) throw() = 0;
    };

    // Asset is the primar interface for all asset types.  It is AssetControl with lifetime
    // control.  For ease of implementation, a code site that gets an Asset can behave as if
    // it fully controlls the load lifetime of the asset.  The asset system will, itself,
    // decide when to flush the bits out of memory.  Use Unknown::getInterface to access
    // interfaces specific to a particular asset type.
    //
    // Both the interface provided to usage sites and the internal singleton both use this
    // interface.  However the internal singleton should never be exposed directly to a usage
    // site.
    struct Asset
        : AssetControl
        , Disposable
    {};

    // Use this as a constant for reloading the current asset.
    TOOLS_API StringId const & AssetPathThis( void );

    // Use this as a constant for reloading the parent asset.
    TOOLS_API StringId const & AssetPathParent( void );

    // Abstract interface for a component that loads the bits for a singleton.
    struct AssetLoader
        : Unknown
        , Request
    {};

    // AssetInstance is used in implementing the semantics for an asset type.  Any per-instance
    // implementation needs to implement this.  Any asset type specific interfaces are provided
    // though Unknown::getInterface.  If the asset type has per-instance state, the AssetSingleton
    // will return an instance of this interface for that per-instance state.  If the asset
    // type does not specifically have per-instance state, the AssetSingleton can return itself
    // as the per-instance data.  The system will do the right thing in that case.
    struct AssetInstance
        : Unknown
        , Disposable
    {};

    // AssetSingleton is used in implementing the semantics for an asset type.  Any asset singleton
    // implementation neesd to implement this.  The system will factory one of these once the
    // bits have been loaded into memory and the system is able to determine what kind of
    // AssetSingletone to create.  The system will track reference counts for outstanding
    // instances.  When it believes the AssetSingleton should be disposed, it will communicate
    // this via AssetSingleton::pendingDispose.
    struct AssetSingleton
        : AssetInstance
    {
        // Create a new per-instance instance for this asset type.  If no asynchronous work
        // needs to be done, this can return NULL, which indicates the operation completed
        // synchronously.
        virtual AutoDispose< Request > newInstance( AutoDispose< AssetInstance > & ) = 0;
        // Create a child for this asset, based on its name.  The implementation should not
        // track its children, that will be handled by the system internally.  If this returns
        // NULL, the operation completed synchronously.
        //
        // Note: there is no standard interface to enumerate all possible children of an asset
        // as this may be impossible or prohibitively expensive for some asset types.  IF this
        // is an important feature it should be implemented via an interface for that asset type.
        virtual AutoDispose< AssetLoader > newChild( StringId const &, StringId & match, StringId & residue ) = 0;
        // When the system wants to dispose this, there may be work in progress that cannot
        // reasonably be interrupted.  So the system will call this method, passing a Disposable.
        // When this implementation can be deleted, it should dispose this parameter, which will
        // cause this implementation to be disposed.
        virtual void pendingDispose( AutoDispose<> & ) = 0;
    };

    struct AssetSingletonFactory
    {
        // The passed in AssetLoader is for access to the Unknown interface.
        virtual AutoDispose< AssetSingleton > factory( AutoDispose< AssetLoader > const &, StringId const & ) = 0;
    };

    // Generate assets that can be reached by paths on the local file system.
    struct LocalFileSystem : SpecifyService< AssetControl > {};

    template< typename InterfaceT >
    struct AutoAsset
        : tools::Notifiable< AutoAsset< InterfaceT >>
    {
        typedef InterfaceT Type;

        AutoAsset( void ) throw()
            : itf_( NULL )
        {}
        AutoAsset( AutoAsset const & c ) throw()
            : itf_( NULL )
        {
            TOOLS_ASSERT( !c.asset_ );
            TOOLS_ASSERT( !c.subscription_ );
        }
        AutoAsset( AutoAsset && r ) throw()
            : asset_( std::move( r.asset_ ))
            , itf_( r.itf_ )
        {
            if( !!asset_ ) {
                subscription_.reset( new SubscriptionLifetime<>( *asset_, this->template toThunk< &AutoAsset::stateInvalid >() ));
            }
        }
        void swap( AutoAsset & r ) throw() {
            asset_.swap( r.asset_ );
            subscription_.release();
            r.subscription_.release();
            itf_ = r.itf_ = NULL;
        }
        void swap( AutoAsset && r ) throw() {
            asset_.swap( r.asset_ );
            subscription_.release();
            r.subscription_.release();
            itf_ = r.itf_ = NULL;
        }
        InterfaceT * get( void )
        {
            if( !itf_ ) {
                if( !!asset_ && ( asset_->status() != tools::detail::AssetStateLoading )) {
                    itf_ = asset_->getInterface< InterfaceT >();
                    if( !itf_ ) {
                        //if( ErrorAsset * err = asset_->getInterface< ErrorAsset >() ) {
                        //    // TODO: process the kind of error and return something meaningful
                        //    TOOLS_ASSERT( !"Not implemented" );
                        //}
                        TOOLS_ASSERT( !"NOt implemented" );
                    }
                }
            }
            return itf_;
        }
        // TODO: probably want access to the error state/asset if there is one
        Asset * getAsset( void ) const throw()
        {
            return asset_.get();
        }
        AutoAsset< InterfaceT > & operator=( AutoAsset const & c ) throw()
        {
            TOOLS_ASSERT( !c.asset_ );
            TOOLS_ASSERT( !c.subscription_ );
            asset_.release();
            subscription_.release();
            itf_ = NULL;
            return *this;
        }
        AutoAsset< InterfaceT > & operator=( AutoAsset && r ) throw()
        {
            asset_ = std::move( r.asset_ );
            if( !!asset_ ) {
                subscription_.reset( new SubscriptionLifetime<>( *asset_, this->template toThunk< &AutoAsset< InterfaceT>::stateInvalid >() ));
            }
            itf_ = r.itf_;
            return *this;
        }
        InterfaceT & operator*( void ) const throw() {
            InterfaceT * ret = get();
            TOOLS_ASSERT( !!ret );
            return *ret;
        }
        InterfaceT * operator->( void ) const throw() {
            return get();
        }
        bool operator!( void ) const throw() {
            return !asset_;
        }
    protected:
        void stateInvalid( void ) throw()
        {
            itf_ = NULL;
            if( !!asset_ ) {
                detail::AssetState state = asset_->status();
                if( ( state == tools::detail::AssetStateReloadable ) || ( state == tools::detail::AssetStateReady )) {
                    AutoDispose< Asset > result( asset_->reload() );
                    TOOLS_ASSERT( result.get() == asset_.get() );
                }
            }
        }
    private:
        AutoDispose< Asset > asset_;
        AutoDispose<> subscription_;
        InterfaceT * itf_;
    };
};  // namespace tools
