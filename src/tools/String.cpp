#include "toolsprecompiled.h"

// #include <tools/Buffer.h>
#include <tools/Concurrency.h>
// #include <tools/Helpers.h>
// #include <tools/Memory.h>
#include <tools/String.h>
#include <tools/Tools.h>
#include <tools/WeakPointer.h>

#include <boost/numeric/conversion/cast.hpp>

#include <stdlib.h>
#include <wctype.h>

#include <unordered_set>

using namespace tools;
using boost::numeric_cast;

namespace {
    static uint64 volatile totalStringIds_ = 0ULL;
    static uint64 volatile totalStaticStringIds_ = 0ULL;
    static uint64 volatile totalStringBytes_ = 0ULL;
};  // anonymous namespace

namespace tools {
    namespace impl {
        void stringIdGetMemoryTracking(
            uint64 & retSidCount,
            uint64 & retStaticSidCount,
            uint64 & retSidBytes )
        {
            retSidCount = atomicRead( &totalStringIds_ );
            retStaticSidCount = atomicRead( &totalStaticStringIds_ );
            retSidBytes = atomicRead( &totalStringBytes_ );
        }
    };  // impl namespace
};  // tools namespace

namespace std {
    template<>
    struct hash< tools::impl::StringIdData * >
    {
        size_t operator()( tools::impl::StringIdData * ptr ) const
        {
            return ptr->hash_;
        }
    };

    template<>
    struct equal_to< tools::impl::StringIdData * >
    {
        bool operator()( tools::impl::StringIdData * ptr1, tools::impl::StringIdData * ptr2 ) const
        {
			bool ret = ( ptr1 == ptr2 );
            if( ret ) { return ret; }
			return ( strcmp( ptr1->string_, ptr2->string_ ) == 0 );
        }
    };
}; // namespace std

typedef std::unordered_set< tools::impl::StringIdData * > StringTable;

namespace {
    static StringTable & GetStringTable( void ) throw()
    {
        static StringTable * table = new StringTable;
        return *table;
    }

    struct RealStringId : tools::impl::StringIdData
    {
        typedef StringTable::iterator Iterator;
        bool isStatic_;
        uint32 refs_;

		RealStringId( void ) = delete;
        RealStringId( char const * str, size_t hsh, size_t len, bool isStatic )
            : tools::impl::StringIdData( str, hsh, len )
            , isStatic_( isStatic )
            , refs_( 0 )
        {}
    };

    inline char const * CopyStringContents( char const * inStr, uint32 copyChars )
    {
        char * ret = new char[ copyChars + 1 ];
        memcpy( ret, inStr, copyChars * sizeof( char ) );
        ret[ copyChars ] = '\0';
        return ret;
    }

    inline size_t StringHash( char const * str, uint32 size )
    {
        size_t ret = 0;
        char const * end = str + size;
        for( ; str != end; ++str ) {
            ret = ret ^ ( ( ret << 10 ) + ( ret >> 3 ) + *str );
        }
        return ret;
    }

    inline RealStringId * CreateRecord( char const * str, uint32 len, uint32 alloc, bool isStatic )
    {
        size_t hsh = StringHash( str, alloc-1 );
        RealStringId * ret = new RealStringId( str, hsh, len, isStatic );
        return ret;
    }

    inline void DestroyRecord( RealStringId * sid )
    {
        if( !( sid->isStatic_ ) ) {
            delete [] sid->string_;
        }
        delete sid;
    }

    inline void AddToTable( RealStringId * sid )
    {
        // TODO: get a lock for the table
        auto resp = GetStringTable().insert( sid );
        TOOLS_ASSERT( resp.second );
    }

    inline void RemoveFromTable( RealStringId * sid )
    {
        // TODO: get a lock for the table
        TOOLS_ASSERT( sid->refs_ == 0 );
        GetStringTable().erase( sid );
    }

