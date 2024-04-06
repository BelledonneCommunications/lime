/*
	lime.cpp
	@author Johan Pascal
	@copyright	Copyright (C) 2017  Belledonne Communications SARL

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
#include "lime_impl.hpp"
#include "bctoolbox/exception.hh"
#include "lime_double_ratchet.hpp"
#include "lime_double_ratchet_protocol.hpp"
#include "lime_x3dh.hpp"
#include <mutex>

using namespace::std;

namespace lime {

	/****************************************************************************/
	/*                                                                          */
	/* Constructor                                                             */
	/*                                                                          */
	/****************************************************************************/
	/**
	 * @brief Load or Create user constructor
	 *
	 *  before calling this constructor, user existence in DB is checked and its Uid retrieved
	 *  - with an Uid : load the user infos in Lime/X3DH object
	 *  - with an Uid to 0 : create the user en DB (only its identity) and set its data in Lime/X3DH object.
	 * The publish_user will the create the needed keys (SPk, OPk) and upload everything to the server
	 *
	 * @param[in,out]	localStorage		pointer to DB accessor
	 * @param[in]		deviceId			device Id(shall be GRUU), stored in the structure
	 * @param[in]		url					URL of the X3DH key server used to publish our keys(retrieved from DB)
	 * @param[in]		X3DH_post_data		A function used to communicate with the X3DH server
	 * @param[in]		Uid					the DB internal Id for this user, speed up DB operations by holding it in DB. If set to 0 -> create the user
	 *
	 */
	template <typename Curve>
	Lime<Curve>::Lime(std::shared_ptr<lime::Db> localStorage, const std::string &deviceId, const std::string &url, const limeX3DHServerPostData &X3DH_post_data, const long int Uid)
	: m_RNG{make_RNG()}, m_selfDeviceId{deviceId},
	m_X3DH{make_X3DH<Curve>(localStorage, deviceId, url, X3DH_post_data, m_RNG, Uid)},
	m_localStorage(localStorage), m_db_Uid{m_X3DH->get_dbUid()}, // When this is a device creation, the make_X3DH will take care of it so the db_Uid must be retrieved from it
	m_DR_sessions_cache{}, m_ongoing_encryption{nullptr}, m_encryption_queue{}
	{ }

	template <typename Curve>
	Lime<Curve>::~Lime() {};

	/****************************************************************************/
	/*                                                                          */
	/* Public API implenenting the virtual class LimeGeneric                    */
	/* API documentation is in lime_lime.hpp                                    */
	/*                                                                          */
	/****************************************************************************/
	template <typename Curve>
	void Lime<Curve>::publish_user(const limeCallback &callback, const uint16_t OPkInitialBatchSize) {
		auto userData = make_shared<callbackUserData>(std::static_pointer_cast<LimeGeneric>(this->shared_from_this()), callback, OPkInitialBatchSize);
		// Publish the user
		m_X3DH->publish_user(userData, OPkInitialBatchSize);
	}

	template <typename Curve>
	void Lime<Curve>::delete_user(const limeCallback &callback) {
		// delete user from local Storage
		m_localStorage->delete_LimeUser(m_selfDeviceId);

		// delete user from server
		auto userData = make_shared<callbackUserData>(std::static_pointer_cast<LimeGeneric>(this->shared_from_this()), callback);
		m_X3DH->delete_user(userData);
	}

	template <typename Curve>
	void Lime<Curve>::delete_peerDevice(const std::string &peerDeviceId) {
		m_DR_sessions_cache.erase(peerDeviceId); // remove session from cache if any
	}

	template <typename Curve>
	void Lime<Curve>::update_SPk(const limeCallback &callback) {
		// Do we need to update the SPk
		if (!m_X3DH->is_currentSPk_valid()) {
			LIME_LOGI<<"User "<<m_selfDeviceId<<" updates its SPk";
			auto userData = make_shared<callbackUserData>(std::static_pointer_cast<LimeGeneric>(this->shared_from_this()), callback);
			// Update SPk locally and on server
			m_X3DH->update_SPk(userData);
		} else { // nothing to do but caller expect a callback
			if (callback) callback(lime::CallbackReturn::success, "");
		}
	}

	template <typename Curve>
	void Lime<Curve>::update_OPk(const limeCallback &callback, uint16_t OPkServerLowLimit, uint16_t OPkBatchSize) {
		// Request Server for the count of our OPk it still holds
		// OPk server low limit cannot be zero, it must be at least one as we test the userData on this to check the server request was a getSelfOPks
		// and republish the user if not found
		auto userData = make_shared<callbackUserData>(std::static_pointer_cast<LimeGeneric>(this->shared_from_this()), callback, std::max(OPkServerLowLimit,static_cast<uint16_t>(1)), OPkBatchSize);
		m_X3DH->update_OPk(userData);
	}

	template <typename Curve>
	void Lime<Curve>::get_Ik(std::vector<uint8_t> &Ik) {
		m_X3DH->get_Ik(Ik);
	}

	template <typename Curve>
	void Lime<Curve>::encrypt(std::shared_ptr<const std::vector<uint8_t>> recipientUserId, std::shared_ptr<std::vector<RecipientData>> recipients, std::shared_ptr<const std::vector<uint8_t>> plainMessage, const lime::EncryptionPolicy encryptionPolicy, std::shared_ptr<std::vector<uint8_t>> cipherMessage, const limeCallback &callback) {
		LIME_LOGI<<"encrypt from "<<m_selfDeviceId<<" to "<<recipients->size()<<" recipients";
		/* Check if we have all the Double Ratchet sessions ready or shall we go for an X3DH */

		/* Create the appropriate recipient infos and fill it with sessions found in cache */
		// internal_recipients is a vector duplicating the recipients one in the same order (ignoring the one with peerStatus set to fail)
		// This allows fast copying of relevant information back to recipients when encryption is completed
		std::vector<RecipientInfos> internal_recipients{};

		std::unique_lock<std::mutex> lock(m_mutex);
		for (const auto &recipient : *recipients) {
			// if the input recipient peerStatus is fail we must ignore it
			// most likely: we're in a call after a key bundle fetch and this peer device does not have keys on the X3DH server
			if (recipient.peerStatus != lime::PeerDeviceStatus::fail) {
				auto sessionElem = m_DR_sessions_cache.find(recipient.deviceId);
				if (sessionElem != m_DR_sessions_cache.end()) { // session is in cache
					if (sessionElem->second->isActive()) { // the session in cache is active
						internal_recipients.emplace_back(recipient.deviceId, sessionElem->second);
					} else { // session in cache is not active(may append if last encryption reach sending chain symmetric ratchet usage)
						internal_recipients.emplace_back(recipient.deviceId);
						m_DR_sessions_cache.erase(recipient.deviceId); // remove unactive session from cache
					}
				} else { // session is not in cache, just create it and the session ptr will be a nullptr
					internal_recipients.emplace_back(recipient.deviceId);
				}
			}
		}

		/* try to load all the session that are not in cache and set the peer Device status for all recipients*/
		std::vector<std::string> missing_devices{};
		cache_DR_sessions(internal_recipients, missing_devices);

		/* If we are still missing session we must ask the X3DH server for key bundles */
		if (missing_devices.size()>0) {
			// create a new callbackUserData, it shall be then deleted in callback, store in all shared_ptr to input/output values needed to call this encrypt function
			auto userData = make_shared<callbackUserData>(std::static_pointer_cast<LimeGeneric>(this->shared_from_this()), callback, recipientUserId, recipients, plainMessage, cipherMessage, encryptionPolicy);
			if (m_ongoing_encryption == nullptr) { // no ongoing asynchronous encryption process it
				m_ongoing_encryption = userData;
			} else { // some one else is expecting X3DH server response, enqueue this request
				m_encryption_queue.push(userData);
				return;
			}
			lock.unlock(); // unlock before calling external callbacks
			// retrieve bundles from X3DH server, when they arrive, it will run the X3DH initiation and create the DR sessions
			m_X3DH->fetch_peerBundles(userData, missing_devices);
			return;
		}

		// We have everyone: encrypt
		encryptMessage(internal_recipients, *plainMessage, *recipientUserId, m_selfDeviceId, *cipherMessage, encryptionPolicy, m_localStorage);

		// move DR messages to the input/output structure, ignoring again the input with peerStatus set to fail
		// so the index on the internal_recipients still matches the way we created it from recipients
		size_t i=0;
		auto callbackStatus = lime::CallbackReturn::fail;
		std::string callbackMessage{"All recipients failed to provide a key bundle"};
		for (auto &recipient : *recipients) {
			if (recipient.peerStatus != lime::PeerDeviceStatus::fail) {
				recipient.DRmessage = std::move(internal_recipients[i].DRmessage);
				recipient.peerStatus = internal_recipients[i].peerStatus;
				i++;
				callbackStatus = lime::CallbackReturn::success; // we must have at least one recipient with a successful encryption to return success
				callbackMessage.clear();
			}
		}

		lock.unlock(); // unlock before calling external callbacks
		if (callback) callback(callbackStatus, callbackMessage);
		lock.lock();

		// is there no one in an asynchronous encryption process and do we have something in encryption queue to process
		if (m_ongoing_encryption == nullptr && !m_encryption_queue.empty()) { // may happend when an encryption was queued but session was created by a previously queued encryption request
			auto userData = m_encryption_queue.front();
			m_encryption_queue.pop(); // remove it from queue and do it
			lock.unlock(); // unlock before recursive call
			encrypt(userData->recipientUserId, userData->recipients, userData->plainMessage, userData->encryptionPolicy, userData->cipherMessage, userData->callback);
		}
	}

	template <typename Curve>
	lime::PeerDeviceStatus Lime<Curve>::decrypt(const std::vector<uint8_t> &recipientUserId, const std::string &senderDeviceId, const std::vector<uint8_t> &DRmessage, const std::vector<uint8_t> &cipherMessage, std::vector<uint8_t> &plainMessage) {
		std::lock_guard<std::mutex> lock(m_mutex);
		// before trying to decrypt, we must check if the sender device is known in the local Storage and if we trust it
		// a successful decryption will insert it in local storage so we must check first if it is there in order to detect new devices
		// Note: a device could already be trusted in DB even before the first message (if we established trust before sending the first message)
		// senderDeviceStatus can only be unknown, untrusted, trusted or unsafe.
		// If decryption succeed, we will return this status but it has no effect on the decryption process
		auto senderDeviceStatus = m_localStorage->get_peerDeviceStatus(senderDeviceId);

		LIME_LOGI<<m_selfDeviceId<<" decrypts from "<<senderDeviceId;
		// do we have any session (loaded or not) matching that senderDeviceId ?
		auto sessionElem = m_DR_sessions_cache.find(senderDeviceId);
		auto db_sessionIdInCache = 0; // this would be the db_sessionId of the session stored in cache if there is one, no session has the Id 0
		if (sessionElem != m_DR_sessions_cache.end()) { // session is in cache, it is the active one, just give it a try
			db_sessionIdInCache = sessionElem->second->dbSessionId();
			std::vector<std::shared_ptr<DR>> cached_DRSessions{1, sessionElem->second}; // copy the session pointer into a vector as the decrypt function ask for it
			if (decryptMessage(senderDeviceId, m_selfDeviceId, recipientUserId, cached_DRSessions, DRmessage, cipherMessage, plainMessage) != nullptr) {
				// we manage to decrypt the message with the current active session loaded in cache
				return senderDeviceStatus;
			} else { // remove session from cache
				// session in local storage is not modified, so it's still the active one, it will change status to stale when an other active session will be created
				m_DR_sessions_cache.erase(sessionElem);
			}
		}

		// If we are still here, no session in cache or it didn't decrypt with it. Lookup in localStorage
		std::vector<std::shared_ptr<DR>> DRSessions{};
		// load in DRSessions all the session found in cache for this peer device, except the one with id db_sessionIdInCache(is ignored if 0) as we already tried it
		get_DRSessions(senderDeviceId, db_sessionIdInCache, DRSessions);
		LIME_LOGI<<m_selfDeviceId<<" decrypts from "<<senderDeviceId<<" : found "<<DRSessions.size()<<" sessions in DB";
		auto usedDRSession = decryptMessage(senderDeviceId, m_selfDeviceId, recipientUserId, DRSessions, DRmessage, cipherMessage, plainMessage);
		if (usedDRSession != nullptr) { // we manage to decrypt with a session
			m_DR_sessions_cache[senderDeviceId] = std::move(usedDRSession); // store it in cache
			return senderDeviceStatus;
		}

		// No luck yet, is this message holds a X3DH header - if no we must give up
		std::vector<uint8_t> X3DH_initMessage{};
		if (!double_ratchet_protocol::parseMessage_get_X3DHinit<Curve>(DRmessage, X3DH_initMessage)) {
			LIME_LOGE<<"Fail to decrypt: No DR session found and no X3DH init message";
			return lime::PeerDeviceStatus::fail;
		}

		// parse the X3DH init message, get keys from localStorage, compute the shared secrets, create DR_Session and return a shared pointer to it
		try {
			std::shared_ptr<DR> DRSession{m_X3DH->init_receiver_session(X3DH_initMessage, senderDeviceId)}; // would just throw an exception in case of failure
			DRSessions.clear();
			DRSessions.push_back(DRSession);
		} catch (BctbxException const &e) {
			LIME_LOGE<<"Fail to create the DR session from the X3DH init message : "<<e;
			return lime::PeerDeviceStatus::fail;
		}

		if (decryptMessage(senderDeviceId, m_selfDeviceId, recipientUserId, DRSessions, DRmessage, cipherMessage, plainMessage) != 0) {
			// we manage to decrypt the message with this session, set it in cache
			m_DR_sessions_cache[senderDeviceId] = std::move(DRSessions.front());
			return senderDeviceStatus;
		}
		LIME_LOGE<<"Fail to decrypt: Newly created DR session failed to decrypt the message";
		return lime::PeerDeviceStatus::fail;
	}

	template <typename Curve>
	std::string Lime<Curve>::get_x3dhServerUrl() {
		return m_X3DH->get_x3dhServerUrl();
	}

	template <typename Curve>
	void Lime<Curve>::set_x3dhServerUrl(const std::string &x3dhServerUrl) {
		m_X3DH->set_x3dhServerUrl(x3dhServerUrl);
	}

	template <typename Curve>
	void Lime<Curve>::processEncryptionQueue(void) {
		m_ongoing_encryption = nullptr; // make sure to free any ongoing encryption
		// check if others encryptions are in queue and call them if needed
		if (!m_encryption_queue.empty()) {
			auto userData = m_encryption_queue.front();
			m_encryption_queue.pop(); // remove it from queue and do it, as there is no more ongoing it shall be processed even if the queue still holds elements
			encrypt(userData->recipientUserId, userData->recipients, userData->plainMessage, userData->encryptionPolicy, userData->cipherMessage, userData->callback);
		}
	}

	template <typename Curve>
	void Lime<Curve>::DRcache_delete(const std::string &deviceId) {
		m_DR_sessions_cache.erase(deviceId);
	}

	template <typename Curve>
	void Lime<Curve>::DRcache_insert(const std::string &deviceId, std::shared_ptr<DR> DRsession) {
		m_DR_sessions_cache.emplace(deviceId, DRsession);
	}

	/* instantiate Lime for C255 and C448 */
