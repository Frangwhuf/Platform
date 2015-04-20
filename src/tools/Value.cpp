#include "toolsprecompiled.h"

//#include <tools/Error.h>
#include <tools/UnitTest.h>
#include <tools/Value.h>

#include <boost/numeric/conversion/cast.hpp>

using namespace tools;

namespace {
  template< typename ToT >
  ToT ScalerConvert( Value const & v ) {
    if( v.isInteger() ) {
      if( v.isSigned() ) {
        switch( v.sizeOf() ) {
          case 1:
            return boost::numeric_cast< ToT >( boost::any_cast< sint8 >( v.value_ ) );
          case 2:
            return boost::numeric_cast< ToT >( boost::any_cast< sint16 >( v.value_ ) );
          case 4:
            return boost::numeric_cast< ToT >( boost::any_cast< sint32 >( v.value_ ) );
          case 8:
            return boost::numeric_cast< ToT >( boost::any_cast< sint64 >( v.value_ ) );
          default:
            break;
        }
      } else {
        switch( v.sizeOf() ) {
          case 1:
            return boost::numeric_cast< ToT >( boost::any_cast< uint8 >( v.value_ ) );
          case 2:
            return boost::numeric_cast< ToT >( boost::any_cast< uint16 >( v.value_ ) );
          case 4:
            return boost::numeric_cast< ToT >( boost::any_cast< uint32 >( v.value_ ) );
          case 8:
            return boost::numeric_cast< ToT >( boost::any_cast< uint64 >( v.value_ ) );
          default:
            break;
        }
      }
    } else if( v.isFloat() ) {
      switch( v.sizeOf() ) {
      case 4:
        return boost::numeric_cast< ToT >( boost::any_cast< float >( v.value_ ) );
      case 8:
        return boost::numeric_cast< ToT >( boost::any_cast< double >( v.value_ ) );
      default:
        break;
      }
    }
    //throw ValueException( v.typeName(), TOOLS_TYPE_NAME( ToT ) );
    return 0;
  }

  template< typename ToT >
  ToT StringIdToScaler( StringId const & str ) {
    // while I like that this will use existing scaler string deserialization, what I don't
    // like is the lack of error information.  For example, if the string does not contain
    // a serialized scaler
    std::istringstream stream( str.c_str() );
    ToT val;
    stream >> val;
    return val;
  }
}; // anonymous namespace

StringId
tools::impl::ValueToStringId( Value const & v )
{
  try {
    return boost::any_cast< StringId >( v.value_ );
  }
  catch( boost::bad_any_cast const & ) {}
  try {
    return boost::any_cast< StringId const >( v.value_ );
  }
  catch( boost::bad_any_cast const & ) {}
  try {
    std::string str( boost::any_cast< std::string >( v.value_ ) );
    return StringId( str );
  }
  catch( boost::bad_any_cast const & ) {}
  try {
    std::string const str( boost::any_cast< std::string const >( v.value_ ) );
    return StringId( str );
  }
  catch( boost::bad_any_cast const & ) {}
  try {
    char * str = boost::any_cast< char * >( v.value_ );
    return StringId( str );
  }
  catch( boost::bad_any_cast const & ) {}
  try {
    char const * str = boost::any_cast< char const * >( v.value_ );
    return StringId( str );
  }
  catch( boost::bad_any_cast const & ) {}
  // TODO: maybe serialize pointers?
  if( v.isInteger() ) {
    if( v.isSigned() ) {
      switch( v.sizeOf() ) {
      case 1:
        {
          sint8 val = boost::any_cast< sint8 >( v.value_ );
          static boost::format fmt( "%d" );
          return StringId( ( fmt % val ).str() );
        }
        break;
      case 2:
        {
          sint16 val = boost::any_cast< sint16 >( v.value_ );
          static boost::format fmt( "%d" );
          return StringId( ( fmt % val ).str() );
        }
        break;
      case 4:
        {
          sint32 val = boost::any_cast< sint32 >( v.value_ );
          static boost::format fmt( "%d" );
          return StringId( ( fmt % val ).str() );
        }
        break;
      case 8:
        {
          sint64 val = boost::any_cast< sint64 >( v.value_ );
          static boost::format fmt( "%lld" );
          return StringId( ( fmt % val ).str() );
        }
        break;
      default:
        //throw ValueException( v.typeName(), TOOLS_TYPE_NAME( StringId ) );
        return StringIdNull();
      }
    } else {
      switch( v.sizeOf() ) {
      case 1:
        {
          uint8 val = boost::any_cast< uint8 >( v.value_ );
          static boost::format fmt( "%u" );
          return StringId( ( fmt % val ).str() );
        }
        break;
      case 2:
        {
          uint16 val = boost::any_cast< uint16 >( v.value_ );
          static boost::format fmt( "%u" );
          return StringId( ( fmt % val ).str() );
        }
        break;
      case 4:
        {
          uint32 val = boost::any_cast< uint32 >( v.value_ );
          static boost::format fmt( "%u" );
          return StringId( ( fmt % val ).str() );
        }
        break;
      case 8:
        {
          uint64 val = boost::any_cast< uint64 >( v.value_ );
          static boost::format fmt( "%llu" );
          return StringId( ( fmt % val ).str() );
        }
        break;
      default:
        //throw ValueException( v.typeName(), TOOLS_TYPE_NAME( StringId ) );
        return StringIdNull();
      }
    }
  } else if( v.isFloat() ) {
    switch( v.sizeOf() ) {
    case 32:
      {
        float val = boost::any_cast< float >( v.value_ );
        static boost::format fmt( "%f" );
        return StringId( ( fmt % val ).str() );
      }
      break;
    case 64:
      {
        double val = boost::any_cast< double >( v.value_ );
        static boost::format fmt( "%lf" );
        return StringId( ( fmt % val ).str() );
      }
      break;
    default:
      //throw ValueException( v.typeName(), TOOLS_TYPE_NAME( StringId ) );
      return StringIdNull();
    }
  }
  // TODO: more string serialization?
  //throw ValueException( v.typeName(), TOOLS_TYPE_NAME( StringId ) );
  return StringIdNull();
}

