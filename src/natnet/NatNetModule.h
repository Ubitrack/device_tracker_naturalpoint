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
 * ART driver
 * This file contains the driver component to
 * talk to the ART infrared tracking system.
 *
 * The driver is build from one module to handle
 * the sockets communicationa and compontens for
 * each tracked object.
 *
 * The received data is sent via a push interface.
 *
 * @author Manuel Huber <huberma@in.tum.de>
 */
#ifndef __ArtModule_h_INCLUDED__
#define __ArtModule_h_INCLUDED__

#include <string>
#include <cstdlib>

#include "UdpSocketSingleton.h"

#include <utDataflow/PushSupplier.h>
#include <utDataflow/Component.h>
#include <utDataflow/Module.h>
#include <utMeasurement/Measurement.h>
#include <utMeasurement/TimestampSync.h>

namespace Ubitrack { namespace Drivers {

using namespace Dataflow;

// forward declaration
class ArtComponent;

/**
 * Module key for art.
 * Represents the port number on which to listen.
 */
MAKE_NODEATTRIBUTEKEY_DEFAULT( ArtModuleKey, int, "Art", "artPort", 5000 );


/**
 * Component key for art.
 * Represents the body number
 */
class ArtComponentKey
{
public:
    enum TargetType { target_6d, target_6d_flystick, target_6d_measurement_tool, target_6d_measurement_tool_reference, target_finger, target_3dcloud };

	enum FingerType { finger_hand, finger_thumb, finger_index, finger_middle };
	enum FingerSide { side_left = 0, side_right = 1 };

	// still ugly refactor art driver sometime..
	// construct from configuration
	ArtComponentKey( boost::shared_ptr< Graph::UTQLSubgraph > subgraph )
	: m_body( 0 )
	, m_targetType( target_6d )
	, m_fingerSide ( side_left )
	{
		Graph::UTQLSubgraph::EdgePtr config;

	  if ( subgraph->hasEdge( "ArtToTarget" ) )
		  config = subgraph->getEdge( "ArtToTarget" );
	  else if ( subgraph->hasEdge( "fingerHandOutput" ) )
		  config = subgraph->getEdge( "fingerHandOutput" );

	  if ( !config )
	  {
		  UBITRACK_THROW( "ArtTracker Pattern has neither \"ArtToTarget\" nor \"fingerHandOutput\" edge");
	  }

	  config->getAttributeData( "artBodyId", m_body );
	  if ( m_body <= 0 )
            UBITRACK_THROW( "Missing or invalid \"artBodyId\" attribute on \"ArtToTarget\" resp. \"fingerHandOutput\" edge" );

	  std::string typeString = config->getAttributeString( "artType" );
	  if ( typeString.empty() )
	  {
	      // no explicit art target type information. so we assume 6D
	      m_targetType = target_6d;
	  }
	  else
	  {
	      if ( typeString == "6d" )
			  m_targetType = target_6d;
	      else if ( typeString == "6df" )
			  m_targetType = target_6d_flystick;
	      else if ( typeString == "6dmt" )
			  m_targetType = target_6d_measurement_tool;
	      else if ( typeString == "6dmtr" )
			  m_targetType = target_6d_measurement_tool_reference;
	      else if ( typeString == "3dcloud" )
		  {
			  m_targetType = target_3dcloud;
			  m_body = 0;
		  }
	      else if ( typeString == "finger" )
		  {
			  m_targetType = target_finger;

			  /*
			  Graph::UTQLSubgraph::NodePtr configNode = config->m_Target.lock();

			  std::string fingerString = configNode->getAttributeString( "finger" );

			  if (fingerString.length() == 0)
				  UBITRACK_THROW( "Art finger target without finger id" );

			  if ( fingerString == "hand" )
				  m_fingerType = finger_hand;
			  else if ( fingerString == "thumb" )
				  m_fingerType = finger_thumb;
			  else if ( fingerString == "index" )
				  m_fingerType = finger_index;
			  else if ( fingerString == "middle" )
				  m_fingerType = finger_middle;
			  else
				  UBITRACK_THROW( "Art finger target with unknown finger type: " + fingerString );
			  */

			  std::string fingerSideString = config->getAttributeString( "fingerSide" );
			  if (fingerSideString.length() == 0)
				  UBITRACK_THROW( "Art finger target without finger side" );

			  if ( fingerSideString == "left" )
				  m_fingerSide = side_left;
			  else if ( fingerSideString == "right" )
				  m_fingerSide = side_right;
			  else
				  UBITRACK_THROW( "Art finger target with unknown finger side: " + fingerSideString );

		  }
	      else
			  UBITRACK_THROW( "Art target with unknown target type: " + typeString );
	  }

	}

	// construct from body number
	ArtComponentKey( int a )
		: m_body( a )
        , m_targetType( target_6d )
		, m_fingerSide ( side_left )
 	{}

