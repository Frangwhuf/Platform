#pragma once

#include <tools/Asset.h>
#include <tools/Concurrency.h>
#include <tools/InterfaceTools.h>
#include <tools/Invalidation.h>

#include <unordered_map>

namespace tools {
    struct InternalAssetSingleton
        : Asset
        , Notifiable< InternalAssetSingleton >
        , Completable< InternalAssetSingleton >
    {
        typedef std::unordered_map< StringId, AutoDispose< Asset >, std::hash< StringId >, std::equal_to< StringId >, AllocatorAffinity< std::pair< StringId, AutoDispose< Asset >>>> ChildMap;

        InternalAssetSingleton( AutoDispose< Asset > &, AutoDispose< AssetLoader > &, StringId const &, AssetSingletonFactory & );

        // Asset
        void dispose( void );
        void * getInterface( StringId const & ) const throw();
        AutoDispose< Asset > load( StringId const & ) throw();
        AutoDispose< Asset > reload( void ) throw();
        detail::AssetState status( void ) throw();
        AutoDispose<> newSubscription( Thunk const &, Thunk const & );
        bool operator==( AssetControl const & ) throw();
        StringId name( void ) throw();
        StringId path( void ) throw();

        // local methods
        AutoDispose< InternalAssetSingleton > ref( void ) const throw();
        void loaded( Error * );
        void onChanged( void );
        void parentUpdate( void );

        AutoDispose< Asset > parent_;
        AutoDispose< AssetLoader > loader_;
        AssetSingletonFactory & factory_;
        AutoDispose< AssetSingleton > singleton_;
        AutoDispose<> subscription_;
        AutoDispose<> parentSub_;
        AtomicSubscribableItem< detail::AssetState > state_;
        AutoDispose< Monitor > childrenLock_;
        ChildMap children_;
        bool reloaded_;
        StringId name_;
        unsigned mutable volatile refs_;
    };

    struct InternalAssetPerInstance
        : Asset
        , Notifiable< InternalAssetPerInstance >
        , Completable< InternalAssetPerInstance >
    {
        InternalAssetPerInstance( AutoDispose< Asset > & );

        // Asset
        void dispose( void );
        void * getInterface( StringId const & ) const throw();
        AutoDispose< Asset > load( StringId const & ) throw();
        AutoDispose< Asset > reload( void ) throw();
        detail::AssetState status( void ) throw();
        AutoDispose<> newSubscription( Thunk const &, Thunk const & );
        bool operator==( AssetControl const & ) throw();
        StringId name( void ) throw();
        StringId path( void ) throw();

        // local methods
        void singletonUpdate( void );
        AutoDispose< InternalAssetPerInstance > ref( void ) const throw();
        void instanceCompleted( Error * );

        AutoDispose< Asset > singleton_;
        AutoDispose< AssetInstance > instance_;
        AutoDispose<> subscription_;
        AtomicSubscribableItem< detail::AssetState > state_;
        detail::AssetState target_;
        AutoDispose< Request > instanceReq_;
        unsigned mutable volatile refs_;
    };

    struct InternalAssetParentWrapper
        : Asset
    {
        InternalAssetParentWrapper( InternalAssetSingleton &, StringId const & );
        ~InternalAssetParentWrapper( void );

        // Asset
        void dispose( void );
        void * getInterface( StringId const & ) const throw();
        AutoDispose< Asset > load( StringId const & ) throw();
        AutoDispose< Asset > reload( void ) throw();
        detail::AssetState status( void ) throw();
        AutoDispose<> newSubscription( Thunk const &, Thunk const & );
        bool operator==( AssetControl const & ) throw();
        StringId name( void ) throw();
        StringId path( void ) throw();

        AutoDispose< InternalAssetSingleton > asset_;
        StringId name_;
    };

    struct InternalAssetFullLoadWrapper
        : Asset
        , Notifiable< InternalAssetFullLoadWrapper >
    {
        InternalAssetFullLoadWrapper( AutoDispose< Asset > &, StringId const &, AssetSingletonFactory & );

        // Asset
        void dispose( void );
        void * getInterface( StringId const & ) const throw();
        AutoDispose< Asset > load( StringId const & ) throw();
        AutoDispose< Asset > reload( void ) throw();
        detail::AssetState status( void ) throw();
        AutoDispose<> newSubscription( Thunk const &, Thunk const & );
        bool operator==( AssetControl const & ) throw();
        StringId name( void ) throw();
        StringId path( void ) throw();

        // local methods
        void stateUpdate( void );
        AutoDispose< Asset > ref( void ) const throw();
        void childInvalidate( void );

        AutoDispose< Asset > parent_;
        StringId residuePath_;
        AssetSingletonFactory & factory_;
        AutoDispose< Asset > asset_;
        StringId relativePath_;
        AutoDispose<> subscription_;
        AutoDispose< Publisher > publisher_;
        AutoDispose<> childSubscription_;
        unsigned mutable volatile refs_;
    };
}; // tools namespace
