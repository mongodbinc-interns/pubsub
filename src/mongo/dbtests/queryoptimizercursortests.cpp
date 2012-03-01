// queryoptimizertests.cpp : query optimizer unit tests
//

/**
 *    Copyright (C) 2009 10gen Inc.
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

#include "pch.h"

#include "../db/queryoptimizercursorimpl.h"
#include "../db/queryoptimizer.h"
#include "../db/instance.h"
#include "../db/ops/delete.h"
#include "dbtests.h"

namespace mongo {
    void __forceLinkGeoPlugin();
    shared_ptr<Cursor> newQueryOptimizerCursor( const char *ns, const BSONObj &query,
                                               const shared_ptr<Projection> &fields =
                                               shared_ptr<Projection>(),
                                               const BSONObj &order = BSONObj(),
                                               const QueryPlanSelectionPolicy &planPolicy =
                                               QueryPlanSelectionPolicy::any(),
                                               bool requireOrder = true );
} // namespace mongo

namespace QueryOptimizerCursorTests {
    
    void dropCollection( const char *ns ) {
     	string errmsg;
        BSONObjBuilder result;
        dropCollection( ns, errmsg, result );
    }
        
    using boost::shared_ptr;
    
    class CachedMatchCounterCount {
    public:
        void run() {
            long long aggregateNscanned;
            CachedMatchCounter c( aggregateNscanned, 0 );
            ASSERT_EQUALS( 0, c.count() );
            ASSERT_EQUALS( 0, c.cumulativeCount() );

            c.resetMatch();
            ASSERT( !c.knowMatch() );

            c.setMatch( false );
            ASSERT( c.knowMatch() );

            c.countMatch( DiskLoc() );
            ASSERT_EQUALS( 0, c.count() );
            ASSERT_EQUALS( 0, c.cumulativeCount() );
            
            c.resetMatch();
            ASSERT( !c.knowMatch() );
            
            c.setMatch( true );
            ASSERT( c.knowMatch() );
            
            c.countMatch( DiskLoc() );
            ASSERT_EQUALS( 1, c.count() );
            ASSERT_EQUALS( 1, c.cumulativeCount() );

            // Don't count the same match twice, without checking the document location.
            c.countMatch( DiskLoc( 1, 1 ) );
            ASSERT_EQUALS( 1, c.count() );
            ASSERT_EQUALS( 1, c.cumulativeCount() );

            // Reset and count another match.
            c.resetMatch();
            c.setMatch( true );
            c.countMatch( DiskLoc( 1, 1 ) );
            ASSERT_EQUALS( 2, c.count() );
            ASSERT_EQUALS( 2, c.cumulativeCount() );
        }
    };
    
    class CachedMatchCounterAccumulate {
    public:
        void run() {
            long long aggregateNscanned;
            CachedMatchCounter c( aggregateNscanned, 10 );
            ASSERT_EQUALS( 0, c.count() );
            ASSERT_EQUALS( 10, c.cumulativeCount() );
            
            c.setMatch( true );
            c.countMatch( DiskLoc() );
            ASSERT_EQUALS( 1, c.count() );
            ASSERT_EQUALS( 11, c.cumulativeCount() );
        }
    };
    
    class CachedMatchCounterDedup {
    public:
        void run() {
            long long aggregateNscanned;
            CachedMatchCounter c( aggregateNscanned, 0 );

            c.setCheckDups( true );
            c.setMatch( true );
            c.countMatch( DiskLoc() );
            ASSERT_EQUALS( 1, c.count() );

            c.resetMatch();
            c.setMatch( true );
            c.countMatch( DiskLoc() );
            ASSERT_EQUALS( 1, c.count() );
        }
    };

    class CachedMatchCounterNscanned {
    public:
        void run() {
            long long aggregateNscanned = 5;
            CachedMatchCounter c( aggregateNscanned, 0 );
            ASSERT_EQUALS( 0, c.nscanned() );
            ASSERT_EQUALS( 5, c.aggregateNscanned() );

            c.updateNscanned( 4 );
            ASSERT_EQUALS( 4, c.nscanned() );
            ASSERT_EQUALS( 9, c.aggregateNscanned() );
        }
    };
    
    class SmallDupSetUpgrade {
    public:
        void run() {
            SmallDupSet d;
            for( int i = 0; i < 100; ++i ) {
                ASSERT( !d.getsetdup( DiskLoc( 0, i ) ) );
                for( int j = 0; j <= i; ++j ) {
                    ASSERT( d.getdup( DiskLoc( 0, j ) ) );
                }
            }
        }
    };

    class SmallDupSetUpgradeRead {
    public:
        void run() {
            SmallDupSet d;
            d.getsetdup( DiskLoc( 0, 0 ) );
            for( int i = 0; i < 550; ++i ) {
                ASSERT( d.getdup( DiskLoc( 0, 0 ) ) );
            }
            ASSERT( d.getsetdup( DiskLoc( 0, 0 ) ) );
        }
    };

    class SmallDupSetUpgradeWrite {
    public:
        void run() {
            SmallDupSet d;
            for( int i = 0; i < 550; ++i ) {
                ASSERT( !d.getsetdup( DiskLoc( 0, i ) ) );
            }
            for( int i = 0; i < 550; ++i ) {
                ASSERT( d.getsetdup( DiskLoc( 0, i ) ) );
            }
        }
    };
    
    class DurationTimerStop {
    public:
        void run() {
            DurationTimer t;
            while( t.duration() == 0 );
            ASSERT( t.duration() > 0 );
            t.stop();
            ASSERT( t.duration() > 0 );
            ASSERT( t.duration() > 0 );
        }
    };

    class Base {
    public:
        Base() {
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            string err;
            userCreateNS( ns(), BSONObj(), err, false );
            dropCollection( ns() );
        }
        ~Base() {
            cc().curop()->reset();
        }
    protected:
        DBDirectClient _cli;
        static const char *ns() { return "unittests.QueryOptimizerTests"; }
        void setQueryOptimizerCursor( const BSONObj &query, const BSONObj &order = BSONObj() ) {
            setQueryOptimizerCursorWithoutAdvancing( query, order );
            if ( ok() && !mayReturnCurrent() ) {
                advance();
            }
        }
        void setQueryOptimizerCursorWithoutAdvancing( const BSONObj &query, const BSONObj &order = BSONObj() ) {
            _c = newQueryOptimizerCursor( ns(), query, shared_ptr<Projection>(), order );
        }
        bool ok() const { return _c->ok(); }
        /** Handles matching and deduping. */
        bool advance() {
            while( _c->advance() && !mayReturnCurrent() );
            return ok();
        }
        int itcount() {
            int ret = 0;
            while( ok() ) {
                ++ret;
                advance();
            }
            return ret;
        }
        BSONObj current() const { return _c->current(); }
        DiskLoc currLoc() const { return _c->currLoc(); }
        void prepareToTouchEarlierIterate() { _c->prepareToTouchEarlierIterate(); }
        void recoverFromTouchingEarlierIterate() { _c->recoverFromTouchingEarlierIterate(); }
        bool mayReturnCurrent() {
//            return _c->currentMatches() && !_c->getsetdup( _c->currLoc() );
            return ( !_c->matcher() || _c->matcher()->matchesCurrent( _c.get() ) ) && !_c->getsetdup( _c->currLoc() );
        }
        bool prepareToYield() const { return _c->prepareToYield(); }
        void recoverFromYield() {
            _c->recoverFromYield();
            if ( ok() && !mayReturnCurrent() ) {
                advance();   
            }
        }
        shared_ptr<Cursor> c() { return _c; }
        long long nscanned() const { return _c->nscanned(); }
        unsigned nNsCursors() const {
            set<CursorId> nsCursors;
            ClientCursor::find( ns(), nsCursors );
            return nsCursors.size();
        }
    private:
        shared_ptr<Cursor> _c;
    };
    
    /** No results for empty collection. */
    class Empty : public Base {
    public:
        void run() {
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            shared_ptr<QueryOptimizerCursor> c =
            dynamic_pointer_cast<QueryOptimizerCursor>
            ( newQueryOptimizerCursor( ns(), BSONObj() ) );
            ASSERT( !c->ok() );
            ASSERT_THROWS( c->_current(), AssertionException );
            ASSERT_THROWS( c->current(), AssertionException );
            ASSERT( c->currLoc().isNull() );
            ASSERT( !c->advance() );
            ASSERT_THROWS( c->currKey(), AssertionException );
            ASSERT_THROWS( c->getsetdup( DiskLoc() ), AssertionException );
            ASSERT_THROWS( c->isMultiKey(), AssertionException );
            ASSERT_THROWS( c->matcher(), AssertionException );
            
            ASSERT_THROWS( c->initialFieldRangeSet(), AssertionException );
            ASSERT_THROWS( c->currentPlanScanAndOrderRequired(), AssertionException );
            ASSERT_THROWS( c->keyFieldsOnly(), AssertionException );
            ASSERT_THROWS( c->runningInitialInOrderPlan(), AssertionException );
            ASSERT_THROWS( c->runningInitialCachedPlan(), AssertionException );

            // ok
            c->initialCandidatePlans();
            c->completePlanOfHybridSetScanAndOrderRequired();
            c->clearIndexesForPatterns();
            c->abortOutOfOrderPlans();
            c->noteIterate( false, false, false );
            c->explainQueryInfo();
        }
    };
    
    /** Simple table scan. */
    class Unindexed : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 ) );
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSONObj() );
            ASSERT_EQUALS( 2, itcount() );
        }
    };
    
    /** Basic test with two indexes and deduping requirement. */
    class Basic : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 2 ) );
            _cli.insert( ns(), BSON( "_id" << 2 << "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
            ASSERT( ok() );
            ASSERT_EQUALS( BSON( "_id" << 1 << "a" << 2 ), current() );
            ASSERT( advance() );
            ASSERT_EQUALS( BSON( "_id" << 2 << "a" << 1 ), current() );
            ASSERT( !advance() );
            ASSERT( !ok() );
        }
    };
    
    class NoMatch : public Base {
    public:
        void run() {
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << 5 << LT << 4 << "a" << GT << 0 ) );
            ASSERT( !ok() );
        }            
    };
    
    /** Order of results indicates that interleaving is occurring. */
    class Interleaved : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 2 ) );
            _cli.insert( ns(), BSON( "_id" << 3 << "a" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 << "a" << 2 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
            ASSERT( ok() );
            ASSERT_EQUALS( BSON( "_id" << 1 << "a" << 2 ), current() );
            ASSERT( advance() );
            ASSERT_EQUALS( BSON( "_id" << 3 << "a" << 1 ), current() );
            ASSERT( advance() );
            ASSERT_EQUALS( BSON( "_id" << 2 << "a" << 2 ), current() );
            ASSERT( !advance() );
            ASSERT( !ok() );
        }
    };
    
    /** Some values on each index do not match. */
    class NotMatch : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 0 << "a" << 10 ) );
            _cli.insert( ns(), BSON( "_id" << 10 << "a" << 0 ) );
            _cli.insert( ns(), BSON( "_id" << 11 << "a" << 12 ) );
            _cli.insert( ns(), BSON( "_id" << 12 << "a" << 11 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << 5 << "a" << GT << 5 ) );
            ASSERT( ok() );
            ASSERT_EQUALS( BSON( "_id" << 11 << "a" << 12 ), current() );
            ASSERT( advance() );
            ASSERT_EQUALS( BSON( "_id" << 12 << "a" << 11 ), current() );
            ASSERT( !advance() );
            ASSERT( !ok() );
        }            
    };
    
    /** After the first 101 matches for a plan, we stop interleaving the plans. */
    class StopInterleaving : public Base {
    public:
        void run() {
            for( int i = 0; i < 101; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << i ) );   
            }
            for( int i = 101; i < 200; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << (301-i) ) );   
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << -1 << "a" << GT << -1 ) );
            for( int i = 0; i < 200; ++i ) {
                ASSERT( ok() );
                ASSERT_EQUALS( i, current().getIntField( "_id" ) );
                advance();
            }
            ASSERT( !advance() );
            ASSERT( !ok() );                
        }
    };
    
    /** Test correct deduping with the takeover cursor. */
    class TakeoverWithDup : public Base {
    public:
        void run() {
            for( int i = 0; i < 101; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << i ) );   
            }
            _cli.insert( ns(), BSON( "_id" << 500 << "a" << BSON_ARRAY( 0 << 300 ) ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << -1 << "a" << GT << -1 ) );
            ASSERT_EQUALS( 102, itcount() );
        }
    };
    
    /** Test usage of matcher with takeover cursor. */
    class TakeoverWithNonMatches : public Base {
    public:
        void run() {
            for( int i = 0; i < 101; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << i ) );   
            }
            _cli.insert( ns(), BSON( "_id" << 101 << "a" << 600 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << -1 << "a" << LT << 500 ) );
            ASSERT_EQUALS( 101, itcount() );
        }
    };
    
    /** Check deduping of dups within just the takeover cursor. */
    class TakeoverWithTakeoverDup : public Base {
    public:
        void run() {
            for( int i = 0; i < 101; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i*2 << "a" << 0 ) );
                _cli.insert( ns(), BSON( "_id" << i*2+1 << "a" << 1 ) );
            }
            _cli.insert( ns(), BSON( "_id" << 202 << "a" << BSON_ARRAY( 2 << 3 ) ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << -1 << "a" << GT << 0) );
            ASSERT_EQUALS( 102, itcount() );
        }
    };
    
    /** Basic test with $or query. */
    class BasicOr : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 0 << "a" << 0 ) );
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << 0 ) << BSON( "a" << 1 ) ) ) );
            ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 0 ), current() );
            ASSERT( advance() );
            ASSERT_EQUALS( BSON( "_id" << 1 << "a" << 1 ), current() );
            ASSERT( !advance() );
        }
    };
    
    /** $or first clause empty. */
    class OrFirstClauseEmpty : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 0 << "a" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << -1 ) << BSON( "a" << 1 ) ) ) );
            ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 1 ), current() );
            ASSERT( advance() );
            ASSERT_EQUALS( BSON( "_id" << 1 << "a" << 1 ), current() );
            ASSERT( !advance() );
        }
    };        
    
    /** $or second clause empty. */
    class OrSecondClauseEmpty : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 0 << "a" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << 0 ) << BSON( "_id" << -1 ) << BSON( "a" << 1 ) ) ) );
            ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 1 ), current() );
            ASSERT( advance() );
            ASSERT_EQUALS( BSON( "_id" << 1 << "a" << 1 ), current() );
            ASSERT( !advance() );
        }
    };
    
    /** $or multiple clauses empty empty. */
    class OrMultipleClausesEmpty : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 0 << "a" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << 2 ) << BSON( "_id" << 4 ) << BSON( "_id" << 0 ) << BSON( "_id" << -1 ) << BSON( "_id" << 6 ) << BSON( "a" << 1 ) << BSON( "_id" << 9 ) ) ) );
            ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 1 ), current() );
            ASSERT( advance() );
            ASSERT_EQUALS( BSON( "_id" << 1 << "a" << 1 ), current() );
            ASSERT( !advance() );
        }
    };
    
    /** Check that takeover occurs at proper match count with $or clauses */
    class TakeoverCountOr : public Base {
    public:
        void run() {
            for( int i = 0; i < 60; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << 0 ) );   
            }
            for( int i = 60; i < 120; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << 1 ) );
            }
            for( int i = 120; i < 150; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << (200-i) ) );
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "a" << 0 ) << BSON( "a" << 1 ) << BSON( "_id" << GTE << 120 << "a" << GT << 1 ) ) ) );
            for( int i = 0; i < 120; ++i ) {
                ASSERT( ok() );
                advance();
            }
            // Expect to be scanning on _id index only.
            for( int i = 120; i < 150; ++i ) {
                ASSERT_EQUALS( i, current().getIntField( "_id" ) );
                advance();
            }
            ASSERT( !ok() );
        }
    };
    
    /** Takeover just at end of clause. */
    class TakeoverEndOfOrClause : public Base {
    public:
        void run() {
            for( int i = 0; i < 102; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i ) );   
            }
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << LT << 101 ) << BSON( "_id" << 101 ) ) ) );
            for( int i = 0; i < 102; ++i ) {
                ASSERT_EQUALS( i, current().getIntField( "_id" ) );
                advance();
            }
            ASSERT( !ok() );
        }
    };
    
    class TakeoverBeforeEndOfOrClause : public Base {
    public:
        void run() {
            for( int i = 0; i < 101; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i ) );   
            }
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << LT << 100 ) << BSON( "_id" << 100 ) ) ) );
            for( int i = 0; i < 101; ++i ) {
                ASSERT_EQUALS( i, current().getIntField( "_id" ) );
                advance();
            }
            ASSERT( !ok() );
        }
    };
    
    class TakeoverAfterEndOfOrClause : public Base {
    public:
        void run() {
            for( int i = 0; i < 103; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i ) );   
            }
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << LT << 102 ) << BSON( "_id" << 102 ) ) ) );
            for( int i = 0; i < 103; ++i ) {
                ASSERT_EQUALS( i, current().getIntField( "_id" ) );
                advance();
            }
            ASSERT( !ok() );
        }
    };
    
    /** Test matching and deduping done manually by cursor client. */
    class ManualMatchingDeduping : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 0 << "a" << 10 ) );
            _cli.insert( ns(), BSON( "_id" << 10 << "a" << 0 ) ); 
            _cli.insert( ns(), BSON( "_id" << 11 << "a" << 12 ) );
            _cli.insert( ns(), BSON( "_id" << 12 << "a" << 11 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            shared_ptr< Cursor > c = newQueryOptimizerCursor( ns(), BSON( "_id" << GT << 5 << "a" << GT << 5 ) );
            ASSERT( c->ok() );
            
            // _id 10 {_id:1}
            ASSERT_EQUALS( 10, c->current().getIntField( "_id" ) );
            ASSERT( !c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->advance() );
            
            // _id 0 {a:1}
            ASSERT_EQUALS( 0, c->current().getIntField( "_id" ) );
            ASSERT( !c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->advance() );
            
            // _id 0 {$natural:1}
            ASSERT_EQUALS( 0, c->current().getIntField( "_id" ) );
            ASSERT( !c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->advance() );
            
            // _id 11 {_id:1}
            ASSERT_EQUALS( BSON( "_id" << 11 << "a" << 12 ), c->current() );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( !c->getsetdup( c->currLoc() ) );
            ASSERT( c->advance() );
            
            // _id 12 {a:1}
            ASSERT_EQUALS( BSON( "_id" << 12 << "a" << 11 ), c->current() );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( !c->getsetdup( c->currLoc() ) );
            ASSERT( c->advance() );
            
            // _id 10 {$natural:1}
            ASSERT_EQUALS( 10, c->current().getIntField( "_id" ) );
            ASSERT( !c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->advance() );
            
            // _id 12 {_id:1}
            ASSERT_EQUALS( BSON( "_id" << 12 << "a" << 11 ), c->current() );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->getsetdup( c->currLoc() ) );
            ASSERT( c->advance() );
            
            // _id 11 {a:1}
            ASSERT_EQUALS( BSON( "_id" << 11 << "a" << 12 ), c->current() );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->getsetdup( c->currLoc() ) );
            ASSERT( c->advance() );
            
            // _id 11 {$natural:1}
            ASSERT_EQUALS( 11, c->current().getIntField( "_id" ) );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->getsetdup( c->currLoc() ) );
            
            // {_id:1} scan is complete.
            ASSERT( !c->advance() );
            ASSERT( !c->ok() );       
            
            // Scan the results again - this time the winning plan has been
            // recorded.
            c = newQueryOptimizerCursor( ns(), BSON( "_id" << GT << 5 << "a" << GT << 5 ) );
            ASSERT( c->ok() );
            
            // _id 10 {_id:1}
            ASSERT_EQUALS( 10, c->current().getIntField( "_id" ) );
            ASSERT( !c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->advance() );
            
            // _id 11 {_id:1}
            ASSERT_EQUALS( BSON( "_id" << 11 << "a" << 12 ), c->current() );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( !c->getsetdup( c->currLoc() ) );
            ASSERT( c->advance() );
            
            // _id 12 {_id:1}
            ASSERT_EQUALS( BSON( "_id" << 12 << "a" << 11 ), c->current() );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( !c->getsetdup( c->currLoc() ) );
            
            // {_id:1} scan complete
            ASSERT( !c->advance() );
            ASSERT( !c->ok() );
        }
    };
    
    /** Curr key must be correct for currLoc for correct matching. */
    class ManualMatchingUsingCurrKey : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << "a" ) );
            _cli.insert( ns(), BSON( "_id" << "b" ) );
            _cli.insert( ns(), BSON( "_id" << "ba" ) );
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            shared_ptr< Cursor > c = newQueryOptimizerCursor( ns(), fromjson( "{_id:/a/}" ) );
            ASSERT( c->ok() );
            // "a"
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( !c->getsetdup( c->currLoc() ) );
            ASSERT( c->advance() );
            ASSERT( c->ok() );
            
            // "b"
            ASSERT( !c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->advance() );
            ASSERT( c->ok() );
            
            // "ba"
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( !c->getsetdup( c->currLoc() ) );
            ASSERT( !c->advance() );
        }
    };
    
    /** Test matching and deduping done manually by cursor client. */
    class ManualMatchingDedupingTakeover : public Base {
    public:
        void run() {
            for( int i = 0; i < 150; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << 0 ) );
            }
            _cli.insert( ns(), BSON( "_id" << 300 << "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            shared_ptr< Cursor > c = newQueryOptimizerCursor( ns(), BSON( "$or" << BSON_ARRAY( BSON( "_id" << LT << 300 ) << BSON( "a" << 1 ) ) ) );
            for( int i = 0; i < 151; ++i ) {
                ASSERT( c->ok() );
                ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( !c->getsetdup( c->currLoc() ) );
                c->advance();
            }
            ASSERT( !c->ok() );
        }
    };
    
    /** Test single key matching bounds. */
    class Singlekey : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "a" << "10" ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            shared_ptr< Cursor > c = newQueryOptimizerCursor( ns(), BSON( "a" << GT << 1 << LT << 5 ) );
            // Two sided bounds work.
            ASSERT( !c->ok() );
        }
    };
    
    /** Test multi key matching bounds. */
    class Multikey : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "a" << BSON_ARRAY( 1 << 10 ) ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "a" << GT << 5 << LT << 3 ) );
            // Multi key bounds work.
            ASSERT( ok() );
        }
    };
    
    /** Add other plans when the recorded one is doing more poorly than expected. */
    class AddOtherPlans : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 0 << "a" << 0 << "b" << 0 ) );
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 << "b" << 0 ) );
            for( int i = 100; i < 150; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << 100 << "b" << i ) );
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "a" << 0 << "b" << 0 ) );
            
            ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 0 << "b" << 0 ), c->current() );
            ASSERT_EQUALS( BSON( "a" << 1 ), c->indexKeyPattern() );

            ASSERT( c->advance() );
            ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 0 << "b" << 0 ), c->current() );
            ASSERT_EQUALS( BSON( "b" << 1 ), c->indexKeyPattern() );
            
            ASSERT( c->advance() );
            ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 0 << "b" << 0 ), c->current() );                
            // Unindexed plan
            ASSERT_EQUALS( BSONObj(), c->indexKeyPattern() );
            ASSERT( !c->advance() );
            
            c = newQueryOptimizerCursor( ns(), BSON( "a" << 100 << "b" << 149 ) );
            // Try {a:1}, which was successful previously.
            for( int i = 0; i < 12; ++i ) {
                ASSERT( 149 != c->current().getIntField( "b" ) );
                ASSERT( c->advance() );
            }
            bool sawB1Index = false;
            do {
                if ( c->indexKeyPattern() == BSON( "b" << 1 ) ) {
                    ASSERT_EQUALS( 149, c->current().getIntField( "b" ) );
                    // We should try the {b:1} index and only see one result from it.
                    ASSERT( !sawB1Index );
                    sawB1Index = true;
                }
            } while ( c->advance() );
            ASSERT( sawB1Index );
        }
    };

    /** Add other plans when the recorded one is doing more poorly than expected, with deletion. */
    class AddOtherPlansDelete : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 0 << "a" << 0 << "b" << 0 ) );
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 << "b" << 0 ) );
            for( int i = 100; i < 120; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << 100 << "b" << i ) );
            }
            for( int i = 199; i >= 150; --i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << 100 << "b" << 150 ) );
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "a" << 0 << "b" << 0 ) );
            
            ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 0 << "b" << 0 ), c->current() );
            ASSERT_EQUALS( BSON( "a" << 1 ), c->indexKeyPattern() );
            
            ASSERT( c->advance() );
            ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 0 << "b" << 0 ), c->current() );
            ASSERT_EQUALS( BSON( "b" << 1 ), c->indexKeyPattern() );
            
            ASSERT( c->advance() );
            ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 0 << "b" << 0 ), c->current() );                
            // Unindexed plan
            ASSERT_EQUALS( BSONObj(), c->indexKeyPattern() );
            ASSERT( !c->advance() );
            
            c = newQueryOptimizerCursor( ns(), BSON( "a" << 100 << "b" << 150 ) );
            // Try {a:1}, which was successful previously.
            for( int i = 0; i < 12; ++i ) {
                ASSERT( 150 != c->current().getIntField( "b" ) );
                ASSERT_EQUALS( BSON( "a" << 1 ), c->indexKeyPattern() );
                ASSERT( c->advance() );
            }
            // Now try {b:1} plan.
            ASSERT_EQUALS( BSON( "b" << 1 ), c->indexKeyPattern() );
            ASSERT_EQUALS( 150, c->current().getIntField( "b" ) );
            ASSERT( c->currentMatches() );
            int id = c->current().getIntField( "_id" );
            c->advance();
            c->prepareToTouchEarlierIterate();
            _cli.remove( ns(), BSON( "_id" << id ) );
            c->recoverFromTouchingEarlierIterate();
            int count = 1;
            while( c->ok() ) {
                if ( c->currentMatches() ) {
                    ++count;
                    int id = c->current().getIntField( "_id" );
                    c->advance();
                    c->prepareToTouchEarlierIterate();
                    _cli.remove( ns(), BSON( "_id" << id ) );
                    c->recoverFromTouchingEarlierIterate();                    
                }
                else {
                    c->advance();
                }
            }
            ASSERT_EQUALS( 50, count );
        }
    };

    /**
     * Add other plans when the recorded one is doing more poorly than expected, with deletion before
     * and after adding the additional plans.
     */
    class AddOtherPlansContinuousDelete : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 0 << "a" << 0 << "b" << 0 ) );
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 << "b" << 0 ) );
            for( int i = 100; i < 400; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << i << "b" << ( 499 - i ) ) );
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "a" << GTE << -1 << LTE << 0 << "b" << GTE << -1 << LTE << 0 ) );
            while( c->advance() );
            // {a:1} plan should be recorded now.
          
            c = newQueryOptimizerCursor( ns(), BSON( "a" << GTE << 100 << LTE << 400 << "b" << GTE << 100 << LTE << 400 ) );
            int count = 0;
            while( c->ok() ) {
                if ( c->currentMatches() ) {
                    ASSERT( !c->getsetdup( c->currLoc() ) );
                    ++count;
                    int id = c->current().getIntField( "_id" );
                    c->advance();
                    c->prepareToTouchEarlierIterate();
                    _cli.remove( ns(), BSON( "_id" << id ) );
                    c->recoverFromTouchingEarlierIterate();
                } else {
                    c->advance();
                }
            }
            ASSERT_EQUALS( 300, count );
            ASSERT_EQUALS( 2U, _cli.count( ns(), BSONObj() ) );
        }
    };

    /** Check $or clause range elimination. */
    class OrRangeElimination : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 ) );
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "$or" << BSON_ARRAY( BSON( "_id" << GT << 0 ) << BSON( "_id" << 1 ) ) ) );
            ASSERT( c->ok() );
            ASSERT( !c->advance() );
        }
    };
    
    /** Check $or match deduping - in takeover cursor. */
    class OrDedup : public Base {
    public:
        void run() {
            for( int i = 0; i < 150; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << i ) );   
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "$or" << BSON_ARRAY( BSON( "_id" << LT << 140 ) << BSON( "_id" << 145 ) << BSON( "a" << 145 ) ) ) );
            
            while( c->current().getIntField( "_id" ) < 140 ) {
                ASSERT( c->advance() );
            }
            // Match from second $or clause.
            ASSERT_EQUALS( 145, c->current().getIntField( "_id" ) );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->advance() );
            // Match from third $or clause.
            ASSERT_EQUALS( 145, c->current().getIntField( "_id" ) );
            // $or deduping is handled by the matcher.
            ASSERT( !c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( !c->advance() );
        }
    };
    
    /** Standard dups with a multikey cursor. */
    class EarlyDups : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "a" << BSON_ARRAY( 0 << 1 << 200 ) ) );
            for( int i = 2; i < 150; ++i ) {
                _cli.insert( ns(), BSON( "a" << i ) );   
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "a" << GT << -1 ) );
            ASSERT_EQUALS( 149, itcount() );
        }
    };
    
    /** Pop or clause in takeover cursor. */
    class OrPopInTakeover : public Base {
    public:
        void run() {
            for( int i = 0; i < 150; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i ) );   
            }
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "$or" << BSON_ARRAY( BSON( "_id" << LTE << 147 ) << BSON( "_id" << 148 ) << BSON( "_id" << 149 ) ) ) );
            for( int i = 0; i < 150; ++i ) {
                ASSERT( c->ok() );
                ASSERT_EQUALS( i, c->current().getIntField( "_id" ) );
                c->advance();
            }
            ASSERT( !c->ok() );
        }
    };
    
    /** Or clause iteration abandoned once full collection scan is performed. */
    class OrCollectionScanAbort : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 0 << "a" << BSON_ARRAY( 1 << 2 << 3 << 4 << 5 ) << "b" << 4 ) );
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << BSON_ARRAY( 6 << 7 << 8 << 9 << 10 ) << "b" << 4 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "$or" << BSON_ARRAY( BSON( "a" << LT << 6 << "b" << 4 ) << BSON( "a" << GTE << 6 << "b" << 4 ) ) ) );
            
            ASSERT( c->ok() );
            
            // _id 0 on {a:1}
            ASSERT_EQUALS( 0, c->current().getIntField( "_id" ) );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( !c->getsetdup( c->currLoc() ) );
            c->advance();
            
            // _id 0 on {$natural:1}
            ASSERT_EQUALS( 0, c->current().getIntField( "_id" ) );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->getsetdup( c->currLoc() ) );
            c->advance();
            
            // _id 0 on {a:1}
            ASSERT_EQUALS( 0, c->current().getIntField( "_id" ) );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->getsetdup( c->currLoc() ) );
            c->advance();
            
            // _id 1 on {$natural:1}
            ASSERT_EQUALS( 1, c->current().getIntField( "_id" ) );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( !c->getsetdup( c->currLoc() ) );
            c->advance();
            
            // _id 0 on {a:1}
            ASSERT_EQUALS( 0, c->current().getIntField( "_id" ) );
            ASSERT( c->matcher()->matchesCurrent( c.get() ) );
            ASSERT( c->getsetdup( c->currLoc() ) );
            c->advance();
            
            // {$natural:1} finished
            ASSERT( !c->ok() );
        }
    };
    
    /** Yield cursor and delete current entry, then continue iteration. */
    class YieldNoOp : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << 0 ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( ok() );
                ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                ASSERT( !advance() );
                ASSERT( !ok() );
                ASSERT( prepareToYield() );
                recoverFromYield();
            }
        }            
    };
    
    /** Yield cursor and delete current entry. */
    class YieldDelete : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << 1 ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            _cli.remove( ns(), BSON( "_id" << 1 ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( !ok() );
                ASSERT( !advance() );
            }
        }
    };
    
    /** Yield cursor and delete current entry, then continue iteration. */
    class YieldDeleteContinue : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << 0 ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            _cli.remove( ns(), BSON( "_id" << 1 ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( ok() );
                ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                ASSERT( !advance() );
                ASSERT( !ok() );
            }
        }            
    };
    
    /** Yield cursor and delete current entry, then continue iteration. */
    class YieldDeleteContinueFurther : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 ) );
            _cli.insert( ns(), BSON( "_id" << 3 ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << 0 ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            _cli.remove( ns(), BSON( "_id" << 1 ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( ok() );
                ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                ASSERT( advance() );
                ASSERT_EQUALS( 3, current().getIntField( "_id" ) );
                ASSERT( !advance() );
                ASSERT( !ok() );
            }
        }            
    };
    
    /** Yield and update current. */
    class YieldUpdate : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "a" << 1 ) );
            _cli.insert( ns(), BSON( "a" << 2 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "a" << GT << 0 ) );
                ASSERT_EQUALS( 1, current().getIntField( "a" ) );
                ASSERT( prepareToYield() );
            }
            
            _cli.update( ns(), BSON( "a" << 1 ), BSON( "$set" << BSON( "a" << 3 ) ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( ok() );
                ASSERT_EQUALS( 2, current().getIntField( "a" ) );
                ASSERT( !advance() );
                ASSERT( !ok() );
            }                
        }
    };
    
    /** Yield and drop collection. */
    class YieldDrop : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << 0 ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            _cli.dropCollection( ns() );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( !ok() );
            }                
        }
    };
    
    /** Yield and drop collection with $or query. */
    class YieldDropOr : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << 1 ) << BSON( "_id" << 2 ) ) ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            _cli.dropCollection( ns() );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                ASSERT_THROWS( recoverFromYield(), MsgAssertionException );
                ASSERT( !ok() );
            }                
        }
    };
    
    /** Yield and remove document with $or query. */
    class YieldRemoveOr : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 ) );

            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << 1 ) << BSON( "_id" << 2 ) ) ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }

            _cli.remove( ns(), BSON( "_id" << 1 ) );

            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( ok() );
                ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
            }
        }
    };

    /** Yield and overwrite current in capped collection. */
    class YieldCappedOverwrite : public Base {
    public:
        void run() {
            _cli.createCollection( ns(), 1000, true );
            _cli.insert( ns(), BSON( "x" << 1 ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "x" << GT << 0 ) );
                ASSERT_EQUALS( 1, current().getIntField( "x" ) );
                ASSERT( prepareToYield() );
            }
            
            int x = 2;
            while( _cli.count( ns(), BSON( "x" << 1 ) ) > 0 ) {
                _cli.insert( ns(), BSON( "x" << x++ ) );   
            }
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                ASSERT_THROWS( recoverFromYield(), MsgAssertionException );
                ASSERT( !ok() );
            }                
        }
    };
    
    /** Yield and drop unrelated index - see SERVER-2454. */
    class YieldDropIndex : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << 1 ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            _cli.dropIndex( ns(), BSON( "a" << 1 ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( !ok() );
            }                
        }
    };
    
    /** Yielding with multiple plans active. */
    class YieldMultiplePlansNoOp : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 2 ) );
            _cli.insert( ns(), BSON( "_id" << 2 << "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( ok() );
                ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                ASSERT( !advance() );
                ASSERT( !ok() );
            }                
        }
    };
    
    /** Yielding with advance and multiple plans active. */
    class YieldMultiplePlansAdvanceNoOp : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 2 ) );
            _cli.insert( ns(), BSON( "_id" << 2 << "a" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 3 << "a" << 3 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                advance();
                ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( ok() );
                ASSERT_EQUALS( 3, current().getIntField( "_id" ) );
                ASSERT( !advance() );
                ASSERT( !ok() );
            }                
        }
    };
    
    /** Yielding with delete and multiple plans active. */
    class YieldMultiplePlansDelete : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 2 ) );
            _cli.insert( ns(), BSON( "_id" << 2 << "a" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 3 << "a" << 4 ) );
            _cli.insert( ns(), BSON( "_id" << 4 << "a" << 3 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                advance();
                ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            _cli.remove( ns(), BSON( "_id" << 2 ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                c()->recoverFromYield();
                ASSERT( ok() );
                // index {a:1} active during yield
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( advance() );
                ASSERT_EQUALS( 3, current().getIntField( "_id" ) );
                ASSERT( advance() );
                ASSERT_EQUALS( 4, current().getIntField( "_id" ) );
                ASSERT( !advance() );
                ASSERT( !ok() );
            }                
        }
    };

    /** Yielding with delete, multiple plans active, and $or clause. */
    class YieldMultiplePlansDeleteOr : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 2 ) );
            _cli.insert( ns(), BSON( "_id" << 2 << "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );

            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << 1 << "a" << 2 ) << BSON( "_id" << 2 << "a" << 1 ) ) ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }

            _cli.remove( ns(), BSON( "_id" << 1 ) );

            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                c()->recoverFromYield();
                ASSERT( ok() );
                ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                ASSERT( !advance() );
                ASSERT( !ok() );
            }
        }
    };
    
    /** Yielding with delete, multiple plans active with advancement to the second, and $or clause. */
    class YieldMultiplePlansDeleteOrAdvance : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 2 ) );
            _cli.insert( ns(), BSON( "_id" << 2 << "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );

            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << 1 << "a" << 2 ) << BSON( "_id" << 2 << "a" << 1 ) ) ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
                c()->advance();
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
            }

            _cli.remove( ns(), BSON( "_id" << 1 ) );

            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                c()->recoverFromYield();
                ASSERT( ok() );
                ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                ASSERT( !advance() );
                ASSERT( !ok() );
            }
        }
    };

    /** Yielding with multiple plans and capped overwrite. */
    class YieldMultiplePlansCappedOverwrite : public Base {
    public:
        void run() {
            _cli.createCollection( ns(), 1000, true );
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "_id" << 1 ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            int i = 1;
            while( _cli.count( ns(), BSON( "_id" << 1 ) ) > 0 ) {
                ++i;
                _cli.insert( ns(), BSON( "_id" << i << "a" << i ) );
            }
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( ok() );
                // {$natural:1} plan does not recover, {_id:1} plan does.
                ASSERT( 1 < current().getIntField( "_id" ) );
            }                
        }
    };
    
    /**
     * Yielding with multiple plans and capped overwrite with unrecoverable cursor
     * active at time of yield.
     */
    class YieldMultiplePlansCappedOverwriteManual : public Base {
    public:
        void run() {
            _cli.createCollection( ns(), 1000, true );
            _cli.insert( ns(), BSON( "a" << 1 << "b" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            shared_ptr<Cursor> c;
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                c = newQueryOptimizerCursor( ns(), BSON( "a" << GT << 0 << "b" << GT << 0 ) );
                ASSERT_EQUALS( 1, c->current().getIntField( "a" ) );
                ASSERT( !c->getsetdup( c->currLoc() ) );
                c->advance();
                ASSERT_EQUALS( 1, c->current().getIntField( "a" ) );
                ASSERT( c->getsetdup( c->currLoc() ) );
                ASSERT( c->prepareToYield() );
            }
            
            int i = 1;
            while( _cli.count( ns(), BSON( "a" << 1 ) ) > 0 ) {
                ++i;
                _cli.insert( ns(), BSON( "a" << i << "b" << i ) );
            }
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                c->recoverFromYield();
                ASSERT( c->ok() );
                // {$natural:1} plan does not recover, {_id:1} plan does.
                ASSERT( 1 < c->current().getIntField( "a" ) );
            }                
        }
    };
    
    /**
     * Yielding with multiple plans and capped overwrite with unrecoverable cursor
     * inctive at time of yield.
     */
    class YieldMultiplePlansCappedOverwriteManual2 : public Base {
    public:
        void run() {
            _cli.createCollection( ns(), 1000, true );
            _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "_id" << 1 ) );
            
            shared_ptr<Cursor> c;
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                c = newQueryOptimizerCursor( ns(), BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
                ASSERT_EQUALS( 1, c->current().getIntField( "_id" ) );
                ASSERT( !c->getsetdup( c->currLoc() ) );
                ASSERT( c->prepareToYield() );
            }
            
            int n = 1;
            while( _cli.count( ns(), BSON( "_id" << 1 ) ) > 0 ) {
                ++n;
                _cli.insert( ns(), BSON( "_id" << n << "a" << n ) );
            }
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                c->recoverFromYield();
                ASSERT( c->ok() );
                // {$natural:1} plan does not recover, {_id:1} plan does.
                ASSERT( 1 < c->current().getIntField( "_id" ) );
                ASSERT( !c->getsetdup( c->currLoc() ) );
                int i = c->current().getIntField( "_id" );
                ASSERT( c->advance() );
                ASSERT( c->getsetdup( c->currLoc() ) );
                while( i < n ) {
                    ASSERT( c->advance() );
                    ++i;
                    ASSERT_EQUALS( i, c->current().getIntField( "_id" ) );
                }
            }                
        }
    };
    
    /** Yield with takeover cursor. */
    class YieldTakeover : public Base {
    public:
        void run() {
            for( int i = 0; i < 150; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << i ) );   
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GTE << 0 << "a" << GTE << 0 ) );
                for( int i = 0; i < 120; ++i ) {
                    ASSERT( advance() );
                }
                ASSERT( ok() );
                ASSERT_EQUALS( 120, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            _cli.remove( ns(), BSON( "_id" << 120 ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( ok() );
                ASSERT_EQUALS( 121, current().getIntField( "_id" ) );
                ASSERT( advance() );
                ASSERT_EQUALS( 122, current().getIntField( "_id" ) );
            }
        }
    };
    
    /** Yield with BacicCursor takeover cursor. */
    class YieldTakeoverBasic : public Base {
    public:
        void run() {
            for( int i = 0; i < 150; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << BSON_ARRAY( i << i+1 ) ) );   
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            auto_ptr<ClientCursor> cc;
            auto_ptr<ClientCursor::YieldData> data( new ClientCursor::YieldData() );
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "b" << NE << 0 << "a" << GTE << 0 ) );
                cc.reset( new ClientCursor( QueryOption_NoCursorTimeout, c(), ns() ) );
                for( int i = 0; i < 120; ++i ) {
                    ASSERT( advance() );
                }
                ASSERT( ok() );
                ASSERT_EQUALS( 120, current().getIntField( "_id" ) );
                cc->prepareToYield( *data );
            }                
            _cli.remove( ns(), BSON( "_id" << 120 ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                ASSERT( ClientCursor::recoverFromYield( *data ) );
                ASSERT( ok() );
                ASSERT_EQUALS( 121, current().getIntField( "_id" ) );
                ASSERT( advance() );
                ASSERT_EQUALS( 122, current().getIntField( "_id" ) );
            }
        }
    };
    
    /** Yield with advance of inactive cursor. */
    class YieldInactiveCursorAdvance : public Base {
    public:
        void run() {
            for( int i = 0; i < 10; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << 10 - i ) );
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
                ASSERT( ok() );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( advance() );
                ASSERT_EQUALS( 9, current().getIntField( "_id" ) );
                ASSERT( advance() );
                ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                ASSERT( prepareToYield() );
            }
            
            _cli.remove( ns(), BSON( "_id" << 9 ) );
            
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                recoverFromYield();
                ASSERT( ok() );
                ASSERT_EQUALS( 8, current().getIntField( "_id" ) );
                ASSERT( advance() );
                ASSERT_EQUALS( 3, current().getIntField( "_id" ) );
                ASSERT( advance() );
                ASSERT_EQUALS( 7, current().getIntField( "_id" ) );
            }                    
        }
    };
    
    class OrderId : public Base {
    public:
        void run() {
            for( int i = 0; i < 10; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i ) );
            }
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSONObj(), BSON( "_id" << 1 ) );
            
            for( int i = 0; i < 10; ++i, advance() ) {
                ASSERT( ok() );
                ASSERT_EQUALS( i, current().getIntField( "_id" ) );
            }
        }
    };
    
    class OrderMultiIndex : public Base {
    public:
        void run() {
            for( int i = 0; i < 10; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << 1 ) );
            }
            _cli.ensureIndex( ns(), BSON( "_id" << 1 << "a" << 1 ) );
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GTE << 0 << "a" << GTE << 0 ), BSON( "_id" << 1 ) );
            
            for( int i = 0; i < 10; ++i, advance() ) {
                ASSERT( ok() );
                ASSERT_EQUALS( i, current().getIntField( "_id" ) );
            }
        }
    };
    
    class OrderReject : public Base {
    public:
        void run() {
            for( int i = 0; i < 10; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << i % 5 ) );
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "a" << GTE << 3 ), BSON( "_id" << 1 ) );
            
            ASSERT( ok() );
            ASSERT_EQUALS( 3, current().getIntField( "_id" ) );
            ASSERT( advance() );
            ASSERT_EQUALS( 4, current().getIntField( "_id" ) );
            ASSERT( advance() );
            ASSERT_EQUALS( 8, current().getIntField( "_id" ) );
            ASSERT( advance() );
            ASSERT_EQUALS( 9, current().getIntField( "_id" ) );
            ASSERT( !advance() );
        }
    };
    
    class OrderNatural : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 5 ) );
            _cli.insert( ns(), BSON( "_id" << 4 ) );
            _cli.insert( ns(), BSON( "_id" << 6 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << 0 ), BSON( "$natural" << 1 ) );
            
            ASSERT( ok() );
            ASSERT_EQUALS( 5, current().getIntField( "_id" ) );
            ASSERT( advance() );
            ASSERT_EQUALS( 4, current().getIntField( "_id" ) );
            ASSERT( advance() );                
            ASSERT_EQUALS( 6, current().getIntField( "_id" ) );
            ASSERT( !advance() );                
        }
    };
    
    class OrderUnindexed : public Base {
    public:
        void run() {
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            ASSERT( !newQueryOptimizerCursor( ns(), BSONObj(), shared_ptr<Projection>(),
                                             BSON( "a" << 1 ) ).get() );
        }
    };
    
    class RecordedOrderInvalid : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "a" << 1 << "b" << 1 ) );
            _cli.insert( ns(), BSON( "a" << 2 << "b" << 2 ) );
            _cli.insert( ns(), BSON( "a" << 3 << "b" << 3 ) );
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            // Plan {a:1} will be chosen and recorded.
            ASSERT( _cli.query( ns(), QUERY( "a" << 2 ).sort( "b" ) )->more() );
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "a" << 2 ),
                                                           shared_ptr<Projection>(),
                                                           BSON( "b" << 1 ) );
            // Check that we are scanning {b:1} not {a:1}, since {a:1} is not properly ordered.
            for( int i = 0; i < 3; ++i ) {
                ASSERT( c->ok() );  
                c->advance();
            }
            ASSERT( !c->ok() );
        }
    };
    
    class KillOp : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 << "b" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 << "b" << 2 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            
            Client::ReadContext ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "b" << GT << 0 ) );
            ASSERT( ok() );
            cc().curop()->kill();
            // First advance() call throws, subsequent calls just fail.
            ASSERT_THROWS( advance(), MsgAssertionException );
            ASSERT( !advance() );
        }
    };
    
    class KillOpFirstClause : public Base {
    public:
        void run() {
            _cli.insert( ns(), BSON( "_id" << 1 << "b" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 << "b" << 2 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            
            Client::ReadContext ctx( ns() );
            shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "$or" << BSON_ARRAY( BSON( "_id" << GT << 0 ) << BSON( "b" << GT << 0 ) ) ) );
            ASSERT( c->ok() );
            cc().curop()->kill();
            // First advance() call throws, subsequent calls just fail.
            ASSERT_THROWS( c->advance(), MsgAssertionException );
            ASSERT( !c->advance() );
        }
    };
    
    class Nscanned : public Base {
    public:
        void run() {
            for( int i = 0; i < 120; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "a" << i ) );
            }
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "_id" << GTE << 0 << "a" << GTE << 0 ) );
            ASSERT( c->ok() );
            ASSERT_EQUALS( 2, c->nscanned() );
            c->advance();
            ASSERT( c->ok() );
            ASSERT_EQUALS( 2, c->nscanned() );
            c->advance();
            for( int i = 3; i < 222; ++i ) {
                ASSERT( c->ok() );
                c->advance();
            }
            ASSERT( !c->ok() );
        }
    };

    /* Test 'touching earlier iterate' without doc modifications. */
    class TouchEarlierIterate : public Base {
    public:
        void run() {            
            _cli.insert( ns(), BSON( "_id" << 1 << "b" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 << "b" << 2 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );

            Client::ReadContext ctx( ns() );
            shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "_id" << GT << 0 << "b" << GT << 0 ) );
            
            ASSERT( c->ok() );
            while( c->ok() ) {
                DiskLoc loc = c->currLoc();
                BSONObj obj = c->current();
                c->prepareToTouchEarlierIterate();
                c->recoverFromTouchingEarlierIterate();
                ASSERT( loc == c->currLoc() );
                ASSERT_EQUALS( obj, c->current() );
                c->advance();
            }
        }
    };

    /* Test 'touching earlier iterate' with doc modifications. */
    class TouchEarlierIterateDelete : public Base {
    public:
        void run() {            
            _cli.insert( ns(), BSON( "_id" << 1 << "b" << 1 ) );
            _cli.insert( ns(), BSON( "_id" << 2 << "b" << 2 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            
            DiskLoc firstLoc;
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "b" << GT << 0 ) );
            ASSERT( ok() );
            firstLoc = currLoc();
            ASSERT( c()->advance() );
            prepareToTouchEarlierIterate();
            
            _cli.remove( ns(), BSON( "_id" << 1 ), true );

            recoverFromTouchingEarlierIterate();
            ASSERT( ok() );
            while( ok() ) {
                ASSERT( firstLoc != currLoc() );
                c()->advance();
            }
        }
    };

    /* Test 'touch earlier iterate' with several doc modifications. */
    class TouchEarlierIterateDeleteMultiple : public Base {
    public:
        void run() {
            for( int i = 1; i < 10; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "b" << i ) );
            }
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            
            set<DiskLoc> deleted;
            int id = 0;
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "b" << GT << 0 ) );
            while( 1 ) {
                if ( !ok() ) {
                    break;
                }
                ASSERT( deleted.count( currLoc() ) == 0 );
                id = current()["_id"].Int();
                deleted.insert( currLoc() );
                c()->advance();
                prepareToTouchEarlierIterate();
                
                _cli.remove( ns(), BSON( "_id" << id ), true );

                recoverFromTouchingEarlierIterate();
            }
            ASSERT_EQUALS( 9U, deleted.size() );
        }
    };

    /* Test 'touch earlier iterate' with takeover. */
    class TouchEarlierIterateTakeover : public Base {
    public:
        void run() {
            for( int i = 1; i < 600; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "b" << i ) );
            }
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            
            Client::ReadContext ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "b" << GT << 0 ) );
            
            ASSERT( ok() );
            int count = 1;
            while( ok() ) {
                DiskLoc loc = currLoc();
                BSONObj obj = current();
                prepareToTouchEarlierIterate();
                recoverFromTouchingEarlierIterate();
                ASSERT( loc == currLoc() );
                ASSERT_EQUALS( obj, current() );
                count += mayReturnCurrent();
                c()->advance();
            }
            ASSERT_EQUALS( 599, count );
        }
    };

    /* Test 'touch earlier iterate' with takeover and deletes. */
    class TouchEarlierIterateTakeoverDeleteMultiple : public Base {
    public:
        void run() {
            for( int i = 1; i < 600; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "b" << i ) );
            }
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            
            set<DiskLoc> deleted;
            int id = 0;

            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursorWithoutAdvancing( BSON( "_id" << GT << 0 << "b" << GT << 0 ) );
            while( 1 ) {
                if ( !ok() ) {
                    break;
                }
                ASSERT( deleted.count( currLoc() ) == 0 );
                id = current()["_id"].Int();
                ASSERT( c()->currentMatches() );
                ASSERT( !c()->getsetdup( currLoc() ) );
                deleted.insert( currLoc() );
                c()->advance();
                prepareToTouchEarlierIterate();

                _cli.remove( ns(), BSON( "_id" << id ), true );

                recoverFromTouchingEarlierIterate();
            }
            ASSERT_EQUALS( 599U, deleted.size() );
        }
    };

    /* Test 'touch earlier iterate' with undexed cursor takeover and deletes. */
    class TouchEarlierIterateUnindexedTakeoverDeleteMultiple : public Base {
    public:
        void run() {
            for( int i = 1; i < 600; ++i ) {
                _cli.insert( ns(), BSON( "a" << BSON_ARRAY( i << i+1 ) << "b" << BSON_ARRAY( i << i+1 ) << "_id" << i ) );
            }
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            
            set<DiskLoc> deleted;
            int id = 0;
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursorWithoutAdvancing( BSON( "a" << GT << 0 << "b" << GT << 0 ) );
            while( 1 ) {
                if ( !ok() ) {
                    break;
                }
                ASSERT( deleted.count( currLoc() ) == 0 );
                id = current()["_id"].Int();
                ASSERT( c()->currentMatches() );
                ASSERT( !c()->getsetdup( currLoc() ) );
                deleted.insert( currLoc() );
                c()->advance();
                prepareToTouchEarlierIterate();
                
                _cli.remove( ns(), BSON( "_id" << id ), true );
                
                recoverFromTouchingEarlierIterate();
            }
            ASSERT_EQUALS( 599U, deleted.size() );
        }
    };
    
    /* Test 'touch earlier iterate' with takeover and deletes, with multiple advances in a row. */
    class TouchEarlierIterateTakeoverDeleteMultipleMultiAdvance : public Base {
    public:
        void run() {
            for( int i = 1; i < 600; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "b" << i ) );
            }
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            
            set<DiskLoc> deleted;
            int id = 0;

            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "b" << GT << 0 ) );
            while( 1 ) {
                if ( !ok() ) {
                    break;
                }
                ASSERT( deleted.count( currLoc() ) == 0 );
                id = current()["_id"].Int();
                ASSERT( c()->currentMatches() );
                deleted.insert( currLoc() );
                advance();
                prepareToTouchEarlierIterate();

                _cli.remove( ns(), BSON( "_id" << id ), true );
                
                recoverFromTouchingEarlierIterate();
            }
            ASSERT_EQUALS( 599U, deleted.size() );
        }
    };

    /* Test yield recovery failure of component capped cursor. */
    class InitialCappedWrapYieldRecoveryFailure : public Base {
    public:
        void run() {
            _cli.createCollection( ns(), 1000, true );
            _cli.insert( ns(), BSON( "_id" << 1 << "x" << 1 ) );
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            setQueryOptimizerCursor( BSON( "x" << GT << 0 ) );
            ASSERT_EQUALS( 1, current().getIntField( "x" ) );
            
            ClientCursor::CleanupPointer p;
            p.reset( new ClientCursor( QueryOption_NoCursorTimeout, c(), ns() ) );
            ClientCursor::YieldData yieldData;
            p->prepareToYield( yieldData );
            
            int x = 2;
            while( _cli.count( ns(), BSON( "x" << 1 ) ) > 0 ) {
                _cli.insert( ns(), BSON( "_id" << x << "x" << x ) );
                ++x;
            }

            // TODO - Might be preferable to return false rather than assert here.
            ASSERT_THROWS( ClientCursor::recoverFromYield( yieldData ), AssertionException );
        }
    };

    /* Test yield recovery failure of takeover capped cursor. */
    class TakeoverCappedWrapYieldRecoveryFailure : public Base {
    public:
        void run() {
            _cli.createCollection( ns(), 10000, true );
            for( int i = 0; i < 300; ++i ) {
                _cli.insert( ns(), BSON( "_id" << i << "x" << i ) );                
            }

            ClientCursor::CleanupPointer p;
            ClientCursor::YieldData yieldData;
            {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "x" << GTE << 0 ) );
                for( int i = 0; i < 299; ++i ) {
                    advance();
                }
                ASSERT_EQUALS( 299, current().getIntField( "x" ) );
                
                p.reset( new ClientCursor( QueryOption_NoCursorTimeout, c(), ns() ) );
                p->prepareToYield( yieldData );
            }
            
            int i = 300;
            while( _cli.count( ns(), BSON( "x" << 299 ) ) > 0 ) {
                _cli.insert( ns(), BSON( "_id" << i << "x" << i ) );
                ++i;
            }
            
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            ASSERT( !ClientCursor::recoverFromYield( yieldData ) );
        }
    };

    /** Test that a ClientCursor holding a QueryOptimizerCursor may be safely invalidated. */
    class InvalidateClientCursorHolder : public Base {
    public:
        void run() {
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            _cli.insert( ns(), BSON( "a" << 1 << "b" << 1 ) );
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            ClientCursor::CleanupPointer p;
            p.reset
            ( new ClientCursor
             ( QueryOption_NoCursorTimeout,
              NamespaceDetailsTransient::getCursor
              ( ns(), BSON( "a" << GTE << 0 << "b" << GTE << 0 ) ),
              ns() ) );

            // Construct component client cursors.
            ClientCursor::YieldData yieldData;
            p->prepareToYield( yieldData );
            ASSERT( nNsCursors() > 1 );

            ClientCursor::invalidate( ns() );
            ASSERT_EQUALS( 0U, nNsCursors() );
        }
    };
    
    /** Test that a ClientCursor holding a QueryOptimizerCursor may be safely timed out. */
    class TimeoutClientCursorHolder : public Base {
    public:
        void run() {
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            _cli.insert( ns(), BSON( "a" << 1 << "b" << 1 ) );
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            ClientCursor::CleanupPointer p;
            p.reset
            ( new ClientCursor
             ( 0,
              NamespaceDetailsTransient::getCursor
              ( ns(), BSON( "a" << GTE << 0 << "b" << GTE << 0 ) ),
              ns() ) );
            
            // Construct component client cursors.
            ClientCursor::YieldData yieldData;
            p->prepareToYield( yieldData );
            ASSERT( nNsCursors() > 1 );
            
            ClientCursor::idleTimeReport( 600001 );
            ASSERT_EQUALS( 0U, nNsCursors() );
        }
    };
    
    class AllowOutOfOrderPlan : public Base {
    public:
        void run() {
            Lock::DBWrite lk(ns());
            Client::Context ctx( ns() );
            shared_ptr<Cursor> c =
            newQueryOptimizerCursor( ns(), BSONObj(), shared_ptr<Projection>(), BSON( "a" << 1 ),
                                    QueryPlanSelectionPolicy::any(), false );
            ASSERT( c );
        }
    };
    
    class NoTakeoverByOutOfOrderPlan : public Base {
    public:
        void run() {
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            // Add enough early matches that the {$natural:1} plan would be chosen if it did not
            // require scan and order.
            for( int i = 0; i < 150; ++i ) {
                _cli.insert( ns(), BSON( "a" << 2 << "b" << 1 ) );
            }
            // Add non matches early on the {a:1} plan.
            for( int i = 0; i < 150; ++i ) {
                _cli.insert( ns(), BSON( "a" << 1 << "b" << 5 ) );
            }
            // Add enough matches outside the {a:1} index range that the {$natural:1} scan will not
            // complete before the {a:1} plan records 101 matches and is selected for takeover.
            for( int i = 0; i < 150; ++i ) {
                _cli.insert( ns(), BSON( "a" << 3 << "b" << 10 ) );
            }            
            Lock::DBWrite lk(ns());
            Client::Context ctx( ns() );
            shared_ptr<Cursor> c =
            newQueryOptimizerCursor( ns(), BSON( "a" << LT << 3 << "b" << 1 ),
                                    shared_ptr<Projection>(), BSON( "a" << 1 ),
                                    QueryPlanSelectionPolicy::any(), false );
            ASSERT( c );
            BSONObj idxKey;
            while( c->ok() ) {
                idxKey = c->indexKeyPattern();
                c->advance();
            }
            // Check that the ordered plan {a:1} took over, despite the unordered plan {$natural:1}
            // seeing > 101 matches.
            ASSERT_EQUALS( BSON( "a" << 1 ), idxKey );
        }
    };
    
    /** If no in order plans are possible, an out of order plan may take over. */
    class OutOfOrderOnlyTakeover : public Base {
    public:
        void run() {
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            for( int i = 0; i < 300; ++i ) {
                _cli.insert( ns(), BSON( "a" << 1 ) );
                _cli.insert( ns(), BSON( "a" << 2 ) );
            }
            
            Lock::DBWrite lk(ns());
            Client::Context ctx( ns() );
            shared_ptr<Cursor> c =
            newQueryOptimizerCursor( ns(), BSON( "a" << 1 ),
                                    shared_ptr<Projection>(), BSON( "b" << 1 ),
                                    QueryPlanSelectionPolicy::any(), false );
            ASSERT( c );
            while( c->advance() );
            // Check that one of the plans took over, and we didn't scan both plans until the a:1
            // index completed (which would yield an nscanned near 600).
            ASSERT( c->nscanned() < 500 );
        }
    };
    
    class CoveredIndex : public Base {
    public:
        void run() {
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            _cli.insert( ns(), BSON( "a" << 1 << "b" << 10 ) );
            
            Lock::DBWrite lk(ns());
            Client::Context ctx( ns() );
            shared_ptr<Projection> fields( new Projection() );
            fields->init( BSON( "_id" << 0 << "a" << 1 ) );
            shared_ptr<QueryOptimizerCursor> c =
            dynamic_pointer_cast<QueryOptimizerCursor>
            ( newQueryOptimizerCursor( ns(), BSON( "a" << GTE << 0 << "b" << GTE << 0 ),
                                      fields, BSON( "a" << 1 ),
                                      QueryPlanSelectionPolicy::any(), false ) );
            bool foundA = false;
            bool foundB = false;
            while( c->ok() ) {
                if ( c->indexKeyPattern() == BSON( "a" << 1 ) ) {
                    foundA = true;
                    ASSERT( c->keyFieldsOnly() );
                    ASSERT_EQUALS( BSON( "a" << 1 ), c->keyFieldsOnly()->hydrate( c->currKey() ) );
                }
                if ( c->indexKeyPattern() == BSON( "b" << 1 ) ) {
                    foundB = true;
                    ASSERT( !c->keyFieldsOnly() );
                }
                c->advance();
            }
            ASSERT( foundA );
            ASSERT( foundB );
        }
    };
    
    class CoveredIndexTakeover : public Base {
    public:
        void run() {
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            for( int i = 0; i < 150; ++i ) {
                _cli.insert( ns(), BSON( "a" << 1 << "b" << 1 ) );
            }
            _cli.insert( ns(), BSON( "a" << 2 ) );
            
            Lock::DBWrite lk(ns());
            Client::Context ctx( ns() );
            shared_ptr<Projection> fields( new Projection() );
            fields->init( BSON( "_id" << 0 << "a" << 1 ) );
            shared_ptr<QueryOptimizerCursor> c =
            dynamic_pointer_cast<QueryOptimizerCursor>
            ( newQueryOptimizerCursor( ns(),
                                      fromjson( "{$or:[{a:1},{b:1},{a:2}]}" ),
                                      fields, BSONObj(),
                                      QueryPlanSelectionPolicy::any(), false ) );
            bool foundA = false;
            bool foundB = false;
            while( c->ok() ) {
                if ( c->indexKeyPattern() == BSON( "a" << 1 ) ) {
                    foundA = true;
                    ASSERT( c->keyFieldsOnly() );
                    ASSERT( BSON( "a" << 1 ) == c->keyFieldsOnly()->hydrate( c->currKey() ) ||
                           BSON( "a" << 2 ) == c->keyFieldsOnly()->hydrate( c->currKey() ) );
                }
                if ( c->indexKeyPattern() == BSON( "b" << 1 ) ) {
                    foundB = true;
                    ASSERT( !c->keyFieldsOnly() );
                }
                c->advance();
            }
            ASSERT( foundA );
            ASSERT( foundB );
        }
    };
    
    class PlanChecking : public Base {
    public:
        virtual ~PlanChecking() {}
    protected:
        void nPlans( int n, const BSONObj &query, const BSONObj &order ) {
            auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), query ) );
            auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
            QueryPlanSet s( ns(), frsp, frspOrig, query, shared_ptr<Projection>(), order );
            ASSERT_EQUALS( n, s.nPlans() );
        }
        shared_ptr<QueryOptimizerCursor> getCursor( const BSONObj &query, const BSONObj &order ) {
            ParsedQuery parsedQuery( ns(), 0, 0, 0,
                                    BSON( "$query" << query << "$orderby" << order ), BSONObj() );
            shared_ptr<Cursor> cursor =
            NamespaceDetailsTransient::getCursor( ns(), query, order,
                                                 QueryPlanSelectionPolicy::any(), 0, &parsedQuery );
            shared_ptr<QueryOptimizerCursor> ret =
            dynamic_pointer_cast<QueryOptimizerCursor>( cursor );
            ASSERT( ret );
            return ret;
        }
        void runQuery( const BSONObj &query, const BSONObj &order ) {
            shared_ptr<QueryOptimizerCursor> cursor = getCursor( query, order );
            while( cursor->advance() );
        }
    };
    
    class SaveGoodIndex : public PlanChecking {
    public:
        void run() {
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );

            Lock::DBWrite lk(ns());
            Client::Context ctx( ns() );
            
            // No best plan - all must be tried.
            nPlans( 3 );
            runQuery();
            // Best plan selected by query.
            nPlans( 1 );
            nPlans( 1 );
            Helpers::ensureIndex( ns(), BSON( "c" << 1 ), false, "c_1" );
            // Best plan cleared when new index added.
            nPlans( 3 );
            runQuery();
            // Best plan selected by query.
            nPlans( 1 );
            
            {
                DBDirectClient client;
                for( int i = 0; i < 334; ++i ) {
                    client.insert( ns(), BSON( "i" << i ) );
                    client.update( ns(), QUERY( "i" << i ), BSON( "i" << i + 1 ) );
                    client.remove( ns(), BSON( "i" << i + 1 ) );
                }
            }
            // Best plan cleared by ~1000 writes.
            nPlans( 3 );

            ParsedQuery parsedQuery( ns(), 0, 0, 0,
                                    BSON( "$query" << BSON( "a" << 4 ) <<
                                         "$hint" << BSON( "$natural" << 1 ) ), BSON( "b" << 1 ) );
            shared_ptr<Cursor> cursor =
            NamespaceDetailsTransient::getCursor( ns(), BSON( "a" << 4 ), BSONObj(),
                                                 QueryPlanSelectionPolicy::any(), 0, &parsedQuery );
            while( cursor->advance() );
            // No plan recorded when a hint is used.
            nPlans( 3 );
            
            ParsedQuery parsedQuery2( ns(), 0, 0, 0,
                                     BSON( "$query" << BSON( "a" << 4 ) <<
                                          "$orderby" << BSON( "b" << 1 << "c" << 1 ) ), BSONObj() );
            shared_ptr<Cursor> cursor2 =
            NamespaceDetailsTransient::getCursor( ns(), BSON( "a" << 4 ),
                                                 BSON( "b" << 1 << "c" << 1 ),
                                                 QueryPlanSelectionPolicy::any(), 0,
                                                 &parsedQuery2 );
            while( cursor2->advance() );
            // Plan recorded was for a different query pattern (different sort spec).
            nPlans( 3 );
            
            // Best plan still selected by query after all these other tests.
            runQuery();
            nPlans( 1 );
        }
    private:
        void nPlans( int n ) {
            return PlanChecking::nPlans( n, BSON( "a" << 4 ), BSON( "b" << 1 ) );
        }
        void runQuery() {
            return PlanChecking::runQuery( BSON( "a" << 4 ), BSON( "b" << 1 ) );
        }
    };
    
    class PossiblePlans : public PlanChecking {
    protected:
        void checkCursor( bool mayRunInOrderPlan, bool mayRunOutOfOrderPlan,
                         bool runningInitialInOrderPlan, bool runningInitialCachedPlan ) {
            QueryOptimizerCursor::CandidatePlans plans = _cursor->initialCandidatePlans();
            ASSERT_EQUALS( mayRunInOrderPlan, plans.mayRunInOrderPlan() );
            ASSERT_EQUALS( mayRunOutOfOrderPlan, plans.mayRunOutOfOrderPlan() );
            ASSERT_EQUALS( runningInitialInOrderPlan, _cursor->runningInitialInOrderPlan() );
            ASSERT_EQUALS( runningInitialCachedPlan, _cursor->runningInitialCachedPlan() );            
        }
        void setCursor( const BSONObj &query, const BSONObj &order ) {
            _cursor = PlanChecking::getCursor( query, order );
        }
        void runCursor( bool completePlanOfHybridSetScanAndOrderRequired = false ) {
            while( _cursor->ok() ) {
                checkIterate( _cursor );
                _cursor->advance();
            }
            ASSERT_EQUALS( completePlanOfHybridSetScanAndOrderRequired,
                          _cursor->completePlanOfHybridSetScanAndOrderRequired() );
        }
        void runCursorUntilTakeover() {
            // This is a bit of a hack, relying on initialFieldRangeSet() being nonzero before
            // takeover and zero after takeover.
            while( _cursor->ok() && _cursor->initialFieldRangeSet() ) {
                checkIterate( _cursor );
                _cursor->advance();
            }
        }
        void checkTakeoverCursor( bool currentPlanScanAndOrderRequired ) {
            ASSERT( !_cursor->initialFieldRangeSet() );
            ASSERT_EQUALS( currentPlanScanAndOrderRequired,
                          _cursor->currentPlanScanAndOrderRequired() );
            ASSERT( !_cursor->completePlanOfHybridSetScanAndOrderRequired() );
            ASSERT( !_cursor->runningInitialInOrderPlan() );
            ASSERT( !_cursor->runningInitialCachedPlan() );
        }
        virtual void checkIterate( const shared_ptr<QueryOptimizerCursor> &cursor ) const = 0;
        shared_ptr<QueryOptimizerCursor> _cursor;
    };
    
    class PossibleInOrderPlans : public PossiblePlans {
    public:
        void run() {
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            _cli.insert( ns(), BSON( "a" << 1 ) );
            for( int i = 0; i < 20; ++i ) {
                _cli.insert( ns(), BSON( "a" << 2 ) );
            }
            
            Lock::DBWrite lk(ns());
            Client::Context ctx( ns() );
            nPlans( 2, BSON( "a" << 1 << "x" << 1 ), BSONObj() );
            setCursor( BSON( "a" << 1 << "x" << 1 ), BSONObj() );
            checkCursor( false );
            ASSERT( _cursor->initialFieldRangeSet()->range( "a" ).equality() );
            ASSERT( !_cursor->initialFieldRangeSet()->range( "b" ).equality() );
            ASSERT( _cursor->initialFieldRangeSet()->range( "x" ).equality() );

            // Without running the (nonempty) cursor, no cached plan is recorded.
            setCursor( BSON( "a" << 1 << "x" << 1 ), BSONObj() );
            checkCursor( false );

            // Running the cursor records the plan.
            runCursor();
            nPlans( 1, BSON( "a" << 1 << "x" << 1 ), BSONObj() );
            setCursor( BSON( "a" << 1 << "x" << 1 ), BSONObj() );
            checkCursor( true );

            // Other plans may be added.
            setCursor( BSON( "a" << 2 << "x" << 1 ), BSONObj() );
            checkCursor( true );
            for( int i = 0; i < 10; ++i, _cursor->advance() );
            // The natural plan has been added in.
            checkCursor( false );
            nPlans( 1, BSON( "a" << 2 << "x" << 1 ), BSONObj() );
            runCursor();

            // The a:1 plan was recorded again.
            nPlans( 1, BSON( "a" << 2 << "x" << 1 ), BSONObj() );
            setCursor( BSON( "a" << 2 << "x" << 1 ), BSONObj() );
            checkCursor( true );
            
            // Clear the recorded plan manually.
            _cursor->clearIndexesForPatterns();
            nPlans( 2, BSON( "a" << 2 << "x" << 1 ), BSONObj() );
            setCursor( BSON( "a" << 2 << "x" << 1 ), BSONObj() );
            checkCursor( false );
            
            // Add more data, and run until takeover occurs.
            for( int i = 0; i < 120; ++i ) {
                _cli.insert( ns(), BSON( "a" << 3 << "x" << 1 ) );
            }
            
            setCursor( BSON( "a" << 3 << "x" << 1 ), BSONObj() );
            checkCursor( false );
            runCursorUntilTakeover();
            ASSERT( _cursor->ok() );
            checkTakeoverCursor( false );
            
            // Try again, with a cached plan this time.
            setCursor( BSON( "a" << 3 << "x" << 1 ), BSONObj() );
            checkCursor( true );
            runCursorUntilTakeover();
            checkTakeoverCursor( false );
        }
    private:
        void checkCursor( bool runningInitialCachedPlan ) {
            return PossiblePlans::checkCursor( true, false, true, runningInitialCachedPlan );
        }
        virtual void checkIterate( const shared_ptr<QueryOptimizerCursor> &cursor ) const {
            ASSERT( !cursor->currentPlanScanAndOrderRequired() );
            ASSERT( !cursor->completePlanOfHybridSetScanAndOrderRequired() );
        }
    };
    
    class PossibleOutOfOrderPlans : public PossiblePlans {
    public:
        void run() {
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            _cli.insert( ns(), BSON( "a" << 1 << "b" << 1 ) );
            for( int i = 0; i < 20; ++i ) {
                _cli.insert( ns(), BSON( "a" << 2 ) );
            }
            _cli.insert( ns(), BSON( "b" << 2 ) );
            
            Lock::DBWrite lk(ns());
            Client::Context ctx( ns() );
            nPlans( 3, BSON( "a" << 1 << "b" << 1 ), BSON( "x" << 1 ) );
            setCursor( BSON( "a" << 1 << "b" << 1 ), BSON( "x" << 1 ) );
            checkCursor( false );
            ASSERT( _cursor->initialFieldRangeSet()->range( "a" ).equality() );
            ASSERT( _cursor->initialFieldRangeSet()->range( "b" ).equality() );
            ASSERT( !_cursor->initialFieldRangeSet()->range( "x" ).equality() );
            
            // Without running the (nonempty) cursor, no cached plan is recorded.
            setCursor( BSON( "a" << 1 << "b" << 1 ), BSON( "x" << 1 ) );
            checkCursor( false );
            
            // Running the cursor records the plan.
            runCursor();
            nPlans( 1, BSON( "a" << 1 << "b" << 1 ), BSON( "x" << 1 ) );
            setCursor( BSON( "a" << 1 << "b" << 1 ), BSON( "x" << 1 ) );
            checkCursor( true );
            
            // Other plans may be added.
            setCursor( BSON( "a" << 2 << "b" << 2 ), BSON( "x" << 1 ) );
            checkCursor( true );
            for( int i = 0; i < 10; ++i, _cursor->advance() );
            // The other plans have been added in.
            checkCursor( false );
            runCursor();
            
            // The b:1 plan was recorded.
            setCursor( BSON( "a" << 1 << "b" << 1 ), BSON( "x" << 1 ) );
            checkCursor( true );
            
            // Clear the recorded plan manually.
            _cursor->clearIndexesForPatterns();
            setCursor( BSON( "a" << 2 << "x" << 1 ), BSON( "x" << 1 ) );
            checkCursor( false );
            
            // Add more data, and run until takeover occurs.
            for( int i = 0; i < 120; ++i ) {
                _cli.insert( ns(), BSON( "a" << 3 << "b" << 3 ) );
            }
            
            setCursor( BSON( "a" << 3 << "b" << 3 ), BSON( "x" << 1 ) );
            checkCursor( false );
            runCursorUntilTakeover();
            ASSERT( _cursor->ok() );
            checkTakeoverCursor( true );
            
            // Try again, with a cached plan this time.
            setCursor( BSON( "a" << 3 << "b" << 3 ), BSON( "x" << 1 ) );
            checkCursor( true );
            runCursorUntilTakeover();
            checkTakeoverCursor( true );
        }
    private:
        void checkCursor( bool runningInitialCachedPlan ) {
            return PossiblePlans::checkCursor( false, true, false, runningInitialCachedPlan );
        }
        virtual void checkIterate( const shared_ptr<QueryOptimizerCursor> &cursor ) const {
            ASSERT( cursor->currentPlanScanAndOrderRequired() );
            ASSERT( !cursor->completePlanOfHybridSetScanAndOrderRequired() );
        }
    };
    
    class PossibleBothPlans : public PossiblePlans {
    public:
        void run() {
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
            _cli.insert( ns(), BSON( "a" << 1 << "b" << 1 ) );
            _cli.insert( ns(), BSON( "a" << 2 << "b" << 1 ) );
            for( int i = 0; i < 20; ++i ) {
                _cli.insert( ns(), BSON( "a" << 2 ) );
                if ( i % 10 == 0 ) {
                    _cli.insert( ns(), BSON( "b" << 2 ) );                    
                }
            }
            
            Lock::DBWrite lk(ns());
            Client::Context ctx( ns() );
            nPlans( 3, BSON( "a" << 1 << "b" << 1 ), BSON( "b" << 1 ) );
            setCursor( BSON( "a" << 1 << "b" << 1 ), BSON( "b" << 1 ) );
            checkCursor( true, false );
            ASSERT( _cursor->initialFieldRangeSet()->range( "a" ).equality() );
            ASSERT( _cursor->initialFieldRangeSet()->range( "b" ).equality() );
            ASSERT( !_cursor->initialFieldRangeSet()->range( "x" ).equality() );
            
            // Without running the (nonempty) cursor, no cached plan is recorded.
            setCursor( BSON( "a" << 1 << "b" << 1 ), BSON( "b" << 1 ) );
            checkCursor( true, false );
            
            // Running the cursor records the a:1 plan.
            runCursor( true );
            nPlans( 1, BSON( "a" << 1 << "b" << 1 ), BSON( "b" << 1 ) );
            setCursor( BSON( "a" << 1 << "b" << 1 ), BSON( "b" << 1 ) );
            checkCursor( false, true );
            
            // Other plans may be added.
            setCursor( BSON( "a" << 2 << "b" << 2 ), BSON( "b" << 1 ) );
            checkCursor( false, true );
            for( int i = 0; i < 10; ++i, _cursor->advance() );
            // The other plans have been added in (including ordered b:1).
            checkCursor( true, false );
            runCursor( false );
            
            // The b:1 plan was recorded.
            setCursor( BSON( "a" << 1 << "b" << 1 ), BSON( "b" << 1 ) );
            checkCursor( true, true );
            
            // Clear the recorded plan manually.
            _cursor->clearIndexesForPatterns();
            setCursor( BSON( "a" << 2 << "b" << 1 ), BSON( "b" << 1 ) );
            checkCursor( true, false );
            
            // Add more data, and run until takeover occurs.
            for( int i = 0; i < 120; ++i ) {
                _cli.insert( ns(), BSON( "a" << 3 << "b" << 3 ) );
            }
            
            setCursor( BSON( "a" << 3 << "b" << 3 ), BSON( "b" << 1 ) );
            checkCursor( true, false );
            runCursorUntilTakeover();
            ASSERT( _cursor->ok() );
            checkTakeoverCursor( false );
            ASSERT_EQUALS( BSON( "b" << 1 ), _cursor->indexKeyPattern() );
            
            // Try again, with a cached plan this time.
            setCursor( BSON( "a" << 3 << "b" << 3 ), BSON( "b" << 1 ) );
            checkCursor( true, true );
            runCursorUntilTakeover();
            checkTakeoverCursor( false );
            ASSERT_EQUALS( BSON( "b" << 1 ), _cursor->indexKeyPattern() );
        }
    private:
        void checkCursor( bool runningInitialInOrderPlan, bool runningInitialCachedPlan ) {
            return PossiblePlans::checkCursor( true, true, runningInitialInOrderPlan,
                                              runningInitialCachedPlan );
        }
        virtual void checkIterate( const shared_ptr<QueryOptimizerCursor> &cursor ) const {
            if ( cursor->indexKeyPattern() == BSON( "b" << 1 ) ) {
                ASSERT( !cursor->currentPlanScanAndOrderRequired() );
            }
            else {
                ASSERT( cursor->currentPlanScanAndOrderRequired() );                
            }
            ASSERT( !cursor->completePlanOfHybridSetScanAndOrderRequired() );
        }
    };
    
    class AbortOutOfOrderPlans : public PlanChecking {
    public:
        void run() {
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            for( int i = 0; i < 10; ++i ) {
                _cli.insert( ns(), BSON( "a" << 1 ) );
            }
            
            Lock::DBWrite lk(ns());
            Client::Context ctx( ns() );
            
            shared_ptr<QueryOptimizerCursor> c = getCursor( BSON( "a" << 1 << "b" << BSONNULL ),
                                                           BSON( "a" << 1 ) );
            // Wait until a $natural plan result is returned.
            while( c->indexKeyPattern() != BSONObj() ) {
                c->advance();
            }
            // Abort the natural plan.
            c->abortOutOfOrderPlans();
            c->advance();
            // Check that no more results from the natural plan are returned.
            ASSERT( c->ok() );
            while( c->ok() ) {
                ASSERT_EQUALS( BSON( "a" << 1 ), c->indexKeyPattern() );
                c->advance();
            }
            ASSERT( !c->completePlanOfHybridSetScanAndOrderRequired() );
        }
    };
    
    class AbortOutOfOrderPlanOnLastMatch : public PlanChecking {
    public:
        void run() {
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            for( int i = 0; i < 10; ++i ) {
                _cli.insert( ns(), BSON( "a" << BSON_ARRAY( 1 << 2 ) ) );
            }
            
            Lock::DBWrite lk(ns());
            Client::Context ctx( ns() );
            
            shared_ptr<QueryOptimizerCursor> c =
            getCursor( BSON( "a" << GTE << 1 << "b" << BSONNULL ), BSON( "a" << 1 ) );
            // Wait until 10 (all) $natural plan results are returned.
            for( int i = 0; i < 10; ++i ) {
                while( c->indexKeyPattern() != BSONObj() ) {
                    c->advance();
                }
                c->advance();
            }
            // Abort the natural plan.
            c->abortOutOfOrderPlans();
            c->advance();
            // Check that no more results from the natural plan are returned, and the cursor is not
            // done iterating.
            ASSERT( c->ok() );
            while( c->ok() ) {
                ASSERT_EQUALS( BSON( "a" << 1 ), c->indexKeyPattern() );
                c->advance();
            }
            ASSERT( !c->completePlanOfHybridSetScanAndOrderRequired() );
        }
    };
    
    class TakeoverOrRangeElimination : public PlanChecking {
    public:
        void run() {
            _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            for( int i = 0; i < 120; ++i ) {
                _cli.insert( ns(), BSON( "a" << 1 ) );
            }
            for( int i = 0; i < 20; ++i ) {
                _cli.insert( ns(), BSON( "a" << 2 ) );
            }
            for( int i = 0; i < 20; ++i ) {
                _cli.insert( ns(), BSON( "a" << 3 ) );
            }

            Lock::DBWrite lk(ns());
            Client::Context ctx( ns() );
            
            shared_ptr<QueryOptimizerCursor> c =
            getCursor( fromjson( "{$or:[{a:{$lte:2}},{a:{$gte:2}},{a:9}]}" ), BSONObj() );

            int count = 0;
            while( c->ok() ) {
                c->advance();
                ++count;
            }
            ASSERT_EQUALS( 160, count );
        }
    };
    
    class TakeoverOrDedups : public PlanChecking {
    public:
        void run() {
            _cli.ensureIndex( ns(), BSON( "a" << 1 << "b" << 1 ) );
            for( int i = 0; i < 120; ++i ) {
                _cli.insert( ns(), BSON( "a" << 1 << "b" << 1 ) );
            }
            for( int i = 0; i < 20; ++i ) {
                _cli.insert( ns(), BSON( "a" << 2 << "b" << 2 ) );
            }
            for( int i = 0; i < 20; ++i ) {
                _cli.insert( ns(), BSON( "a" << 3 << "b" << 3 ) );
            }
            
            Lock::DBWrite lk(ns());
            Client::Context ctx( ns() );
            
            BSONObj query =
            BSON(
                 "$or" << BSON_ARRAY(
                                     BSON(
                                          "a" << GTE << 0 << LTE << 2 <<
                                          "b" << GTE << 0 << LTE << 2
                                          ) <<
                                     BSON(
                                          "a" << GTE << 1 << LTE << 3 <<
                                          "b" << GTE << 1 << LTE << 3
                                          ) <<
                                     BSON(
                                          "a" << GTE << 1 << LTE << 4 <<
                                          "b" << GTE << 1 << LTE << 4
                                          )
                                     )
                 );
                                     
            shared_ptr<QueryOptimizerCursor> c = getCursor( query, BSONObj() );
            
            int count = 0;
            while( c->ok() ) {
                if ( ( c->indexKeyPattern() == BSON( "a" << 1 << "b" << 1 ) ) &&
                    c->currentMatches() ) {
                    ++count;
                }
                c->advance();
            }
            ASSERT_EQUALS( 160, count );
        }
    };
    
    namespace GetCursor {
        
        class Base : public QueryOptimizerCursorTests::Base {
        public:
            Base() {
                // create collection
                _cli.insert( ns(), BSON( "_id" << 5 ) );
            }
            virtual ~Base() {}
            void run() {
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                bool simpleEqualityMatch;
                if ( expectException() ) {
                    ASSERT_THROWS
                    ( NamespaceDetailsTransient::getCursor
                     ( ns(), query(), order(), planPolicy(), &simpleEqualityMatch ),
                     MsgAssertionException );
                    return;
                }
                _query = query();
                _parsedQuery.reset( new ParsedQuery( ns(), skip(), limit(), 0, _query,
                                                    BSONObj() ) );
                BSONObj extractedQuery = _query;
                if ( !_query["$query"].eoo() ) {
                    extractedQuery = _query["$query"].Obj();
                }
                shared_ptr<Cursor> c =
                NamespaceDetailsTransient::getCursor( ns(), extractedQuery, order(), planPolicy(),
                                                     &simpleEqualityMatch, _parsedQuery.get() );
                ASSERT_EQUALS( expectSimpleEquality(), simpleEqualityMatch );
                string type = c->toString().substr( 0, expectedType().length() );
                ASSERT_EQUALS( expectedType(), type );
                check( c );
            }
        protected:
            virtual string expectedType() const { return "TESTDUMMY"; }
            virtual bool expectException() const { return false; }
            virtual bool expectSimpleEquality() const { return false; }
            virtual BSONObj query() const { return BSONObj(); }
            virtual BSONObj order() const { return BSONObj(); }
            virtual long long skip() const { return 0; }
            virtual long long limit() const { return 0; }
            virtual const QueryPlanSelectionPolicy &planPolicy() const {
                return QueryPlanSelectionPolicy::any();
            }
            virtual void check( const shared_ptr<Cursor> &c ) {
                ASSERT( c->ok() );
                ASSERT( !c->matcher() );
                ASSERT_EQUALS( 5, c->current().getIntField( "_id" ) );
                ASSERT( !c->advance() );
            }
        private:
            BSONObj _query;
            shared_ptr<ParsedQuery> _parsedQuery;
        };
        
        class NoConstraints : public Base {
            string expectedType() const { return "BasicCursor"; }
        };
        
        class SimpleId : public Base {
        public:
            SimpleId() {
                _cli.insert( ns(), BSON( "_id" << 0 ) );
                _cli.insert( ns(), BSON( "_id" << 10 ) );
            }
            string expectedType() const { return "BtreeCursor _id_"; }
            BSONObj query() const { return BSON( "_id" << 5 ); }
        };
        
        class OptimalIndex : public Base {
        public:
            OptimalIndex() {
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                _cli.insert( ns(), BSON( "a" << 5 ) );
                _cli.insert( ns(), BSON( "a" << 6 ) );
            }
            string expectedType() const { return "BtreeCursor a_1"; }
            BSONObj query() const { return BSON( "a" << GTE << 5 ); }
            void check( const shared_ptr<Cursor> &c ) {
                ASSERT( c->ok() );
                ASSERT( c->matcher() );
                ASSERT_EQUALS( 5, c->current().getIntField( "a" ) );
                ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( c->advance() );                    
                ASSERT_EQUALS( 6, c->current().getIntField( "a" ) );
                ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( !c->advance() );                    
            }
        };
        
        class SimpleKeyMatch : public Base {
        public:
            SimpleKeyMatch() {
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                _cli.update( ns(), BSONObj(), BSON( "$set" << BSON( "a" << true ) ) );
            }
            string expectedType() const { return "BtreeCursor a_1"; }
            bool expectSimpleEquality() const { return true; }
            BSONObj query() const { return BSON( "a" << true ); }
            virtual void check( const shared_ptr<Cursor> &c ) {
                ASSERT( c->ok() );
                ASSERT_EQUALS( 5, c->current().getIntField( "_id" ) );
                ASSERT( !c->advance() );
            }
        };
        
        class Geo : public Base {
        public:
            Geo() {
                _cli.insert( ns(), BSON( "_id" << 44 << "loc" << BSON_ARRAY( 44 << 45 ) ) );
                _cli.ensureIndex( ns(), BSON( "loc" << "2d" ) );
            }
            string expectedType() const { return "GeoSearchCursor"; }
            BSONObj query() const { return fromjson( "{ loc : { $near : [50,50] } }" ); }
            void check( const shared_ptr<Cursor> &c ) {
                ASSERT( c->ok() );
                ASSERT( c->matcher() );
                ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                ASSERT_EQUALS( 44, c->current().getIntField( "_id" ) );
                ASSERT( !c->advance() );
            }
        };
        
        class GeoNumWanted : public Base {
        public:
            GeoNumWanted() {
                _cli.ensureIndex( ns(), BSON( "loc" << "2d" ) );
                for( int i = 0; i < 140; ++i ) {
                    _cli.insert( ns(), BSON( "loc" << BSON_ARRAY( 44 << 45 ) ) );
                }
            }
            string expectedType() const { return "GeoSearchCursor"; }
            BSONObj query() const { return fromjson( "{ loc : { $near : [50,50] } }" ); }
            void check( const shared_ptr<Cursor> &c ) {
                int count = 0;
                while( c->ok() ) {
                    ++count;
                    c->advance();
                }
                ASSERT_EQUALS( 130, count );
            }
            long long skip() const { return 27; }
            long long limit() const { return 103; }
        };
        
        class PreventOutOfOrderPlan : public QueryOptimizerCursorTests::Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 5 ) );
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                shared_ptr<Cursor> c = NamespaceDetailsTransient::getCursor( ns(), BSONObj(), BSON( "b" << 1 ) );
                ASSERT( !c );
            }
        };
        
        class AllowOutOfOrderPlan : public Base {
        public:
            void run() {
                Lock::DBWrite lk(ns());
                Client::Context ctx( ns() );
                ParsedQuery parsedQuery
                ( ns(), 0, 0, 0, BSON( "$query" << BSONObj() << "$orderby" << BSON( "a" << 1 ) ),
                 BSONObj() );
                shared_ptr<Cursor> c =
                NamespaceDetailsTransient::getCursor( ns(), BSONObj(), BSON( "a" << 1 ), QueryPlanSelectionPolicy::any(), 0,
                                                     &parsedQuery );
                ASSERT( c );
            }
        };
        
        class BestSavedOutOfOrder : public QueryOptimizerCursorTests::Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 5 << "b" << BSON_ARRAY( 1 << 2 << 3 << 4 << 5 ) ) );
                _cli.insert( ns(), BSON( "_id" << 1 << "b" << 6 ) );
                _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
                // record {_id:1} index for this query
                ASSERT( _cli.query( ns(), QUERY( "_id" << GT << 0 << "b" << GT << 0 ).sort( "b" ) )->more() );
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                shared_ptr<Cursor> c = NamespaceDetailsTransient::getCursor( ns(), BSON( "_id" << GT << 0 << "b" << GT << 0 ), BSON( "b" << 1 ) );
                // {_id:1} requires scan and order, so {b:1} must be chosen.
                ASSERT( c );
                ASSERT_EQUALS( 5, c->current().getIntField( "_id" ) );
            }
        };

        /**
         * If an optimal plan is a candidate, return a cursor for it rather than a QueryOptimizerCursor.  Avoid
         * caching optimal plans since simple cursors will not save a plan anyway (so in the most common case optimal
         * plans won't be cached) and because this simplifies the implementation for selecting a simple cursor.
         */
        class BestSavedOptimal : public QueryOptimizerCursorTests::Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 1 ) );
                _cli.ensureIndex( ns(), BSON( "_id" << 1 << "q" << 1 ) );
                // {_id:1} index not recorded for these queries since it is an optimal index.
                ASSERT( _cli.query( ns(), QUERY( "_id" << GT << 0 ) )->more() );
                ASSERT( _cli.query( ns(), QUERY( "$or" << BSON_ARRAY( BSON( "_id" << GT << 0 ) ) ) )->more() );
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                // Check that no plan was recorded for this query.
                ASSERT( BSONObj().woCompare( NamespaceDetailsTransient::get_inlock( ns() ).indexForPattern( FieldRangeSet( ns(), BSON( "_id" << GT << 0 ), true ).pattern() ) ) == 0 );
                shared_ptr<Cursor> c = NamespaceDetailsTransient::getCursor( ns(), BSON( "_id" << GT << 0 ) );
                // No need for query optimizer cursor since the plan is optimal.
                ASSERT_EQUALS( "BtreeCursor _id_", c->toString() );
            }
        };
        
        /** If a no optimal plan is a candidate a QueryOptimizerCursor should be returned, even if plan has been recorded. */
        class BestSavedNotOptimal : public QueryOptimizerCursorTests::Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 1 << "q" << 1 ) );
                _cli.ensureIndex( ns(), BSON( "q" << 1 ) );
                // Record {_id:1} index for this query
                ASSERT( _cli.query( ns(), QUERY( "q" << 1 << "_id" << 1 ) )->more() );
                Lock::GlobalWrite lk;
                Client::Context ctx( ns() );
                ASSERT( BSON( "_id" << 1 ).woCompare( NamespaceDetailsTransient::get_inlock( ns() ).indexForPattern( FieldRangeSet( ns(), BSON( "q" << 1 << "_id" << 1 ), true ).pattern() ) ) == 0 );
                shared_ptr<Cursor> c = NamespaceDetailsTransient::getCursor( ns(), BSON( "q" << 1 << "_id" << 1 ) );
                // Need query optimizer cursor since the cached plan is not optimal.
                ASSERT_EQUALS( "QueryOptimizerCursor", c->toString() );
            }
        };
                
        class MultiIndex : public Base {
        public:
            MultiIndex() {
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            }
            string expectedType() const { return "QueryOptimizerCursor"; }
            BSONObj query() const { return BSON( "_id" << GT << 0 << "a" << GT << 0 ); }
            void check( const shared_ptr<Cursor> &c ) {}
        };
        
        class Hint : public Base {
        public:
            Hint() {
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            }
            string expectedType() const { return "BtreeCursor a_1"; }
            BSONObj query() const {
                return BSON( "$query" << BSON( "_id" << 1 ) << "$hint" << BSON( "a" << 1 ) );
            }
            void check( const shared_ptr<Cursor> &c ) {}
        };
        
        class Snapshot : public Base {
        public:
            Snapshot() {
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            }
            string expectedType() const { return "BtreeCursor _id_"; }
            BSONObj query() const {
                return BSON( "$query" << BSON( "a" << 1 ) << "$snapshot" << true );
            }
            void check( const shared_ptr<Cursor> &c ) {}            
        };
        
        class SnapshotWithoutIdIndex : public Base {
        public:
            SnapshotWithoutIdIndex() {
                _cli.dropCollection( ns() );
                _cli.createCollection( ns(), 1000, true );
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            }
            string expectedType() const { return "BtreeCursor a_1"; }
            BSONObj query() const {
                return BSON( "$query" << BSON( "a" << 1 ) << "$snapshot" << true );
            }
            void check( const shared_ptr<Cursor> &c ) {}            
        };
        
        class Min : public Base {
        public:
            Min() {
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            }
            string expectedType() const { return "BtreeCursor a_1"; }
            BSONObj query() const {
                return BSON( "$query" << BSONObj() << "$min" << BSON( "a" << 1 ) );
            }
            void check( const shared_ptr<Cursor> &c ) {}            
        };
        
        class Max : public Base {
        public:
            Max() {
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            }
            string expectedType() const { return "BtreeCursor a_1"; }
            BSONObj query() const {
                return BSON( "$query" << BSONObj() << "$max" << BSON( "a" << 1 ) );
            }
            void check( const shared_ptr<Cursor> &c ) {}            
        };
        
        namespace RequireIndex {
            
            class Base : public GetCursor::Base {
                const QueryPlanSelectionPolicy &planPolicy() const {
                    return QueryPlanSelectionPolicy::indexOnly();
                }
            };
            
            class NoConstraints : public Base {
                bool expectException() const { return true; }
            };

            class SimpleId : public Base {
                string expectedType() const { return "BtreeCursor _id_"; }
                BSONObj query() const { return BSON( "_id" << 5 ); }
            };

            class UnindexedQuery : public Base {
                bool expectException() const { return true; }
                BSONObj query() const { return BSON( "a" << GTE << 5 ); }
            };

            class IndexedQuery : public Base {
            public:
                IndexedQuery() {
                    _cli.insert( ns(), BSON( "_id" << 6 << "a" << 6 << "c" << 4 ) );
                    _cli.ensureIndex( ns(), BSON( "a" << 1 << "b" << 1 << "c" << 1 ) );
                }
                string expectedType() const { return "QueryOptimizerCursor"; }
                BSONObj query() const { return BSON( "a" << GTE << 5 << "c" << 4 ); }
                void check( const shared_ptr<Cursor> &c ) {
                    ASSERT( c->ok() );
                    ASSERT( c->matcher() );
                    ASSERT_EQUALS( 6, c->current().getIntField( "_id" ) );
                    ASSERT( !c->advance() );
                }
            };

            class SecondOrClauseIndexed : public Base {
            public:
                SecondOrClauseIndexed() {
                    _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                    _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
                    _cli.insert( ns(), BSON( "a" << 1 ) );
                    _cli.insert( ns(), BSON( "b" << 1 ) );
                }
                string expectedType() const { return "QueryOptimizerCursor"; }
                BSONObj query() const { return fromjson( "{$or:[{a:1},{b:1}]}" ); }
                void check( const shared_ptr<Cursor> &c ) {
                    ASSERT( c->ok() );
                    ASSERT( c->matcher() );
                    ASSERT( c->advance() );
                    ASSERT( !c->advance() ); // 2 matches exactly
                }
            };
            
            class SecondOrClauseUnindexed : public Base {
            public:
                SecondOrClauseUnindexed() {
                    _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                    _cli.insert( ns(), BSON( "a" << 1 ) );
                }
                bool expectException() const { return true; }
                BSONObj query() const { return fromjson( "{$or:[{a:1},{b:1}]}" ); }
            };

            class SecondOrClauseUnindexedUndetected : public Base {
            public:
                SecondOrClauseUnindexedUndetected() {
                    _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                    _cli.ensureIndex( ns(), BSON( "a" << 1 << "b" << 1 ) );
                    _cli.insert( ns(), BSON( "a" << 1 ) );
                    _cli.insert( ns(), BSON( "b" << 1 ) );
                }
                string expectedType() const { return "QueryOptimizerCursor"; }
                BSONObj query() const { return fromjson( "{$or:[{a:1},{b:1}]}" ); }
                void check( const shared_ptr<Cursor> &c ) {
                    ASSERT( c->ok() );
                    ASSERT( c->matcher() );
                    // An unindexed cursor is required for the second clause, but is not allowed.
                    ASSERT_THROWS( c->advance(), MsgAssertionException );
                }
            };
            
            class RecordedUnindexedPlan : public Base {
            public:
                RecordedUnindexedPlan() {
                    _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                    _cli.insert( ns(), BSON( "a" << BSON_ARRAY( 1 << 2 << 3 ) << "b" << 1 ) );
                    auto_ptr<DBClientCursor> cursor =
                    _cli.query( ns(), QUERY( "a" << GT << 0 << "b" << 1 ).explain() );
                    BSONObj explain = cursor->next();
                    ASSERT_EQUALS( "BasicCursor", explain[ "cursor" ].String() );
                }
                string expectedType() const { return "QueryOptimizerCursor"; }
                BSONObj query() const { return BSON( "a" << GT << 0 << "b" << 1 ); }
                void check( const shared_ptr<Cursor> &c ) {
                    ASSERT( c->ok() );
                    ASSERT_EQUALS( BSON( "a" << 1 ), c->indexKeyPattern() );
                    while( c->advance() ) {
                        ASSERT_EQUALS( BSON( "a" << 1 ), c->indexKeyPattern() );                    
                    }
                }
            };
                
        } // namespace RequireIndex
        
        namespace IdElseNatural {

            class Base : public GetCursor::Base {
                const QueryPlanSelectionPolicy &planPolicy() const {
                    return QueryPlanSelectionPolicy::idElseNatural();
                }
            };
            
            class AllowOptimalNaturalPlan : public Base {
                string expectedType() const { return "BasicCursor"; }
                void check( const shared_ptr<Cursor> &c ) {
                    ASSERT( c->ok() );
                    ASSERT( !c->matcher() );
                    ASSERT_EQUALS( 5, c->current().getIntField( "_id" ) );
                    ASSERT( !c->advance() );
                }
            };

            class AllowOptimalIdPlan : public Base {
                string expectedType() const { return "BtreeCursor _id_"; }
                BSONObj query() const { return BSON( "_id" << 5 ); }
            };

            class HintedIdForQuery : public Base {
            public:
                HintedIdForQuery( const BSONObj &query ) : _query( query ) {
                    _cli.remove( ns(), BSONObj() );
                    _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                    _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 ) );
                }
                string expectedType() const { return "BtreeCursor _id_"; }
                BSONObj query() const { return _query; }
                void check( const shared_ptr<Cursor> &c ) {
                    ASSERT( c->ok() );
                    ASSERT( c->currentMatches() );
                    ASSERT_EQUALS( 1, c->current().getIntField( "_id" ) );
                    ASSERT( !c->advance() );
                }
            private:
                BSONObj _query;
            };

            class HintedNaturalForQuery : public Base {
            public:
                HintedNaturalForQuery( const BSONObj &query ) : _query( query ) {
                    _cli.dropCollection( ns() );
                    _cli.createCollection( ns(), 1024, true );
                    _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                    _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 ) );
                }
                ~HintedNaturalForQuery() {
                    _cli.dropCollection( ns() );
                }
                string expectedType() const { return "ForwardCappedCursor"; }
                BSONObj query() const { return _query; }
                void check( const shared_ptr<Cursor> &c ) {
                    ASSERT( c->ok() );
                    ASSERT( c->currentMatches() );
                    ASSERT_EQUALS( 1, c->current().getIntField( "_id" ) );
                    ASSERT( !c->advance() );
                }
            private:
                BSONObj _query;
            };

        } // namespace IdElseNatural
        
    } // namespace GetCursor
    
    namespace Explain {
        
        class ClearRecordedIndex : public QueryOptimizerCursorTests::Base {
        public:
            void run() {
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
                _cli.insert( ns(), BSON( "a" << 1 << "b" << 1 ) );
                _cli.insert( ns(), BSON( "a" << 1 << "b" << 1 ) );
                
                Lock::DBWrite lk(ns());
                Client::Context ctx( ns() );
                BSONObj query = BSON( "a" << 1 << "b" << 1 );
                shared_ptr<Cursor> c =
                NamespaceDetailsTransient::getCursor( ns(), query );
                while( c->advance() );
                ParsedQuery parsedQuery( ns(), 0, 0, 0,
                                        BSON( "$query" << query << "$explain" << true ),
                                        BSONObj() );
                c = NamespaceDetailsTransient::getCursor( ns(), query, BSONObj(), QueryPlanSelectionPolicy::any(), 0,
                                                         &parsedQuery );
                set<BSONObj> indexKeys;
                while( c->ok() ) {
                    indexKeys.insert( c->indexKeyPattern() );
                    c->advance();
                }
                ASSERT( indexKeys.size() > 1 );
            }
        };
        
        class Base : public QueryOptimizerCursorTests::Base {
        public:
            virtual ~Base() {}
            void run() {
                setupCollection();
                
                Lock::DBWrite lk(ns());
                Client::Context ctx( ns() );
                ParsedQuery parsedQuery( ns(), 0, 0, 0,
                                        BSON( "$query" << query() << "$explain" << true ),
                                        fields() );
                _cursor =
                dynamic_pointer_cast<QueryOptimizerCursor>
                ( NamespaceDetailsTransient::getCursor( ns(), query(), BSONObj(), QueryPlanSelectionPolicy::any(), 0,
                                                       &parsedQuery ) );
                ASSERT( _cursor );
                
                handleCursor();
                
                _explainInfo = _cursor->explainQueryInfo();
                _explain = _explainInfo->bson();

                checkExplain();
            }
        protected:
            virtual void setupCollection() {
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                _cli.insert( ns(), BSON( "a" << 1 << "b" << 1 ) );
                _cli.insert( ns(), BSON( "a" << 2 << "b" << 1 ) );
            }
            virtual BSONObj query() const { return BSON( "a" << 1 << "b" << 1 ); }
            virtual BSONObj fields() const { return BSONObj(); }
            virtual void handleCursor() {
            }
            virtual void checkExplain() {
            }
            shared_ptr<QueryOptimizerCursor> _cursor;
            shared_ptr<ExplainQueryInfo> _explainInfo;
            BSONObj _explain;
        };
        
        class Initial : public Base {
            virtual void checkExplain() {
                ASSERT( !_explain[ "cursor" ].eoo() );
                ASSERT( !_explain[ "isMultiKey" ].Bool() );
                ASSERT_EQUALS( 0, _explain[ "n" ].Long() );
                ASSERT_EQUALS( 0, _explain[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 2, _explain[ "nscanned" ].Long() );
                ASSERT( !_explain[ "scanAndOrder" ].Bool() );
                ASSERT( !_explain[ "indexOnly" ].Bool() );
                ASSERT_EQUALS( 0, _explain[ "nYields" ].Int() );
                ASSERT_EQUALS( 0, _explain[ "nChunkSkips" ].Long() );
                ASSERT( !_explain[ "millis" ].eoo() );
                ASSERT( !_explain[ "indexBounds" ].eoo() );
                ASSERT_EQUALS( 2U, _explain[ "allPlans" ].Array().size() );

                BSONObj plan1 = _explain[ "allPlans" ].Array()[ 0 ].Obj();
                ASSERT_EQUALS( "BtreeCursor a_1", plan1[ "cursor" ].String() );
                ASSERT_EQUALS( 0, plan1[ "n" ].Long() );
                ASSERT_EQUALS( 0, plan1[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 1, plan1[ "nscanned" ].Long() );
                ASSERT_EQUALS( fromjson( "{a:[[1,1]]}" ), plan1[ "indexBounds" ].Obj() );

                BSONObj plan2 = _explain[ "allPlans" ].Array()[ 1 ].Obj();
                ASSERT_EQUALS( "BasicCursor", plan2[ "cursor" ].String() );
                ASSERT_EQUALS( 0, plan2[ "n" ].Long() );
                ASSERT_EQUALS( 0, plan2[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 1, plan2[ "nscanned" ].Long() );
                ASSERT_EQUALS( BSONObj(), plan2[ "indexBounds" ].Obj() );
            }
        };
        
        class Empty : public Base {
            virtual void setupCollection() {
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
            }
            virtual void handleCursor() {
                ASSERT( !_cursor->ok() );
            }
            virtual void checkExplain() {
                ASSERT( !_explain[ "cursor" ].eoo() );
                ASSERT( !_explain[ "isMultiKey" ].Bool() );
                ASSERT_EQUALS( 0, _explain[ "n" ].Long() );
                ASSERT_EQUALS( 0, _explain[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 0, _explain[ "nscanned" ].Long() );
                ASSERT( !_explain[ "scanAndOrder" ].Bool() );
                ASSERT( !_explain[ "indexOnly" ].Bool() );
                ASSERT_EQUALS( 0, _explain[ "nYields" ].Int() );
                ASSERT_EQUALS( 0, _explain[ "nChunkSkips" ].Long() );
                ASSERT( !_explain[ "millis" ].eoo() );
                ASSERT( !_explain[ "indexBounds" ].eoo() );
                ASSERT_EQUALS( 2U, _explain[ "allPlans" ].Array().size() );
                
                BSONObj plan1 = _explain[ "allPlans" ].Array()[ 0 ].Obj();
                ASSERT_EQUALS( "BtreeCursor a_1", plan1[ "cursor" ].String() );
                ASSERT_EQUALS( 0, plan1[ "n" ].Long() );
                ASSERT_EQUALS( 0, plan1[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 0, plan1[ "nscanned" ].Long() );
                ASSERT_EQUALS( fromjson( "{a:[[1,1]]}" ), plan1[ "indexBounds" ].Obj() );
                
                BSONObj plan2 = _explain[ "allPlans" ].Array()[ 1 ].Obj();
                ASSERT_EQUALS( "BasicCursor", plan2[ "cursor" ].String() );
                ASSERT_EQUALS( 0, plan2[ "n" ].Long() );
                ASSERT_EQUALS( 0, plan2[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 0, plan2[ "nscanned" ].Long() );
                ASSERT_EQUALS( BSONObj(), plan2[ "indexBounds" ].Obj() );                
            }
        };
        
        class SimpleCount : public Base {
            virtual void handleCursor() {
                while( _cursor->ok() ) {
                    MatchDetails matchDetails;
                    if ( _cursor->currentMatches( &matchDetails ) &&
                        !_cursor->getsetdup( _cursor->currLoc() ) ) {
                        _cursor->noteIterate( true, true, false );
                    }
                    else {
                        _cursor->noteIterate( false, matchDetails._loadedObject, false );
                    }
                    _cursor->advance();
                }
            }
            virtual void checkExplain() {
                ASSERT_EQUALS( "BtreeCursor a_1", _explain[ "cursor" ].String() );
                ASSERT_EQUALS( 1, _explain[ "n" ].Long() );
                ASSERT_EQUALS( 2, _explain[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 2, _explain[ "nscanned" ].Long() );

                BSONObj plan1 = _explain[ "allPlans" ].Array()[ 0 ].Obj();
                ASSERT_EQUALS( 1, plan1[ "n" ].Long() );
                ASSERT_EQUALS( 1, plan1[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 1, plan1[ "nscanned" ].Long() );

                BSONObj plan2 = _explain[ "allPlans" ].Array()[ 1 ].Obj();
                ASSERT_EQUALS( 1, plan2[ "n" ].Long() );
                ASSERT_EQUALS( 1, plan2[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 1, plan2[ "nscanned" ].Long() );
            }
        };
        
        class IterateOnly : public Base {
            virtual BSONObj query() const { return BSON( "a" << GT << 0 << "b" << 1 ); }
            virtual void handleCursor() {
                while( _cursor->ok() ) {
                    _cursor->advance();
                }
            }
            virtual void checkExplain() {
                ASSERT_EQUALS( "BtreeCursor a_1", _explain[ "cursor" ].String() );
                ASSERT_EQUALS( 0, _explain[ "n" ].Long() ); // needs to be set with noteIterate()
                ASSERT_EQUALS( 0, _explain[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 3, _explain[ "nscanned" ].Long() );

                BSONObj plan1 = _explain[ "allPlans" ].Array()[ 0 ].Obj();
                ASSERT_EQUALS( 2, plan1[ "n" ].Long() );
                ASSERT_EQUALS( 2, plan1[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 2, plan1[ "nscanned" ].Long() );
                
                // Not fully incremented without checking for matches.
                BSONObj plan2 = _explain[ "allPlans" ].Array()[ 1 ].Obj();
                ASSERT_EQUALS( 1, plan2[ "n" ].Long() );
                ASSERT_EQUALS( 1, plan2[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 1, plan2[ "nscanned" ].Long() );                
            }
        };
        
        class ExtraMatchChecks : public Base {
            virtual BSONObj query() const { return BSON( "a" << GT << 0 << "b" << 1 ); }
            virtual void handleCursor() {
                while( _cursor->ok() ) {
                    _cursor->currentMatches();
                    _cursor->currentMatches();
                    _cursor->currentMatches();
                    _cursor->advance();
                }
            }
            virtual void checkExplain() {
                ASSERT_EQUALS( "BtreeCursor a_1", _explain[ "cursor" ].String() );
                ASSERT_EQUALS( 0, _explain[ "n" ].Long() ); // needs to be set with noteIterate()
                ASSERT_EQUALS( 0, _explain[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 4, _explain[ "nscanned" ].Long() );
                
                BSONObj plan1 = _explain[ "allPlans" ].Array()[ 0 ].Obj();
                ASSERT_EQUALS( 2, plan1[ "n" ].Long() );
                // nscannedObjects are not deduped.
                ASSERT_EQUALS( 6, plan1[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 2, plan1[ "nscanned" ].Long() );
                
                BSONObj plan2 = _explain[ "allPlans" ].Array()[ 1 ].Obj();
                ASSERT_EQUALS( 2, plan2[ "n" ].Long() );
                ASSERT_EQUALS( 6, plan2[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 2, plan2[ "nscanned" ].Long() );                
            }
        };
        
        class PartialIteration : public Base {
            virtual void handleCursor() {
                _cursor->currentMatches();
                _cursor->advance();
                _cursor->noteIterate( true, true, false );
            }
            virtual void checkExplain() {
                ASSERT_EQUALS( "BtreeCursor a_1", _explain[ "cursor" ].String() );
                ASSERT_EQUALS( 1, _explain[ "n" ].Long() );
                ASSERT_EQUALS( 1, _explain[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 2, _explain[ "nscanned" ].Long() );
                
                BSONObj plan1 = _explain[ "allPlans" ].Array()[ 0 ].Obj();
                ASSERT_EQUALS( 1, plan1[ "n" ].Long() );
                ASSERT_EQUALS( 1, plan1[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 1, plan1[ "nscanned" ].Long() );
                
                BSONObj plan2 = _explain[ "allPlans" ].Array()[ 1 ].Obj();
                ASSERT_EQUALS( 0, plan2[ "n" ].Long() );
                ASSERT_EQUALS( 0, plan2[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 1, plan2[ "nscanned" ].Long() );                
            }
        };
        
        class Multikey : public Base {
            virtual void setupCollection() {
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                _cli.insert( ns(), BSON( "a" << BSON_ARRAY( 1 << 2 ) ) );
            }
            virtual void handleCursor() {
                while( _cursor->advance() );
            }
            virtual void checkExplain() {
                ASSERT( _explain[ "isMultiKey" ].Bool() );
            }            
        };
        
        class MultikeyInitial : public Base {
            virtual void setupCollection() {
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                _cli.insert( ns(), BSON( "a" << BSON_ARRAY( 1 << 2 ) ) );
            }
            virtual void checkExplain() {
                ASSERT( _explain[ "isMultiKey" ].Bool() );
            }
        };

        class BecomesMultikey : public Base {
            virtual void setupCollection() {
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                _cli.insert( ns(), BSON( "a" << 1 ) );
            }
            virtual void checkExplain() {
                ASSERT( !_explain[ "isMultiKey" ].Bool() );
                
                _cursor->prepareToYield();
                {
                    dbtemprelease t;
                    _cli.insert( ns(), BSON( "a" << BSON_ARRAY( 1 << 2 ) ) );
                }
                _cursor->recoverFromYield();
                _cursor->currentMatches();
                ASSERT( _explainInfo->bson()[ "isMultiKey" ].Bool() );
            }
        };
        
        class CountAndYield : public Base {
            virtual void setupCollection() {
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                for( int i = 0; i < 5; ++i ) {
                    _cli.insert( ns(), BSON( "a" << 1 << "b" << 1 ) );
                }
            }
            virtual void handleCursor() {
                _nYields = 0;
                while( _cursor->ok() ) {
                    _cursor->prepareToYield();
                    ++_nYields;
                    _cursor->recoverFromYield();
                    MatchDetails matchDetails;
                    if ( _cursor->currentMatches( &matchDetails ) &&
                        !_cursor->getsetdup( _cursor->currLoc() ) ) {
                        _cursor->noteIterate( true, true, false );
                    }
                    else {
                        _cursor->noteIterate( false, matchDetails._loadedObject, false );
                    }
                    _cursor->advance();
                }
            }
            virtual void checkExplain() {
                ASSERT( _nYields > 0 );
                ASSERT_EQUALS( _nYields, _explain[ "nYields" ].Int() );
                
                ASSERT_EQUALS( "BtreeCursor a_1", _explain[ "cursor" ].String() );
                ASSERT_EQUALS( 5, _explain[ "n" ].Long() );
                ASSERT_EQUALS( 10, _explain[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 10, _explain[ "nscanned" ].Long() );
                
                BSONObj plan1 = _explain[ "allPlans" ].Array()[ 0 ].Obj();
                ASSERT_EQUALS( 5, plan1[ "n" ].Long() );
                ASSERT_EQUALS( 5, plan1[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 5, plan1[ "nscanned" ].Long() );
                
                BSONObj plan2 = _explain[ "allPlans" ].Array()[ 1 ].Obj();
                ASSERT_EQUALS( 5, plan2[ "n" ].Long() );
                ASSERT_EQUALS( 5, plan2[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 5, plan2[ "nscanned" ].Long() );                
            }
        protected:
            int _nYields;
        };
        
        class MultipleClauses : public CountAndYield {
            virtual void setupCollection() {
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
                for( int i = 0; i < 4; ++i ) {
                    _cli.insert( ns(), BSON( "a" << 1 << "b" << 1 ) );
                }
                _cli.insert( ns(), BSON( "a" << 0 << "b" << 1 ) );
            }
            virtual BSONObj query() const { return fromjson( "{$or:[{a:1,c:null},{b:1,c:null}]}"); }
            virtual void checkExplain() {
                ASSERT_EQUALS( 18, _nYields );

                ASSERT_EQUALS( 5, _explain[ "n" ].Long() );
                ASSERT_EQUALS( 18, _explain[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 18, _explain[ "nscanned" ].Long() );

                BSONObj clause1 = _explain[ "clauses" ].Array()[ 0 ].Obj();
                ASSERT_EQUALS( "BtreeCursor a_1", clause1[ "cursor" ].String() );
                ASSERT_EQUALS( 4, clause1[ "n" ].Long() );
                ASSERT_EQUALS( 8, clause1[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 8, clause1[ "nscanned" ].Long() );
                ASSERT_EQUALS( 8, clause1[ "nYields" ].Int() );
                
                BSONObj c1plan1 = clause1[ "allPlans" ].Array()[ 0 ].Obj();
                ASSERT_EQUALS( "BtreeCursor a_1", c1plan1[ "cursor" ].String() );
                ASSERT_EQUALS( 4, c1plan1[ "n" ].Long() );
                ASSERT_EQUALS( 4, c1plan1[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 4, c1plan1[ "nscanned" ].Long() );

                BSONObj c1plan2 = clause1[ "allPlans" ].Array()[ 1 ].Obj();
                ASSERT_EQUALS( "BasicCursor", c1plan2[ "cursor" ].String() );
                ASSERT_EQUALS( 4, c1plan2[ "n" ].Long() );
                ASSERT_EQUALS( 4, c1plan2[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 4, c1plan2[ "nscanned" ].Long() );

                BSONObj clause2 = _explain[ "clauses" ].Array()[ 1 ].Obj();
                ASSERT_EQUALS( "BtreeCursor b_1", clause2[ "cursor" ].String() );
                ASSERT_EQUALS( 1, clause2[ "n" ].Long() );
                ASSERT_EQUALS( 10, clause2[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 10, clause2[ "nscanned" ].Long() );
                ASSERT_EQUALS( 10, clause2[ "nYields" ].Int() );

                BSONObj c2plan1 = clause2[ "allPlans" ].Array()[ 0 ].Obj();
                ASSERT_EQUALS( "BtreeCursor b_1", c2plan1[ "cursor" ].String() );
                ASSERT_EQUALS( 1, c2plan1[ "n" ].Long() );
                ASSERT_EQUALS( 5, c2plan1[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 5, c2plan1[ "nscanned" ].Long() );
                
                BSONObj c2plan2 = clause2[ "allPlans" ].Array()[ 1 ].Obj();
                ASSERT_EQUALS( "BasicCursor", c2plan2[ "cursor" ].String() );
                ASSERT_EQUALS( 1, c2plan2[ "n" ].Long() );
                ASSERT_EQUALS( 5, c2plan2[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 5, c2plan2[ "nscanned" ].Long() );
            }
        };
        
        class MultiCursorTakeover : public CountAndYield {
            virtual void setupCollection() {
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                for( int i = 20; i >= 1; --i ) {
                    for( int j = 0; j < i; ++j ) {
                        _cli.insert( ns(), BSON( "a" << i ) );
                    }
                }
            }
            virtual BSONObj query() const {
                BSONArrayBuilder bab;
                for( int i = 20; i >= 1; --i ) {
                    bab << BSON( "a" << i );
                }
                return BSON( "$or" << bab.arr() );
            }
            virtual void checkExplain() {
                ASSERT_EQUALS( 20U, _explain[ "clauses" ].Array().size() );
                for( int i = 20; i >= 1; --i ) {
                    BSONObj clause = _explain[ "clauses" ].Array()[ 20-i ].Obj();
                    ASSERT_EQUALS( "BtreeCursor a_1", clause[ "cursor" ].String() );
                    ASSERT_EQUALS( BSON( "a" << BSON_ARRAY( BSON_ARRAY( i << i ) ) ),
                                  clause[ "indexBounds" ].Obj() );
                    ASSERT_EQUALS( i, clause[ "n" ].Long() );
                    ASSERT_EQUALS( i, clause[ "nscannedObjects" ].Long() );
                    ASSERT_EQUALS( i, clause[ "nscanned" ].Long() );
                    ASSERT_EQUALS( i, clause[ "nYields" ].Int() );
                    
                    ASSERT_EQUALS( 1U, clause[ "allPlans" ].Array().size() );
                    BSONObj plan = clause[ "allPlans" ].Array()[ 0 ].Obj();
                    ASSERT_EQUALS( i, plan[ "n" ].Long() );
                    ASSERT_EQUALS( i, plan[ "nscannedObjects" ].Long() );
                    ASSERT_EQUALS( i, plan[ "nscanned" ].Long() );                    
                }
                
                ASSERT_EQUALS( 210, _explain[ "n" ].Long() );
                ASSERT_EQUALS( 210, _explain[ "nscannedObjects" ].Long() );
                ASSERT_EQUALS( 210, _explain[ "nscanned" ].Long() );
            }
        };
        
        class NChunkSkipsTakeover : public Base {
            virtual void setupCollection() {
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                for( int i = 0; i < 200; ++i ) {
                    _cli.insert( ns(), BSON( "a" << 1 << "b" << 1 ) );
                }
                for( int i = 0; i < 200; ++i ) {
                    _cli.insert( ns(), BSON( "a" << 2 << "b" << 2 ) );
                }
            }
            virtual BSONObj query() const { return fromjson( "{$or:[{a:1,b:1},{a:2,b:2}]}" ); }
            virtual void handleCursor() {
                ASSERT_EQUALS( "QueryOptimizerCursor", _cursor->toString() );
                int i = 0;
                while( _cursor->ok() ) {
                    if ( _cursor->currentMatches() && !_cursor->getsetdup( _cursor->currLoc() ) ) {
                        _cursor->noteIterate( true, true, i++ %2 == 0 );
                    }
                    _cursor->advance();
                }
            }
            virtual void checkExplain() {
                // Historically, nChunkSkips has been excluded from the query summary.
                ASSERT( _explain[ "nChunkSkips" ].eoo() );

                BSONObj clause0 = _explain[ "clauses" ].Array()[ 0 ].Obj();
                ASSERT_EQUALS( 100, clause0[ "nChunkSkips" ].Long() );
                BSONObj plan0 = clause0[ "allPlans" ].Array()[ 0 ].Obj();
                ASSERT( plan0[ "nChunkSkips" ].eoo() );

                BSONObj clause1 = _explain[ "clauses" ].Array()[ 1 ].Obj();
                ASSERT_EQUALS( 100, clause1[ "nChunkSkips" ].Long() );
                BSONObj plan1 = clause1[ "allPlans" ].Array()[ 0 ].Obj();
                ASSERT( plan1[ "nChunkSkips" ].eoo() );
            }            
        };

        class CoveredIndex : public Base {
            virtual BSONObj query() const { return fromjson( "{$or:[{a:1},{a:2}]}" ); }
            virtual BSONObj fields() const { return BSON( "_id" << 0 << "a" << 1 ); }
            virtual void handleCursor() {
                ASSERT_EQUALS( "QueryOptimizerCursor", _cursor->toString() );
                while( _cursor->advance() );
            }
            virtual void checkExplain() {
                BSONObj clause0 = _explain[ "clauses" ].Array()[ 0 ].Obj();
                ASSERT( clause0[ "indexOnly" ].Bool() );
                
                BSONObj clause1 = _explain[ "clauses" ].Array()[ 1 ].Obj();
                ASSERT( clause1[ "indexOnly" ].Bool() );
            }
        };

        class CoveredIndexTakeover : public Base {
            virtual void setupCollection() {
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                for( int i = 0; i < 150; ++i ) {
                    _cli.insert( ns(), BSON( "a" << 1 ) );
                }
                _cli.insert( ns(), BSON( "a" << 2 ) );
            }
            virtual BSONObj query() const { return fromjson( "{$or:[{a:1},{a:2}]}" ); }
            virtual BSONObj fields() const { return BSON( "_id" << 0 << "a" << 1 ); }
            virtual void handleCursor() {
                ASSERT_EQUALS( "QueryOptimizerCursor", _cursor->toString() );
                while( _cursor->advance() );
            }
            virtual void checkExplain() {
                BSONObj clause0 = _explain[ "clauses" ].Array()[ 0 ].Obj();
                ASSERT( clause0[ "indexOnly" ].Bool() );
                
                BSONObj clause1 = _explain[ "clauses" ].Array()[ 1 ].Obj();
                ASSERT( clause1[ "indexOnly" ].Bool() );
            }
        };
        
        /**
         * Check that the plan with the most matches is reported at the top of the explain output
         * in the absence of a done or picked plan.
         */
        class VirtualPickedPlan : public Base {
        public:
            void run() {
                dblock lk;
                Client::Context ctx( ns() );
                
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
                _cli.ensureIndex( ns(), BSON( "c" << 1 ) );
                
                shared_ptr<Cursor> aCursor
                ( NamespaceDetailsTransient::getCursor( ns(), BSON( "a" << 1 ) ) );
                shared_ptr<Cursor> bCursor
                ( NamespaceDetailsTransient::getCursor( ns(), BSON( "b" << 1 ) ) );
                shared_ptr<Cursor> cCursor
                ( NamespaceDetailsTransient::getCursor( ns(), BSON( "c" << 1 ) ) );
                
                shared_ptr<ExplainPlanInfo> aPlan( new ExplainPlanInfo() );
                aPlan->notePlan( *aCursor, false, false );
                shared_ptr<ExplainPlanInfo> bPlan( new ExplainPlanInfo() );
                bPlan->notePlan( *bCursor, false, false );
                shared_ptr<ExplainPlanInfo> cPlan( new ExplainPlanInfo() );
                cPlan->notePlan( *cCursor, false, false );
                
                aPlan->noteIterate( true, false, *aCursor ); // one match a
                bPlan->noteIterate( true, false, *bCursor ); // two matches b
                bPlan->noteIterate( true, false, *bCursor );
                cPlan->noteIterate( true, false, *cCursor ); // one match c
                
                shared_ptr<ExplainClauseInfo> clause( new ExplainClauseInfo() );
                clause->addPlanInfo( aPlan );
                clause->addPlanInfo( bPlan );
                clause->addPlanInfo( cPlan );
                
                ASSERT_EQUALS( "BtreeCursor b_1", clause->bson()[ "cursor" ].String() );
            }
        };

        // test takeover w/ mixed plan clause ? necessary?

    } // namespace Explain
    
    class All : public Suite {
    public:
        All() : Suite( "queryoptimizercursor" ) {}
        
        void setupTests() {
            __forceLinkGeoPlugin();
            add<CachedMatchCounterCount>();
            add<CachedMatchCounterAccumulate>();
            add<CachedMatchCounterDedup>();
            add<CachedMatchCounterNscanned>();
            add<SmallDupSetUpgrade>();
            add<CachedMatchCounterCount>();
            add<SmallDupSetUpgradeRead>();
            add<SmallDupSetUpgradeWrite>();
            add<DurationTimerStop>();
            add<Empty>();
            add<Unindexed>();
            add<Basic>();
            add<NoMatch>();
            add<Interleaved>();
            add<NotMatch>();
            add<StopInterleaving>();
            add<TakeoverWithDup>();
            add<TakeoverWithNonMatches>();
            add<TakeoverWithTakeoverDup>();
            add<BasicOr>();
            add<OrFirstClauseEmpty>();
            add<OrSecondClauseEmpty>();
            add<OrMultipleClausesEmpty>();
            add<TakeoverCountOr>();
            add<TakeoverEndOfOrClause>();
            add<TakeoverBeforeEndOfOrClause>();
            add<TakeoverAfterEndOfOrClause>();
            add<ManualMatchingDeduping>();
            add<ManualMatchingUsingCurrKey>();
            add<ManualMatchingDedupingTakeover>();
            add<Singlekey>();
            add<Multikey>();
            add<AddOtherPlans>();
            add<AddOtherPlansDelete>();
            add<AddOtherPlansContinuousDelete>();
            add<OrRangeElimination>();
            add<OrDedup>();
            add<EarlyDups>();
            add<OrPopInTakeover>();
            add<OrCollectionScanAbort>();
            add<YieldNoOp>();
            add<YieldDelete>();
            add<YieldDeleteContinue>();
            add<YieldDeleteContinueFurther>();
            add<YieldUpdate>();
            add<YieldDrop>();
            add<YieldDropOr>();
            add<YieldRemoveOr>();
            add<YieldCappedOverwrite>();
            add<YieldDropIndex>();
            add<YieldMultiplePlansNoOp>();
            add<YieldMultiplePlansAdvanceNoOp>();
            add<YieldMultiplePlansDelete>();
            add<YieldMultiplePlansDeleteOr>();
            add<YieldMultiplePlansDeleteOrAdvance>();
            add<YieldMultiplePlansCappedOverwrite>();
            add<YieldMultiplePlansCappedOverwriteManual>();
            add<YieldMultiplePlansCappedOverwriteManual2>();
            add<YieldTakeover>();
            add<YieldTakeoverBasic>();
            add<YieldInactiveCursorAdvance>();
            add<OrderId>();
            add<OrderMultiIndex>();
            add<OrderReject>();
            add<OrderNatural>();
            add<OrderUnindexed>();
            add<RecordedOrderInvalid>();
            add<KillOp>();
            add<KillOpFirstClause>();
            add<Nscanned>();
            add<TouchEarlierIterate>();
            add<TouchEarlierIterateDelete>();
            add<TouchEarlierIterateDeleteMultiple>();
            add<TouchEarlierIterateTakeover>();
            add<TouchEarlierIterateTakeoverDeleteMultiple>();
            add<TouchEarlierIterateUnindexedTakeoverDeleteMultiple>();
            add<TouchEarlierIterateTakeoverDeleteMultipleMultiAdvance>();
            add<InitialCappedWrapYieldRecoveryFailure>();
            add<TakeoverCappedWrapYieldRecoveryFailure>();
            add<InvalidateClientCursorHolder>();
            add<TimeoutClientCursorHolder>();
            add<AllowOutOfOrderPlan>();
            add<NoTakeoverByOutOfOrderPlan>();
            add<OutOfOrderOnlyTakeover>();
            add<CoveredIndex>();
            add<CoveredIndexTakeover>();
            add<SaveGoodIndex>();
            add<PossibleInOrderPlans>();
            add<PossibleOutOfOrderPlans>();
            add<PossibleBothPlans>();
            add<AbortOutOfOrderPlans>();
            add<AbortOutOfOrderPlanOnLastMatch>();
            add<TakeoverOrRangeElimination>();
            add<TakeoverOrDedups>();
            add<GetCursor::NoConstraints>();
            add<GetCursor::SimpleId>();
            add<GetCursor::OptimalIndex>();
            add<GetCursor::SimpleKeyMatch>();
            add<GetCursor::Geo>();
            add<GetCursor::GeoNumWanted>();
            add<GetCursor::PreventOutOfOrderPlan>();
            add<GetCursor::AllowOutOfOrderPlan>();
            add<GetCursor::BestSavedOutOfOrder>();
            add<GetCursor::BestSavedOptimal>();
            add<GetCursor::BestSavedNotOptimal>();
            add<GetCursor::MultiIndex>();
            add<GetCursor::Hint>();
            add<GetCursor::Snapshot>();
            add<GetCursor::SnapshotWithoutIdIndex>();
            add<GetCursor::Min>();
            add<GetCursor::Max>();
            add<GetCursor::RequireIndex::NoConstraints>();
            add<GetCursor::RequireIndex::SimpleId>();
            add<GetCursor::RequireIndex::UnindexedQuery>();
            add<GetCursor::RequireIndex::IndexedQuery>();
            add<GetCursor::RequireIndex::SecondOrClauseIndexed>();
            add<GetCursor::RequireIndex::SecondOrClauseUnindexed>();
            add<GetCursor::RequireIndex::SecondOrClauseUnindexedUndetected>();
            add<GetCursor::RequireIndex::RecordedUnindexedPlan>();
            add<GetCursor::IdElseNatural::AllowOptimalNaturalPlan>();
            add<GetCursor::IdElseNatural::AllowOptimalIdPlan>();
            add<GetCursor::IdElseNatural::HintedIdForQuery>( BSON( "_id" << 1 ) );
            add<GetCursor::IdElseNatural::HintedIdForQuery>( BSON( "a" << 1 ) );
            add<GetCursor::IdElseNatural::HintedIdForQuery>( BSON( "_id" << 1 << "a" << 1 ) );
            add<GetCursor::IdElseNatural::HintedNaturalForQuery>( BSONObj() );
            add<GetCursor::IdElseNatural::HintedNaturalForQuery>( BSON( "_id" << 1 ) );
            add<GetCursor::IdElseNatural::HintedNaturalForQuery>( BSON( "a" << 1 ) );
            add<GetCursor::IdElseNatural::HintedNaturalForQuery>( BSON( "_id" << 1 << "a" << 1 ) );
            add<Explain::ClearRecordedIndex>();
            add<Explain::Initial>();
            add<Explain::Empty>();
            add<Explain::SimpleCount>();
            add<Explain::IterateOnly>();
            add<Explain::ExtraMatchChecks>();
            add<Explain::PartialIteration>();
            add<Explain::Multikey>();
            add<Explain::MultikeyInitial>();
            add<Explain::BecomesMultikey>();
            add<Explain::CountAndYield>();
            add<Explain::MultipleClauses>();
            add<Explain::MultiCursorTakeover>();
            add<Explain::NChunkSkipsTakeover>();
            add<Explain::CoveredIndex>();
            add<Explain::CoveredIndexTakeover>();
            add<Explain::VirtualPickedPlan>();
        }
    } myall;
    
} // namespace QueryOptimizerTests

