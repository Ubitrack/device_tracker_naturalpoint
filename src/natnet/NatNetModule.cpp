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


#include "NatNetModule.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <utDataflow/ComponentFactory.h>
#include <utMath/Vector.h>
#include <utMath/Quaternion.h>
#include <utMath/Pose.h>
#include <utMath/Matrix.h>

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

boost::asio::io_service& NatNetModule::get_io_service()
{
    static boost::asio::io_service io_service;
    return io_service;
}

boost::asio::ip::udp::resolver& NatNetModule::get_resolver()
{
    static boost::asio::ip::udp::resolver resolver(get_io_service());
    return resolver;
}



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
{

	Graph::UTQLSubgraph::EdgePtr config;

	if ( subgraph->hasEdge( "Output" ) )
	  config = subgraph->getEdge( "Output" );

	if ( !config )
	{
	  UBITRACK_THROW( "NatNetTracker Pattern has no \"Output\" edge");
	}

	m_clientName = config->getAttributeString( "clientName" );

}


NatNetComponent::~NatNetComponent()
{
	LOG4CPP_INFO( logger, "Destroying NatNet component" );
}


// Performs thread-safe initialization of networking... boost::asio is not thread-safe, at least not for sharing a single socket between threads,
// see also http://www.boost.org/doc/libs/1_44_0/doc/html/boost_asio/overview/core/threads.html)
void NatNetModule::startModule()
{
	LOG4CPP_INFO( logger, "Creating NatNet network service on port " << m_moduleKey.get() );

	LOG4CPP_DEBUG( logger, "Starting network receiver thread" );
	m_pNetworkThread = boost::shared_ptr< boost::thread >( new boost::thread( boost::bind( &boost::asio::io_service::run, get_io_service() ) ) );

    if (command_socket) delete command_socket; command_socket = NULL;
    if (data_socket) delete data_socket; data_socket = NULL;

    boost::system::error_code ec;

    {
    	LOG4CPP_DEBUG( logger, "Resolving " <<  m_serverName << std::endl);

        udp::resolver::query query(udp::v4(), m_serverName, "0");
        udp::resolver::iterator result = get_resolver().resolve(query, ec);
        if (ec)
        {
        	LOG4CPP_ERROR( logger, ec.category().name() << " ERROR while resolving " << m_serverName << " : " << ec.message() << std::endl);
            return;
        }

        server_endpoint = *result;
        server_endpoint.port(PORT_COMMAND);
        LOG4CPP_DEBUG( logger, "Resolved " <<  m_serverName << " to " << server_endpoint << std::endl);
    }

    udp::endpoint client_endpoint(udp::v4(), PORT_DATA);
    if (!(m_clientName == ""))
    {
    	LOG4CPP_DEBUG( logger, "Resolving " <<  m_clientName << std::endl);
        udp::resolver::query query(udp::v4(), m_clientName, "0");
        udp::resolver::iterator result = get_resolver().resolve(query, ec);
        if (ec)
        {
        	LOG4CPP_ERROR( logger, ec.category().name() << " ERROR while resolving " << m_clientName << " : " << ec.message() << std::endl);
            return;
        }

        client_endpoint = *result;
        client_endpoint.port(PORT_DATA);
        LOG4CPP_DEBUG( logger, "Resolved " <<  m_clientName << " to " << client_endpoint << std::endl);
    }

    LOG4CPP_DEBUG( logger, "Opening data socket on " <<  client_endpoint << std::endl);
    data_socket = new udp::socket(get_io_service());
    if (data_socket->open(udp::v4(), ec))
    {
    	LOG4CPP_ERROR( logger, ec.category().name() << " ERROR while opening data socket : " << ec.message() << std::endl);
        return;
    }

    data_socket->set_option(udp::socket::reuse_address(true));
    if (data_socket->bind(client_endpoint, ec))
    {
    	LOG4CPP_ERROR( logger, ec.category().name() << " ERROR while binding data socket : " << ec.message() << std::endl);
        return;
    }

    // Join the multicast group.
    if (!(m_clientName == ""))
        data_socket->set_option(boost::asio::ip::multicast::join_group(boost::asio::ip::address::from_string(MULTICAST_ADDRESS).to_v4(), client_endpoint.address().to_v4()));
    else
        data_socket->set_option(boost::asio::ip::multicast::join_group(boost::asio::ip::address::from_string(MULTICAST_ADDRESS)));

    LOG4CPP_DEBUG( logger, "Data socket ready" << std::endl);
    start_data_receive();

    LOG4CPP_DEBUG( logger, "Opening command socket" << std::endl);

    command_socket = new udp::socket(get_io_service());
    if (command_socket->open(udp::v4(), ec))
    {
    	LOG4CPP_ERROR( logger, ec.category().name() << " ERROR while opening command socket : " << ec.message() << std::endl);
        return;
    }

    if (!(m_clientName == ""))
    {
        client_endpoint.port(0);
        if (command_socket->bind(client_endpoint, ec))
        {
        	LOG4CPP_ERROR( logger, ec.category().name() << " ERROR while binding command socket : " << ec.message() << std::endl);
            return;
        }
    }

    LOG4CPP_DEBUG( logger, "Command socket ready" << std::endl);
    start_command_receive();

    //boost::shared_pointer<sPacket> helloMsg = new sPacket;
    //helloMsg.iMessage = NAT_PING;
    //helloMsg.nDataBytes = 0;

    LOG4CPP_DEBUG( logger, "Sending hello message..." << std::endl);

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
	get_io_service().stop();

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
        LOG4CPP_ERROR( logger, ec.category().name() << " ERROR while receiving command from " << recv_command_endpoint << std::endl);
    }
    else
    {
    	LOG4CPP_TRACE( logger, "Received " << bytes_transferred << "b command from " << recv_command_endpoint << std::endl);
        sPacket& PacketIn = *recv_command_packet;
        switch (PacketIn.iMessage)
        {
        case NAT_MODELDEF:
        {
        	LOG4CPP_DEBUG( logger, "Received MODELDEF" << std::endl);
            if (serverInfoReceived)
            {
                decodeModelDef(PacketIn);
            }
            else
            {
                server_endpoint = recv_command_endpoint;
                server_endpoint.port(PORT_COMMAND);
                LOG4CPP_INFO( logger, "Requesting server info to " << server_endpoint << std::endl);
                boost::array<unsigned short, 2> helloMsg;
                helloMsg[0] = NAT_PING; helloMsg[1] = 0;
                command_socket->send_to(boost::asio::buffer(helloMsg), server_endpoint);
            }
            break;
        }
        case NAT_FRAMEOFDATA:
        {
            LOG4CPP_INFO( logger, "Received FRAMEOFDATA" << std::endl);
            if (serverInfoReceived)
            {
                decodeFrame(PacketIn);
            }
            else
            {
                server_endpoint = recv_command_endpoint;
                server_endpoint.port(PORT_COMMAND);
                LOG4CPP_INFO( logger, "Requesting server info to " << server_endpoint << std::endl);
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
            msg << std::endl;
            LOG4CPP_INFO( logger, msg);

            // request scene info
            boost::array<unsigned short, 2> reqMsg;
            reqMsg[0] = NAT_REQUEST_MODELDEF; reqMsg[1] = 0;
            command_socket->send_to(boost::asio::buffer(reqMsg), server_endpoint);
            break;
        }
        case NAT_RESPONSE:
        {
            LOG4CPP_DEBUG( logger, "Received response : " << PacketIn.Data.szData << std::endl);
            break;
        }
        case NAT_UNRECOGNIZED_REQUEST:
        {
            LOG4CPP_ERROR( logger, "Received 'unrecognized request'" << std::endl);
            break;
        }
        case NAT_MESSAGESTRING:
        {
            LOG4CPP_INFO( logger, "Received message: " << PacketIn.Data.szData << std::endl);
            break;
        }
        default:
        {
            LOG4CPP_ERROR( logger, "Received unrecognized command packet type: " << PacketIn.iMessage << std::endl);
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
        LOG4CPP_ERROR( logger, ec.category().name() << " ERROR while receiving data from " << recv_data_endpoint << std::endl);
    }
    else
    {
        LOG4CPP_DEBUG( logger, "Received " << bytes_transferred << "b data from " << recv_data_endpoint << std::endl);
        sPacket& PacketIn = *recv_data_packet;
        switch (PacketIn.iMessage)
        {
        case NAT_MODELDEF:
        {
        	LOG4CPP_DEBUG( logger, "Received MODELDEF" << std::endl);
            if (serverInfoReceived)
            {
                decodeModelDef(PacketIn);
            }
            else
            {
                server_endpoint = recv_data_endpoint;
                server_endpoint.port(PORT_COMMAND);
                LOG4CPP_INFO( logger, "Requesting server info to " << server_endpoint << std::endl);
                boost::array<unsigned short, 2> helloMsg;
                helloMsg[0] = NAT_PING; helloMsg[1] = 0;
                command_socket->send_to(boost::asio::buffer(helloMsg), server_endpoint);
            }
            break;
        }
        case NAT_FRAMEOFDATA:
        {
        	LOG4CPP_DEBUG( logger, "Received FRAMEOFDATA" << std::endl);
            if (serverInfoReceived)
            {
                decodeFrame(PacketIn);
            }
            else
            {
                server_endpoint = recv_data_endpoint;
                server_endpoint.port(PORT_COMMAND);
                LOG4CPP_INFO( logger, "Requesting server info to " << server_endpoint << std::endl);
                boost::array<unsigned short, 2> helloMsg;
                helloMsg[0] = NAT_PING; helloMsg[1] = 0;
                command_socket->send_to(boost::asio::buffer(helloMsg), server_endpoint);
            }
            break;
        }
        default:
        {
            LOG4CPP_ERROR( logger, "Received unrecognized data packet type: " << PacketIn.iMessage << std::endl);
            break;
        }
        }
    }
    start_data_receive();
}

template<class T>
static void memread(T& dest, const unsigned char*& ptr, const unsigned char*& end, const char* fieldName=NULL)
{
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
            LOG4CPP_DEBUG( logger, msg );
            ptr = end;
        }
    }
}

