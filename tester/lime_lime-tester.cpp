/*
	lime_lime-tester.cpp
	@author Johan Pascal
	@copyright 	Copyright (C) 2017  Belledonne Communications SARL

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

#include "lime_log.hpp"
#include "lime/lime.hpp"
#include "lime_lime.hpp"
#include "lime-tester.hpp"
#include "lime_keys.hpp"
#include "lime-tester-utils.hpp"

#include <bctoolbox/tester.h>
#include <bctoolbox/exception.hh>
#include <belle-sip/belle-sip.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <memory>
#include <chrono>
#include <thread>
#include <deque>
#include <mutex>
#include <list>

using namespace::std;
using namespace::lime;

static belle_sip_stack_t *bc_stack=NULL;
static belle_http_provider_t *prov=NULL;
/**
 * An enumeration to control simulated http transmission failure
 */
enum class HttpLinkStatus : uint8_t {
	ok,
	sending_fail,
	reception_fail
};

static HttpLinkStatus httpLink = HttpLinkStatus::ok;

static int http_before_all(void) {
	bc_stack=belle_sip_stack_new(NULL);

	prov=belle_sip_stack_create_http_provider(bc_stack,"0.0.0.0");

	belle_tls_crypto_config_t *crypto_config=belle_tls_crypto_config_new();

	belle_tls_crypto_config_set_root_ca(crypto_config,std::string(bc_tester_get_resource_dir_prefix()).append("/data/").data());
	belle_http_provider_set_tls_crypto_config(prov,crypto_config);
	belle_sip_object_unref(crypto_config);
	return 0;
}

static int http_after_all(void) {
	belle_sip_object_unref(prov);
	belle_sip_object_unref(bc_stack);
	return 0;
}

struct C_Callback_userData {
	const limeX3DHServerResponseProcess responseProcess;
	const std::string username; // the username sending message, used for logs
	C_Callback_userData(const limeX3DHServerResponseProcess &response, const std::string &username) : responseProcess(response), username{username} {};
};

static void process_io_error(void *data, const belle_sip_io_error_event_t *event) noexcept{
	C_Callback_userData *userData = static_cast<C_Callback_userData *>(data);
	LIME_LOGI<<"IO Error on X3DH server request from user "<<userData->username;
	(userData->responseProcess)(0, std::vector<uint8_t>{});
	delete(userData);
}

static void process_response(void *data, const belle_http_response_event_t *event) noexcept {
	C_Callback_userData *userData = static_cast<C_Callback_userData *>(data);
	LIME_LOGI<<"Response from X3DH server for user "<<userData->username;
	if (event->response){
		auto code=belle_http_response_get_status_code(event->response);
		belle_sip_message_t *message = BELLE_SIP_MESSAGE(event->response);
		// all raw data access functions in lime use uint8_t *, so safely cast the body pointer to it, it's just a data stream pointer anyway
		auto body = reinterpret_cast<const uint8_t *>(belle_sip_message_get_body(message));
		auto bodySize = belle_sip_message_get_body_size(message);
		(userData->responseProcess)(code, std::vector<uint8_t>{body, body+bodySize});
	} else {
		(userData->responseProcess)(0, std::vector<uint8_t>{});
	}
	delete(userData);
}

/** @brief Post data to X3DH server.
 * Communication with X3DH server is entirely managed out of the lib lime, in this example code it is performed over HTTPS provided by belle-sip
 * Here the HTTPS stack provider prov is a static variable in global context so there is no need to capture it, it may be the case in real usage
 * This lambda prototype is defined in lime.hpp
 *
 * @param[in] url		The URL of X3DH server
 * @param[in] from		The local device id, used to identify user on the X3DH server, user identification and credential verification is out of lib lime scope.
 * 				Here identification is performed on test server via belle-sip authentication mechanism and providing the test user credentials
 * @param[in] message		The data to be sent to the X3DH server
 * @param[in] responseProcess	The function to be called when response from server arrives. Function prototype is defined in lime.hpp: (void)(int responseCode, std::vector<uint8_t>response)
 */
static limeX3DHServerPostData X3DHServerPost([](const std::string &url, const std::string &from, std::vector<uint8_t> &&message, const limeX3DHServerResponseProcess &responseProcess){
	belle_http_request_listener_callbacks_t cbs;
	belle_http_request_listener_t *l;
	belle_generic_uri_t *uri;
	belle_http_request_t *req;
	belle_sip_memory_body_handler_t *bh;

	memset(&cbs,0,sizeof(belle_http_request_listener_callbacks_t));

	bh = belle_sip_memory_body_handler_new_copy_from_buffer(message.data(), message.size(), NULL, NULL);

	uri=belle_generic_uri_parse(url.data());

	req=belle_http_request_create("POST",
			uri,
			belle_http_header_create("User-Agent", "lime"),
			belle_http_header_create("Content-type", "x3dh/octet-stream"),
			belle_http_header_create("X-Lime-user-identity", from.data()),
			NULL);

	belle_sip_message_set_body_handler(BELLE_SIP_MESSAGE(req),BELLE_SIP_BODY_HANDLER(bh));
	cbs.process_response=process_response;
	cbs.process_io_error=process_io_error;
	// store a reference to the responseProcess function in a wrapper as belle-sip request C-style callbacks with a void * user data parameter, C++ implementation shall
	// use lambda and capture the function.
	C_Callback_userData *userData = new C_Callback_userData(responseProcess, from); // create on the heap a copy of the responseProcess closure so it's available when we're called back by belle-sip
	l=belle_http_request_listener_create_from_callbacks(&cbs, userData);
	belle_sip_object_data_set(BELLE_SIP_OBJECT(req), "http_request_listener", l, belle_sip_object_unref); // Ensure the listener object is destroyed when the request is destroyed
	LIME_LOGI<<"user "<<from<<"post a request to X3DH server";
	belle_http_provider_send_request(prov,req,l);
});

/** @brief Post data to X3DH server: use the previous function but is able to simulate emission or reception failure.
 * uses the httpLink global variable to simulate transmission error:
 *
 *
 * @param[in] url		The URL of X3DH server
 * @param[in] from		The local device id, used to identify user on the X3DH server, user identification and credential verification is out of lib lime scope.
 * 				Here identification is performed on test server via belle-sip authentication mechanism and providing the test user credentials
 * @param[in] message		The data to be sent to the X3DH server
 * @param[in] responseProcess	The function to be called when response from server arrives. Function prototype is defined in lime.hpp: (void)(int responseCode, std::vector<uint8_t>response)
 */
static limeX3DHServerPostData X3DHServerPost_Failing_Simulation([](const std::string &url, const std::string &from, std::vector<uint8_t> &&message, const limeX3DHServerResponseProcess &responseProcess){
	switch (httpLink) {
		case HttpLinkStatus::reception_fail :
			X3DHServerPost(url, from, std::move(message), [](int response_code, const std::vector<uint8_t> &responseBody){
					// This is a dummy responseProcessing, just swallow the server answer and do nothing
					});
		break;
		case HttpLinkStatus::sending_fail :
			// Just do nothing, swallow the packet and do not give any answer.
		break;
		case HttpLinkStatus::ok :
		default:
			X3DHServerPost(url, from, std::move(message), responseProcess);
		break;
	}
});

/* This function will destroy and recreate managers given in parameter, force deleting all internal cache and start back from what is in local Storage */
static void managersClean(std::unique_ptr<LimeManager> &alice, std::unique_ptr<LimeManager> &bob, std::string aliceDb, std::string bobDb) {
	alice = nullptr;
	bob = nullptr;
	alice = make_unique<LimeManager>(aliceDb, X3DHServerPost);
	bob = make_unique<LimeManager>(bobDb, X3DHServerPost);
	LIME_LOGI<<"Trash and reload alice and bob LimeManagers";
}

/**
 * Helper, alice and bob exchange messages:
 * - bob sends batch_size message to alice, alice decrypts
 * - alice responds with batch_size messages to bob, bob decrypts
 * - repeat batch_number times
 *
 */
static void lime_exchange_messages(std::shared_ptr<std::string> &aliceDeviceId, std::unique_ptr<LimeManager> &aliceManager,
				   std::shared_ptr<std::string> &bobDeviceId, std::unique_ptr<LimeManager> &bobManager,
				   const std::vector<lime::CurveId> &algos,
				   int batch_number, int batch_size) {


	size_t messageCount = 0;
	lime_tester::events_counters_t counters={};
	int expected_success=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};

	try {
		for (auto batches=0; batches<batch_number; batches++) {
			for (auto i=0; i<batch_size; i++) {
				auto patternIndex = messageCount % lime_tester::messages_pattern.size();
				// bob encrypt a message to Alice
				auto encryptionContext = make_shared<EncryptionContext>("alice", lime_tester::messages_pattern[patternIndex]);
				encryptionContext->addRecipient(*aliceDeviceId);

				bobManager->encrypt(*bobDeviceId, algos, encryptionContext, callback);
				BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

				// alice decrypt
				std::vector<uint8_t> receivedMessage{};
				// in that context we cannot know the expected decrypt return value, just check it is not fail
				BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDeviceId, "alice", *bobDeviceId, encryptionContext->m_recipients[0].DRmessage, encryptionContext->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
				BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[patternIndex]);
				messageCount++;
			}

			for (auto i=0; i<batch_size; i++) {
				auto patternIndex = messageCount % lime_tester::messages_pattern.size();
				// alice respond to bob
				auto encryptionContext = make_shared<EncryptionContext>("bob", lime_tester::messages_pattern[patternIndex]);
				encryptionContext->addRecipient(*bobDeviceId);

				aliceManager->encrypt(*aliceDeviceId, algos, encryptionContext, callback);
				BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

				// bob decrypt
				std::vector<uint8_t> receivedMessage{};
				// in that context we cannot know the expected decrypt return value, just check it is not fail
				BC_ASSERT_TRUE(bobManager->decrypt(*bobDeviceId, "bob", *aliceDeviceId, encryptionContext->m_recipients[0].DRmessage, encryptionContext->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
				BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[patternIndex]);
				messageCount++;
			}
		}
	} catch (BctbxException &) {
		BC_FAIL("Message Exchange failed");
		throw;
	}

}


/**
 * Helper, create DB, alice and bob devices, and exchange a message
 */
static void lime_session_establishment(const std::vector<lime::CurveId> &algos, const std::string &dbBaseFilename,
					std::string &dbFilenameAlice, std::shared_ptr<std::string> &aliceDeviceId, std::unique_ptr<LimeManager> &aliceManager,
					std::string &dbFilenameBob, std::shared_ptr<std::string> &bobDeviceId, std::unique_ptr<LimeManager> &bobManager) {
	// create DB
	dbFilenameAlice = dbBaseFilename;
	dbFilenameAlice.append(".alice.").append(CurveId2String(algos, "-")).append(".sqlite3");
	dbFilenameBob = dbBaseFilename;
	dbFilenameBob.append(".bob.").append(CurveId2String(algos, "-")).append(".sqlite3");

	remove(dbFilenameAlice.data()); // delete the database file if already exists
	remove(dbFilenameBob.data()); // delete the database file if already exists

	lime_tester::events_counters_t counters={};
	int expected_success=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};
	try {
		// create Manager and device for alice
		aliceManager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);
		aliceDeviceId = lime_tester::makeRandomDeviceName("alice.d1.");
		aliceManager->create_user(*aliceDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));

		// Create manager and device for bob
		bobManager = make_unique<LimeManager>(dbFilenameBob, X3DHServerPost);
		bobDeviceId = lime_tester::makeRandomDeviceName("bob.d");
		bobManager->create_user(*bobDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));

		lime_exchange_messages(aliceDeviceId, aliceManager, bobDeviceId, bobManager, algos, 1, 1);

	} catch (BctbxException &) {
		BC_FAIL("Session establishment failed");
		throw;
	}
}
/**
 * Scenario: create DB, alice and bob devices and then Bob encrypt a message to Alice but we mess with the cipherMessage to
 * 	get cipherMessage and cipherHeader policy not matching anymore
 */
static void lime_encryptionPolicyError_test(const lime::CurveId curve, const std::string &dbBaseFilename,
		const std::vector<uint8_t> &plainMessage, const lime::EncryptionPolicy setEncryptionPolicy) {
	// create DB
	std::string dbFilenameAlice = dbBaseFilename;
	dbFilenameAlice.append(".alice.").append(CurveId2String(curve)).append(".sqlite3");
	std::string dbFilenameBob = dbBaseFilename;
	dbFilenameBob.append(".bob.").append(CurveId2String(curve)).append(".sqlite3");

	remove(dbFilenameAlice.data()); // delete the database file if already exists
	remove(dbFilenameBob.data()); // delete the database file if already exists

	lime_tester::events_counters_t counters={};
	int expected_success=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};
	try {
		std::vector<lime::CurveId> algos{curve};
		// create Manager and recipient(s) device for alice
		std::unique_ptr<LimeManager> aliceManager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);
		std::shared_ptr<std::string> aliceDeviceId = lime_tester::makeRandomDeviceName("alice.d");
		aliceManager->create_user(*aliceDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));

		// Create manager and device for bob
		std::unique_ptr<LimeManager> bobManager = make_unique<LimeManager>(dbFilenameBob, X3DHServerPost);
		std::shared_ptr<std::string> bobDeviceId = lime_tester::makeRandomDeviceName("bob.d");
		bobManager->create_user(*bobDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));


		// bob encrypt a message to Alice device
		auto encryptionContext = make_shared<EncryptionContext>("alice", plainMessage, setEncryptionPolicy);
		encryptionContext->addRecipient(*aliceDeviceId);
		bobManager->encrypt(*bobDeviceId, algos, encryptionContext, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		bool is_directEncryptionType = lime_tester::DR_message_payloadDirectEncrypt(encryptionContext->m_recipients[0].DRmessage);
		if (setEncryptionPolicy == lime::EncryptionPolicy::DRMessage) {
			BC_ASSERT_TRUE(is_directEncryptionType);
			BC_ASSERT_EQUAL((int)(encryptionContext->m_cipherMessage.size()), 0, int, "%d"); // in direct Encryption mode, cipherMessage is empty

			encryptionContext->m_cipherMessage.resize(32, 0xaa); // just create a 0xaa filled buffer, its presence shall prevent the perferctly correct DR message to be decrypted
		} else {
			BC_ASSERT_FALSE(is_directEncryptionType);
			BC_ASSERT_NOT_EQUAL((int)(encryptionContext->m_cipherMessage.size()), 0, int, "%d"); // in direct cipher message mode, cipherMessage is not empty

			encryptionContext->m_cipherMessage.clear(); // delete the cipher message, the DR decryption will fail and will not return the random seed as plaintext
		}

		// alice tries to decrypt, but it shall fail
		std::vector<uint8_t> receivedMessage{};
		BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDeviceId, "alice", *bobDeviceId, encryptionContext->m_recipients[0].DRmessage, encryptionContext->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::fail);

		if (cleanDatabase) {
			aliceManager->delete_user(DeviceId(*aliceDeviceId, curve), callback);
			bobManager->delete_user(DeviceId(*bobDeviceId, curve), callback);
			expected_success += 2;
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success,lime_tester::wait_for_timeout));
			remove(dbFilenameAlice.data());
			remove(dbFilenameBob.data());
		}

	} catch (BctbxException &) {
		BC_FAIL("Session establishment failed");
		throw;
	}
}
static void lime_encryptionPolicyError() {
#ifdef EC25519_ENABLED
	lime_encryptionPolicyError_test(lime::CurveId::c25519, "lime_encryptionPolicyError_DR",
			lime_tester::shortMessage, lime::EncryptionPolicy::DRMessage);
	lime_encryptionPolicyError_test(lime::CurveId::c25519, "lime_encryptionPolicyError_cipher",
			lime_tester::shortMessage, lime::EncryptionPolicy::cipherMessage);
#endif
#ifdef EC448_ENABLED
	lime_encryptionPolicyError_test(lime::CurveId::c448, "lime_encryptionPolicyError_DR",
			lime_tester::shortMessage, lime::EncryptionPolicy::DRMessage);
	lime_encryptionPolicyError_test(lime::CurveId::c448, "lime_encryptionPolicyError_cipher",
			lime_tester::shortMessage, lime::EncryptionPolicy::cipherMessage);
#endif
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	lime_encryptionPolicyError_test(lime::CurveId::c25519k512, "lime_encryptionPolicyError_DR",
			lime_tester::shortMessage, lime::EncryptionPolicy::DRMessage);
	lime_encryptionPolicyError_test(lime::CurveId::c25519k512, "lime_encryptionPolicyError_cipher",
			lime_tester::shortMessage, lime::EncryptionPolicy::cipherMessage);

	lime_encryptionPolicyError_test(lime::CurveId::c25519mlk512, "lime_encryptionPolicyError_DR",
			lime_tester::shortMessage, lime::EncryptionPolicy::DRMessage);
	lime_encryptionPolicyError_test(lime::CurveId::c25519mlk512, "lime_encryptionPolicyError_cipher",
			lime_tester::shortMessage, lime::EncryptionPolicy::cipherMessage);
#endif
#ifdef EC448_ENABLED
	lime_encryptionPolicyError_test(lime::CurveId::c448mlk1024, "lime_encryptionPolicyError_DR",
			lime_tester::shortMessage, lime::EncryptionPolicy::DRMessage);
	lime_encryptionPolicyError_test(lime::CurveId::c448mlk1024, "lime_encryptionPolicyError_cipher",
			lime_tester::shortMessage, lime::EncryptionPolicy::cipherMessage);
#endif
#endif
}

/**
 * Scenario: Bob encrypt a message to Alice device 1 and 2 using given encryptionPolicy
 *
 * parameters allow to control:
 *  - plaintext message
 *  - number of recipients (1 or 2)
 *  - forced encryption policy(with a bool switch)
 *  - expected message type (must be DRMessage or cipherMessage)
 */
static void lime_encryptionPolicy_test(const std::vector<lime::CurveId> &algos, std::shared_ptr<LimeManager> aliceManager, std::shared_ptr<std::string> aliceDevice1Id, std::shared_ptr<std::string> aliceDevice2Id,
		std::shared_ptr<LimeManager> bobManager, std::shared_ptr<std::string> bobDeviceId,
		const std::vector<uint8_t> &plainMessage, const bool multipleRecipients,
		const lime::EncryptionPolicy setEncryptionPolicy, bool forceEncryptionPolicy,
		const lime::EncryptionPolicy getEncryptionPolicy) {

	lime_tester::events_counters_t counters={};
	int expected_success=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};
	try {
		// bob encrypt a message to Alice devices 1 and 2
		std::shared_ptr<lime::EncryptionContext> encryptionContext;
		if (forceEncryptionPolicy) {
			encryptionContext = make_shared<EncryptionContext>("alice", plainMessage, setEncryptionPolicy);
		} else {
			encryptionContext = make_shared<EncryptionContext>("alice", plainMessage);
		}
		encryptionContext->addRecipient(*aliceDevice1Id);
		if (multipleRecipients) {
			encryptionContext->addRecipient(*aliceDevice2Id);
		}
		bobManager->encrypt(*bobDeviceId, algos, encryptionContext, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		bool is_directEncryptionType = lime_tester::DR_message_payloadDirectEncrypt(encryptionContext->m_recipients[0].DRmessage);
		if (multipleRecipients) {
			// all cipher header must have the same message type
			BC_ASSERT_TRUE(is_directEncryptionType == lime_tester::DR_message_payloadDirectEncrypt(encryptionContext->m_recipients[1].DRmessage));
		}
		if (getEncryptionPolicy == lime::EncryptionPolicy::DRMessage) {
			BC_ASSERT_TRUE(is_directEncryptionType);
			BC_ASSERT_EQUAL((int)(encryptionContext->m_cipherMessage.size()), 0, int, "%d"); // in direct Encryption mode, cipherMessage is empty
		} else {
			BC_ASSERT_FALSE(is_directEncryptionType);
			BC_ASSERT_NOT_EQUAL((int)(encryptionContext->m_cipherMessage.size()), 0, int, "%d"); // in direct cipher message mode, cipherMessage is not empty
		}

		// alice1 decrypt
		std::vector<uint8_t> receivedMessage{};
		if (is_directEncryptionType) { // when having the message in DR message only, use the decrypt interface without cipherMessage
			BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDevice1Id, "alice", *bobDeviceId, encryptionContext->m_recipients[0].DRmessage, receivedMessage) != lime::PeerDeviceStatus::fail);
		} else {
			BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDevice1Id, "alice", *bobDeviceId, encryptionContext->m_recipients[0].DRmessage, encryptionContext->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
		}
		BC_ASSERT_TRUE(receivedMessage == plainMessage);

		if (multipleRecipients) {
			// alice2 decrypt
			receivedMessage.clear();
			BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDevice2Id, "alice", *bobDeviceId, encryptionContext->m_recipients[1].DRmessage, encryptionContext->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
			BC_ASSERT_TRUE(receivedMessage == plainMessage);
		}


	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}
}
static void lime_encryptionPolicy_suite(const lime::CurveId curve) {
	// create DB
	std::string dbBaseFilename("lime_encryptionPolicy");
	auto dbFilenameAlice = dbBaseFilename;
	dbFilenameAlice.append(".alice.").append(CurveId2String(curve)).append(".sqlite3");
	auto dbFilenameBob = dbBaseFilename;
	dbFilenameBob.append(".bob.").append(CurveId2String(curve)).append(".sqlite3");
	remove(dbFilenameAlice.data()); // delete the database file if already exists
	remove(dbFilenameBob.data()); // delete the database file if already exists

	lime_tester::events_counters_t counters={};
	int expected_success=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};

		std::vector<lime::CurveId> algos{curve};
	try {
		// create 2 devices for alice and 1 for bob
		// create Manager and device for alice
		auto aliceManager = make_shared<LimeManager>(dbFilenameAlice, X3DHServerPost);
		auto aliceDevice1Id = lime_tester::makeRandomDeviceName("alice.d1.");
		aliceManager->create_user(*aliceDevice1Id, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));
		auto aliceDevice2Id = lime_tester::makeRandomDeviceName("alice.d2.");
		aliceManager->create_user(*aliceDevice2Id, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));

		// Create manager and device for bob
		auto bobManager = make_shared<LimeManager>(dbFilenameBob, X3DHServerPost);
		auto bobDeviceId = lime_tester::makeRandomDeviceName("bob.d");
		bobManager->create_user(*bobDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));

		// Bob encrypts to alice and we check result
		/**** Short messages ****/
		lime_encryptionPolicy_test(algos, aliceManager, aliceDevice1Id, aliceDevice2Id,
				bobManager, bobDeviceId,
				lime_tester::shortMessage, false, // single recipient
				lime::EncryptionPolicy::optimizeUploadSize, false, // default policy(->optimizeUploadSize)
				lime::EncryptionPolicy::DRMessage); // -> DRMessage
	
		lime_encryptionPolicy_test(algos, aliceManager, aliceDevice1Id, aliceDevice2Id,
				bobManager, bobDeviceId,
				lime_tester::shortMessage, false, // single recipient
				lime::EncryptionPolicy::optimizeUploadSize, true, // force to optimizeUploadSize
				lime::EncryptionPolicy::DRMessage); // -> DRMessage
	
		lime_encryptionPolicy_test(algos, aliceManager, aliceDevice1Id, aliceDevice2Id,
				bobManager, bobDeviceId,
				lime_tester::shortMessage, false, // single recipient
				lime::EncryptionPolicy::optimizeGlobalBandwidth, true, // force to optimizeGlobalBandwidth
				lime::EncryptionPolicy::DRMessage); // -> DRMessage
	
		lime_encryptionPolicy_test(algos, aliceManager, aliceDevice1Id, aliceDevice2Id,
				bobManager, bobDeviceId,
				lime_tester::shortMessage, false, // single recipient
				lime::EncryptionPolicy::DRMessage, true, // force to DRMessage
				lime::EncryptionPolicy::DRMessage); // -> DRMessage
	
		lime_encryptionPolicy_test(algos, aliceManager, aliceDevice1Id, aliceDevice2Id,
				bobManager, bobDeviceId,
				lime_tester::shortMessage, false, // single recipient
				lime::EncryptionPolicy::cipherMessage, true, // force to cipherMessage
				lime::EncryptionPolicy::cipherMessage); // -> cipherMessage
	
		lime_encryptionPolicy_test(algos, aliceManager, aliceDevice1Id, aliceDevice2Id,
				bobManager, bobDeviceId,
				lime_tester::shortMessage, true, //multiple recipient
				lime::EncryptionPolicy::optimizeUploadSize, false, // default policy(->optimizeUploadSize)
				lime::EncryptionPolicy::DRMessage); // -> DRMessage
	
		lime_encryptionPolicy_test(algos, aliceManager, aliceDevice1Id, aliceDevice2Id,
				bobManager, bobDeviceId,
				lime_tester::shortMessage, true, //multiple recipient
				lime::EncryptionPolicy::optimizeUploadSize, true, // force to optimizeUploadSize
				lime::EncryptionPolicy::DRMessage); // -> DRMessage
	
		lime_encryptionPolicy_test(algos, aliceManager, aliceDevice1Id, aliceDevice2Id,
				bobManager, bobDeviceId,
				lime_tester::shortMessage, true, //multiple recipient
				lime::EncryptionPolicy::optimizeGlobalBandwidth, true, // force to optimizeGlobalBandwidth
				lime::EncryptionPolicy::DRMessage); // -> DRMessage
	
	
		lime_encryptionPolicy_test(algos, aliceManager, aliceDevice1Id, aliceDevice2Id,
				bobManager, bobDeviceId,
				lime_tester::shortMessage, true, //multiple recipient
				lime::EncryptionPolicy::DRMessage, true, // force to DRMessage
				lime::EncryptionPolicy::DRMessage); // -> DRMessage
	
		lime_encryptionPolicy_test(algos, aliceManager, aliceDevice1Id, aliceDevice2Id,
				bobManager, bobDeviceId,
				lime_tester::shortMessage, true, //multiple recipient
				lime::EncryptionPolicy::cipherMessage, true, // force to DRMessage
				lime::EncryptionPolicy::cipherMessage); // -> cipherMessage
	
		/**** Long or veryLong messages ****/
		lime_encryptionPolicy_test(algos, aliceManager, aliceDevice1Id, aliceDevice2Id,
				bobManager, bobDeviceId,
				lime_tester::longMessage, false, // single recipient
				lime::EncryptionPolicy::optimizeUploadSize, false, // default policy(->optimizeUploadSize)
				lime::EncryptionPolicy::DRMessage); // -> DRMessage
	
		lime_encryptionPolicy_test(algos, aliceManager, aliceDevice1Id, aliceDevice2Id,
				bobManager, bobDeviceId,
				lime_tester::longMessage, false, // single recipient
				lime::EncryptionPolicy::optimizeUploadSize, true, // force to optimizeUploadSize
				lime::EncryptionPolicy::DRMessage); // -> DRMessage
	
		lime_encryptionPolicy_test(algos, aliceManager, aliceDevice1Id, aliceDevice2Id,
				bobManager, bobDeviceId,
				lime_tester::longMessage, false, // single recipient
				lime::EncryptionPolicy::optimizeGlobalBandwidth, true, // force to optimizeGlobalBandwidth
				lime::EncryptionPolicy::DRMessage); // -> DRMessage
	
		lime_encryptionPolicy_test(algos, aliceManager, aliceDevice1Id, aliceDevice2Id,
				bobManager, bobDeviceId,
				lime_tester::longMessage, false, // single recipient
				lime::EncryptionPolicy::DRMessage, true, // force to DRMessage
				lime::EncryptionPolicy::DRMessage); // -> DRMessage
	
		lime_encryptionPolicy_test(algos, aliceManager, aliceDevice1Id, aliceDevice2Id,
				bobManager, bobDeviceId,
				lime_tester::longMessage, false, // single recipient
				lime::EncryptionPolicy::cipherMessage, true, // force to cipherMessage
				lime::EncryptionPolicy::cipherMessage); // -> cipherMessage
	
		lime_encryptionPolicy_test(algos, aliceManager, aliceDevice1Id, aliceDevice2Id,
				bobManager, bobDeviceId,
				lime_tester::longMessage, true, //multiple recipient
				lime::EncryptionPolicy::optimizeUploadSize, false, // default policy(->optimizeUploadSize)
				lime::EncryptionPolicy::cipherMessage); // -> cipherMessage
	
		lime_encryptionPolicy_test(algos, aliceManager, aliceDevice1Id, aliceDevice2Id,
				bobManager, bobDeviceId,
				lime_tester::longMessage, true, //multiple recipient
				lime::EncryptionPolicy::optimizeUploadSize, true, // force to optimizeUploadSize
				lime::EncryptionPolicy::cipherMessage); // -> cipherMessage
	
		lime_encryptionPolicy_test(algos, aliceManager, aliceDevice1Id, aliceDevice2Id,
				bobManager, bobDeviceId,
				lime_tester::longMessage, true, //multiple recipient
				lime::EncryptionPolicy::optimizeGlobalBandwidth, true, // force to optimizeGlobalBandwidth
				lime::EncryptionPolicy::DRMessage); // -> DRMessage (we need a very long message to switch to cipherMessage with that setting)
	
		lime_encryptionPolicy_test(algos, aliceManager, aliceDevice1Id, aliceDevice2Id,
				bobManager, bobDeviceId,
				lime_tester::veryLongMessage, true, //multiple recipient
				lime::EncryptionPolicy::optimizeGlobalBandwidth, true, // force to optimizeGlobalBandwidth
				lime::EncryptionPolicy::cipherMessage); // -> cipherMessage (we need a very long message to switch to cipherMessage with that setting)
	
		lime_encryptionPolicy_test(algos, aliceManager, aliceDevice1Id, aliceDevice2Id,
				bobManager, bobDeviceId,
				lime_tester::longMessage, true, //multiple recipient
				lime::EncryptionPolicy::DRMessage, true, // force to DRMessage
				lime::EncryptionPolicy::DRMessage); // -> DRMessage
	
		lime_encryptionPolicy_test(algos, aliceManager, aliceDevice1Id, aliceDevice2Id,
				bobManager, bobDeviceId,
				lime_tester::longMessage, true, //multiple recipient
				lime::EncryptionPolicy::cipherMessage, true, // force to cipherMessage
				lime::EncryptionPolicy::cipherMessage); // -> cipherMessage
	
		if (cleanDatabase) {
			aliceManager->delete_user(DeviceId(*aliceDevice1Id, curve), callback);
			aliceManager->delete_user(DeviceId(*aliceDevice2Id, curve), callback);
			bobManager->delete_user(DeviceId(*bobDeviceId, curve), callback);
			expected_success +=3;
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success,lime_tester::wait_for_timeout));
			remove(dbFilenameAlice.data());
			remove(dbFilenameBob.data());
		}
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}

}

static void lime_encryptionPolicy() {
#ifdef EC25519_ENABLED
	lime_encryptionPolicy_suite(lime::CurveId::c25519);
#endif
#ifdef EC448_ENABLED
	lime_encryptionPolicy_suite(lime::CurveId::c448);
#endif
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	lime_encryptionPolicy_suite(lime::CurveId::c25519k512);

	lime_encryptionPolicy_suite(lime::CurveId::c25519mlk512);
#endif
#ifdef EC448_ENABLED
	lime_encryptionPolicy_suite(lime::CurveId::c448mlk1024);
#endif
#endif
}

