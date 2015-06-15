#include "toolsprecompiled.h"

#include <tools/AtomicCollections.h>
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

namespace {
    TOOLS_FORCE_INLINE size_t stringHash( char const * str, uint32 size )
    {
        size_t ret = 0;
        char const * end = str + size;
        for( ; str != end; ++str ) {
            ret = ret ^ ( ( ret << 10 ) + ( ret >> 3 ) + *str );
        }
        return ret;
    }

    TOOLS_FORCE_INLINE char const * copyStringContents( char const * inStr, uint32 copyChars )
    {
        char * ret = new char[ copyChars + 1 ];
        memcpy( ret, inStr, copyChars * sizeof( char ) );
        ret[ copyChars ] = '\0';
        return ret;
    }
	struct NewStringIdData
		: tools::impl::StringIdData
        , Referenced<NewStringIdData>
	{
        NewStringIdData(void) : tools::impl::StringIdData(nullptr, 0, 0), isStatic_(false), generation_(0) {}
		TOOLS_FORCE_INLINE bool operator==( NewStringIdData const & r ) const {
			return tools::impl::StringIdData::operator==( r );
		}
        TOOLS_FORCE_INLINE void populate(char const * str, size_t hsh, size_t len, bool stat, uint64 gen)
        {
            *static_cast<tools::impl::StringIdData *>(this) = tools::impl::StringIdData(str, hsh, len);
            isStatic_ = stat;
            generation_ = gen;
        }
        TOOLS_FORCE_INLINE void populate(tools::impl::StringIdData const & data, bool stat, uint64 gen)
        {
            *static_cast<tools::impl::StringIdData *>(this) = data;
            isStatic_ = stat;
            generation_ = gen;
        }

		bool isStatic_;
        uint64 generation_;
	};

	struct NewRealStringId
		: StandardReferenced< NewRealStringId, NewStringIdData, AllocStatic< Platform >>
	{
        NewRealStringId(void) = default;
        TOOLS_FORCE_INLINE NewRealStringId(char const * str, size_t hsh, size_t len, bool stat, uint64 gen)
        {
            populate(str, hsh, len, stat, gen);
        }
        TOOLS_FORCE_INLINE NewRealStringId(tools::impl::StringIdData const & data, bool stat, uint64 gen)
        {
            populate(data, stat, gen);
        }
    };

