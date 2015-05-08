#pragma once

///
// \mainpage
//
// This is the Tools library of Frang's codebase.  It serves as the foundation
// for all of the other libraries.  Included are utility classes and functions
// that help to standardize programming under various environments.  Classes
// and functions in Tools are all under the tools namespace.
//
// Tools modules can be found under the Modules tab.
//
// \file Tools.h
// \brief The front page for the tools api documentation.
//
// This file contains defines and macros to be used both by tools libraries and
// other projects.

///
// \addtogroup ToolsStuff Basic System Types
// @(
// \def TOOLS_API
//
// \brief Defines a function as being exported from the tools DLL

#ifdef WINDOWS_PLATFORM
  #ifndef TOOLS_API
    #define TOOLS_API __declspec(dllimport)
  #endif // TOOLS_API
  #pragma warning( disable : 4200 )
#else // WINDOWS_PLATFORM
  #define TOOLS_API
#endif // WINDOWS_PLATFORM

// stdint.h contains [u]int8_t, [u]int16_t, [u]int32_t, [u]int64_t, [u]intptr_t.  Some other stuff, but
// it's not too bad.
#define __STDC_LIMIT_MACROS
#include <stdint.h>

// stddef.h contains size_t, ptrdiff_t, and wchar_t.  Some other stuff, but it's not too bad
#include <stddef.h>