/**
 * Scenario:
 * - create Bob and Alice devices
 * - retrieve their respective Identity keys
 * - check if they are verified -> they shall not be
 * - set alice key as verified in bob's context
 * - check it is now verified
 * - set it to unsafe and check
 * - set it as non verified and check
 * - set it to unsafe and then untrusted using the alternative API without giving the Ik
 * - try to set it to trusted using the API without Ik, we shall have and exception
 * - try to set it to unknown, we shall have and exception
 * - try to set it to fail, we shall have and exception
 * - try to set a different alice identity key in bob's context, we shall have an exception
 * - bob encrypts a message to alice -> check return status give NOT all recipients trusted
 * - set alice key as verified in bob's context
 * - bob encrypts a message to alice -> check return status give all recipients trusted
 * - set a fake bob key in alice context
 * - try to decrypt bob's message, it shall fail
 * - alice try to encrypt a message to bob, it shall fail
 */
static void lime_identityVerifiedStatus_test(const lime::CurveId curve) {
	const std::string dbBaseFilename{"lime_identityVerifiedStatus"};
	// create DB
	std::string dbFilenameAlice{dbBaseFilename};
	std::string dbFilenameBob{dbBaseFilename};
	dbFilenameAlice.append(".alice.").append(CurveId2String(curve)).append(".sqlite3");
	dbFilenameBob.append(".bob.").append(CurveId2String(curve)).append(".sqlite3");

	remove(dbFilenameAlice.data()); // delete the database file if already exists
	remove(dbFilenameBob.data()); // delete the database file if already exists

	lime_tester::events_counters_t counters={};
	int expected_success=0;
	int expected_failure=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};
	// declare variable outside the try block as we will generate exceptions during the test
	std::unique_ptr<LimeManager> aliceManager = nullptr;
	std::unique_ptr<LimeManager> bobManager = nullptr;
	std::shared_ptr<std::string> aliceDeviceId = nullptr;
	std::shared_ptr<std::string> bobDeviceId = nullptr;
	std::vector<uint8_t> aliceIk{};
	std::vector<uint8_t> bobIk{};
	std::vector<uint8_t> fakeIk{};

	std::vector<lime::CurveId> algos{curve};
	try {
		// create Manager and device for alice and bob
		aliceManager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);
		aliceDeviceId = lime_tester::makeRandomDeviceName("alice.d1.");
		aliceManager->create_user(*aliceDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		bobManager = make_unique<LimeManager>(dbFilenameBob, X3DHServerPost);
		bobDeviceId = lime_tester::makeRandomDeviceName("bob.d1.");
		bobManager->create_user(*bobDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		expected_success += 2;
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, expected_success,lime_tester::wait_for_timeout));

		// retrieve their respective Ik
		std::map<lime::CurveId, std::vector<uint8_t>> Iks{};
		aliceManager->get_selfIdentityKey(*aliceDeviceId, algos, Iks);
		aliceIk = Iks[curve];
		Iks.clear();
		bobManager->get_selfIdentityKey(*bobDeviceId, algos, Iks);
		bobIk = Iks[curve];
		// build the fake alice Ik
		fakeIk = aliceIk;
		fakeIk[0] ^= 0xFF;


		// check their status: they don't know each other
		BC_ASSERT_TRUE(aliceManager->get_peerDeviceStatus(*bobDeviceId) == lime::PeerDeviceStatus::unknown);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::unknown);

		// set alice Id key as verified in Bob's Manager and check it worked
		bobManager->set_peerDeviceStatus(*aliceDeviceId, curve, aliceIk, lime::PeerDeviceStatus::trusted);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::trusted);
		// set it to unsafe and check it worked
		bobManager->set_peerDeviceStatus(*aliceDeviceId, curve, aliceIk, lime::PeerDeviceStatus::unsafe);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::unsafe);

		// reset it to untrusted and check it is still unsafe : we can escape unsafe only by setting to safe
		bobManager->set_peerDeviceStatus(*aliceDeviceId, curve, aliceIk, lime::PeerDeviceStatus::untrusted);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::unsafe);

		// set alice Id key as verified and check it this time it worked
		bobManager->set_peerDeviceStatus(*aliceDeviceId, curve, aliceIk, lime::PeerDeviceStatus::trusted);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::trusted);

		// set to untrusted without using alice Ik
		bobManager->set_peerDeviceStatus(*aliceDeviceId, algos, lime::PeerDeviceStatus::untrusted);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::untrusted);

		// set to unsafe without using alice Ik
		bobManager->set_peerDeviceStatus(*aliceDeviceId, algos, lime::PeerDeviceStatus::unsafe);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::unsafe);

		// try to set it to trusted without giving the Ik, it shall be ignored
		bobManager->set_peerDeviceStatus(*aliceDeviceId, algos, lime::PeerDeviceStatus::trusted);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::unsafe);

		// set it back to trusted and check it this time it worked
		bobManager->set_peerDeviceStatus(*aliceDeviceId, curve, aliceIk, lime::PeerDeviceStatus::trusted);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::trusted);

		// try to set it to unknown, it shall be ignored
		bobManager->set_peerDeviceStatus(*aliceDeviceId, curve, aliceIk, lime::PeerDeviceStatus::unknown);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::trusted);

		// try to set it to fail, it shall be ignored
		bobManager->set_peerDeviceStatus(*aliceDeviceId, curve, aliceIk, lime::PeerDeviceStatus::fail);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::trusted);

		// try to set another key for alice in bob's context, setting it to untrusted, it shall be ok as the Ik is ignored when setting to untrusted
		bobManager->set_peerDeviceStatus(*aliceDeviceId, curve, fakeIk, lime::PeerDeviceStatus::untrusted);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::untrusted);

		// same goes for unsafe
		bobManager->set_peerDeviceStatus(*aliceDeviceId, curve, fakeIk, lime::PeerDeviceStatus::unsafe);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::unsafe);

		// set it back to trusted with the real key, it shall be Ok as it is still the one present in local storage
		bobManager->set_peerDeviceStatus(*aliceDeviceId, curve, aliceIk, lime::PeerDeviceStatus::trusted);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::trusted);

	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}

	auto gotException = false;

	try {
		// try to set another key for alice in bob's context, it shall generate an exception
		bobManager->set_peerDeviceStatus(*aliceDeviceId, curve, fakeIk, lime::PeerDeviceStatus::trusted);
	} catch (BctbxException &) {
		BC_PASS("");
		gotException = true;
	}

	BC_ASSERT_TRUE(gotException);
	gotException = false;

	try {
		// Now delete the alice device from Bob's cache and check its status is now back to unknown
		bobManager->delete_peerDevice(*aliceDeviceId);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::unknown);

		// set the device with the fake Ik but to untrusted so it won't be actually stored and shall still be unknown
		bobManager->set_peerDeviceStatus(*aliceDeviceId, curve, fakeIk, lime::PeerDeviceStatus::untrusted);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::unknown);

		// set the device with the fake Ik but to unsafe so the key shall not be registered in base but the user will
		bobManager->set_peerDeviceStatus(*aliceDeviceId, curve, fakeIk, lime::PeerDeviceStatus::unsafe);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::unsafe);

		// Set it to trusted, still using the fake Ik, it shall replace the invalid/empty Ik replacing it with the fake one
		bobManager->set_peerDeviceStatus(*aliceDeviceId, curve, fakeIk, lime::PeerDeviceStatus::trusted);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::trusted);
		bobManager->set_peerDeviceStatus(*aliceDeviceId, curve, fakeIk, lime::PeerDeviceStatus::trusted); // do it twice so we're sure the store Ik is the fake one
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::trusted);
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}

	try {
		// same than above but using the actual key : try to set it to trusted, it shall generate an exception as the Ik is the fake one in storage
		bobManager->set_peerDeviceStatus(*aliceDeviceId, curve, aliceIk, lime::PeerDeviceStatus::trusted);
	} catch (BctbxException &) {
		BC_PASS("");
		gotException = true;

		// Now delete the alice device from Bob's cache and check its status is now back to unknown
		bobManager->delete_peerDevice(*aliceDeviceId);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::unknown);
	}

	BC_ASSERT_TRUE(gotException);
	gotException = false;

	try {
		// Bob encrypts a message for Alice, alice device status shall be : unknown(it is the first message bob sends and alice is not in cache)
		auto encryptionContext = make_shared<EncryptionContext>("alice", lime_tester::messages_pattern[0]);
		encryptionContext->addRecipient(*aliceDeviceId);
		bobManager->encrypt(*bobDeviceId, algos, encryptionContext, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		BC_ASSERT_TRUE(encryptionContext->m_recipients[0].peerStatus == lime::PeerDeviceStatus::unknown);

		// Bob encrypts a second message for Alice, alice device status shall now be : untrusted(we know that device but didn't share the trust yet)
		encryptionContext = make_shared<EncryptionContext>("alice", lime_tester::messages_pattern[1]);
		encryptionContext->addRecipient(*aliceDeviceId);
		bobManager->encrypt(*bobDeviceId, algos, encryptionContext, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		BC_ASSERT_TRUE(encryptionContext->m_recipients[0].peerStatus == lime::PeerDeviceStatus::untrusted);

		// set the key as verified in bob's context
		bobManager->set_peerDeviceStatus(*aliceDeviceId, curve, aliceIk, lime::PeerDeviceStatus::trusted);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::trusted);

		// Bob encrypts a message for Alice, alice device status shall now be : trusted
		encryptionContext = make_shared<EncryptionContext>("alice", lime_tester::messages_pattern[2]);
		encryptionContext->addRecipient(*aliceDeviceId);
		bobManager->encrypt(*bobDeviceId, algos, encryptionContext, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		BC_ASSERT_TRUE(encryptionContext->m_recipients[0].peerStatus == lime::PeerDeviceStatus::trusted);

		// set a fake bob key in alice context(set is as trusted otherwise the request is just ignored)
		fakeIk = bobIk;
		fakeIk[0] ^= 0xFF;
		aliceManager->set_peerDeviceStatus(*bobDeviceId, curve, fakeIk, lime::PeerDeviceStatus::trusted);

		// alice decrypt but it will fail as the identity key in X3DH init packet is not matching the one we assert as verified
		std::vector<uint8_t> receivedMessage{};
		BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDeviceId, "alice", *bobDeviceId, encryptionContext->m_recipients[0].DRmessage, encryptionContext->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::fail);

		// alice now try to encrypt to Bob but it will fail as key fetched from X3DH server won't match the one we assert as verified
		encryptionContext = make_shared<EncryptionContext>("bob", lime_tester::messages_pattern[3]);
		encryptionContext->addRecipient(*bobDeviceId);
		aliceManager->encrypt(*aliceDeviceId, algos, encryptionContext, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_failed,++expected_failure,lime_tester::wait_for_timeout));

		// delete bob's key from alice context and just set it to unsafe, he will get then no Ik in local storage
		aliceManager->delete_peerDevice(*bobDeviceId);
		BC_ASSERT_TRUE(aliceManager->get_peerDeviceStatus(*bobDeviceId) == lime::PeerDeviceStatus::unknown);
		aliceManager->set_peerDeviceStatus(*bobDeviceId, algos, lime::PeerDeviceStatus::unsafe);
		BC_ASSERT_TRUE(aliceManager->get_peerDeviceStatus(*bobDeviceId) == lime::PeerDeviceStatus::unsafe);

		// delete alice's key from Bob context, it will delete all session associated to Alice so when we encrypt a new message, it will fetch a new OPk as the previous one was deleted by alice
		bobManager->delete_peerDevice(*aliceDeviceId);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::unknown);
		// Bob encrypts a message for Alice, alice device status shall be : unknown(it is the first message bob sends and alice is not in cache)
		encryptionContext = make_shared<EncryptionContext>("alice", lime_tester::messages_pattern[1]);
		encryptionContext->addRecipient(*aliceDeviceId);
		bobManager->encrypt(*bobDeviceId, algos, encryptionContext, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		BC_ASSERT_TRUE(encryptionContext->m_recipients[0].peerStatus == lime::PeerDeviceStatus::unknown);

		// alice decrypts, this will update the empty Bob's Ik in storage using the X3DH init packet but shall give an unsafe status
		receivedMessage.clear();
		BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDeviceId, "alice", *bobDeviceId, encryptionContext->m_recipients[0].DRmessage, encryptionContext->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::unsafe);

		// delete bob's key from alice context
		aliceManager->delete_peerDevice(*bobDeviceId);
		BC_ASSERT_TRUE(aliceManager->get_peerDeviceStatus(*bobDeviceId) == lime::PeerDeviceStatus::unknown);

		// delete alice's key from Bob context, it will delete all session associated to Alice so when we encrypt a new message, it will fetch a new OPk as the previous one was deleted by alice
		bobManager->delete_peerDevice(*aliceDeviceId);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::unknown);
		// Bob encrypts a message for Alice, alice device status shall be : unknown(it is the first message bob sends and alice is not in cache)
		encryptionContext = make_shared<EncryptionContext>("alice", lime_tester::messages_pattern[5]);
		encryptionContext->addRecipient(*aliceDeviceId);
		bobManager->encrypt(*bobDeviceId, algos, encryptionContext, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		BC_ASSERT_TRUE(encryptionContext->m_recipients[0].peerStatus == lime::PeerDeviceStatus::unknown);

		// alice tries again to decrypt but it shall work and return status unknown as we just deleted bob's device
		receivedMessage.clear();
		BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDeviceId, "alice", *bobDeviceId, encryptionContext->m_recipients[0].DRmessage, encryptionContext->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::unknown);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[5]);

		// now set bob's to trusted in alice cache, it shall work as key retrieved from X3DH init message during decryption match the one we're giving
		aliceManager->set_peerDeviceStatus(*bobDeviceId, curve, bobIk, lime::PeerDeviceStatus::trusted);
		BC_ASSERT_TRUE(aliceManager->get_peerDeviceStatus(*bobDeviceId) == lime::PeerDeviceStatus::trusted);

		if (cleanDatabase) {
			aliceManager->delete_user(DeviceId(*aliceDeviceId, curve), callback);
			bobManager->delete_user(DeviceId(*bobDeviceId, curve), callback);
			expected_success +=2;
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success,lime_tester::wait_for_timeout));
			remove(dbFilenameAlice.data());
			remove(dbFilenameBob.data());
		}

	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}
}

static void lime_identityVerifiedStatus() {
#ifdef EC25519_ENABLED
	lime_identityVerifiedStatus_test(lime::CurveId::c25519);
#endif
#ifdef EC448_ENABLED
	lime_identityVerifiedStatus_test(lime::CurveId::c448);
#endif
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	lime_identityVerifiedStatus_test(lime::CurveId::c25519k512);

	lime_identityVerifiedStatus_test(lime::CurveId::c25519mlk512);
#endif
#ifdef EC448_ENABLED
	lime_identityVerifiedStatus_test(lime::CurveId::c448mlk1024);
#endif
#endif
}

/**
 * Scenario
 * - Create managers and DB for alice, bob, carol and dave
 * - Get their Identity key and gives alice (apply mutual action with alice's identity key on bob's, carol's and dave's manager)
 *     - bob's keys as trusted
 *     - set carol's key as trusted and then untrusted so it is in Alice local storage as untrusted
 *     - set dave's key as untrusted, as it was not konwn before, it shall not be registered at all in Alice local storage
 * - Alice encrypts a message to bob, carol and dave. Check that the peerDevice status given after encryption are respectively: trusted, untrusted, unknown
 * - Recipients decrypt Alice message and check we have the expected return values:
 *   - Bob: trusted
 *   - Carol: untrusted
 *   - Dave: unknown
 * - Alice encrypts a second message to bob, carol and dave. Check that the peerDevice status given after encryption are respectively: trusted, untrusted, untrusted
 * - Recipients decrypt Alice message and check we have the expected return values:
 *   - Bob: trusted
 *   - Carol: untrusted
 *   - Dave: untrusted
 */
static void lime_peerDeviceStatus_test(const lime::CurveId curve) {
	const std::string dbBaseFilename{"lime_peerDeviceStatus"};
	// create DB
	std::string dbFilenameAlice{dbBaseFilename};
	std::string dbFilenameBob{dbBaseFilename};
	std::string dbFilenameCarol{dbBaseFilename};
	std::string dbFilenameDave{dbBaseFilename};

	dbFilenameAlice.append(".alice.").append(CurveId2String(curve)).append(".sqlite3");
	dbFilenameBob.append(".bob.").append(CurveId2String(curve)).append(".sqlite3");
	dbFilenameCarol.append(".carol.").append(CurveId2String(curve)).append(".sqlite3");
	dbFilenameDave.append(".dave.").append(CurveId2String(curve)).append(".sqlite3");

	remove(dbFilenameAlice.data()); // delete the database file if already exists
	remove(dbFilenameBob.data()); // delete the database file if already exists
	remove(dbFilenameCarol.data()); // delete the database file if already exists
	remove(dbFilenameDave.data()); // delete the database file if already exists

	lime_tester::events_counters_t counters={};
	int expected_success=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};

	try {
		std::vector<lime::CurveId> algos{curve};
		// create Manager and devices
		auto aliceManager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);
		auto aliceDeviceId = lime_tester::makeRandomDeviceName("alice.");
		aliceManager->create_user(*aliceDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);

		auto bobManager = make_unique<LimeManager>(dbFilenameBob, X3DHServerPost);
		auto bobDeviceId = lime_tester::makeRandomDeviceName("bob.");
		bobManager->create_user(*bobDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);

		auto carolManager = make_unique<LimeManager>(dbFilenameCarol, X3DHServerPost);
		auto carolDeviceId = lime_tester::makeRandomDeviceName("carol.");
		carolManager->create_user(*carolDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);

		auto daveManager = make_unique<LimeManager>(dbFilenameDave, X3DHServerPost);
		auto daveDeviceId = lime_tester::makeRandomDeviceName("dave.");
		daveManager->create_user(*daveDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);

		// This is a list of all device's id to test getting status of a device list
		std::list<std::string> allDevicesId{*aliceDeviceId, *bobDeviceId, *carolDeviceId, *daveDeviceId};

		expected_success += 4;
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, expected_success,lime_tester::wait_for_timeout));

		// retrieve their respective Ik
		std::vector<uint8_t> aliceIk{};
		std::vector<uint8_t> bobIk{};
		std::vector<uint8_t> carolIk{};
		std::vector<uint8_t> daveIk{};
		std::map<lime::CurveId, std::vector<uint8_t>> Iks{};
		aliceManager->get_selfIdentityKey(*aliceDeviceId, algos, Iks);
		aliceIk = Iks[curve];
		Iks.clear();
		bobManager->get_selfIdentityKey(*bobDeviceId, algos, Iks);
		bobIk = Iks[curve];
		Iks.clear();
		carolManager->get_selfIdentityKey(*carolDeviceId, algos, Iks);
		carolIk = Iks[curve];
		Iks.clear();
		daveManager->get_selfIdentityKey(*daveDeviceId, algos, Iks);
		daveIk = Iks[curve];

		// set alice as untrusted in bob local storage, so it will store it with an invalid/empty Ik
		bobManager->set_peerDeviceStatus(*aliceDeviceId, algos, lime::PeerDeviceStatus::unsafe);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::unsafe);

		// exchange trust between alice and bob
		bobManager->set_peerDeviceStatus(*aliceDeviceId, curve, aliceIk, lime::PeerDeviceStatus::trusted); // this one will replace the invalid/empty Ik by the given one
		aliceManager->set_peerDeviceStatus(*bobDeviceId, curve, bobIk, lime::PeerDeviceStatus::trusted);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::trusted);
		BC_ASSERT_TRUE(aliceManager->get_peerDeviceStatus(*bobDeviceId) == lime::PeerDeviceStatus::trusted);

		// alice and carol gets trust and back to not trust so the Ik gets registered in their local storage
		carolManager->set_peerDeviceStatus(*aliceDeviceId, curve, aliceIk, lime::PeerDeviceStatus::trusted);
		aliceManager->set_peerDeviceStatus(*carolDeviceId, curve, carolIk, lime::PeerDeviceStatus::trusted);
		carolManager->set_peerDeviceStatus(*aliceDeviceId, curve, aliceIk, lime::PeerDeviceStatus::untrusted);
		aliceManager->set_peerDeviceStatus(*carolDeviceId, curve, carolIk, lime::PeerDeviceStatus::untrusted);
		BC_ASSERT_TRUE(carolManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::untrusted);
		BC_ASSERT_TRUE(aliceManager->get_peerDeviceStatus(*carolDeviceId) == lime::PeerDeviceStatus::untrusted);

		// alice and dave gets just an untrusted setting, as they do not know each other, it shall not affect their respective local storage and they would remain unknown
		daveManager->set_peerDeviceStatus(*aliceDeviceId, curve, aliceIk, lime::PeerDeviceStatus::untrusted);
		aliceManager->set_peerDeviceStatus(*daveDeviceId, curve, daveIk, lime::PeerDeviceStatus::untrusted);
		BC_ASSERT_TRUE(daveManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::unknown);
		BC_ASSERT_TRUE(aliceManager->get_peerDeviceStatus(*daveDeviceId) == lime::PeerDeviceStatus::unknown);

		// Now Alice's storage has Bob as trusted and Carol as untrusted and does not know Dave
		// Getting status for all of them as a list shall return unknown (alice considers herself as trusted)
		BC_ASSERT_TRUE(aliceManager->get_peerDeviceStatus(allDevicesId) == lime::PeerDeviceStatus::unknown);

		// Alice encrypts a message for Bob, Carol and Dave
		auto encryptionContext = make_shared<EncryptionContext>("my friends group", lime_tester::messages_pattern[0]);
		encryptionContext->addRecipient(*bobDeviceId);
		encryptionContext->addRecipient(*carolDeviceId);
		encryptionContext->addRecipient(*daveDeviceId);
		aliceManager->encrypt(*aliceDeviceId, algos, encryptionContext, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		BC_ASSERT_TRUE(encryptionContext->m_recipients[0].peerStatus == lime::PeerDeviceStatus::trusted); // recipient 0 is Bob: trusted
		BC_ASSERT_TRUE(encryptionContext->m_recipients[1].peerStatus == lime::PeerDeviceStatus::untrusted); // recipient 1 is Carol: untrusted
		BC_ASSERT_TRUE(encryptionContext->m_recipients[2].peerStatus == lime::PeerDeviceStatus::unknown); // recipient 2 is Dave: unknown

		// Now Alice's storage has Bob as trusted and Carol as untrusted and Dave untrusted
		// Getting status for all of them as a list shall return untrusted
		BC_ASSERT_TRUE(aliceManager->get_peerDeviceStatus(allDevicesId) == lime::PeerDeviceStatus::untrusted);

		// recipients decrypt
		std::vector<uint8_t> receivedMessage{};
		// bob shall return trusted
		BC_ASSERT_TRUE(bobManager->decrypt(*bobDeviceId, "my friends group", *aliceDeviceId, encryptionContext->m_recipients[0].DRmessage, encryptionContext->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::trusted);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[0]);

		receivedMessage.clear();
		// carol shall return untrusted
		BC_ASSERT_TRUE(carolManager->decrypt(*carolDeviceId, "my friends group", *aliceDeviceId, encryptionContext->m_recipients[1].DRmessage, encryptionContext->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[0]);

		receivedMessage.clear();
		// dave shall return unknown
		BC_ASSERT_TRUE(daveManager->decrypt(*daveDeviceId, "my friends group", *aliceDeviceId, encryptionContext->m_recipients[2].DRmessage, encryptionContext->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::unknown);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[0]);

		// Alice encrypts a second message for Bob, Carol and Dave
		encryptionContext = make_shared<EncryptionContext>("my friends group", lime_tester::messages_pattern[1]);
		encryptionContext->addRecipient(*bobDeviceId);
		encryptionContext->addRecipient(*carolDeviceId);
		encryptionContext->addRecipient(*daveDeviceId);
		aliceManager->encrypt(*aliceDeviceId, algos, encryptionContext, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		BC_ASSERT_TRUE(encryptionContext->m_recipients[0].peerStatus == lime::PeerDeviceStatus::trusted); // recipient 0 is Bob: trusted
		BC_ASSERT_TRUE(encryptionContext->m_recipients[1].peerStatus == lime::PeerDeviceStatus::untrusted); // recipient 1 is Carol: untrusted
		BC_ASSERT_TRUE(encryptionContext->m_recipients[2].peerStatus == lime::PeerDeviceStatus::untrusted); // recipient 2 is Dave: untrusted

		// recipients decrypt
		receivedMessage.clear();
		// bob shall return trusted
		BC_ASSERT_TRUE(bobManager->decrypt(*bobDeviceId, "my friends group", *aliceDeviceId, encryptionContext->m_recipients[0].DRmessage, encryptionContext->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::trusted);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[1]);

		receivedMessage.clear();
		// carol shall return untrusted
		BC_ASSERT_TRUE(carolManager->decrypt(*carolDeviceId, "my friends group", *aliceDeviceId, encryptionContext->m_recipients[1].DRmessage, encryptionContext->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[1]);

		receivedMessage.clear();
		// dave shall return untrusted now
		BC_ASSERT_TRUE(daveManager->decrypt(*daveDeviceId, "my friends group", *aliceDeviceId, encryptionContext->m_recipients[2].DRmessage, encryptionContext->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[1]);

		// set Dave's status to trusted in Alice's cache and query the group status, it still shall be untrusted
		aliceManager->set_peerDeviceStatus(*daveDeviceId, curve, daveIk, lime::PeerDeviceStatus::trusted);
		BC_ASSERT_TRUE(aliceManager->get_peerDeviceStatus(allDevicesId) == lime::PeerDeviceStatus::untrusted);

		// now also set Carol as trusted, the group status shall be trusted
		aliceManager->set_peerDeviceStatus(*carolDeviceId, curve, carolIk, lime::PeerDeviceStatus::trusted);
		BC_ASSERT_TRUE(aliceManager->get_peerDeviceStatus(allDevicesId) == lime::PeerDeviceStatus::trusted);

		// Turn Dave to unsafe, the group status shall be unsafe
		aliceManager->set_peerDeviceStatus(*daveDeviceId, curve, daveIk, lime::PeerDeviceStatus::unsafe);
		BC_ASSERT_TRUE(aliceManager->get_peerDeviceStatus(allDevicesId) == lime::PeerDeviceStatus::unsafe);
		
		// Remove Carol from Alice cache, Alice is unknown but the group is still unsafe
		aliceManager->delete_peerDevice(*carolDeviceId);
		BC_ASSERT_TRUE(aliceManager->get_peerDeviceStatus(*carolDeviceId) == lime::PeerDeviceStatus::unknown);
		BC_ASSERT_TRUE(aliceManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::trusted); // query herself as peer, should be trusted
		BC_ASSERT_TRUE(aliceManager->get_peerDeviceStatus(std::list<std::string>{*aliceDeviceId}) == lime::PeerDeviceStatus::trusted); // query herself as peer, should be trusted
		BC_ASSERT_TRUE(aliceManager->get_peerDeviceStatus(allDevicesId) == lime::PeerDeviceStatus::unsafe);

		if (cleanDatabase) {
			aliceManager->delete_user(DeviceId(*aliceDeviceId, curve), callback);
			bobManager->delete_user(DeviceId(*bobDeviceId, curve), callback);
			carolManager->delete_user(DeviceId(*carolDeviceId, curve), callback);
			daveManager->delete_user(DeviceId(*daveDeviceId, curve), callback);
			expected_success +=4;
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success,lime_tester::wait_for_timeout));
			remove(dbFilenameAlice.data());
			remove(dbFilenameBob.data());
			remove(dbFilenameCarol.data());
			remove(dbFilenameDave.data());
		}

	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}
}

static void lime_peerDeviceStatus() {
#ifdef EC25519_ENABLED
	lime_peerDeviceStatus_test(lime::CurveId::c25519);
#endif
#ifdef EC448_ENABLED
	lime_peerDeviceStatus_test(lime::CurveId::c448);
#endif
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	lime_peerDeviceStatus_test(lime::CurveId::c25519k512);

	lime_peerDeviceStatus_test(lime::CurveId::c25519mlk512);
#endif
#ifdef EC448_ENABLED
	lime_peerDeviceStatus_test(lime::CurveId::c448mlk1024);
#endif
#endif
}

/**
 * Scenario
 * - Create managers and DB for alice, bob
 * - Set Bob as unsafe in Alice's base, so Alice would store an empty/invalid identity key for Bob
 * - Set Alice as unsafe in Bob's base, so Bob would store an empty/invalid identity key for Alice
 * - Alice encrypts a message to bob(so it would fetch Bob's Identity key and replace the empty/invalid one in its DB) but encrypt shall give a unsafe peer device status for Bob
 * - Bob decrypt Alice message and check we have the expected Alice status: unsafe. Bob shall replace the invalid key in its local storage for Alice by the one found in the incoming message
 */
static void lime_encryptToUnsafe_test(const lime::CurveId curve) {
	const std::string dbBaseFilename{"lime_encryptToUnsafe"};
	// create DB
	std::string dbFilenameAlice{dbBaseFilename};
	std::string dbFilenameBob{dbBaseFilename};

	dbFilenameAlice.append(".alice.").append(CurveId2String(curve)).append(".sqlite3");
	dbFilenameBob.append(".bob.").append(CurveId2String(curve)).append(".sqlite3");

	remove(dbFilenameAlice.data()); // delete the database file if already exists
	remove(dbFilenameBob.data()); // delete the database file if already exists

	lime_tester::events_counters_t counters={};
	int expected_success=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};

	try {
		std::vector<lime::CurveId> algos{curve};
		// create Manager and devices
		auto aliceManager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);
		auto aliceDeviceId = lime_tester::makeRandomDeviceName("alice.");
		aliceManager->create_user(*aliceDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);

		auto bobManager = make_unique<LimeManager>(dbFilenameBob, X3DHServerPost);
		auto bobDeviceId = lime_tester::makeRandomDeviceName("bob.");
		bobManager->create_user(*bobDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);

		expected_success += 2;
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, expected_success,lime_tester::wait_for_timeout));

		// set each of them to unsafe for the other
		bobManager->set_peerDeviceStatus(*aliceDeviceId, algos, lime::PeerDeviceStatus::unsafe);
		aliceManager->set_peerDeviceStatus(*bobDeviceId, algos, lime::PeerDeviceStatus::unsafe);
		BC_ASSERT_TRUE(bobManager->get_peerDeviceStatus(*aliceDeviceId) == lime::PeerDeviceStatus::unsafe);
		BC_ASSERT_TRUE(aliceManager->get_peerDeviceStatus(*bobDeviceId) == lime::PeerDeviceStatus::unsafe);

		// Alice encrypts a message for Bob
		auto encryptionContext = make_shared<EncryptionContext>("my friends group", lime_tester::messages_pattern[0]);
		encryptionContext->addRecipient(*bobDeviceId);
		aliceManager->encrypt(*aliceDeviceId, algos, encryptionContext, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		BC_ASSERT_TRUE(encryptionContext->m_recipients[0].peerStatus == lime::PeerDeviceStatus::unsafe); // recipient 0 is Bob: unsafe

		// recipients decrypt
		std::vector<uint8_t> receivedMessage{};
		// bob shall return unsafe
		BC_ASSERT_TRUE(bobManager->decrypt(*bobDeviceId, "my friends group", *aliceDeviceId, encryptionContext->m_recipients[0].DRmessage, encryptionContext->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::unsafe);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[0]);

		if (cleanDatabase) {
			aliceManager->delete_user(DeviceId(*aliceDeviceId, curve), callback);
			bobManager->delete_user(DeviceId(*bobDeviceId, curve), callback);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success+2,lime_tester::wait_for_timeout));
			remove(dbFilenameAlice.data());
			remove(dbFilenameBob.data());
		}

	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}
}