	struct StringPhantom
		: StandardPhantomSlistElement< StringPhantom, StandardPhantom< StringPhantom, AllocStatic< Platform >>>
	{
        StringPhantom(AutoDispose<NewRealStringId::Reference> & r, uint64 c = 1U) : data_(r->ref()), count_(c) {}

		AutoDispose<NewRealStringId::Reference> data_;
        uint64 count_;
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

    struct TablePair
        : AllocStatic< Platform >
    {
        NewStringTable * table_;
        uint64 generation_;
    };

	static TOOLS_FORCE_INLINE TablePair getNewStringTable(void) throw()
	{
        static AtomicAny<TablePair, true> table;
        // First make sure the table exists.
        atomicTryUpdate(&table, [](TablePair & element)->bool {
            bool result = !element.table_;
            if (result) {
                element.table_ = new NewStringTable();
            }
            return result;
        });
        // Next increment the generation
        TablePair ret;
        atomicUpdate(&table, [&ret](TablePair element)->TablePair {
            ++element.generation_;
            ret = element;
            return element;
        });
        return ret;
	}

    TOOLS_FORCE_INLINE AutoDispose<NewRealStringId::Reference> createRecord(
        char const * str,
        uint32 len,
        uint32 alloc,
        bool isStatic)
    {
        size_t hsh = stringHash(str, alloc - 1);
        return new NewRealStringId(str, hsh, len, isStatic, 0);
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
    AutoDispose<NewRealStringId::Reference> sid(createRecord(copyStringContents(inStr, numeric_cast<uint32>(len)), numeric_cast<uint32>(len), numeric_cast<uint32>(len + 1), false));
    AutoDispose<NewRealStringId::Reference> found;
    {
        auto table = getNewStringTable();
        sid->generation_ = table.generation_;
        AutoDispose<> proto(phantomTryBindPrototype<PhantomUniversal>());
        table.table_->update(*sid, [&](StringPhantom *& old)->void {
            if (!old || ((sid->generation_ < old->data_->generation_) && !old->data_->isStatic_)) {
                old = new StringPhantom(sid);
            } else {
                old = new StringPhantom(old->data_, old->count_ + 1);
            }
            found = old->data_->ref();
        });
    }
    TOOLS_ASSERT(!!found);
    data_ = found.release();
}

StringId::StringId( StringId const & c ) throw()
{
    if (!c.data_) {
        data_ = nullptr;
    } else {
        AutoDispose<NewRealStringId::Reference> sid(static_cast<NewRealStringId *>(c.data_)->ref());
        AutoDispose<NewRealStringId::Reference> found;
        {
            auto table = getNewStringTable();
            AutoDispose<> proto(phantomTryBindPrototype<PhantomUniversal>());
            table.table_->update(*sid, [&](StringPhantom *& old)->void {
                if (!old || ((sid->generation_ < old->data_->generation_) && !old->data_->isStatic_)) {
                    old = new StringPhantom(sid);
                } else {
                    old = new StringPhantom(old->data_, old->count_ + 1);
                }
                found = old->data_->ref();
            });
        }
        TOOLS_ASSERT(!!found);
        data_ = found.release();
    }
}

StringId::~StringId( void ) throw()
{
    AutoDispose<NewRealStringId::Reference> sid(static_cast<NewRealStringId::Reference *>(data_));
    if (!!sid) {
        auto table = getNewStringTable();
        AutoDispose<> proto(phantomTryBindPrototype<PhantomUniversal>());
        table.table_->update(*sid, [&](StringPhantom *& old)->void {
            if (!!old && (old->data_ == sid) && (old->data_->generation_ == sid->generation_)) {
                if (old->count_ == 1U) {
                    old = nullptr;
                } else {
                    old = new StringPhantom(old->data_, old->count_ - 1);
                }
            }
        });
    }
    data_ = nullptr;
}

StringId & StringId::copy( StringId const & c )
{
    // First release the current data
    {
        AutoDispose<NewRealStringId::Reference> sid(static_cast<NewRealStringId::Reference *>(data_));
        if (!!sid) {
            auto table = getNewStringTable();
            AutoDispose<> proto(phantomTryBindPrototype<PhantomUniversal>());
            table.table_->update(*sid, [&](StringPhantom *& old)->void {
                if (!!old && (old->data_ == sid) && (old->data_->generation_ == sid->generation_)) {
                    if (old->count_ == 1U) {
                        old = nullptr;
                    } else {
                        old = new StringPhantom(old->data_, old->count_ - 1);
                    }
                }
            });
        }
    }
    // Then reference the new one
    if (!c.data_) {
        data_ = nullptr;
    } else {
        AutoDispose<NewRealStringId::Reference> sid(static_cast<NewRealStringId *>(c.data_)->ref());
        AutoDispose<NewRealStringId::Reference> found;
        {
            auto table = getNewStringTable();
            AutoDispose<> proto(phantomTryBindPrototype<PhantomUniversal>());
            table.table_->update(*sid, [&](StringPhantom *& old)->void {
                if (!old || ((sid->generation_ < old->data_->generation_) && !old->data_->isStatic_)) {
                    old = new StringPhantom(sid);
                } else {
                    old = new StringPhantom(old->data_, old->count_ + 1);
                }
                found = old->data_->ref();
            });
        }
        TOOLS_ASSERT(!!found);
        data_ = found.release();
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
    AutoDispose<NewRealStringId::Reference> sid(createRecord(inStr, numeric_cast<uint32>(len), numeric_cast<uint32>(len + 1), true));
    AutoDispose<NewRealStringId::Reference> found;
    {
        auto table = getNewStringTable();
        sid->generation_ = table.generation_;
        AutoDispose<> proto(phantomTryBindPrototype<PhantomUniversal>());
        table.table_->update(*sid, [&](StringPhantom *& old)->void {
            if (!old || (sid->generation_ < old->data_->generation_) || (!old->data_->isStatic_)) {
                old = new StringPhantom(sid);
            } else {
                old = new StringPhantom(old->data_, old->count_ + 1);
            }
            found = old->data_->ref();
        });
    }
    TOOLS_ASSERT(!!found);
    return StringId(found.release());
}

#include <tools/UnitTest.h>
#if TOOLS_UNIT_TEST

TOOLS_TEST_CASE("StringId.raw", [](Test &)
{
    char const * str = "TestRawStringTable string";
    size_t len = strlen( str );
    size_t hsh = stringHash( str, numeric_cast<uint32>(len) );
    // create string record
    AutoDispose<NewRealStringId::Reference> rid(new NewRealStringId(str, hsh, len, true, 0));
    TOOLS_ASSERTR(!!rid);
    TOOLS_ASSERTR(rid->isStatic_);
    TOOLS_ASSERTR(rid->length_ == len);
    TOOLS_ASSERTR(rid->string_ == str);
    TOOLS_ASSERTR(rid->hash_ == hsh);
    // Add record to the table
    {
        auto table = getNewStringTable();
        rid->generation_ = table.generation_;
        AutoDispose<> proto(phantomTryBindPrototype<PhantomUniversal>());
        table.table_->update(*rid, [&](StringPhantom *& old)->void {
            if (!old || ((rid->generation_ < old->data_->generation_) && !old->data_->isStatic_)) {
                old = new StringPhantom(rid);
            } else {
                TOOLS_ASSERTR(!"Test string already exists in table");
            }
        });
    }
    // Can we find the string in the table?
    {
        bool found = false;
        AutoDispose<NewRealStringId::Reference> rid2(new NewRealStringId(str, hsh, len, true, 0));
        auto table = getNewStringTable();
        rid2->generation_ = table.generation_;
        table.table_->find(*rid2, [&](StringPhantom * element)->void {
            if (!!element) {
                TOOLS_ASSERTR(*element->data_ == *rid2);
                TOOLS_ASSERTR(element->data_->generation_ < rid2->generation_);
                TOOLS_ASSERTR(element->data_->isStatic_);
                found = true;
            }
        });
        TOOLS_ASSERTR(found);
    }
    // Make a copy of the string so that we have a different pointer, then check
    {
        char * str2 = static_cast<char *>(alloca(sizeof(char) * (len + 1)));
        memcpy(str2, str, sizeof(char) * (len + 1));
        size_t len2 = strlen(str2);
        TOOLS_ASSERTR(len == len2);
        size_t hsh2 = stringHash(str2, numeric_cast<uint32>(len2));
        TOOLS_ASSERTR(hsh == hsh2);
        AutoDispose<NewRealStringId::Reference> rid2(new NewRealStringId(str2, hsh2, len2, true, 0));
        TOOLS_ASSERTR(!!rid2);
        TOOLS_ASSERTR(rid2->isStatic_);
        TOOLS_ASSERTR(rid2->length_ == len2);
        TOOLS_ASSERTR(rid2->string_ == str2);
        TOOLS_ASSERTR(rid2->hash_ == hsh2);
        // Lookup the new record
        bool found = false;
        auto table = getNewStringTable();
        rid2->generation_ = table.generation_;
        table.table_->find(*rid2, [&](StringPhantom * element)->void {
            if (!!element) {
                TOOLS_ASSERTR(*element->data_ == *rid2);
                TOOLS_ASSERTR(element->data_->generation_ < rid2->generation_);
                TOOLS_ASSERTR(element->data_->isStatic_);
                TOOLS_ASSERTR(element->data_->string_ != rid2->string_);
                found = true;
            }
        });
        TOOLS_ASSERTR(found);
    }
    // Cleanup by removing the record from the table
    {
        TOOLS_ASSERTR(!!rid);
        auto table = getNewStringTable();
        AutoDispose<> proto(phantomTryBindPrototype<PhantomUniversal>());
        table.table_->update(*rid, [&](StringPhantom *& old)->void {
            if (!!old && (old->data_ == rid) && (old->data_->generation_ == rid->generation_)) {
                if (old->count_ == 1U) {
                    old = nullptr;
                } else {
                    TOOLS_ASSERTR(!"string record has too high reference count");
                }
            }
        });
    }
});

TOOLS_TEST_CASE("StringId.static", [](Test &)
{
    char const * str = "TestStaticStringId string";
    size_t len = strlen( str );
    size_t hsh = stringHash( str, numeric_cast<uint32>(len) );
    AutoDispose<NewRealStringId::Reference> rid(new NewRealStringId(str, hsh, numeric_cast<uint32>(len), true, 0));
    // Check that the table does not already contain our test string
    {
        auto table = getNewStringTable();
        table.table_->find(*rid, [](StringPhantom * element)->void {
            TOOLS_ASSERTR(!element);
        });
    }
    {
        StringId sid(StaticStringId(str));
        bool found = false;
        auto table = getNewStringTable();
        table.table_->find(*rid, [&](StringPhantom * element)->void {
            if (!!element) {
                TOOLS_ASSERTR(*element->data_ == *rid);
                TOOLS_ASSERTR(element->data_->isStatic_);
                TOOLS_ASSERTR(element->data_->string_ == rid->string_);
                TOOLS_ASSERTR(element->data_->hash_ == rid->hash_);
                TOOLS_ASSERTR(element->data_->length_ == rid->length_);
                found = true;
            }
        });
        TOOLS_ASSERTR(found);
        TOOLS_ASSERTR(sid.c_str() == str);
        TOOLS_ASSERTR(sid.length() == len);
        TOOLS_ASSERTR(sid.hash() == hsh);
        TOOLS_ASSERTR(!!sid);
        // Make a copy so we have a different pointer
        {
            char * str2 = static_cast<char *>(alloca(sizeof(char) * (len + 1)));
            memcpy(str2, str, sizeof(char) * (len + 1));
            StringId sid2(StaticStringId(str2));
            TOOLS_ASSERTR(sid == sid2);
            TOOLS_ASSERTR(sid2.c_str() == str);
        }
    }
    // Make sure the entry is no longer in the table
    {
        auto table = getNewStringTable();
        table.table_->find(*rid, [](StringPhantom * element)->void {
            TOOLS_ASSERTR(!element);
        });
    }
});

TOOLS_TEST_CASE("StringId.nonStatic", [](Test &)
{
    char const * str = "TestNonStaticStringId string";
    size_t len = strlen( str );
    size_t hsh = stringHash( str, numeric_cast<uint32>(len) );
    AutoDispose<NewRealStringId::Reference> rid(new NewRealStringId(str, hsh, len, true, 0));
    // Check that our test string is not already in the table
    {
        auto table = getNewStringTable();
        table.table_->find(*rid, [](StringPhantom * element)->void {
            TOOLS_ASSERTR(!element);
        });
    }
    {
        StringId sid(str);
        bool found = false;
        auto table = getNewStringTable();
        table.table_->find(*rid, [&](StringPhantom * element)->void {
            if (!!element) {
                TOOLS_ASSERTR(*element->data_ == *rid);
                TOOLS_ASSERTR(element->data_->string_ != str);
                TOOLS_ASSERTR(strcmp(element->data_->string_, str) == 0);
                TOOLS_ASSERTR(element->data_->hash_ == hsh);
                TOOLS_ASSERTR(element->data_->length_ == len);
                TOOLS_ASSERTR(!element->data_->isStatic_);
                found = true;
            }
        });
        TOOLS_ASSERTR(found);
        TOOLS_ASSERTR(sid.c_str() != str);
        TOOLS_ASSERTR(strcmp(sid.c_str(), str) == 0);
        TOOLS_ASSERTR(sid.length() == len);
        TOOLS_ASSERTR(sid.hash() == hsh);
        TOOLS_ASSERTR(!!sid);
        char const * str2 = "TestNonStaticStringId stringblahlblah";
        StringId sid2(str2, numeric_cast<sint32>(len));
        TOOLS_ASSERTR(!!sid2);
        TOOLS_ASSERTR(sid == sid2);
        std::string str3(str);
        StringId sid3(str3);
        TOOLS_ASSERTR(!!sid3);
        TOOLS_ASSERTR(sid == sid3);
        StringId sid4(sid);
        TOOLS_ASSERTR(!!sid4);
        TOOLS_ASSERTR(sid == sid4);
        StringId sid5(str2);
        TOOLS_ASSERTR(!!sid5);
        TOOLS_ASSERTR(sid != sid5);
        StringId sid6;
        TOOLS_ASSERTR(!sid6);
        TOOLS_ASSERTR(sid != sid6);
        sid6 = sid;
        TOOLS_ASSERTR(!!sid6);
        TOOLS_ASSERTR(sid == sid6);
        // Non-member functions
        TOOLS_ASSERTR(str == sid);
        TOOLS_ASSERTR(str3 == sid);
        TOOLS_ASSERTR(str2 != sid);
        std::string str4(str2);
        TOOLS_ASSERTR(str4 != sid);
    }
    // All StringIds have gone out of scope, the string table should no longer have our test string.
    {
        auto table = getNewStringTable();
        table.table_->find(*rid, [](StringPhantom * element)->void {
            TOOLS_ASSERTR(!element);
        });
    }
});

TOOLS_TEST_CASE("StringId.static.promotion", [](Test &)
{
    char const * str = "TestStringIdStaticPromotion string";
    size_t len = strlen( str );
    size_t hsh = stringHash( str, numeric_cast<uint32>(len) );
    AutoDispose<NewRealStringId::Reference> rid(new NewRealStringId(str, hsh, len, true, 0));
    // Check that our test string is not already in the table
    {
        auto table = getNewStringTable();
        table.table_->find(*rid, [](StringPhantom * element)->void {
            TOOLS_ASSERTR(!element);
        });
    }
    {
        // Non-static to static promotion
        {
            StringId sid(str);
            {
                bool found = false;
                auto table = getNewStringTable();
                table.table_->find(*rid, [&](StringPhantom * element)->void {
                    if (!!element) {
                        TOOLS_ASSERTR(*element->data_ == *rid);
                        TOOLS_ASSERTR(!element->data_->isStatic_);
                        found = true;
                    }
                });
                TOOLS_ASSERTR(found);
            }
            StringId sid2(StaticStringId(str));
            {
                bool found = false;
                auto table = getNewStringTable();
                table.table_->find(*rid, [&](StringPhantom * element)->void {
                    if (!!element) {
                        TOOLS_ASSERTR(*element->data_ == *rid);
                        TOOLS_ASSERTR(element->data_->isStatic_);
                        found = true;
                    }
                });
                TOOLS_ASSERTR(found);
            }
            TOOLS_ASSERTR(sid2 == sid);
            TOOLS_ASSERTR(sid2.c_str() == str);
        }
    }
    {
        auto table = getNewStringTable();
        table.table_->find(*rid, [](StringPhantom * element)->void {
            TOOLS_ASSERTR(!element);
        });
    }
    // Static remains static
    {
        StringId sid(StaticStringId(str));
        {
            bool found = false;
            auto table = getNewStringTable();
            table.table_->find(*rid, [&](StringPhantom * element)->void {
                if (!!element) {
                    TOOLS_ASSERTR(*element->data_ == *rid);
                    TOOLS_ASSERTR(element->data_->isStatic_);
                    found = true;
                }
            });
            TOOLS_ASSERTR(found);
        }
        StringId sid2(str);
        {
            bool found = false;
            auto table = getNewStringTable();
            table.table_->find(*rid, [&](StringPhantom * element)->void {
                if (!!element) {
                    TOOLS_ASSERTR(*element->data_ == *rid);
                    TOOLS_ASSERTR(element->data_->isStatic_);
                    found = true;
                }
            });
            TOOLS_ASSERTR(found);
        }
        TOOLS_ASSERTR(sid2 == sid);
        TOOLS_ASSERTR(sid2.c_str() == str);
    }
    {
        auto table = getNewStringTable();
        table.table_->find(*rid, [](StringPhantom * element)->void {
            TOOLS_ASSERTR(!element);
        });
    }
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
