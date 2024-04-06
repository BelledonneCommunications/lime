/*
	lime_impl.hpp
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
#ifndef lime_x3dh_hpp
#define lime_x3dh_hpp

#include <memory>
#include <vector>

#include "lime/lime.hpp"
#include "lime_crypto_primitives.hpp"
#include "lime_log.hpp"

namespace lime {

	/* The key type for Signed PreKey */
	template <typename Curve, bool = std::is_base_of_v<genericKEM, Curve>>
	struct SignedPreKey;

	template <typename Curve>
	struct SignedPreKey <Curve, false> {
		private:
			Xpair<Curve> m_SPk; /**< The key pair */
			DSA<Curve, lime::DSAtype::signature> m_Sig; /**< its signature */
			uint32_t m_Id; /**< The key Id */
		public:
			/// Serializing:
			///  - public is publicKey || signature || Id (4bytes) -> used to publish
			///  - storage publicKey || privateKey -> used to store in DB, Id is stored separately
			static constexpr size_t serializedPublicSize(void) {return X<Curve, lime::Xtype::publicKey>::ssize() + DSA<Curve, lime::DSAtype::signature>::ssize() + 4;};
			static constexpr size_t serializedSize(void) {return X<Curve, lime::Xtype::publicKey>::ssize() + X<Curve, lime::Xtype::privateKey>::ssize();};

			SignedPreKey(const X<Curve, lime::Xtype::publicKey> &SPkPublic, const X<Curve, lime::Xtype::privateKey> &SPkPrivate) {
				m_SPk.publicKey() = SPkPublic;
				m_SPk.privateKey() = SPkPrivate;
			};
			SignedPreKey() {};
			/// Unserializing constructor: from data read in DB
			SignedPreKey(const sBuffer<serializedSize()> &SPk, uint32_t Id) {
				m_SPk.publicKey() = SPk.cbegin();
				m_SPk.privateKey() = SPk.cbegin() + X<Curve, lime::Xtype::publicKey>::ssize();
				m_Id = Id;
			};
			/// Unserializing constructor: from data read in received bundle
			SignedPreKey(const std::vector<uint8_t>::const_iterator s) {
				m_SPk.publicKey() =  X<Curve, lime::Xtype::publicKey>(s);
				size_t index = X<Curve, lime::Xtype::publicKey>::ssize();
				m_Id = static_cast<uint32_t>(s[index])<<24 |
					static_cast<uint32_t>(s[index + 1])<<16 |
					static_cast<uint32_t>(s[index + 2])<<8 |
					static_cast<uint32_t>(s[index + 3]);
				index +=4;
				m_Sig = DSA<Curve, lime::DSAtype::signature>(s+index);
			};

			/// accessors
			const X<Curve, lime::Xtype::privateKey> &cprivateKey(void) const {return m_SPk.cprivateKey();};
			const X<Curve, lime::Xtype::publicKey> &cpublicKey(void) const {return m_SPk.cpublicKey();};
			const DSA<Curve, lime::DSAtype::signature> &csignature(void) const {return m_Sig;};
			X<Curve, lime::Xtype::privateKey> &privateKey(void) {return m_SPk.privateKey();};
			X<Curve, lime::Xtype::publicKey> &publicKey(void) {return m_SPk.publicKey();};
			DSA<Curve, lime::DSAtype::signature> &signature(void) {return m_Sig;};
			uint32_t get_Id(void) const {return m_Id;};
			void set_Id(uint32_t Id) {m_Id = Id;};

			/// Serialize the key pair (to store in DB): First the public value, then the private one
			sBuffer<serializedSize()> serialize(void) const {
				sBuffer<serializedSize()> s{};
				std::copy_n(m_SPk.cpublicKey().cbegin(), X<Curve, lime::Xtype::publicKey>::ssize(), s.begin());
				std::copy_n(m_SPk.cprivateKey().cbegin(), X<Curve, lime::Xtype::privateKey>::ssize(), s.begin()+X<Curve, lime::Xtype::publicKey>::ssize());
				return s;
			}
			/// Serialize the public key, signature and Id to publish on the server
			std::vector<uint8_t> serializePublic(void) const {
				std::vector<uint8_t> v(m_SPk.cpublicKey().cbegin(), m_SPk.cpublicKey().cend());
				v.insert(v.end(), m_Sig.cbegin(), m_Sig.cend());
				v.push_back(static_cast<uint8_t>((m_Id>>24)&0xFF));
				v.push_back(static_cast<uint8_t>((m_Id>>16)&0xFF));
				v.push_back(static_cast<uint8_t>((m_Id>>8)&0xFF));
				v.push_back(static_cast<uint8_t>((m_Id)&0xFF));
				return v;
			}

			/// Dump the public key, signature and Id
			void dump(std::ostringstream &os, std::string indent="        ") const {
				os<<std::endl<<indent<<"SPK Id: 0x"<<std::hex<<std::setw(8) << std::setfill('0') << m_Id <<indent<<"SPK: ";
				hexStr(os, m_SPk.cpublicKey().data(),  X<Curve, lime::Xtype::publicKey>::ssize());
				os<<std::endl<<indent<<"SPK Sig: ";
				hexStr(os, m_Sig.data(),  DSA<Curve, lime::DSAtype::signature>::ssize());
			}
	};

	/* The key type for One time PreKey */
	template <typename Curve, bool = std::is_base_of_v<genericKEM, Curve>>
	struct OneTimePreKey;

	template <typename Curve>
	struct OneTimePreKey <Curve, false> {
		private:
			Xpair<Curve> m_OPk; /**< The key pair */
			uint32_t m_Id; /**< The key Id */
		public:
			/// Serializing:
			///  - public is publicKey || Id (4bytes) -> used to publish
			///  - storage publicKey || privateKey -> used to store in DB, Id is stored separately
			static constexpr size_t serializedPublicSize(void) {return X<Curve, lime::Xtype::publicKey>::ssize() + DSA<Curve, lime::DSAtype::signature>::ssize() + 4;};
			static constexpr size_t serializedSize(void) {return X<Curve, lime::Xtype::publicKey>::ssize() + X<Curve, lime::Xtype::privateKey>::ssize();};

			OneTimePreKey(const X<Curve, lime::Xtype::publicKey> &OPkPublic, const X<Curve, lime::Xtype::privateKey> &OPkPrivate, uint32_t Id) : m_OPk(OPkPublic, OPkPrivate), m_Id{Id} {};
			OneTimePreKey() {};
			/// Unserializing constructor: from data read in DB
			OneTimePreKey(const sBuffer<serializedSize()> &OPk, uint32_t Id) {
				m_OPk.publicKey() = OPk.cbegin();
				m_OPk.privateKey() = OPk.cbegin() + X<Curve, lime::Xtype::publicKey>::ssize();
				m_Id = Id;
			};
			/// Unserializing constructor: from data read in received bundle
			OneTimePreKey(const std::vector<uint8_t>::const_iterator s) {
				m_OPk.publicKey() =  X<Curve, lime::Xtype::publicKey>(s);
				size_t index = X<Curve, lime::Xtype::publicKey>::ssize();
				m_Id = static_cast<uint32_t>(s[index])<<24 |
					static_cast<uint32_t>(s[index + 1])<<16 |
					static_cast<uint32_t>(s[index + 2])<<8 |
					static_cast<uint32_t>(s[index + 3]);
			};

			/// accessors
			const X<Curve, lime::Xtype::privateKey> &cprivateKey(void) const {return m_OPk.cprivateKey();};
			const X<Curve, lime::Xtype::publicKey> &cpublicKey(void) const {return m_OPk.cpublicKey();};
			X<Curve, lime::Xtype::privateKey> &privateKey(void) {return m_OPk.privateKey();};
			X<Curve, lime::Xtype::publicKey> &publicKey(void) {return m_OPk.publicKey();};
			uint32_t get_Id(void) const {return m_Id;};
			void set_Id(uint32_t Id) {m_Id = Id;};

			/// Serialize the key pair (to store in DB): First the public value, then the private one
			sBuffer<serializedSize()> serialize(void) const {
				sBuffer<serializedSize()> s{};
				std::copy_n(m_OPk.cpublicKey().cbegin(), X<Curve, lime::Xtype::publicKey>::ssize(), s.begin());
				std::copy_n(m_OPk.cprivateKey().cbegin(), X<Curve, lime::Xtype::privateKey>::ssize(), s.begin()+X<Curve, lime::Xtype::publicKey>::ssize());
				return s;
			}
			/// Serialize the public key and Id to publish on the server
			std::vector<uint8_t> serializePublic(void) const {
				std::vector<uint8_t> v(m_OPk.cpublicKey().cbegin(), m_OPk.cpublicKey().cend());
				v.push_back(static_cast<uint8_t>((m_Id>>24)&0xFF));
				v.push_back(static_cast<uint8_t>((m_Id>>16)&0xFF));
				v.push_back(static_cast<uint8_t>((m_Id>>8)&0xFF));
				v.push_back(static_cast<uint8_t>((m_Id)&0xFF));
				return v;
			}

			/// Dump the public key and Id
			void dump(std::ostringstream &os, std::string indent="        ") const {
				os<<std::endl<<indent<<"OPK Id: 0x"<<std::hex<<std::setw(8) << std::setfill('0') << m_Id <<indent<<"OPK: ";
				hexStr(os, m_OPk.cpublicKey().data(),  X<Curve, lime::Xtype::publicKey>::ssize());
			}
	};

	// forward declarations
	struct callbackUserData;
	class DR;

	class X3DH
	{
	public:
		virtual void set_x3dhServerUrl(const std::string &x3dhServerUrl) = 0;
		virtual std::string get_x3dhServerUrl(void) = 0;
		virtual std::shared_ptr<DR> init_receiver_session(const std::vector<uint8_t> X3DH_initMessage, const std::string &senderDeviceId) = 0;
		virtual void fetch_peerBundles(std::shared_ptr<callbackUserData> userData, std::vector<std::string> &peerDeviceIds) = 0; /**< fetch key bundles from server */
		virtual void publish_user(std::shared_ptr<callbackUserData> userData, const uint16_t OPkInitialBatchSize) = 0; /**< publish a new user */
		virtual void delete_user(std::shared_ptr<callbackUserData> userData) = 0; /**< delete current user from server */
		virtual std::vector<uint8_t> get_Ik(void) = 0; /**< returns our public identity key */
		virtual long int get_dbUid(void) const noexcept = 0; /**< get the User Id in database */
		virtual bool is_currentSPk_valid(void) = 0;
		virtual void update_SPk(std::shared_ptr<callbackUserData> userData) = 0;
		virtual void update_OPk(std::shared_ptr<callbackUserData> userData) = 0;
		virtual void get_Ik(std::vector<uint8_t> &Ik) = 0;
		virtual ~X3DH() = default;
	};

	/**
	 * Factory functions : Create an X3DH pointer - instanciate the correct type matching the given template param
	 *
	 * @param[in,out]	localStorage	pointer to DB accessor
	 * @param[in]		deviceId		device Id(shall be GRUU), stored in the structure
	 * @param[in]		url				URL of the X3DH key server used to publish our keys(retrieved from DB)
	 * @param[in]		X3DH_post_data	A function used to communicate with the X3DH server
	 * @param[in]		Uid				the DB internal Id for this user, speed up DB operations by holding it in object, when 0: create the user
	 *
	 * @return	pointer to a generic X3DH object
	 */
	template <typename Algo> std::shared_ptr<X3DH> make_X3DH(std::shared_ptr<lime::Db> localStorage, const std::string &selfDeviceId, const std::string &X3DHServerURL, const limeX3DHServerPostData &X3DH_post_data, std::shared_ptr<RNG> RNG_context, const long Uid = 0);


#ifdef EC25519_ENABLED
	extern template std::shared_ptr<X3DH> make_X3DH<C255>(std::shared_ptr<lime::Db> localStorage, const std::string &selfDeviceId, const std::string &X3DHServerURL, const limeX3DHServerPostData &X3DH_post_data, std::shared_ptr<RNG> RNG_context, const long Uid);
#endif
#ifdef EC448_ENABLED
	extern template std::shared_ptr<X3DH> make_X3DH<C448>(std::shared_ptr<lime::Db> localStorage, const std::string &selfDeviceId, const std::string &X3DHServerURL, const limeX3DHServerPostData &X3DH_post_data, std::shared_ptr<RNG> RNG_context, const long Uid);
#endif
} //namespace lime
#endif /* lime_x3dh_hpp */