	struct NewStringIdData
		: tools::impl::StringIdData
	{
		bool isStatic_;

		NewStringIdData( void ) = delete;
		TOOLS_FORCE_INLINE NewStringIdData( char const * str, size_t hsh, size_t len, bool stat )
			: tools::impl::StringIdData( str, hsh, len )
			, isStatic_( stat )
		{}
		TOOLS_FORCE_INLINE NewStringIdData( tools::impl::StringIdData const & data, bool stat )
			: tools::impl::StringIdData( data )
			, isStatic_( stat )
		{}
		TOOLS_FORCE_INLINE bool operator==( NewStringIdData const & r ) const {
			return tools::impl::StringIdData::operator==( r ) & ( isStatic_ == r.isStatic_ );
		}
	};

	struct NewRealStringId
		: StandardIsReferenced< NewRealStringId, NewStringIdData, AllocStatic< Platform >>
	{};

	struct StringPhantom
		: StandardPhantomSlistElement< StringPhantom, StandardPhantom< StringPhantom, AllocStatic< Platform >>>
	{
		AutoDispose<NewRealStringId> data_;
	};

	TOOLS_FORCE_INLINE tools::impl::StringIdData const &
	keyOf(StringPhantom const & v)
	{
		return *v.data_;
	}

	TOOLS_FORCE_INLINE uint32 defineHashAny(NewStringIdData const & nsid, uint32)
	{
		return static_cast<uint32>(nsid.hash_ & 0xFFFFFFFF);
	}

	typedef PhantomHashMap< StringPhantom, tools::impl::StringIdData, PhantomUniversal > NewStringTable;

	static TOOLS_FORCE_INLINE NewStringTable & GetNewStringTable(void) throw()
	{
		static NewStringTable * table = new NewStringTable;
		return *table;
	}
}; // anonymous namespace

void
StringId::fillInStringId( char const * inStr, sint32 count ) throw()
{
    if( !inStr ) {
        data_ = nullptr;
        return;
    }
    size_t len;
    if( count == -1 ) {
        len = strlen( inStr );
    } else {
        TOOLS_ASSERT( count > 0 );
        len = numeric_cast<uint32>(count);
    }
    RealStringId * sid = CreateRecord( CopyStringContents( inStr, numeric_cast<uint32>(len) ), numeric_cast<uint32>(len), numeric_cast<uint32>(len+1), false );
    StringTable::iterator i = GetStringTable().find( sid );
    if( i == GetStringTable().end() ) {
        AddToTable( sid );
        atomicIncrement( &totalStringIds_ );
        atomicAdd( &totalStringBytes_, sid->length_ );
        data_ = static_cast< tools::impl::StringIdData * >( sid );
    } else {
        data_ = *i;
        DestroyRecord( sid );
    }
    atomicIncrement( &static_cast< RealStringId * >( data_ )->refs_ );
}

StringId::StringId( StringId const & c ) throw()
{
    data_ = c.data_;
    if( !!data_ ) {
        atomicIncrement( &static_cast< RealStringId * >( data_ )->refs_ );
    }
}

StringId::~StringId( void ) throw()
{
    if( !!data_ ) {
        if( !atomicDecrement( &static_cast< RealStringId * >( data_ )->refs_ )) {
            RemoveFromTable( static_cast< RealStringId * >( data_ ));
            if( static_cast< RealStringId * >( data_ )->isStatic_ ) {
                atomicDecrement( &totalStaticStringIds_ );
            } else {
                atomicDecrement( &totalStringIds_ );
                atomicSubtract( &totalStringBytes_, static_cast< RealStringId * >( data_ )->length_ );
            }
            TOOLS_ASSERT( static_cast< RealStringId * >( data_ )->refs_ == 0 );  // make sure no one has refed it up again
            DestroyRecord( static_cast< RealStringId * >( data_ ) );
        }
    }
    data_ = nullptr;
}

