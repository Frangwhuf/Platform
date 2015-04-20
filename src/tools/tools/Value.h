#pragma once

//#include <tools/Error.h>
//#include <tools/Iterator.h>
#include <tools/Interface.h>
#include <tools/Tools.h>

#include <typeinfo>
#include <boost/any.hpp>
#include <boost/type_traits.hpp>

namespace tools {
  namespace impl {
    struct ValueTypeInfoBase {
      virtual bool isVoid( void ) const = 0;
      virtual bool isInteger( void ) const = 0;
      virtual bool isFloat( void ) const = 0;
      virtual bool isSigned( void ) const = 0;
      virtual bool isPointer( void ) const = 0;
      virtual size_t sizeOf( void ) const = 0;
      virtual StringId const & typeName( void ) const = 0;
    };

    template< typename TypeT >
    struct ValueTypeInfo : ValueTypeInfoBase
    {
      bool isVoid( void ) const { return false; }
      bool isInteger( void ) const { return boost::is_integral< TypeT >::value; }
      bool isFloat( void ) const { return boost::is_floating_point< TypeT >::value; }
      bool isSigned( void ) const { return boost::is_signed< TypeT >::value; }
      bool isPointer( void ) const { return boost::is_pointer< TypeT >::value; }
      size_t sizeOf( void ) const { return sizeof( TypeT ); }
      StringId const & typeName( void ) const { return tools::nameOf< TypeT >(); }
      static ValueTypeInfoBase const * getSingleton( void ) {
        static const ValueTypeInfo< TypeT > singleton;
        return static_cast< ValueTypeInfoBase const * >( &singleton );
      }
    };

    template<>
    struct ValueTypeInfo< void > : ValueTypeInfoBase
    {
      ValueTypeInfo() {}
      bool isVoid( void ) const { return true; }
      bool isInteger( void ) const { return false; }
      bool isFloat( void ) const { return false; }
      bool isSigned( void ) const { return false; }
      bool isPointer( void ) const { return false; }
      size_t sizeOf( void ) const { return 0; }
      StringId const & typeName( void ) const { return tools::nameOf< void >(); }
      static ValueTypeInfoBase const * getSingleton( void ) {
        static const ValueTypeInfo< void > singleton;
        return static_cast< ValueTypeInfoBase const * >( &singleton );
      }
    };
  }; // namespace impl

  struct Value {
    Value( void ) : type_( impl::ValueTypeInfo< void >::getSingleton() ) {}
    template< typename TypeT >
    Value( TypeT const & v ) : value_( v ), type_( impl::ValueTypeInfo< TypeT >::getSingleton() ) {};
    Value( Value const & c ) : value_( c.value_ ), type_( c.type_ ) {}
    Value const & operator=( Value const & c ) { value_ = c.value_; type_ = c.type_; return *this; }
    template< typename TypeT >
    void set( TypeT const & v ) { value_ = v; type_ = impl::ValueTypeInfo< TypeT >::getSingleton(); }
    bool operator!( void ) const { return !( type_->isVoid() ); }
    inline bool isVoid( void ) const { return type_->isVoid(); }
    inline bool isInteger( void ) const { return type_->isInteger(); }
    inline bool isFloat( void ) const { return type_->isFloat(); }
    inline bool isSigned( void ) const { return type_->isSigned(); }
    inline bool isPointer( void ) const { return type_->isPointer(); }
    inline size_t sizeOf( void ) const { return type_->sizeOf(); }
    inline StringId const & typeName( void ) const { return type_->typeName(); }

    boost::any value_;
    impl::ValueTypeInfoBase const * type_;
  };

  //struct ValueException : Exception {
  //  ValueException( StringId const & vt, StringId const & it )
  //    : valueType_( vt ), interpretType_( it )
  //  {}
  //  ValueException( ValueException const & c )
  //    : valueType_( c.valueType_ ), interpretType_( c.interpretType_ )
  //  {}
  //  ValueException & operator=( ValueException const & c ) {
  //    valueType_ = c.valueType_;
  //    interpretType_ = c.interpretType_;
  //    return *this;
  //  }

  //  StringId valueType_, interpretType_;
  //protected:
  //  ValueException( void ) {}
  //};

  namespace impl {
    TOOLS_API StringId ValueToStringId( Value const & );
    TOOLS_API bool ValueToBool( Value const & );
    TOOLS_API sint8 ValueToSint8( Value const & );
    TOOLS_API sint16 ValueToSint16( Value const & );
    TOOLS_API sint32 ValueToSint32( Value const & );
    TOOLS_API sint64 ValueToSint64( Value const & );
    TOOLS_API uint8 ValueToUint8( Value const & );
    TOOLS_API uint16 ValueToUint16( Value const & );
    TOOLS_API uint32 ValueToUint32( Value const & );
    TOOLS_API uint64 ValueToUint64( Value const & );
    TOOLS_API float ValueToFloat( Value const & );
    TOOLS_API double ValueToDouble( Value const & );

