/*
	x3dh node server
	author Johan Pascal
	copyright 	Copyright (C) 2017-2025  Belledonne Communications SARL

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// parse arguments to find port to listen and path to DB
// db path is mandatory
// port default to 25519
// path to server certificate default to ./x3dh-cert.pem
// path to server key default to ./x3dh-key.pem
const yargs = require('yargs')
	.option('setting', {
		alias : 's',
		describe: 'path to the config file',
		type: 'string',
		default: 'multiserver.cfg'
	})
	.option('port', {
		alias : 'p',
		describe: 'port to listen',
		type: 'number',
		default: 25519
	})
	.option('certificate', {
		alias : 'c',
		describe: 'path to server certificate',
		type: 'string',
		default : 'x3dh-cert.pem'
	})
	.option('key', {
		alias : 'k',
		describe: 'path to server private key',
		type: 'string',
		default : 'x3dh-key.pem'
	})
	.option('resource_dir', {
		alias : 'r',
		describe : 'set directory path to used as base to find database and key files',
		type : 'string',
		default : './'
	})
	.option('lifetime', {
		alias : 'l',
		describe : 'lifetime of a user in seconds, 0 is forever.',
		type : 'number',
		default : 300
	})
	.argv;

const https = require('https');
const fs = require('fs');
const sqlite3 = require('sqlite3').verbose();
const ReadWriteLock = require('rwlock');
var lock = new ReadWriteLock();

const options = {
  key: fs.readFileSync(yargs.resource_dir+"/"+yargs.key),
  cert: fs.readFileSync(yargs.resource_dir+"/"+yargs.certificate)
};

// retrieve the list of available curve Ids as an enum
const enum_curveId = require('./curveIds.js');

// get the enabled curves from configuration file: curveId->db path
var dbs = require((yargs.resource_dir+"/"+yargs.setting));

// Resource abuse protection
// Setting to 0 disable the function
//lime_max_device_per_user does not apply to this test server as it is used to test the lime lib only and device id are not gruub in the tests
// Do not set this value too low, it shall not be lower than the server_low_limit+batch_size used by client - default is 100+25
const lime_max_opk_per_device = 200;

const X3DH_protocolVersion = 0x01;
const X3DH_headerSize = 3;

const enum_messageTypes = {
	unset_type:0x00,
	deprecated_registerUser:0x01,
	deleteUser:0x02,
	postSPk:0x03,
	postOPks:0x04,
	getPeerBundle:0x05,
	peerBundle:0x06,
	getSelfOPks:0x07,
	selfOPks:0x08,
	registerUser:0x09,
	error:0xff
};

const enum_errorCodes = {
	bad_content_type : 0x00,
	bad_curve : 0x01,
	missing_senderId : 0x02,
	bad_x3dh_protocol_version : 0x03,
	bad_size : 0x04,
	user_already_in : 0x05,
	user_not_found : 0x06,
	db_error : 0x07,
	bad_request : 0x08,
	server_failure : 0x09,
	resource_limit_reached : 0x0a
};

const enum_keyBundleFlag = {
	noOPk : 0x00,
	OPk : 0x01,
	noBundle : 0x02
};

const keySizes = {	// 1 is for enum_curveId.CURVE25519
			// 2 is for enum_curveId.CURVE448
			// 3 is for enum_curveId.CURVE25519K512
			// 4 is for enum_curveId.CURVE25519MLK512
			// 5 is for enum_curveId.CURVE448MLK1024
	1 : {X_pub : 32, ED_pub : 32, Sig : 64, OPk : 32, SPk : 96, Ik : 32},
	2 : {X_pub : 56, ED_pub : 57, Sig : 114, OPk : 56, SPk : 170, Ik : 57},
	3 : {X_pub : 832, ED_pub : 32, Sig : 64, OPk : 832, SPk : 896, Ik : 32},
	4 : {X_pub : 832, ED_pub : 32, Sig : 64, OPk : 832, SPk : 896, Ik : 32},
	5 : {X_pub : 1624, ED_pub : 57, Sig : 114, OPk : 1624, SPk : 1738, Ik : 57}
};


// open DataBases
function openDb(curveId, dbPath) {
console.log(`Open DB : ${dbPath} on curve ${curveId}`);
let db = new sqlite3.Database(yargs.resource_dir+dbPath);
db.run("PRAGMA foreign_keys = ON;"); // enable foreign keys
//db.run("PRAGMA synchronous = OFF;"); // WARNING: huge performance boost be DON'T DO THAT ON A REAL SERVER.
//db.on('trace', function(query) {console.log(query);});
//db.on('profile', function(query,time) {console.log("Profile Sqlite "); console.log(query); console.log(time);});


// Check the user version, do we need to do something on this DB?
db.get("PRAGMA user_version;", function(err, row) {
	if (row.user_version < 1) { // current user version is 1
		console.log("Create Database "+dbPath+" using curve id "+curveId);
		db.run(
		/* Users table:
		 *  - id as primary key (internal use)
		 *  - a userId(shall be the GRUU)
		 *  - Ik (Identity key - EDDSA public key)
		 *  - SPk(the current signed pre-key - ECDH public key)
		 *  - SPk_sig(current SPk public key signed with Identity key)
		 *  - SPh_id (id for SPk provided and internally used by client) - 4 bytes unsigned integer */
		 "CREATE TABLE Users(Uid INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, UserId TEXT NOT NULL, Ik BLOB NOT NULL, SPk BLOB DEFAULT NULL, SPk_sig BLOB, SPk_id UNSIGNED INTEGER);")
		/* One Time PreKey table:
		 *  - an id as primary key (internal use)
		 *  - the Uid of user owning that key
		 *  - the public key (ECDH public key)
		 *  - OPk_id (id for OPk provided and internally used by client) - 4 bytes unsigned integer */
		.run("CREATE TABLE OPk(id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, Uid INTEGER NOT NULL, OPk BLOB NOT NULL, OPk_id UNSIGNED INTEGER NOT NULL, FOREIGN KEY(Uid) REFERENCES Users(Uid) ON UPDATE CASCADE ON DELETE CASCADE);")
		/* a one row configuration table holding: Id Curve in use on this server*/
		.run("CREATE TABLE Config(CurveId INTEGER NOT NULL);", function(err) {
			/* and insert configured curveId */
			db.run("INSERT INTO Config(CurveId) VALUES(?);", curveId);
		})
		// set user_version to 1
		.run("PRAGMA user_version=1;");
	} else {
		// load config table and display it
		db.get("SELECT CurveId FROM Config", function (err, row) {
			if (row.CurveId != curveId) {
				console.log("Open DB "+dbPath+" expect curveId to be "+curveId+" but found "+row.CurveId);
				process.exit(1);
			}
		});
	}
});
return db;
}
// open the databases according to setting: turns dbs from curveId->dbPath to curveId->db Object
for (const [curveId, dbPath] of Object.entries(dbs)) {
	dbs[curveId] = openDb(curveId, dbPath);
}