static void lime_encryptToUnsafe() {
#ifdef EC25519_ENABLED
	lime_encryptToUnsafe_test(lime::CurveId::c25519);
#endif
#ifdef EC448_ENABLED
	lime_encryptToUnsafe_test(lime::CurveId::c448);
#endif
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	lime_encryptToUnsafe_test(lime::CurveId::c25519k512);

	lime_encryptToUnsafe_test(lime::CurveId::c25519mlk512);
#endif
#ifdef EC448_ENABLED
	lime_encryptToUnsafe_test(lime::CurveId::c448mlk1024);
#endif
#endif
}

/**
 * scenario :
 * - check the Ik in pattern_db is retrieved as expected
 * - try asking for an unknown user, we shall get an exception
 */
static void lime_getSelfIk_test(const lime::CurveId curve, const std::string &dbFilename, const std::vector<uint8_t> &pattern) {
	// retrieve the Ik and check it matches given pattern
	std::unique_ptr<LimeManager> aliceManager = nullptr;
	std::map<lime::CurveId, std::vector<uint8_t>> Iks{};
	std::vector<uint8_t> Ik{};
	try  {
		// create Manager for alice
		aliceManager = make_unique<LimeManager>(dbFilename, X3DHServerPost);
		// retrieve alice identity key
		aliceManager->get_selfIdentityKey("alice", std::vector<lime::CurveId>{curve}, Iks);
		Ik = Iks[curve];

		BC_ASSERT_TRUE((Ik==pattern));
		Iks.clear();
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
		return;
	}

	// try to get the Ik of a user not in there, we shall get an exception
	try {
		aliceManager->get_selfIdentityKey("bob", std::vector<lime::CurveId>{curve}, Iks);
	} catch (BctbxException &) {
		// just swallow it
		BC_PASS("");
		return;
	}
	BC_FAIL("Get the Ik of a user not in local Storage didn't throw an exception");
}

static void lime_getSelfIk() {
#ifdef EC25519_ENABLED
	std::vector<uint8_t> pattern_selfIk_C25519 {{0x55, 0x6b, 0x4a, 0xc2, 0x24, 0xc1, 0xd4, 0xff, 0xb7, 0x44, 0x82, 0xe2, 0x3c, 0x75, 0x1c, 0x2b, 0x1c, 0xcb, 0xf6, 0xe2, 0x96, 0xcb, 0x18, 0x01, 0xc6, 0x76, 0x2d, 0x30, 0xa0, 0xa2, 0xbb, 0x27}};
	lime_getSelfIk_test(lime::CurveId::c25519, std::string(bc_tester_get_resource_dir_prefix()).append("/data/pattern_getSelfIk.C25519.sqlite3"), pattern_selfIk_C25519);
#endif
#ifdef EC448_ENABLED
	std::vector<uint8_t> pattern_selfIk_C448 {{0xe7, 0x96, 0x9e, 0x53, 0xd3, 0xbf, 0xfb, 0x4c, 0x6d, 0xdb, 0x79, 0xd2, 0xd7, 0x24, 0x91, 0x7b, 0xa8, 0x99, 0x87, 0x20, 0x23, 0xe1, 0xec, 0xd4, 0xb5, 0x76, 0x0f, 0xc2, 0x83, 0xae, 0x5a, 0xf9, 0x1d, 0x25, 0x47, 0xda, 0x0e, 0x71, 0x50, 0xd5, 0xaf, 0x79, 0x92, 0x48, 0xb0, 0xb6, 0x0f, 0xdc, 0x6f, 0x73, 0x3f, 0xd9, 0x9c, 0x2c, 0x95, 0xe3, 0x00}};
	lime_getSelfIk_test(lime::CurveId::c448, std::string(bc_tester_get_resource_dir_prefix()).append("/data/pattern_getSelfIk.C448.sqlite3"), pattern_selfIk_C448);
#endif
}

// create a DB in the given file with the given version
// exception must be catched by caller
static void write_db_version(uint32_t version, std::string &dbFilename) {
	soci::session sql;
	sql.open("sqlite3", dbFilename);
	sql<<"PRAGMA foreign_keys = ON;"; // make sure this connection enable foreign keys
	soci::transaction tr(sql);

	switch (version) {
		case 0x000001:
		{
			// CREATE OR IGNORE TABLE db_module_version(
			sql<<"CREATE TABLE IF NOT EXISTS db_module_version("
			"name VARCHAR(16) PRIMARY KEY,"
			"version UNSIGNED INTEGER NOT NULL"
			")";
			sql<<"INSERT INTO db_module_version(name,version) VALUES('lime',:DbVersion)", soci::use(version);
			sql<<"CREATE TABLE DR_sessions( \
				Did INTEGER NOT NULL DEFAULT 0, \
				Uid INTEGER NOT NULL DEFAULT 0, \
				sessionId INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, \
				Ns UNSIGNED INTEGER NOT NULL, \
				Nr UNSIGNED INTEGER NOT NULL, \
				PN UNSIGNED INTEGER NOT NULL, \
				DHr BLOB NOT NULL, \
				DHs BLOB NOT NULL, \
				RK BLOB NOT NULL, \
				CKs BLOB NOT NULL, \
				CKr BLOB NOT NULL, \
				AD BLOB NOT NULL, \
				Status INTEGER NOT NULL DEFAULT 1, \
				timeStamp DATETIME DEFAULT CURRENT_TIMESTAMP, \
				X3DHInit BLOB DEFAULT NULL, \
				FOREIGN KEY(Did) REFERENCES lime_PeerDevices(Did) ON UPDATE CASCADE ON DELETE CASCADE, \
				FOREIGN KEY(Uid) REFERENCES lime_LocalUsers(Uid) ON UPDATE CASCADE ON DELETE CASCADE);";
			sql<<"CREATE TABLE DR_MSk_DHr( \
				DHid INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, \
				sessionId INTEGER NOT NULL DEFAULT 0, \
				DHr BLOB NOT NULL, \
				received UNSIGNED INTEGER NOT NULL DEFAULT 0, \
				FOREIGN KEY(sessionId) REFERENCES DR_sessions(sessionId) ON UPDATE CASCADE ON DELETE CASCADE);";
			sql<<"CREATE TABLE DR_MSk_MK( \
				DHid INTEGER NOT NULL, \
				Nr INTEGER NOT NULL, \
				MK BLOB NOT NULL, \
				PRIMARY KEY( DHid , Nr ), \
				FOREIGN KEY(DHid) REFERENCES DR_MSk_DHr(DHid) ON UPDATE CASCADE ON DELETE CASCADE);";
			sql<<"CREATE TABLE lime_LocalUsers( \
				Uid INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, \
				UserId TEXT NOT NULL, \
				Ik BLOB NOT NULL, \
				server TEXT NOT NULL, \
				curveId INTEGER NOT NULL DEFAULT 0);";
			sql<<"CREATE TABLE lime_PeerDevices( \
				Did INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, \
				DeviceId TEXT NOT NULL, \
				Ik BLOB NOT NULL, \
				Status UNSIGNED INTEGER DEFAULT 0);";
			sql<<"CREATE TABLE X3DH_SPK( \
				SPKid UNSIGNED INTEGER PRIMARY KEY NOT NULL, \
				SPK BLOB NOT NULL, \
				timeStamp DATETIME DEFAULT CURRENT_TIMESTAMP, \
				Status INTEGER NOT NULL DEFAULT 1, \
				Uid INTEGER NOT NULL, \
				FOREIGN KEY(Uid) REFERENCES lime_LocalUsers(Uid) ON UPDATE CASCADE ON DELETE CASCADE);";
			sql<<"CREATE TABLE X3DH_OPK( \
				OPKid UNSIGNED INTEGER PRIMARY KEY NOT NULL, \
				OPK BLOB NOT NULL, \
				Uid INTEGER NOT NULL, \
				Status INTEGER NOT NULL DEFAULT 1, \
				timeStamp DATETIME DEFAULT CURRENT_TIMESTAMP, \
				FOREIGN KEY(Uid) REFERENCES lime_LocalUsers(Uid) ON UPDATE CASCADE ON DELETE CASCADE);";
		}
		break;
		case 0x000100:
		{
			// CREATE OR IGNORE TABLE db_module_version
			sql<<"CREATE TABLE IF NOT EXISTS db_module_version("
			"name VARCHAR(16) PRIMARY KEY,"
			"version UNSIGNED INTEGER NOT NULL"
			")";
			sql<<"INSERT INTO db_module_version(name,version) VALUES('lime',:DbVersion)", soci::use(version);
			sql<<"CREATE TABLE DR_sessions( \
				Did INTEGER NOT NULL DEFAULT 0, \
				Uid INTEGER NOT NULL DEFAULT 0, \
				sessionId INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, \
				Ns UNSIGNED INTEGER NOT NULL, \
				Nr UNSIGNED INTEGER NOT NULL, \
				PN UNSIGNED INTEGER NOT NULL, \
				DHr BLOB NOT NULL, \
				DHs BLOB NOT NULL, \
				RK BLOB NOT NULL, \
				CKs BLOB NOT NULL, \
				CKr BLOB NOT NULL, \
				AD BLOB NOT NULL, \
				Status INTEGER NOT NULL DEFAULT 1, \
				timeStamp DATETIME DEFAULT CURRENT_TIMESTAMP, \
				X3DHInit BLOB DEFAULT NULL, \
				FOREIGN KEY(Did) REFERENCES lime_PeerDevices(Did) ON UPDATE CASCADE ON DELETE CASCADE, \
				FOREIGN KEY(Uid) REFERENCES lime_LocalUsers(Uid) ON UPDATE CASCADE ON DELETE CASCADE);";
			sql<<"CREATE TABLE DR_MSk_DHr( \
				DHid INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, \
				sessionId INTEGER NOT NULL DEFAULT 0, \
				DHr BLOB NOT NULL, \
				received UNSIGNED INTEGER NOT NULL DEFAULT 0, \
				FOREIGN KEY(sessionId) REFERENCES DR_sessions(sessionId) ON UPDATE CASCADE ON DELETE CASCADE);";
			sql<<"CREATE TABLE DR_MSk_MK( \
				DHid INTEGER NOT NULL, \
				Nr INTEGER NOT NULL, \
				MK BLOB NOT NULL, \
				PRIMARY KEY( DHid , Nr ), \
				FOREIGN KEY(DHid) REFERENCES DR_MSk_DHr(DHid) ON UPDATE CASCADE ON DELETE CASCADE);";
			sql<<"CREATE TABLE lime_LocalUsers( \
				Uid INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, \
				UserId TEXT NOT NULL, \
				Ik BLOB NOT NULL, \
				server TEXT NOT NULL, \
				curveId INTEGER NOT NULL DEFAULT 0, \
				updateTs DATETIME DEFAULT CURRENT_TIMESTAMP);";
			sql<<"CREATE TABLE lime_PeerDevices( \
				Did INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, \
				DeviceId TEXT NOT NULL, \
				Ik BLOB NOT NULL, \
				Status UNSIGNED INTEGER DEFAULT 0);";
			sql<<"CREATE TABLE X3DH_SPK( \
				SPKid UNSIGNED INTEGER PRIMARY KEY NOT NULL, \
				SPK BLOB NOT NULL, \
				timeStamp DATETIME DEFAULT CURRENT_TIMESTAMP, \
				Status INTEGER NOT NULL DEFAULT 1, \
				Uid INTEGER NOT NULL, \
				FOREIGN KEY(Uid) REFERENCES lime_LocalUsers(Uid) ON UPDATE CASCADE ON DELETE CASCADE);";
			sql<<"CREATE TABLE X3DH_OPK( \
				OPKid UNSIGNED INTEGER PRIMARY KEY NOT NULL, \
				OPK BLOB NOT NULL, \
				Uid INTEGER NOT NULL, \
				Status INTEGER NOT NULL DEFAULT 1, \
				timeStamp DATETIME DEFAULT CURRENT_TIMESTAMP, \
				FOREIGN KEY(Uid) REFERENCES lime_LocalUsers(Uid) ON UPDATE CASCADE ON DELETE CASCADE);";
		}
			break;
		case 0x000200:
		{
			// CREATE OR IGNORE TABLE db_module_version
			sql<<"CREATE TABLE IF NOT EXISTS db_module_version("
			"name VARCHAR(16) PRIMARY KEY,"
			"version UNSIGNED INTEGER NOT NULL"
			")";
			sql<<"INSERT INTO db_module_version(name,version) VALUES('lime',:DbVersion)", soci::use(version);
			sql<<"CREATE TABLE DR_sessions( \
				Did INTEGER NOT NULL DEFAULT 0, \
				Uid INTEGER NOT NULL DEFAULT 0, \
				sessionId INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, \
				Ns UNSIGNED INTEGER NOT NULL, \
				Nr UNSIGNED INTEGER NOT NULL, \
				PN UNSIGNED INTEGER NOT NULL, \
				DHr BLOB NOT NULL, \
				DHrStatus INTEGER NOT NULL DEFAULT 0, \
				DHs BLOB NOT NULL, \
				RK BLOB NOT NULL, \
				CKs BLOB NOT NULL, \
				CKr BLOB NOT NULL, \
				AD BLOB NOT NULL, \
				Status INTEGER NOT NULL DEFAULT 1, \
				timeStamp DATETIME DEFAULT CURRENT_TIMESTAMP, \
				X3DHInit BLOB DEFAULT NULL, \
				FOREIGN KEY(Did) REFERENCES lime_PeerDevices(Did) ON UPDATE CASCADE ON DELETE CASCADE, \
				FOREIGN KEY(Uid) REFERENCES lime_LocalUsers(Uid) ON UPDATE CASCADE ON DELETE CASCADE);";

			sql<<"CREATE TABLE DR_MSk_DHr( \
				DHid INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, \
				sessionId INTEGER NOT NULL DEFAULT 0, \
				DHr BLOB NOT NULL, \
				received UNSIGNED INTEGER NOT NULL DEFAULT 0, \
				FOREIGN KEY(sessionId) REFERENCES DR_sessions(sessionId) ON UPDATE CASCADE ON DELETE CASCADE);";

			sql<<"CREATE TABLE DR_MSk_MK( \
				DHid INTEGER NOT NULL, \
				Nr INTEGER NOT NULL, \
				MK BLOB NOT NULL, \
				PRIMARY KEY( DHid , Nr ), \
				FOREIGN KEY(DHid) REFERENCES DR_MSk_DHr(DHid) ON UPDATE CASCADE ON DELETE CASCADE);";

			sql<<"CREATE TABLE lime_LocalUsers( \
				Uid INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, \
				UserId TEXT NOT NULL, \
				Ik BLOB NOT NULL, \
				server TEXT NOT NULL, \
				curveId INTEGER NOT NULL DEFAULT 0, \
				updateTs DATETIME DEFAULT CURRENT_TIMESTAMP);";

			sql<<"CREATE TABLE lime_PeerDevices( \
				Did INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, \
				DeviceId TEXT NOT NULL, \
				Ik BLOB NOT NULL, \
				Status UNSIGNED INTEGER DEFAULT 0);";

			sql<<"CREATE TABLE X3DH_SPK( \
				SPKid UNSIGNED INTEGER PRIMARY KEY NOT NULL, \
				SPK BLOB NOT NULL, \
				timeStamp DATETIME DEFAULT CURRENT_TIMESTAMP, \
				Status INTEGER NOT NULL DEFAULT 1, \
				Uid INTEGER NOT NULL, \
				FOREIGN KEY(Uid) REFERENCES lime_LocalUsers(Uid) ON UPDATE CASCADE ON DELETE CASCADE);";

			sql<<"CREATE TABLE X3DH_OPK( \
				OPKid UNSIGNED INTEGER PRIMARY KEY NOT NULL, \
				OPK BLOB NOT NULL, \
				Uid INTEGER NOT NULL, \
				Status INTEGER NOT NULL DEFAULT 1, \
				timeStamp DATETIME DEFAULT CURRENT_TIMESTAMP, \
				FOREIGN KEY(Uid) REFERENCES lime_LocalUsers(Uid) ON UPDATE CASCADE ON DELETE CASCADE);";
		}
			break;
	}

	// Insert a dummy row in the table modified by the migration as some operation are permitted over empty tables but not ones with data
	sql<<"INSERT INTO lime_LocalUsers(UserId, Ik, server, curveId) VALUES ('sip:notauser', '0x1234556', 'http://notalimeserver.com', 2);";
	sql<<"INSERT INTO lime_PeerDevices(DeviceId, Ik, Status) VALUES ('sip:notausertoo', '0x6543210', 1);";
	sql<<"INSERT INTO DR_sessions(Did, Uid, Ns, Nr, PN, DHr, DHs, RK, CKs, CKr, AD, Status) VALUES (1, 1, 0, 0, 0, '0x123', '0x456', '0x789', '0xabc', '0xdef', 'AssociatedData', 1);";

	tr.commit(); // commit all the previous queries
	sql.close();
}

static void lime_db_migration() {
	// migrate from version 0x000001 to 0x000300
	std::string dbFilename("lime_db_migration-v000001.sqlite3");
	remove(dbFilename.data());
	soci::session	sql;
	try{
		// Write a db version 000001
		write_db_version(0x000001, dbFilename);
		// Check we have a version 0x000001 DB
		sql.open("sqlite3", dbFilename);
		int userVersion=-1;
		sql<<"SELECT version FROM db_module_version WHERE name='lime'", soci::into(userVersion);
		BC_ASSERT_EQUAL(userVersion, 0x000001, int, "%d");
		// Version 0x000100 of db added a Timestamp
		int haveTs=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('lime_LocalUsers') WHERE name='updateTs'", soci::into(haveTs);
		BC_ASSERT_EQUAL(haveTs, 0, int, "%d");
		// Version 0x000200 of db an integer defaulted to 0 in the DR_sessions table
		int haveDHrStatus=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('DR_sessions') WHERE name='DHrStatus'", soci::into(haveDHrStatus);
		BC_ASSERT_EQUAL(haveDHrStatus, 0, int, "%d");
		// Version 0x000300 of db an integer defaulted to 0 table and an integer default to 1 in the lime_PeerDevices
		int haveCurveId=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('lime_PeerDevices') WHERE name='curveId'", soci::into(haveCurveId);
		BC_ASSERT_EQUAL(haveCurveId, 0, int, "%d");
		int haveActive=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('lime_PeerDevices') WHERE name='Active'", soci::into(haveActive);
		BC_ASSERT_EQUAL(haveActive, 0, int, "%d");
		sql.close();
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("Can't create test version 1 DB");
		return;
	}

	// Open a manager giving the same DB, it shall migrate the structure to version 0x000300
	try  {
		// create Manager
		std::unique_ptr<LimeManager> manager = std::make_unique<LimeManager>(dbFilename, X3DHServerPost);
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("Can't open manager to perform DB migration");
		return;
	}

	try  {
		sql.open("sqlite3", dbFilename);
		int userVersion=-1;
		sql<<"SELECT version FROM db_module_version WHERE name='lime'", soci::into(userVersion);
		BC_ASSERT_EQUAL(userVersion, 0x300, int, "%d");
		// Version 0x000100 of db added a Timestamp
		int haveTs=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('lime_LocalUsers') WHERE name='updateTs'", soci::into(haveTs);
		BC_ASSERT_EQUAL(haveTs, 1, int, "%d");
		// Version 0x000200 of db an integer defaulted to 0 in the DR_sessions table
		int haveDHrStatus=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('DR_sessions') WHERE name='DHrStatus'", soci::into(haveDHrStatus);
		BC_ASSERT_EQUAL(haveDHrStatus, 1, int, "%d");
		// Version 0x000300 of db an integer defaulted to 0 table and an integer default to 1 in the lime_PeerDevices
		int haveCurveId=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('lime_PeerDevices') WHERE name='curveId'", soci::into(haveCurveId);
		BC_ASSERT_EQUAL(haveCurveId, 1, int, "%d");
		if (haveCurveId == 1) {
			int curveId = 0;
			// Check the newly created curveId is set to 2 (copy from the lime_localUsers table)
			sql<<"SELECT curveId FROM lime_PeerDevices LIMIT 1", soci::into(curveId);
			BC_ASSERT_EQUAL(curveId, 2, int, "%d");
		}
		int haveActive=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('lime_PeerDevices') WHERE name='Active'", soci::into(haveActive);
		BC_ASSERT_EQUAL(haveActive, 1, int, "%d");
		if (haveActive == 1) {
			int active = 0;
			// Check the newly created Active is defaulted to 1
			sql<<"SELECT Active FROM lime_PeerDevices LIMIT 1", soci::into(active);
			BC_ASSERT_EQUAL(active, 1, int, "%d");
		}
		sql.close();
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("Can't check DB migration done");
		return;
	}
	if (cleanDatabase) {
		remove(dbFilename.data());
	}

	// migrate from version 0x000100 to 0x000300
	dbFilename = std::string("lime_db_migration-v000100.sqlite3");
	remove(dbFilename.data());
	try{
		// Write a db version 000100
		write_db_version(0x000100, dbFilename);
		// Check we have a version 0x000100 DB
		sql.open("sqlite3", dbFilename);
		int userVersion=-1;
		sql<<"SELECT version FROM db_module_version WHERE name='lime'", soci::into(userVersion);
		BC_ASSERT_EQUAL(userVersion, 0x000100, int, "%d");
		// Version 0x000100 of db added a Timestamp
		int haveTs=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('lime_LocalUsers') WHERE name='updateTs'", soci::into(haveTs);
		BC_ASSERT_EQUAL(haveTs, 1, int, "%d");
		// Version 0x000200 of db an integer defaulted to 0 in the DR_sessions table
		int haveDHrStatus=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('DR_sessions') WHERE name='DHrStatus'", soci::into(haveDHrStatus);
		BC_ASSERT_EQUAL(haveDHrStatus, 0, int, "%d");
		// Version 0x000300 of db an integer defaulted to 0 table and an integer default to 1 in the lime_PeerDevices
		int haveCurveId=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('lime_PeerDevices') WHERE name='curveId'", soci::into(haveCurveId);
		BC_ASSERT_EQUAL(haveCurveId, 0, int, "%d");
		int haveActive=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('lime_PeerDevices') WHERE name='Active'", soci::into(haveActive);
		BC_ASSERT_EQUAL(haveActive, 0, int, "%d");
		sql.close();
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("Can't create test version 1 DB");
		return;
	}

	// Open a manager giving the same DB, it shall migrate the structure to version 0x000300
	try  {
		// create Manager
		std::unique_ptr<LimeManager> manager = std::make_unique<LimeManager>(dbFilename, X3DHServerPost);
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("Can't open manager to perform DB migration");
		return;
	}

	try  {
		sql.open("sqlite3", dbFilename);
		int userVersion=-1;
		sql<<"SELECT version FROM db_module_version WHERE name='lime'", soci::into(userVersion);
		BC_ASSERT_EQUAL(userVersion, 0x300, int, "%d");
		// Version 0x000100 of db added a Timestamp
		int haveTs=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('lime_LocalUsers') WHERE name='updateTs'", soci::into(haveTs);
		BC_ASSERT_EQUAL(haveTs, 1, int, "%d");
		// Version 0x000200 of db an integer defaulted to 0 in the DR_sessions table
		int haveDHrStatus=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('DR_sessions') WHERE name='DHrStatus'", soci::into(haveDHrStatus);
		BC_ASSERT_EQUAL(haveDHrStatus, 1, int, "%d");
		// Version 0x000300 of db an integer defaulted to 0 table and an integer default to 1 in the lime_PeerDevices
		int haveCurveId=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('lime_PeerDevices') WHERE name='curveId'", soci::into(haveCurveId);
		BC_ASSERT_EQUAL(haveCurveId, 1, int, "%d");
		if (haveCurveId == 1) {
			int curveId = 0;
			// Check the newly created curveId is set to 2 (copy from the lime_localUsers table)
			sql<<"SELECT curveId FROM lime_PeerDevices LIMIT 1", soci::into(curveId);
			BC_ASSERT_EQUAL(curveId, 2, int, "%d");
		}
		int haveActive=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('lime_PeerDevices') WHERE name='Active'", soci::into(haveActive);
		BC_ASSERT_EQUAL(haveActive, 1, int, "%d");
		if (haveActive == 1) {
			int active = 0;
			// Check the newly created Active is defaulted to 1
			sql<<"SELECT Active FROM lime_PeerDevices LIMIT 1", soci::into(active);
			BC_ASSERT_EQUAL(active, 1, int, "%d");
		}
		sql.close();
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("Can't check DB migration done");
		return;
	}
	if (cleanDatabase) {
		remove(dbFilename.data());
	}

	// migrate from version 0x000200 to 0x000300
	dbFilename = std::string("lime_db_migration-v000200.sqlite3");
	remove(dbFilename.data());
	try{
		// Write a db version 000200
		write_db_version(0x000200, dbFilename);
		// Check we have a version 0x000200 DB
		sql.open("sqlite3", dbFilename);
		int userVersion=-1;
		sql<<"SELECT version FROM db_module_version WHERE name='lime'", soci::into(userVersion);
		BC_ASSERT_EQUAL(userVersion, 0x000200, int, "%d");
		// Version 0x000100 of db added a Timestamp
		int haveTs=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('lime_LocalUsers') WHERE name='updateTs'", soci::into(haveTs);
		BC_ASSERT_EQUAL(haveTs, 1, int, "%d");
		// Version 0x000200 of db an integer defaulted to 0 in the DR_sessions table
		int haveDHrStatus=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('DR_sessions') WHERE name='DHrStatus'", soci::into(haveDHrStatus);
		BC_ASSERT_EQUAL(haveDHrStatus, 1, int, "%d");
		// Version 0x000300 of db an integer defaulted to 0 table and an integer default to 1 in the lime_PeerDevices
		int haveCurveId=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('lime_PeerDevices') WHERE name='curveId'", soci::into(haveCurveId);
		BC_ASSERT_EQUAL(haveCurveId, 0, int, "%d");
		int haveActive=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('lime_PeerDevices') WHERE name='Active'", soci::into(haveActive);
		BC_ASSERT_EQUAL(haveActive, 0, int, "%d");
		sql.close();
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("Can't create test version 1 DB");
		return;
	}

	// Open a manager giving the same DB, it shall migrate the structure to version 0x000300
	try  {
		// create Manager
		std::unique_ptr<LimeManager> manager = std::make_unique<LimeManager>(dbFilename, X3DHServerPost);
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("Can't open manager to perform DB migration");
		return;
	}

	try  {
		sql.open("sqlite3", dbFilename);
		int userVersion=-1;
		sql<<"SELECT version FROM db_module_version WHERE name='lime'", soci::into(userVersion);
		BC_ASSERT_EQUAL(userVersion, 0x300, int, "%d");
		// Version 0x000100 of db added a Timestamp
		int haveTs=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('lime_LocalUsers') WHERE name='updateTs'", soci::into(haveTs);
		BC_ASSERT_EQUAL(haveTs, 1, int, "%d");
		// Version 0x000200 of db an integer defaulted to 0 in the DR_sessions table
		int haveDHrStatus=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('DR_sessions') WHERE name='DHrStatus'", soci::into(haveDHrStatus);
		BC_ASSERT_EQUAL(haveDHrStatus, 1, int, "%d");
		// Version 0x000300 of db an integer defaulted to 0 table and an integer default to 1 in the lime_PeerDevices
		int haveCurveId=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('lime_PeerDevices') WHERE name='curveId'", soci::into(haveCurveId);
		BC_ASSERT_EQUAL(haveCurveId, 1, int, "%d");
		if (haveCurveId == 1) {
			int curveId = 0;
			// Check the newly created curveId is set to 2 (copy from the lime_localUsers table)
			sql<<"SELECT curveId FROM lime_PeerDevices LIMIT 1", soci::into(curveId);
			BC_ASSERT_EQUAL(curveId, 2, int, "%d");
		}
		int haveActive=0;
		sql<<"SELECT COUNT(*) FROM pragma_table_info('lime_PeerDevices') WHERE name='Active'", soci::into(haveActive);
		BC_ASSERT_EQUAL(haveActive, 1, int, "%d");
		if (haveActive == 1) {
			int active = 0;
			// Check the newly created Active is defaulted to 1
			sql<<"SELECT Active FROM lime_PeerDevices LIMIT 1", soci::into(active);
			BC_ASSERT_EQUAL(active, 1, int, "%d");
		}
		sql.close();
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("Can't check DB migration done");
		return;
	}

	if (cleanDatabase) {
		remove(dbFilename.data());
	}
}

/**
 * Scenario:
 * - Create a user alice
 * - Create Bob with two devices and encrypt to alice
 * - update so localStorage can notice two keys are not on server anymore and upload more keys(check it worked)
 * - simulate a move forward in time so the OPks missing from server shall be deleted from base
 * - decrypt the first message, it shall work
 * - update
 * - decrypt the second message, it shall fail
 */