StringId & StringId::copy( StringId const & c )
{
    if( !!data_ ) {
        if( !atomicDecrement( &static_cast< RealStringId * >( data_ )->refs_ )) {
            RemoveFromTable( static_cast< RealStringId * >( data_ ));
            if( static_cast< RealStringId * >( data_ )->isStatic_ ) {
                atomicDecrement( &totalStaticStringIds_ );
            } else {
                atomicDecrement( &totalStringIds_ );
                atomicSubtract( &totalStringBytes_, static_cast< RealStringId * >( data_ )->length_ );
            }
            TOOLS_ASSERT( static_cast< RealStringId * >( data_ )->refs_ == 0 );  // make sure no one has refed it up again
            DestroyRecord( static_cast< RealStringId * >( data_ ) );
        }
    }
    data_ = c.data_;
    if( !!data_ ) {
        atomicIncrement( &static_cast< RealStringId * >( data_ )->refs_ );
    }
    return *this;
}

StringId const & tools::StringIdNull( void )
{
    static const StringId ret;
    return ret;
}

StringId const & tools::StringIdEmpty( void )
{
    static const StringId ret( "" );
    return ret;
}

StringId const & tools::StringIdWhitespace( void )
{
    static const StringId ret( " \f\n\r\t" );
    return ret;
}

bool tools::IsNullOrEmptyStringId( StringId const & str )
{
    // cache these to remove a couple function calls.
    static const StringId nullId( StringIdNull() );
    static const StringId emptyId( StringIdEmpty() );

    return ( str == nullId ) || ( str == emptyId );
}

// StringId tools::Widen( char const * str, sint32 count )
// {
//   if( !str ) {
//     return StringIdNull();
//   }
//   Bytes len;
//   if( count < 0 ) {
//     len = strlen( str );
//   } else {
//     // check that there are no imbeded nullptrs
//     char const * end = str + count;
//     char const * c;
//     for( c=str; c!=end; ++c ) {
//       if( *c == '\0' ) {
//         break;
//       }
//     }
//     len = c - str;
//   }
//   if( len == 0 ) {
//     return StringIdEmpty();
//   }
//   AutoRelease< Buffer > source( NewSimpleBuffer( TOOLS_ALLOCATION_DESC_NONLEAKY ) );
//   memcpy( source->extend( len ), str, len );
//   AutoRelease< Buffer > destination( NewSimpleBuffer( TOOLS_ALLOCATION_DESC_NONLEAKY ) );
//   ConvertUtf8ToWchar( source, destination );
//   Elements newLength = destination->length() / sizeof( wchar_t );
//   TOOLS_ASSERT( ( destination->length() % sizeof( wchar_t ) ) == 0 );
//   return StringId( static_cast< wchar_t const * >( destination->data() ), newLength );
// }

// StringId tools::Widen( std::string const & str )
// {
//   return Widen( str.c_str(), str.length() );
// }

StringId tools::StaticStringId( char const * inStr ) throw()
{
    size_t len = strlen( inStr );
    RealStringId * sid = CreateRecord( inStr, numeric_cast<uint32>(len), numeric_cast<uint32>(len+1), true );
    StringTable::iterator i = GetStringTable().find( sid );
    if( i == GetStringTable().end() ) {
        AddToTable( sid );
        atomicIncrement( &totalStaticStringIds_ );
    } else {
        DestroyRecord( sid );
        sid = static_cast< RealStringId * >( *i );
        if( !( sid->isStatic_ ) ) {
            char const * stmp = sid->string_;
            sid->string_ = inStr;
            sid->isStatic_ = true;
            delete [] stmp;
            atomicDecrement( &totalStringIds_ );
            atomicIncrement( &totalStaticStringIds_ );
            atomicSubtract( &totalStringBytes_, sid->length_ );
        }
    }
    atomicIncrement( &sid->refs_ );
    return StringId( static_cast< tools::impl::StringIdData * >( sid ) );
}

#include <tools/UnitTest.h>
#if TOOLS_UNIT_TEST

