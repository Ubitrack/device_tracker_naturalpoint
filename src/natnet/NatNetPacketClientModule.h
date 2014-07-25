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
 * NatNet driver
 * This file contains the driver component to
 * talk to the NatNet infrared tracking system.
 *
 * The driver is build from one module to handle
 * the sockets communicationa and compontens for
 * each tracked object.
 *
 * The received data is sent via a push interface.
 *
 * @author Manuel Huber <huberma@in.tum.de>
 */
#ifndef __NatNetModule_h_INCLUDED__
#define __NatNetModule_h_INCLUDED__

#include <string>
#include <cstdlib>


// on windows, asio must be included before anything that possible includes windows.h
// don't ask why.
#include <boost/asio.hpp>

#include <iostream>
#include <map>
#include <boost/array.hpp>
#include <boost/utility.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/function.hpp>
#include <boost/bind.hpp>
#include <boost/thread.hpp>

#include <utDataflow/PushSupplier.h>
#include <utDataflow/PushConsumer.h>
#include <utDataflow/Component.h>
#include <utDataflow/Module.h>
#include <utMeasurement/Measurement.h>
#include <utMeasurement/TimestampSync.h>

namespace Ubitrack { namespace Drivers {


/// internal message buffer class, as defined in NatNet SDK
struct sPacket;

/// decoded definition of tracked objects
struct ModelDef;

/// decoded frame of tracked data
struct FrameData;



using namespace Dataflow;

// forward declaration
class NatNetComponent;
class NatNetRigidBodyReceiverComponent;
class NatNetPointCloudReceiverComponent;

/**
 * Module key for natnet.
 * Represents the port number on which to listen.
 */
MAKE_NODEATTRIBUTEKEY_DEFAULT( NatNetModuleKey, std::string, "OptiTrack", "serverName", "natnet.local" );


/**
 * Component key for natnet.
 * Represents the body number
 */
class NatNetComponentKey
{
public:
    enum TargetType { target_6d, target_3dcloud };

	// still ugly refactor natnet driver sometime..
	// construct from configuration
	NatNetComponentKey( boost::shared_ptr< Graph::UTQLSubgraph > subgraph )
	: m_body( 0 )
    , m_name( "" )
	, m_targetType( target_6d )
	{
		Graph::UTQLSubgraph::EdgePtr config;

	  if ( subgraph->hasEdge( "Output" ) )
		  config = subgraph->getEdge( "Output" );

	  if ( !config )
	  {
		  UBITRACK_THROW( "NatNetTracker Pattern has no \"Output\" edge");
	  }

	  config->getAttributeData( "natnetBodyId", m_body );
	  config->getAttributeData( "natnetBodyName", m_name );

	  if (( m_body <= 0 ))
            UBITRACK_THROW( "Missing or invalid \"natnetBodyId\" or \"natnetBodyName\" attribute on \"Output\" edge" );


	  // type of the component
	  std::string typeString = subgraph->m_DataflowAttributes.getAttributeString( "natnetType" );
	  if ( typeString.empty() )
	  {
	      // no explicit natnet target type information. so we assume 6D
	      m_targetType = target_6d;
	  }
	  else
	  {
	      if ( typeString == "6d" )
			  m_targetType = target_6d;
	      else if ( typeString == "3dcloud" )
		  {
			  m_targetType = target_3dcloud;
			  if ( m_name.empty() )
		            UBITRACK_THROW( "Missing or invalid \"natnetBodyName\" attribute on \"Output\" for PointCloud target" );
		  }
	      else
			  UBITRACK_THROW( "NatNet target with unknown target type: " + typeString );
	  }

	}

	// construct from body number
	NatNetComponentKey( int a )
		: m_body( a )
		, m_name( "" )
        , m_targetType( target_6d )
 	{}

    // construct from body number and target type
    NatNetComponentKey( int a, TargetType t )
        : m_body( a )
		, m_name( "" )
        , m_targetType( t )
    {}

