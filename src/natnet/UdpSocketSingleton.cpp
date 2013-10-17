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


#include "UdpSocketSingleton.h"
#include <log4cpp/Category.hh>

namespace Ubitrack { namespace Drivers {

static log4cpp::Category& logger( log4cpp::Category::getInstance( "Drivers.UdpSocketSingleton" ) );

// declare static members
UdpSocketSingleton::SingletonMap UdpSocketSingleton::s_singletonMap;

std::map< int, int > UdpSocketSingleton::occupationCountMap;


UdpSocketSingleton::UdpSocketSingleton( int port )
{
	using boost::asio::ip::udp;

	// create io service
	m_pIoService.reset( new boost::asio::io_service );

	udp::endpoint endpoint( udp::v4(), port );

	// open receive socket
	LOG4CPP_DEBUG( logger, "Creating UDP receiver for port " << endpoint.port() );
	m_pReceiveSocket.reset( new udp::socket( *m_pIoService, endpoint ) );
}

void UdpSocketSingleton::startNetwork( void )
{
	// network thread runs until io_service is interrupted
	LOG4CPP_DEBUG( logger, "Starting network receiver thread" );
	m_pNetworkThread = boost::shared_ptr< boost::thread >( new boost::thread( boost::bind( &boost::asio::io_service::run, m_pIoService.get() ) ) );
}

UdpSocketSingleton::~UdpSocketSingleton()
{
	LOG4CPP_INFO( logger, "Shutting down UDP receiver for port " << m_pReceiveSocket->local_endpoint().port() );

	// close socket
	boost::mutex::scoped_lock l( m_socketMutex );
	m_pReceiveSocket->close();

	// stop the network thread
	m_pIoService->stop();

	LOG4CPP_DEBUG( logger, "Waiting for network thread to stop..." );
	m_pNetworkThread->join();
}


void UdpSocketSingleton::releaseSingleton()
{
	using boost::asio::ip::udp;

	// Retrieve port
	udp::endpoint endpoint = m_pReceiveSocket->local_endpoint();

	LOG4CPP_DEBUG( logger, "releaseSingleton() called for port " << endpoint.port() );

	boost::recursive_mutex::scoped_lock l( m_receiverMutex );
	m_receiverFunction = 0;

	// One occupant less for this port
	occupationCountMap[endpoint.port()]--;

	// Destroy shared instance if not used any more
	if ( occupationCountMap[endpoint.port()] == 0 )
	{
		LOG4CPP_DEBUG( logger, "Network service on port " << endpoint.port() << " no more needed. Destroying..." );

		s_singletonMap.erase( endpoint.port() );
	}
}


void UdpSocketSingleton::send_to( const std::string& data, const boost::asio::ip::udp::endpoint& to )
{
	LOG4CPP_TRACE( logger, "Sending to " << to << ": " << data );
	boost::mutex::scoped_lock l( m_socketMutex );
	m_pReceiveSocket->send_to( boost::asio::buffer( data.c_str(), data.size() ), to );

	// Note: If there are strange threading bugs, try to use async_send_to!
}


void UdpSocketSingleton::ReceiveProxy( const boost::system::error_code err, size_t length )
{
	// check for cancelling
	if ( err == boost::asio::error::operation_aborted )
		return;

	boost::recursive_mutex::scoped_lock l( m_receiverMutex );

	if ( m_receiverFunction )
		m_receiverFunction( err, length );
	else
		// drop packet
		LOG4CPP_DEBUG( logger, "silently dropping received UDP packet (no receiver)" );
}


boost::shared_ptr< UdpSocketSingleton > UdpSocketSingleton::getSingleton( int port )
{
	LOG4CPP_DEBUG( logger, "getSingleton() called for port " << port );

	// Get existent or new pointer object for port
	boost::shared_ptr< UdpSocketSingleton >& p( s_singletonMap[ port ] );

	if ( !p ) {
		LOG4CPP_DEBUG( logger, "Network service on port " << port << " non-existent. Creating..." );

		p.reset( new UdpSocketSingleton( port ) );
	}

	// One more occupant for this port
	occupationCountMap[port]++;

	return p;
}


} } // namespace Ubitrack::Drivers