static void memread(const char*& dest, const unsigned char*& ptr, const unsigned char*& end, const char* fieldName=NULL)
{
    unsigned int len = 0;
    while (ptr+len < end && ptr[len])
        ++len;
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
            LOG4CPP_DEBUG( logger, msg );
            ptr = end;
        }
    }
}

template<class T>
static void memread(const T*& dest, int n, const unsigned char*& ptr, const unsigned char*& end, const char* fieldName=NULL)
{
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
            LOG4CPP_DEBUG( logger, msg );
            ptr = end;
        }
    }
}

template<>
static void memread(Ubitrack::Math::Vector3& dest, const unsigned char*& ptr, const unsigned char*& end, const char* fieldName=NULL)
{
	float dest_tmp[3];
    if (ptr + sizeof(float[3]) <= end)
    {
        dest_tmp = *(const float*)ptr;
        ptr += sizeof(float[3]);
        dest = Ubitrack::Math::Vector3(dest_tmp[0], dest_tmp[1], dest_tmp[2]);
    }
    else
    {
        dest = Ubitrack::Math::Vector3(0,0,0);
        if (ptr != end)
        {
        	std::stringstream msg;
            msg << "OptiTrackNatNet decode ERROR: end of message reached";
            if (fieldName) msg << " while reading " << fieldName << std::endl;
            LOG4CPP_DEBUG( logger, msg );
            ptr = end;
        }
    }
}

