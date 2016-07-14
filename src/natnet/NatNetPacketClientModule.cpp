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


#include "NatNetPacketClientModule.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <utDataflow/ComponentFactory.h>
#include <utMath/Vector.h>
#include <utMath/Quaternion.h>
#include <utMath/Pose.h>
#include <utMath/Matrix.h>
#include <utUtil/TracingProvider.h>

#include <boost/array.hpp>

#include <log4cpp/Category.hh>


/* The pattern (allows "trackable" edges);
<Pattern name="NatNet6D">
	<Input>
		<Node name="NatNet"/>
		<Node name="Body"/>
		<Edge name="Config" source="NatNet" destination="Body">
			<Predicate>trackable=="NatNet"</Predicate>
		</Edge>
	</Input>

	<Output>
		<Edge name="NatNetToTarget" source="NatNet" destination="Body">
			<Attribute name="type" value="6D"/>
			<Attribute name="mode" value="push"/>
			<AttributeExpression name="natnetType">Config.natnetType</AttributeExpression>
			<AttributeExpression name="natnetBodyId">Config.natnetBodyId</AttributeExpression>
		</Edge>
	</Output>

	<DataflowConfiguration>
		<UbitrackLib class="NatNetTracker"/>
	</DataflowConfiguration>
</Pattern>

Without the pattern, a valid dataflow description or SRG definition is:

<Pattern name="NatNet6D" id="NatNetToBody1">

	<Output>
		<Node name="NatNet" id="NatNet">
			<Attribute name="natnetPort" value="5000"/>
		</Node>
		<Node name="Body" id="Body1"/>
		<Edge name="NatNetToTarget" source="NatNet" destination="Body">
			<Attribute name="natnetBodyId" value="3"/>
			<Attribute name="natnetBodyName" value="Trackable3"/>
			<Attribute name="natnetType" value="6d"/>
			<Attribute name="type" value="6D"/>
			<Attribute name="mode" value="push"/>
		</Edge>
	</Output>

	<DataflowConfiguration>
		<UbitrackLib class="NatNetTracker"/>
	</DataflowConfiguration>

</Pattern>
*/

