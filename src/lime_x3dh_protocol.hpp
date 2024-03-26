/*
	lime_x3dh_protocol.hpp
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

#ifndef lime_x3dh_protocol_hpp
#define lime_x3dh_protocol_hpp

#include "lime_crypto_primitives.hpp"
#include "lime_x3dh.hpp"
#include "lime_log.hpp"
#include <iostream> // ostreamstring to generate incoming/outgoing messages debug trace
#include <iomanip>

namespace lime {

	/** @brief Set possible values for a flag in the keyBundle X3DH packet
	 *
	 *  @note Do not modify values or we'll loose sync with existing X3DH server
	 */
	enum class X3DHKeyBundleFlag : uint8_t {
		noOPk=0, /**< This bundle does not contain an OPk */
		OPk=1, /**< This bundle contains an OPk */
		noBundle=2}; /**< This bundle is empty(just a deviceId) as this user was not found on X3DH server */

	/**
	 * @brief Holds everything found in a key bundle received from X3DH server
	 */
	template <typename Curve>
	struct X3DH_peerBundle {
		static constexpr size_t ssize(bool haveOPk) {return DSA<Curve, lime::DSAtype::publicKey>::ssize() + X<Curve, lime::Xtype::publicKey>::ssize() + DSA<Curve, lime::DSAtype::signature>::ssize() + 4 + (haveOPk?(X<Curve, lime::Xtype::publicKey>::ssize()+4):0); };
		const std::string deviceId; /**< peer device Id */
		DSA<Curve, lime::DSAtype::publicKey> Ik; /**< peer device public identity key */
		SignedPreKey<Curve> SPk; /**< peer device current public pre-signed key */
		const lime::X3DHKeyBundleFlag bundleFlag; /**< Flag this bundle as empty and if not if it holds an OPk, possible values */
		X<Curve, lime::Xtype::publicKey> OPk; /**< peer device One Time preKey */
		uint32_t OPk_id; /**< id of the peer device current public pre-signed key */

		/**
		 * Constructor gets vector<uint8_t> iterators to the bundle begining 
		 *
		 * @param[in]	deviceId	peer Device Id providing this key bundle
		 * @param[in]	bundle		iterator pointing to the begining of the key bundle - Ik begin
		 * @param[in]	haveOPk		true when there is an OPk to parse
		 * @param[in/out]	message_trace	Debug information to accumulate
		 */
		X3DH_peerBundle(std::string &&deviceId, const std::vector<uint8_t>::const_iterator bundle, bool haveOPk, std::ostringstream &message_trace) : deviceId{deviceId}, bundleFlag(haveOPk?lime::X3DHKeyBundleFlag::OPk:lime::X3DHKeyBundleFlag::noOPk) {
			// Ik: DSA public key
			Ik =  DSA<Curve, lime::DSAtype::publicKey>(bundle);
			size_t index = DSA<Curve, lime::DSAtype::publicKey>::ssize();

			// add Ik to message trace
			message_trace << "        Ik: "<<std::hex << std::setfill('0');
			hexStr(message_trace, Ik.data(), DSA<Curve, lime::DSAtype::publicKey>::ssize());

			SPk = SignedPreKey<Curve>(bundle+index);
			index += SignedPreKey<Curve>::serializedPublicSize();

			// add SPk Id, SPk and SPk signature to the trace
			SPk.dump(message_trace);

			if (haveOPk) {
				OPk = bundle+index; index += X<Curve, lime::Xtype::publicKey>::ssize();
				OPk_id = static_cast<uint32_t>(bundle[index])<<24 |
					static_cast<uint32_t>(bundle[index+1])<<16 |
					static_cast<uint32_t>(bundle[index+2])<<8 |
					static_cast<uint32_t>(bundle[index+3]);
				index += 4;

				// add OPk Id and OPk to the trace
				message_trace <<std::endl<<"        OPk Id: 0x" << std::setw(8) << static_cast<unsigned int>(OPk_id)<<"        OPk: ";
				hexStr(message_trace, OPk.data(), X<Curve, lime::Xtype::publicKey>::ssize());
			}
		};
		/**
		 * @overload
		 * construct without bundle when not present in the parsed server response
		 */
		X3DH_peerBundle(std::string &&deviceId) :
		deviceId{deviceId}, Ik{}, SPk{}, bundleFlag{lime::X3DHKeyBundleFlag::noBundle}, OPk{}, OPk_id{0} {};
	};

	namespace x3dh_protocol {
		template <typename Curve>
		void buildMessage_registerUser(std::vector<uint8_t> &message, const DSA<Curve, lime::DSAtype::publicKey> &Ik, const SignedPreKey<Curve> &SPk, const std::vector<X<Curve, lime::Xtype::publicKey>> &OPks, const std::vector<uint32_t> &OPk_ids) noexcept;

		template <typename Curve>
		void buildMessage_deleteUser(std::vector<uint8_t> &message) noexcept;

		template <typename Curve>
		void buildMessage_publishSPk(std::vector<uint8_t> &message, const SignedPreKey<Curve> &SPk) noexcept;

		template <typename Curve>
		void buildMessage_publishOPks(std::vector<uint8_t> &message, const std::vector<X<Curve, lime::Xtype::publicKey>> &OPks, const std::vector<uint32_t> &OPk_ids) noexcept;

		template <typename Curve>
		void buildMessage_getPeerBundles(std::vector<uint8_t> &message, std::vector<std::string> &peer_device_ids) noexcept;

		template <typename Curve>
		void buildMessage_getSelfOPks(std::vector<uint8_t> &message) noexcept;

		/* this templates are intanciated in lime_x3dh_procotocol.cpp, do not re-instanciate it anywhere else */
#ifdef EC25519_ENABLED
		extern template void buildMessage_registerUser<C255>(std::vector<uint8_t> &message, const DSA<C255, lime::DSAtype::publicKey> &Ik, const SignedPreKey<C255> &SPk, const std::vector<X<C255, lime::Xtype::publicKey>> &OPks, const std::vector<uint32_t> &OPk_ids) noexcept;
		extern template void buildMessage_deleteUser<C255>(std::vector<uint8_t> &message) noexcept;
		extern template void buildMessage_publishSPk<C255>(std::vector<uint8_t> &message, const SignedPreKey<C255> &SPk) noexcept;
		extern template void buildMessage_publishOPks<C255>(std::vector<uint8_t> &message, const std::vector<X<C255, lime::Xtype::publicKey>> &OPks, const std::vector<uint32_t> &OPk_ids) noexcept;
		extern template void buildMessage_getPeerBundles<C255>(std::vector<uint8_t> &message, std::vector<std::string> &peer_device_ids) noexcept;
		extern template void buildMessage_getSelfOPks<C255>(std::vector<uint8_t> &message) noexcept;
#endif

#ifdef EC448_ENABLED
		extern template void buildMessage_registerUser<C448>(std::vector<uint8_t> &message, const DSA<C448, lime::DSAtype::publicKey> &Ik,  const SignedPreKey<C448> &SPk, const std::vector<X<C448, lime::Xtype::publicKey>> &OPks, const std::vector<uint32_t> &OPk_ids) noexcept;
		extern template void buildMessage_deleteUser<C448>(std::vector<uint8_t> &message) noexcept;
		extern template void buildMessage_publishSPk<C448>(std::vector<uint8_t> &message, const SignedPreKey<C448> &SPk) noexcept;
		extern template void buildMessage_publishOPks<C448>(std::vector<uint8_t> &message, const std::vector<X<C448, lime::Xtype::publicKey>> &OPks, const std::vector<uint32_t> &OPk_ids) noexcept;
		extern template void buildMessage_getPeerBundles<C448>(std::vector<uint8_t> &message, std::vector<std::string> &peer_device_ids) noexcept;
		extern template void buildMessage_getSelfOPks<C448>(std::vector<uint8_t> &message) noexcept;
#endif

	} // namespace x3dh_protocol
} // namespace lime

#endif /* lime_x3dh_protocol_hpp */
