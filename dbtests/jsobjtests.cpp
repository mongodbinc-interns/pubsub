// jsobjtests.h : Test suite generator headers.
//

/**
 *    Copyright (C) 2008 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "../db/jsobj.h"

#include "dbtests.h"

#include <limits>

namespace JsobjTests {
namespace BSONObjTests {
class Create {
public:
    void run() {
        BSONObj b;
        ASSERT_EQUALS( 0, b.nFields() );
    }
};

class Base {
protected:
    static BSONObj basic( const char *name, int val ) {
        BSONObjBuilder b;
        b.appendInt( name, val );
        return b.doneAndDecouple();
    }
    static BSONObj basic( const char *name, vector< int > val ) {
        BSONObjBuilder b;
        b.appendIntArray( name, val );
        return b.doneAndDecouple();
    }
    template< class T >
    static BSONObj basic( const char *name, T val ) {
        BSONObjBuilder b;
        b.append( name, val );
        return b.doneAndDecouple();
    }
};

class WoCompareBasic : public Base {
public:
    void run() {
        ASSERT( basic( "a", 1 ).woCompare( basic( "a", 1 ) ) == 0 );
        ASSERT( basic( "a", 2 ).woCompare( basic( "a", 1 ) ) > 0 );
        ASSERT( basic( "a", 1 ).woCompare( basic( "a", 2 ) ) < 0 );
        // field name comparison
        ASSERT( basic( "a", 1 ).woCompare( basic( "b", 1 ) ) < 0 );
    }
};

class NumericCompareBasic : public Base {
public:
    void run() {
        ASSERT( basic( "a", 1 ).woCompare( basic( "a", 1.0 ) ) == 0 );
    }
};

class WoCompareEmbeddedObject : public Base {
public:
    void run() {
        ASSERT( basic( "a", basic( "b", 1 ) ).woCompare
                ( basic( "a", basic( "b", 1.0 ) ) ) == 0 );
        ASSERT( basic( "a", basic( "b", 1 ) ).woCompare
                ( basic( "a", basic( "b", 2 ) ) ) < 0 );
    }
};

class WoCompareEmbeddedArray : public Base {
public:
    void run() {
        vector< int > i;
        i.push_back( 1 );
        i.push_back( 2 );
        vector< double > d;
        d.push_back( 1 );
        d.push_back( 2 );
        ASSERT( basic( "a", i ).woCompare( basic( "a", d ) ) == 0 );

        vector< int > j;
        j.push_back( 1 );
        j.push_back( 3 );
        ASSERT( basic( "a", i ).woCompare( basic( "a", j ) ) < 0 );
    }
};

namespace JsonStringTests {
    class Empty {
    public:
        void run() {
            BSONObjBuilder b;
            ASSERT_EQUALS( "{}", b.done().jsonString() );
        }
    };

    class SingleStringMember {
    public:
        void run() {
            BSONObjBuilder b;
            b.append( "a", "b" );
            ASSERT_EQUALS( "{ \"a\" : \"b\" }", b.done().jsonString() );
        }
    };

    class EscapedCharacters {
    public:
        void run() {
            BSONObjBuilder b;
            b.append( "a", "\" \\ / \b \f \n \r \t" );
            ASSERT_EQUALS( "{ \"a\" : \"\\\" \\\\ \\/ \\b \\f \\n \\r \\t\" }", b.done().jsonString() );
        }        
    };
    
    class AdditionalControlCharacters {
    public:
        void run() {
            BSONObjBuilder b;
            b.append( "a", "\x1 \x1f \x7f" );
            ASSERT_EQUALS( "{ \"a\" : \"\\u0001 \\u001f \\u007f\" }", b.done().jsonString() );
        }
    };
    
    class ExtendedAscii {
    public:
        void run() {
            BSONObjBuilder b;
            b.append( "a", "\x80" );
            ASSERT_EQUALS( "{ \"a\" : \"\x80\" }", b.done().jsonString() );
        }        
    };
    
    class SingleIntMember {
    public:
        void run() {
            BSONObjBuilder b;
            b.appendInt( "a", 1 );
            ASSERT_EQUALS( "{ \"a\" : 1 }", b.done().jsonString() );
        }
    };
    
    class SingleNumberMember {
    public:
        void run() {
            BSONObjBuilder b;
            b.append( "a", 1.5 );
            ASSERT_EQUALS( "{ \"a\" : 1.5 }", b.done().jsonString() );
        }
    };

    class InvalidNumbers {
    public:
        void run() {
            BSONObjBuilder b;
            b.append( "a", numeric_limits< double >::infinity() );
            ASSERT_EXCEPTION( b.done().jsonString(), AssertionException );

            BSONObjBuilder c;
            c.append( "a", numeric_limits< double >::quiet_NaN() );
            ASSERT_EXCEPTION( c.done().jsonString(), AssertionException );

            BSONObjBuilder d;
            d.append( "a", numeric_limits< double >::signaling_NaN() );
            ASSERT_EXCEPTION( d.done().jsonString(), AssertionException );            
        }
    };    

    class NumberPrecision {
    public:
        void run() {
            BSONObjBuilder b;
            b.append( "a", 123456789 );
            ASSERT_EQUALS( "{ \"a\" : 123456789 }", b.done().jsonString() );            
        }
    };
    
    class SingleBoolMember {
    public:
        void run() {
            BSONObjBuilder b;
            b.appendBool( "a", true );
            ASSERT_EQUALS( "{ \"a\" : true }", b.done().jsonString() );

            BSONObjBuilder c;
            c.appendBool( "a", false );
            ASSERT_EQUALS( "{ \"a\" : false }", c.done().jsonString() );            
        }
    };

    class SingleNullMember {
    public:
        void run() {
            BSONObjBuilder b;
            b.appendNull( "a" );
            ASSERT_EQUALS( "{ \"a\" : null }", b.done().jsonString() );
        }
    };
    
    class SingleObjectMember {
    public:
        void run() {
            BSONObjBuilder b, c;
            b.append( "a", c.done() );
            ASSERT_EQUALS( "{ \"a\" : {} }", b.done().jsonString() );
        }
    };
    
    class TwoMembers {
    public:
        void run() {
            BSONObjBuilder b;
            b.append( "a", 1 );
            b.append( "b", 2 );
            ASSERT_EQUALS( "{ \"a\" : 1, \"b\" : 2 }", b.done().jsonString() );
        }
    };

    class EmptyArray {
    public:
        void run() {
            vector< int > arr;
            BSONObjBuilder b;
            b.append( "a", arr );
            ASSERT_EQUALS( "{ \"a\" : [] }", b.done().jsonString() );
        }
    };
    
    class Array {
    public:
        void run() {
            vector< int > arr;
            arr.push_back( 1 );
            arr.push_back( 2 );
            BSONObjBuilder b;
            b.append( "a", arr );
            ASSERT_EQUALS( "{ \"a\" : [ 1, 2 ] }", b.done().jsonString() );
        }        
    };
    
    class DBRef {
    public:
        void run() {
            OID oid;
            memset( &oid, 0xff, 12 );
            BSONObjBuilder b;
            b.appendDBRef( "a", "namespace", oid );
            ASSERT_EQUALS( "{ \"a\" : { \"$ns\" : \"namespace\", \"$id\" : \"ffffffffffffffffffffffff\" } }",
                          b.done().jsonString() );
        }        
    };

    class ObjectId {
    public:
        void run() {
            OID oid;
            memset( &oid, 0xff, 12 );
            BSONObjBuilder b;
            b.appendOID( "a", &oid );
            ASSERT_EQUALS( "{ \"a\" : \"ffffffffffffffffffffffff\" }",
                          b.done().jsonString() );
        }        
    };
    
    class BinData {
    public:
        void run() {
            char d[ 3 ];
            d[ 0 ] = 'a';
            d[ 1 ] = '\0';
            d[ 2 ] = 'b';
            BSONObjBuilder b;
            b.appendBinData( "a", 3, ByteArray, d );
            ASSERT_EQUALS( "{ \"a\" : { \"$binary\" : \"a\\u0000b\", \"$type\" : \"02\" } }",
                          b.done().jsonString() );
        }
    };
    
    class Symbol {
    public:
        void run() {
            BSONObjBuilder b;
            b.appendSymbol( "a", "b" );
            ASSERT_EQUALS( "{ \"a\" : \"b\" }", b.done().jsonString() );
        }        
    };
    
    class Date {
    public:
        void run() {
            BSONObjBuilder b;
            b.appendDate( "a", 0 );
            ASSERT_EQUALS( "{ \"a\" : { \"$date\" : 0 } }", b.done().jsonString() );            
        }
    };
    
    class Regex {
    public:
        void run() {
            BSONObjBuilder b;
            b.appendRegex( "a", "abc", "I" );
            ASSERT_EQUALS( "{ \"a\" : { \"$regex\" : \"abc\", \"$options\" : \"I\" } }",
                          b.done().jsonString() );            
        }        
    };
} // namespace JsonStringTests

} // namespace BSONObjTests

class All : public UnitTest::Suite {
public:
    All() {
        add< BSONObjTests::Create >();
        add< BSONObjTests::WoCompareBasic >();
        add< BSONObjTests::NumericCompareBasic >();
        add< BSONObjTests::WoCompareEmbeddedObject >();
        add< BSONObjTests::WoCompareEmbeddedArray >();
        add< BSONObjTests::JsonStringTests::Empty >();
        add< BSONObjTests::JsonStringTests::SingleStringMember >();
        add< BSONObjTests::JsonStringTests::EscapedCharacters >();
        add< BSONObjTests::JsonStringTests::AdditionalControlCharacters >();
        add< BSONObjTests::JsonStringTests::ExtendedAscii >();
        add< BSONObjTests::JsonStringTests::SingleIntMember >();
        add< BSONObjTests::JsonStringTests::SingleNumberMember >();
        add< BSONObjTests::JsonStringTests::InvalidNumbers >();
        add< BSONObjTests::JsonStringTests::NumberPrecision >();
        add< BSONObjTests::JsonStringTests::SingleBoolMember >();
        add< BSONObjTests::JsonStringTests::SingleNullMember >();
        add< BSONObjTests::JsonStringTests::SingleObjectMember >();
        add< BSONObjTests::JsonStringTests::TwoMembers >();
        add< BSONObjTests::JsonStringTests::EmptyArray >();
        add< BSONObjTests::JsonStringTests::Array >();
        add< BSONObjTests::JsonStringTests::DBRef >();
        add< BSONObjTests::JsonStringTests::ObjectId >();
        add< BSONObjTests::JsonStringTests::BinData >();
        add< BSONObjTests::JsonStringTests::Symbol >();
        add< BSONObjTests::JsonStringTests::Date >();
        add< BSONObjTests::JsonStringTests::Regex >();
    }
};

} // namespace JsobjTests

UnitTest::TestPtr jsobjTests() {
    return UnitTest::createSuite< JsobjTests::All >();
}