TOOLS_TEST_CASE("StringId.raw", [](Test &)
{
    char const * str = "TestRawStringTable string";
    size_t len = strlen( str );
    size_t hsh = StringHash( str, numeric_cast<uint32>(len) );
    // create string record
    RealStringId * rid = new RealStringId( str, hsh, len, true );
    TOOLS_ASSERTR( !!rid );
    TOOLS_ASSERTR( rid->isStatic_ );
    TOOLS_ASSERTR( rid->length_ == len );
    TOOLS_ASSERTR( rid->string_ == str );
    TOOLS_ASSERTR( rid->hash_ == hsh );
    TOOLS_ASSERTR( rid->refs_ == 0 );
    // add record to the table
    StringTable::iterator i = GetStringTable().find( rid );
    TOOLS_ASSERTR( i == GetStringTable().end() );
    std::pair< StringTable::iterator, bool > resp = GetStringTable().insert( rid );
    TOOLS_ASSERTR( resp.second );
    TOOLS_ASSERTR( resp.first != GetStringTable().end() );
    rid->refs_ = 1;
    i = GetStringTable().find( rid );
    TOOLS_ASSERTR( i != GetStringTable().end() );
    // make a copy of the string so that we have a different pointer
    char * str2 = (char *)( alloca( sizeof( char ) * ( len + 1 ) ) );
    memcpy( str2, str, sizeof( char ) * ( len + 1 ) );
    size_t len2 = strlen( str2 );
    TOOLS_ASSERTR( len == len2 );
    size_t hsh2 = StringHash( str2, numeric_cast<uint32>(len2) );
    TOOLS_ASSERTR( hsh == hsh2 );
    RealStringId * rid2 = new RealStringId( str2, hsh2, len2, true );
    TOOLS_ASSERTR( !!rid2 );
    TOOLS_ASSERTR( rid2->isStatic_ );
    TOOLS_ASSERTR( rid2->length_ == len2 );
    TOOLS_ASSERTR( rid2->string_ == str2 );
    TOOLS_ASSERTR( rid2->hash_ == hsh2 );
    TOOLS_ASSERTR( rid2->refs_ == 0 );
    // lookup the new record
    i = GetStringTable().find( rid2 );
    TOOLS_ASSERTR( (*i)->string_ == str );
    // delete duplicate record
    delete rid2;
    // remove record from the table
    i = GetStringTable().find( rid );
    TOOLS_ASSERTR( i != GetStringTable().end() );
    rid->refs_ = 0;
    GetStringTable().erase( rid );
    i = GetStringTable().find( rid );
    TOOLS_ASSERTR( i == GetStringTable().end() );
    // destroy record
    delete rid;
});

TOOLS_TEST_CASE("StringId.static", [](Test &)
{
    char const * str = "TestStaticStringId string";
    size_t len = strlen( str );
    size_t hsh = StringHash( str, numeric_cast<uint32>(len) );
    RealStringId * rid = new RealStringId( str, hsh, numeric_cast<uint32>(len), true );
    StringTable::iterator i;
    {
        StringId sid( StaticStringId( str ) );
        i = GetStringTable().find( rid );
        TOOLS_ASSERTR( i != GetStringTable().end() );
        RealStringId * rid2 = static_cast< RealStringId * >( *i );
        TOOLS_ASSERTR( rid2->hash_ == hsh );
        TOOLS_ASSERTR( rid2->isStatic_ );
        TOOLS_ASSERTR( rid2->length_ == len );
        TOOLS_ASSERTR( rid2->refs_ == 1 );
        TOOLS_ASSERTR( rid2->string_ == str );
        TOOLS_ASSERTR( sid.c_str() == str );
        TOOLS_ASSERTR( sid.length() == len );
        TOOLS_ASSERTR( sid.hash() == hsh );
        TOOLS_ASSERTR( !!sid );
        // make a copy of the string so that we have a different pointer
        {
            char * str2 = (char *)( alloca( sizeof( char ) * ( len + 1 ) ) );
            memcpy( str2, str, sizeof( char ) * ( len + 1 ) );
            StringId sid2( StaticStringId( str2 ) );
            TOOLS_ASSERTR( rid2->refs_ == 2 );
            TOOLS_ASSERTR( sid == sid2 );
        }
        // sid2 went out of scope, and so should have released the reference in the table
        TOOLS_ASSERTR( rid2->refs_ == 1 );
    }
    // sid1 went out of scope, so the string should not be in the table anymore
    i = GetStringTable().find( rid );
    TOOLS_ASSERTR( i == GetStringTable().end() );
    delete rid;
});

