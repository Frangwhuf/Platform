#pragma once

#include <tools/MetaBase.h>
#include <tools/String.h>

namespace tools {
    namespace detail {
        // Demangle sompiler type/function/method names into a human readable form
        TOOLS_API StringId platformDemangleTypeInfo( std::type_info const & ) throw();
        TOOLS_API StringId platformDemangleSymbol( StringId const & ) throw();
        TOOLS_API StringId platformSymbolNameFromAddress( void *, unsigned * = NULL );
        TOOLS_API StringId symbolNameFromAddress( void *, unsigned * = NULL );
        TOOLS_API void logStackTrace( bool, bool );

        // Standard type to name implementation, overload this using ADL to take control of an
        // individual name.
        template< typename TypeT >
        inline StringId standardNameOf( TypeT *** ) {
            return platformDemangleTypeInfo( typeid( TypeT ));
        }

        // names for the primitive types
        TOOLS_API StringId standardNameOf( int8_t *** );
        TOOLS_API StringId standardNameOf( int16_t *** );
        TOOLS_API StringId standardNameOf( int32_t *** );
        TOOLS_API StringId standardNameOf( int64_t *** );
        TOOLS_API StringId standardNameOf( uint8_t *** );
        TOOLS_API StringId standardNameOf( uint16_t *** );
        TOOLS_API StringId standardNameOf( uint32_t *** );
        TOOLS_API StringId standardNameOf( uint64_t *** );
        TOOLS_API StringId standardNameOf( bool *** );

        template< typename TypeT >
        inline StringId dispatchNameOf( TypeT *** typ ) {
            return standardNameOf( typ );
        }
    };  // detail namespace

    template< typename TypeT >
    inline StringId * staticServiceCacheInit( StringId ***, TypeT *** typ ) {
        return new StringId( tools::detail::dispatchNameOf( typ ));
    }

    // Return a StringId for a given type.  This is good for comparing types, diagnostics, etc.
    template< typename TypeT >
    inline StringId const & nameOf( TypeT *** = NULL ) {
        return *tools::staticServiceCacheFetch< StringId, TypeT >();
    }

    TOOLS_API StringId standardNameOf( StringId *** );
}; // namespace tools