	int getBody() const
	{
		return m_body;
	}

	std::string getName() const
	{
		return m_name;
	}

	void setName(std::string name)
	{
		m_name = name;
	}

    TargetType getTargetType() const
    {
        return m_targetType;
    }

	// less than operator for map
	bool operator<( const NatNetComponentKey& b ) const
    {
        if ( m_targetType == b.m_targetType )
			return m_body < b.m_body;
        else
            return m_targetType < b.m_targetType;
    }

protected:
	int m_body;
	std::string m_name;
	TargetType m_targetType;
};


/**
 * Module for NatNet tracker.
 * Does all the work
 */
class NatNetModule
	: public Module< NatNetModuleKey, NatNetComponentKey, NatNetModule, NatNetComponent >
{
public:
	/** UTQL constructor */
	NatNetModule( const NatNetModuleKey& key, boost::shared_ptr< Graph::UTQLSubgraph >, FactoryHelper* pFactory );

	/** destructor */
	~NatNetModule();

	virtual void startModule();

	virtual void stopModule();

	/** thread method */
    void HandleReceive (const boost::system::error_code err, size_t length);

	inline long int getDefaultLatency() {
		return m_defaultLatency;
	}
	
protected:

	Measurement::TimestampSync m_synchronizer;


	/** measurments received */
	std::size_t m_counter;

	/** Timestamp of the last received measurement */
	Ubitrack::Measurement::Timestamp m_lastTimestamp;

	/** create the components **/
	boost::shared_ptr< ComponentClass > createComponent( const std::string&, const std::string& name, boost::shared_ptr< Graph::UTQLSubgraph> subgraph,
				const ComponentKey& key, ModuleClass* pModule );


private:

	boost::asio::ip::udp::endpoint server_endpoint;
    boost::asio::ip::udp::socket* command_socket;
    boost::asio::ip::udp::socket* data_socket;

	/** thread running the IO service */
	boost::shared_ptr< boost::thread > m_pNetworkThread;

    sPacket* recv_command_packet;
    boost::asio::ip::udp::endpoint recv_command_endpoint;

    sPacket* recv_data_packet;
    boost::asio::ip::udp::endpoint recv_data_endpoint;

    void start_command_receive();
    void handle_command_receive(const boost::system::error_code& error,
            std::size_t bytes_transferred);

    void start_data_receive();
    void handle_data_receive(const boost::system::error_code& error,
            std::size_t bytes_transferred);

    void decodeFrame(const sPacket& data);
    void decodeModelDef(const sPacket& data);

    virtual void processFrame(const FrameData* data);
    virtual void processModelDef(const ModelDef* data);

    std::string serverString;
    unsigned char serverVersion[4]; // sending app's version [major.minor.build.revision]
    unsigned char natNetVersion[4]; // sending app's NatNet version [major.minor.build.revision]

    std::map<int, int> bodyIdMap;
    std::map<std::string, int> pointcloudNameIdMap;

    bool serverInfoReceived;
    bool modelInfoReceived;

    static boost::asio::io_service ios;
    static boost::asio::ip::udp::resolver& get_resolver();

    std::string m_serverName;
    std::string m_clientName;

	long int m_defaultLatency;
};

std::ostream& operator<<( std::ostream& s, const NatNetComponentKey& k );

/**
 * Component for NatNet tracker.
 * Does nothing but provide a push port

 * @TODO: make this two separate components for 6d/3dlist
 */
class NatNetComponent : public NatNetModule::Component {
public:
	/** constructor */
	NatNetComponent( const std::string& name, boost::shared_ptr< Graph::UTQLSubgraph > subgraph, const NatNetComponentKey& componentKey, NatNetModule* pModule )
		: NatNetModule::Component( name, componentKey, pModule )
		, m_latencyPort("Latency", *this, boost::bind( &NatNetComponent::receiveLatency, this, _1 ) )
		, m_latency(pModule->getDefaultLatency())
	{}