static void lime_update_OPk_test(const lime::CurveId curve) {
	const std::string dbBaseFilename{"lime_update_OPk"};
	// create DB
	std::string dbFilenameAlice{dbBaseFilename};
	std::string dbFilenameBob{dbBaseFilename};
	dbFilenameAlice.append(".alice.").append(CurveId2String(curve)).append(".sqlite3");
	dbFilenameBob.append(".bob.").append(CurveId2String(curve)).append(".sqlite3");

	remove(dbFilenameAlice.data()); // delete the database file if already exists
	remove(dbFilenameBob.data()); // delete the database file if already exists

	lime_tester::events_counters_t counters={};
	int expected_success=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};
	try {
		std::vector<lime::CurveId> algos{curve};
		// create Manager and device for alice
		auto aliceManager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);
		auto aliceDeviceId = lime_tester::makeRandomDeviceName("alice.d1.");
		aliceManager->create_user(*aliceDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));

		// check we have the expected count of OPk in base : the initial batch
		BC_ASSERT_EQUAL((int)lime_tester::get_OPks(dbFilenameAlice, *aliceDeviceId, curve), lime_tester::OPkInitialBatchSize, int, "%d");

		// call the update, set the serverLimit to initialBatch size and upload an other initial batch if needed
		// As all the keys are still on server, it shall have no effect, check it then
		aliceManager->update(*aliceDeviceId, algos, callback, lime_tester::OPkInitialBatchSize, lime_tester::OPkInitialBatchSize);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));
		BC_ASSERT_EQUAL((int)lime_tester::get_OPks(dbFilenameAlice, *aliceDeviceId, curve), lime_tester::OPkInitialBatchSize, int, "%d");

		// We will create a bob device and encrypt for each new epoch
		std::vector<std::unique_ptr<LimeManager>> bobManagers{};
		std::vector<std::shared_ptr<std::string>> bobDeviceIds{};
		std::vector<std::shared_ptr<lime::EncryptionContext>> encryptionContexts{};

		size_t patternIndex = 0;
		// create two devices for bob and encrypt to alice
		for (auto i=0; i<2; i++) {
			bobManagers.push_back(make_unique<LimeManager>(dbFilenameBob, X3DHServerPost));
			bobDeviceIds.push_back(lime_tester::makeRandomDeviceName("bob.d"));
			bobManagers.back()->create_user(*(bobDeviceIds.back()), algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));

			encryptionContexts.push_back(make_shared<EncryptionContext>("alice", lime_tester::messages_pattern[patternIndex]));
			encryptionContexts.back()->addRecipient(*aliceDeviceId);
			bobManagers.back()->encrypt(*(bobDeviceIds.back()), algos, encryptionContexts.back(), callback);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));

			patternIndex++;
			patternIndex %= lime_tester::messages_pattern.size();
		}

		// check we have the expected count of OPk in base : we shall still have the initial batch
		BC_ASSERT_EQUAL((int)lime_tester::get_OPks(dbFilenameAlice, *aliceDeviceId, curve), lime_tester::OPkInitialBatchSize, int, "%d");

		// call the update, set the serverLimit to initialBatch size and upload an other initial batch if needed
		// As some keys were removed from server this time we shall generate and upload a new batch
		lime_tester::forwardTime(dbFilenameAlice, 2); // Forward time by 2 days so the update actually do something
		aliceManager->update(*aliceDeviceId, algos, callback, lime_tester::OPkInitialBatchSize, lime_tester::OPkInitialBatchSize);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));
		// we uploaded a new batch but no key were removed from localStorage so we now have 2*batch size keys
		BC_ASSERT_EQUAL((int)lime_tester::get_OPks(dbFilenameAlice, *aliceDeviceId, curve), 2*lime_tester::OPkInitialBatchSize, int, "%d");

		// forward time by OPK_limboTime_days
		aliceManager=nullptr; // destroy manager before modifying DB
		lime_tester::forwardTime(dbFilenameAlice, lime::settings::OPk_limboTime_days+1);
		aliceManager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);

		// check nothing has changed on our local OPk count
		BC_ASSERT_EQUAL((int)lime_tester::get_OPks(dbFilenameAlice, *aliceDeviceId, curve), 2*lime_tester::OPkInitialBatchSize, int, "%d");

		// decrypt Bob first message(he is then an unknown device)
		std::vector<uint8_t> receivedMessage{};
		BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDeviceId, "alice", *(bobDeviceIds[0]), encryptionContexts[0]->m_recipients[0].DRmessage, encryptionContexts[0]->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::unknown);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[0]);

		// check we have one less key
		BC_ASSERT_EQUAL((int)lime_tester::get_OPks(dbFilenameAlice, *aliceDeviceId, curve), 2*lime_tester::OPkInitialBatchSize - 1, int, "%d");

		// call the update, set the serverLimit to 0, we don't want to upload more keys, but too old unused local OPk dispatched by server long ago shall be deleted
		aliceManager->update(*aliceDeviceId, algos, callback, 0, 0);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));

		// check the local OPk missing on server for a long time has been deleted
		BC_ASSERT_EQUAL((int)lime_tester::get_OPks(dbFilenameAlice, *aliceDeviceId, curve), 2*lime_tester::OPkInitialBatchSize - 2, int, "%d");

		// try to decrypt Bob's second message, it shall fail as we got rid of the OPk
		receivedMessage.clear();
		BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDeviceId, "alice", *(bobDeviceIds[1]), encryptionContexts[1]->m_recipients[0].DRmessage, encryptionContexts[1]->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::fail);

		if (cleanDatabase) {
			auto i=0;
			for (auto &bobManager : bobManagers) {
					bobManager->delete_user(DeviceId(*(bobDeviceIds[i]), curve), callback);
				i++;
			}
			aliceManager->delete_user(DeviceId(*aliceDeviceId, curve), callback);
			expected_success += 1+(int)bobManagers.size();
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success,lime_tester::wait_for_timeout));
			remove(dbFilenameAlice.data());
			remove(dbFilenameBob.data());
		}
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}
}

static void lime_update_OPk() {
#ifdef EC25519_ENABLED
	lime_update_OPk_test(lime::CurveId::c25519);
#endif
#ifdef EC448_ENABLED
	lime_update_OPk_test(lime::CurveId::c448);
#endif
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	lime_update_OPk_test(lime::CurveId::c25519k512);

	lime_update_OPk_test(lime::CurveId::c25519mlk512);
#endif
#ifdef EC448_ENABLED
	lime_update_OPk_test(lime::CurveId::c448mlk1024);
#endif
#endif
}

/**
 * Scenario:
 * - Create a user alice
 * - simulate a move forward in time and update
 * - check we changed SPk and still got the old one
 * - create a device for Bob and encrypt a message for alice fetching a new key bundle(check Bob uses the current SPk set as active)
 * - repeat until we reach the epoch in which SPk shall be deleted, check it is the case
 * - decrypt all Bob message, it shall be Ok for all except the one encrypted with a bundle based on the deleted SPk where decrypt shall fail
 */
static void lime_update_SPk_test(const lime::CurveId curve) {
	const std::string dbBaseFilename{"lime_update_SPk"};
	// create DB
	std::string dbFilenameAlice{dbBaseFilename};
	std::string dbFilenameBob{dbBaseFilename};
	dbFilenameAlice.append(".alice.").append(CurveId2String(curve)).append(".sqlite3");
	dbFilenameBob.append(".bob.").append(CurveId2String(curve)).append(".sqlite3");

	remove(dbFilenameAlice.data()); // delete the database file if already exists
	remove(dbFilenameBob.data()); // delete the database file if already exists

	lime_tester::events_counters_t counters={};
	int expected_success=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};
	try {
		std::vector<lime::CurveId> algos{curve};
		// starting epoch nad number of SPk keys in localStorage
		unsigned int epoch=0;
		auto SPkExpectedCount=1;
		size_t patternIndex = 0;

		// create Manager and device for alice
		auto aliceManager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);
		auto aliceDeviceId = lime_tester::makeRandomDeviceName("alice.d1.");
		aliceManager->create_user(*aliceDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));
		size_t SPkCount=0;
		uint32_t activeSPkId=0;
		BC_ASSERT_TRUE(lime_tester::get_SPks(dbFilenameAlice, *aliceDeviceId, curve, SPkCount, activeSPkId));
		BC_ASSERT_EQUAL((int)SPkCount, (int)SPkExpectedCount, int, "%d");

		// We will create a bob device and encrypt for each new epoch
		std::vector<std::unique_ptr<LimeManager>> bobManagers{};
		std::vector<std::shared_ptr<std::string>> bobDeviceIds{};
		std::vector<std::shared_ptr<lime::EncryptionContext>> encryptionContexts{};

		// create a device for bob and encrypt to alice
		bobManagers.push_back(make_unique<LimeManager>(dbFilenameBob, X3DHServerPost));
		bobDeviceIds.push_back(lime_tester::makeRandomDeviceName("bob.d"));
		bobManagers.back()->create_user(*(bobDeviceIds.back()), algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));

		encryptionContexts.push_back(make_shared<EncryptionContext>("alice", lime_tester::messages_pattern[patternIndex]));
		encryptionContexts.back()->addRecipient(*aliceDeviceId);
		bobManagers.back()->encrypt(*(bobDeviceIds.back()), algos, encryptionContexts.back(), callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));

		// extract SPKid from bob's message, and check it matches the current one from Alice DB
		uint32_t SPkIdMessage=0;
		BC_ASSERT_TRUE(lime_tester::DR_message_extractX3DHInit_SPkId(encryptionContexts.back()->m_recipients[0].DRmessage, SPkIdMessage));
		BC_ASSERT_EQUAL(SPkIdMessage, activeSPkId, uint32_t, "%x");

		patternIndex++;
		patternIndex %= lime_tester::messages_pattern.size();

		// steping by SPK_lifeTime_days go ahead in time and check the update is performed correctly
		while (epoch<=lime::settings::SPK_limboTime_days) {
			// forward time by SPK_lifeTime_days
			aliceManager=nullptr; // destroy manager before modifying DB
			lime_tester::forwardTime(dbFilenameAlice, lime::settings::SPK_lifeTime_days);
			epoch+=lime::settings::SPK_lifeTime_days;
			aliceManager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);

			// call the update, it shall create and upload a new SPk but keep the old ones
			aliceManager->update(*aliceDeviceId, algos, callback, 0, lime_tester::OPkInitialBatchSize);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));
			SPkExpectedCount++;

			// check we have the correct count of keys in local
			SPkCount=0;
			activeSPkId=0;
			BC_ASSERT_TRUE(lime_tester::get_SPks(dbFilenameAlice, *aliceDeviceId, curve, SPkCount, activeSPkId));
			BC_ASSERT_EQUAL((int)SPkCount, (int)SPkExpectedCount, int, "%d");

			// create a device for bob and use it to encrypt
			bobManagers.push_back(make_unique<LimeManager>(dbFilenameBob, X3DHServerPost));
			bobDeviceIds.push_back(lime_tester::makeRandomDeviceName("bob.d"));
			bobManagers.back()->create_user(*(bobDeviceIds.back()), algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));

			encryptionContexts.push_back(make_shared<EncryptionContext>("alice", lime_tester::messages_pattern[patternIndex]));
			encryptionContexts.back()->addRecipient(*aliceDeviceId);
			bobManagers.back()->encrypt(*(bobDeviceIds.back()), algos, encryptionContexts.back(), callback);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));

			// extract SPKid from bob's message, and check it matches the current one from Alice DB
			SPkIdMessage=0;
			BC_ASSERT_TRUE(lime_tester::DR_message_extractX3DHInit_SPkId(encryptionContexts.back()->m_recipients[0].DRmessage, SPkIdMessage));
			BC_ASSERT_EQUAL(SPkIdMessage, activeSPkId, uint32_t, "%x");

			patternIndex++;
			patternIndex %= lime_tester::messages_pattern.size();
		}

		// forward time once more by SPK_lifeTime_days, our first SPk shall now be out of limbo and ready to be deleted
		aliceManager=nullptr; // destroy manager before modifying DB
		lime_tester::forwardTime(dbFilenameAlice, lime::settings::SPK_lifeTime_days);
		epoch+=lime::settings::SPK_lifeTime_days;
		aliceManager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);

		// call the update, it shall create and upload a new SPk but keep the old ones and delete one
		aliceManager->update(*aliceDeviceId, algos, callback, 0, lime_tester::OPkInitialBatchSize);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));
		// there shall not be any rise in the number of SPk keys found in DB, check that
		SPkCount=0;
		activeSPkId=0;
		BC_ASSERT_TRUE(lime_tester::get_SPks(dbFilenameAlice, *aliceDeviceId, curve, SPkCount, activeSPkId));
		BC_ASSERT_EQUAL((int)SPkCount, (int)SPkExpectedCount, int, "%d");

		// Try to decrypt all message: the first message must fail to decrypt as we just deleted the SPk needed to create the session
		std::vector<uint8_t> receivedMessage{};
		BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDeviceId, "alice", *(bobDeviceIds[0]), encryptionContexts[0]->m_recipients[0].DRmessage, encryptionContexts[0]->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::fail);
		// other shall be Ok.
		for (size_t i=1; i<bobManagers.size(); i++) {
			receivedMessage.clear();
			BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDeviceId, "alice", *(bobDeviceIds[i]), encryptionContexts[i]->m_recipients[0].DRmessage, encryptionContexts[i]->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
			BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[i%lime_tester::messages_pattern.size()]);
		}

		if (cleanDatabase) {
			auto i=0;
			for (auto &bobManager : bobManagers) {
				bobManager->delete_user(DeviceId(*(bobDeviceIds[i]), curve), callback);
				i++;
			}
			aliceManager->delete_user(DeviceId(*aliceDeviceId, curve), callback);
			expected_success += 1+(int)bobManagers.size();
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success,lime_tester::wait_for_timeout));
			remove(dbFilenameAlice.data());
			remove(dbFilenameBob.data());
		}
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}
}

static void lime_update_SPk() {
#ifdef EC25519_ENABLED
	lime_update_SPk_test(lime::CurveId::c25519);
#endif
#ifdef EC448_ENABLED
	lime_update_SPk_test(lime::CurveId::c448);
#endif
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	lime_update_SPk_test(lime::CurveId::c25519k512);

	lime_update_SPk_test(lime::CurveId::c25519mlk512);
#endif
#ifdef EC448_ENABLED
	lime_update_SPk_test(lime::CurveId::c448mlk1024);
#endif
#endif
}

/**
 * Scenario:
 * - Establish a session between alice and bob
 * - Skip two messages and keep on exchanging post until maxMessagesReceivedAfterSkip
 * - Check we have two message key in localStorage
 * - decrypt a message
 * - Check we have one message key in localStorage
 * - call the update
 * - Check we have no more message key in storage
 * - try to decrypt the message, it shall fail
 */
static void lime_update_clean_MK_test(const lime::CurveId curve) {
	const std::string dbBaseFilename{"lime_update_clean_MK"};
	std::string dbFilenameAlice;
	std::shared_ptr<std::string> aliceDeviceId;
	std::unique_ptr<LimeManager> aliceManager;
	std::string dbFilenameBob;
	std::shared_ptr<std::string> bobDeviceId;
	std::unique_ptr<LimeManager> bobManager;

	lime_tester::events_counters_t counters={};
	int expected_success=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};
	try {
		std::vector<lime::CurveId> algos{curve};
		lime_session_establishment(algos, dbBaseFilename,
					dbFilenameAlice, aliceDeviceId, aliceManager,
					dbFilenameBob, bobDeviceId, bobManager);

		/* Alice encrypt 2 messages that are kept */
		auto aliceEncCtx1 = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[0]);
		aliceEncCtx1->addRecipient(*bobDeviceId);
		auto aliceEncCtx2 = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[1]);
		aliceEncCtx2->addRecipient(*bobDeviceId);
		aliceManager->encrypt(*aliceDeviceId, algos, aliceEncCtx1, callback);
		aliceManager->encrypt(*aliceDeviceId, algos, aliceEncCtx2, callback);
		expected_success+=2;
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, expected_success, lime_tester::wait_for_timeout));

		/* Alice and Bob exchange more than maxMessagesReceivedAfterSkip messages(do batches of 4 to be faster) */
		lime_exchange_messages(aliceDeviceId, aliceManager, bobDeviceId, bobManager, algos, lime::settings::maxMessagesReceivedAfterSkip/4+1, 4);

		/* Check that bob got 2 message key in local Storage */
		BC_ASSERT_EQUAL(lime_tester::get_StoredMessageKeyCount(dbFilenameBob, *bobDeviceId, *aliceDeviceId, curve), 2, unsigned int, "%d");

		/* decrypt held message 1, we know that device but no trust was ever established */
		std::vector<uint8_t> receivedMessage{};
		BC_ASSERT_TRUE(bobManager->decrypt(*bobDeviceId, "bob", *aliceDeviceId, aliceEncCtx1->m_recipients[0].DRmessage, aliceEncCtx1->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[0]);

		/* Check that bob got 1 message key in local Storage */
		BC_ASSERT_EQUAL(lime_tester::get_StoredMessageKeyCount(dbFilenameBob, *bobDeviceId, *aliceDeviceId, curve), 1, unsigned int, "%d");

		/* update belle-sip stack processing possible incoming messages from server */
		belle_sip_stack_sleep(bc_stack,0);

		/* call the update function, forwardtime by 2 days before or the update is skipped */
		lime_tester::forwardTime(dbFilenameBob, 2);
		bobManager->update(*bobDeviceId, algos, callback, 0, lime_tester::OPkInitialBatchSize);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success, lime_tester::wait_for_timeout));

		/* Check that bob got 0 message key in local Storage */
		BC_ASSERT_EQUAL(lime_tester::get_StoredMessageKeyCount(dbFilenameBob, *bobDeviceId, *aliceDeviceId, curve), 0, unsigned int, "%d");

		/* try to decrypt message 2, it shall fail */
		receivedMessage.clear();
		BC_ASSERT_TRUE(bobManager->decrypt(*bobDeviceId, "bob", *aliceDeviceId, aliceEncCtx2->m_recipients[0].DRmessage, aliceEncCtx2->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::fail);

		if (cleanDatabase) {
			aliceManager->delete_user(DeviceId(*aliceDeviceId, curve), callback);
			bobManager->delete_user(DeviceId(*bobDeviceId, curve), callback);
			expected_success +=2;
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success,lime_tester::wait_for_timeout));
			remove(dbFilenameAlice.data());
			remove(dbFilenameBob.data());
		}
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}
}

static void lime_update_clean_MK() {
#ifdef EC25519_ENABLED
	lime_update_clean_MK_test(lime::CurveId::c25519);
#endif
#ifdef EC448_ENABLED
	lime_update_clean_MK_test(lime::CurveId::c448);
#endif
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	lime_update_clean_MK_test(lime::CurveId::c25519k512);

	lime_update_clean_MK_test(lime::CurveId::c25519mlk512);
#endif
#ifdef EC448_ENABLED
	lime_update_clean_MK_test(lime::CurveId::c448mlk1024);
#endif
#endif
}

/**
 * Scenario:
 * - Create Alice and Bob users
 * - Backup local Alice DB.
 * - Delete Alice user (so it is not anymore on the server)
 * - restore local Alice DB and do an update, it shall republish the user on server(using new OPks)
 * - Bob encrypt a message to Alice, it shall work as the user has been republished
 * - Alice decrypt just to be sure we're good with the OPks
 */
static void lime_update_republish_test(const lime::CurveId curve) {
	const std::string dbBaseFilename{"lime_update_republish"};
	// create DB
	std::string dbFilenameAlice{dbBaseFilename};
	std::string dbFilenameAliceBackup{dbBaseFilename};
	dbFilenameAlice.append(".alice.").append(CurveId2String(curve)).append(".sqlite3");
	dbFilenameAliceBackup.append(".alice.backup.").append(CurveId2String(curve)).append(".sqlite3");
	std::string dbFilenameBob{dbBaseFilename};
	dbFilenameBob.append(".bob.").append(CurveId2String(curve)).append(".sqlite3");

	remove(dbFilenameAlice.data()); // delete the database file if already exists
	remove(dbFilenameAliceBackup.data()); // delete the database file if already exists
	remove(dbFilenameBob.data()); // delete the database file if already exists

	lime_tester::events_counters_t counters={};
	int expected_success=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};

	try {
		std::vector<lime::CurveId> algos{curve};
		// create Managers and devices for alice and Bob
		auto aliceManager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);
		auto aliceDeviceId = lime_tester::makeRandomDeviceName("alice.");
		aliceManager->create_user(*aliceDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		auto bobManager = make_unique<LimeManager>(dbFilenameBob, X3DHServerPost);
		auto bobDeviceId = lime_tester::makeRandomDeviceName("bob.");
		bobManager->create_user(*bobDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		expected_success += 2;
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, expected_success,lime_tester::wait_for_timeout));

		// Backup Alice Database
		std::ifstream  src(dbFilenameAlice, std::ios::binary);
		std::ofstream  dst(dbFilenameAliceBackup,   std::ios::binary);
		dst << src.rdbuf();
		src.close();
		dst.close();

		// Delete Alice user
		aliceManager->delete_user(DeviceId(*aliceDeviceId, curve), callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));
		// Start a new manager using the backuped local base, so the user is present in local but no more on remote
		aliceManager = nullptr;
		lime_tester::forwardTime(dbFilenameAliceBackup, 2); // forward time otherwise the update won't do anything
		aliceManager = make_unique<LimeManager>(dbFilenameAliceBackup, X3DHServerPost);
		// Update: that shall set all current OPk as dispatched, create a new batch of default creation size and republish the user on server
		aliceManager->update(*aliceDeviceId, algos, callback, lime_tester::OPkInitialBatchSize, lime_tester::OPkInitialBatchSize);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));
		// Bob encrypt a message to Alice, it will fetch keys from server
		auto enc = make_shared<EncryptionContext>("alice", lime_tester::messages_pattern[0]);
		enc->addRecipient(*aliceDeviceId);
		bobManager->encrypt(*bobDeviceId, algos, enc, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		// Alice decrypt
		std::vector<uint8_t> receivedMessage{};
		BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDeviceId, "alice", *bobDeviceId, enc->m_recipients[0].DRmessage, enc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::unknown);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[0]);

		if (cleanDatabase) {
			aliceManager->delete_user(DeviceId(*aliceDeviceId, curve), callback);
			bobManager->delete_user(DeviceId(*bobDeviceId, curve), callback);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success+2,lime_tester::wait_for_timeout));
			remove(dbFilenameAlice.data());
			remove(dbFilenameAliceBackup.data());
			remove(dbFilenameBob.data());
		}
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}
}


static void lime_update_republish() {
#ifdef EC25519_ENABLED
	lime_update_republish_test(lime::CurveId::c25519);
#endif
#ifdef EC448_ENABLED
	lime_update_republish_test(lime::CurveId::c448);
#endif
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	lime_update_republish_test(lime::CurveId::c25519k512);

	lime_update_republish_test(lime::CurveId::c25519mlk512);
#endif
#ifdef EC448_ENABLED
	lime_update_republish_test(lime::CurveId::c448mlk1024);
#endif
#endif
}
/** Scenario
 * - Create one device for alice
 * - Create OPk_batch_size devices for bob, they will all fetch a key and encrypt a messaage to alice, server shall not have anymore OPks
 * - Create another device for alice and send a message to bob, it shall get a non OPk bundle, check it is the case in the message sent to Alice
 * Alice decrypt all messages to complete the check
 *
 */
static void x3dh_without_OPk_test(const lime::CurveId curve, const std::string &dbBaseFilename, bool continuousSession=true) {
	// create DB
	std::string dbFilenameAlice{dbBaseFilename};
	dbFilenameAlice.append(".alice.").append(CurveId2String(curve)).append(".sqlite3");
	std::string dbFilenameBob{dbBaseFilename};
	dbFilenameBob.append(".bob.").append(CurveId2String(curve)).append(".sqlite3");

	remove(dbFilenameAlice.data()); // delete the database file if already exists
	remove(dbFilenameBob.data()); // delete the database file if already exists

	lime_tester::events_counters_t counters={};
	int expected_success=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};
	try {
		std::vector<lime::CurveId> algos{curve};
		// create Manager and device for alice
		auto aliceManager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);
		auto aliceDeviceId = lime_tester::makeRandomDeviceName("alice.d1.");
		auto aliceOPkInitialBatchSize = 3; // give it only 3 OPks as initial batch
		aliceManager->create_user(*aliceDeviceId, algos, lime_tester::test_x3dh_default_server, aliceOPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));

		for (auto i=0; i<aliceOPkInitialBatchSize+1; i++) {
			// Create manager and device for bob
			auto bobManager = make_unique<LimeManager>(dbFilenameBob, X3DHServerPost);
			auto bobDeviceId = lime_tester::makeRandomDeviceName("bob.d");
			bobManager->create_user(*bobDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));

			// encrypt a message to Alice
			auto messagePatternIndex = i % lime_tester::messages_pattern.size();
			auto enc = make_shared<EncryptionContext>("alice", lime_tester::messages_pattern[messagePatternIndex]);
			enc->addRecipient(*aliceDeviceId);
			bobManager->encrypt(*bobDeviceId, algos, enc, callback);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

			// alice decrypt
			std::vector<uint8_t> receivedMessage{};
			bool haveOPk=false;
			BC_ASSERT_TRUE(lime_tester::DR_message_holdsX3DHInit(enc->m_recipients[0].DRmessage, haveOPk));
			if (i<aliceOPkInitialBatchSize) { // the first aliceOPkInitialBatchSize messages must hold an X3DH init message with an OPk
				BC_ASSERT_TRUE(haveOPk);
			} else { // then the last message shall not convey OPK_id as none were available
				BC_ASSERT_FALSE(haveOPk);
			}
			BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDeviceId, "alice", *bobDeviceId, enc->m_recipients[0].DRmessage, enc->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
			BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[messagePatternIndex]);

			if (cleanDatabase) {
				bobManager->delete_user(DeviceId(*bobDeviceId, curve), callback);
				BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
			}

			// destroy and reload the Managers(tests everything is correctly saved/load from local Storage)
			if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}
		}

		// delete the users so the remote DB will be clean too
		if (cleanDatabase) {
			aliceManager->delete_user(DeviceId(*aliceDeviceId, curve), callback);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
			remove(dbFilenameAlice.data());
			remove(dbFilenameBob.data());
		}
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}
}
static void x3dh_without_OPk() {
#ifdef EC25519_ENABLED
	x3dh_without_OPk_test(lime::CurveId::c25519, "lime_x3dh_without_OPk");
	x3dh_without_OPk_test(lime::CurveId::c25519, "lime_x3dh_without_OPk_clean", false);
#endif
#ifdef EC448_ENABLED
	x3dh_without_OPk_test(lime::CurveId::c448, "lime_x3dh_without_OPk");
	x3dh_without_OPk_test(lime::CurveId::c448, "lime_x3dh_without_OPk_clean", false);
#endif
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	x3dh_without_OPk_test(lime::CurveId::c25519k512, "lime_x3dh_without_OPk");
	x3dh_without_OPk_test(lime::CurveId::c25519k512, "lime_x3dh_without_OPk_clean", false);

	x3dh_without_OPk_test(lime::CurveId::c25519mlk512, "lime_x3dh_without_OPk");
	x3dh_without_OPk_test(lime::CurveId::c25519mlk512, "lime_x3dh_without_OPk_clean", false);
#endif
#ifdef EC448_ENABLED
	x3dh_without_OPk_test(lime::CurveId::c448mlk1024, "lime_x3dh_without_OPk");
	x3dh_without_OPk_test(lime::CurveId::c448mlk1024, "lime_x3dh_without_OPk_clean", false);
#endif
#endif
}


/* Alice encrypt to bob, bob replies so session is fully established, then alice encrypt more than maxSendingChain message so we must start a new session with bob
 * - alice.d1 and bob.d1 exchange messages
 * - alice encrypt maxSendingChain messages, bob never reply, no real need to decryp them, just check they are not holding X3DH init message
 * - alice encrypt one more message, it shall trigger new session creation so message will hold an X3DH init, decrypt it just to check bob can hold the session change
 */
static void x3dh_sending_chain_limit_test(const lime::CurveId curve, const std::string &dbBaseFilename, bool continuousSession=true) {
	// create DB
	std::string dbFilenameAlice{dbBaseFilename};
	dbFilenameAlice.append(".alice.").append(CurveId2String(curve)).append(".sqlite3");
	std::string dbFilenameBob{dbBaseFilename};
	dbFilenameBob.append(".bob.").append(CurveId2String(curve)).append(".sqlite3");

	remove(dbFilenameAlice.data()); // delete the database file if already exists
	remove(dbFilenameBob.data()); // delete the database file if already exists

	lime_tester::events_counters_t counters={};
	int expected_success=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};
	try {
		std::vector<lime::CurveId> algos{curve};
		// create Manager
		auto aliceManager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);
		auto bobManager = make_unique<LimeManager>(dbFilenameBob, X3DHServerPost);

		// create Random devices names
		auto aliceDevice1 = lime_tester::makeRandomDeviceName("alice.d1.");
		auto bobDevice1 = lime_tester::makeRandomDeviceName("bob.d1.");

		// create users alice.d1 and bob.d1
		aliceManager->create_user(*aliceDevice1, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		bobManager->create_user(*bobDevice1, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		expected_success +=2; // we have two asynchronous operation on going
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, expected_success,lime_tester::wait_for_timeout));
		if (counters.operation_failed == 1) return; // skip the end of the test if we can't do this

		// destroy and reload the Managers(tests everything is correctly saved/load from local Storage)
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// alice.d1 encrypts a message for bob.d1, bob replies
		auto aliceEnc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[0]);
		aliceEnc->addRecipient(*bobDevice1);
		auto bobEnc = make_shared<lime::EncryptionContext>("alice", lime_tester::messages_pattern[1]);
		bobEnc->addRecipient(*aliceDevice1);

		// alice encrypt
		aliceManager->encrypt(*aliceDevice1, algos, aliceEnc, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// destroy and reload the Managers(tests everything is correctly saved/load from local Storage)
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// bob decrypt
		std::vector<uint8_t> receivedMessage{};
		BC_ASSERT_TRUE(lime_tester::DR_message_holdsX3DHInit(aliceEnc->m_recipients[0].DRmessage)); // new sessions created, they must convey X3DH init message and peer device is unknown
		BC_ASSERT_TRUE(bobManager->decrypt(*bobDevice1, "bob", *aliceDevice1, aliceEnc->m_recipients[0].DRmessage, aliceEnc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::unknown);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[0]);

		// destroy and reload the Managers(tests everything is correctly saved/load from local Storage)
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// bob encrypt
		bobManager->encrypt(*bobDevice1, algos, bobEnc, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// destroy and reload the Managers(tests everything is correctly saved/load from local Storage)
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// alice decrypt
		BC_ASSERT_FALSE(lime_tester::DR_message_holdsX3DHInit(bobEnc->m_recipients[0].DRmessage)); // Bob didn't initiate a new session created, so no X3DH init message
		BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDevice1, "alice", *bobDevice1, bobEnc->m_recipients[0].DRmessage, bobEnc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[1]);

		// Alice encrypts until the sending chain reachs  maxSendingChain messages to bob, none shall have the X3DH init
		auto i =0;
		while (lime_tester::DR_message_get_Ns(aliceEnc->m_recipients[0].DRmessage) < lime::settings::maxSendingChain - 1 ) {
			// alice encrypt
			aliceEnc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[i%lime_tester::messages_pattern.size()]);
			aliceEnc->addRecipient(*bobDevice1);
			aliceManager->encrypt(*aliceDevice1, algos, aliceEnc, callback);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

			// destroy and reload the Managers(tests everything is correctly saved/load from local Storage)
			if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

			BC_ASSERT_FALSE(lime_tester::DR_message_holdsX3DHInit(aliceEnc->m_recipients[0].DRmessage)); // it's an ongoing session, no X3DH init

			// bob decrypt, it's not really needed here but cannot really hurt, comment if the test is too slow
/*
			std::vector<uint8_t> receivedMessage{};
			BC_ASSERT_TRUE(bobManager->decrypt(*bobDevice1, "bob", *aliceDevice1, (*aliceRecipients)[0].DRmessage, *aliceCipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted);
			std::string receivedMessageString{receivedMessage.begin(), receivedMessage.end()};
			BC_ASSERT_TRUE(receivedMessageString == lime_tester::messages_pattern[i%lime_tester::messages_pattern.size()]);
*/
			i++;
		}

		// update belle-sip stack processing possible incoming messages from server
		belle_sip_stack_sleep(bc_stack,0);

		// destroy and reload the Managers(tests everything is correctly saved/load from local Storage)
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// alice encrypt, we are over the maximum number, so Alice shall fetch a new key on server and start a new session
		aliceEnc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[0]);
		aliceEnc->addRecipient(*bobDevice1);
		aliceManager->encrypt(*aliceDevice1, algos, aliceEnc, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// bob decrypt, it's not really needed here but...
		receivedMessage.clear();
		BC_ASSERT_TRUE(lime_tester::DR_message_holdsX3DHInit(aliceEnc->m_recipients[0].DRmessage)); // we started a new session: this is what we really want to check
		BC_ASSERT_TRUE(bobManager->decrypt(*bobDevice1, "bob", *aliceDevice1, aliceEnc->m_recipients[0].DRmessage, aliceEnc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[0]);

		// delete the users so the remote DB will be clean too
		if (cleanDatabase) {
			aliceManager->delete_user(DeviceId(*aliceDevice1, curve), callback);
			bobManager->delete_user(DeviceId(*bobDevice1, curve), callback);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success+2,lime_tester::wait_for_timeout));
			remove(dbFilenameAlice.data());
			remove(dbFilenameBob.data());
		}
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}
}
static void x3dh_sending_chain_limit() {
#ifdef EC25519_ENABLED
	x3dh_sending_chain_limit_test(lime::CurveId::c25519, "lime_x3dh_sending_chain_limit");
	x3dh_sending_chain_limit_test(lime::CurveId::c25519, "lime_x3dh_sending_chain_limit_clean", false);
#endif
#ifdef EC448_ENABLED
	x3dh_sending_chain_limit_test(lime::CurveId::c448, "lime_x3dh_sending_chain_limit");
	x3dh_sending_chain_limit_test(lime::CurveId::c448, "lime_x3dh_sending_chain_limit_clean", false);
#endif
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	x3dh_sending_chain_limit_test(lime::CurveId::c25519k512, "lime_x3dh_sending_chain_limit");
	x3dh_sending_chain_limit_test(lime::CurveId::c25519k512, "lime_x3dh_sending_chain_limit_clean", false);

	x3dh_sending_chain_limit_test(lime::CurveId::c25519mlk512, "lime_x3dh_sending_chain_limit");
	x3dh_sending_chain_limit_test(lime::CurveId::c25519mlk512, "lime_x3dh_sending_chain_limit_clean", false);
#endif
#ifdef EC448_ENABLED
	x3dh_sending_chain_limit_test(lime::CurveId::c448mlk1024, "lime_x3dh_sending_chain_limit");
	x3dh_sending_chain_limit_test(lime::CurveId::c448mlk1024, "lime_x3dh_sending_chain_limit_clean", false);
#endif
#endif

}

