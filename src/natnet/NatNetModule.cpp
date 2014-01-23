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


namespace Ubitrack { namespace Drivers {

static log4cpp::Category& logger( log4cpp::Category::getInstance( "Drivers.NatNet" ) );

#define MULTICAST_ADDRESS		"239.255.42.99"     // IANA, local network
#define PORT_COMMAND            1510
#define PORT_DATA  			    1511                // Default multicast group



// DataHandler receives data from the server
void __cdecl DataHandler(sFrameOfMocapData* data, void* pUserData)
{
	NatNetModule* nnm = (NatNetModule*) pUserData;
	nnm->processFrame(data);
}

// MessageHandler receives NatNet error/debug messages
void __cdecl MessageHandler(int msgType, char* msg)
{
	LOG4CPP_ERROR(logger, "Type: " << msgType << " Msg: " << msg);
}



NatNetModule::NatNetModule( const NatNetModuleKey& moduleKey, boost::shared_ptr< Graph::UTQLSubgraph > subgraph, FactoryHelper* pFactory )
	: Module< NatNetModuleKey, NatNetComponentKey, NatNetModule, NatNetComponent >( moduleKey, pFactory )
	, m_synchronizer( 100 ) // assume 100 Hz for timestamp synchronization
    , serverInfoReceived(false)
    , modelInfoReceived(false)
    , m_serverName(m_moduleKey.get())
    , m_clientName("")
	, m_latency(0)
    , m_lastTimestamp(0)
	, theClient(NULL)
{

	Graph::UTQLSubgraph::NodePtr config;

	if ( subgraph->hasNode( "OptiTrack" ) )
	  config = subgraph->getNode( "OptiTrack" );

	if ( !config )
	{
	  UBITRACK_THROW( "NatNetTracker Pattern has no \"OptiTrack\" node");
	}

	m_clientName = config->getAttributeString( "clientName" );
	config->getAttributeData("latency", m_latency);

}



// Establish a NatNet Client connection
int NatNetModule::CreateClient(int iConnectionType)
{
    // release previous server
    if(theClient)
    {
        theClient->Uninitialize();
        delete theClient;
    }

    // create NatNet client
    theClient = new NatNetClient(iConnectionType);

    // [optional] use old multicast group
    //theClient->SetMulticastAddress("224.0.0.1");

    // print version info
    unsigned char ver[4];
    theClient->NatNetVersion(ver);
    LOG4CPP_INFO(logger, "NatNet Client (NatNet ver. " << ver[0] << "." << ver[1] << "." << ver[2] << "." << ver[3] << ")");

    // Set callback handlers
    theClient->SetMessageCallback( MessageHandler );
	// configurable ?
    theClient->SetVerbosityLevel( Verbosity_Warning );
    theClient->SetDataCallback(  DataHandler, this );	// this function will receive data from the server

    // Init Client and connect to NatNet server
    // to use NatNet default port assigments
	int retCode = theClient->Initialize( (char*)m_clientName.c_str(), (char*)m_serverName.c_str() );
    // to use a different port for commands and/or data:
    //int retCode = theClient->Initialize(szMyIPAddress, szServerIPAddress, MyServersCommandPort, MyServersDataPort);
    if (retCode != ErrorCode_OK)
    {
        LOG4CPP_ERROR(logger, "Unable to connect to server.  Error code: " << retCode);
        return ErrorCode_Internal;
    }
    else
    {
        // print server info
        sServerDescription ServerDescription;
        memset(&ServerDescription, 0, sizeof(ServerDescription));
        theClient->GetServerDescription(&ServerDescription);
        if(!ServerDescription.HostPresent)
        {
            LOG4CPP_ERROR(logger, "Unable to connect to server. Host not present.");
            return 1;
        }
        LOG4CPP_INFO(logger, "Server info:" << std::endl 
			<< "Application: " << ServerDescription.szHostApp << " (ver. " << ServerDescription.HostAppVersion[0] << "." << ServerDescription.HostAppVersion[1] << "." << ServerDescription.HostAppVersion[2] << "." << ServerDescription.HostAppVersion[3] << ")" << std::endl
			<< "NatNet Version: " << ServerDescription.NatNetVersion[0] << "." << ServerDescription.NatNetVersion[1] << "." << ServerDescription.NatNetVersion[2] << "." << ServerDescription.NatNetVersion[3] << std::endl );
    }

    return ErrorCode_OK;

}



void NatNetModule::startModule()
{
	LOG4CPP_INFO( logger, "Creating NatNet for server: " << m_moduleKey.get() );

    int iResult;
    int iConnectionType = ConnectionType_Multicast;

	// Create NatNet Client
    iResult = CreateClient(iConnectionType);
    if(iResult != ErrorCode_OK)
    {
        LOG4CPP_ERROR(logger, "Error initializing client.  See log for details.  Exiting");
        return;
    }
    else
    {
        LOG4CPP_INFO(logger, "Client initialized and ready.");
    }


	// Retrieve Data Descriptions from server
	LOG4CPP_INFO(logger, "Requesting Data Descriptions...");

	sDataDescriptions* pDataDefs = NULL;
	int nBodies = theClient->GetDataDescriptions(&pDataDefs);
	if(!pDataDefs)
	{
		LOG4CPP_WARN(logger, "Unable to retrieve Data Descriptions.");
	}
	else
	{
        LOG4CPP_INFO(logger, "Received " << pDataDefs->nDataDescriptions << " Data Descriptions." );

		// reset mappings
		bodyIdMap.clear();
		bodyNameIdMap.clear();
		pointcloudNameIdMap.clear();


		//////////////////////////////////////  CONTENTS OF PROCESS MODELDEF NEEDS TO BE ADAPTED ... /////////////////////////////////////////////////////////////

		int index = 0;
		ComponentList allComponents( getAllComponents() );

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




        for(int i=0; i < pDataDefs->nDataDescriptions; i++)
        {
            //printf("Data Description # %d (type=%d)\n", i, pDataDefs->arrDataDescriptions[i].type);
            if(pDataDefs->arrDataDescriptions[i].type == Descriptor_MarkerSet)
            {
                // MarkerSet
                sMarkerSetDescription* pMS = pDataDefs->arrDataDescriptions[i].Data.MarkerSetDescription;
                //printf("MarkerSet Name : %s\n", pMS->szName);

				if (pMS->szName)
				{
        			if (pointcloudNameIdMap.count(pMS->szName) > 0) {
        				LOG4CPP_INFO( logger, "Receiver connected for Pointcloud: " << pMS->szName );
        			} else {
        				LOG4CPP_WARN( logger, "Received PointCloudDef: " << pMS->szName << " but receiver component was not found." );
        			}
				}
				
				//for(int i=0; i < pMS->nMarkers; i++) 
                //    printf("%s\n", pMS->szMarkerNames[i]);

            }
            else if(pDataDefs->arrDataDescriptions[i].type == Descriptor_RigidBody)
            {
                // RigidBody
                sRigidBodyDescription* pRB = pDataDefs->arrDataDescriptions[i].Data.RigidBodyDescription;
                //printf("RigidBody Name : %s\n", pRB->szName);
                //printf("RigidBody ID : %d\n", pRB->ID);
                //printf("RigidBody Parent ID : %d\n", pRB->parentID);
                //printf("Parent Offset : %3.2f,%3.2f,%3.2f\n", pRB->offsetx, pRB->offsety, pRB->offsetz);

	    		NatNetComponentKey key( pRB->ID, NatNetComponentKey::target_6d );

    			if ( hasComponent( key ) ) {

    				bodyIdMap[pRB->ID] = getComponent( key )->getKey().getBody();

					if (getComponent( key )->getKey().getName() != pRB->szName) {
        				LOG4CPP_WARN( logger, "Received RigidBodyDef: " << pRB->szName << " but configured name does not match: " << getComponent( key )->getKey().getName() );
    				} else {
        				LOG4CPP_INFO( logger, "Receiver connected for Rigid Body: " << pRB->szName );
    				}
    			} else {

        			if (pRB->szName)
					{
        				if (bodyNameIdMap.count(pRB->szName) > 0) {
        					bodyIdMap[pRB->ID] = bodyNameIdMap[pRB->szName];
            				LOG4CPP_INFO( logger, "Receiver connected for Rigid Body: " << pRB->szName );
        				} else {
            				LOG4CPP_WARN( logger, "Received RigidBodyDef: " << pRB->szName << " but receiver component was not found." );
        				}

					} else {
            			LOG4CPP_WARN( logger, "Received RigidBodyDef without name and found no matching component for ID:" << pRB->ID << "." );
					}
    			}




            }
            else if(pDataDefs->arrDataDescriptions[i].type == Descriptor_Skeleton)
            {
                // Skeleton
                sSkeletonDescription* pSK = pDataDefs->arrDataDescriptions[i].Data.SkeletonDescription;
                //printf("Skeleton Name : %s\n", pSK->szName);
                //printf("Skeleton ID : %d\n", pSK->skeletonID);
                //printf("RigidBody (Bone) Count : %d\n", pSK->nRigidBodies);
                for(int j=0; j < pSK->nRigidBodies; j++)
                {
                    sRigidBodyDescription* pRB = &pSK->RigidBodies[j];
                    //printf("  RigidBody Name : %s\n", pRB->szName);
                    //printf("  RigidBody ID : %d\n", pRB->ID);
                    //printf("  RigidBody Parent ID : %d\n", pRB->parentID);
                    //printf("  Parent Offset : %3.2f,%3.2f,%3.2f\n", pRB->offsetx, pRB->offsety, pRB->offsetz);
                }
            }
            else
            {
                LOG4CPP_WARN(logger, "Unknown data type.");
                // Unknown
            }
        }


	}

}


void NatNetModule::stopModule()
{
	LOG4CPP_INFO( logger, "Stopping NatNet network service on port " << m_moduleKey.get() );
	Module<NatNetModuleKey, NatNetComponentKey, NatNetModule, NatNetComponent>::stopModule();

	if (theClient) {
		LOG4CPP_INFO( logger, "Uninitialize NatNetClient on port " << m_moduleKey.get() );
		theClient->Uninitialize();
		delete theClient;
		theClient = NULL;
	}
}


NatNetModule::~NatNetModule()
{
	// delete references
	if (theClient) {
		delete theClient;	
		theClient = NULL;
	}

}

void NatNetModule::processFrame(sFrameOfMocapData* data)
{
	// save the timestamp as soon as possible
	// XXXthis could be done earlier, but would require passing the timestamp down
	// .. but would not matter too much if time-delay estimation is in place
	Ubitrack::Measurement::Timestamp timestamp = Ubitrack::Measurement::now();

	m_lastTimestamp = timestamp;


	// use synchronizer to correct timestamps
	// XXX is this correct ??
	timestamp = m_synchronizer.convertNativeToLocal( data->iFrame, timestamp );

	if ( m_running )
	{
		// XXX What's the default delay with NatNet .. needs to be measured..
		// subtract the approximate processing time of the cameras and DTrack (19ms)
		// (daniel) better synchronize the DTrack controller to a common NTP server and use "ts" fields directly.
		//          This should work well at least with ARTtrack2/3 cameras (not necessarily ARTtrack/TP)

		LOG4CPP_DEBUG( logger , "NatNet Latency (is actually an increasing float of unkown accuracy): " << data->fLatency );
		// should substract latency + network from timestamp .. instead of constant.
		// was 19,000,000
		timestamp -= m_latency;

		for (int i=0; i<data->nRigidBodies; ++i) {

			int id = bodyIdMap[data->RigidBodies[i].ID];
		    NatNetComponentKey key( id, NatNetComponentKey::target_6d );

		    // check for non-recognized markers (all sub-markers are at (0,0,0))
		    bool is_recognized = true;
		    if (data->RigidBodies[i].nMarkers > 0) {
			    if ((data->RigidBodies[i].Markers[0][0] == 0) &&
			    	(data->RigidBodies[i].Markers[0][1] == 0) &&
			    	(data->RigidBodies[i].Markers[0][2] == 0)) {
					for (int j=1; j < data->RigidBodies[i].nMarkers; j++) {
						if ((data->RigidBodies[i].Markers[j][0] == 0) && 
							(data->RigidBodies[i].Markers[j][1] == 0) && 
							(data->RigidBodies[i].Markers[j][2] == 0)) {
							is_recognized = false;
						}
					}
			    }
		    }

		    if (!is_recognized) {
		    	continue;
		    }

		    // check for tracker component
		    if ( hasComponent( key ) )
		    {
		        // generate pose
		        Ubitrack::Measurement::Pose pose( timestamp,
		        	Ubitrack::Math::Pose(
		        		Ubitrack::Math::Quaternion((double)data->RigidBodies[i].qx, (double)data->RigidBodies[i].qy, (double)data->RigidBodies[i].qz, (double)data->RigidBodies[i].qw),
		        		Ubitrack::Math::Vector< double, 3 >((double)data->RigidBodies[i].x, (double)data->RigidBodies[i].y, (double)data->RigidBodies[i].z)
	        		)
		        );

		        //send it to the component
				LOG4CPP_DEBUG( logger, "Sending pose for id " << id << " using " << getComponent( key )->getName() << ": " << pose << " mean error: " << data->RigidBodies[i].MeanError );
				static_cast<NatNetRigidBodyReceiverComponent*>(getComponent( key ).get())->send( pose );
		    }
			else {
				LOG4CPP_TRACE( logger, "No component for body id " << id );
			}
		}

		for (int i=0; i<data->nMarkerSets; i++) {

			// handle pointcloud receivers
			int id = pointcloudNameIdMap[data->MocapData[i].szName];
		    NatNetComponentKey key( id, NatNetComponentKey::target_3dcloud );

		    // check for component
		    if ( hasComponent( key ) )
		    {

		    	// possible without copying ??
				boost::shared_ptr< std::vector< Ubitrack::Math::Vector< double, 3 > > > cloud(new std::vector< Ubitrack::Math::Vector< double, 3 > >(data->MocapData[i].nMarkers));
		    	for (int j=0; j < data->MocapData[i].nMarkers; j++) {
					cloud->at(j) = Ubitrack::Math::Vector< double, 3 >(data->MocapData[i].Markers[j][0], data->MocapData[i].Markers[j][1], data->MocapData[i].Markers[j][2] );
		    	}

				Ubitrack::Measurement::PositionList pc( timestamp, cloud );

		        //send it to the component
				LOG4CPP_DEBUG( logger, "Sending pose for id " << id << " using " << getComponent( key )->getName() << ": " << cloud );
				static_cast<NatNetPointCloudReceiverComponent*>(getComponent( key ).get())->send( pc );
		    }
			else {
				LOG4CPP_TRACE( logger, "No component for cloud name " << data->MocapData[i].szName );
			}
		}
	}
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





NatNetComponent::~NatNetComponent()
{
	LOG4CPP_INFO( logger, "Destroying NatNet component" );
}




// register module at factory
UBITRACK_REGISTER_COMPONENT( ComponentFactory* const cf ) {
	cf->registerModule< NatNetModule > ( "NatNetTracker" );
}

} } // namespace Ubitrack::Drivers