template<>
static void memread(Ubitrack::Math::Quaternion& dest, const unsigned char*& ptr, const unsigned char*& end, const char* fieldName=NULL)
{
	float dest_tmp[4];
    if (ptr + sizeof(float[4]) <= end)
    {
        dest_tmp = *(const float*)ptr;
        ptr += sizeof(float[4]);
        dest = Ubitrack::Math::Quaternion(dest_tmp[0], dest_tmp[1], dest_tmp[2], dest_tmp[3]);
    }
    else
    {
        dest = Ubitrack::Math::Quaternion(0,0,0,1);
        if (ptr != end)
        {
        	std::stringstream msg;
            msg << "OptiTrackNatNet decode ERROR: end of message reached";
            if (fieldName) msg << " while reading " << fieldName << std::endl;
            LOG4CPP_DEBUG( logger, msg );
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

    if (major <= 1 || (major == 2 && minor <= 0))
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
    memread(frame.latency, ptr,end,"latency");
    if (ptr != end)
    {
	    LOG4CPP_DEBUG( logger, "decodeFrame: extra " << end-ptr << " bytes at end of message" << std::endl);
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
            LOG4CPP_ERROR( logger, "decodeModelDef: unknown type " << type << std::endl);
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


	if ( m_running )
	{
		// XXX What's the default delay with NatNet .. needs to be measured..
		// subtract the approximate processing time of the cameras and DTrack (19ms)
		// (daniel) better synchronize the DTrack controller to a common NTP server and use "ts" fields directly.
		//          This should work well at least with NatNettrack2/3 cameras (not necessarily NatNettrack/TP)

		LOG4CPP_DEBUG( logger , "NatNet Latency: " << data->latency << std::endl);
		// substract from timestamp .. instead of constant.
		timestamp -= 19000000;

		for (int i=0; i<data->nRigids; ++i) {

			int id = bodyIdMap[data->rigids[i].ID];
		    // on the network the IDs are 0 based, in the DTrack software they are 1 based..
		    NatNetComponentKey key( id, NatNetComponentKey::target_6d );

		    // check for component
		    if ( hasComponent( key ) )
		    {
		        // generate pose
		        Ubitrack::Measurement::Pose pose( timestamp, Ubitrack::Math::Pose( data->rigids[i].rot, data->rigids[i].pos ) );

		        //send it to the component
				LOG4CPP_TRACE( logger, "Sending pose for id " << id << " using " << getComponent( key )->getName() << ": " << pose );
		        getComponent( key )->send( pose );
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
		    	boost::shared_ptr< std::vector< Ubitrack::Math::Vector < 3 > > > cloud(data->pointClouds[i].markersPos);

				Ubitrack::Measurement::PositionList pc( timestamp, cloud );

		        //send it to the component
				LOG4CPP_TRACE( logger, "Sending pose for id " << id << " using " << getComponent( key )->getName() << ": " << cloud );
		        getComponent( key )->send( cloud );
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

    NatNetModule::ComponentList components = this->getAllComponents();
    int index = 0;
	ComponentList allComponents( getAllComponents() );
	std::map<std::string, int> bodyNameIdMap;

	for ( ComponentList::iterator it = allComponents.begin(); it != allComponents.end(); it++ ) {
		if ( (*it)->getKey().m_targetType == NatNetComponentKey::target_3dcloud  ) {
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
        		LOG4CPP_WARN( logger, "Received RigidBodyDef: " << data->rigids[i].name << " but configured name does not match: " << getComponent( key )->getKey().getName() << std::endl);
    		} else {
        		LOG4CPP_INFO( logger, "Receiver connected for Rigid Body: " << data->rigids[i].name << std::endl);
    		}
    	} else {

        	if (data->rigids[i].name)
            {
        		if (bodyNameIdMap.find(data->rigids[i].name) > 0) {
        			bodyIdMap[data->rigids[i].ID] = bodyNameIdMap[data->rigids[i].name];
            		LOG4CPP_INFO( logger, "Receiver connected for Rigid Body: " << data->rigids[i].name << std::endl);
        		} else {
            		LOG4CPP_WARN( logger, "Received RigidBodyDef: " << data->rigids[i].name << " but receiver component was not found." << std::endl);
        		}

            } else {
            	LOG4CPP_WARN( logger, "Received RigidBodyDef without name and found no matching component for ID:" << data->rigids[i].ID << "." << std::endl);
            }



    	}


    }



    for (int i=0; i<data->nPointClouds; ++i) {
        if (data->pointClouds[i].name)
        {
        	if (pointcloudNameIdMap.find(data->rigids[i].name) > 0) {
        		LOG4CPP_INFO( logger, "Receiver connected for Pointcloud: " << data->pointClouds[i].name << std::endl);
        	} else {
        		LOG4CPP_WARN( logger, "Received PointCloudDef: " << data->pointClouds[i].name << " but receiver component was not found." << std::endl);
        	}
        }
    }
    // other details of interest ??
}

































void NatNetModule::HandleReceive (const boost::system::error_code err, size_t length)
{
	// save the timestamp as soon as possible
	Ubitrack::Measurement::Timestamp timestamp = Ubitrack::Measurement::now();

	if ( m_running )
	{
		// XXX What's the default delay with NatNet .. needs to be measured..
		// subtract the approximate processing time of the cameras and DTrack (19ms)
		// (daniel) better synchronize the DTrack controller to a common NTP server and use "ts" fields directly.
		//          This should work well at least with NatNettrack2/3 cameras (not necessarily NatNettrack/TP)
		timestamp -= 19000000;

		// some error chekcing
		if ( err && err != boost::asio::error::message_size )
		{
			LOG4CPP_CRIT( logger,"Error receiving from socket: \"" << err << "\"" );
			return;
		}

		if (length >= max_receive_length)
		{
			LOG4CPP_CRIT( logger, "FIXME: received more than max_receive_length bytes." );
			return;
		}

		// make receive data null terminated
		receive_data[length] = 0;

		// split the received data into lines
		std::string data (receive_data);
		std::vector<std::string> lines;
		{
			std::size_t lastpos = 0;
			std::size_t findpos = 0;
			while ( ( findpos = data.find ( "\r\n", lastpos ) ) != std::string::npos )
			{
			  std::string s = data.substr ( lastpos, ( findpos-lastpos ) );
			  lines.push_back ( s );
			  lastpos = findpos+2;
			}
		}

		// look at every line
		for (std::vector<std::string>::iterator it = lines.begin(); it != lines.end();
		   it++)
		{
			std::string line (*it);
			LOG4CPP_DEBUG( logger, "Parsing line: " << line );

			// find the first space to determine the recordtype
			std::size_t lastpos = 0;

			std::size_t findpos = line.find (" ");
			// invalid record
			if (findpos == std::string::npos)
				break;

			std::string recordType = line.substr (lastpos, findpos-lastpos);


			if (recordType == "fr")
			{
				LOG4CPP_TRACE( logger, "record type: fr" );

				// frame start
				// we found a new frame
				// eg: "fr 12345"
				lastpos = findpos+1;
				findpos = line.find (" ", lastpos);
				std::string frString = line.substr (lastpos, findpos-lastpos);
				int fr = atol ( frString.c_str() );

				timestamp = m_synchronizer.convertNativeToLocal( fr, timestamp );

			}
			else if (recordType == "ts")
			{
				LOG4CPP_TRACE( logger, "record type: ts" );

				// timestamp
				// timestamp
				// eg: "ts 37949.735000"

			}
			else if (recordType == "6d")
			{
				LOG4CPP_TRACE( logger, "record type: 6d" );

				// 6d standard bodies
				// eg: "6d 2 [0 1.000][1092.858 -662.008 -189.740 -177.4073 27.3061 -18.7763][0.841281 0.301898 0.448447 0.286007 -0.952493 0.104679 0.458744 0.040194 -0.887659] [3 1.000][242.154 -674.257 -223.429 -105.4729 16.5325 3.4309][0.956940 -0.289719 0.018103 -0.057371 -0.249893 -0.966572 0.284559 0.923914 -0.255754] "
				// or for DTrack2, note the subtle difference in the end!
				// eg: "6d 2 [0 1.000][1092.858 -662.008 -189.740 -177.4073 27.3061 -18.7763][0.841281 0.301898 0.448447 0.286007 -0.952493 0.104679 0.458744 0.040194 -0.887659] [3 1.000][242.154 -674.257 -223.429 -105.4729 16.5325 3.4309][0.956940 -0.289719 0.018103 -0.057371 -0.249893 -0.966572 0.284559 0.923914 -0.255754]"

				// retrieve amount of bodies
				lastpos = findpos + 1;
				findpos = line.find (" ", lastpos);
				int bodyCount;
				std::string count = line.substr(lastpos, findpos - lastpos);
				if ( sscanf( count.c_str(), "%d", &bodyCount ) != 1 )
				{
					LOG4CPP_TRACE( logger, "illegal line, amount of bodies inaccessible, token: " << count );
					continue;
				}
				LOG4CPP_TRACE( logger, "amount of bodies: " << bodyCount );
				for ( int bodyIdx = 0; bodyIdx < bodyCount; bodyIdx ++ )
				{
					if ( lastpos > line.length() )
					{
						LOG4CPP_TRACE( logger, "illegal line, unexpected end of line" );
						break;
					}
					// compute start pos by looking for the first '['
					if ( (lastpos = line.find ("[", lastpos)) == std::string::npos )
					{
						LOG4CPP_TRACE( logger, "illegal line, next body inaccessible" );
						break;
					}
					// compute end pos by looking for the 3rd ']'
					if ( (findpos = line.find ("] ", lastpos)) == std::string::npos )
					{
						LOG4CPP_TRACE( logger, "last body record" );
					}
					// retrieve substring for body
					std::string record = line.substr (lastpos, findpos - lastpos);
					LOG4CPP_TRACE( logger, "record for body " << bodyIdx << " : " << record );
					// parse substring
					int id;
					double qual;
					double rot[6];
					double mat[9];
					int tokenCount = sscanf (record.c_str(), "[%d %lf][%lf %lf %lf %lf %lf %lf][%lf %lf %lf %lf %lf %lf %lf %lf %lf]",
							&id, &qual, &rot[0], &rot[1], &rot[2], &rot[3], &rot[4], &rot[5],
							&mat[0], &mat[3], &mat[6], &mat[1], &mat[4], &mat[7], &mat[2], &mat[5], &mat[8]);

					if ( tokenCount != 17 ) {
						LOG4CPP_TRACE( logger, "invalid record for body " << bodyIdx );
						break;
					}
					// try to send
					trySendPose( id, NatNetComponentKey::target_6d, qual, rot, mat, timestamp );
					// update position for line search
					lastpos = findpos + 2;
				}
			}
			else if (recordType == "6df")
			{
				LOG4CPP_TRACE( logger, "record type: 6df" );

				// 6d flightsticks
				// never seen one so far

				// retrieve amount of bodies
				lastpos = findpos + 1;
				findpos = line.find (" ", lastpos);
				int bodyCount;
				std::string count = line.substr(lastpos, findpos - lastpos);
				if ( sscanf( count.c_str(), "%d", &bodyCount ) != 1 )
				{
					LOG4CPP_TRACE( logger, "illegal line, amount of bodies inaccessible, token: " << count );
					continue;
				}
				LOG4CPP_TRACE( logger, "amount of bodies: " << bodyCount );
				for ( int bodyIdx = 0; bodyIdx < bodyCount; bodyIdx ++ )
				{
					if ( lastpos > line.length() )
					{
						LOG4CPP_TRACE( logger, "illegal line, unexpected end of line" );
						break;
					}
					// compute start pos by looking for the first '['
					if ( (lastpos = line.find ("[", lastpos)) == std::string::npos )
					{
						LOG4CPP_TRACE( logger, "illegal line, next body inaccessible" );
						break;
					}
					// compute end pos by looking for the 3rd ']'
					if ( (findpos = line.find ("] ", lastpos)) == std::string::npos )
					{
						LOG4CPP_TRACE( logger, "last body record" );
					}
					// retrieve substring for body
					std::string record = line.substr (lastpos, findpos - lastpos);
					LOG4CPP_TRACE( logger, "record for body " << bodyIdx << " : " << record );
					// parse substring
					int id;
					double qual;
					int button;
					double rot[6];
					double mat[9];
					int tokenCount = sscanf (record.c_str(), "[%d %lf %d][%lf %lf %lf %lf %lf %lf][%lf %lf %lf %lf %lf %lf %lf %lf %lf]",
							&id, &qual, &button, &rot[0], &rot[1], &rot[2], &rot[3], &rot[4], &rot[5],
							&mat[0], &mat[3], &mat[6], &mat[1], &mat[4], &mat[7], &mat[2], &mat[5], &mat[8]);

					if ( tokenCount != 18 ) {
						LOG4CPP_TRACE( logger, "invalid record for body " << bodyIdx );
						break;
					}
					// try to send
					trySendPose( id, NatNetComponentKey::target_6d_flystick, qual, rot, mat, timestamp );
					// update position for line search
					lastpos = findpos + 2;
				}
			}
			else if (recordType == "6dmt")
			{
				LOG4CPP_TRACE( logger, "record type: 6dmt" );

				// 6d measurement tool

				// retrieve amount of bodies
				lastpos = findpos + 1;
				findpos = line.find (" ", lastpos);
				int bodyCount;
				std::string count = line.substr(lastpos, findpos - lastpos);
				if ( sscanf( count.c_str(), "%d", &bodyCount ) != 1 )
				{
					LOG4CPP_TRACE( logger, "illegal line, amount of bodies inaccessible, token: " << count );
					continue;
				}
				LOG4CPP_TRACE( logger, "amount of bodies: " << bodyCount );
				for ( int bodyIdx = 0; bodyIdx < bodyCount; bodyIdx ++ )
				{
					if ( lastpos > line.length() )
					{
						LOG4CPP_TRACE( logger, "illegal line, unexpected end of line" );
						break;
					}
					// compute start pos by looking for the first '['
					if ( (lastpos = line.find ("[", lastpos)) == std::string::npos )
					{
						LOG4CPP_TRACE( logger, "illegal line, next body inaccessible" );
						break;
					}
					// compute end pos by looking for the 3rd ']'
					if ( (findpos = line.find ("] ", lastpos)) == std::string::npos )
					{
						LOG4CPP_TRACE( logger, "last body record" );
					}
					// retrieve substring for body
					std::string record = line.substr (lastpos, findpos - lastpos);
					LOG4CPP_TRACE( logger, "record for body " << bodyIdx << " : " << record );
					// parse substring
					int id;
					double qual;
					int button;
					double rot[6];
					double mat[9];
					int tokenCount = sscanf (record.c_str(), "[%d %lf %d][%lf %lf %lf][%lf %lf %lf %lf %lf %lf %lf %lf %lf]",
							&id, &qual, &button, &rot[0], &rot[1], &rot[2],
							&mat[0], &mat[3], &mat[6], &mat[1], &mat[4], &mat[7], &mat[2], &mat[5], &mat[8]);

					if ( tokenCount != 15 ) {
						LOG4CPP_TRACE( logger, "invalid record for body " << bodyIdx );
						break;
					}
					// try to send
					trySendPose( id, NatNetComponentKey::target_6d_measurement_tool, qual, rot, mat, timestamp );
					// update position for line search
					lastpos = findpos + 2;
				}
			}
			else if (recordType == "6dmtr")
			{
				LOG4CPP_TRACE( logger, "record type: 6dmtr" );

				// 6d standard bodies
				// eg: "6d 2 [0 1.000][1092.858 -662.008 -189.740 -177.4073 27.3061 -18.7763][0.841281 0.301898 0.448447 0.286007 -0.952493 0.104679 0.458744 0.040194 -0.887659] [3 1.000][242.154 -674.257 -223.429 -105.4729 16.5325 3.4309][0.956940 -0.289719 0.018103 -0.057371 -0.249893 -0.966572 0.284559 0.923914 -0.255754] "
				// or for DTrack2, note the subtle difference in the end!
				// eg: "6d 2 [0 1.000][1092.858 -662.008 -189.740 -177.4073 27.3061 -18.7763][0.841281 0.301898 0.448447 0.286007 -0.952493 0.104679 0.458744 0.040194 -0.887659] [3 1.000][242.154 -674.257 -223.429 -105.4729 16.5325 3.4309][0.956940 -0.289719 0.018103 -0.057371 -0.249893 -0.966572 0.284559 0.923914 -0.255754]"

				// retrieve amount of bodies
				lastpos = findpos + 1;
				findpos = line.find (" ", lastpos);
				int bodyCount;
				std::string count = line.substr(lastpos, findpos - lastpos);
				if ( sscanf( count.c_str(), "%d", &bodyCount ) != 1 )
				{
					LOG4CPP_TRACE( logger, "illegal line, amount of bodies inaccessible, token: " << count );
					continue;
				}
				LOG4CPP_TRACE( logger, "amount of bodies: " << bodyCount );
				for ( int bodyIdx = 0; bodyIdx < bodyCount; bodyIdx ++ )
				{
					if ( lastpos > line.length() )
					{
						LOG4CPP_TRACE( logger, "illegal line, unexpected end of line" );
						break;
					}
					// compute start pos by looking for the first '['
					if ( (lastpos = line.find ("[", lastpos)) == std::string::npos )
					{
						LOG4CPP_TRACE( logger, "illegal line, next body inaccessible" );
						break;
					}
					// compute end pos by looking for the 3rd ']'
					if ( (findpos = line.find ("] ", lastpos)) == std::string::npos )
					{
						LOG4CPP_TRACE( logger, "last body record" );
					}
					// retrieve substring for body
					std::string record = line.substr (lastpos, findpos - lastpos);
					LOG4CPP_TRACE( logger, "record for body " << bodyIdx << " : " << record );
					// parse substring
					int id;
					double qual;
					double rot[6];
					double mat[9];
					int tokenCount = sscanf (record.c_str(), "[%d %lf][%lf %lf %lf][%lf %lf %lf %lf %lf %lf %lf %lf %lf]",
							&id, &qual, &rot[0], &rot[1], &rot[2],
							&mat[0], &mat[3], &mat[6], &mat[1], &mat[4], &mat[7], &mat[2], &mat[5], &mat[8]);

					if ( tokenCount != 14 ) {
						LOG4CPP_TRACE( logger, "invalid record for body " << bodyIdx );
						break;
					}
					// try to send
					trySendPose( id, NatNetComponentKey::target_6d_measurement_tool_reference, qual, rot, mat, timestamp );
					// update position for line search
					lastpos = findpos + 2;
				}
			}
			else if (recordType == "gl")
			{
				LOG4CPP_TRACE( logger, "record type: gl" );

				// fingertracker
				// determine the number of found hands

				lastpos = findpos+1;
				findpos = line.find (" ", lastpos);
				std::string numOfRecordsString = line.substr (lastpos, findpos-lastpos);
				int numOfRecords = atol ( numOfRecordsString.c_str() );

				lastpos = findpos+1;
				//      while ((findpos = line.find ("] ", lastpos)) != std::string::npos) {

				for (int i=0; i<numOfRecords; ++i)
				{
					std::string record = line.substr (lastpos);

					// std::cout << "record: " << record << std::endl;

					int id;
					double qual;
					int side;
					int numFingers;

					double rot[6];
					double mat[9];
					double phalanx[6];

					const char* recordC = record.c_str();
					int readChars;

					sscanf (recordC,
							"[%d %lf %d %d]%n",
							&id, &qual, &side, &numFingers, &readChars);
					lastpos += readChars;
					recordC += readChars;

					NatNetComponentKey::FingerSide fingerSide = (side==0)?(NatNetComponentKey::side_left):(NatNetComponentKey::side_right);

					// read and send hand pose
					sscanf (recordC,
							"[%lf %lf %lf][%lf %lf %lf %lf %lf %lf %lf %lf %lf]%n",
							&rot[0], &rot[1], &rot[2c],
							&mat[0], &mat[3], &mat[6], &mat[1], &mat[4], &mat[7], &mat[2], &mat[5], &mat[8],
							&readChars );
					lastpos += readChars;
					recordC += readChars;

					trySendPose( id, NatNetComponentKey::target_finger, qual, rot, mat, timestamp, NatNetComponentKey::finger_hand, fingerSide );

					// read and send thumb
					sscanf (recordC,
							"[%lf %lf %lf][%lf %lf %lf %lf %lf %lf %lf %lf %lf][%lf %lf %lf %lf %lf %lf]%n",
							&rot[0], &rot[1], &rot[2],
							&mat[0], &mat[3], &mat[6], &mat[1], &mat[4], &mat[7], &mat[2], &mat[5], &mat[8],
							&phalanx[0], &phalanx[1], &phalanx[2], &phalanx[3], &phalanx[4], &phalanx[5],
							&readChars );
					lastpos += readChars;
					recordC += readChars;

					trySendPose( id, NatNetComponentKey::target_finger, qual, rot, mat, timestamp, NatNetComponentKey::finger_thumb, fingerSide );

					// read and send index
					sscanf (recordC,
							"[%lf %lf %lf][%lf %lf %lf %lf %lf %lf %lf %lf %lf][%lf %lf %lf %lf %lf %lf]%n",
							&rot[0], &rot[1], &rot[2],
							&mat[0], &mat[3], &mat[6], &mat[1], &mat[4], &mat[7], &mat[2], &mat[5], &mat[8],
							&phalanx[0], &phalanx[1], &phalanx[2], &phalanx[3], &phalanx[4], &phalanx[5],
							&readChars );
					lastpos += readChars;
					recordC += readChars;

					trySendPose( id, NatNetComponentKey::target_finger, qual, rot, mat, timestamp, NatNetComponentKey::finger_index, fingerSide );

					// read and send middle
					sscanf (recordC,
							"[%lf %lf %lf][%lf %lf %lf %lf %lf %lf %lf %lf %lf][%lf %lf %lf %lf %lf %lf]%n",
							&rot[0], &rot[1], &rot[2],
							&mat[0], &mat[3], &mat[6], &mat[1], &mat[4], &mat[7], &mat[2], &mat[5], &mat[8],
							&phalanx[0], &phalanx[1], &phalanx[2], &phalanx[3], &phalanx[4], &phalanx[5],
							&readChars );
					lastpos += readChars;
					recordC += readChars;

					lastpos += 1;

					trySendPose( id, NatNetComponentKey::target_finger, qual, rot, mat, timestamp, NatNetComponentKey::finger_middle, fingerSide );
				}
			}
			else if (recordType == "3d")
			{
				LOG4CPP_TRACE( logger, "record type: 3d" );

				// 3dof marker
				// determine the number of found 3dof marker

				lastpos = findpos+1;
				findpos = line.find (" ", lastpos);
				std::string numOfRecordsString = line.substr (lastpos, findpos-lastpos);
				int numOfRecords = atol ( numOfRecordsString.c_str() );

				lastpos = findpos+1;

				boost::shared_ptr< std::vector< Ubitrack::Math::Vector< 3 > > > cloud( new std::vector< Ubitrack::Math::Vector< 3 > > );

				for (int i=0; i<numOfRecords; ++i)
				{
					std::string record = line.substr (lastpos);

					int id;
					double qual;

					double rot[3];

					const char* recordC = record.c_str();
					int readChars;

					sscanf (recordC,
							"[%d %lf][%lf %lf %lf]%n",
							&id, &qual, &rot[0], &rot[1], &rot[2], &readChars);
					lastpos += readChars;
					recordC += readChars;

					Ubitrack::Math::Vector< 3 > pos (rot);
					cloud->push_back( pos / 1000.0 );

					lastpos += 1;
				}

				trySendPose( cloud, timestamp );
			}
			else if (recordType == "6dcal")
			{
				LOG4CPP_TRACE( logger, "record type: 6dcal" );

				// 6d calibrated bodies
				// the number of 6d calibrated bodies in the software
				// eg: "6dcal 6"
			}
			else
			{
				LOG4CPP_TRACE( logger, "unknown record type" );

				// unknown record type
				// silently ignore
			}
		}
	}

	// restart receiving new packet
	m_pSocket->async_receive_from (
		boost::asio::buffer ( receive_data, max_receive_length ),
		sender_endpoint,
		boost::bind (&NatNetModule::HandleReceive, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));

}


void NatNetModule::trySendPose( int id, NatNetComponentKey::TargetType type, PointCloudData& cdata, Ubitrack::Measurement::Timestamp ts)
{


	//	boost::shared_ptr< std::vector< Ubitrack::Math::Vector < 3 > > > cloud, Ubitrack::Measurement::Timestamp ts )
	boost::shared_ptr< std::vector< Ubitrack::Math::Vector < 3 > > > cloud(cdata.markersPos);

	NatNetComponentKey key( id, NatNetComponentKey::target_3dcloud );

	if ( hasComponent( key ) )
	{
		Ubitrack::Measurement::PositionList pc( ts, cloud );

		getComponent( key )->getPort().send( pc );
	}
// 	else
// 	{
// 		std::cout << ":-(" << std::endl;
// 		for (ComponentMap::iterator it = m_componentMap.begin();
// 			 it != m_componentMap.end(); ++it)
// 		{
// 			boost::shared_ptr< NatNetComponent > cc(it->second);
// 			NatNetComponentKey keyc = cc->getKey();
// 			std::cout << "Comp: " << keyc.getBody() << " "
// 					  << keyc.getTargetType() << " "
// 					  << keyc.getFingerType() << " "
// 					  << keyc.getFingerSide() << std::endl;
// 			std::cout << "Wank: " << key.getBody() << " "
// 					  << key.getTargetType() << " "
// 					  << key.getFingerType() << " "
// 					  << key.getFingerSide() << std::endl;
// 		}
// 	}


}

void NatNetModule::trySendPose( int id, NatNetComponentKey::TargetType type, RigidData& rdata, Ubitrack::Measurement::Timestamp ts )
{
    // on the network the IDs are 0 based, in the DTrack software they are 1 based..
    NatNetComponentKey key( id, type );

    // check for component
    if ( hasComponent( key ) )
    {
        // generate pose
        Ubitrack::Measurement::Pose pose( ts, Ubitrack::Math::Pose( rdata.rot, rdata.pos ) );

        //send it to the component
		LOG4CPP_TRACE( logger, "Sending pose for id " << id << " using " << getComponent( key )->getName() << ": " << pose );
        getComponent( key )->getPort().send( pose );
    }
	else {
		LOG4CPP_TRACE( logger, "No component for body id " << id+1 );
	}
}


// register module at factory
UBITRACK_REGISTER_COMPONENT( ComponentFactory* const cf ) {
	cf->registerModule< NatNetModule > ( "NatNetTracker" );
}

} } // namespace Ubitrack::Drivers
