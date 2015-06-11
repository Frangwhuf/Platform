#include "toolsprecompiled.h"

#include <tools/Meta.h>
#include <tools/Tools.h>
#include <tools/UnitTest.h>

#include <boost/type_traits.hpp>

using namespace tools;

namespace tools {
    namespace detail {
        StringId
        standardNameOf( int8_t *** )
        {
            static StringId name( "__c_int8" );
            return name;
        }

        StringId
        standardNameOf( int16_t *** )
        {
            static StringId name( "__c_int16" );
            return name;
        }

        StringId
        standardNameOf( int32_t *** )
        {
            static StringId name( "__c_int32" );
            return name;
        }

        StringId
        standardNameOf( int64_t *** )
        {
            static StringId name( "__c_int64" );
            return name;
        }

        StringId
        standardNameOf( uint8_t *** )
        {
            static StringId name( "__c_uint8" );
            return name;
        }

        StringId
        standardNameOf( uint16_t *** )
        {
            static StringId name( "__c_uint16" );
            return name;
        }

        StringId
        standardNameOf( uint32_t *** )
        {
            static StringId name( "__c_uint32" );
            return name;
        }

        StringId
        standardNameOf( uint64_t *** )
        {
            static StringId name( "__c_uint64" );
            return name;
        }

        StringId
        standardNameOf( bool *** )
        {
            static StringId name( "__c_bool" );
            return name;
        }
    };  // detail namespace

    StringId
    standardNameOf( StringId *** )
    {
        static StringId name( "__c_stringid" );
        return name;
    }
};  // tools namespace

#if TOOLS_UNIT_TEST

TOOLS_TEST_CASE("tools.types", [](Test &)
{
  TOOLS_ASSERTR( ! boost::is_signed< tools::uchar >::value );
  TOOLS_ASSERTR( sizeof( tools::uchar ) == 1 );
  TOOLS_ASSERTR( ! boost::is_signed< tools::uint8 >::value );
  TOOLS_ASSERTR( sizeof( tools::uint8 ) == 1 );
  TOOLS_ASSERTR( ! boost::is_signed< tools::Byte >::value );
  TOOLS_ASSERTR( sizeof( tools::Byte ) == 1 );
  TOOLS_ASSERTR( boost::is_signed< tools::sint8 >::value );
  TOOLS_ASSERTR( sizeof( tools::sint8 ) == 1 );
  TOOLS_ASSERTR( ! boost::is_signed< tools::uint16 >::value );
  TOOLS_ASSERTR( sizeof( tools::uint16 ) == 2 );
  TOOLS_ASSERTR( boost::is_signed< tools::sint16 >::value );
  TOOLS_ASSERTR( sizeof( tools::sint16 ) == 2 );
  TOOLS_ASSERTR( ! boost::is_signed< tools::uint32 >::value );
  TOOLS_ASSERTR( sizeof( tools::uint32 ) == 4 );
  TOOLS_ASSERTR( boost::is_signed< tools::sint32 >::value );
  TOOLS_ASSERTR( sizeof( tools::sint32 ) == 4 );
  TOOLS_ASSERTR( ! boost::is_signed< tools::uint64 >::value );
  TOOLS_ASSERTR( sizeof( tools::uint64 ) == 8 );
  TOOLS_ASSERTR( boost::is_signed< tools::sint64 >::value );
  TOOLS_ASSERTR( sizeof( tools::sint64 ) == 8 );
});

#endif /* TOOLS_UNIT_TEST */