/*
 * Create multiple DR sessions between a pair of devices and check it will converge to one session being used but also that an old message to a stale session is decrypted correctly
 * - create users alice.d1 and bob.d1
 * - they both encrypt to peer (so each one fetch the peer key bundle from server)
 * - decrypt messages and check we have now 2 sessions in base and the session created by peer is the active one
 * - bob.d1 encrypt to Alice (it must use the alice created session, we have no easy way to check that at this point)
 * - alice.d decrypt, it will move its active session back to the one originally created, now devices are in sync on one session.
 */
static void x3dh_multiple_DRsessions_test(const lime::CurveId curve, const std::string &dbBaseFilename, bool continuousSession=true) {
	// create DB
	std::string dbFilenameAlice{dbBaseFilename};
	dbFilenameAlice.append(".alice.").append(CurveId2String(curve)).append(".sqlite3");
	std::string dbFilenameBob{dbBaseFilename};
	dbFilenameBob.append(".bob.").append(CurveId2String(curve)).append(".sqlite3");

	remove(dbFilenameAlice.data()); // delete the database file if already exists
	remove(dbFilenameBob.data()); // delete the database file if already exists

	lime_tester::events_counters_t counters={};
	int expected_success=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						LIME_LOGI<<"Lime operation success : "<<anythingToSay;
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};
	try {
		std::vector<lime::CurveId> algos{curve};
		// create Manager
		auto aliceManager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);
		auto bobManager = make_unique<LimeManager>(dbFilenameBob, X3DHServerPost);

		// create Random devices names
		auto aliceDevice1 = lime_tester::makeRandomDeviceName("alice.d1.");
		auto bobDevice1 = lime_tester::makeRandomDeviceName("bob.d1.");

		// create users alice.d1 and bob.d1
		aliceManager->create_user(*aliceDevice1, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		bobManager->create_user(*bobDevice1, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		expected_success +=2; // we have two asynchronous operation on going
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, expected_success,lime_tester::wait_for_timeout));
		if (counters.operation_failed == 1) return; // skip the end of the test if we can't do this

		// destroy and reload the Managers(tests everything is correctly saved/load from local Storage)
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// alice.d1 encrypts a message for bob.d1 and bob.d1 encrypts a message for alice.d1
		auto aliceEnc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[0]);
		aliceEnc->addRecipient(*bobDevice1);
		auto bobEnc = make_shared<lime::EncryptionContext>("alice", lime_tester::messages_pattern[1]);
		bobEnc->addRecipient(*aliceDevice1);
		aliceManager->encrypt(*aliceDevice1, algos, aliceEnc, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		bobManager->encrypt(*bobDevice1, algos, bobEnc, callback);
		expected_success += 1;
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success,lime_tester::wait_for_timeout));

		// DB are cleans, so we shall have only one session between pairs and it shall have Id 1, check it
		std::vector<long int> aliceSessionsId{};
		auto aliceActiveSessionId =  lime_tester::get_DRsessionsId(dbFilenameAlice, *aliceDevice1, *bobDevice1, aliceSessionsId);
		BC_ASSERT_EQUAL(aliceActiveSessionId, 1, long int, "%ld");
		BC_ASSERT_EQUAL((int)aliceSessionsId.size(), 1, int, "%d");

		std::vector<long int> bobSessionsId{};
		auto bobActiveSessionId =  lime_tester::get_DRsessionsId(dbFilenameBob, *bobDevice1, *aliceDevice1, bobSessionsId);
		BC_ASSERT_EQUAL(bobActiveSessionId, 1, long int, "%ld");
		BC_ASSERT_EQUAL((int)bobSessionsId.size(), 1, int, "%d");

		// destroy and reload the Managers(tests everything is correctly saved/load from local Storage)
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// both decrypt the messages, they have crossed on the network
		std::vector<uint8_t> receivedMessage{};
		BC_ASSERT_TRUE(lime_tester::DR_message_holdsX3DHInit(aliceEnc->m_recipients[0].DRmessage)); // new sessions created, they must convey X3DH init message
		BC_ASSERT_TRUE(bobManager->decrypt(*bobDevice1, "bob", *aliceDevice1, aliceEnc->m_recipients[0].DRmessage, aliceEnc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted); // but we know that device already
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[0]);

		BC_ASSERT_TRUE(lime_tester::DR_message_holdsX3DHInit(bobEnc->m_recipients[0].DRmessage)); // new sessions created, they must convey X3DH init message
		BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDevice1, "alice", *bobDevice1, bobEnc->m_recipients[0].DRmessage, bobEnc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted); // but we know that device already
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[1]);

		// Now DB shall holds 2 sessions each, and both shall have their active as session 2 as the active one must be the last one used for decrypt
		aliceSessionsId.clear();
		aliceActiveSessionId =  lime_tester::get_DRsessionsId(dbFilenameAlice, *aliceDevice1, *bobDevice1, aliceSessionsId);
		BC_ASSERT_EQUAL(aliceActiveSessionId, 2, long int, "%ld");
		BC_ASSERT_EQUAL((int)aliceSessionsId.size(), 2, int, "%d");

		bobSessionsId.clear();
		bobActiveSessionId =  lime_tester::get_DRsessionsId(dbFilenameBob, *bobDevice1, *aliceDevice1, bobSessionsId);
		BC_ASSERT_EQUAL(bobActiveSessionId, 2, long int, "%ld");
		BC_ASSERT_EQUAL((int)bobSessionsId.size(), 2, int, "%d");

		// destroy and reload the Managers(tests everything is correctly saved/load from local Storage)
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// bob.d1 encrypts a message for alice.d1, it shall use the active session, which is his session 2(used to decrypt alice message) but matched session 1(used to encrypt the first message) in alice DB
		bobEnc = make_shared<lime::EncryptionContext>("alice", lime_tester::messages_pattern[2]);
		bobEnc->addRecipient(*aliceDevice1);
		bobManager->encrypt(*bobDevice1, algos, bobEnc, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// destroy and reload the Managers(tests everything is correctly saved/load from local Storage)
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		aliceSessionsId.clear();
		aliceActiveSessionId =  lime_tester::get_DRsessionsId(dbFilenameAlice, *aliceDevice1, *bobDevice1, aliceSessionsId);
		BC_ASSERT_EQUAL(aliceActiveSessionId, 2, long int, "%ld");
		BC_ASSERT_EQUAL((int)aliceSessionsId.size(), 2, int, "%d");

		bobSessionsId.clear();
		bobActiveSessionId =  lime_tester::get_DRsessionsId(dbFilenameBob, *bobDevice1, *aliceDevice1, bobSessionsId);
		BC_ASSERT_EQUAL(bobActiveSessionId, 2, long int, "%ld");
		BC_ASSERT_EQUAL((int)bobSessionsId.size(), 2, int, "%d");

		// alice decrypt the messages it shall set back session 1 to be the active one as bob used this one to encrypt to alice, they have now converged on a session
		receivedMessage.clear();
		BC_ASSERT_FALSE(lime_tester::DR_message_holdsX3DHInit(bobEnc->m_recipients[0].DRmessage)); // it is not a new session, no more X3DH message
		BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDevice1, "alice", *bobDevice1, bobEnc->m_recipients[0].DRmessage, bobEnc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[2]);

		// check we have the expected configuration: 2 sessions in each base, session 1 active for alice, session 2 for bob
		aliceSessionsId.clear();
		aliceActiveSessionId =  lime_tester::get_DRsessionsId(dbFilenameAlice, *aliceDevice1, *bobDevice1, aliceSessionsId);
		BC_ASSERT_EQUAL(aliceActiveSessionId, 1, long int, "%ld");
		BC_ASSERT_EQUAL((int)aliceSessionsId.size(), 2, int, "%d");

		bobSessionsId.clear();
		bobActiveSessionId =  lime_tester::get_DRsessionsId(dbFilenameBob, *bobDevice1, *aliceDevice1, bobSessionsId);
		BC_ASSERT_EQUAL(bobActiveSessionId, 2, long int, "%ld");
		BC_ASSERT_EQUAL((int)bobSessionsId.size(), 2, int, "%d");

		// run the update function
		aliceManager->update(*aliceDevice1, algos, callback, 0, lime_tester::OPkInitialBatchSize);
		bobManager->update(*bobDevice1, algos, callback, 0, lime_tester::OPkInitialBatchSize);
		expected_success+=2;
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success,lime_tester::wait_for_timeout));

		// check we have still the same configuration on DR sessions
		aliceSessionsId.clear();
		aliceActiveSessionId =  lime_tester::get_DRsessionsId(dbFilenameAlice, *aliceDevice1, *bobDevice1, aliceSessionsId);
		BC_ASSERT_EQUAL(aliceActiveSessionId, 1, long int, "%ld");
		BC_ASSERT_EQUAL((int)aliceSessionsId.size(), 2, int, "%d");

		bobSessionsId.clear();
		bobActiveSessionId =  lime_tester::get_DRsessionsId(dbFilenameBob, *bobDevice1, *aliceDevice1, bobSessionsId);
		BC_ASSERT_EQUAL(bobActiveSessionId, 2, long int, "%ld");
		BC_ASSERT_EQUAL((int)bobSessionsId.size(), 2, int, "%d");

		// fast forward enought time to get the session old enough to be cleared by update
		// actually, move all timeStamps in local base back in time.
		// destroy the manager to be sure to get an updated version of the DB we are going to modify
		aliceManager = nullptr;
		bobManager = nullptr;
		lime_tester::forwardTime(dbFilenameAlice, lime::settings::DRSession_limboTime_days+1);
		lime_tester::forwardTime(dbFilenameBob, lime::settings::DRSession_limboTime_days+1);

		aliceManager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);
		bobManager = make_unique<LimeManager>(dbFilenameBob, X3DHServerPost);

		// run the update function
		aliceManager->update(*aliceDevice1, algos, callback, 0, lime_tester::OPkInitialBatchSize);
		bobManager->update(*bobDevice1, algos, callback, 0, lime_tester::OPkInitialBatchSize);
		expected_success+=2;
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success,lime_tester::wait_for_timeout));

		// check the configuration, kept the active sessions but removed the old ones
		aliceSessionsId.clear();
		aliceActiveSessionId =  lime_tester::get_DRsessionsId(dbFilenameAlice, *aliceDevice1, *bobDevice1, aliceSessionsId);
		BC_ASSERT_EQUAL(aliceActiveSessionId, 1, long int, "%ld");
		BC_ASSERT_EQUAL((int)aliceSessionsId.size(), 1, int, "%d");

		bobSessionsId.clear();
		bobActiveSessionId =  lime_tester::get_DRsessionsId(dbFilenameBob, *bobDevice1, *aliceDevice1, bobSessionsId);
		BC_ASSERT_EQUAL(bobActiveSessionId, 2, long int, "%ld");
		BC_ASSERT_EQUAL((int)bobSessionsId.size(), 1, int, "%d");

		// delete the users so the remote DB will be clean too
		if (cleanDatabase) {
			aliceManager->delete_user(DeviceId(*aliceDevice1, curve), callback);
			bobManager->delete_user(DeviceId(*bobDevice1, curve), callback);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success+2,lime_tester::wait_for_timeout));
			remove(dbFilenameAlice.data()); // delete the database file if already exists
			remove(dbFilenameBob.data()); // delete the database file if already exists
		}
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}
}

static void x3dh_multiple_DRsessions(void) {
#ifdef EC25519_ENABLED
	x3dh_multiple_DRsessions_test(lime::CurveId::c25519, "lime_x3dh_multiple_DRsessions");
	x3dh_multiple_DRsessions_test(lime::CurveId::c25519, "lime_x3dh_multiple_DRsessions_clean", false);
#endif
#ifdef EC448_ENABLED
	x3dh_multiple_DRsessions_test(lime::CurveId::c448, "lime_x3dh_multiple_DRsessions");
	x3dh_multiple_DRsessions_test(lime::CurveId::c448, "lime_x3dh_multiple_DRsessions_clean", false);
#endif
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	x3dh_multiple_DRsessions_test(lime::CurveId::c25519k512, "lime_x3dh_multiple_DRsessions");
	x3dh_multiple_DRsessions_test(lime::CurveId::c25519k512, "lime_x3dh_multiple_DRsessions_clean", false);

	x3dh_multiple_DRsessions_test(lime::CurveId::c25519mlk512, "lime_x3dh_multiple_DRsessions");
	x3dh_multiple_DRsessions_test(lime::CurveId::c25519mlk512, "lime_x3dh_multiple_DRsessions_clean", false);
#endif
#ifdef EC448_ENABLED
	x3dh_multiple_DRsessions_test(lime::CurveId::c448mlk1024, "lime_x3dh_multiple_DRsessions");
	x3dh_multiple_DRsessions_test(lime::CurveId::c448mlk1024, "lime_x3dh_multiple_DRsessions_clean", false);
#endif
#endif
}


/*
 * alice.d1 will encrypt to bob.d1, bob.d2, bob.d3, bob.d4
 * - message burst from alice.d1 -> bob.d1
 * - wait for callbacks. alice.d1 hold session toward d1 only
 * then burst encrypt to:
 * - bob.d1, bob.d2 : test enqueing if a part of recipients are not available
 * - bob.d1 : test going through if we can process it without calling X3DH server
 * - bob.d2 : test enqueue and have session ready when processed
 * - bob.d3 : test enqueue and must start an asynchronous X3DH request when back
 * - bob.d4 : test enqueue and must start an asynchronous X3DH request when back
 *
 */
static void x3dh_multidev_operation_queue_test(const lime::CurveId curve, const std::string &dbBaseFilename, bool continuousSession=true) {
	// create DB
	std::string dbFilenameAlice{dbBaseFilename};
	dbFilenameAlice.append(".alice.").append(CurveId2String(curve)).append(".sqlite3");
	std::string dbFilenameBob{dbBaseFilename};
	dbFilenameBob.append(".bob.").append(CurveId2String(curve)).append(".sqlite3");

	remove(dbFilenameAlice.data()); // delete the database file if already exists
	remove(dbFilenameBob.data()); // delete the database file if already exists

	lime_tester::events_counters_t counters={};
	int expected_success=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};

	try {
		std::vector<lime::CurveId> algos{curve};
		// create Manager
		auto aliceManager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);
		auto bobManager = make_unique<LimeManager>(dbFilenameBob, X3DHServerPost);

		// create Random devices names
		auto aliceDevice1 = lime_tester::makeRandomDeviceName("alice.d1.");
		auto bobDevice1 = lime_tester::makeRandomDeviceName("bob.d1.");
		auto bobDevice2 = lime_tester::makeRandomDeviceName("bob.d2.");
		auto bobDevice3 = lime_tester::makeRandomDeviceName("bob.d3.");
		auto bobDevice4 = lime_tester::makeRandomDeviceName("bob.d4.");

		// create users alice.d1 and bob.d1,d2,d3,d4
		aliceManager->create_user(*aliceDevice1, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		bobManager->create_user(*bobDevice1, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		bobManager->create_user(*bobDevice2, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		bobManager->create_user(*bobDevice3, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		bobManager->create_user(*bobDevice4, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		expected_success +=5; // we have five asynchronous operation on going
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, expected_success,lime_tester::wait_for_timeout));
		if (counters.operation_failed == 1) return; // skip the end of the test if we can't do this

		// destroy and reload the Managers(tests everything is correctly saved/load from local Storage)
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// alice first send a message to bob.d1 without waiting for callback before asking encryption of a new one
		constexpr size_t messageBurstSize = 5; // we will reuse the same buffers for further messages storage, so just encrypt 5 messages to d1
		std::array<std::shared_ptr<lime::EncryptionContext>, messageBurstSize> encs;

		for (size_t i=0; i<messageBurstSize; i++) {
			encs[i] = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[i]);
			encs[i]->addRecipient(*bobDevice1);
		}

		for (auto &enc : encs) {
			aliceManager->encrypt(*aliceDevice1, algos, enc, callback);
			expected_success++;
		}
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success,lime_tester::wait_for_timeout));

		// destroy and reload the Managers(tests everything is correctly saved/load from local Storage)
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		std::vector<uint8_t> X3DH_initMessageBuffer{}; // store the X3DH init message extracted from first message of the burst to be able to compare it to the following one, they must be the same.
		// loop on cipher message and decrypt them bob Manager
		for (size_t i=0; i<messageBurstSize; i++) {
			auto &recipient = encs[i]->m_recipients[0];
			std::vector<uint8_t> receivedMessage{};
			BC_ASSERT_TRUE(lime_tester::DR_message_holdsX3DHInit(recipient.DRmessage)); // new sessions created, they must convey X3DH init message
			if (i==0) { // first message of the burst, extract and store the X3DH init message
				lime_tester::DR_message_extractX3DHInit(recipient.DRmessage, X3DH_initMessageBuffer);
			} else { // following message of the burst, extract X3DH init message and compare it to the first one, they must be the same, we shall not create new sessions
				std::vector<uint8_t> X3DH_initMessageBuffer_next{};
				lime_tester::DR_message_extractX3DHInit(recipient.DRmessage, X3DH_initMessageBuffer_next);
				BC_ASSERT_TRUE(X3DH_initMessageBuffer == X3DH_initMessageBuffer_next);
			}
			BC_ASSERT_TRUE(bobManager->decrypt(recipient.deviceId, "bob", *aliceDevice1, recipient.DRmessage, encs[i]->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
			BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[i]);
		}

		// destroy and reload the Managers(tests everything is correctly saved/load from local Storage)
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// prepare for new encryptions
		for (size_t i=0; i<messageBurstSize; i++) {
			encs[i] = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[messageBurstSize+i]);
		}

		// now alice will request encryption of message without waiting for callback to:
		//  bob.d1,bob.d2 -> this one shall trigger a X3DH request to acquire bob.d2 key bundle
		encs[0]->addRecipient(*bobDevice1);
		encs[0]->addRecipient(*bobDevice2);
		//  bob.d1 -> this one shall be just processed synchronously so callback will be called before even returning from encrypt call
		encs[1]->addRecipient(*bobDevice1);
		//  bob.d2 -> this one shall be queued and processed when d1,d2 is done but it won't trigger an X3DH request
		encs[2]->addRecipient(*bobDevice2);
		//  bob.d3 -> this one shall be queued and processed when previous one is done, it will trigger an X3DH request to get d3 key bundle
		encs[3]->addRecipient(*bobDevice3);
		//  bob.d4 -> this one shall be queued and processed when previous one is done, it will trigger an X3DH request to get d4 key bundle
		encs[4]->addRecipient(*bobDevice4);

		for (auto &enc : encs) {
			aliceManager->encrypt(*aliceDevice1, algos, enc, callback);
			expected_success++;
		}
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success,lime_tester::wait_for_timeout));

		// destroy and reload the Managers(tests everything is correctly saved/load from local Storage)
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// decrypt all sent messages and check if they match
		// recipients holds:
		// enc[0] -> bob.d1, bob.d2
		// enc[1] -> bob.d1
		// enc[2] -> bob.d2
		// Check on these that the X3DH init message are matching (we didn't create a second session an encryption was queued correctly)
		std::vector<uint8_t> X3DH_initMessageBuffer1{};
		std::vector<uint8_t> X3DH_initMessageBuffer2{};
		// encs[0][0] and encs[1][0]
		lime_tester::DR_message_extractX3DHInit(encs[0]->m_recipients[0].DRmessage, X3DH_initMessageBuffer1);
		lime_tester::DR_message_extractX3DHInit(encs[1]->m_recipients[0].DRmessage, X3DH_initMessageBuffer2);
		BC_ASSERT_TRUE(X3DH_initMessageBuffer1 == X3DH_initMessageBuffer2);
		// encs[0][1] and encs[2][0]
		lime_tester::DR_message_extractX3DHInit(encs[0]->m_recipients[1].DRmessage, X3DH_initMessageBuffer1);
		lime_tester::DR_message_extractX3DHInit(encs[2]->m_recipients[0].DRmessage, X3DH_initMessageBuffer2);
		BC_ASSERT_TRUE(X3DH_initMessageBuffer1 == X3DH_initMessageBuffer2);

		// in encs[0] we have a message encrypted for bob.d1 and bob.d2
		std::vector<uint8_t> receivedMessage{};
		std::string receivedMessageString{};
		BC_ASSERT_TRUE(bobManager->decrypt(encs[0]->m_recipients[0].deviceId, "bob", *aliceDevice1, encs[0]->m_recipients[0].DRmessage, encs[0]->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[messageBurstSize+0]);
		BC_ASSERT_TRUE(bobManager->decrypt(encs[0]->m_recipients[1].deviceId, "bob", *aliceDevice1, encs[0]->m_recipients[1].DRmessage, encs[0]->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[messageBurstSize+0]);

		// in encs[1] we have a message encrypted to bob.d1
		BC_ASSERT_TRUE(bobManager->decrypt(encs[1]->m_recipients[0].deviceId, "bob", *aliceDevice1, encs[1]->m_recipients[0].DRmessage, encs[1]->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[messageBurstSize+1]);

		// in encs[2] we have a message encrypted to bob.d2
		BC_ASSERT_TRUE(bobManager->decrypt(encs[2]->m_recipients[0].deviceId, "bob", *aliceDevice1, encs[2]->m_recipients[0].DRmessage, encs[2]->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[messageBurstSize+2]);

		// in encs[3] we have a message encrypted to bob.d3
		BC_ASSERT_TRUE(bobManager->decrypt(encs[3]->m_recipients[0].deviceId, "bob", *aliceDevice1, encs[3]->m_recipients[0].DRmessage, encs[3]->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[messageBurstSize+3]);

		// in encs[4] we have a message encrypted to bob.d4
		BC_ASSERT_TRUE(bobManager->decrypt(encs[4]->m_recipients[0].deviceId, "bob", *aliceDevice1, encs[4]->m_recipients[0].DRmessage, encs[4]->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[messageBurstSize+4]);

		// delete the users and db
		if (cleanDatabase) {
			aliceManager->delete_user(DeviceId(*aliceDevice1, curve), callback);
			bobManager->delete_user(DeviceId(*bobDevice1, curve), callback);
			bobManager->delete_user(DeviceId(*bobDevice2, curve), callback);
			bobManager->delete_user(DeviceId(*bobDevice3, curve), callback);
			bobManager->delete_user(DeviceId(*bobDevice4, curve), callback);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success+5,lime_tester::wait_for_timeout));
			remove(dbFilenameAlice.data());
			remove(dbFilenameBob.data());
		}
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}
}

static void x3dh_multidev_operation_queue(void) {
#ifdef EC25519_ENABLED
	x3dh_multidev_operation_queue_test(lime::CurveId::c25519, "lime_x3dh_multidev_operation_queue");
	x3dh_multidev_operation_queue_test(lime::CurveId::c25519, "lime_x3dh_multidev_operation_queue_clean", false);
#endif
#ifdef EC448_ENABLED
	x3dh_multidev_operation_queue_test(lime::CurveId::c448, "lime_x3dh_multidev_operation_queue");
	x3dh_multidev_operation_queue_test(lime::CurveId::c448, "lime_x3dh_multidev_operation_queue_clean", false);
#endif
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	x3dh_multidev_operation_queue_test(lime::CurveId::c25519k512, "lime_x3dh_multidev_operation_queue");
	x3dh_multidev_operation_queue_test(lime::CurveId::c25519k512, "lime_x3dh_multidev_operation_queue_clean", false);

	x3dh_multidev_operation_queue_test(lime::CurveId::c25519mlk512, "lime_x3dh_multidev_operation_queue");
	x3dh_multidev_operation_queue_test(lime::CurveId::c25519mlk512, "lime_x3dh_multidev_operation_queue_clean", false);
#endif
#ifdef EC448_ENABLED
	x3dh_multidev_operation_queue_test(lime::CurveId::c448mlk1024, "lime_x3dh_multidev_operation_queue");
	x3dh_multidev_operation_queue_test(lime::CurveId::c448mlk1024, "lime_x3dh_multidev_operation_queue_clean", false);
#endif
#endif
}


/* test scenario:
 * - create alice.d1 and bob.d1
 * - alice ask for a burst encryption of several message to bob.d1, first one shall trigger a X3DH init, the other ones shall be queued, if not we will initiate several X3DH init
 * - bob.d1 decrypt messages, check decryption is Ok and that they encapsulate the same X3DH init message
 * - Delete Alice and Bob devices to leave distant server base clean
 *
 * if continuousSession is set to false, delete and recreate LimeManager before each new operation to force relying on local Storage
 */
static void x3dh_operation_queue_test(const lime::CurveId curve, const std::string &dbBaseFilename, bool continuousSession=true) {
	// create DB
	std::string dbFilenameAlice{dbBaseFilename};
	dbFilenameAlice.append(".alice.").append(CurveId2String(curve)).append(".sqlite3");
	std::string dbFilenameBob{dbBaseFilename};
	dbFilenameBob.append(".bob.").append(CurveId2String(curve)).append(".sqlite3");

	remove(dbFilenameAlice.data()); // delete the database file if already exists
	remove(dbFilenameBob.data()); // delete the database file if already exists

	lime_tester::events_counters_t counters={};
	int expected_success=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};

	try {
		std::vector<lime::CurveId> algos{curve};
		// create Manager
		auto aliceManager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);
		auto bobManager = make_unique<LimeManager>(dbFilenameBob, X3DHServerPost);

		// create Random devices names
		auto aliceDevice1 = lime_tester::makeRandomDeviceName("alice.d1.");
		auto bobDevice1 = lime_tester::makeRandomDeviceName("bob.d1.");

		// create users alice.d1 and bob.d1
		aliceManager->create_user(*aliceDevice1, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		bobManager->create_user(*bobDevice1, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		expected_success +=2; // we have two asynchronous operation on going
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, expected_success,lime_tester::wait_for_timeout)); // we must get callback saying all went well
		if (counters.operation_failed == 1) return; // skip the end of the test if we can't do this

		/* destroy and reload the Managers(tests everything is correctly saved/load from local Storage) */
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// alice send a burst of messages to bob without waiting for callback before asking encryption of a new one
		constexpr size_t messageBurstSize = 8;
		std::array<std::shared_ptr<lime::EncryptionContext>, messageBurstSize> encs;

		for (size_t i=0; i<messageBurstSize; i++) {
			encs[i] = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[i]);
			encs[i]->addRecipient(*bobDevice1);
		}

		for (auto &enc : encs) {
			aliceManager->encrypt(*aliceDevice1, algos, enc, callback);
			expected_success++;
		}
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success,lime_tester::wait_for_timeout));

		/* destroy and reload the Managers(tests everything is correctly saved/load from local Storage) */
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// bob.d1 decrypt the messages
		std::vector<uint8_t> X3DH_initMessageBuffer{}; // store the X3DH init message extracted from first message of the burst to be able to compare it to the following one, they must be the same.
		// loop on cipher message and decrypt them using bob Manager
		for (size_t i=0; i<messageBurstSize; i++) {
			const auto &recipient = encs[i]->m_recipients[0];
			std::vector<uint8_t> receivedMessage{};
			BC_ASSERT_TRUE(lime_tester::DR_message_holdsX3DHInit(recipient.DRmessage)); // new sessions created, they must convey X3DH init message
			if (i==0) { // first message of the burst, extract and store the X3DH init message
				lime_tester::DR_message_extractX3DHInit(recipient.DRmessage, X3DH_initMessageBuffer);
			} else { // following message of the burst, extract X3DH init message and compare it to the first one, they must be the same, we shall not create new sessions
				std::vector<uint8_t> X3DH_initMessageBuffer_next{};
				lime_tester::DR_message_extractX3DHInit(recipient.DRmessage, X3DH_initMessageBuffer_next);
				BC_ASSERT_TRUE(X3DH_initMessageBuffer == X3DH_initMessageBuffer_next);
			}
			BC_ASSERT_TRUE(bobManager->decrypt(recipient.deviceId, "bob", *aliceDevice1, recipient.DRmessage, encs[i]->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
			BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[i]);
		}

		if (cleanDatabase) {

			// delete the users
			aliceManager->delete_user(DeviceId(*aliceDevice1, curve), callback);
			bobManager->delete_user(DeviceId(*bobDevice1, curve), callback);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success+2,lime_tester::wait_for_timeout));
			remove(dbFilenameAlice.data()); // delete the database file if already exists
			remove(dbFilenameBob.data()); // delete the database file if already exists
		}
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}
}

