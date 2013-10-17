/*
 * Ubitrack - Library for Ubiquitous Tracking
 * Copyright 2006, Technische Universitaet Muenchen, and individual
 * contributors as indicated by the @authors tag. See the
 * copyright.txt in the distribution for a full listing of individual
 * contributors.
 *
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this software; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA, or see the FSF site: http://www.fsf.org.
 */


/**
 * @ingroup driver_components
 * @file
 * Network2Singleton
 *
 * A singleton object that holds different important networking resources that should not be destroyed
 * when modules are deleted and re-instantiated.
 *
 * @author Daniel Pustka <pustka@in.tum.de>
 */

#ifndef _UdpSocketSingleton_H_
#define _UdpSocketSingleton_H_

// on windows, asio must be included before anything that possible includes windows.h
// don't ask why.
#include <boost/asio.hpp>

#include <map>

#include <boost/utility.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/function.hpp>
#include <boost/bind.hpp>
#include <boost/thread.hpp>

namespace Ubitrack { namespace Drivers {

/**
 * Singleton class for network resources required by a single socket.
 */
class UdpSocketSingleton
	: protected boost::noncopyable
{
protected:
	/** constructor */
	UdpSocketSingleton( int port );

public:
	/** destruktor */
	~UdpSocketSingleton();

	/** Occupy the singleton. An instance will be created upon the first occupation. */
	static boost::shared_ptr< UdpSocketSingleton > getSingleton( int port );

	/** Start the network thread. Call after registering callbacks, to enable operation. */
	void startNetwork( void );

	/**
	  * Releases occupancy of the singleton. The single instance will be destroyed upon
	  * release of the last occupation. The amout of calls to this method must
	  * correspond _exactly_ to the amout of calls to getSingleton() for proper cleanup.
	  */
	void releaseSingleton();

	/** accessor method for components */
	boost::asio::io_service& getIoService()
	{ return *m_pIoService; }

	/** sets the receiver */
	template < class Buffer >
	void async_receive_from( const Buffer& buffer, boost::asio::ip::udp::endpoint& to, boost::function< void ( boost::system::error_code, size_t ) > function )
	{
		{
			boost::recursive_mutex::scoped_lock l( m_receiverMutex );
			m_receiverFunction = function;
		}

		boost::mutex::scoped_lock l( m_socketMutex );
		m_pReceiveSocket->async_receive_from( buffer, to,
											  boost::bind( &UdpSocketSingleton::ReceiveProxy, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred ) );
	}

	/** sends udp packets */
	void send_to( const std::string& data, const boost::asio::ip::udp::endpoint& to );

protected:

	/** proxies receive requests */
	void ReceiveProxy( const boost::system::error_code err, size_t length );

	/** singleton map type definition */
	typedef std::map< int, boost::shared_ptr< UdpSocketSingleton > > SingletonMap;

	/** static map of singletons by endpoint */
	static SingletonMap s_singletonMap;

	/** static map to count the occupants of each endpoint */
	static std::map< int, int > occupationCountMap;

	/** Boost::ASIO IO Service */
	boost::shared_ptr< boost::asio::io_service > m_pIoService;

	/** thread running the IO service */
	boost::shared_ptr< boost::thread > m_pNetworkThread;

	/** socket for receiving data */
	boost::shared_ptr< boost::asio::ip::udp::socket > m_pReceiveSocket;

	/** mutex for socket access */
	boost::mutex m_socketMutex;

	/** functor to call when data is received */
	boost::function< void ( boost::system::error_code, size_t ) > m_receiverFunction;

	/** mutex for receiver functor */
	boost::recursive_mutex m_receiverMutex;

};


} } // namespace Ubitrack::Drivers

#endif