#ifdef EC25519_ENABLED
	/* These extern templates are defined in lime_localStorage.cpp */
	extern template void Lime<C255>::cache_DR_sessions(std::vector<RecipientInfos> &internal_recipients, std::vector<std::string> &missing_devices);
	extern template void Lime<C255>::get_DRSessions(const std::string &senderDeviceId, const long int ignoreThisDBSessionId, std::vector<std::shared_ptr<DR>> &DRSessions);
	extern template void Lime<C255>::stale_sessions(const std::string &peerDeviceId);

	template class Lime<C255>;
#endif

#ifdef EC448_ENABLED
	/* These extern templates are defined in lime_localStorage.cpp */
	extern template void Lime<C448>::cache_DR_sessions(std::vector<RecipientInfos> &internal_recipients, std::vector<std::string> &missing_devices);
	extern template void Lime<C448>::get_DRSessions(const std::string &senderDeviceId, const long int ignoreThisDBSessionId, std::vector<std::shared_ptr<DR>> &DRSessions);
	extern template void Lime<C448>::stale_sessions(const std::string &peerDeviceId);

	template class Lime<C448>;
#endif

	/****************************************************************************/
	/*                                                                          */
	/* Factory functions and Delete user                                        */
	/*                                                                          */
	/****************************************************************************/
	/**
	 * @brief : Insert user in database and return a pointer to the control class instanciating the appropriate Lime children class
	 *
	 *	Once created a user cannot be modified, insertion of existing deviceId will raise an exception.
	 *
	 * @param[in]	localStorage			Database access
	 * @param[in]	deviceId				User to create in DB, deviceId shall be the GRUU
	 * @param[in]	url						URL of X3DH key server to be used to publish our keys
	 * @param[in]	curve					Which curve shall we use for this account, select the implemenation to instanciate when using this user
	 * @param[in]	OPkInitialBatchSize		Number of OPks in the first batch uploaded to X3DH server
	 * @param[in]	X3DH_post_data			A function used to communicate with the X3DH server
	 * @param[in]	callback				To provide caller the operation result
	 *
	 * @return a pointer to the LimeGeneric class allowing access to API declared in lime_lime.hpp
	 */
	std::shared_ptr<LimeGeneric> insert_LimeUser(std::shared_ptr<lime::Db> localStorage, const std::string &deviceId, const std::string &url, const lime::CurveId curve, const uint16_t OPkInitialBatchSize,
			const limeX3DHServerPostData &X3DH_post_data, const limeCallback &callback) {
		LIME_LOGI<<"Create Lime user "<<deviceId;
		/* first check the requested curve is instanciable and return an exception if not */
#ifndef EC25519_ENABLED
		if (curve == lime::CurveId::c25519) {
			throw BCTBX_EXCEPTION << "Lime User creation asking to use Curve 25519 but it's not supported - change lib lime compile option to enable it";
		}
#endif
#ifndef EC448_ENABLED
		if (curve == lime::CurveId::c448) {
			throw BCTBX_EXCEPTION << "Lime User creation asking to use Curve 448 but it's not supported - change lib lime compile option to enable it";
		}
#endif

		//instanciate the correct Lime object
		switch (curve) {
			case lime::CurveId::c25519 :
#ifdef EC25519_ENABLED
			{
				/* constructor will insert user in Db, if already present, raise an exception*/
				auto lime_ptr = std::make_shared<Lime<C255>>(localStorage, deviceId, url, X3DH_post_data);
				lime_ptr->publish_user(callback, OPkInitialBatchSize);
				return std::static_pointer_cast<LimeGeneric>(lime_ptr);
			}
#endif
			break;

			case lime::CurveId::c448 :
#ifdef EC448_ENABLED
			{
				auto lime_ptr = std::make_shared<Lime<C448>>(localStorage, deviceId, url, X3DH_post_data);
				lime_ptr->publish_user(callback, OPkInitialBatchSize);
				return std::static_pointer_cast<LimeGeneric>(lime_ptr);
			}
#endif
			break;

			case lime::CurveId::unset :
			default: // asking for an unsupported type
				throw BCTBX_EXCEPTION << "Cannot create lime user "<<deviceId;//<<". Unsupported curve (id <<"static_cast<uint8_t>(curve)") requested";
				break;
		}
		return nullptr;
	};

	/**
	 * @brief : Load user from database and return a pointer to the control class instanciating the appropriate Lime children class
	 *
	 *	Fail to find the user will raise an exception
	 *	If allStatus flag is set to false (default value), raise an exception on inactive users otherwise load inactive user.
	 *
	 * @param[in]	localStorage		Database access
	 * @param[in]	deviceId		User to lookup in DB, deviceId shall be the GRUU
	 * @param[in]	X3DH_post_data		A function used to communicate with the X3DH server
	 * @param[in]	allStatus		allow loading of inactive user if set to true
	 *
	 * @return a pointer to the LimeGeneric class allowing access to API declared in lime_lime.hpp
	 */
	std::shared_ptr<LimeGeneric> load_LimeUser(std::shared_ptr<lime::Db> localStorage, const std::string &deviceId, const limeX3DHServerPostData &X3DH_post_data, const bool allStatus) {

		/* load user */
		auto curve = CurveId::unset;
		long int Uid=0;
		std::string x3dh_server_url;

		localStorage->load_LimeUser(deviceId, Uid, curve, x3dh_server_url, allStatus); // this one will throw an exception if user is not found, just let it rise
		LIME_LOGI<<"Load Lime user "<<deviceId;

		/* check the curve id retrieved from DB is instanciable and return an exception if not */
#ifndef EC25519_ENABLED
		if (curve == lime::CurveId::c25519) {
			throw BCTBX_EXCEPTION << "Lime load User "<<deviceId<<" requests usage of Curve 25519 but it's not supported - change lib lime compile option to enable it";
		}
#endif
#ifndef EC448_ENABLED
		if (curve == lime::CurveId::c448) {
			throw BCTBX_EXCEPTION << "Lime load User "<<deviceId<<" requests usage of Curve 448 but it's not supported - change lib lime compile option to enable it";
		}
#endif


		switch (curve) {
			case lime::CurveId::c25519 :
#ifdef EC25519_ENABLED
				return std::make_shared<Lime<C255>>(localStorage, deviceId, x3dh_server_url, X3DH_post_data, Uid);
#endif
			break;

			case lime::CurveId::c448 :
#ifdef EC448_ENABLED
				return std::make_shared<Lime<C448>>(localStorage, deviceId, x3dh_server_url, X3DH_post_data, Uid);
#endif
			break;

			case lime::CurveId::unset :
			default: // asking for an unsupported type
				throw BCTBX_EXCEPTION << "Cannot create load user "<<deviceId;
			break;
		}
		return nullptr;
	};
} //namespace lime