static void x3dh_operation_queue(void) {
#ifdef EC25519_ENABLED
	x3dh_operation_queue_test(lime::CurveId::c25519, "lime_x3dh_operation_queue");
	x3dh_operation_queue_test(lime::CurveId::c25519, "lime_x3dh_operation_queue_clean", false);
#endif
#ifdef EC448_ENABLED
	x3dh_operation_queue_test(lime::CurveId::c448, "lime_x3dh_operation_queue");
	x3dh_operation_queue_test(lime::CurveId::c448, "lime_x3dh_operation_queue_clean", false);
#endif
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	x3dh_operation_queue_test(lime::CurveId::c25519k512, "lime_x3dh_operation_queue");
	x3dh_operation_queue_test(lime::CurveId::c25519k512, "lime_x3dh_operation_queue_clean", false);

	x3dh_operation_queue_test(lime::CurveId::c25519mlk512, "lime_x3dh_operation_queue");
	x3dh_operation_queue_test(lime::CurveId::c25519mlk512, "lime_x3dh_operation_queue_clean", false);
#endif
#ifdef EC448_ENABLED
	x3dh_operation_queue_test(lime::CurveId::c448mlk1024, "lime_x3dh_operation_queue");
	x3dh_operation_queue_test(lime::CurveId::c448mlk1024, "lime_x3dh_operation_queue_clean", false);
#endif
#endif
}
 /* Test Scenario
 * - Alice and Bob register themselves on X3DH server
 * - Alice send message to Bob
 * - Bob deletes himself and create a user with the same DeviceId (but different Ik)
 * - Bob encrypts a message to Alice
 * - Alice tries to decrypt it, it shall fail
 */
static void lime_identity_theft_test(const lime::CurveId curve) {
	const std::string dbBaseFilename{"lime_identity_theft"};
	// create DB
	std::string dbFilenameAlice{dbBaseFilename};
	dbFilenameAlice.append(".alice.").append(CurveId2String(curve)).append(".sqlite3");
	std::string dbFilenameBob{dbBaseFilename};
	dbFilenameBob.append(".bob.").append(CurveId2String(curve)).append(".sqlite3");

	remove(dbFilenameAlice.data()); // delete the database file if already exists
	remove(dbFilenameBob.data()); // delete the database file if already exists

	lime_tester::events_counters_t counters={};
	int expected_success=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};

	try {
		std::vector<lime::CurveId> algos{curve};

		// create Manager
		auto aliceManager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);
		auto bobManager = make_unique<LimeManager>(dbFilenameBob, X3DHServerPost);

		// create Random devices names
		auto aliceDevice = lime_tester::makeRandomDeviceName("alice.");
		auto bobDevice = lime_tester::makeRandomDeviceName("bob.");

		// create users
		aliceManager->create_user(*aliceDevice, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		bobManager->create_user(*bobDevice, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		if (counters.operation_failed > 0) return; // skip the end of the test if we can't do this

		// alice encrypts a message to bob
		auto enc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[0]);
		enc->addRecipient(*bobDevice);
		aliceManager->encrypt(*aliceDevice, algos, enc, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// we don't need to decrypt, it only matters that Alice registered Bob's device Ik
		// Bob deletes its user and create it again (with the same deviceId)
		bobManager->delete_user(DeviceId(*bobDevice, curve), callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		bobManager->create_user(*bobDevice, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// Bob encrypts a message to Alice
		enc = make_shared<lime::EncryptionContext>("alice", lime_tester::messages_pattern[1]);
		enc->addRecipient(*aliceDevice);
		bobManager->encrypt(*bobDevice, algos, enc, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// alice tries decrypt it, it shall fail as Bob changed Ik!
		std::vector<uint8_t> receivedMessage{};
		BC_ASSERT(aliceManager->decrypt(*aliceDevice, "alice", *bobDevice, enc->m_recipients[0].DRmessage, enc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::fail);

		// Alice encrypts maxSendingChain messages to bob, she does it with its old Ik
		for (auto i=0; i<lime::settings::maxSendingChain-1; i++) { // -1 as we already encrypted one message to bob
			// alice encrypt
			enc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[0]);
			enc->addRecipient(*bobDevice);
			aliceManager->encrypt(*aliceDevice, algos, enc, callback);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
			BC_ASSERT_TRUE(enc->m_recipients[0].peerStatus == lime::PeerDeviceStatus::untrusted);
		}

		// update belle-sip stack processing possible incoming messages from server
		belle_sip_stack_sleep(bc_stack,0);

		// now we've ran the maximum number of messages, Alice shall ask for a new key bundle for bob, but it will fail because it will come with a new Ik
		enc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[0]);
		enc->addRecipient(*bobDevice);
		aliceManager->encrypt(*aliceDevice, algos, enc, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_failed,1,lime_tester::wait_for_timeout));

		// delete the users
		if (cleanDatabase) {
			aliceManager->delete_user(DeviceId(*aliceDevice, curve), callback);
			bobManager->delete_user(DeviceId(*bobDevice, curve), callback);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success+2,lime_tester::wait_for_timeout));
			remove(dbFilenameAlice.data());
			remove(dbFilenameBob.data());
		}
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}
}

static void lime_identity_theft(void) {
#ifdef EC25519_ENABLED
	lime_identity_theft_test(lime::CurveId::c25519);
#endif
#ifdef EC448_ENABLED
	lime_identity_theft_test(lime::CurveId::c448);
#endif
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	lime_identity_theft_test(lime::CurveId::c25519k512);

	lime_identity_theft_test(lime::CurveId::c25519mlk512);
#endif
#ifdef EC448_ENABLED
	lime_identity_theft_test(lime::CurveId::c448mlk1024);
#endif
#endif
}
 /* A simple test with alice having 1 device and bob 2
 * - Alice and Bob(d1 and d2) register themselves on X3DH server
 * - Alice send message to Bob (d1 and d2)
 * - Alice send another message to Bob (d1 and d2)
 * - Bob d1 respond to Alice(with bob d2 in copy)
 * - Bob d2 respond to Alice(with bob d1 in copy)
 * - Alice send another message to Bob(d1 and d2)
 * - Delete Alice and Bob devices to leave distant server base clean
 *
 * At each message check that the X3DH init is present or not in the DR header
 * if continuousSession is set to false, delete and recreate LimeManager before each new operation to force relying on local Storage
 * if useAD=true, encrypt and decrypt operation are performed using the uint8_t buffer and not the string expecting recipientDeviceId
 * Note: no asynchronous operation will start before the previous is over(callback returns)
 */
static void x3dh_basic_test(const lime::CurveId curve, const std::string &dbBaseFilename, bool continuousSession=true, bool useAD=false) {
	// create DB
	std::string dbFilenameAlice{dbBaseFilename};
	dbFilenameAlice.append(".alice.").append(CurveId2String(curve)).append(".sqlite3");
	std::string dbFilenameBob{dbBaseFilename};
	dbFilenameBob.append(".bob.").append(CurveId2String(curve)).append(".sqlite3");
	auto aliceUserId = std::make_shared<std::vector<uint8_t>>();
	aliceUserId->assign({'\0','a','l','i','c','e'});
	auto bobUserId = std::make_shared<std::vector<uint8_t>>();
	bobUserId->assign({'b','o','b'});
	remove(dbFilenameAlice.data()); // delete the database file if already exists
	remove(dbFilenameBob.data()); // delete the database file if already exists

	lime_tester::events_counters_t counters={};
	int expected_success=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};

	try {
		std::vector<lime::CurveId> algos{curve};
		// create Manager
		auto aliceManager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);
		auto bobManager = make_unique<LimeManager>(dbFilenameBob, X3DHServerPost);

		// create Random devices names
		auto aliceDevice1 = lime_tester::makeRandomDeviceName("alice.d1.");
		auto bobDevice1 = lime_tester::makeRandomDeviceName("bob.d1.");
		auto bobDevice2 = lime_tester::makeRandomDeviceName("bob.d2.");

		// create users
		aliceManager->create_user(*aliceDevice1, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		bobManager->create_user(*bobDevice1, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		bobManager->create_user(*bobDevice2, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		if (counters.operation_failed == 1) return; // skip the end of the test if we can't do this

		/* destroy and reload the Managers(tests everything is correctly saved/load from local Storage) */
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// alice send a message to bob
		std::shared_ptr<lime::EncryptionContext> enc;
		if (useAD) {
			enc = make_shared<lime::EncryptionContext>(*bobUserId, lime_tester::messages_pattern[0]);
		} else {
			enc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[0]);
		}
		enc->addRecipient(*bobDevice1);
		enc->addRecipient(*bobDevice2);
		aliceManager->encrypt(*aliceDevice1, algos, enc, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		// loop on cipher message and decrypt with bob Manager
		for (const auto &recipient : enc->m_recipients) {
			std::vector<uint8_t> receivedMessage{};
			BC_ASSERT_TRUE(lime_tester::DR_message_holdsX3DHInit(recipient.DRmessage)); // new sessions created, they must convey X3DH init message
			if (useAD) {
				BC_ASSERT_TRUE(bobManager->decrypt(recipient.deviceId, *bobUserId, *aliceDevice1, recipient.DRmessage, enc->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
			} else {
				BC_ASSERT_TRUE(bobManager->decrypt(recipient.deviceId, "bob", *aliceDevice1, recipient.DRmessage, enc->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
			}
			BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[0]);
		}

		// cleaning
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// encrypt another one, same recipients(we shall have no new X3DH session but still the X3DH init message)
		if (useAD) {
			enc = make_shared<lime::EncryptionContext>(*bobUserId, lime_tester::messages_pattern[1]);
		} else {
			enc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[1]);
		}
		enc->addRecipient(*bobDevice1);
		enc->addRecipient(*bobDevice2);
		aliceManager->encrypt(*aliceDevice1, algos, enc, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// loop on cipher message and decrypt with bob Manager
		for (auto &recipient : enc->m_recipients) {
			std::vector<uint8_t> receivedMessage{};
			BC_ASSERT_TRUE(lime_tester::DR_message_holdsX3DHInit(recipient.DRmessage)); // again X3DH message as no one ever responded to alice.d1
			if (useAD) {
				BC_ASSERT_TRUE(bobManager->decrypt(recipient.deviceId, *bobUserId, *aliceDevice1, recipient.DRmessage, enc->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
			} else {
				BC_ASSERT_TRUE(bobManager->decrypt(recipient.deviceId, "bob", *aliceDevice1, recipient.DRmessage, enc->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
			}
			BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[1]);
		}

		// cleaning
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}


		// bob.d1 reply to alice and copy bob.d2
		if (useAD) {
			enc = make_shared<lime::EncryptionContext>(*aliceUserId, lime_tester::messages_pattern[2]);
		} else {
			enc = make_shared<lime::EncryptionContext>("alice", lime_tester::messages_pattern[2]);
		}
		enc->addRecipient(*aliceDevice1);
		enc->addRecipient(*bobDevice2);
		bobManager->encrypt(*bobDevice1, algos, enc, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// decrypt it
		std::vector<uint8_t> receivedMessage{};
		BC_ASSERT_FALSE(lime_tester::DR_message_holdsX3DHInit(enc->m_recipients[0].DRmessage)); // alice.d1 to bob.d1 already set up the DR Session, we shall not have any  X3DH message here
		if (useAD) {
			BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDevice1, *aliceUserId, *bobDevice1, enc->m_recipients[0].DRmessage, enc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted);
		} else {
			BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDevice1, "alice", *bobDevice1, enc->m_recipients[0].DRmessage, enc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted);
		}
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[2]);

		receivedMessage.clear();
		BC_ASSERT_TRUE(lime_tester::DR_message_holdsX3DHInit(enc->m_recipients[1].DRmessage)); // bob.d1 to bob.d2 is a new session, we must have a X3DH message here
		if (useAD) {
			BC_ASSERT_TRUE(bobManager->decrypt(*bobDevice2, *aliceUserId, *bobDevice1, enc->m_recipients[1].DRmessage, enc->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
		} else {
			BC_ASSERT_TRUE(bobManager->decrypt(*bobDevice2, "alice", *bobDevice1, enc->m_recipients[1].DRmessage, enc->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
		}
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[2]);

		// cleaning
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// Now do bob.d2 to alice and bob.d1 every one has an open session towards everyone
		if (useAD) {
			enc = make_shared<lime::EncryptionContext>(*aliceUserId, lime_tester::messages_pattern[3]);
		} else {
			enc = make_shared<lime::EncryptionContext>("alice", lime_tester::messages_pattern[3]);
		}
		enc->addRecipient(*aliceDevice1);
		enc->addRecipient(*bobDevice1);
		bobManager->encrypt(*bobDevice2, algos, enc, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// decrypt it
		receivedMessage.clear();
		BC_ASSERT_FALSE(lime_tester::DR_message_holdsX3DHInit(enc->m_recipients[0].DRmessage)); // alice.d1 to bob.d2 already set up the DR Session, we shall not have any  X3DH message here
		if (useAD) {
			BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDevice1, *aliceUserId, *bobDevice2, enc->m_recipients[0].DRmessage, enc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted);
		} else {
			BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDevice1, "alice", *bobDevice2, enc->m_recipients[0].DRmessage, enc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted);
		}
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[3]);
		receivedMessage.clear();
		BC_ASSERT_FALSE(lime_tester::DR_message_holdsX3DHInit(enc->m_recipients[1].DRmessage)); // bob.d1 to bob.d2 already set up the DR Session, we shall not have any  X3DH message here
		// bob.d1 and bob.d2 share the same db, like two accounts on the same device, so for bob.d1, bob.d2 status is always trusted.
		if (useAD) {
			BC_ASSERT_TRUE(bobManager->decrypt(*bobDevice1, *aliceUserId, *bobDevice2, enc->m_recipients[1].DRmessage, enc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::trusted);
		} else {
			BC_ASSERT_TRUE(bobManager->decrypt(*bobDevice1, "alice", *bobDevice2, enc->m_recipients[1].DRmessage, enc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::trusted);
		}
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[3]);

		// cleaning
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// encrypt another one from alice to bob.d1 and .d2, it must not send X3DH init anymore
		if (useAD) {
			enc = make_shared<lime::EncryptionContext>(*bobUserId, lime_tester::messages_pattern[4]);
		} else {
			enc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[4]);
		}
		enc->addRecipient(*bobDevice1);
		enc->addRecipient(*bobDevice2);
		aliceManager->encrypt(*aliceDevice1, algos, enc, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// loop on cipher message and decrypt with bob Manager
		for (auto &recipient : enc->m_recipients) {
			std::vector<uint8_t> receivedMessage{};
			BC_ASSERT_FALSE(lime_tester::DR_message_holdsX3DHInit(recipient.DRmessage)); // bob.d1 and d2 both responded, so no more X3DH message shall be sent
			if (useAD) {
				BC_ASSERT_TRUE(bobManager->decrypt(recipient.deviceId, *bobUserId, *aliceDevice1, recipient.DRmessage, enc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted);
			} else {
				BC_ASSERT_TRUE(bobManager->decrypt(recipient.deviceId, "bob", *aliceDevice1, recipient.DRmessage, enc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted);
			}
			BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[4]);
		}

		// cleaning
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// delete the users
		if (cleanDatabase) {
			aliceManager->delete_user(DeviceId(*aliceDevice1, curve), callback);
			bobManager->delete_user(DeviceId(*bobDevice1, curve), callback);
			bobManager->delete_user(DeviceId(*bobDevice2, curve), callback);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success+3,lime_tester::wait_for_timeout));
			remove(dbFilenameAlice.data());
			remove(dbFilenameBob.data());
		}
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}
}

static void x3dh_basic(void) {
#ifdef EC25519_ENABLED
	x3dh_basic_test(lime::CurveId::c25519, "lime_x3dh_basic");
	x3dh_basic_test(lime::CurveId::c25519, "lime_x3dh_basic_clean", false);
	x3dh_basic_test(lime::CurveId::c25519, "lime_x3dh_basic_AD", true, true);
#endif
#ifdef EC448_ENABLED
	x3dh_basic_test(lime::CurveId::c448, "lime_x3dh_basic");
	x3dh_basic_test(lime::CurveId::c448, "lime_x3dh_basic_clean", false);
	x3dh_basic_test(lime::CurveId::c448, "lime_x3dh_basic_AD", true, true);
#endif
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	x3dh_basic_test(lime::CurveId::c25519k512, "lime_x3dh_basic");
	x3dh_basic_test(lime::CurveId::c25519k512, "lime_x3dh_basic_clean", false);
	x3dh_basic_test(lime::CurveId::c25519k512, "lime_x3dh_basic_AD", true, true);

	x3dh_basic_test(lime::CurveId::c25519mlk512, "lime_x3dh_basic");
	x3dh_basic_test(lime::CurveId::c25519mlk512, "lime_x3dh_basic_clean", false);
	x3dh_basic_test(lime::CurveId::c25519mlk512, "lime_x3dh_basic_AD", true, true);
#endif
#ifdef EC448_ENABLED
	x3dh_basic_test(lime::CurveId::c448mlk1024, "lime_x3dh_basic");
	x3dh_basic_test(lime::CurveId::c448mlk1024, "lime_x3dh_basic_clean", false);
	x3dh_basic_test(lime::CurveId::c448mlk1024, "lime_x3dh_basic_AD", true, true);
#endif
#endif
}

/**
 * alice encrypts to bob but she's given a recipient list holding twice the same device
 */
static void x3dh_double_recipient_test(const lime::CurveId curve) {
	const std::string dbBaseFilename{"lime_double_recipient"};
	// create DB
	std::string dbFilenameAlice{dbBaseFilename};
	dbFilenameAlice.append(".alice.").append(CurveId2String(curve)).append(".sqlite3");
	std::string dbFilenameBob{dbBaseFilename};
	dbFilenameBob.append(".bob.").append(CurveId2String(curve)).append(".sqlite3");

	remove(dbFilenameAlice.data()); // delete the database file if already exists
	remove(dbFilenameBob.data()); // delete the database file if already exists

	lime_tester::events_counters_t counters={};
	int expected_success=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};

	try {
		std::vector<lime::CurveId> algos{curve};
		// create Manager
		auto aliceManager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);
		auto bobManager = make_unique<LimeManager>(dbFilenameBob, X3DHServerPost);

		// create Random devices names
		auto aliceDevice = lime_tester::makeRandomDeviceName("alice.d1.");
		auto bobDevice = lime_tester::makeRandomDeviceName("bob.d1.");

		// create users
		aliceManager->create_user(*aliceDevice, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		bobManager->create_user(*bobDevice, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		if (counters.operation_failed == 1) return; // skip the end of the test if we can't do this

		// alice send a message to bob, he is twice in the list
		auto enc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[0]);
		enc->addRecipient(*bobDevice);
		enc->addRecipient(*bobDevice);
		aliceManager->encrypt(*aliceDevice, algos, enc, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// first recipient should hold a message, the second a failed status
		BC_ASSERT_TRUE(enc->m_recipients[1].peerStatus == lime::PeerDeviceStatus::fail);
		std::vector<uint8_t> receivedMessage{};
		BC_ASSERT_TRUE(bobManager->decrypt(enc->m_recipients[0].deviceId, "bob", *aliceDevice, enc->m_recipients[0].DRmessage, enc->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[0]);

		// Repeat the process now that Alice has a session with Bob in DB
		enc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[1]);
		enc->addRecipient(*bobDevice);
		enc->addRecipient(*bobDevice);
		aliceManager->encrypt(*aliceDevice, algos, enc, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// first recipient should hold a message, the second a failed status
		BC_ASSERT_TRUE(enc->m_recipients[1].peerStatus == lime::PeerDeviceStatus::fail);
		receivedMessage.clear();
		BC_ASSERT_TRUE(bobManager->decrypt(enc->m_recipients[0].deviceId, "bob", *aliceDevice, enc->m_recipients[0].DRmessage, enc->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[1]);

		// reset managers so the session is not in cache but in DB, and repeat
		managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);
		enc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[2]);
		enc->addRecipient(*bobDevice);
		enc->addRecipient(*bobDevice);
		aliceManager->encrypt(*aliceDevice, algos, enc, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// first recipient should hold a message, the second a failed status
		BC_ASSERT_TRUE(enc->m_recipients[1].peerStatus == lime::PeerDeviceStatus::fail);
		receivedMessage.clear();
		BC_ASSERT_TRUE(bobManager->decrypt(enc->m_recipients[0].deviceId, "bob", *aliceDevice, enc->m_recipients[0].DRmessage, enc->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[2]);


		// delete the users
		if (cleanDatabase) {
			aliceManager->delete_user(DeviceId(*aliceDevice, curve), callback);
			bobManager->delete_user(DeviceId(*bobDevice, curve), callback);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success+2,lime_tester::wait_for_timeout));
			remove(dbFilenameAlice.data());
			remove(dbFilenameBob.data());
		}
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}
}
static void x3dh_double_recipient(void) {
#ifdef EC25519_ENABLED
	x3dh_double_recipient_test(lime::CurveId::c25519);
#endif
#ifdef EC448_ENABLED
	x3dh_double_recipient_test(lime::CurveId::c448);
#endif
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	x3dh_double_recipient_test(lime::CurveId::c25519k512);

	x3dh_double_recipient_test(lime::CurveId::c25519mlk512);
#endif
#ifdef EC448_ENABLED
	x3dh_double_recipient_test(lime::CurveId::c448mlk1024);
#endif
#endif
}

 /* A simple test with alice having 1 device and bob 2
 * - Alice and Bob(d1 and d2) register themselves on X3DH server
 * - Alice send another message to Bob non existing device d3, it shall fail without exception
 * - Alice send another message to Bob (d1 and d2) and try to send to an non existing device d3, d1 and d2 shall work
 * - repeat with the unknown device at the begining and in the middle of the recipients list
 * - Delete Alice and Bob devices to leave distant server base clean
 *
 * At each message check that the X3DH init is present or not in the DR header
 * if continuousSession is set to false, delete and recreate LimeManager before each new operation to force relying on local Storage
 * Note: no asynchronous operation will start before the previous is over(callback returns)
 */
static void x3dh_user_not_found_test(const lime::CurveId curve, const std::string &dbBaseFilename, bool continuousSession=true) {
	// create DB
	std::string dbFilenameAlice{dbBaseFilename};
	dbFilenameAlice.append(".alice.").append(CurveId2String(curve)).append(".sqlite3");
	std::string dbFilenameBob{dbBaseFilename};
	dbFilenameBob.append(".bob.").append(CurveId2String(curve)).append(".sqlite3");

	remove(dbFilenameAlice.data()); // delete the database file if already exists
	remove(dbFilenameBob.data()); // delete the database file if already exists

	lime_tester::events_counters_t counters={};
	int expected_success=0;
	int expected_fail=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};

	try {
		std::vector<lime::CurveId> algos{curve};
		// create Manager
		auto aliceManager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);
		auto bobManager = make_unique<LimeManager>(dbFilenameBob, X3DHServerPost);

		// create Random devices names
		auto aliceDevice1 = lime_tester::makeRandomDeviceName("alice.d1.");
		auto bobDevice1 = lime_tester::makeRandomDeviceName("bob.d1.");
		auto bobDevice2 = lime_tester::makeRandomDeviceName("bob.d2.");
		auto bobDevice3 = lime_tester::makeRandomDeviceName("bob.d3."); // for this one we won't create the user nor register it on the X3DH server

		// create users
		aliceManager->create_user(*aliceDevice1, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		bobManager->create_user(*bobDevice1, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		bobManager->create_user(*bobDevice2, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		if (counters.operation_failed == 1) return; // skip the end of the test if we can't do this

		/* destroy and reload the Managers(tests everything is correctly saved/load from local Storage) */
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// alice send a message to bob d3
		auto enc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[0]);
		enc->addRecipient(*bobDevice3);
		aliceManager->encrypt(*aliceDevice1, algos, enc, callback);
		// The encryption shall return an error: no recipient device could get a key
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_failed,++expected_fail,lime_tester::wait_for_timeout));
		// and the status of the recipient disclose the failure on that one
		BC_ASSERT_TRUE(enc->m_recipients[0].peerStatus == lime::PeerDeviceStatus::fail);

		// cleaning
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// encrypt another one, for d1, d2 and d3. The first two shall be ok and d3 shall have a failed status
		enc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[1]);
		enc->addRecipient(*bobDevice1);
		enc->addRecipient(*bobDevice2);
		enc->addRecipient(*bobDevice3);
		aliceManager->encrypt(*aliceDevice1, algos, enc, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// loop on cipher message and decrypt with bob Manager
		for (auto &recipient : enc->m_recipients) {
			if (recipient.deviceId == *bobDevice3) {
				BC_ASSERT_TRUE(recipient.peerStatus == lime::PeerDeviceStatus::fail);
			} else {
				std::vector<uint8_t> receivedMessage{};
				BC_ASSERT_TRUE(recipient.peerStatus == lime::PeerDeviceStatus::unknown); // it is the first time alice has contact with bob's d1 and d2 devices
				BC_ASSERT_TRUE(lime_tester::DR_message_holdsX3DHInit(recipient.DRmessage)); // first communication holds a X3DH message
				BC_ASSERT_TRUE(bobManager->decrypt(recipient.deviceId, "bob", *aliceDevice1, recipient.DRmessage, enc->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
				BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[1]);
			}
		}

		// cleaning
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// encrypt another one, for d3, d2 and d1.
		enc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[2]);
		enc->addRecipient(*bobDevice3);
		enc->addRecipient(*bobDevice2);
		enc->addRecipient(*bobDevice1);
		aliceManager->encrypt(*aliceDevice1, algos, enc, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// loop on cipher message and decrypt with bob Manager
		for (auto &recipient : enc->m_recipients) {
			if (recipient.deviceId == *bobDevice3) {
				BC_ASSERT_TRUE(recipient.peerStatus == lime::PeerDeviceStatus::fail);
			} else {
				std::vector<uint8_t> receivedMessage{};
				BC_ASSERT_TRUE(recipient.peerStatus == lime::PeerDeviceStatus::untrusted);
				BC_ASSERT_TRUE(lime_tester::DR_message_holdsX3DHInit(recipient.DRmessage)); // first communication holds a X3DH message
				BC_ASSERT_TRUE(bobManager->decrypt(recipient.deviceId, "bob", *aliceDevice1, recipient.DRmessage, enc->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
				BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[2]);
			}
		}

		// cleaning
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// encrypt another one, for d2, d3 and d1.
		enc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[3]);
		enc->addRecipient(*bobDevice2);
		enc->addRecipient(*bobDevice3);
		enc->addRecipient(*bobDevice1);
		aliceManager->encrypt(*aliceDevice1, algos, enc, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// loop on cipher message and decrypt with bob Manager
		for (auto &recipient : enc->m_recipients) {
			if (recipient.deviceId == *bobDevice3) {
				BC_ASSERT_TRUE(recipient.peerStatus == lime::PeerDeviceStatus::fail);
			} else {
				std::vector<uint8_t> receivedMessage{};
				BC_ASSERT_TRUE(recipient.peerStatus == lime::PeerDeviceStatus::untrusted);
				BC_ASSERT_TRUE(lime_tester::DR_message_holdsX3DHInit(recipient.DRmessage)); // first communication holds a X3DH message
				BC_ASSERT_TRUE(bobManager->decrypt(recipient.deviceId, "bob", *aliceDevice1, recipient.DRmessage, enc->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
				BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[3]);
			}
		}

		// cleaning
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// Repeat the 3 previous encrypt/decrypt but each time delete bob's devices from alice's cache so it will force
		// request to the X3DH server to ask for the 3 devices
		aliceManager->delete_peerDevice(*bobDevice1);
		aliceManager->delete_peerDevice(*bobDevice2);
		aliceManager->delete_peerDevice(*bobDevice3);

		// encrypt another one, for d1, d2 and d3. The first two shall be ok and d3 shall have a failed status
		enc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[1]);
		enc->addRecipient(*bobDevice1);
		enc->addRecipient(*bobDevice2);
		enc->addRecipient(*bobDevice3);
		aliceManager->encrypt(*aliceDevice1, algos, enc, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// loop on cipher message and decrypt with bob Manager
		for (auto &recipient : enc->m_recipients) {
			if (recipient.deviceId == *bobDevice3) {
				BC_ASSERT_TRUE(recipient.peerStatus == lime::PeerDeviceStatus::fail);
			} else {
				std::vector<uint8_t> receivedMessage{};
				BC_ASSERT_TRUE(recipient.peerStatus == lime::PeerDeviceStatus::unknown); // it is the first time alice has contact with bob's d1 and d2 devices
				BC_ASSERT_TRUE(lime_tester::DR_message_holdsX3DHInit(recipient.DRmessage)); // first communication holds a X3DH message
				BC_ASSERT_TRUE(bobManager->decrypt(recipient.deviceId, "bob", *aliceDevice1, recipient.DRmessage, enc->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
				BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[1]);
			}
		}

		// cleaning
		aliceManager->delete_peerDevice(*bobDevice1);
		aliceManager->delete_peerDevice(*bobDevice2);
		aliceManager->delete_peerDevice(*bobDevice3);
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// encrypt another one, for d3, d2 and d1.
		enc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[2]);
		enc->addRecipient(*bobDevice3);
		enc->addRecipient(*bobDevice2);
		enc->addRecipient(*bobDevice1);
		aliceManager->encrypt(*aliceDevice1, algos, enc, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// loop on cipher message and decrypt with bob Manager
		for (auto &recipient : enc->m_recipients) {
			if (recipient.deviceId == *bobDevice3) {
				BC_ASSERT_TRUE(recipient.peerStatus == lime::PeerDeviceStatus::fail);
			} else {
				std::vector<uint8_t> receivedMessage{};
				BC_ASSERT_TRUE(recipient.peerStatus == lime::PeerDeviceStatus::unknown);
				BC_ASSERT_TRUE(lime_tester::DR_message_holdsX3DHInit(recipient.DRmessage)); // first communication holds a X3DH message
				BC_ASSERT_TRUE(bobManager->decrypt(recipient.deviceId, "bob", *aliceDevice1, recipient.DRmessage, enc->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
				BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[2]);
			}
		}

		// cleaning
		aliceManager->delete_peerDevice(*bobDevice1);
		aliceManager->delete_peerDevice(*bobDevice2);
		aliceManager->delete_peerDevice(*bobDevice3);
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// encrypt another one, for d2, d3 and d1.
		enc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[3]);
		enc->addRecipient(*bobDevice2);
		enc->addRecipient(*bobDevice3);
		enc->addRecipient(*bobDevice1);
		aliceManager->encrypt(*aliceDevice1, algos, enc, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// loop on cipher message and decrypt with bob Manager
		for (auto &recipient : enc->m_recipients) {
			if (recipient.deviceId == *bobDevice3) {
				BC_ASSERT_TRUE(recipient.peerStatus == lime::PeerDeviceStatus::fail);
			} else {
				std::vector<uint8_t> receivedMessage{};
				BC_ASSERT_TRUE(recipient.peerStatus == lime::PeerDeviceStatus::unknown);
				BC_ASSERT_TRUE(lime_tester::DR_message_holdsX3DHInit(recipient.DRmessage)); // first communication holds a X3DH message
				BC_ASSERT_TRUE(bobManager->decrypt(recipient.deviceId, "bob", *aliceDevice1, recipient.DRmessage, enc->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
				BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[3]);
			}
		}

		// cleaning
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// delete the users
		if (cleanDatabase) {
			aliceManager->delete_user(DeviceId(*aliceDevice1, curve), callback);
			bobManager->delete_user(DeviceId(*bobDevice1, curve), callback);
			bobManager->delete_user(DeviceId(*bobDevice2, curve), callback);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success+3,lime_tester::wait_for_timeout));
			remove(dbFilenameAlice.data());
			remove(dbFilenameBob.data());
		}
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}
}

static void x3dh_user_not_found(void) {
#ifdef EC25519_ENABLED
	x3dh_user_not_found_test(lime::CurveId::c25519, "lime_x3dh_user_not_found");
	x3dh_user_not_found_test(lime::CurveId::c25519, "lime_x3dh_user_not_found_clean", false);
#endif
#ifdef EC448_ENABLED
	x3dh_user_not_found_test(lime::CurveId::c448, "lime_x3dh_user_not_found");
	x3dh_user_not_found_test(lime::CurveId::c448, "lime_x3dh_user_not_found_clean", false);
#endif
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	x3dh_user_not_found_test(lime::CurveId::c25519k512, "lime_x3dh_user_not_found");
	x3dh_user_not_found_test(lime::CurveId::c25519k512, "lime_x3dh_user_not_found_clean", false);

	x3dh_user_not_found_test(lime::CurveId::c25519mlk512, "lime_x3dh_user_not_found");
	x3dh_user_not_found_test(lime::CurveId::c25519mlk512, "lime_x3dh_user_not_found_clean", false);
#endif
#ifdef EC448_ENABLED
	x3dh_user_not_found_test(lime::CurveId::c448mlk1024, "lime_x3dh_user_not_found");
	x3dh_user_not_found_test(lime::CurveId::c448mlk1024, "lime_x3dh_user_not_found_clean", false);
#endif
#endif
}

/**
 * NOTE: this test uses low level user management functions which are not available to lib user which have access to
 * LimeManager functions only. Do not consider this test code as an example as it is not the way users shall
 * be managed by library end users.
 *
 * - Create alice using LimeManager
 * - Load alice directly using load_limeUser (it's for test only, this is not really available to lib user)
 * - Fail at creating alice again in the same DB
 * - Fail at loading an unknown user
 * - Delete Alice through LimeManager
 * - Fail at deleting an unknown user
 * - Create Alice again
 * - using an other DB, create alice, success on local Storage but shall get a callback error
 * - clean DB by deleting alice
 *
 */
static void user_management_test(const lime::CurveId curve) {
	const std::string dbBaseFilename{"lime_user_management"};

	std::string dbFilenameAlice{dbBaseFilename};
	dbFilenameAlice.append(".alice.").append(CurveId2String(curve)).append(".sqlite3");

	remove(dbFilenameAlice.data()); // delete the database file if already exists

	lime_tester::events_counters_t counters={};
	int expected_success=0;
	int expected_fail=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGI <<"Insert Lime user failed : "<< anythingToSay.data() ;
					}
				};
	// we need a LimeManager
	auto Manager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);
	auto aliceDeviceName = lime_tester::makeRandomDeviceName("alice.");

	std::vector<lime::CurveId> algos{curve};
	try {
		/*Check if Alice exists in the database */
		BC_ASSERT_FALSE(Manager->is_user(*aliceDeviceName, algos));

		/* create a user in a fresh database */
		Manager->create_user(*aliceDeviceName, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		if (counters.operation_failed == 1) return; // skip the end of the test if we can't do this

		/*Check if Alice exists in the database, it will also load it */
		BC_ASSERT_TRUE(Manager->is_user(*aliceDeviceName, algos));

		// Get alice x3dh server url
		BC_ASSERT_TRUE(Manager->get_x3dhServerUrl(DeviceId(*aliceDeviceName, curve)) == lime_tester::test_x3dh_default_server);

		// Set the X3DH URL server to something else and check it worked
		Manager->set_x3dhServerUrl(*aliceDeviceName, algos, "https://testing.testing:12345");
		BC_ASSERT_TRUE(Manager->get_x3dhServerUrl(DeviceId(*aliceDeviceName, curve)) == "https://testing.testing:12345");
		// Force a reload of data from local storage just to be sure the modification was perform correctly
		Manager = nullptr;
		Manager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);
		BC_ASSERT_TRUE(Manager->is_user(*aliceDeviceName, algos)); // Check again just after LimeManager reload that Alice is in local storage
		BC_ASSERT_TRUE(Manager->get_x3dhServerUrl(DeviceId(*aliceDeviceName, curve)) == "https://testing.testing:12345");
		// Try to create the same user in the same data base, should fail
		Manager->create_user(*aliceDeviceName, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_fail,lime_tester::wait_for_timeout));
		// Check the second creation was ignored and that our user sticks to the old server
		BC_ASSERT_TRUE(Manager->get_x3dhServerUrl(DeviceId(*aliceDeviceName, curve)) == "https://testing.testing:12345");
		// Set it back to the regular one to be able to complete the test
		Manager->set_x3dhServerUrl(*aliceDeviceName, algos, lime_tester::test_x3dh_default_server);
		// Check a non existent user is not found
		BC_ASSERT_FALSE(Manager->is_user("bob", algos));
		// delete Alice, wait for callback confirmation from x3dh server
		Manager->delete_user(DeviceId(*aliceDeviceName, curve), callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
	} catch (BctbxException &e) {
		LIME_LOGE <<e;
		BC_FAIL("Delete Lime user raised exception");
		return;
	}
	bool gotExpectedException = false;
	auto savedCounters = counters;
	try {
		// delete bob which is not there, it shall raise an exception and never get to the callback
		Manager->delete_user(DeviceId("bob", curve), callback);
	} catch (BctbxException &) {
		gotExpectedException = true;
	}
	if (!gotExpectedException) {
		// we didn't got any exception on trying to delete bob from DB while he isn't theee
		BC_FAIL("No exception arised when deleting inexistent user from DB");
		return;
	}
	BC_ASSERT_FALSE(lime_tester::wait_for(bc_stack,&counters.operation_failed,savedCounters.operation_failed+1,lime_tester::wait_for_timeout/2)); // give we few seconds to possible call to the callback(it shall not occurs
	// check we didn't land into callback as any call to it will modify the counters
	BC_ASSERT_TRUE(counters == savedCounters);


	/* Create Alice again */
	try {
		Manager->create_user(*aliceDeviceName, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// create another manager with a fresh DB
		std::string dbFilenameAliceTmp{dbFilenameAlice};
		dbFilenameAliceTmp.append(".tmp.sqlite3");

		// create a manager and try to create alice again, it shall pass local creation(db is empty) but server shall reject it
		auto ManagerTmp = make_unique<LimeManager>(dbFilenameAliceTmp, X3DHServerPost);

		ManagerTmp->create_user(*aliceDeviceName, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_failed,counters.operation_failed+1,lime_tester::wait_for_timeout)); // wait on this one but we shall get a fail from server

		/* Clean DB */
		if (cleanDatabase) {
			// delete Alice, wait for callback confirmation from server
			Manager->delete_user(DeviceId(*aliceDeviceName, curve), callback);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
			remove(dbFilenameAlice.data()); // delete the database file
			remove(dbFilenameAliceTmp.data()); // delete the database file
		}
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}
}

static void user_management(void) {
#ifdef EC25519_ENABLED
	user_management_test(lime::CurveId::c25519);
#endif
#ifdef EC448_ENABLED
	user_management_test(lime::CurveId::c448);
#endif
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	user_management_test(lime::CurveId::c25519k512);

	user_management_test(lime::CurveId::c25519mlk512);
#endif
#ifdef EC448_ENABLED
	user_management_test(lime::CurveId::c448mlk1024);
#endif
#endif
}

/**
 * Scenario:
 *  - Create a user in local DB but do not send any message to the X3DH server
 *  - Check the user is there and inactive then delete it from local only
 *  - Create the same user with different keys on both local and server to check server didn't get it in the first place
 *  - Clean DBs (local and remote)
 *  - Repeat the previous 3 steps but let the message reach the server this time. At the second creation we shall get a fail from server as we are creating again a user but with different keys
 *  - Clean DBs (local and remote)
 *  - Create a local user but do not send anything to the server
 *  - Create it again letting the message go, it shall be a success
 *  - Clean DBs (local and remote)
 *  - Create a user but discard the response from server (so it is stored as inactive)
 *  - Create it again with normal connectivity, server is happy as we get the same Ik and SPk, user is activated on local base
 */
static void user_registration_failure_test(const lime::CurveId curve) {
	const std::string dbBaseFilename{"lime_user_registration_failure"};

	std::string dbFilenameAlice{dbBaseFilename};
	dbFilenameAlice.append(".alice.").append(CurveId2String(curve)).append(".sqlite3");

	remove(dbFilenameAlice.data()); // delete the database file if already exists

	lime_tester::events_counters_t counters={};
	int expected_success=0;
	int expected_failed=0;
	// reset the global setting for Http Link
	httpLink = HttpLinkStatus::ok;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGI <<"Insert Lime user failed : "<< anythingToSay.data() ;
					}
				};
	// we need a LimeManager
	auto Manager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost_Failing_Simulation);
	auto aliceDeviceId = lime_tester::makeRandomDeviceName("alice.");
	std::vector<lime::CurveId> algos{curve};

	try {
		/* create a user in a fresh database but discard the message to server */
		httpLink = HttpLinkStatus::sending_fail;
		Manager->create_user(*aliceDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_FALSE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success+1,lime_tester::wait_for_timeout));
		BC_ASSERT_TRUE(counters.operation_failed == expected_failed); // We shall have no failure either, the server never answer so the callback is never called (in a real situation the timeout on answer may be forwarded to the lime engine)
		Manager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost_Failing_Simulation); // reload manager from db after the timeout
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}

	bool gotExpectedException = false;
	long int Uid=0;
	try {
		/* now we have the user in local base but not active and not in remote, lets check that */
		/* load alice from DB(using insider functions) : user is not active, we shall get an exception */
		auto localStorage = std::unique_ptr<lime::Db>(new lime::Db(dbFilenameAlice));
		auto curve = CurveId::unset;

		localStorage->load_LimeUser(DeviceId(*aliceDeviceId, curve), Uid, lime_tester::test_x3dh_default_server); // this one will throw an exception if user is not found, just let it rise
	} catch (BctbxException &) {
		gotExpectedException = true;
	}
	BC_ASSERT(Uid == 0); // when user is not active, the db::load_LimeUser set the Uid to -1 before generating the exception
	BC_ASSERT(gotExpectedException == true);

	try {
		// Delete local user from base and create it letting it flow to the server
		httpLink = HttpLinkStatus::sending_fail; // make sure our delete never reach the server
		Manager->delete_user(DeviceId(*aliceDeviceId, curve), callback);
		BC_ASSERT_FALSE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success+1,lime_tester::wait_for_timeout));
		Manager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost_Failing_Simulation); // reload manager from db after the timeout
		BC_ASSERT_TRUE(counters.operation_failed == expected_failed);
		httpLink = HttpLinkStatus::ok; // restore htpp link
		Manager->create_user(*aliceDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback); // This one has different keys but the same user Id
		// if this userId was already registered on server(with the old Ik) we would have an error
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		// and delete it again
		Manager->delete_user(DeviceId(*aliceDeviceId, curve), callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));


		// Start again but this time let the message flow to the server, just block the answer
		httpLink = HttpLinkStatus::reception_fail;
		Manager->create_user(*aliceDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_FALSE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success+1,lime_tester::wait_for_timeout));
		BC_ASSERT_TRUE(counters.operation_failed == expected_failed); // We shall have no failure either, the server never answer so the callback is never called (in a real situation the timeout on answer may be forwarded to the lime engine)
		Manager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost_Failing_Simulation); // reload manager from db after the timeout

		// Now we have the user inactive in local but set on server. Delete the local one
		httpLink = HttpLinkStatus::sending_fail; // make sure our delete never reach the server
		Manager->delete_user(DeviceId(*aliceDeviceId, curve), callback);
		BC_ASSERT_FALSE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success+1,lime_tester::wait_for_timeout));
		Manager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost_Failing_Simulation); // reload manager from db after the timeout
		httpLink = HttpLinkStatus::ok; // restore htpp link
		Manager->create_user(*aliceDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback); // This one has different keys but the same user Id
		// this userId is already registered on server(with the old Ik) we shall have an error -> the error is deleted from local base
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_failed,++expected_failed,lime_tester::wait_for_timeout));
		// Now the user is on the server but no more in local, to delete it from server, we must create it in local only and then delete
		httpLink = HttpLinkStatus::sending_fail; // make sure our delete never reach the server
		Manager->create_user(*aliceDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_FALSE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success+1,lime_tester::wait_for_timeout));
		BC_ASSERT_TRUE(counters.operation_failed == expected_failed); // We shall have no failure either, the server never answer so the callback is never called (in a real situation the timeout on answer may be forwarded to the lime engine)
		Manager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost_Failing_Simulation); // reload manager from db after the timeout
		httpLink = HttpLinkStatus::ok; // restore htpp link
		Manager->delete_user(DeviceId(*aliceDeviceId, curve), callback); // delete from local and remote
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// Create again on local only, and then call the create_user again with the same id, with restored connectivity to get an active user
		httpLink = HttpLinkStatus::sending_fail; // make sure our delete never reach the server
		Manager->create_user(*aliceDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_FALSE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success+1,lime_tester::wait_for_timeout));
		BC_ASSERT_TRUE(counters.operation_failed == expected_failed); // We shall have no failure either, the server never answer so the callback is never called (in a real situation the timeout on answer may be forwarded to the lime engine)
		Manager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost_Failing_Simulation); // reload manager from db after the timeout
		// create one more time, it shall this time publish on the server without error
		httpLink = HttpLinkStatus::ok; // restore htpp link
		Manager->create_user(*aliceDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// Clean
		Manager->delete_user(DeviceId(*aliceDeviceId, curve), callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

		// Same than previous but this time we block the server answer and then call again create
		httpLink = HttpLinkStatus::reception_fail; // make sure our delete never reach the server
		Manager->create_user(*aliceDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_FALSE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success+1,lime_tester::wait_for_timeout));
		BC_ASSERT_TRUE(counters.operation_failed == expected_failed); // We shall have no failure either, the server never answer so the callback is never called (in a real situation the timeout on answer may be forwarded to the lime engine)
		Manager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost_Failing_Simulation); // reload manager from db after the timeout
		// create one more time, it shall this time publish on the server without error
		httpLink = HttpLinkStatus::ok; // restore htpp link
		Manager->create_user(*aliceDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));

	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}

	try {
		if (cleanDatabase) {
			// delete Alice, wait for callback confirmation from server
			httpLink = HttpLinkStatus::ok; // restore http link functionality
			Manager->delete_user(DeviceId(*aliceDeviceId, curve), callback);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
			remove(dbFilenameAlice.data()); // delete the database file
		}
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}
}

