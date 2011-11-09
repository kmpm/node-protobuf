
var util = require('util');
var assert = require('assert');
var fs = require('fs');

// Load schema
var Schema = require('../protobuf').Schema;
var TestSchema = new Schema(fs.readFileSync('./test/test.desc'));
var TestMessage = TestSchema['node_protobuf.TestMessage'];

/* Generic roundtrip serialization test */
var shouldSerialize = function(test, message){
	// given
	var message = message;

	// when
	var serialzied = TestMessage.serialize(message);

	// then
	test.equal(util.inspect(TestMessage.parse(serialzied)), util.inspect(message));
}

exports.shouldSerializeAndParseMessage = function(test){
	shouldSerialize(test, {test : 123});
	test.done();
}

exports.shouldSkipUnknownAttributes = function(test){
	// given
	var message = {test : 123, unknown : 'hello'};

	// when
	var serialzied = TestMessage.serialize(message);
	var parsed = TestMessage.parse(serialzied);

	// then
	test.ok(!parsed.unknown);
	test.ok(!!parsed.test);
	test.done();
}

exports.shouldSerializeDifferentTypes = function(test){
	shouldSerialize(test, {string : 'Hello'});
	shouldSerialize(test, {int32 : 123});
	shouldSerialize(test, {repeated: [{test : 123}]});

	test.done();
}