    template< typename TypeT >
    TypeT interpretAsIntegral( Value const & v ) {
      if( boost::is_signed< TypeT >::value ) {
        switch( sizeof( TypeT ) ) {
        case 1:
          return ValueToSint8( v );
        case 2:
          return ValueToSint16( v );
        case 4:
          return ValueToSint32( v );
        case 8:
          return ValueToSint64( v );
        default:
          break;
        }
      } else {
        switch( sizeof( TypeT ) ) {
        case 1:
          return ValueToUint8( v );
        case 2:
          return ValueToUint16( v );
        case 4:
          return ValueToUint32( v );
        case 8:
          return ValueToUint64( v );
        default:
          break;
        }
      }
      // what exactly do we have here?
      throw ValueException( v.typeName(), tools::nameOf< TypeT >() );
      return TypeT();
    }

    template< typename TypeT >
    TypeT interpretAsFloat( Value const & v ) {
      switch( sizeof( TypeT ) ) {
      case 4:
        return ValueToFloat( v );
      case 8:
        return ValueToDouble( v );
      default:
        break;
      }
      // what exactly do we have here?
      throw ValueException( v.typeName(), tools::nameOf< TypeT >() );
      return TypeT();
    }
  };  // namespace impl

  namespace detail {
    template< typename TypeT >
    struct InterpretCastOp {
      TypeT operator()( Value const & v ) const {
        // try the general case first
        try {
          return boost::any_cast< TypeT >( v.value_ );
        }
        catch( boost::bad_any_cast const & ) {
        }
        if( boost::is_integral< TypeT >::value ) {
          return impl::interpretAsIntegral< TypeT >( v );
        }
        if( boost::is_floating_point< TypeT >::value ) {
          return impl::interpretAsFloat< TypeT >( v );
        }
        // TODO: implement this
        throw ValueException( v.typeName(), tools::nameOf< TypeT >() );
        return TypeT();
      }
    };

    template< typename TypeT >
    struct InterpretCastOp< TypeT * > {
      TypeT * operator()( Value const & v ) const {
        try {
          return boost::any_cast< TypeT * >( v.value_ );
        }
        catch( boost::bad_any_cast const & ) {
        }
        // TODO: implement this
        throw ValueException( v.typeName(), tools::nameOf< TypeT * >() );
        return (TypeT *)0L;
      }
    };

    template<>
    struct InterpretCastOp< StringId > {
      StringId operator()( Value const & v ) const {
        return tools::impl::ValueToStringId( v );
      }
    };

    template<>
    struct InterpretCastOp< StringId const > {
      StringId const operator()( Value const & v ) const {
        return tools::impl::ValueToStringId( v );
      }
    };

    template<>
    struct InterpretCastOp< bool > {
      bool operator()( Value const & v ) const {
        return tools::impl::ValueToBool( v );
      }
    };
  };  // namespace detail

  template< typename TypeT >
  inline TypeT interpret_cast( Value const & v ) {
    return detail::InterpretCastOp< TypeT >()( v );
  }

  //typedef Iterator< Value > ValueIterator;
  //typedef Iterator< Value const > ValueConstIterator;

  //namespace impl {
  //  template< typename ValueT >
  //  struct ValueIteratorAdapter
  //    : StandardDisposable< ValueIteratorAdapter< ValueT >, Iterator< ValueT > >
  //  {
  //    ValueIteratorAdapter( ValueIterator * iter ) : iter_( iter ) {}
  //    ValueIteratorAdapter( AutoDispose< ValueIterator > & iter ) : iter_( iter ) {}
  //    // Iterator
  //    ValueT * next( void )
  //    {
  //      Value * v = iter_->next();
  //      if( !v ) { return NULL; }
  //      ret_ = interpret_cast< ValueT >( *v );
  //      return &ret_;
  //    }
  //  protected:
  //    AutoDispose< ValueIterator > iter_;
  //    ValueT ret_;
  //  };
  //}; // namespace impl

  //template< typename ValueT >
  //AutoDispose< Iterator< ValueT > > NewValueIteratorAdapter( AutoDispose< ValueIterator > & iter )
  //{
  //  return AutoDispose< Iterator< ValueT > >( TOOLS_NEW( impl::ValueIteratorAdapter< ValueT > )( iter ) );
  //}

  //template< typename ValueT >
  //AutoDispose< Iterator< ValueT > > NewValueIteratorAdapter( ValueIterator * iter )
  //{
  //  return AutoDispose< Iterator< ValueT > >( TOOLS_NEW( impl::ValueIteratorAdapter< ValueT > )( iter ) );
  //}
}; // namespace tools