static void user_registration_failure(void) {
#ifdef EC25519_ENABLED
	user_registration_failure_test(lime::CurveId::c25519);
#endif
#ifdef EC448_ENABLED
	user_registration_failure_test(lime::CurveId::c448);
#endif
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	user_registration_failure_test(lime::CurveId::c25519k512);

	user_registration_failure_test(lime::CurveId::c25519mlk512);
#endif
#ifdef EC448_ENABLED
	user_registration_failure_test(lime::CurveId::c448mlk1024);
#endif
#endif
}

// For multithread test : a mailbox system to post and fetch messages
struct mth_message {
	std::vector<uint8_t> DRmessage;
	std::vector<uint8_t> cipherMessage;
	std::vector<uint8_t> plainMessage; // to check after decryption if we're good
	std::string senderId;

	mth_message() {};
	mth_message(const std::vector<uint8_t> &DRmessage, const std::vector<uint8_t> &cipherMessage, const std::vector<uint8_t> &plainMessage, const std::string senderId) :
		DRmessage{DRmessage}, cipherMessage{cipherMessage}, plainMessage{plainMessage}, senderId{senderId} {};
};

struct mth_mailbox {
	std::mutex b_mutex;
	std::deque<mth_message> box;
	std::string owner;
	const int expectedMessageCount; // set at creation, expected message number to transit
	int messageCount; // number of message fetched

	bool fetch(mth_message &m) {
		std::lock_guard<std::mutex> lock(b_mutex);
		if (box.size()>0) {
			m = box.back();
			box.pop_back();
			messageCount++;
			return true;
		}
		return false;
	}

	void post(mth_message m) {
		std::lock_guard<std::mutex> lock(b_mutex);
		box.push_front(m);
	}

	bool done() { // return true if we already processed the expected number of messages
		std::lock_guard<std::mutex> lock(b_mutex);
		return (messageCount>=expectedMessageCount);
	}

	mth_mailbox(std::string &owner, int expectedMessageCount) : owner{owner}, expectedMessageCount{expectedMessageCount}, messageCount{0} {}
};


struct manager_thread_arg {
	std::shared_ptr<LimeManager> manager;
	size_t userIndex; // index of self user in the user lists
	std::array<std::string,4> userlist; // list of all users
	lime::CurveId curve;
	std::shared_ptr<std::recursive_mutex> belle_sip_mutex; // A mutex to manage belle_sip http stack access
	std::shared_ptr<std::map<std::string, std::shared_ptr<mth_mailbox>>> mailbox; // mailbox system to post and fetch messages

	manager_thread_arg(std::shared_ptr<LimeManager> manager, const uint8_t userIndex, const std::array<std::string, 4> &userlist, const lime::CurveId curve, std::shared_ptr<std::recursive_mutex> belle_sip_mutex, std::shared_ptr<std::map<std::string, std::shared_ptr<mth_mailbox>>> mailbox)
		: manager{manager}, userIndex{userIndex}, userlist(userlist),
		curve{curve}, belle_sip_mutex{belle_sip_mutex},
		mailbox(mailbox)	{};
};

static constexpr size_t test_multithread_message_number = 10;

static void lime_multithread_decrypt_thread(manager_thread_arg thread_arg) {

	auto pool = belle_sip_object_pool_push();

	// RNG uniform between 0 and 400 ms
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<> dis(0, 400);

	try {
		auto localDeviceId = thread_arg.userlist[thread_arg.userIndex];
		auto box = thread_arg.mailbox->find(localDeviceId)->second;

		// loop on all patterns available
		while (box->done() == false) {
			// wait for a random period betwwen 0 and 400 ms
			std::this_thread::sleep_for(std::chrono::milliseconds(dis(gen)));
			// Fetch mailbox
			mth_message m{};
			int fetchMax = 1+dis(gen)/200;
			int fetched=0;
			while (box->fetch(m) && fetched<fetchMax) {
				std::vector<uint8_t> receivedMessage{};
				BC_ASSERT_TRUE(thread_arg.manager->decrypt(localDeviceId, "friends", m.senderId, m.DRmessage, m.cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
				BC_ASSERT_TRUE(receivedMessage == m.plainMessage);
				fetched++;
			}
		}

	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}

	belle_sip_object_unref(pool);
}

static void lime_multithread_encrypt_thread(manager_thread_arg thread_arg) {
	lime_tester::events_counters_t counters={};
	int expected_success=0;
	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
		if (returnCode == lime::CallbackReturn::success) {
			counters.operation_success++;
		} else {
			counters.operation_failed++;
			LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
		}
	};

	auto pool = belle_sip_object_pool_push();
	// RNG uniform between 1 and 20 ms
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<> dis(1000, 20000);

	try {
		std::vector<lime::CurveId> algos{thread_arg.curve};
		// loop on all patterns available
		for (auto message=lime_tester::messages_pattern.cbegin(); message != lime_tester::messages_pattern.cbegin()+test_multithread_message_number; message++) {
			// wait for a random period betwwen 1 and 20 ms
			std::this_thread::sleep_for(std::chrono::microseconds(dis(gen)));
			// Encrypt to all but me
			auto enc = make_shared<lime::EncryptionContext>("friends", *message);
			for (size_t i=0; i<thread_arg.userlist.size(); i++) {
				if (i != thread_arg.userIndex) { // do not encrypt to myself
					enc->addRecipient(thread_arg.userlist[i]);
				}
			}

			auto localDeviceId = thread_arg.userlist[thread_arg.userIndex];
			auto mailbox = thread_arg.mailbox;
			// encrypt to all, use a generic "friends" id as recipient user id
			thread_arg.manager->encrypt(localDeviceId, algos, enc,
					[&counters, localDeviceId, mailbox, enc](lime::CallbackReturn returnCode, std::string anythingToSay) {
						if (returnCode == lime::CallbackReturn::success) {
							// Post the messages
							for (auto &recipient : enc->m_recipients) {
								mth_message m(recipient.DRmessage, enc->m_cipherMessage, enc->m_plainMessage, localDeviceId);
								auto search = mailbox->find(recipient.deviceId);
								if (search != mailbox->end()) {
									auto box = search->second;
									box->post(m);
								}
							}
							counters.operation_success++;
						} else {
							counters.operation_failed++;
							LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
						}
					});
			expected_success++;
			// make sure we process possible message incoming from X3DH server
			std::unique_lock<std::recursive_mutex> lock(*(thread_arg.belle_sip_mutex));
			belle_sip_stack_sleep(bc_stack,0);
			lock.unlock();
		}
		BC_ASSERT_TRUE(lime_tester::wait_for_mutex(bc_stack,&counters.operation_success, expected_success, lime_tester::wait_for_timeout, thread_arg.belle_sip_mutex));

	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}

	belle_sip_object_unref(pool);	
}

static void lime_multithread_create_thread(manager_thread_arg thread_arg) {
	lime_tester::events_counters_t counters={};
	int expected_success=0;
	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
		if (returnCode == lime::CallbackReturn::success) {
			counters.operation_success++;
		} else {
			counters.operation_failed++;
			LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
		}
	};

	auto pool = belle_sip_object_pool_push();

	try {
		std::vector<lime::CurveId> algos{thread_arg.curve};
		// Create the device
		auto deviceId = thread_arg.userlist[thread_arg.userIndex];
		thread_arg.manager->create_user(deviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for_mutex(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout, thread_arg.belle_sip_mutex));
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}

	belle_sip_object_unref(pool);	
}

static void lime_multithread_delete_thread(manager_thread_arg thread_arg) {
	lime_tester::events_counters_t counters={};
	int expected_success=0;
	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
		if (returnCode == lime::CallbackReturn::success) {
			counters.operation_success++;
		} else {
			counters.operation_failed++;
			LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
		}
	};

	auto pool = belle_sip_object_pool_push();

	try {
		// Delete the device
		thread_arg.manager->delete_user(DeviceId(thread_arg.userlist[thread_arg.userIndex], thread_arg.curve), callback);
		BC_ASSERT_TRUE(lime_tester::wait_for_mutex(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout, thread_arg.belle_sip_mutex));
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}

	belle_sip_object_unref(pool);	
}

static void lime_multithread_update_thread(manager_thread_arg thread_arg) {
	auto pool = belle_sip_object_pool_push();

	// RNG uniform between 25 and 100 ms
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<> dis(25, 100);
	std::uniform_int_distribution<> rnd_serverLimit(0, 4);

	lime_tester::events_counters_t counters={};
	int expected_success=0;
	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
			if (returnCode == lime::CallbackReturn::success) {
				counters.operation_success++;
			} else {
				counters.operation_failed++;
				LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
			}
		};

	try {
		auto localDeviceId = thread_arg.userlist[thread_arg.userIndex];
		auto box = thread_arg.mailbox->find(localDeviceId)->second;

		uint16_t serverLimit = 2;
		uint16_t batchSize = 2;

		std::vector<lime::CurveId> algos{thread_arg.curve};
		while (box->done() == false) { // use the mailbox to synchronise to the end of the decryption threads
			// wait for a random period betwwen 25 and 100 ms
			std::this_thread::sleep_for(std::chrono::milliseconds(dis(gen)));
			// Update
			thread_arg.manager->update(thread_arg.userlist[thread_arg.userIndex], algos, callback, serverLimit, batchSize);
			expected_success++;
			serverLimit+=rnd_serverLimit(gen); // improver server limit by a random 0 to 4
			// make sure we process possible message incoming from X3DH server
			std::unique_lock<std::recursive_mutex> lock(*(thread_arg.belle_sip_mutex));
			belle_sip_stack_sleep(bc_stack,0);
			lock.unlock();
		}
		BC_ASSERT_TRUE(lime_tester::wait_for_mutex(bc_stack,&counters.operation_success, expected_success, lime_tester::wait_for_timeout, thread_arg.belle_sip_mutex));

	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}

	belle_sip_object_unref(pool);
}



/*
 * Scenario:
 * - Create 2 managers
 * - In separated threads, each manager create 2 devices
 * - each device gets several threads to encrypt/decrypt/update
 */
static void lime_multithread_test(const lime::CurveId curve) {
	const std::string dbBaseFilename{"dbBaseFilename"};
	// create DB
	std::string dbFilenameAlice{dbBaseFilename};
	dbFilenameAlice.append(".alice.").append(CurveId2String(curve)).append(".sqlite3");
	std::string dbFilenameBob{dbBaseFilename};
	dbFilenameBob.append(".bob.").append(CurveId2String(curve)).append(".sqlite3");

	remove(dbFilenameAlice.data()); // delete the database file if already exists
	remove(dbFilenameBob.data()); // delete the database file if already exists

	auto belle_sip_mutex = make_shared<std::recursive_mutex>(); // a mutex for belle-sip operations, must be recursive a stack processing may trigger response reception wich may trigger message sending

	limeX3DHServerPostData X3DHServerPost_mutex([belle_sip_mutex](const std::string &url, const std::string &from, std::vector<uint8_t> &&message, const limeX3DHServerResponseProcess &responseProcess){
			std::lock_guard<std::recursive_mutex> lock(*belle_sip_mutex); // belle_sip_mutex is recursive so a thread can acquire the lock there even if it already got it from a stack processing
			X3DHServerPost(url, from, std::move(message), responseProcess);
			});


	try {
		// Generate the 4 devices Id we will use
		std::array<std::string,4> deviceList;
		deviceList[0] = *lime_tester::makeRandomDeviceName("alice.d1.");
		deviceList[1] = *lime_tester::makeRandomDeviceName("alice.d2.");
		deviceList[2] = *lime_tester::makeRandomDeviceName("bob.d1.");
		deviceList[3] = *lime_tester::makeRandomDeviceName("bob.d2.");
		std::deque<std::thread> activeThreads{};

		// get a mailbox for each of them, same index
		auto mailbox = make_shared<std::map<std::string, std::shared_ptr<mth_mailbox>>>();
		auto expectedMessageCount = 9*test_multithread_message_number; // each recipient shall get 9 times the number of expedited messages by each encryption thread
		mailbox->emplace(std::make_pair(deviceList[0], make_shared<mth_mailbox>(deviceList[0], (int)expectedMessageCount)));
		mailbox->emplace(std::make_pair(deviceList[1], make_shared<mth_mailbox>(deviceList[1], (int)expectedMessageCount)));
		mailbox->emplace(std::make_pair(deviceList[2], make_shared<mth_mailbox>(deviceList[2], (int)expectedMessageCount)));
		mailbox->emplace(std::make_pair(deviceList[3], make_shared<mth_mailbox>(deviceList[3], (int)expectedMessageCount)));

		// create Manager
		auto aliceManager = std::make_shared<LimeManager>(dbFilenameAlice, X3DHServerPost_mutex);
		auto bobManager = std::make_shared<LimeManager>(dbFilenameBob, X3DHServerPost_mutex);

		// Create arguments to thread, as local variable, they are copy passed to threads
		std::list<manager_thread_arg> devArg;
		devArg.emplace_back(aliceManager, 0, deviceList, curve, belle_sip_mutex, mailbox);
		devArg.emplace_back(aliceManager, 1, deviceList, curve, belle_sip_mutex, mailbox);
		devArg.emplace_back(bobManager, 2, deviceList, curve, belle_sip_mutex, mailbox);
		devArg.emplace_back(bobManager, 3, deviceList, curve, belle_sip_mutex, mailbox);
		
		// create device
		for (const auto &arg : devArg) {
			activeThreads.emplace_back(lime_multithread_create_thread, arg);
		}

		// wait for the threads to complete
		for (auto &t : activeThreads) {
			t.join();
		}
		activeThreads.clear();

		// encrypt, three encryption threads per user
		for (auto i=0; i<3; i++) {
			for (const auto &arg : devArg) {
				activeThreads.emplace_back(lime_multithread_encrypt_thread, arg);
			}
		}

		// decrypt, two decryption threads per user
		for (auto i=0; i<2; i++) {
			for (const auto &arg : devArg) {
				activeThreads.emplace_back(lime_multithread_decrypt_thread, arg);
			}
		}

		//update, two threads per user
		for (auto i=0; i<2; i++) {
			for (const auto &arg : devArg) {
				activeThreads.emplace_back(lime_multithread_update_thread, arg);
			}
		}
		// wait for the threads to complete
		for (auto &t : activeThreads) {
			t.join();
		}
		activeThreads.clear();

		// delete devices
		for (const auto &arg : devArg) {
			activeThreads.emplace_back(lime_multithread_delete_thread, arg);
		}

		// wait for the threads to complete
		for (auto &t : activeThreads) {
			t.join();
		}
		activeThreads.clear();

		if (cleanDatabase) {
			remove(dbFilenameAlice.data()); // delete the database file if already exists
			remove(dbFilenameBob.data()); // delete the database file if already exists
		}
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}
}

static void lime_multithread(void) {
	for (size_t i=0; i<3; i++) { // loop several times as the first messages sending/receiving are the most susceptible to bump into problems
#ifdef EC25519_ENABLED
	lime_multithread_test(lime::CurveId::c25519);
#endif
#ifdef EC448_ENABLED
	lime_multithread_test(lime::CurveId::c448);
#endif
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	lime_multithread_test(lime::CurveId::c25519k512);

	lime_multithread_test(lime::CurveId::c25519mlk512);
#endif
#ifdef EC448_ENABLED
	lime_multithread_test(lime::CurveId::c448mlk1024);
#endif
#endif
	}
}

/*
 * Scenario
 * - Establish a session between Alice and Bob
 * - Alice encrypts to Bob, check it does not holds a X3DH init message
 * - Alice cancel session with bob
 * - Alice encrypts to Bob, check it does holds a X3DH init message
 * - Bob encrypts a message to Alice using the old session
 * - Bob and Alice decrypt them all (including the one encrypted with now stale session)
 */
