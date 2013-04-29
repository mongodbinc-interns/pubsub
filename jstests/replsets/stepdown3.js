// Test that GLE asserts when the primary steps down while we're waiting for w:

var replTest = new ReplSetTest({ name: 'testSet', nodes: 2 });
var nodes = replTest.startSet();
replTest.initiate();
var master = replTest.getMaster();

// do a write to allow stepping down of the primary;
// otherwise, the primary will refuse to step down
print("\ndo a write");
master.getDB("test").foo.insert({x:1});
replTest.awaitReplication();

// lock secondary, to pause replication
print("\nlock secondary");
var locked = replTest.liveNodes.slaves[0];
printjson( locked.getDB("admin").runCommand({fsync : 1, lock : 1}) );

// do a write
print("\ndo a write");
master.getDB("test").foo.insert({x:2});

// step down the primary asyncronously
print("stepdown");
var command = "sleep(4000); tojson(rs.stepDown());"
var mongoprogram = startMongoProgram( "mongo", "--quiet", "--port", "" + master.port, 
                                      "--eval", command );

print("getlasterror; should assert");
var gleFunction = function() { 
    master.getDB("test").runCommand({getLastError : 1, w: 2 , wtimeout :30000 }) 
};
var result = assert.throws(gleFunction);
print("result of gle:");
printjson(result);

// unlock and shut down
printjson(locked.getDB("admin").$cmd.sys.unlock.findOne());
replTest.stopSet();