function deleteUser(curveId, userId) {
	lock.writeLock(function (release) {
		dbs[curveId].run("DELETE FROM Users WHERE UserId = ?;", [userId], function(errDelete){
			release();
			console.log("Timeout for user "+userId+" on curveId "+curveId);
		})
	})
}

// Helper function to compare two byte arrays
function arrayEquals(array1, array2) {
	return (array1.length === array2.length && array1.every(function(value, index) { return value === array2[index]}));
}
// start https server
console.log("X3DH server on, listening port "+yargs.port);
https.createServer(options, (req, res) => {
  function returnError(curveId, code, errorMessage) {
	console.log("return an error message code "+code+" : "+errorMessage);
	let errorBuffer = Buffer.from([X3DH_protocolVersion, enum_messageTypes.error, curveId, code]); // build the X3DH response header, append the error code
	let errorString = Buffer.from(errorMessage); // encode the errorMessage in UTF8 in a buffer
	errorBuffer = Buffer.concat([errorBuffer, errorString]);
	res.writeHead(200, {'Content-type' : 'x3dh/octet-stream'});
	console.log("returned message ("+errorBuffer.length+" bytes):"+errorBuffer.toString('hex'));
	res.end(errorBuffer);
  }

  function returnOk(message) {
	console.log("Ok return a message "+message.toString('hex'));
	res.writeHead(200, {'Content-type' : 'x3dh/octet-stream'});
	res.end(message);
  }

  // TODO: first thing would be to identify and authenticate the request origin against userDB on user server

  // is this someone trying to post something
  if (req.method=='POST') {
	let userId = null;
	let body = Buffer.allocUnsafe(0);
	req.on('data', function(data) {
		body = Buffer.concat([body, data]);
	});
	req.on('end', function() {
		// check http header
		if (req.headers['content-type'] != 'x3dh/octet-stream') {
			returnError(enum_CurveId.UNSET, enum_errorCodes.bad_content_type, "Accept x3dh/octet-stream content type only, not "+req.headers['content-type']);
			return;
		}
		if (!("x-lime-user-identity" in req.headers)) { // user id (GRUU) in HTTP custom header
			if (!("from" in req.headers)) { // legacy : user id in HTTP from
				returnError(enum_CurveId.UNSET, enum_errorCodes.missing_senderId, "X-Lime-user-identity or From field must be present in http packet header");
				return;
			} else {
				userId = req.headers['from'];
			}
		} else {
			userId = req.headers['x-lime-user-identity'];
		}

		// do we have at least a header to parse?
		if (body.length<X3DH_headerSize) {
			returnError(enum_CurveId.UNSET, enum_errorCodes.bad_size, "Packet is not even holding a header. Size "+body.length);
			return;
		}

		// Check X3DH header
		// parse message header
		let protocolVersion = body.readUInt8(0);
		let messageType = body.readUInt8(1);
		let currentCurveId = body.readUInt8(2);

		if (protocolVersion != X3DH_protocolVersion) {
			returnError(currentCurveId, enum_errorCodes.bad_x3dh_protocol_version, "Server running X3DH procotol version "+X3DH_protocolVersion+". Can't process packet with version "+protocolVersion);
			return;
		}

		// is this one supported in the config?
		if (!dbs.hasOwnProperty(currentCurveId)) {
			returnError(currentCurveId, enum_errorCodes.bad_curve, "Server running X3DH procotol can't serve client using curveId "+currentCurveId);
			return;
		}

		// connect to DB
		//let db = new sqlite3.Database(yargs.resource_dir+yargs.database);
		//db.run("PRAGMA foreign_keys = ON;"); // enable foreign keys

		let returnHeader = Buffer.from([protocolVersion, messageType, currentCurveId]); // acknowledge message by sending an empty message with same header (modified in case of getPeerBundle request)
		switch (messageType) {
			/* Deprecated Register User Identity Key : Identity Key <EDDSA Public key size >*/
				/*
			case enum_messageTypes.deprecated_registerUser:
				console.log("Got a deprecated registerUser Message from "+userId);
				var x3dh_expectedSize = keySizes[curveId]['ED_pub'];
				if (body.length<X3DH_headerSize + x3dh_expectedSize) {
					returnError(enum_errorCodes.bad_size, "Register Identity packet is expexted to be "+(X3DH_headerSize+x3dh_expectedSize)+" bytes, but we got "+body.length+" bytes");
					return;
				}
				var Ik = body.slice(X3DH_headerSize, X3DH_headerSize + x3dh_expectedSize);
				lock.writeLock(function (release) {
					// check it is not already present in DB
					db.get("SELECT Uid FROM Users WHERE UserId = ?;", userId , function (err, row) {
							if (row != undefined) { // usedId is already present in base
								release();
								returnError(enum_errorCodes.user_already_in, "Can't insert user "+userId+" - is already present in base");
							} else {
								db.run("INSERT INTO Users(UserId,Ik) VALUES(?,?);", [userId, Ik], function(errInsert){
									release();
									if (errInsert == null) {
										if (yargs.lifetime!=0) {
											console.log("User inserted will be deleted in "+yargs.lifetime*1000);
											setTimeout(deleteUser, yargs.lifetime*1000, userId);
										}
										returnOk(returnHeader);
									} else {
										console.log("INSERT failed err is "); console.log(errInsert);
										returnError(enum_errorCodes.db_error, "Error while trying to insert user "+userId);
									}
								});
							}
						});
				});
			break;
*/
			/* Register User Identity Key :
			 * Identity Key <EDDSA Public key size > |
			 * Signed Pre Key <ECDH public key size> | SPk Signature <Signature length> | SPk id <uint32_t big endian: 4 bytes>
			 * OPk number < uint16_t big endian: 2 bytes> | (OPk <ECDH public key size> | OPk id <uint32_t big endian: 4 bytes> ){OPk number} */
			case enum_messageTypes.registerUser: {
				console.log("Got a registerUser Message from "+userId);
				// check we have at least Ik, SPk, SPk_sig, SPk_Id and OPk_count
				let x3dh_expectedSize = keySizes[currentCurveId]['Ik']  // Ik
					+ keySizes[currentCurveId]['SPk'] + 4 //SPk, SPk_Sig, SPk_id
					+2; // OPk count

				if (body.length<X3DH_headerSize + x3dh_expectedSize) {
					returnError(currentCurveId, enum_errorCodes.bad_size, "Register User packet is expected to be at least(without OPk) "+(X3DH_headerSize+x3dh_expectedSize)+" bytes, but we got "+body.length+" bytes");
					return;
				}
				// Now get the OPk count
				let bufferIndex = X3DH_headerSize;
				let OPk_number = body.readUInt16BE(bufferIndex+x3dh_expectedSize-2);
				if ((lime_max_opk_per_device > 0) && (OPk_number > lime_max_opk_per_device)) { // too much OPk, reject registration
					returnError(currentCurveId, enum_errorCodes.resource_limit_reached,  userId+" is trying to register itself with "+OPk_number+" OPks but server has a limit of "+lime_max_opk_per_device);
					return;
				}
				// And check again the size with OPks
				x3dh_expectedSize += OPk_number*(keySizes[currentCurveId]['OPk'] + 4);
				if (body.length<X3DH_headerSize + x3dh_expectedSize) {
					returnError(currentCurveId, enum_errorCodes.bad_size, "Register User packet is expected to be (with "+OPk_number+" OPks) "+(X3DH_headerSize+x3dh_expectedSize)+" bytes, but we got "+body.length+" bytes");
					return;
				}

				// Read Ik
				let Ik = body.slice(bufferIndex, bufferIndex + keySizes[currentCurveId]['Ik']);
				bufferIndex += keySizes[currentCurveId]['Ik'];
				// SPk
				let SPk = body.slice(bufferIndex, bufferIndex + keySizes[currentCurveId]['X_pub']);
				bufferIndex += keySizes[currentCurveId]['X_pub'];
				// SPk Sig
				let Sig = body.slice(bufferIndex, bufferIndex + keySizes[currentCurveId]['Sig']);
				bufferIndex += keySizes[currentCurveId]['Sig'];
				// SPk Id
				let SPk_id = body.readUInt32BE(bufferIndex); // SPk id is a 32 bits unsigned integer in Big endian
				bufferIndex += 6; // 4 from SPk_id and 2 from OPk count.
				// all OPks
				let OPks_param = [];
				for (let i = 0; i < OPk_number; i++) {
					let OPk = body.slice(bufferIndex, bufferIndex + keySizes[currentCurveId]['OPk']);
					bufferIndex += keySizes[currentCurveId]['OPk'];
					let OPk_id = body.readUInt32BE(bufferIndex); // SPk id is a 32 bits unsigned integer in Big endian
					bufferIndex += 4;
					OPks_param.push([OPk, OPk_id]);
				}



				lock.writeLock(function (release) {
					// check it is not already present in DB
					dbs[currentCurveId].get("SELECT Uid,Ik,SPk,SPk_sig,SPk_id FROM Users WHERE UserId = ?;", userId , function (err, row) {
							if (row != undefined) { // usedId is already present in base
								// Check that Ik, SPk, SPk_sig match the content of the message
								if (arrayEquals(row['Ik'], Ik) && arrayEquals(row['SPk'], SPk) && arrayEquals(row['SPk_sig'], Sig) && SPk_id === row['SPk_id']) {
									console.log("User "+userId+" was already in db, reinsertion with same Ik and SPk required, do nothing and return Ok");
									returnOk(returnHeader);
									release();
								} else {
									release();
									returnError(currentCurveId, enum_errorCodes.user_already_in, "Can't insert user "+userId+" - is already present in base and we try to insert a new one with differents Keys");
								}
							} else {
								dbs[currentCurveId].run("begin transaction");
								dbs[currentCurveId].run("INSERT INTO Users(UserId,Ik,SPk,SPk_sig,SPk_id) VALUES(?,?,?,?,?);", [userId, Ik, SPk, Sig, SPk_id], function(errInsert){
									if (errInsert == null) {
										// Now bulk insert the OPks
										let Uid = this.lastID;
										let stmt_ran = 0;
										let stmt_success = 0;
										let stmt_err_message = '';
										let stmt = dbs[currentCurveId].prepare("INSERT INTO OPk(Uid, OPk, OPk_id) VALUES(?,?,?);")
										for (let i=0; i<OPks_param.length; i++) {
											param = OPks_param[i];
											stmt.run([Uid, param[0], param[1]], function(stmt_err, stmt_res){ // callback is called for each insertion, so we shall count errors and success
												stmt_ran++;
												if (stmt_err) {
													stmt_err_message +=' ## '+stmt_err;
												} else {
													stmt_success++;
												}
												if (stmt_ran == OPk_number) { // and react only when we reach correct count
													if (stmt_ran != stmt_success) {
														dbs[currentCurveId].run("rollback")
														release();
														returnError(currentCurveId, enum_errorCodes.db_error, "Error while trying to insert OPk for user "+userId+". Backend says :"+stmt_err);
													} else {
														dbs[currentCurveId].run("commit")
														release();
														if (yargs.lifetime!=0) {
															console.log("User inserted will be deleted in "+yargs.lifetime*1000);
															setTimeout(deleteUser, yargs.lifetime*1000, currentCurveId, userId);
														}
														returnOk(returnHeader);
													}
												}
											});
										}
									} else {
										release();
										console.log("INSERT failed err is "); console.log(errInsert);
										returnError(currentCurveId, enum_errorCodes.db_error, "Error while trying to insert user "+userId);
									}
								});
							}
						});
				});
			}break;


			/* Delete user: message is empty(or at least shall be, anyway, just ignore anything present in the messange and just delete the user given in From header */
			case enum_messageTypes.deleteUser:{
				console.log("Got a deleteUser Message from "+userId);
				lock.writeLock(function (release) {
					dbs[currentCurveId].run("DELETE FROM Users WHERE UserId = ?;", [userId], function(errDelete){
						release();
						if (errDelete == null) {
							returnOk(returnHeader);
						} else {
							console.log("DELETE failed err is "); console.log(errInsert);
							returnError(currentCurveId, enum_errorCodes.db_error, "Error while trying to delete user "+userId);
						}
					});
				});

			}break;

			/* Post Signed Pre Key: Signed Pre Key <ECDH public key size> | SPk Signature <Signature length> | SPk id <uint32_t big endian: 4 bytes> */
			case enum_messageTypes.postSPk:{
				let x3dh_expectedSize = keySizes[currentCurveId]['SPk'] + 4;
				console.log("Got a postSPk Message from "+userId);
				if (body.length<X3DH_headerSize + x3dh_expectedSize) {
					returnError(currentCurveId, enum_errorCodes.bad_size, "post SPK packet is expexted to be "+(X3DH_headerSize+x3dh_expectedSize)+" bytes, but we got "+body.length+" bytes");
					return;
				}

				// parse message
				let bufferIndex = X3DH_headerSize;
				let SPk = body.slice(bufferIndex, bufferIndex + keySizes[currentCurveId]['X_pub']);
				bufferIndex += keySizes[currentCurveId]['X_pub'];
				let Sig = body.slice(bufferIndex, bufferIndex + keySizes[currentCurveId]['Sig']);
				bufferIndex += keySizes[currentCurveId]['Sig'];
				let SPk_id = body.readUInt32BE(bufferIndex); // SPk id is a 32 bits unsigned integer in Big endian

				// check we have a matching user in DB
				lock.writeLock(function (release) {
					dbs[currentCurveId].get("SELECT Uid FROM Users WHERE UserId = ?;", userId , function (err, row) {
							if (row == undefined) { // user not found in DB
								release();
								returnError(currentCurveId, enum_errorCodes.user_not_found, "Post SPk but "+userId+" not found in db");
							} else {
								let Uid = row['Uid'];

								dbs[currentCurveId].run("UPDATE Users SET SPk = ?, SPk_sig = ?, SPk_id = ? WHERE Uid = ?;", [SPk, Sig, SPk_id, Uid], function(errInsert){
									release();
									if (errInsert == null) {
										returnOk(returnHeader);
									} else {
										console.log("INSERT failed err is "); console.log(errInsert);
										returnError(currentCurveId, enum_errorCodes.db_error, "Error while trying to insert SPK for user "+userId+". Backend says :"+errInsert);
									}
								});
							}
						});
				});

			}break;

			/* Post OPks : OPk number < uint16_t big endian: 2 bytes> | (OPk <ECDH public key size> | OPk id <uint32_t big endian: 4 bytes> ){OPk number} */
			case enum_messageTypes.postOPks:{
				console.log("Got a postOPks Message from "+userId);
				// get the OPks number in the first message bytes(unsigned int 16 in big endian)
				let bufferIndex = X3DH_headerSize;
				let OPk_number = body.readUInt16BE(bufferIndex);
				let x3dh_expectedSize = 2 + OPk_number*(keySizes[currentCurveId]['OPk'] + 4); // read the public key
				if (body.length<X3DH_headerSize + x3dh_expectedSize) {
					returnError(currentCurveId, enum_errorCodes.bad_size, "post OPK packet is expexted to be "+(X3DH_headerSize+x3dh_expectedSize)+" bytes, but we got "+body.length+" bytes");
					return;
				}
				bufferIndex+=2; // point to the beginning of first key

				console.log("It contains "+OPk_number+" keys");

				if ((lime_max_opk_per_device > 0) && (OPk_number > lime_max_opk_per_device)) { // too much OPk, reject registration
					returnError(currentCurveId, enum_errorCodes.resource_limit_reached,  userId+" is trying to insert "+OPk_number+" OPks but server has a limit of "+lime_max_opk_per_device);
					return;
				}

				// check we have a matching user in DB
				lock.writeLock(function (release) {
					dbs[currentCurveId].get("SELECT Uid FROM Users WHERE UserId = ?;", userId , function (err, row) {
							if (row == undefined) { // user not found in DB
								release();
								returnError(currentCurveId, enum_errorCodes.user_not_found, "Post OPks but "+userId+" not found in db");
							} else {
								let Uid = row['Uid'];

								// Check we won't get over the maximum OPks allowed
								if (lime_max_opk_per_device > 0) {
									dbs[currentCurveId].get("SELECT COUNT(OPK_id) as OPk_count FROM Users as u INNER JOIN OPk as o ON u.Uid=o.Uid WHERE UserId = ?;", userId, function (err, row) {
										if (err) {
											release();
											returnError(currentCurveId, enum_errorCodes.db_error, " Database error in postOPks by "+userId+" : "+err2);
											return;
										}

										let OPk_count = 0;
										if (row != undefined) {
											OPk_count = row['OPk_count'];
										}

										if (OPk_count+OPk_number > lime_max_opk_per_device) { // too much OPks
											release();
											returnError(currentCurveId, enum_errorCodes.resource_limit_reached,  userId+" is trying to insert "+OPk_number+" OPks but server has a limit of "+lime_max_opk_per_device+" and it already holds "+OPk_count);
											return;
										}

										// parse all OPks to be inserted
										let OPks_param = [];
										for (let i = 0; i < OPk_number; i++) {
											let OPk = body.slice(bufferIndex, bufferIndex + keySizes[currentCurveId]['OPk']);
											bufferIndex += keySizes[currentCurveId]['OPk'];
											let OPk_id = body.readUInt32BE(bufferIndex); // OPk id is a 32 bits unsigned integer in Big endian
											bufferIndex += 4;
											OPks_param.push([Uid, OPk, OPk_id]);
										}

										// bulk insert of OPks
										let stmt_ran = 0;
										let stmt_success = 0;
										let stmt_err_message = '';
										dbs[currentCurveId].run("begin transaction");
										let stmt = dbs[currentCurveId].prepare("INSERT INTO OPk(Uid, OPk, OPk_id) VALUES(?,?,?);")
										for (let i=0; i<OPks_param.length; i++) {
											param = OPks_param[i];
											stmt.run(param, function(stmt_err, stmt_res){ // callback is called for each insertion, so we shall count errors and success
												stmt_ran++;
												if (stmt_err) {
													stmt_err_message +=' ## '+stmt_err;
												} else {
													stmt_success++;
												}
												if (stmt_ran == OPk_number) { // and react only when we reach correct count
													if (stmt_ran != stmt_success) {
														dbs[currentCurveId].run("rollback")
														release();
														returnError(currentCurveId, enum_errorCodes.db_error, "Error while trying to insert OPk for user "+userId+". Backend says :"+stmt_err);
													} else {
														dbs[currentCurveId].run("commit")
														release();
														returnOk(returnHeader);
													}
												}
											});
										}
										return;
									});
								} else { // Note : this code shall be factorized

									// parse all OPks to be inserted
									let OPks_param = [];
									for (let i = 0; i < OPk_number; i++) {
										let OPk = body.slice(bufferIndex, bufferIndex + keySizes[currentCurveId]['OPk']);
										bufferIndex += keySizes[currentCurveId]['OPk'];
										let OPk_id = body.readUInt32BE(bufferIndex); // OPk id is a 32 bits unsigned integer in Big endian
										bufferIndex += 4;
										OPks_param.push([Uid, OPk, OPk_id]);
									}

									// bulk insert of OPks
									let stmt_ran = 0;
									let stmt_success = 0;
									let stmt_err_message = '';
									dbs[currentCurveId].run("begin transaction");
									let stmt = dbs[currentCurveId].prepare("INSERT INTO OPk(Uid, OPk, OPk_id) VALUES(?,?,?);")
									for (let i=0; i<OPks_param.length; i++) {
										param = OPks_param[i];
										stmt.run(param, function(stmt_err, stmt_res){ // callback is called for each insertion, so we shall count errors and success
											stmt_ran++;
											if (stmt_err) {
												stmt_err_message +=' ## '+stmt_err;
											} else {
												stmt_success++;
											}
											if (stmt_ran == OPk_number) { // and react only when we reach correct count
												if (stmt_ran != stmt_success) {
													dbs[currentCurveId].run("rollback")
													release();
													returnError(currentCurveId, enum_errorCodes.db_error, "Error while trying to insert OPk for user "+userId+". Backend says :"+stmt_err);
												} else {
													dbs[currentCurveId].run("commit")
													release();
													returnOk(returnHeader);
												}
											}
										});
									}
								}
							}
						});
				});
			}break;

			/* peerBundle :	bundle Count < 2 bytes unsigned Big Endian> |
			 *	(   deviceId Size < 2 bytes unsigned Big Endian > | deviceId
			 *	    Flag<1 byte: 0 if no OPK in bundle, 1 if present | 2 no key bundle is present> |
			 *	    Ik <EDDSA Public Key Length> |
			 *	    SPk <ECDH Public Key Length> | SPK id <4 bytes>
			 *	    SPk_sig <Signature Length> |
			 *	    (OPk <ECDH Public Key Length> | OPk id <4 bytes>){0,1 in accordance to flag}
			 *	) { bundle Count}
			 */
			case enum_messageTypes.getPeerBundle:{
				function buildPeerBundlePacket(peersBundle) {
					let peersBundleBuffer = Buffer.allocUnsafe(X3DH_headerSize+2);
					peersBundleBuffer.writeUInt8(X3DH_protocolVersion, 0);
					peersBundleBuffer.writeUInt8(enum_messageTypes.peerBundle, 1);
					peersBundleBuffer.writeUInt8(currentCurveId, 2);
					peersBundleBuffer.writeUInt16BE(peersBundle.length, 3); // peers bundle count on 2 bytes in Big Endian

					for (let i=0; i<peersBundle.length; i++) {
						let userId = peersBundle[i][0];
						let haveOPk = (peersBundle[i].length>5)?1:0; // elements in peersBundle[i] are : deviceId, Ik, SPk, SPk_id, SPk_sig, [OPk, OPk_id] last 2 are optionnal
						let peerBundle = Buffer.allocUnsafe(2);
						peerBundle.writeUInt16BE(userId.length, 0); // device Id size on 2 bytes in Big Endian
						let flagBuffer = Buffer.allocUnsafe(1);
						if (peersBundle[i].length==1) { // We do not have any key bundle, user was not found
							flagBuffer.writeUInt8(enum_keyBundleFlag.noBundle,0);
							peerBundle = Buffer.concat([peerBundle, Buffer.from(userId), flagBuffer]); // bundle is just the id and the flag set to 2
						} else { /* we do have a peer bundle, insert it*/
							/* set the flag */
							if (haveOPk) {
								flagBuffer.writeUInt8(enum_keyBundleFlag.OPk,0);
							} else {
								flagBuffer.writeUInt8(enum_keyBundleFlag.noOPk,0);
							}
							let SPk_idBuffer = Buffer.allocUnsafe(4);
							SPk_idBuffer.writeUInt32BE(peersBundle[i][3], 0); // SPk id on 4 bytes in Big Endian
							peerBundle = Buffer.concat([peerBundle, Buffer.from(userId), flagBuffer, peersBundle[i][1], peersBundle[i][2], SPk_idBuffer, peersBundle[i][4]]);
							if (haveOPk) {
								let OPk_idBuffer = Buffer.allocUnsafe(4);
								OPk_idBuffer.writeUInt32BE(peersBundle[i][6], 0); // OPk id on 4 bytes in Big Endian
								peerBundle = Buffer.concat([peerBundle, peersBundle[i][5], OPk_idBuffer]);
							}
						}
						peersBundleBuffer = Buffer.concat([peersBundleBuffer, peerBundle]);
					}
					returnOk(peersBundleBuffer);
				}

				console.log("Got a getPeerBundle Message from "+userId);


				// first parse the message
				let bufferIndex = X3DH_headerSize;
				let peersCount = body.readUInt16BE(bufferIndex); // 2 bytes BE unsigned int : number of devices uri

				if (peersCount == 0) {
					returnError(currentCurveId, enum_errorCodes.bad_request, "Ask for peer Bundles but no device id given");
					release();
				}

				bufferIndex+=2;
				let peersBundle = [];
				for (let i=0; i<peersCount; i++) {
					let IdLength = body.readUInt16BE(bufferIndex); // 2 bytes BE unsigned int : length of the following id string
					bufferIndex+=2;
					peersBundle.push([body.toString('utf8', bufferIndex, bufferIndex+IdLength)]); // create element of peersBundle as an array as we will then push the keys on it
					bufferIndex+=IdLength;
				}

				console.log("Found request for "+peersCount+" keys bundles");
				console.dir(peersBundle);

				lock.writeLock(function (release) {
					console.log("Process a getPeerBundle Message from "+userId);
					// Try to access all requested values
					let queries_ran = 0;
					let queries_success = 0;
					let queries_err_message = '';

					for (let i=0; i<peersCount; i++) {
					// left join as we may not have any OPk but shall cope with it
						dbs[currentCurveId].get("SELECT u.Ik, u.SPk, u.SPk_id, u.SPk_sig, o.OPk, o.OPk_id, o.id FROM Users as u LEFT JOIN OPk as o ON u.Uid=o.Uid WHERE UserId = ? AND u.SPk IS NOT NULL LIMIT 1;", peersBundle[i][0] , function (err, row) {
								queries_ran++;
								if (err) {
									queries_err_message +=' ## get bundle for user '+peersBundle[i]+" error : "+err;
									return;
								}
								if (row == undefined) { // user not found in DB
									console.log("user "+peersBundle[i][0]+" not found");
									queries_success++;
								} else {
									console.log("Ok push on peersBundle array "+i);
									console.dir(peersBundle[i]);
									peersBundle[i].push(row['Ik']);
									peersBundle[i].push(row['SPk']);
									peersBundle[i].push(row['SPk_id']);
									peersBundle[i].push(row['SPk_sig']);
									if (row['OPk']) {
										peersBundle[i].push(row['OPk']);
										peersBundle[i].push(row['OPk_id']);
										console.log("Ok we retrieved OPk "+row['id']+" from table, we must remove it");
										//NOTE: in real situation we have to find a way to insure the value wasn't also read by an other request
										queries_ran--; // this query is not over: step back
										dbs[currentCurveId].run("DELETE FROM OPk WHERE id = ?;", [row['id']], function(errDelete){
											queries_ran++;
											if (errDelete == null) {
												queries_success++;
											} else {
												queries_err_message +=' ## could not delete OPk key retrieved user '+peersBundle[i]+" err : "+errDelete;
											}

											if (queries_ran == peersCount) {
												if (queries_ran != queries_success) {
													returnError(currentCurveId, enum_errorCodes.db_error, "Error while trying to get peers bundles :"+queries_err_message);
												} else {
													// build the peerBundle Message
													buildPeerBundlePacket(peersBundle); // this one build and return the buffer
												}
												release();
											}
										});
									} else {
										queries_success++;
									}
								}

								if (queries_ran == peersCount) {
									if (queries_ran != queries_success) {
										returnError(currentCurveId, enum_errorCodes.db_error, "Error while trying to get peers bundles :"+queries_err_message);
									} else {
										// build the peerBundle Message
										buildPeerBundlePacket(peersBundle); // this one build and return the buffer
									}
									release();
								}
						});
					}
				}); // writeLock


			}break;

			/* selfOPks :	OPKs Id Count < 2 bytes unsigned Big Endian> |
			 *	    (OPk id <4 bytes>){ OPks Id Count}
			 */
			case enum_messageTypes.getSelfOPks:{
				console.log("Process a getSelfOPks Message from "+userId);
				// check we have a matching user in DB
				dbs[currentCurveId].get("SELECT Uid FROM Users WHERE UserId = ?;", userId , function (err, row) {
					if (row == undefined) { // user not found in DB
						returnError(currentCurveId, enum_errorCodes.user_not_found, "Get Self OPks but "+userId+" not found in db");
					} else {

						dbs[currentCurveId].all("SELECT o.OPk_id  as OPk_id FROM Users as u INNER JOIN OPk as o ON u.Uid=o.Uid WHERE UserId = ?;", userId, function (err, rows) {
							if (err) {
								returnError(currentCurveId, enum_errorCodes.db_error, " Database error in getSelfOPks by "+userId+" : "+err);
								return;
							}

							// create the return message
							let selfOPKsBuffer = Buffer.allocUnsafe(X3DH_headerSize+2);
							selfOPKsBuffer.writeUInt8(X3DH_protocolVersion, 0);
							selfOPKsBuffer.writeUInt8(enum_messageTypes.selfOPks, 1);
							selfOPKsBuffer.writeUInt8(currentCurveId, 2);

							if (rows == undefined || rows.length == 0) { // no Id founds
								selfOPKsBuffer.writeUInt16BE(0, 3); // peers bundle count on 2 bytes in Big Endian
							} else {
								selfOPKsBuffer.writeUInt16BE(rows.length, 3); // peers bundle count on 2 bytes in Big Endian

								for (let i=0; i<rows.length; i++) {
									let OPk_idBuffer = Buffer.allocUnsafe(4);
									OPk_idBuffer.writeUInt32BE(rows[i]['OPk_id'], 0); // OPk id on 4 bytes in Big Endian
									selfOPKsBuffer = Buffer.concat([selfOPKsBuffer, OPk_idBuffer]);
								}
							}
							returnOk(selfOPKsBuffer);
						});
					}
				});
			}break;

			default:
				returnError(currentCurveId, enum_errorCodes.bad_message_type, "Unknown message type "+messageType);
			break;
		}
	});
  } else {
	  // do nothing we just want POST
  }
}).listen(yargs.port);