static void lime_session_cancel_test(const lime::CurveId curve, const std::string &dbBaseFilename, bool continuousSession=true) {
	std::string dbFilenameAlice;
	std::shared_ptr<std::string> aliceDeviceId;
	std::unique_ptr<LimeManager> aliceManager;
	std::string dbFilenameBob;
	std::shared_ptr<std::string> bobDeviceId;
	std::unique_ptr<LimeManager> bobManager;

	lime_tester::events_counters_t counters={};
	int expected_success=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};

	try {
		std::vector<lime::CurveId> algos{curve};
		lime_session_establishment(algos, dbBaseFilename,
					dbFilenameAlice, aliceDeviceId, aliceManager,
					dbFilenameBob, bobDeviceId, bobManager);

		/* Alice encrypts to Bob */
		auto encAlice = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[0]);
		encAlice->addRecipient(*bobDeviceId);
		aliceManager->encrypt(*aliceDeviceId, algos, encAlice, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		BC_ASSERT_FALSE(lime_tester::DR_message_holdsX3DHInit(encAlice->m_recipients[0].DRmessage)); // no X3DH init as this session is fully establsihed

		/* force reset so the active session is not in cache anymore */
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		/* Alice stale session with Bob */
		aliceManager->stale_sessions(*aliceDeviceId, algos, *bobDeviceId);

		/* Alice encrypts to Bob again */
		auto encAlice2 = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[1]);
		encAlice2->addRecipient(*bobDeviceId);
		aliceManager->encrypt(*aliceDeviceId, algos, encAlice2, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		BC_ASSERT_TRUE(lime_tester::DR_message_holdsX3DHInit(encAlice2->m_recipients[0].DRmessage)); // X3DH init message in, it is a new session

		/* Bob encrypts to Alice, he uses the old session staled by Alice */
		auto encBob = make_shared<lime::EncryptionContext>("alice", lime_tester::messages_pattern[2]);
		encBob->addRecipient(*aliceDeviceId);
		bobManager->encrypt(*bobDeviceId, algos, encBob, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		BC_ASSERT_FALSE(lime_tester::DR_message_holdsX3DHInit(encBob->m_recipients[0].DRmessage)); // no X3DH init as this session is fully establsihed

		/* Everyone decrypt */
		std::vector<uint8_t> receivedMessage{};
		BC_ASSERT_TRUE(bobManager->decrypt(*bobDeviceId, "bob", *aliceDeviceId, encAlice->m_recipients[0].DRmessage, encAlice->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[0]);

		receivedMessage.clear();
		BC_ASSERT_TRUE(bobManager->decrypt(*bobDeviceId, "bob", *aliceDeviceId, encAlice2->m_recipients[0].DRmessage, encAlice2->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[1]);

		receivedMessage.clear();
		BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDeviceId, "alice", *bobDeviceId, encBob->m_recipients[0].DRmessage, encBob->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[2]);

		// cleaning
		if (cleanDatabase) {
			aliceManager->delete_user(DeviceId(*aliceDeviceId, curve), callback);
			bobManager->delete_user(DeviceId(*bobDeviceId, curve), callback);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success+2,lime_tester::wait_for_timeout));
			remove(dbFilenameAlice.data());
			remove(dbFilenameBob.data());
		}
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}
}

static void lime_session_cancel(void) {
#ifdef EC25519_ENABLED
	lime_session_cancel_test(lime::CurveId::c25519, "lime_session_cancel");
	lime_session_cancel_test(lime::CurveId::c25519, "lime_session_cancel_clean", false);
#endif
#ifdef EC448_ENABLED
	lime_session_cancel_test(lime::CurveId::c448, "lime_session_cancel");
	lime_session_cancel_test(lime::CurveId::c448, "lime_session_cancel_clean", false);
#endif
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	lime_session_cancel_test(lime::CurveId::c25519k512, "lime_session_cancel");
	lime_session_cancel_test(lime::CurveId::c25519k512, "lime_session_cancel_clean", false);

	lime_session_cancel_test(lime::CurveId::c25519mlk512, "lime_session_cancel");
	lime_session_cancel_test(lime::CurveId::c25519mlk512, "lime_session_cancel_clean", false);
#endif
#ifdef EC448_ENABLED
	lime_session_cancel_test(lime::CurveId::c448mlk1024, "lime_session_cancel");
	lime_session_cancel_test(lime::CurveId::c448mlk1024, "lime_session_cancel_clean", false);
#endif
#endif
}

/*
 * Scenario
 * - Establish a session between Alice and Bob
 * - Alice encrypts to Bob, check it does not holds a X3DH init message
 * - Alice stale session with bob
 * - Alice encrypts to Bob, check it does holds a X3DH init message. Bob decrypts
 * - Check Alice and Bob have two DR sessions, one active one not
 * - Update Alice and Bob, and check again the sessions
 * - fast forward Alice and Bob by DRSession_limboTime_days -1 days
 * - Update Alice and Bob, and check again the sessions
 * - Alice stale session with bob
 * - Alice encrypts to Bob, check it does holds a X3DH init message. Bob decrypts
 * - Check Alice and Bob have three DR sessions, one active two not
 * - fast forward Alice and Bob by 2 days so we shall have one session inactive session older than DRSession_limboTime_days, one younger and one active
 * - Update Alice and Bob, and check again the sessions : the older one shall have been cleaned
 */
static void lime_DR_session_clean_test(const lime::CurveId curve, const std::string &dbBaseFilename, bool continuousSession=true) {
	std::string dbFilenameAlice;
	std::shared_ptr<std::string> aliceDeviceId;
	std::unique_ptr<LimeManager> aliceManager;
	std::string dbFilenameBob;
	std::shared_ptr<std::string> bobDeviceId;
	std::unique_ptr<LimeManager> bobManager;

	lime_tester::events_counters_t counters={};
	int expected_success=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};

	try {
		std::vector<lime::CurveId> algos{curve};
		lime_session_establishment(algos, dbBaseFilename,
					dbFilenameAlice, aliceDeviceId, aliceManager,
					dbFilenameBob, bobDeviceId, bobManager);

		/* Alice encrypts to Bob */
		auto encAlice = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[0]);
		encAlice->addRecipient(*bobDeviceId);
		aliceManager->encrypt(*aliceDeviceId, algos, encAlice, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		BC_ASSERT_FALSE(lime_tester::DR_message_holdsX3DHInit(encAlice->m_recipients[0].DRmessage)); // no X3DH init as this session is fully established

		/* force reset so the active session is not in cache anymore */
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		/* Alice stale session with Bob */
		aliceManager->stale_sessions(*aliceDeviceId, algos, *bobDeviceId);

		/* force reset so the active session is not in cache anymore */
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		/* Alice encrypts to Bob again: she creates a new session */
		auto encAlice2 = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[1]);
		encAlice2->addRecipient(*bobDeviceId);
		aliceManager->encrypt(*aliceDeviceId, algos, encAlice2, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		BC_ASSERT_TRUE(lime_tester::DR_message_holdsX3DHInit(encAlice2->m_recipients[0].DRmessage)); // X3DH init message in, it is a new session
		/* Bob decrypts: he creates a new session staling the old one */
		std::vector<uint8_t> receivedMessage{};
		BC_ASSERT_TRUE(bobManager->decrypt(*bobDeviceId, "bob", *aliceDeviceId, encAlice2->m_recipients[0].DRmessage, encAlice2->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[1]);

		/* force reset so the active session is not in cache anymore */
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		/* Check Alice and Bob db holds 2 DR session, one active, one not */
		std::vector<long int> sessionsId;
		BC_ASSERT_TRUE(lime_tester::get_DRsessionsId(dbFilenameAlice, *aliceDeviceId, *bobDeviceId, sessionsId) != 0); // don't return 0 -> one session is active
		BC_ASSERT_TRUE(sessionsId.size() == 2); // We have a total of 2 sessions
		sessionsId.clear();
		BC_ASSERT_TRUE(lime_tester::get_DRsessionsId(dbFilenameAlice, *aliceDeviceId, *bobDeviceId, sessionsId) != 0);
		BC_ASSERT_TRUE(sessionsId.size() == 2);

		// Update Alice and Bob
		aliceManager->update(*aliceDeviceId, algos, callback, 0, lime_tester::OPkInitialBatchSize);
		bobManager->update(*bobDeviceId, algos, callback, 0, lime_tester::OPkInitialBatchSize);
		expected_success+=2;
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success,lime_tester::wait_for_timeout));

		/* force reset so the active session is not in cache anymore */
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		/* Check Alice and Bob db holds 2 DR session, one active, one not */
		sessionsId.clear();
		BC_ASSERT_TRUE(lime_tester::get_DRsessionsId(dbFilenameAlice, *aliceDeviceId, *bobDeviceId, sessionsId) != 0); // don't return 0 -> one session is active
		BC_ASSERT_TRUE(sessionsId.size() == 2); // We have a total of 2 sessions
		sessionsId.clear();
		BC_ASSERT_TRUE(lime_tester::get_DRsessionsId(dbFilenameAlice, *aliceDeviceId, *bobDeviceId, sessionsId) != 0);
		BC_ASSERT_TRUE(sessionsId.size() == 2);

		// forward time by DRSession_limboTime_days - 1
		lime_tester::forwardTime(dbFilenameAlice, lime::settings::DRSession_limboTime_days - 1);
		lime_tester::forwardTime(dbFilenameBob, lime::settings::DRSession_limboTime_days - 1);

		// Update Alice and Bob
		aliceManager->update(*aliceDeviceId, algos, callback, 0, lime_tester::OPkInitialBatchSize);
		bobManager->update(*bobDeviceId, algos, callback, 0, lime_tester::OPkInitialBatchSize);
		expected_success+=2;
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success,lime_tester::wait_for_timeout));

		/* Check Alice and Bob db holds 2 DR session, one active, one not */
		sessionsId.clear();
		BC_ASSERT_TRUE(lime_tester::get_DRsessionsId(dbFilenameAlice, *aliceDeviceId, *bobDeviceId, sessionsId) != 0); // don't return 0 -> one session is active
		BC_ASSERT_TRUE(sessionsId.size() == 2); // We have a total of 2 sessions
		sessionsId.clear();
		BC_ASSERT_TRUE(lime_tester::get_DRsessionsId(dbFilenameAlice, *aliceDeviceId, *bobDeviceId, sessionsId) != 0);
		BC_ASSERT_TRUE(sessionsId.size() == 2);

		/* force reset so the active session is not in cache anymore */
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		/* Alice stale session with Bob */
		aliceManager->stale_sessions(*aliceDeviceId, algos, *bobDeviceId);

		/* Alice encrypts to Bob again: she creates a new session */
		encAlice2 = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[2]);
		encAlice2->addRecipient(*bobDeviceId);
		aliceManager->encrypt(*aliceDeviceId, algos, encAlice2, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,++expected_success,lime_tester::wait_for_timeout));
		BC_ASSERT_TRUE(lime_tester::DR_message_holdsX3DHInit(encAlice2->m_recipients[0].DRmessage)); // X3DH init message in, it is a new session
		/* Bob decrypts: he creates a new session staling the old one */
		receivedMessage.clear();
		BC_ASSERT_TRUE(bobManager->decrypt(*bobDeviceId, "bob", *aliceDeviceId, encAlice2->m_recipients[0].DRmessage, encAlice2->m_cipherMessage, receivedMessage) != lime::PeerDeviceStatus::fail);
		BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[2]);

		/* Check Alice and Bob db holds 3 DR session, one active, two not */
		sessionsId.clear();
		BC_ASSERT_TRUE(lime_tester::get_DRsessionsId(dbFilenameAlice, *aliceDeviceId, *bobDeviceId, sessionsId) != 0); // don't return 0 -> one session is active
		BC_ASSERT_TRUE(sessionsId.size() == 3); // We have a total of 2 sessions
		sessionsId.clear();
		BC_ASSERT_TRUE(lime_tester::get_DRsessionsId(dbFilenameAlice, *aliceDeviceId, *bobDeviceId, sessionsId) != 0);
		BC_ASSERT_TRUE(sessionsId.size() == 3);

		// forward time by 2 days
		lime_tester::forwardTime(dbFilenameAlice, 2);
		lime_tester::forwardTime(dbFilenameBob, 2);

		/* force reset so the active session is not in cache anymore */
		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// Update Alice and Bob
		aliceManager->update(*aliceDeviceId, algos, callback, 0, lime_tester::OPkInitialBatchSize);
		bobManager->update(*bobDeviceId, algos, callback, 0, lime_tester::OPkInitialBatchSize);
		expected_success+=2;
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success,lime_tester::wait_for_timeout));

		/* Check Alice and Bob db holds 2 DR session, one active, one not as the update has deleted the first staled session */
		sessionsId.clear();
		BC_ASSERT_TRUE(lime_tester::get_DRsessionsId(dbFilenameAlice, *aliceDeviceId, *bobDeviceId, sessionsId) != 0); // don't return 0 -> one session is active
		BC_ASSERT_TRUE(sessionsId.size() == 2); // We have a total of 2 sessions
		sessionsId.clear();
		BC_ASSERT_TRUE(lime_tester::get_DRsessionsId(dbFilenameAlice, *aliceDeviceId, *bobDeviceId, sessionsId) != 0);
		BC_ASSERT_TRUE(sessionsId.size() == 2);

		// cleaning
		if (cleanDatabase) {
			aliceManager->delete_user(DeviceId(*aliceDeviceId, curve), callback);
			bobManager->delete_user(DeviceId(*bobDeviceId, curve), callback);
			BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success+2,lime_tester::wait_for_timeout));
			remove(dbFilenameAlice.data());
			remove(dbFilenameBob.data());
		}
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
	}
}

static void lime_DR_session_clean(void) {
#ifdef EC25519_ENABLED
	lime_DR_session_clean_test(lime::CurveId::c25519, "lime_DR_session_clean");
	lime_DR_session_clean_test(lime::CurveId::c25519, "lime_DR_session_clean_clean", false);
#endif
#ifdef EC448_ENABLED
	lime_DR_session_clean_test(lime::CurveId::c448, "lime_DR_session_clean");
	lime_DR_session_clean_test(lime::CurveId::c448, "lime_DR_session_clean_clean", false);
#endif
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	lime_DR_session_clean_test(lime::CurveId::c25519k512, "lime_DR_session_clean");
	lime_DR_session_clean_test(lime::CurveId::c25519k512, "lime_DR_session_clean_clean", false);

	lime_DR_session_clean_test(lime::CurveId::c25519mlk512, "lime_DR_session_clean");
	lime_DR_session_clean_test(lime::CurveId::c25519mlk512, "lime_DR_session_clean_clean", false);
#endif
#ifdef EC448_ENABLED
	lime_DR_session_clean_test(lime::CurveId::c448mlk1024, "lime_DR_session_clean");
	lime_DR_session_clean_test(lime::CurveId::c448mlk1024, "lime_DR_session_clean_clean", false);
#endif
#endif
}

/** Scenario:
 * - Alice and Bob create sessions
 * - Alice sends a message to Bob :it holds a pk as it is the first one
 * - Bob decrypts and reply: it holds a pk as it is the session creation on Bob's side
 * - Alice decrypts so she gets Bob's new pk
 * - Alice encrypts minSymmetricChainSize-1 to Bob, none of them holds a Pk as she does not perform an asymmetric ratchet. Bob decrypts all of them
 * - Alice encrypts one more message to Bob, she performs an asymmetric ratchet and the message holds a pk. Bob decrypts
 * - Alice encrypts another message, she cannot know for sure that Bob got her previous one so she keeps including her Pk in it. Bob decrypts
 * - Bob replies, he does not perform an asymmetric ratchet so no pk in his message
 * - Alice decrypts so now she nows that Bob got her public key
 * - Alice encrypts ot Bob, no pk in it
 * - Bob decrypts and reply, no pk in his message
 * - Bob travels in time by one day
 * - Bob encrypt again: he performs an asymmetric ratchet so his messages should not hold a Pk
 */
#ifdef HAVE_BCTBXPQ
static bool lime_kem_asymmetric_ratchet_test(const lime::CurveId curve, const std::string &dbBaseFilename, bool continuousSession=true) {
	std::string dbFilenameAlice;
	std::shared_ptr<std::string> aliceDeviceId;
	std::unique_ptr<LimeManager> aliceManager;
	std::string dbFilenameBob;
	std::shared_ptr<std::string> bobDeviceId;
	std::unique_ptr<LimeManager> bobManager;

	lime_tester::events_counters_t counters={};
	int expected_success=0;

	limeCallback callback = [&counters](lime::CallbackReturn returnCode, std::string anythingToSay) {
					if (returnCode == lime::CallbackReturn::success) {
						counters.operation_success++;
					} else {
						counters.operation_failed++;
						LIME_LOGE<<"Lime operation failed : "<<anythingToSay;
					}
				};

	try {
		std::vector<lime::CurveId> algos{curve};
		// create DB
		dbFilenameAlice = dbBaseFilename;
		dbFilenameAlice.append(".alice.").append(CurveId2String(curve)).append(".sqlite3");
		dbFilenameBob = dbBaseFilename;
		dbFilenameBob.append(".bob.").append(CurveId2String(curve)).append(".sqlite3");

		remove(dbFilenameAlice.data()); // delete the database file if already exists
		remove(dbFilenameBob.data()); // delete the database file if already exists

		// create Manager and device for alice
		aliceManager = make_unique<LimeManager>(dbFilenameAlice, X3DHServerPost);
		aliceDeviceId = lime_tester::makeRandomDeviceName("alice.d.");
		aliceManager->create_user(*aliceDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));

		// Create manager and device for bob
		bobManager = make_unique<LimeManager>(dbFilenameBob, X3DHServerPost);
		bobDeviceId = lime_tester::makeRandomDeviceName("bob.d");
		bobManager->create_user(*bobDeviceId, algos, lime_tester::test_x3dh_default_server, lime_tester::OPkInitialBatchSize, callback);
		BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success,lime_tester::wait_for_timeout));

		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// Alice sends a message to Bob, she establish a new session, so her message holds a public key (and a X3DH init)
		auto enc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[0]);
		enc->addRecipient(*bobDeviceId);
		aliceManager->encrypt(*aliceDeviceId, algos, enc, callback);
		if (!BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success, lime_tester::wait_for_timeout))) return false;
		if (!BC_ASSERT_TRUE(lime_tester::DR_message_holdsAsymmetricKeys(enc->m_recipients[0].DRmessage))) return false;
		/* Bob decrypts */
		std::vector<uint8_t> receivedMessage{};
		if (!BC_ASSERT_TRUE(bobManager->decrypt(*bobDeviceId, "bob", *aliceDeviceId, enc->m_recipients[0].DRmessage, enc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::unknown)) return false;
		if (!BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[0])) return false;

		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// Bob replies, this is the first response so his message should hold a public key
		enc = make_shared<lime::EncryptionContext>("alice", lime_tester::messages_pattern[1]);
		enc->addRecipient(*aliceDeviceId);
		bobManager->encrypt(*bobDeviceId, algos, enc, callback);
		if (!BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success, lime_tester::wait_for_timeout))) return false;
		if (!BC_ASSERT_TRUE(lime_tester::DR_message_holdsAsymmetricKeys(enc->m_recipients[0].DRmessage))) return false;
		/* alice decrypts */
		receivedMessage.clear();
		if (!BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDeviceId, "alice", *bobDeviceId, enc->m_recipients[0].DRmessage, enc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted)) return false;
		if (!BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[1])) return false;

		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// Alice encrypts more messages to Bob: we shall skip the asymmetric ratchet until the limit is reached
		// Bob also encrypts message to Alice
		// The limit includes the message sent by Bob, so we already had 1 messages
		for (auto i=1; i<lime::settings::KEMRatchetChainSize; i+=2) {
			auto patternIndex = i % lime_tester::messages_pattern.size();
			enc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[patternIndex]);
			enc->addRecipient(*bobDeviceId);
			aliceManager->encrypt(*aliceDeviceId, algos, enc, callback);
			if (!BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success, lime_tester::wait_for_timeout))) return false;
			if (!BC_ASSERT_FALSE(lime_tester::DR_message_holdsAsymmetricKeys(enc->m_recipients[0].DRmessage))) return false;

			/* decrypt */
			std::vector<uint8_t> receivedMessage{};
			if (!BC_ASSERT_TRUE(bobManager->decrypt(*bobDeviceId, "bob", *aliceDeviceId, enc->m_recipients[0].DRmessage, enc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted)) return false;
			if (!BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[patternIndex])) return false;
			if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

			// Bob encrypts to alice, his message should not hold any public key
			enc = make_shared<lime::EncryptionContext>("alice", lime_tester::messages_pattern[i+1]);
			enc->addRecipient(*aliceDeviceId);
			bobManager->encrypt(*bobDeviceId, algos, enc, callback);
			if (!BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success, lime_tester::wait_for_timeout))) return false;
			if (!BC_ASSERT_FALSE(lime_tester::DR_message_holdsAsymmetricKeys(enc->m_recipients[0].DRmessage))) return false;
			/* alice decrypts so now she nows Bob has her current key */
			receivedMessage.clear();
			if (!BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDeviceId, "alice", *bobDeviceId, enc->m_recipients[0].DRmessage, enc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted)) return false;
			if (!BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[i+1])) return false;
			if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}
		}

		// Alice encrypts one more: she shall make an asymmetric ratchet and we have a public key in the DR header
		// This works because in his last message Bob sent a ECDH to alice so she can perform a EC ratchet (no KEM only ratchet is possible)
		enc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[0]);
		enc->addRecipient(*bobDeviceId);
		aliceManager->encrypt(*aliceDeviceId, algos, enc, callback);
		if (!BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success, lime_tester::wait_for_timeout))) return false;
		if (!BC_ASSERT_TRUE(lime_tester::DR_message_holdsAsymmetricKeys(enc->m_recipients[0].DRmessage))) return false;
		/* decrypt */
		receivedMessage.clear();
		if (!BC_ASSERT_TRUE(bobManager->decrypt(*bobDeviceId, "bob", *aliceDeviceId, enc->m_recipients[0].DRmessage, enc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted)) return false;
		if (!BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[0])) return false;

		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// Alice encrypts one more: she should still send her public key as she cannot know if Bob got it
		enc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[1]);
		enc->addRecipient(*bobDeviceId);
		aliceManager->encrypt(*aliceDeviceId, algos, enc, callback);
		if (!BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success, lime_tester::wait_for_timeout))) return false;
		if (!BC_ASSERT_TRUE(lime_tester::DR_message_holdsAsymmetricKeys(enc->m_recipients[0].DRmessage))) return false;
		/* decrypt */
		receivedMessage.clear();
		if (!BC_ASSERT_TRUE(bobManager->decrypt(*bobDeviceId, "bob", *aliceDeviceId, enc->m_recipients[0].DRmessage, enc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted)) return false;
		if (!BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[1])) return false;

		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// Bob replies, his message should not hold any public key
		enc = make_shared<lime::EncryptionContext>("alice", lime_tester::messages_pattern[1]);
		enc->addRecipient(*aliceDeviceId);
		bobManager->encrypt(*bobDeviceId, algos, enc, callback);
		if (!BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success, lime_tester::wait_for_timeout))) return false;
		if (!BC_ASSERT_FALSE(lime_tester::DR_message_holdsAsymmetricKeys(enc->m_recipients[0].DRmessage))) return false;
		/* alice decrypts so now she nows Bob has her current key */
		receivedMessage.clear();
		if (!BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDeviceId, "alice", *bobDeviceId, enc->m_recipients[0].DRmessage, enc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted)) return false;
		if (!BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[1])) return false;

		// Alice replies, her message should not hold a public key as she knows bot got her's'
		enc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[0]);
		enc->addRecipient(*bobDeviceId);
		aliceManager->encrypt(*aliceDeviceId, algos, enc, callback);
		if (!BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success, lime_tester::wait_for_timeout))) return false;
		if (!BC_ASSERT_FALSE(lime_tester::DR_message_holdsAsymmetricKeys(enc->m_recipients[0].DRmessage))) return false;
		/* decrypt */
		receivedMessage.clear();
		if (!BC_ASSERT_TRUE(bobManager->decrypt(*bobDeviceId, "bob", *aliceDeviceId, enc->m_recipients[0].DRmessage, enc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted)) return false;
		if (!BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[0])) return false;

		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// Fast forward time by one day on Bob side
		bobManager=nullptr; // destroy manager before modifying DB or the DR session keeps the old information cached
		lime_tester::forwardTime(dbFilenameBob, lime::settings::maxKEMRatchetChainPeriod/3600/24 + 1);
		bobManager = make_unique<LimeManager>(dbFilenameBob, X3DHServerPost);

		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// Bob replies again, his message should now hold a public key
		auto bobSkipped = make_shared<lime::EncryptionContext>("alice", lime_tester::messages_pattern[3]);
		bobSkipped->addRecipient(*aliceDeviceId);
		bobManager->encrypt(*bobDeviceId, algos, bobSkipped, callback);
		if (!BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success, lime_tester::wait_for_timeout))) return false;
		if (!BC_ASSERT_TRUE(lime_tester::DR_message_holdsAsymmetricKeys(bobSkipped->m_recipients[0].DRmessage))) return false;

		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// Decryption is delayed, first Alice send another message - without asymmetric ratchet as she cannot do it, and she does not know yet the new Bob's Pk'
		enc = make_shared<lime::EncryptionContext>("bob", lime_tester::messages_pattern[1]);
		enc->addRecipient(*bobDeviceId);
		aliceManager->encrypt(*aliceDeviceId, algos, enc, callback);
		if (!BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success, lime_tester::wait_for_timeout))) return false;
		if (!BC_ASSERT_FALSE(lime_tester::DR_message_holdsAsymmetricKeys(enc->m_recipients[0].DRmessage))) return false;
		/* Bob decrypts a message from Alice but she did not tell she knows the new Pk so Bob should keep sending it */
		receivedMessage.clear();
		if (!BC_ASSERT_TRUE(bobManager->decrypt(*bobDeviceId, "bob", *aliceDeviceId, enc->m_recipients[0].DRmessage, enc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted)) return false;
		if (!BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[1])) return false;

		if (!continuousSession) { managersClean (aliceManager, bobManager, dbFilenameAlice, dbFilenameBob);}

		// Bob send a second message with his new key still unknown to Alice, so he must insert it in the header
		// otherwise alice cannot peform the receiver asymmetric ratchet
		enc = make_shared<lime::EncryptionContext>("alice", lime_tester::messages_pattern[1]);
		enc->addRecipient(*aliceDeviceId);
		bobManager->encrypt(*bobDeviceId, algos, enc, callback);
		if (!BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success, ++expected_success, lime_tester::wait_for_timeout))) return false;
		if (!BC_ASSERT_TRUE(lime_tester::DR_message_holdsAsymmetricKeys(enc->m_recipients[0].DRmessage))) return false;
		/* alice decrypts so now she nows Bob has her current key */
		receivedMessage.clear();
		if (!BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDeviceId, "alice", *bobDeviceId, enc->m_recipients[0].DRmessage, enc->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted)) return false;
		if (!BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[1])) return false;


		// Delayed decryption. Now Alice decrypts the ealier Bob's message
		receivedMessage.clear();
		if (!BC_ASSERT_TRUE(aliceManager->decrypt(*aliceDeviceId, "alice", *bobDeviceId, bobSkipped->m_recipients[0].DRmessage, bobSkipped->m_cipherMessage, receivedMessage) == lime::PeerDeviceStatus::untrusted)) return false;
		if (!BC_ASSERT_TRUE(receivedMessage == lime_tester::messages_pattern[3])) return false;

		if (cleanDatabase) {
			aliceManager->delete_user(DeviceId(*aliceDeviceId, curve), callback);
			bobManager->delete_user(DeviceId(*bobDeviceId, curve), callback);
			if (!BC_ASSERT_TRUE(lime_tester::wait_for(bc_stack,&counters.operation_success,expected_success+2,lime_tester::wait_for_timeout))) return false;
			remove(dbFilenameAlice.data());
			remove(dbFilenameBob.data());
		}
	} catch (BctbxException &e) {
		LIME_LOGE << e;
		BC_FAIL("");
		return false;
	}
	return true;
}
#endif //HAVE_BCTBXPQ
static void lime_kem_asymmetric_ratchet(void) {
#ifdef HAVE_BCTBXPQ
#ifdef EC25519_ENABLED
	BC_ASSERT_TRUE(lime_kem_asymmetric_ratchet_test(lime::CurveId::c25519k512, "lime_kem_asymmetric_ratchet"));
	BC_ASSERT_TRUE(lime_kem_asymmetric_ratchet_test(lime::CurveId::c25519k512, "lime_kem_asymmetric_ratchet_clean"));

	BC_ASSERT_TRUE(lime_kem_asymmetric_ratchet_test(lime::CurveId::c25519mlk512, "lime_kem_asymmetric_ratchet"));
	BC_ASSERT_TRUE(lime_kem_asymmetric_ratchet_test(lime::CurveId::c25519mlk512, "lime_kem_asymmetric_ratchet_clean"));
#endif
#ifdef EC448_ENABLED
	BC_ASSERT_TRUE(lime_kem_asymmetric_ratchet_test(lime::CurveId::c448mlk1024, "lime_kem_asymmetric_ratchet"));
	BC_ASSERT_TRUE(lime_kem_asymmetric_ratchet_test(lime::CurveId::c448mlk1024, "lime_kem_asymmetric_ratchet_clean"));
#endif
#endif
}

static test_t tests[] = {
	TEST_NO_TAG("Basic", x3dh_basic),
	TEST_NO_TAG("User Management", user_management),
	TEST_NO_TAG("User registration failure", user_registration_failure),
	TEST_NO_TAG("User not found", x3dh_user_not_found),
	TEST_NO_TAG("User twice in recipients", x3dh_double_recipient),
	TEST_NO_TAG("Queued encryption", x3dh_operation_queue),
	TEST_NO_TAG("Multi devices queued encryption", x3dh_multidev_operation_queue),
	TEST_NO_TAG("Multiple sessions", x3dh_multiple_DRsessions),
	TEST_NO_TAG("Sending chain limit", x3dh_sending_chain_limit),
	TEST_NO_TAG("Without OPk", x3dh_without_OPk),
	TEST_NO_TAG("Update - clean MK", lime_update_clean_MK),
	TEST_NO_TAG("Update - SPk", lime_update_SPk),
	TEST_NO_TAG("Update - OPk", lime_update_OPk),
	TEST_NO_TAG("Update - Republish", lime_update_republish),
	TEST_NO_TAG("get self Identity Key", lime_getSelfIk),
	TEST_NO_TAG("Verified Status", lime_identityVerifiedStatus),
	TEST_NO_TAG("Peer Device Status", lime_peerDeviceStatus),
	TEST_NO_TAG("Encrypt to unsafe", lime_encryptToUnsafe),
	TEST_NO_TAG("Encryption Policy", lime_encryptionPolicy),
	TEST_NO_TAG("Encryption Policy Error", lime_encryptionPolicyError),
	TEST_NO_TAG("Identity theft", lime_identity_theft),
	TEST_NO_TAG("Multithread", lime_multithread),
	TEST_NO_TAG("Session cancel", lime_session_cancel),
	TEST_NO_TAG("DR Session clean", lime_DR_session_clean),
	TEST_NO_TAG("DB Migration", lime_db_migration),
	TEST_NO_TAG("KEM asymmetric ratchet", lime_kem_asymmetric_ratchet)
};

test_suite_t lime_lime_test_suite = {
	"Lime",
	http_before_all,
	http_after_all,
	NULL,
	NULL,
	sizeof(tests) / sizeof(tests[0]),
	tests,
	0,
	0
};