TOOLS_TEST_CASE("StringId.nonStatic", [](Test &)
{
    char const * str = "TestNonStaticStringId string";
    size_t len = strlen( str );
    size_t hsh = StringHash( str, numeric_cast<uint32>(len) );
    RealStringId * rid = new RealStringId( str, hsh, len, true );
    StringTable::iterator i;
    {
        StringId sid( str );
        i = GetStringTable().find( rid );
        TOOLS_ASSERTR( i != GetStringTable().end() );
        RealStringId * rid2 = static_cast< RealStringId * >( *i );
        TOOLS_ASSERTR( rid2->hash_ == hsh );
        TOOLS_ASSERTR( !( rid2->isStatic_ ) );
        TOOLS_ASSERTR( rid2->length_ == len );
        TOOLS_ASSERTR( rid2->refs_ == 1 );
        TOOLS_ASSERTR( rid2->string_ != str );
        TOOLS_ASSERTR( strcmp( rid2->string_, str ) == 0 );
        TOOLS_ASSERTR( sid.c_str() != str );
        TOOLS_ASSERTR( strcmp( sid.c_str(), str ) == 0 );
        TOOLS_ASSERTR( sid.length() == len );
        TOOLS_ASSERTR( sid.hash() == hsh );
        TOOLS_ASSERTR( !!sid );
        char const * str2 = "TestNonStaticStringId stringblahlblah";
        StringId sid2( str2, numeric_cast<sint32>(len) );
        TOOLS_ASSERTR( !!sid2 );
        TOOLS_ASSERTR( sid == sid2 );
        TOOLS_ASSERTR( rid2->refs_ == 2 );
        std::string wstr( str );
        StringId sid3( wstr );
        TOOLS_ASSERTR( !!sid3 );
        TOOLS_ASSERTR( sid == sid3 );
        TOOLS_ASSERTR( rid2->refs_ == 3 );
        StringId sid4( sid );
        TOOLS_ASSERTR( !!sid4 );
        TOOLS_ASSERTR( sid == sid4 );
        TOOLS_ASSERTR( rid2->refs_ == 4 );
        StringId sid5( str2 );
        TOOLS_ASSERTR( !!sid5 );
        TOOLS_ASSERTR( sid != sid5 );
        TOOLS_ASSERTR( rid2->refs_ == 4 );
        StringId sid6;
        TOOLS_ASSERTR( !sid6 );
        TOOLS_ASSERTR( sid != sid6 );
        TOOLS_ASSERTR( rid2->refs_ == 4 );
        sid6 = sid;
        TOOLS_ASSERTR( !!sid6 );
        TOOLS_ASSERTR( sid == sid6 );
        TOOLS_ASSERTR( rid2->refs_ == 5 );
        // non-member functions
        TOOLS_ASSERTR( str == sid );
        TOOLS_ASSERTR( wstr == sid );
        TOOLS_ASSERTR( str2 != sid );
        std::string str3( str2 );
        TOOLS_ASSERTR( str3 != sid );
    }
    // all StringId instances are out of scope, string should no longer be in table
    i = GetStringTable().find( rid );
    TOOLS_ASSERTR( i == GetStringTable().end() );
    delete rid;
});

