#include "toolsprecompiled.h"

// #include <tools/Configuration.h>
// #include <tools/Notification.h>
#include <tools/UnitTest.h>

#include <iterator>
#include <vector>

#ifdef WINDOWS_PLATFORM
#  pragma warning( disable : 4365 )
#  pragma warning( disable : 4619 )
#endif // WINDOWS_PLATFORM
#include <boost/format.hpp>
#include <boost/regex.hpp>
#ifdef WINDOWS_PLATFORM
#  pragma warning( default : 4365 )
#  pragma warning( default : 4619 )
#endif // WINDOWS_PLATFORM

using namespace tools;

typedef std::vector< unittest::UnitTest * > UnitTests;

// CONFIGURE_STATIC( UnitTestConfig );
// CONFIGURE_FUNCTION( UnitTestConfig ) {
// }

namespace {
  static UnitTests & GetUnitTests( void )
  {
    static UnitTests collection_;
    return collection_;
  }

  // static notification::Category unitCat( StaticStringId( L"UnitTest" ), StringIdNull() );
};  // anonymous namespace

void
impl::RegisterUnitTest( unittest::UnitTest * test )
{
  GetUnitTests().push_back( test );
}

void
impl::UnregisterUnitTest( unittest::UnitTest * test )
{
  UnitTests & collection( GetUnitTests() );
  for( UnitTests::iterator i=collection.begin(); i!=collection.end(); ++i ) {
    if( *i == test ) {
      *i = collection.back();
      collection.pop_back();
      return;
    }
  }
}

void
impl::UnitTestMain( int, char ** )
{
  // if( UnitTestConfig.defined( L"help" ) ) {
  //   boost::wformat fmt( L"%s [-help] [-verbose 0/1] [-repeat n] [-runTest name]*" );
  //   // TODO: replace the application name below
  //   NOTIFY_INFO( unitCat ) << ( fmt % L"UnitTestRunner" ).str() << std::endl;
  // }
  // tools::PreloadLibraries();
  // bool verbose( UnitTestConfig.get< bool >( L"verbose" ) );
  // uint32 count( UnitTestConfig.get< uint32 >( L"repeat", 1 ) );
  //bool verbose = false;
  uint32 count = 1;
  typedef std::vector< boost::regex > Patterns;
  Patterns patterns;
  {
    // AutoDispose< StringIdIterator > runIter( UnitTestConfig.getAll< StringId >( L"runTest" ) );
    // StringId * sVal;
    // while( ( sVal = runIter->next() ) != nullptr ) {
    //   patterns.push_back( boost::wregex( sVal->c_str() ) );
    // }
  }
  // TODO: put this back when I make thread context
  // AutoDispose< Context > context( NewThreadContext( alloca( ContextGetThreadAllocationSize() ) ) );
  boost::format nameFmt( "%s:%s" );
  UnitTests & collection( GetUnitTests() );
  for( UnitTests::iterator i=collection.begin(); i!=collection.end(); ++i ) {
    // Check against patterns
    bool matched( false );
    if( !patterns.empty() ) {
      for( Patterns::iterator pi=patterns.begin(); pi!=patterns.end(); ++pi ) {
            nameFmt % (*i)->file() % (*i)->name();
            StringId nameId( nameFmt.str() );
#ifdef WINDOWS_PLATFORM
            // TODO: flip backslash to slash in file part.  Better yet, do this
            // when the flie part is assigned.
#endif /* WINDOWS_PLATFORM */
            if( regex_match( nameId.c_str(), *pi ) ) {
              matched = true;
              break;  // only need one
            }
      }
    } else {
      matched = true;  // no patterns = always match.
    }
    if( matched ) {
      for( uint32 c=0; c<count; ++c ) {
          //   if( verbose ) {
          // NOTIFY_INFO( unitCat ) << L"Running test " << (*i)->file() << L":" << (*i)->name() << L"..." << std::endl;
          //   }
            (*i)->run();
            // context->clean();
      }
    }
  }
  // if( verbose ) {
  //   NOTIFY_INFO( unitCat ) << L"Finished running unit tests." << std::endl;
  // }
  patterns.clear();
}

