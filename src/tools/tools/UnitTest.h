#pragma once

/* #include <tools/Memory.h> */
#include <tools/String.h>
#include <tools/Tools.h>

#ifdef WINDOWS_PLATFORM
#  pragma warning( disable : 4365 )
#  pragma warning( disable : 4371 )
#  pragma warning( disable : 4548 )
#  pragma warning( disable : 4571 )
#endif // WINDOWS_PLATFORM
#include <boost/format.hpp>
#ifdef WINDOWS_PLATFORM
#  pragma warning( default : 4365 )
#  pragma warning( default : 4371 )
#  pragma warning( default : 4548 )
#  pragma warning( default : 4571 )
#endif // WINDOWS_PLATFORM

namespace tools {
  namespace unittest {
    struct UnitTest;
  }; // namespace unittest
  namespace impl {
    TOOLS_API void RegisterUnitTest( tools::unittest::UnitTest * );
    TOOLS_API void UnregisterUnitTest( tools::unittest::UnitTest * );
    TOOLS_API void UnitTestMain( int, char ** );
  }; // namespace impl
  namespace unittest {
    /// The UnitTest class is created when we define a new unit test class/
    /// method pair or a new unit test function in code.  It gets called when
    /// the UnitTestMain function is called, i.e.: by UnitTestRunner.
    struct UnitTest
    {
      /// Register this test in the global collection of all tests on
      /// construction.  This way, static initiailization can register all
      /// available tests automatically.
      ///
      /// \param name The name of the test.
      /// \param file The file in which the test was defined.
      UnitTest( StringId const & name, StringId const & file )
        : name_( name )
        , file_( file )
      {
        tools::impl::RegisterUnitTest( this );
      }

      /// Remove ourself from the collection of unit tests.
      virtual ~UnitTest( void )
      {
        tools::impl::UnregisterUnitTest( this );
      }

      /// By default, asserts as it should never get to the base class
      /// implementation.
      virtual void run( void )
      {
        TOOLS_ASSERT( !"Should not run UnitTest::run()" );
      }

      /// Return the name of the test.
      /// \return The name of the test.
      StringId const & name( void ) { return name_; }

      /// Returns the file in which the test was defined.
      /// \return The file in which the test was defined.
      StringId const & file( void ) { return file_; }
    private:
      StringId name_, file_;
    };

    /// FuncUnitTest will call a unit test function of the form:
    ///   void SomeFunction()
    template< void (*FuncT)( void ) >
    struct FuncUnitTest
      : public UnitTest
    {
      FuncUnitTest( StringId name, StringId file )
        : UnitTest( name, file )
      {}
      void run( void )
      {
	printf("Running function test %s (%s)\n", name().c_str(), file().c_str());
        (*FuncT)();
      }
    };

    /// MethodUnitTest will call a unit test class method of the form:
    ///   void classInstance.someMethod()
    template< typename ClassT, void (ClassT::*FuncT)( void ) >
    class MethodUnitTest
      : public UnitTest
    {
    public:
      MethodUnitTest( StringId name, StringId file )
        : UnitTest( name, file )
      {}
      void run( void )
      {
	printf("Running method test %s (%s)\n", name().c_str(), file().c_str());
        ClassT instance;
        ( instance.*FuncT )();
      }
    };

    /// Helps provides a unique name for each unit test
    struct TestNameHelper
    {
      // Guarantees that each time this function is called returns a unique
      // name.
      static StringId UniqueName( StringId const & name )
      {
        static uint32 count = 1;
        static boost::format fmt( "%s_%d" );
        fmt % name % count;
        return StringId( fmt.str() );
      }
    };

    /// A static initialized factory will be created each time a unit test
    /// needs to be initialized.  This is neccessary for the macros to work
    /// as expected to initialize function unit tests.
    template< typename ImplT, void (*FuncT)( void ) >
    struct FuncTestFactory
    {
      static void createTest( void )
      {
        StringId name = TestNameHelper::UniqueName( ImplT::Name() );
        new FuncUnitTest< FuncT >( name, ImplT::File() );
      }
    };

    /// A factory for method unit tests
    template< typename ClassT, void (ClassT::*FuncT)( void ), typename ImplT >
    struct MethodTestFactory
    {
      typedef MethodUnitTest< ClassT, FuncT > LocalType;
      static void createTest( void )
      {
        StringId name = TestNameHelper::UniqueName( ImplT::Name() );
        new LocalType( name, ImplT::File() );
      }
    };
  }; // namespace unittest
}; // namespace tools

/// \def TOOLS_UNIT_TEST_FUNCTION
/// Creates a unit test to call a test function
/// \param func The function to be run.
#define TOOLS_UNIT_TEST_FUNCTION( func ) \
namespace { \
  template< int line > \
  struct FuncFactoryImpl ## func; \
  template<> \
  struct FuncFactoryImpl ## func< __LINE__ > \
  { \
    typedef FuncFactoryImpl ## func< __LINE__ > Type; \
    FuncFactoryImpl ## func( void ) { tools::unittest::FuncTestFactory< Type, &  func >::createTest(); } \
    static tools::StringId Name( void ) { return tools::StringId( #func ); } \
    static tools::StringId File( void ) { return tools::StringId( __FILE__ ); } \
    static Type factory_; \
  }; \
  FuncFactoryImpl ## func< __LINE__ > FuncFactoryImpl ## func< __LINE__ >::factory_; \
}; // anonymous namespace

/// \def TOOLS_UNIT_TEST_METHOD
/// Creates a unit test to call a test method.  The test class will be
/// constructed and destructed for each test method.
///
/// \param cls The class wihch contains the unit test method.
/// \param mth The method within the class which is to be called.
#define TOOLS_UNIT_TEST_METHOD( cls, mth ) \
namespace { \
  template< int line > \
  struct MethFactoryImpl ## cls ## mth; \
  template<> \
  struct MethFactoryImpl ## cls ## mth< __LINE__ > \
  { \
    typedef MethFactoryImpl ## cls ## mth< __LINE__ > Type; \
    MethFactoryImpl ## cls ## mth( void ) { tools::unittest::MethodTestFactory< cls, & cls :: mth, Type >::createTest(); } \
    static tools::StringId Name( void ) { return tools::StringId( #cls "::" #mth ); } \
    static tools::StringId File( void ) { return tools::StringId( __FILE__ ); } \
    static Type factory_; \
  }; \
  MethFactoryImpl ## cls ## mth< __LINE__ > MethFactoryImpl ## cls ## mth< __LINE__ >::factory_; \
}; // anonymous namespace