	template< class EventType >
	void send( const EventType& rEvent ) {
		UBITRACK_THROW("Not Implemented.");
	};

	void receiveLatency( const Measurement::Distance& m );

	/** destructor */
	~NatNetComponent();
	
protected:
	PushConsumer< Ubitrack::Measurement::Distance > m_latencyPort;
	long int m_latency;


};

class NatNetRigidBodyReceiverComponent : public NatNetComponent {
public:
	/** constructor */
	NatNetRigidBodyReceiverComponent( const std::string& name, boost::shared_ptr< Graph::UTQLSubgraph > subgraph, const NatNetComponentKey& componentKey, NatNetModule* pModule )
		: NatNetComponent( name, subgraph, componentKey, pModule )
		, m_port( "Output", *this )
	{}
	
	inline void send( const Ubitrack::Measurement::Pose& rEvent ) {
		m_port.send(Ubitrack::Measurement::Pose(rEvent.time() -  m_latency, rEvent));
	}

protected:
	// the port is the only member
	PushSupplier< Ubitrack::Measurement::Pose > m_port;
};

class NatNetPointCloudReceiverComponent : public NatNetComponent {
public:
	/** constructor */
	NatNetPointCloudReceiverComponent( const std::string& name, boost::shared_ptr< Graph::UTQLSubgraph > subgraph, const NatNetComponentKey& componentKey, NatNetModule* pModule )
		: NatNetComponent( name, subgraph, componentKey, pModule )
		, m_port( "Output", *this )
	{}

	inline void send( const Ubitrack::Measurement::PositionList& rEvent ) {
		m_port.send(Ubitrack::Measurement::PositionList(rEvent.time() -  m_latency, rEvent));
	}

protected:
	// the port is the only member
	PushSupplier< Ubitrack::Measurement::PositionList > m_port;
};



struct PointCloudDef
{
    const char* name;
    int nMarkers;
    struct Marker
    {
        const char* name;
    };
    Marker* markers;
};

struct RigidDef
{
    const char* name;
    int ID;
    int parentID;
    Ubitrack::Math::Vector< float, 3 > offset;
};

struct SkeletonDef
{
    const char* name;
    int ID;
    int nRigids;
    RigidDef* rigids;
};

struct ModelDef
{
    int nPointClouds;
    PointCloudDef* pointClouds;
    int nRigids;
    RigidDef* rigids;
    int nSkeletons;
    SkeletonDef* skeletons;
};

struct PointCloudData
{
    const char* name;
    int nMarkers;
    std::vector< Ubitrack::Math::Vector< float, 3 > > markersPos;
};

struct RigidData
{
    int ID;
    Ubitrack::Math::Vector< float, 3 > pos;
    Ubitrack::Math::Vector< float, 4 > rot;
    int nMarkers;
    std::vector< Ubitrack::Math::Vector< float, 3 > > markersPos;
    const int* markersID; // optional (2.0+)
    const float* markersSize; // optional (2.0+)
    float meanError; // optional (2.0+)
};

struct MarkerData
{
    int ID;
    Ubitrack::Math::Vector< float, 3 > pos;
    const float* markersSize; // optional (2.0+)
};

struct SkeletonData
{
    int ID;
    int nRigids;
    RigidData* rigids;
};

struct FrameData
{
    int frameNumber;
    int nPointClouds;
    PointCloudData* pointClouds;
    int nRigids;
    RigidData* rigids;
    int nSkeletons;
    SkeletonData* skeletons;

    float latency;
    unsigned int timecode;
    unsigned int timecodeSub;
    int eod;

    // unidentified markers
    int nOtherMarkers;
    std::vector< Ubitrack::Math::Vector< float, 3 > > otherMarkersPos;


    // labeled markers
    int nLabeledMarkers;
    MarkerData* labeledMarkersPos;

};


} } // namespace Ubitrack::Drivers

#endif