namespace Ubitrack { namespace Drivers {



static log4cpp::Category& logger( log4cpp::Category::getInstance( "Drivers.NatNet" ) );

//////////
// From PacketClient.cpp in NatNet SDK

#define MAX_NAMELENGTH              256

// NATNET message ids
#define NAT_PING                    0
#define NAT_PINGRESPONSE            1
#define NAT_REQUEST                 2
#define NAT_RESPONSE                3
#define NAT_REQUEST_MODELDEF        4
#define NAT_MODELDEF                5
#define NAT_REQUEST_FRAMEOFDATA     6
#define NAT_FRAMEOFDATA             7
#define NAT_MESSAGESTRING           8
#define NAT_UNRECOGNIZED_REQUEST    100
//#define UNDEFINED                   999999.9999
#if defined(__GNUC__)
#pragma pack(push,1)
#endif

// sender
struct sSender
{
    char szName[MAX_NAMELENGTH];            // sending app's name
    unsigned char Version[4];               // sending app's version [major.minor.build.revision]
    unsigned char NatNetVersion[4];         // sending app's NatNet version [major.minor.build.revision]

};

struct sPacket
{
    unsigned short iMessage;                // message ID (e.g. NAT_FRAMEOFDATA)
    unsigned short nDataBytes;              // Num bytes in payload
    union
    {
        unsigned char  cData[20000];
        char           szData[20000];
        unsigned long  lData[5000];
        float          fData[5000];
        sSender        Sender;
    } Data;                                 // Payload

};

#if defined(__GNUC__)
#pragma pack(pop)
#endif

#define MULTICAST_ADDRESS		"239.255.42.99"     // IANA, local network
#define PORT_COMMAND            1510
#define PORT_DATA  			    1511                // Default multicast group

//////////

using boost::asio::ip::udp;

boost::asio::ip::udp::resolver& NatNetModule::get_resolver()
{
    static boost::asio::ip::udp::resolver resolver(ios);
    return resolver;
}

boost::asio::io_service NatNetModule::ios;

NatNetModule::NatNetModule( const NatNetModuleKey& moduleKey, boost::shared_ptr< Graph::UTQLSubgraph > subgraph, FactoryHelper* pFactory )
	: Module< NatNetModuleKey, NatNetComponentKey, NatNetModule, NatNetComponent >( moduleKey, pFactory )
	, m_synchronizer( 100 ) // assume 100 Hz for timestamp synchronization
    , command_socket(NULL)
    , data_socket(NULL)
    , recv_command_packet(new sPacket)
    , recv_data_packet(new sPacket)
    , serverInfoReceived(false)
    , modelInfoReceived(false)
    , m_serverName(m_moduleKey.get())
    , m_clientName("")
	, m_defaultLatency(10000000)
    , m_counter(0)
    , m_lastTimestamp(0)
{

	Graph::UTQLSubgraph::NodePtr config;

	if ( subgraph->hasNode( "OptiTrack" ) )
	  config = subgraph->getNode( "OptiTrack" );

	if ( !config )
	{
	  UBITRACK_THROW( "NatNetTracker Pattern has no \"OptiTrack\" node");
	}

	m_clientName = config->getAttributeString( "clientName" );
	config->getAttributeData("latency", m_defaultLatency);

}


NatNetComponent::~NatNetComponent()
{
	LOG4CPP_INFO( logger, "Destroying NatNet component" );
}


// Performs thread-safe initialization of networking... boost::asio is not thread-safe, at least not for sharing a single socket between threads,
// see also http://www.boost.org/doc/libs/1_44_0/doc/html/boost_asio/overview/core/threads.html)
void NatNetModule::startModule()
{


	LOG4CPP_INFO( logger, "Creating NatNet for server: " << m_moduleKey.get() );

    if (command_socket) delete command_socket; command_socket = NULL;
    if (data_socket) delete data_socket; data_socket = NULL;

    boost::system::error_code ec;

    {
    	LOG4CPP_DEBUG( logger, "Resolving " <<  m_serverName );

        udp::resolver::query query(udp::v4(), m_serverName, "0");
        udp::resolver::iterator result = get_resolver().resolve(query, ec);
        if (ec)
        {
        	LOG4CPP_ERROR( logger, ec.category().name() << " ERROR while resolving " << m_serverName << " : " << ec.message() );
            return;
        }

        server_endpoint = *result;
        server_endpoint.port(PORT_COMMAND);
        LOG4CPP_DEBUG( logger, "Resolved " <<  m_serverName << " to " << server_endpoint );
    }


    udp::endpoint mc_endpoint(boost::asio::ip::address::from_string( MULTICAST_ADDRESS ), PORT_DATA);
    udp::endpoint client_endpoint(udp::v4(), PORT_DATA);


    if (!(m_clientName == ""))
    {
    	LOG4CPP_DEBUG( logger, "Resolving " <<  m_clientName );
        udp::resolver::query query(udp::v4(), m_clientName, "0");
        udp::resolver::iterator result = get_resolver().resolve(query, ec);
        if (ec)
        {
        	LOG4CPP_ERROR( logger, ec.category().name() << " ERROR while resolving " << m_clientName << " : " << ec.message() );
            return;
        }

        client_endpoint = *result;
        client_endpoint.port(PORT_DATA);
        LOG4CPP_DEBUG( logger, "Resolved " <<  m_clientName << " to " << client_endpoint );
    }

    LOG4CPP_DEBUG( logger, "Opening data socket on " <<  client_endpoint );
    data_socket = new udp::socket(ios);
    if (data_socket->open(udp::v4(), ec))
    {
    	LOG4CPP_ERROR( logger, ec.category().name() << " ERROR while opening data socket : " << ec.message() );
        return;
    }

#ifdef _WINDOWS
	// on windows, one cannot bind a socket to a multicast address ..
    data_socket->set_option(udp::socket::reuse_address(true));
    if (data_socket->bind(client_endpoint, ec))
    {
    	LOG4CPP_ERROR( logger, ec.category().name() << " ERROR while binding data socket : " << ec.message() );
        return;
    }
#else
	// on Linux (others not tested) we need to bind to the multicast address to receive data
    data_socket->set_option(udp::socket::reuse_address(true));
    if (data_socket->bind(mc_endpoint, ec))
    {
    	LOG4CPP_ERROR( logger, ec.category().name() << " ERROR while binding data socket : " << ec.message() );
        return;
    }
#endif
    // Join the multicast group.
    if (!(m_clientName == ""))
        data_socket->set_option(boost::asio::ip::multicast::join_group(boost::asio::ip::address::from_string(MULTICAST_ADDRESS).to_v4(), client_endpoint.address().to_v4()));
    else
        data_socket->set_option(boost::asio::ip::multicast::join_group(boost::asio::ip::address::from_string(MULTICAST_ADDRESS).to_v4()));

    LOG4CPP_DEBUG( logger, "Data socket ready. PacketSize = " << sizeof(sPacket) );
    start_data_receive();

    LOG4CPP_DEBUG( logger, "Opening command socket" );

    command_socket = new udp::socket(ios);
    if (command_socket->open(udp::v4(), ec))
    {
    	LOG4CPP_ERROR( logger, ec.category().name() << " ERROR while opening command socket : " << ec.message() );
        return;
    }

    if (!(m_clientName == ""))
    {
        client_endpoint.port(0);
        if (command_socket->bind(client_endpoint, ec))
        {
        	LOG4CPP_ERROR( logger, ec.category().name() << " ERROR while binding command socket : " << ec.message() );
            return;
        }
    }

    LOG4CPP_DEBUG( logger, "Command socket ready" );
    start_command_receive();



	LOG4CPP_DEBUG( logger, "Starting network receiver thread" );
	m_pNetworkThread = boost::shared_ptr< boost::thread >( new boost::thread(  boost::bind( &boost::asio::io_service::run, &(this->ios) ) ) );

    //boost::shared_pointer<sPacket> helloMsg = new sPacket;
    //helloMsg.iMessage = NAT_PING;
    //helloMsg.nDataBytes = 0;

    LOG4CPP_DEBUG( logger, "Sending hello message..." );

    boost::array<unsigned short, 2> helloMsg;
    helloMsg[0] = NAT_PING; helloMsg[1] = 0;
    command_socket->send_to(boost::asio::buffer(helloMsg), server_endpoint);


}


// Performs thread-safe cleanup of networking... (boost::asio is not thread-safe, at least not for sharing a single socket between threads,
// see also http://www.boost.org/doc/libs/1_44_0/doc/html/boost_asio/overview/core/threads.html)
void NatNetModule::stopModule()
{
	LOG4CPP_INFO( logger, "Stopping NatNet network service on port " << m_moduleKey.get() );

	// XXX any other cleanup necessary ??

	// stop the network thread
	ios.stop();

	LOG4CPP_DEBUG( logger, "Waiting for network thread to stop..." );
	m_pNetworkThread->join();

	Module<NatNetModuleKey, NatNetComponentKey, NatNetModule, NatNetComponent>::stopModule();
}


NatNetModule::~NatNetModule()
{
    delete command_socket;
    delete data_socket;
    delete recv_command_packet;
    delete recv_data_packet;

}



void NatNetModule::start_command_receive()
{
    command_socket->async_receive_from(
        boost::asio::buffer(recv_command_packet, sizeof(sPacket)), recv_command_endpoint,
        boost::bind(&NatNetModule::handle_command_receive, this,
                boost::asio::placeholders::error,
                boost::asio::placeholders::bytes_transferred));
}

void NatNetModule::handle_command_receive(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
    if (ec)
    {
		LOG4CPP_ERROR( logger, ec.category().name() << " ERROR while receiving command from " << recv_command_endpoint << ": " << ec.message());
    }
    else
    {
    	LOG4CPP_TRACE( logger, "Received " << bytes_transferred << "b command from " << recv_command_endpoint );
        sPacket& PacketIn = *recv_command_packet;
        switch (PacketIn.iMessage)
        {
        case NAT_MODELDEF:
        {
        	LOG4CPP_TRACE( logger, "Received MODELDEF" );
            if (serverInfoReceived)
            {
                decodeModelDef(PacketIn);
            }
            else
            {
                server_endpoint = recv_command_endpoint;
                server_endpoint.port(PORT_COMMAND);
                LOG4CPP_INFO( logger, "Requesting server info to " << server_endpoint );
                boost::array<unsigned short, 2> helloMsg;
                helloMsg[0] = NAT_PING; helloMsg[1] = 0;
                command_socket->send_to(boost::asio::buffer(helloMsg), server_endpoint);
            }
            break;
        }
        case NAT_FRAMEOFDATA:
        {
        	LOG4CPP_TRACE( logger, "Received FRAMEOFDATA" );
            if (serverInfoReceived)
            {
                decodeFrame(PacketIn);
            }
            else
            {
                server_endpoint = recv_command_endpoint;
                server_endpoint.port(PORT_COMMAND);
                LOG4CPP_INFO( logger, "Requesting server info to " << server_endpoint );
                boost::array<unsigned short, 2> helloMsg;
                helloMsg[0] = NAT_PING; helloMsg[1] = 0;
                command_socket->send_to(boost::asio::buffer(helloMsg), server_endpoint);
            }
            break;
        }
        case NAT_PINGRESPONSE:
        {
            serverInfoReceived = true;
            serverString = PacketIn.Data.Sender.szName;
            for(int i=0; i<4; i++)
            {
                natNetVersion[i] = PacketIn.Data.Sender.NatNetVersion[i];
                serverVersion[i] = PacketIn.Data.Sender.Version[i];
            }

            std::stringstream msg;

            msg << "Connected to server \"" << serverString << "\" v" << (int)serverVersion[0];
            if (serverVersion[1] || serverVersion[2] || serverVersion[3])
            	msg << "." << (int)serverVersion[1];
            if (serverVersion[2] || serverVersion[3])
            	msg << "." << (int)serverVersion[2];
            if (serverVersion[3])
            	msg << "." << (int)serverVersion[3];
            msg << " protocol v" << (int)natNetVersion[0];
            if (natNetVersion[1] || natNetVersion[2] || natNetVersion[3])
            	msg << "." << (int)natNetVersion[1];
            if (natNetVersion[2] || natNetVersion[3])
            	msg << "." << (int)natNetVersion[2];
            if (natNetVersion[3])
            	msg << "." << (int)natNetVersion[3];
            LOG4CPP_INFO( logger, msg.str());

            // request scene info
            boost::array<unsigned short, 2> reqMsg;
            reqMsg[0] = NAT_REQUEST_MODELDEF; reqMsg[1] = 0;
            command_socket->send_to(boost::asio::buffer(reqMsg), server_endpoint);
            break;
        }
        case NAT_RESPONSE:
        {
        	LOG4CPP_TRACE( logger, "Received response : " << PacketIn.Data.szData );
            break;
        }
        case NAT_UNRECOGNIZED_REQUEST:
        {
        	LOG4CPP_TRACE( logger, "Received 'unrecognized request'" );
            break;
        }
        case NAT_MESSAGESTRING:
        {
            LOG4CPP_INFO( logger, "Received message: " << PacketIn.Data.szData );
            break;
        }
        default:
        {
            LOG4CPP_ERROR( logger, "Received unrecognized command packet type: " << PacketIn.iMessage );
            break;
        }
        }
    }
    start_command_receive();
}

void NatNetModule::start_data_receive()
{
    data_socket->async_receive_from(
        boost::asio::buffer(recv_data_packet, sizeof(sPacket)), recv_data_endpoint,
        boost::bind(&NatNetModule::handle_data_receive, this,
                boost::asio::placeholders::error,
                boost::asio::placeholders::bytes_transferred));
}

void NatNetModule::handle_data_receive(const boost::system::error_code& ec,
        std::size_t bytes_transferred)
{
    if (ec)
    {
        LOG4CPP_ERROR( logger, ec.category().name() << " ERROR while receiving data from " << recv_data_endpoint );
    }
    else
    {
        LOG4CPP_TRACE( logger, "Received " << bytes_transferred << "b data from " << recv_data_endpoint );
        sPacket& PacketIn = *recv_data_packet;
        switch (PacketIn.iMessage)
        {
        case NAT_MODELDEF:
        {
        	LOG4CPP_TRACE( logger, "Received MODELDEF" );
            if (serverInfoReceived)
            {
                decodeModelDef(PacketIn);
            }
            else
            {
                server_endpoint = recv_data_endpoint;
                server_endpoint.port(PORT_COMMAND);
                LOG4CPP_INFO( logger, "Requesting server info to " << server_endpoint );
                boost::array<unsigned short, 2> helloMsg;
                helloMsg[0] = NAT_PING; helloMsg[1] = 0;
                command_socket->send_to(boost::asio::buffer(helloMsg), server_endpoint);
            }
            break;
        }
        case NAT_FRAMEOFDATA:
        {
        	LOG4CPP_TRACE( logger, "Received FRAMEOFDATA" );
            if (serverInfoReceived)
            {
                decodeFrame(PacketIn);
            }
            else
            {
                server_endpoint = recv_data_endpoint;
                server_endpoint.port(PORT_COMMAND);
                LOG4CPP_INFO( logger, "Requesting server info to " << server_endpoint );
                boost::array<unsigned short, 2> helloMsg;
                helloMsg[0] = NAT_PING; helloMsg[1] = 0;
                command_socket->send_to(boost::asio::buffer(helloMsg), server_endpoint);
            }
            break;
        }
        default:
        {
            LOG4CPP_ERROR( logger, "Received unrecognized data packet type: " << PacketIn.iMessage );
            break;
        }
        }
    }
    start_data_receive();
}

template<class T>
static void memread(T& dest, const unsigned char*& ptr, const unsigned char*& end, const char* fieldName=NULL)
{
	if (fieldName)
	    	LOG4CPP_TRACE( logger, "memread1 " << fieldName << " size " << sizeof(T));

    if (ptr + sizeof(T) <= end)
    {
        dest = *(const T*)ptr;
        ptr += sizeof(T);
    }
    else
    {
        memset(&dest,0,sizeof(T));
        if (ptr != end)
        {
        	std::stringstream msg;
            msg << "OptiTrackNatNet decode ERROR: end of message reached";
            if (fieldName) msg << " while reading " << fieldName << std::endl;
            LOG4CPP_DEBUG( logger, msg.str() );
            ptr = end;
        }
    }
}

static void memread(const char*& dest, const unsigned char*& ptr, const unsigned char*& end, const char* fieldName=NULL)
{

    unsigned int len = 0;
    while (ptr+len < end && ptr[len])
        ++len;

    if (fieldName)
    	LOG4CPP_TRACE( logger, "memread2 " << fieldName << " size " << len);

    if (end-ptr > len)
    {
        dest = (const char*) ptr;
        ptr += len+1;
    }
    else
    {
        dest = "";
        if (ptr != end)
        {
        	std::stringstream msg;
            msg << "OptiTrackNatNet decode ERROR: end of message reached";
            if (fieldName) msg << " while reading string " << fieldName << std::endl;
            LOG4CPP_DEBUG( logger, msg.str() );
            ptr = end;
        }
    }
}

template<class T>
static void memread(const T*& dest, int n, const unsigned char*& ptr, const unsigned char*& end, const char* fieldName=NULL)
{
	if (fieldName)
	    	LOG4CPP_TRACE( logger, "memread3 " << fieldName << " elements " << n << " size " << sizeof(T));
    if (n <= 0)
        dest = NULL;
    else if (ptr + n*sizeof(T) <= end)
    {
        dest = (const T*)ptr;
        ptr += n*sizeof(T);
    }
    else
    {
        dest = NULL;
        if (ptr != end)
        {
        	std::stringstream msg;
        	msg << "OptiTrackNatNet decode ERROR: end of message reached";
            if (fieldName) msg << " while reading " << n << " values for array " << fieldName << std::endl;
            LOG4CPP_DEBUG( logger, msg.str() );
            ptr = end;
        }
    }
}

static void memread(Ubitrack::Math::Vector< float, 3 >& dest, const unsigned char*& ptr, const unsigned char*& end, const char* fieldName=NULL)
{
	if (fieldName)
	    	LOG4CPP_TRACE( logger, "memread vec3 " << fieldName << " size " << sizeof(float)*3);

	if (ptr + sizeof(float)*3 <= end)
    {
        dest = Ubitrack::Math::Vector3d(((const float*)ptr)[0], ((const float*)ptr)[1], ((const float*)ptr)[2]);
        ptr += sizeof(float)*3;
    }
    else
    {
        dest = Ubitrack::Math::Vector3d(0,0,0);
        if (ptr != end)
        {
        	std::stringstream msg;
            msg << "OptiTrackNatNet decode ERROR: end of message reached";
            if (fieldName) msg << " while reading " << fieldName << std::endl;
            LOG4CPP_DEBUG( logger, msg.str() );
            ptr = end;
        }
    }
}

static void memread(Ubitrack::Math::Vector< float, 4 >& dest, const unsigned char*& ptr, const unsigned char*& end, const char* fieldName=NULL)
{
	if (fieldName)
	    	LOG4CPP_TRACE( logger, "memread vec4 " << fieldName << " size " << sizeof(float)*4);

	if (ptr + sizeof(float)*4 <= end)
    {
        dest = Ubitrack::Math::Vector4d(((const float*)ptr)[0], ((const float*)ptr)[1], ((const float*)ptr)[2], ((const float*)ptr)[3]);
        ptr += sizeof(float)*4;
    }
    else
    {
        dest = Ubitrack::Math::Vector4d(0,0,0,1);
        if (ptr != end)
        {
        	std::stringstream msg;
            msg << "OptiTrackNatNet decode ERROR: end of message reached";
            if (fieldName) msg << " while reading " << fieldName << std::endl;
            LOG4CPP_DEBUG( logger, msg.str() );
            ptr = end;
        }
    }
}

static void memread(std::vector< Ubitrack::Math::Vector< float, 3 > >& dest, int n, const unsigned char*& ptr, const unsigned char*& end, const char* fieldName=NULL)
{
	if (fieldName)
	    	LOG4CPP_TRACE( logger, "memread vec3* " << fieldName << " elements " << n << " size " << sizeof(float)*3);
    if (n <= 0)
        dest.clear();
    else if (ptr + n*sizeof(float)*3 <= end)
    {
    	dest.resize(n);
    	for (int i=0; i < n; i++) {
    		dest.at(i) = Ubitrack::Math::Vector< float, 3 >(((const float*)ptr)[0],((const float*)ptr)[1],((const float*)ptr)[2]);
            ptr += sizeof(float)*3;
    	}
    }
    else
    {
        dest.clear();
        if (ptr != end)
        {
        	std::stringstream msg;
        	msg << "OptiTrackNatNet decode ERROR: end of message reached";
            if (fieldName) msg << " while reading " << n << " values for array " << fieldName << std::endl;
            LOG4CPP_DEBUG( logger, msg.str() );
            ptr = end;
        }
    }
}


void NatNetModule::decodeFrame(const sPacket& data)
{
    const int major = natNetVersion[0];
    const int minor = natNetVersion[1];

    FrameData frame;

    int nTrackedMarkers = 0;
    int nOtherMarkers = 0;

    const unsigned char *ptr = data.Data.cData;
    const unsigned char *end = ptr + data.nDataBytes;

    memread(frame.frameNumber,ptr,end,"frameNumber");

    memread(frame.nPointClouds,ptr,end,"nPointClouds");
    if (frame.nPointClouds <= 0)
        frame.pointClouds = NULL;
    else
    {
        frame.pointClouds = new PointCloudData[frame.nPointClouds];
        for (int iP = 0; iP < frame.nPointClouds; ++iP)
        {
            PointCloudData& pdata = frame.pointClouds[iP];
            memread(pdata.name,ptr,end,"pointCloud.name");
            memread(pdata.nMarkers,ptr,end,"pointCloud.nMarkers");
            nTrackedMarkers += pdata.nMarkers;
            memread(pdata.markersPos, pdata.nMarkers, ptr, end, "pointCloud.markersPos");
        }
    }
    memread(frame.nOtherMarkers,ptr,end,"nOtherMarkers");
    nOtherMarkers += frame.nOtherMarkers;
    memread(frame.otherMarkersPos, frame.nOtherMarkers, ptr,end,"otherMarkersPos");

    memread(frame.nRigids,ptr,end,"nRigids");
    if (frame.nRigids <= 0)
        frame.rigids = NULL;
    else
    {
        frame.rigids = new RigidData[frame.nRigids];
        for (int iR = 0; iR < frame.nRigids; ++iR)
        {
            RigidData& rdata = frame.rigids[iR];
            memread(rdata.ID,ptr,end,"rigid.ID");
            memread(rdata.pos,ptr,end,"rigid.pos");
            memread(rdata.rot,ptr,end,"rigid.rot");
            memread(rdata.nMarkers, ptr,end,"rigid.nMarkers");
            nTrackedMarkers += rdata.nMarkers;
            memread(rdata.markersPos, rdata.nMarkers, ptr,end,"rigid.markersPos");
            if (major < 2)
            {
                rdata.markersID = NULL;
                rdata.markersSize = NULL;
                rdata.meanError = -1.0f;
            }
            else
            {
                memread(rdata.markersID, rdata.nMarkers, ptr,end,"rigid.markersID");
                memread(rdata.markersSize, rdata.nMarkers, ptr,end,"rigid.markersSize");
                memread(rdata.meanError, ptr,end,"rigid.meanError");
            }
        }
    }

    if ((major <= 1) || ((major == 2) && (minor <= 0)))
    {
        frame.nSkeletons = 0;
        frame.skeletons = NULL;
    }
    else
    {
        memread(frame.nSkeletons,ptr,end,"nSkeletons");
        if (frame.nSkeletons <= 0)
            frame.skeletons = NULL;
        else
        {
            frame.skeletons = new SkeletonData[frame.nSkeletons];
            for (int iS = 0; iS < frame.nSkeletons; ++iS)
            {
                SkeletonData& sdata = frame.skeletons[iS];
                memread(sdata.ID,ptr,end,"skeleton.ID");
                memread(sdata.nRigids, ptr,end,"skeleton.nRigids");
                if (sdata.nRigids <= 0)
                    sdata.rigids = NULL;
                else
                {
                    sdata.rigids = new RigidData[sdata.nRigids];
                    for (int iR = 0; iR < sdata.nRigids; ++iR)
                    {
                        RigidData& rdata = sdata.rigids[iR];
                        memread(rdata.ID,ptr,end,"rigid.ID");
                        memread(rdata.pos,ptr,end,"rigid.pos");
                        memread(rdata.rot,ptr,end,"rigid.rot");
                        memread(rdata.nMarkers, ptr,end,"rigid.nMarkers");
                        nTrackedMarkers += rdata.nMarkers;
                        memread(rdata.markersPos, rdata.nMarkers, ptr,end,"rigid.markersPos");
                        memread(rdata.markersID, rdata.nMarkers, ptr,end,"rigid.markersID");
                        memread(rdata.markersSize, rdata.nMarkers, ptr,end,"rigid.markersSize");
                        memread(rdata.meanError, ptr,end,"rigid.meanError");
                    }
                }
            }
        }
    }

    if ((major > 2) || ((major == 2) && (minor >= 3))) {
		memread(frame.nLabeledMarkers, ptr, end, "frame.nLabeledMarkers");
		if (frame.nLabeledMarkers <= 0)
			frame.labeledMarkersPos = NULL;
		else
		{
			frame.labeledMarkersPos = new MarkerData[frame.nLabeledMarkers];
			for (int iM = 0; iM < frame.nLabeledMarkers; iM++) {
				MarkerData& mdata = frame.labeledMarkersPos[iM];
				memread(mdata.ID, ptr, end, "mdata.ID");
				memread(mdata.pos, ptr, end, "mdata.pos");
				memread(mdata.markersSize, ptr, end, "mdata.markersSize");
			}
		}
	}
	else
	{
		frame.nLabeledMarkers = 0;
		frame.labeledMarkersPos = NULL;
	}

    memread(frame.latency, ptr,end,"latency");

    memread(frame.timecode, ptr,end,"timecode");
    memread(frame.timecodeSub, ptr,end,"timecodeSub");
    memread(frame.eod, ptr,end,"eod");

    if (ptr != end)
    {
	    LOG4CPP_DEBUG( logger, "decodeFrame: extra " << end-ptr << " bytes at end of message" );
    }


    processFrame(&frame);



    if (frame.pointClouds)
    {
        delete[] frame.pointClouds;
    }
    if (frame.rigids)
    {
        delete[] frame.rigids;
    }
    if (frame.skeletons)
    {
        for (int i=0; i<frame.nSkeletons; ++i)
        {
            if (frame.skeletons[i].rigids)
                delete[] frame.skeletons[i].rigids;
        }
        delete[] frame.skeletons;
    }
}



void NatNetModule::decodeModelDef(const sPacket& data)
{
    const int major = natNetVersion[0];
    //const int minor = natNetVersion[1];

    ModelDef model;
    std::vector<PointCloudDef> pointClouds;
    std::vector<RigidDef> rigids;
    std::vector<SkeletonDef> skeletons;

    const unsigned char *ptr = data.Data.cData;
    const unsigned char *end = ptr + data.nDataBytes;

    int nDatasets = 0;
    memread(nDatasets,ptr,end,"nDatasets");

    LOG4CPP_TRACE( logger, "ModelDef nDatasets: " << nDatasets);

    for(int i=0; i < nDatasets; i++)
    {
        int type = 0;
        memread(type,ptr,end,"type");
        switch(type)
        {
        case 0: // point cloud
        {
            PointCloudDef pdef;
            memread(pdef.name,ptr,end,"name");
            memread(pdef.nMarkers,ptr,end,"nMarkers");
            if (pdef.nMarkers <= 0)
                pdef.markers = NULL;
            else
            {
                pdef.markers = new PointCloudDef::Marker[pdef.nMarkers];
                for (int j=0; j<pdef.nMarkers; ++j)
                {
                    memread(pdef.markers[j].name,ptr,end,"markers.name");
                }
            }
            pointClouds.push_back(pdef);
            break;
        }
        case 1: // rigid
        {
            RigidDef rdef;
            if(major >= 2)
                memread(rdef.name,ptr,end,"rigid.name");
            else
                rdef.name = NULL;
            memread(rdef.ID,ptr,end,"rigid.ID");
            memread(rdef.parentID,ptr,end,"rigid.parentID");
            memread(rdef.offset,ptr,end,"rigid.offset");
            rigids.push_back(rdef);
            break;
        }
        case 2: // skeleton
        {
            SkeletonDef sdef;
            memread(sdef.name,ptr,end,"skeleton.name");
            memread(sdef.nRigids,ptr,end,"skeleton.nRigids");
            if (sdef.nRigids <= 0)
                sdef.rigids = NULL;
            else
            {
                sdef.rigids = new RigidDef[sdef.nRigids];
                for (int j=0; j<sdef.nRigids; ++j)
                {
                    RigidDef& rdef = sdef.rigids[j];
                    memread(rdef.name,ptr,end,"skeleton.rigid.name");
                    memread(rdef.ID,ptr,end,"skeleton.rigid.ID");
                    memread(rdef.parentID,ptr,end,"skeleton.rigid.parentID");
                    memread(rdef.offset,ptr,end,"skeleton.rigid.offset");
                }
            }
            skeletons.push_back(sdef);
            break;
        }
        default:
        {
            LOG4CPP_ERROR( logger, "decodeModelDef: unknown type " << type );
        }
        }
    }

    model.nPointClouds = pointClouds.size();
    model.pointClouds = (pointClouds.size() > 0) ? &(pointClouds[0]) : NULL;
    model.nRigids = rigids.size();
    model.rigids = (rigids.size() > 0) ? &(rigids[0]) : NULL;
    model.nSkeletons = skeletons.size();
    model.skeletons = (skeletons.size() > 0) ? &(skeletons[0]) : NULL;


    processModelDef(&model);


    for (int i=0; i<model.nPointClouds; ++i)
    {
        if (model.pointClouds[i].markers)
            delete[] model.pointClouds[i].markers;
    }

    for (int i=0; i<model.nSkeletons; ++i)
    {
        if (model.skeletons[i].rigids)
            delete[] model.skeletons[i].rigids;
    }
}

void NatNetModule::processFrame(const FrameData* data)
{
	// save the timestamp as soon as possible
	// XXXthis could be done earlier, but would require passing the timestamp down
	// .. but would not matter too much if time-delay estimation is in place
	Ubitrack::Measurement::Timestamp timestamp = Ubitrack::Measurement::now();

	m_lastTimestamp = timestamp;
	++m_counter;


	// use synchronizer to correct timestamps
	// XXX is this correct ??
	timestamp = m_synchronizer.convertNativeToLocal( m_counter, timestamp );

	if ( m_running )
	{
		// XXX What's the default delay with NatNet .. needs to be measured..
		// subtract the approximate processing time of the cameras and DTrack (19ms)
		// (daniel) better synchronize the DTrack controller to a common NTP server and use "ts" fields directly.
		//          This should work well at least with ARTtrack2/3 cameras (not necessarily ARTtrack/TP)

		/* latency is now managed in the components, since modules cannot have ports
		LOG4CPP_DEBUG( logger , "NatNet Latency: " << data->latency );
		// should substract latency + network from timestamp .. instead of constant.
		// was 19,000,000
		timestamp -= m_latency;
		*/

		for (int i=0; i<data->nRigids; ++i) {

			int id = bodyIdMap[data->rigids[i].ID];
		    // on the network the IDs are 0 based, in the DTrack software they are 1 based..
		    NatNetComponentKey key( id, NatNetComponentKey::target_6d );

		    // check for non-recognized markers (all sub-markers are at (0,0,0))
		    bool is_recognized = true;
		    if (data->rigids[i].nMarkers > 0) {
			    if ((data->rigids[i].markersPos[0](0) == 0) &&
			    		(data->rigids[i].markersPos[0](1) == 0) &&
			    		(data->rigids[i].markersPos[0](1) == 0)) {
					for (int j=1; j < data->rigids[i].nMarkers; j++) {
						if (data->rigids[i].markersPos[j-1] == data->rigids[i].markersPos[j]) {
							is_recognized = false;
						}
					}
			    }
		    }

		    if (!is_recognized) {
		    	continue;
		    }

		    // check for component
		    if ( hasComponent( key ) )
		    {
		        // generate pose
		        Ubitrack::Measurement::Pose pose( timestamp,
		        	Ubitrack::Math::Pose(
		        		Ubitrack::Math::Quaternion((double)data->rigids[i].rot(0), (double)data->rigids[i].rot(1), (double)data->rigids[i].rot(2), (double)data->rigids[i].rot(3)),
		        		Ubitrack::Math::Vector< double, 3 >((double)data->rigids[i].pos(0), (double)data->rigids[i].pos(1), (double)data->rigids[i].pos(2))
	        		)
		        );

		        //send it to the component
				LOG4CPP_DEBUG( logger, "Sending pose for id " << id << " using " << getComponent( key )->getName() << ": " << pose << " mean error: " << data->rigids[i].meanError );
#ifdef ENABLE_EVENT_TRACING
                TRACEPOINT_MEASUREMENT_CREATE(getComponent( key )->getEventDomain(), timestamp, getComponent( key )->getName().c_str(), "PoseTracking")
#endif
				static_cast<NatNetRigidBodyReceiverComponent*>(getComponent( key ).get())->send( pose );
		    }
			else {
				LOG4CPP_TRACE( logger, "No component for body id " << id );
			}
		}

		for (int i=0; i<data->nPointClouds; ++i) {

			int id = pointcloudNameIdMap[data->pointClouds[i].name];
		    NatNetComponentKey key( id, NatNetComponentKey::target_3dcloud );

		    // check for component
		    if ( hasComponent( key ) )
		    {

		    	// possible without copying ??
		    	boost::shared_ptr< std::vector< Ubitrack::Math::Vector< double, 3 > > > cloud(new std::vector< Ubitrack::Math::Vector< double, 3 > >(data->pointClouds[i].nMarkers));
		    	for (int j=0; j < data->pointClouds[i].nMarkers; j++) {
		    		cloud->at(j) = Ubitrack::Math::Vector< double, 3 >(data->pointClouds[i].markersPos[j]);
		    	}

				Ubitrack::Measurement::PositionList pc( timestamp, cloud );

		        //send it to the component
				LOG4CPP_DEBUG( logger, "Sending pose for id " << id << " using " << getComponent( key )->getName() << ": " << cloud );
#ifdef ENABLE_EVENT_TRACING
                TRACEPOINT_MEASUREMENT_CREATE(getComponent( key )->getEventDomain(), timestamp, getComponent( key )->getName().c_str(), "MarkerTracking")
#endif
				static_cast<NatNetPointCloudReceiverComponent*>(getComponent( key ).get())->send( pc );
		    }
			else {
				LOG4CPP_TRACE( logger, "No component for cloud name " << data->pointClouds[i].name );
			}
		}

	}
}



void NatNetModule::processModelDef(const ModelDef* data)
{

	// reset mappings
	bodyIdMap.clear();
	pointcloudNameIdMap.clear();

//    NatNetModule::ComponentList components = this->getAllComponents();
    int index = 0;
	ComponentList allComponents( getAllComponents() );
	std::map<std::string, int> bodyNameIdMap;

	for ( ComponentList::iterator it = allComponents.begin(); it != allComponents.end(); it++ ) {
		if ( (*it)->getKey().getTargetType() == NatNetComponentKey::target_3dcloud  ) {
			// pointclouds require a name, enforced in configuration
			pointcloudNameIdMap[(*it)->getKey().getName()] = (*it)->getKey().getBody();
		} else {
			if (!(*it)->getKey().getName().empty()) {
				bodyNameIdMap[(*it)->getKey().getName()] = (*it)->getKey().getBody();
			}
		}
	}


	for (int i=0; i<data->nRigids; ++i) {

    	NatNetComponentKey key( data->rigids[i].ID, NatNetComponentKey::target_6d );

    	if ( hasComponent( key ) ) {

    		bodyIdMap[data->rigids[i].ID] = getComponent( key )->getKey().getBody();

    		if (getComponent( key )->getKey().getName() != data->rigids[i].name) {
        		LOG4CPP_WARN( logger, "Received RigidBodyDef: " << data->rigids[i].name << " but configured name does not match: " << getComponent( key )->getKey().getName() );
    		} else {
        		LOG4CPP_INFO( logger, "Receiver connected for Rigid Body: " << data->rigids[i].name );
    		}
    	} else {

        	if (data->rigids[i].name)
            {
        		if (bodyNameIdMap.count(data->rigids[i].name) > 0) {
        			bodyIdMap[data->rigids[i].ID] = bodyNameIdMap[data->rigids[i].name];
            		LOG4CPP_INFO( logger, "Receiver connected for Rigid Body: " << data->rigids[i].name );
        		} else {
            		LOG4CPP_WARN( logger, "Received RigidBodyDef: " << data->rigids[i].name << " but receiver component was not found." );
        		}

            } else {
            	LOG4CPP_WARN( logger, "Received RigidBodyDef without name and found no matching component for ID:" << data->rigids[i].ID << "." );
            }



    	}


    }



    for (int i=0; i<data->nPointClouds; ++i) {
        if (data->pointClouds[i].name)
        {
        	if (pointcloudNameIdMap.count(data->pointClouds[i].name) > 0) {
        		LOG4CPP_INFO( logger, "Receiver connected for Pointcloud: " << data->pointClouds[i].name );
        	} else {
        		LOG4CPP_WARN( logger, "Received PointCloudDef: " << data->pointClouds[i].name << " but receiver component was not found." );
        	}
        }
    }
    // other details of interest ??
}



boost::shared_ptr< NatNetModule::ComponentClass > NatNetModule::createComponent( const std::string&, const std::string& name, boost::shared_ptr< Graph::UTQLSubgraph> subgraph,
	const ComponentKey& key, ModuleClass* pModule )
{
	NatNetComponentKey::TargetType tt;
	std::string typeString = subgraph->m_DataflowAttributes.getAttributeString("natnetType");
	if (typeString.empty()) {
		// no explicit natnet target type information. so we assume 6D
		tt = NatNetComponentKey::target_6d;
	} else {
		if (typeString == "6d")
			tt = NatNetComponentKey::target_6d;
		else if (typeString == "3dcloud") {
			tt = NatNetComponentKey::target_3dcloud;
		} else
			UBITRACK_THROW(
					"NatNet target with unknown target type: "
							+ typeString);
	}
	if ( tt == NatNetComponentKey::target_6d ) {
		return boost::shared_ptr< ComponentClass >( new NatNetRigidBodyReceiverComponent( name, subgraph, key, pModule ) );
	} else {
		return boost::shared_ptr< ComponentClass >( new NatNetPointCloudReceiverComponent( name, subgraph, key, pModule ) );
	}

}

void NatNetComponent::receiveLatency( const Measurement::Distance& m ) {
		double l = *m;
		LOG4CPP_DEBUG( logger , "NatNetComponent received new latency measurement in ms: " << l );
		// convert ms to timestamp offset
		m_latency = (long int)(1000000.0 * l);
};



std::ostream& operator<<( std::ostream& s, const NatNetComponentKey& k )
{
        s << "NatNetComponent[ " << k.getBody() << " "
                             << k.getTargetType() << " ]";
        return s;
}

// register module at factory
UBITRACK_REGISTER_COMPONENT( ComponentFactory* const cf ) {
	cf->registerModule< NatNetModule > ( "NatNetTracker" );
}

} } // namespace Ubitrack::Drivers
