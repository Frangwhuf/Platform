#pragma once

#include <tools/Algorithms.h>
#include <tools/Tools.h>

#include <boost/numeric/conversion/cast.hpp>

#include <algorithm>
#include <unordered_set>
#include <string.h>
#include <wchar.h>
#include <string>
#include <iostream>

namespace tools {
#ifdef WINDOWS_PLATFORM
    inline sint32 strcasecmp( char const * s1, char const * s2 ) {
        return _stricmp( ( s1 ), ( s2 ) );
    }
#endif // WINDOWS_PLATFORM
// #if defined(wcscasecmp)
// #undef wcscasecmp
// #endif /* wcscasecmp */
// #ifdef WINDOWS_PLATFORM
//   inline sint32 wcscasecmp( wchar_t const * s1, wchar_t const * s2 ) {
//     return _wcsicmp( ( s1 ), ( s2 ) );
//   }
// #else /* WINDOWS_PLATFORM */
//   using ::wcscasecmp;
// #endif /* WINDOWS_PLATFORM */

    namespace impl {
        struct StringIdData
        {
            char const * string_;
            size_t hash_;
            size_t length_;

			StringIdData( void ) = delete;
            StringIdData( char const * str, size_t hsh, size_t len ) : string_( str ), hash_( hsh ), length_( len ) {}
            TOOLS_FORCE_INLINE bool operator==(StringIdData const & r) const {
                bool ret = (string_ == r.string_);
                if (!ret) {
                    ret = (hash_ == r.hash_) && (length_ == r.length_);
                    if (ret) {
                        ret = (strncmp(string_, r.string_, length_) == 0);
                    }
                } else {
                    ret = (length_ == r.length_);
                }
                return ret;
            }
            TOOLS_FORCE_INLINE bool operator<(StringIdData const & r) const {
                int cmp = strncmp(string_, r.string_, std::min(length_, r.length_));
                bool ret = (cmp == 0);
                if (ret) {
                    ret = (length_ < r.length_);
                } else {
                    ret = (cmp < 0);
                }
                return ret;
            }
        };

        TOOLS_FORCE_INLINE uint32 defineHashAny(StringIdData const & sid, uint32)
        {
            return static_cast<uint32>(sid.hash_ & 0xFFFFFFFF);
        }
    };  // namespace impl

    TOOLS_API StringId StaticStringId( char const * ) throw();