// Do we have unit tests for the unit test framework?  Of course we do!
// These tests assume that tests will be run in the order listed in this file,
// and that all of the tests will be run.  If either of those stops being the
// case, these tests will need to be changed.
#if TOOLS_UNIT_TEST

void
NoMacroFuncTest( void )
{
  bool test( true );
  TOOLS_ASSERTR( test );
}

namespace {
 template< int line >
 struct FuncFactoryImplNoMacroFuncTest;
 template<>
 struct FuncFactoryImplNoMacroFuncTest< 117 > {
   typedef FuncFactoryImplNoMacroFuncTest< 117 > Type;
   FuncFactoryImplNoMacroFuncTest( void ) { tools::unittest::FuncTestFactory< Type, &NoMacroFuncTest >::createTest(); }
   static tools::StringId Name( void ) { return tools::StringId( "NoMacroFuncTest" ); }
   static tools::StringId File( void ) { return tools::StringId( ".\\UnitTest.cpp" ); }
   static Type factory_;
 };
 FuncFactoryImplNoMacroFuncTest< 117 > FuncFactoryImplNoMacroFuncTest< 117 >::factory_;
};

struct NoMacroTest
{
  NoMacroTest( void ) {}
  ~NoMacroTest( void ) {}

  void test( void ) { bool t( true ); TOOLS_ASSERTR( t ); }
};

namespace {
  template< int line >
  struct MethFactoryImplNoMacroTesttest;
  template<>
  struct MethFactoryImplNoMacroTesttest< 139 > {
    typedef MethFactoryImplNoMacroTesttest< 139 > Type;
    MethFactoryImplNoMacroTesttest( void ) { tools::unittest::MethodTestFactory< NoMacroTest, &NoMacroTest::test, Type >::createTest(); }
    static tools::StringId Name( void ) { return tools::StringId( "NoMacroTest" "::" "test" ); }
    static tools::StringId File( void ) { return tools::StringId( ".\\UnitTest.cpp" ); }
    static Type factory_;
  };
  MethFactoryImplNoMacroTesttest< 139 > MethFactoryImplNoMacroTesttest< 139 >::factory_;
};

static std::string &
GetTestString( void )
{
  static std::string ret;
  return ret;
}

void
TestBegin( void )
{
  GetTestString() = "B";
}

TOOLS_UNIT_TEST_FUNCTION( TestBegin );

struct TestClass
{
  TestClass( void ) { GetTestString() += "F"; }
  ~TestClass( void ) { GetTestString() += "f"; }

  void test1( void ) { GetTestString() += "1"; }
  void test2( void ) { GetTestString() += "2"; }
};

TOOLS_UNIT_TEST_METHOD( TestClass, test1 );
TOOLS_UNIT_TEST_METHOD( TestClass, test2 );

void
TestEnd( void )
{
  TOOLS_ASSERTR( GetTestString() == "BF1fF2f" );
}

TOOLS_UNIT_TEST_FUNCTION( TestEnd );

void
MultiTestBegin( void )
{
  GetTestString() = "B";
}

TOOLS_UNIT_TEST_FUNCTION( MultiTestBegin );

void
MultiTest( void )
{
  GetTestString() += "t";
}

TOOLS_UNIT_TEST_FUNCTION( MultiTest );
TOOLS_UNIT_TEST_FUNCTION( MultiTest );
TOOLS_UNIT_TEST_FUNCTION( MultiTest );
TOOLS_UNIT_TEST_FUNCTION( MultiTest );

void
MultiTestEnd( void )
{
  TOOLS_ASSERTR( GetTestString() == "Btttt" );
}

TOOLS_UNIT_TEST_FUNCTION( MultiTestEnd );

#endif /* TOOLS_UNIT_TEST != 0 */