#ifdef WINDOWS_PLATFORM
#  ifdef _M_IX86
#    define TOOLS_ARCH_X86
#  elif defined(_M_X64)
#    define TOOLS_ARCH_X86
#  else // _M_IX86
#    define TOOLS_ARCH_UNKNOWN
#  endif // _M_IX86
#  pragma warning(disable:4200)  // expansion arrays
#  pragma warning(disable:4355)  // 'this' used in member initialization
#  ifdef TOOLS_DEBUG
#    define TOOLS_DEBUG_OPT_BEGIN __pragma(runtime_checks("", off))
#    define TOOLS_DEBUG_OPT_END __pragma(runtime_checks("", restore))
#    define TOOLS_DEBUG_OPT_LIB __pragma(optimize("gs", on)) __pragma(runtime_checks("", off))
#    define TOOLS_DEBUG_OPT_LIB_BEGIN __pragma(optimize("gs", on)) __pragma(runtime_checks("", off))
#    define TOOLS_DEBUG_OPT_LIB_END __pragma(optimize("", on)) __pragma(runtime_checks("", restore))
#    pragma pointers_to_members( full_generality, multiple_inheritance )
#  endif // TOOLS_DEBUG
#  define TOOLS_NO_RETURN
#  define TOOLS_FORCE_INLINE __forceinline
#  define TOOLS_NO_INLINE __declspec(noinline)
// TOOLS_PURE_INLINE is a function that only depends on it's arguments and never accesses global memory.
// This means that there is no need to update global memory and/or relead after calling it.
#  define TOOLS_PURE_INLINE __declspec(noalias)
#  define TOOLS_LIKELY(x) (x)
#  define TOOLS_UNLIKELY(x) (x)
#  define TOOLS_WARNINGS_SAVE
#  define TOOLS_WARNINGS_DISABLE_UNINITIALIZED
#  define TOOLS_WARNINGS_DISABLE_UNUSED_TYPEDEF
#  define TOOLS_WARNINGS_DISABLE_PARENTHESES
#  define TOOLS_WARNINGS_RESTORE
#endif // WINDOWS_PLATFORM
#ifdef UNIX_PLATFORM
#  ifdef __GNUC__
#    ifdef __i386__
#      define TOOLS_ARCH_X86
#    else // __i386__
#      define TOOLS_ARCH_UNKNOWN
#    endif // __i386__
#  else // __GNUC__
#    define TOOLS_ARCH_UNKNOWN
#  endif // __GNUC__
#  ifdef TOOLS_DEBUG
#    define TOOLS_DEBUG_OPT_BEGIN
#    define TOOLS_DEBUG_OPT_END
#    define TOOLS_DEBUG_OPT_LIB _Pragma("GCC optimize 2")
#    define TOOLS_DEBUG_OPT_LIB_BEGIN
#    define TOOLS_DEBUG_OPT_LIB_END
#  endif // TOOLS_DEBUG
#  define TOOLS_NO_RETURN __attribute__ ((__noreturn__))
#  define TOOLS_FORCE_INLINE __attribute__((always_inline)) inline
#  define TOOLS_NO_INLINE __attribute__((noinline))
// TOOLS_PURE_INLINE is a function that only depends on it's arguments and never accesses global memory.
// This means that there is no need to update global memory and/or relead after calling it.
#  define TOOLS_PURE_INLINE __attribute__((const))
#  define TOOLS_LIKELY(x) __builtin_expect((x), true)
#  define TOOLS_UNLIKELY(x) __builtin_expect((x), false)
#  define TOOLS_WARNINGS_SAVE _Pragma("GCC diagnostic push")
#  define TOOLS_WARNINGS_DISABLE_UNINITIALIZED _Pragma("GCC diagnostic ignored \"-Wuninitialized\"") \
    _Pragma("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
#  define TOOLS_WARNINGS_DISABLE_UNUSED_TYPEDEF _Pragma("GCC diagnostic ignored \"-Wunused-local-typedefs\"")
#  define TOOLS_WARNINGS_DISABLE_PARENTHESES _Pragma("GCC diagnostic ignored \"-Wparentheses\"")
#  define TOOLS_WARNINGS_RESTORE _Pragma("GCC diagnostic pop")
#endif // UNIX_PLATFORM

/// Namespace 'tools' contains basic functionality for things like threads,
// subscriptions, requests, etc.

namespace tools {
  ///
  // \name "Fundamental Types"
  // \addtogroup ToolsStuff
  // @(

  /// \brief Syntactic sugar for unsigned char
  typedef unsigned char uchar;

  /// \brief Guarenteed to be 1 byte on all platforms
  typedef uint8_t uint8;

  /// \brief Guarenteed to be 1 byte on all platforms
  typedef uint8 Byte;

  /// \brief Guarenteed to be 1 byte on all platforms
  typedef int8_t sint8;

  /// \brief Guarenteed to be 2 bytes on all platforms
  typedef uint16_t uint16;

  /// \brief Guarenteed to be 2 bytes on all platforms
  typedef int16_t sint16;

  /// \brief Guarenteed to be 4 bytes on all platforms
  typedef uint32_t uint32;

  /// \brief Guarenteed to be 4 bytes on all platforms
  typedef int32_t sint32;

  /// \brief Guarenteed to be 8 bytes on all platforms
  typedef uint64_t uint64;

  /// \brief Guarenteed to be 8 bytes on all platforms
  typedef int64_t sint64;

  // @)

  /// \name "Basic Types"
  // @(

  typedef double Seconds;
  typedef double Meters;
  typedef double Degrees;
  typedef double Radians;
  typedef double Hz;
  typedef sint32 Pixels;
  /// \brief Able to count all the bytes in a 32-bit address space.
  typedef uint32 Bytes;

  typedef uint32 Index;
  typedef uint32 Elements;

  // @)

#ifndef DOXYGEN_SKIP
  #ifndef TOOLS_RELEASE
    #define TOOLS_FILE_AND_LINE_VALS __FILE__, __LINE__
    #define TOOLS_FILE_AND_LINE_DECL char const * file, int line
    #define TOOLS_FILE_AND_LINE_CALL ( TOOLS_FILE_AND_LINE_VALS )
    #define TOOLS_FILE_AND_LINE_PASS file, line
    #define TOOLS_FILE_AND_LINE_SEP ,
  #else // TOOLS_RELEASE
    #define TOOLS_FILE_AND_LINE_VALS
    #define TOOLS_FILE_AND_LINE_DECL
    #define TOOLS_FILE_AND_LINE_CALL
    #define TOOLS_FILE_AND_LINE_PASS
    #define TOOLS_FILE_AND_LINE_SEP
  #endif // TOOLS_RELEASE
  #define TOOLS_FILE_AND_LINE_DECL_ TOOLS_FILE_AND_LINE_DECL TOOLS_FILE_AND_LINE_SEP
  #define TOOLS_FILE_AND_LINE_CALL_ TOOLS_FILE_AND_LINE_VALS TOOLS_FILE_AND_LINE_SEP
  #define TOOLS_FILE_AND_LINE_PASS_ TOOLS_FILE_AND_LINE_PASS TOOLS_FILE_AND_LINE_SEP
  #define TOOLS__FILE_AND_LINE_DECL TOOLS_FILE_AND_LINE_SEP TOOLS_FILE_AND_LINE_DECL
  #define TOOLS__FILE_AND_LINE_CALL TOOLS_FILE_AND_LINE_SEP TOOLS_FILE_AND_LINE_VALS
  #define TOOLS__FILE_AND_LINE_PASS TOOLS_FILE_AND_LINE_SEP TOOLS_FILE_AND_LINE_PASS

  ///
  // Concatenate two words together, result is still considered a single token.
  #define TOOLS_CAT(a,b) a##b

  ///
  // This fools the compiler into expanding __LINE__ before token pasting.
  #define TOOLS_CAT_LINE_OK(a,b) TOOLS_CAT(a,b)

  ///
  // This is used to make the preprocessor do the right thing with __FILE__ and friends.
  // Ultimately TOOLS_WIDEN(x) will take a string and make it a wide-char string.
  #define TOOLS_WIDEN_AUX(x) L ## x
  #define TOOLS_WIDEN(x) TOOLS_WIDEN_AUX(x)

  ///
  // This yields a wide-char string version of __FILE__.
  #define TOOLS_WFILE TOOLS_WIDEN(__FILE__)

  #ifndef TOOLS_RELEASE
    #define TOOLS_WFILE_AND_LINE_VALS TOOLS_FILE, __LINE__
    #define TOOLS_WFILE_AND_LINE_DECL wchar_t const * file, int line
    #define TOOLS_WFILE_AND_LINE_CALL ( TOOLS_WFILE_AND_LINE_VALS )
    #define TOOLS_WFILE_AND_LINE_PASS file, line
    #define TOOLS_WFILE_AND_LINE_SEP ,
  #else // TOOLS_RELEASE
    #define TOOLS_WFILE_AND_LINE_VALS
    #define TOOLS_WFILE_AND_LINE_DECL
    #define TOOLS_WFILE_AND_LINE_CALL
    #define TOOLS_WFILE_AND_LINE_PASS
    #define TOOLS_WFILE_AND_LINE_SEP
  #endif // TOOLS_RELEASE
  #define TOOLS_WFILE_AND_LINE_DECL_ TOOLS_WFILE_AND_LINE_DECL TOOLS_WFILE_AND_LINE_SEP
  #define TOOLS_WFILE_AND_LINE_CALL_ TOOLS_WFILE_AND_LINE_VALS TOOLS_WFILE_AND_LINE_SEP
  #define TOOLS_WFILE_AND_LINE_PASS_ TOOLS_WFILE_AND_LINE_PASS TOOLS_WFILE_AND_LINE_SEP
  #define TOOLS__WFILE_AND_LINE_DECL TOOLS_WFILE_AND_LINE_SEP TOOLS_WFILE_AND_LINE_DECL
  #define TOOLS__WFILE_AND_LINE_CALL TOOLS_WFILE_AND_LINE_SEP TOOLS_WFILE_AND_LINE_VALS
  #define TOOLS__WFILE_AND_LINE_PASS TOOLS_WFILE_AND_LINE_SEP TOOLS_WFILE_AND_LINE_PASS
#endif // DOXYGEN_SKIP

  /// \name "Assert functionality"
  // @(

  ///
  // Handles stopping the program in case of a serious failure.  Ideally we
  // should generate as much information as possible in a stack trace for
  // debugging review.
  //
  // \param assertion The assert which failed.
  // \param file The file which contains the code that asserted
  // \param line The line of the file in which the code asserted
  // \return 1 if the user disabled this assert, 0 if not

  extern "C" TOOLS_API int ToolsHandleAssertFailure( char const * assertion, char const *, int );

  ///
  // \def TOOLS_ASSERT(exp)
  // Assert only in debug or QA mode.  Otherwise it does nothing and code
  // execution continues.
  //
  // \param exp The expression that must be true, otherwise assert.

  ///
  // \def TOOLS_ASSERTD(exp)
  // Assert only in debug mode.  Otherwise does nothing and code execution
  // continues.
  //
  // \param exp The expression that must be true, otherwise assert.

  ///
  // \def TOOLS_ASSERTR(exp)
  // Assert in debug, QA, and release modes.
  //
  // \param exp The expression that must be true, otherwise assert.

  ///
  // \def TOOLS_ASSERTMSG(exp,str)
  // TODO: document this
  //
  // \param exp The expression that must be true, otherwise we assert
  // \param str The string to display if we assert

#ifndef TOOLS_NO_ASSERT_MACROS
  #ifdef TOOLS_ASSERT
    #undef TOOLS_ASSERT
  #endif // TOOLS_ASSERT
  #ifdef TOOLS_ASSERTD
    #undef TOOLS_ASSERTD
  #endif // TOOLS_ASSERTD
  #ifdef TOOLS_ASSERTR
    #undef TOOLS_ASSERTR
  #endif // TOOLS_ASSERTR
  #ifdef TOOLS_ASSERTMSG
    #undef TOOLS_ASSERTMSG
  #endif // TOOLS_ASSERTMSG

  #ifdef TOOLS_RELEASE
    #define TOOLS_ASSERT(exp)  ((void)0)
    #define TOOLS_ASSERTD(exp) ((void)0)
    #define TOOLS_ASSERTR(exp) if(exp);else((void)tools::ToolsHandleAssertFailure( #exp, __FILE__, __LINE__ ))
    #define TOOLS_ASSERTMSG(exp,str) ((void)0)
  #else // TOOLS_RELEASE
    #ifndef DOXYGEN_SKIP
      #define TOOLS_ASSERT(exp) do { \
  if( exp ); else { \
    (void)tools::ToolsHandleAssertFailure( #exp TOOLS__FILE_AND_LINE_CALL ); \
  } \
} while( 0 )
      #define TOOLS_ASSERTMSG(exp,str) do { \
  if( exp ); else { \
    (void)tools::ToolsHandleAssertFailure( #exp TOOLS__FILE_AND_LINE_CALL ); \
  } \
} while( 0 )
    #else // DOXYGEN_SKIP
      #define TOOLS_ASSERT(exp) /* ... */
      #define TOOLS_ASSERTMSG(exp,str) /* ... */
    #endif // DOXYGEN_SKIP
    #ifdef NDEBUG
      #define TOOLS_ASSERTD(exp) ((void)0)
    #else // NDEBUG
      #define TOOLS_ASSERTD(exp) TOOLS_ASSERT(exp)
    #endif // NDEBUG
    #define TOOLS_ASSERTR(exp) TOOLS_ASSERT(exp)
  #endif // TOOLS_RELEASE
#endif // TOOLS_NO_ASSERT_MACROS

  // @)

#define TOOLS_UNIQUE_LOCAL(prefix) TOOLS_CAT_LINE_OK(prefix,__LINE__)

  struct Build
  {
      // Should always be a const expression
#ifdef TOOLS_DEBUG
      static const bool isDebug_ = true;
#else // TOOLS_DEBUG
      static const bool isDebug_ = false;
#endif // TOOLS_DEBUG
  };

#ifndef DOXYGEN_SKIP
  class StringId;   // predeclare this to make some includes simplier
  struct Request;   // predeclare this to make some includes simplier
  struct Generator; // predeclare this to make some includes simplier
#endif // DOXYGEN_SKIP
  namespace impl
  {
        TOOLS_API bool isAbnormalShutdown( void );
  };  // impl namespace
}; // namespace tools

//! @)