    class StringId
    {
        impl::StringIdData * data_;
#ifdef STRINGID_DEBUGGING
        StringId * thisPointer_;
#endif // STRINGID_DEBUGGING
        TOOLS_API void fillInStringId( char const *, sint32 ) throw();
        friend StringId tools::StaticStringId( char const * ) throw();
        TOOLS_FORCE_INLINE StringId( impl::StringIdData * data )
            : data_( data )
#ifdef STRINGID_DEBUGGING
            , thisPointer_( this )
#endif // STRINGID_DEBUGGING
        {}
    public:
        TOOLS_FORCE_INLINE StringId( void ) throw()
            : data_( nullptr )
#ifdef STRINGID_DEBUGGING
            , thisPointer_( this )
#endif // STRINGID_DEBUGGING
        {}
        TOOLS_FORCE_INLINE StringId( char const * str, sint32 cnt = -1 ) throw() { fillInStringId( str, cnt ); }
        TOOLS_API StringId( StringId const & ) throw();
        TOOLS_FORCE_INLINE StringId( std::string const & str ) throw() { fillInStringId( str.c_str(), boost::numeric_cast<sint32>(str.length()) ); }
        TOOLS_API ~StringId( void ) throw();
        TOOLS_FORCE_INLINE StringId & operator=( StringId const & sid ) throw() {
            return copy( sid );
        }
        TOOLS_FORCE_INLINE bool operator==( StringId const & sid ) const throw() {
            bool ret = (data_ == sid.data_);
            if (!ret && !!data_ && !!sid.data_) {
                ret = (*data_ == *sid.data_);
            }
            return ret;
        }
        TOOLS_FORCE_INLINE bool operator!=( StringId const & sid ) const throw() {
            return !operator==(sid);
        }
        TOOLS_FORCE_INLINE bool operator==( char const * str ) const throw() {
            if( !data_ ) {
                return !str;
            }
            return strcmp( data_->string_, str ) == 0;
        }
        TOOLS_FORCE_INLINE bool operator!=( char const * str ) const throw() {
            if( !data_ ) {
                return !!str;
            }
            return strcmp( data_->string_, str ) != 0;
        }
        TOOLS_FORCE_INLINE bool operator==( std::string const & str ) const throw() {
            if( !data_ ) {
                return false;
            }
            return str == data_->string_;
        }
        TOOLS_FORCE_INLINE bool operator!=( std::string const & str ) const throw() {
            if( !data_ ) {
                return true;
            }
            return str != data_->string_;
        }
        TOOLS_FORCE_INLINE sint32 compareTo( StringId const & str ) const throw() {
            if( !str.data_ ) {
                return ( !data_ ? 0 : 1 );
            }
            if( !data_ ) {
                return -1;
            }
            return strcmp( data_->string_, str.data_->string_ );
        }
        TOOLS_FORCE_INLINE sint32 compareTo( char const * str ) const throw() {
            if( !str ) {
                return ( !data_ ? 0 : 1 );
            }
            if( !data_ ) {
                return -1;
            }
            return strcmp( data_->string_, str );
        }
        TOOLS_FORCE_INLINE sint32 compareTo( std::string const & str ) const throw() {
            if( !data_ ) {
                return -1;
            }
            return strcmp( data_->string_, str.c_str() );  // TODO: look at std::string interface for better way of doing this
        }
        TOOLS_FORCE_INLINE sint32 compareToIgnoreCase( StringId const & str ) const throw() {
            if( !str.data_ ) {
                return ( !data_ ? 0 : 1 );
            }
            if( !data_ ) {
                return -1;
            }
            return strcasecmp( data_->string_, str.data_->string_ );
        }
        TOOLS_FORCE_INLINE sint32 compareToIgnoreCase( char const * str ) const throw() {
            if( !str ) {
                return ( !data_ ? 0 : 1 );
            }
            if( !data_ ) {
                return -1;
            }
            return strcasecmp( data_->string_, str );
        }
        TOOLS_FORCE_INLINE sint32 compareToIgnoreCase( std::string const & str ) const throw() {
            if( !data_ ) {
                return -1;
            }
            return strcasecmp( data_->string_, str.c_str() ); // TODO: look at the std::string interface for a better way of doing this
        }
        TOOLS_FORCE_INLINE bool operator<( StringId const & sid ) const throw() {
            return compareTo( sid ) < 0;
        }
        TOOLS_FORCE_INLINE bool operator<( char const * str ) const throw() {
            return compareTo( str ) < 0;
        }
        TOOLS_FORCE_INLINE bool operator<( std::string const & str ) const throw() {
            return compareTo( str ) < 0;
        }
        TOOLS_FORCE_INLINE char const * c_str( void ) const throw() {
#ifdef STRINGID_DEBUGGING
            TOOLS_ASSERT( thisPointer_ == this );
#endif // STRINGID_DEBUGGING
            if( !data_ ) {
                return nullptr;
            }
            return data_->string_;
        }
        TOOLS_API StringId & copy( StringId const & );
        TOOLS_FORCE_INLINE bool operator!( void ) const throw() {
            return !data_;
        }
        TOOLS_FORCE_INLINE size_t length( void ) const throw() {
            if( !data_ ) {
                return 0;
            }
            return data_->length_;
        }
        TOOLS_FORCE_INLINE size_t hash( void ) const throw() {
            if( !data_ ) {
                return 0;
            }
            return data_->hash_;
        }
    };

    TOOLS_FORCE_INLINE bool operator==( char const * str, StringId const & sid ) {
        return sid == str;
    }

    TOOLS_FORCE_INLINE bool operator==( std::string const & str, StringId const & sid ) {
        return sid == str;
    }

    TOOLS_FORCE_INLINE bool operator!=( char const * str, StringId const & sid ) {
        return sid != str;
    }

    TOOLS_FORCE_INLINE bool operator!=( std::string const & str, StringId const & sid ) {
        return sid != str;
    }

    TOOLS_FORCE_INLINE bool operator<( char const * str, StringId const & sid ) {
        return sid.compareTo( str ) > 0;
    }

    TOOLS_FORCE_INLINE bool operator<( std::string const & str, StringId const & sid ) {
        return sid.compareTo( str ) > 0;
    }

    TOOLS_API StringId const & StringIdNull( void );
    TOOLS_API StringId const & StringIdEmpty( void );
    TOOLS_API StringId const & StringIdWhitespace( void );
    TOOLS_API bool IsNullOrEmptyStringId( StringId const & );
    // TOOLS_API StringId Widen( char const *, sint32 = -1 );
    // TOOLS_API StringId Widen( std::string const & );

    TOOLS_FORCE_INLINE std::ostream & operator<<( std::ostream & stream, StringId const & str ) {
        return ( !!str ? ( stream << str.c_str() ) : ( stream << "(NULL)" ) );
    }

    TOOLS_FORCE_INLINE uint32 defineHashAnyInit( StringId *** )
    {
        return 0x099BB42CU;
    }

    TOOLS_FORCE_INLINE uint32 defineHashAny( StringId const & v, uint32 initial )
    {
        return tools::impl::hashMix( v.hash(), initial );
    }
};  // namespace tools

namespace std {
    template<>
    struct hash< tools::StringId > {
        size_t operator()( tools::StringId const & sid ) const {
            return sid.hash();
        }
    };
};  // namespace std