bool
tools::impl::ValueToBool( Value const & v )
{
  if( v.isInteger() || v.isFloat() ) {
    return ScalerConvert< sint16 >( v ) != 0;
  }
  // TODO: implement this
  tools::StringId valSid = ValueToStringId( v );
  static const tools::StringId oneId( tools::StaticStringId( "1" ) );
  static const tools::StringId zeroId( tools::StaticStringId( "0" ) );
  if( valSid == oneId ) {
    return true;
  } else if( valSid == zeroId ) {
    return false;
  } else if( valSid.compareToIgnoreCase( "true" ) == 0 ) {
    return true;
  } else if( valSid.compareToIgnoreCase( "false" ) == 0 ) {
    return false;
  } else if( valSid.compareToIgnoreCase( "yes" ) == 0 ) {
    return true;
  } else if( valSid.compareToIgnoreCase( "no" ) == 0 ) {
    return false;
  }
  //throw ValueException( v.typeName(), TOOLS_TYPE_NAME( bool ) );
  return false;
}

sint8
tools::impl::ValueToSint8( Value const & v )
{
  if( v.isInteger() || v.isFloat() ) {
    return ScalerConvert< sint8 >( v );
  }
  // we use sint16 to avoid confusion with char
  return boost::numeric_cast< sint8 >( StringIdToScaler< sint16 >( ValueToStringId( v ) ) );
}

sint16
tools::impl::ValueToSint16( Value const & v )
{
  if( v.isInteger() || v.isFloat() ) {
    return ScalerConvert< sint16 >( v );
  }
  return StringIdToScaler< sint16 >( ValueToStringId( v ) );
}

sint32
tools::impl::ValueToSint32( Value const & v )
{
  if( v.isInteger() || v.isFloat() ) {
    return ScalerConvert< sint32 >( v );
  }
  return StringIdToScaler< sint32 >( ValueToStringId( v ) );
}

sint64
tools::impl::ValueToSint64( Value const & v )
{
  if( v.isInteger() || v.isFloat() ) {
    return ScalerConvert< sint64 >( v );
  }
  return StringIdToScaler< sint64 >( ValueToStringId( v ) );
}

uint8
tools::impl::ValueToUint8( Value const & v )
{
  if( v.isInteger() || v.isFloat() ) {
    return ScalerConvert< uint8 >( v );
  }
  // we use uint16 so as not to confuse it with char
  return boost::numeric_cast< uint8 >( StringIdToScaler< uint16 >( ValueToStringId( v ) ) );
}

uint16
tools::impl::ValueToUint16( Value const & v )
{
  if( v.isInteger() || v.isFloat() ) {
    return ScalerConvert< uint16 >( v );
  }
  return StringIdToScaler< uint16 >( ValueToStringId( v ) );
}

uint32
tools::impl::ValueToUint32( Value const & v )
{
  if( v.isInteger() || v.isFloat() ) {
    return ScalerConvert< uint32 >( v );
  }
  return StringIdToScaler< uint32 >( ValueToStringId( v ) );
}

uint64
tools::impl::ValueToUint64( Value const & v )
{
  if( v.isInteger() || v.isFloat() ) {
    return ScalerConvert< uint64 >( v );
  }
  return StringIdToScaler< uint64 >( ValueToStringId( v ) );
}

float
tools::impl::ValueToFloat( Value const & v )
{
  if( v.isInteger() || v.isFloat() ) {
    return ScalerConvert< float >( v );
  }
  return StringIdToScaler< float >( ValueToStringId( v ) );
}

double
tools::impl::ValueToDouble( Value const & v )
{
  if( v.isInteger() || v.isFloat() ) {
    return ScalerConvert< double >( v );
  }
  return StringIdToScaler< double >( ValueToStringId( v ) );
}

// unit tests for Value functionality
#if TOOLS_UNIT_TEST

#endif /* TOOLS_UNIT_TEST != 0 */