TOOLS_TEST_CASE("StringId.static.promotion", [](Test &)
{
    char const * str = "TestStringIdStaticPromotion string";
    size_t len = strlen( str );
    size_t hsh = StringHash( str, numeric_cast<uint32>(len) );
    RealStringId * rid = new RealStringId( str, hsh, len, true );
    StringTable::iterator i;
    {
        // non-static to static promotion
        {
            StringId sid( str );
            i = GetStringTable().find( rid );
            TOOLS_ASSERTR( i != GetStringTable().end() );
            RealStringId * rid2 = static_cast< RealStringId * >( *i );
            TOOLS_ASSERTR( !( rid2->isStatic_ ) );
            StringId sid2( StaticStringId( str ) );
            TOOLS_ASSERTR( sid2 == sid );
            TOOLS_ASSERTR( sid2.c_str() == str );
            TOOLS_ASSERTR( rid2->isStatic_ );
            TOOLS_ASSERTR( rid2->refs_ == 2 );
        }
        i = GetStringTable().find( rid );
        TOOLS_ASSERTR( i == GetStringTable().end() );
        // static remains static
        {
            StringId sid( StaticStringId( str ) );
            i = GetStringTable().find( rid );
            TOOLS_ASSERTR( i != GetStringTable().end() );
            RealStringId * rid2 = static_cast< RealStringId * >( *i );
            TOOLS_ASSERTR( rid2->isStatic_ );
            StringId sid2( str );
            TOOLS_ASSERTR( sid2 == sid );
            TOOLS_ASSERTR( sid2.c_str() == str );
            TOOLS_ASSERTR( rid2->isStatic_ );
            TOOLS_ASSERTR( rid2->refs_ == 2 );
        }
    }
    // all StringId instances are out of scope, string should no longer be in table
    i = GetStringTable().find( rid );
    TOOLS_ASSERTR( i == GetStringTable().end() );
    delete rid;
});

TOOLS_TEST_CASE("StringId.empty", [](Test &)
{
    StringId sid;
    TOOLS_ASSERTR( !sid );
    TOOLS_ASSERTR( IsNullOrEmptyStringId( sid ) );
    StringId sid2( StringIdNull() );
    TOOLS_ASSERTR( !sid2 );
    TOOLS_ASSERTR( IsNullOrEmptyStringId( sid2 ) );
    TOOLS_ASSERTR( sid == sid2 );
    StringId sid3( "" );
    TOOLS_ASSERTR( !!sid3 );
    TOOLS_ASSERTR( IsNullOrEmptyStringId( sid3 ) );
    TOOLS_ASSERTR( sid != sid3 );
    StringId sid4( StringIdEmpty() );
    TOOLS_ASSERTR( !!sid4 );
    TOOLS_ASSERTR( IsNullOrEmptyStringId( sid4 ) );
    TOOLS_ASSERTR( sid != sid4 );
    TOOLS_ASSERTR( sid3 == sid4 );
    StringId sid5( (char const *)nullptr );
    TOOLS_ASSERTR( !sid5 );
    TOOLS_ASSERTR( IsNullOrEmptyStringId( sid5 ) );
    TOOLS_ASSERTR( sid == sid5 );
    StringId sid6( "TestNullAndEmptyStringId string" );
    TOOLS_ASSERTR( !IsNullOrEmptyStringId( sid6 ) );
});

// TODO: reinstate this
//TOOLS_TEST_CASE("StringId.widen", [](Test &)
//{
//    char const * str = "TestWiden string";
//    wchar_t const * str2 = L"TestWiden string";
//    StringId sid( str2 );
//    StringId sid2( Widen( str ) );
//    TOOLS_ASSERTR( sid == sid2 );
//    std::string nstr( str );
//    StringId sid3( Widen( nstr ) );
//    TOOLS_ASSERTR( sid == sid3 );
//});

#endif /* TOOLS_UNIT_TEST */