    // construct from body number and target type
    ArtComponentKey( int a, TargetType t )
        : m_body( a )
        , m_targetType( t )
		, m_fingerSide( side_left )
    {}

	// construct from body number and target type and finger type
    ArtComponentKey( int a, TargetType t, FingerSide s )
        : m_body( a )
        , m_targetType( t )
		, m_fingerSide( s )
    {}

	int getBody() const
	{
		return m_body;
	}

    TargetType getTargetType() const
    {
        return m_targetType;
    }

	// less than operator for map
	bool operator<( const ArtComponentKey& b ) const
    {
        if ( m_targetType == b.m_targetType )
			if ( m_fingerSide == b.m_fingerSide )
				return m_body < b.m_body;
			else
				return m_fingerSide < b.m_fingerSide;
        else
            return m_targetType < b.m_targetType;
    }

protected:
	int m_body;
	TargetType m_targetType;
	FingerSide m_fingerSide;
};


/**
 * Module for ART tracker.
 * Does all the work
 */
class ArtModule
	: public Module< ArtModuleKey, ArtComponentKey, ArtModule, ArtComponent >
{
public:
	/** UTQL constructor */
	ArtModule( const ArtModuleKey& key, boost::shared_ptr< Graph::UTQLSubgraph >, FactoryHelper* pFactory );

	/** destructor */
	~ArtModule();

	virtual void startModule();

	virtual void stopModule();

	/** thread method */
    void HandleReceive (const boost::system::error_code err, size_t length);

protected:
    boost::shared_ptr< UdpSocketSingleton > m_pSocket;

    // Recive data. Do not touch from outside of async network thread
    enum { max_receive_length = 10240, receive_buffer_size = 10242 };
    char receive_data[receive_buffer_size];
	boost::asio::ip::udp::endpoint sender_endpoint;

	Measurement::TimestampSync m_synchronizer;

private:
    void trySendPose( int id, ArtComponentKey::TargetType type, double qual, double* rot, double* mat, Ubitrack::Measurement::Timestamp ts );
	void trySendPose( int id, ArtComponentKey::TargetType type, double qual, double* rot, double* mat, Ubitrack::Measurement::Timestamp ts, ArtComponentKey::FingerType f, ArtComponentKey::FingerSide s );
	void trySendPose( boost::shared_pARTtr< std::vector< Ubitrack::Math::Vector < 3 > > > cloud, Ubitrack::Measurement::Timestamp ts );
};


/**
 * Component for ART tracker.
 * Does nothing but provide a push port

 * @TODO: make this two separate components for 6d/3dlist
 */
class ArtComponent
	: public ArtModule::Component
{
public:
	/** constructor */
	ArtComponent( const std::string& name, boost::shared_ptr< Graph::UTQLSubgraph > subgraph, const ArtComponentKey& componentKey, ArtModule* pModule )
		: ArtModule::Component( name, componentKey, pModule )
		, m_port( "ArtToTarget", *this )
		, m_cloudPort( "3DOutput", *this )
		, m_fingerHandPort( "fingerHandOutput", *this )
		, m_fingerThumbPort( "fingerThumbOutput", *this )
		, m_fingerIndexPort( "fingerIndexOutput", *this )
		, m_fingerMiddlePort( "fingerMiddleOutput", *this )
	{}
	
	/** destructor */
	~ArtComponent();

	/** returns the port for usage by the module */
	PushSupplier< Ubitrack::Measurement::Pose >& getPort()
	{ return m_port; }

	/** returns the port for usage by the module */
	PushSupplier< Ubitrack::Measurement::PositionList >& getCloudPort()
	{ return m_cloudPort; }

	/** returns the port for usage bARTy the module */
	PushSupplier< Ubitrack::Measurement::Pose >& getFingerPort( ArtComponentKey::FingerType t )
	{
		switch (t)
		{
		case ArtComponentKey::finger_hand:
			return m_fingerHandPort;
			break;
		case ArtComponentKey::finger_thumb:
			return m_fingerThumbPort;
			break;
		case ArtComponentKey::finger_index:
			return m_fingerIndexPort;
			break;
		case ArtComponentKey::finger_middle:
			return m_fingerMiddlePort;
			break;
		}

		// return hand if nothing else matches..
		return m_fingerHandPort;
	}

protected:
	// the port is the only member
	PushSupplier< Ubitrack::Measurement::Pose > m_port;
	PushSupplier< Ubitrack::Measurement::PositionList > m_cloudPort;

	PushSupplier< Ubitrack::Measurement::Pose > m_fingerHandPort;
	PushSupplier< Ubitrack::Measurement::Pose > m_fingerThumbPort;
	PushSupplier< Ubitrack::Measurement::Pose > m_fingerIndexPort;
	PushSupplier< Ubitrack::Measurement::Pose > m_fingerMiddlePort;
};

} } // namespace Ubitrack::Drivers

#endif
