/*
	lime_mutex.hpp
	@author Johan Pascal
	@copyright	Copyright (C) 2019  Belledonne Communications SARL

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
#ifndef lime_mutex_hpp
#define lime_mutex_hpp

// use the portable mutex defined in bctoolbox/port.h
#include <bctoolbox/port.h>

namespace lime {
	/** @brief Remap the C interface of bctoolbox portable mutex in a C++ class.
	 * The constructor has a boolean switch to just do nothing if set to false
	 */
	class LimeMutex {
		private :
			bctbx_mutex_t m_mutex; // Hold the actual mutext
			const bool m_enabled; // a boolean to active the mutex, set only at construction
		public :
			/**
			 * @brief Mutex initialisation
			 *
			 * @param[in]	multithread	An optional boolean, set to true by default, if set to false, the mutex is not activated
			 */
			LimeMutex(const bool multithread=true) : m_enabled(multithread) {
				if (multithread) {
					bctbx_mutex_init(&m_mutex, NULL);
				}
			}
			~LimeMutex() { if (m_enabled) bctbx_mutex_destroy(&m_mutex);} // destructor destroys it
			void lock() { if (m_enabled) bctbx_mutex_lock(&m_mutex);} // lock the mutex
			void unlock() { if (m_enabled) bctbx_mutex_unlock(&m_mutex);} // unlock the mutex
	};
}
#endif //lime_mutex_hpp

