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


#include "ArtModule.h"
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
<Pattern name="Art6D">
	<Input>
		<Node name="Art"/>
		<Node name="Body"/>
		<Edge name="Config" source="Art" destination="Body">
			<Predicate>trackable=="Art"</Predicate>
		</Edge>
	</Input>

	<Output>
		<Edge name="ArtToTarget" source="Art" destination="Body">
			<Attribute name="type" value="6D"/>
			<Attribute name="mode" value="push"/>
			<AttributeExpression name="artType">Config.artType</AttributeExpression>
			<AttributeExpression name="artBodyId">Config.artBodyId</AttributeExpression>
		</Edge>
	</Output>

	<DataflowConfiguration>
		<UbitrackLib class="ArtTracker"/>
	</DataflowConfiguration>
</Pattern>

Without the pattern, a valid dataflow description or SRG definition is:

<Pattern name="Art6D" id="ArtToBody1">

	<Output>
		<Node name="Art" id="Art">
			<Attribute name="artPort" value="5000"/>
		</Node>
		<Node name="Body" id="Body1"/>
		<Edge name="ArtToTarget" source="Art" destination="Body">
			<Attribute name="artBodyId" value="3"/>
			<Attribute name="artType" value="6d"/>
			<Attribute name="type" value="6D"/>
			<Attribute name="mode" value="push"/>
		</Edge>
	</Output>

	<DataflowConfiguration>
		<UbitrackLib class="ArtTracker"/>
	</DataflowConfiguration>

</Pattern>
*/

namespace Ubitrack { namespace Drivers {

static log4cpp::Category& logger( log4cpp::Category::getInstance( "Drivers.Art" ) );


ArtModule::ArtModule( const ArtModuleKey& moduleKey, boost::shared_ptr< Graph::UTQLSubgraph >, FactoryHelper* pFactory )
	: Module< ArtModuleKey, ArtComponentKey, ArtModule, ArtComponent >( moduleKey, pFactory )
	, m_synchronizer( 60 ) // assume 60 Hz for timestamp synchronization
{}


ArtComponent::~ArtComponent()
{
	LOG4CPP_INFO( logger, "Destroying ART component" );
}


// Performs thread-safe initialization of networking... boost::asio is not thread-safe, at least not for sharing a single socket between threads,
// see also http://www.boost.org/doc/libs/1_44_0/doc/html/boost_asio/overview/core/threads.html)
void ArtModule::startModule()
{
	LOG4CPP_INFO( logger, "Creating ART network service on port " << m_moduleKey.get() );

	// Peter Keitler, 2010-12-14.
	// Why do we need a Singleton here? There is exactly one UDP port per ART module (the module key of which is exactly the specified port)
	// Maybe, this should be refactured in the future!
	
	// Daniel Pustka, 2011-03-08
	// Because closing and shortly afterwards re-opening a socket does not always work. This is exactly what
	// happens when a dataflow is dynamically re-configured in mobile UBI-track setups.
	m_pSocket = UdpSocketSingleton::getSingleton( m_moduleKey.get() );

	m_pSocket->async_receive_from(
		boost::asio::buffer( receive_data, max_receive_length ),
		sender_endpoint,
		boost::bind( &ArtModule::HandleReceive, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred ) );

	m_pSocket->startNetwork();
}


// Performs thread-safe cleanup of networking... (boost::asio is not thread-safe, at least not for sharing a single socket between threads,
// see also http://www.boost.org/doc/libs/1_44_0/doc/html/boost_asio/overview/core/threads.html)
void ArtModule::stopModule()
{
	LOG4CPP_INFO( logger, "Stopping ART network service on port " << m_moduleKey.get() );

	m_pSocket->releaseSingleton();

	// Added by Peter Keitler, 2010-12-14.
	// Fixes problem of not destroying the shared instance of the singleton if the m_pSocket shared pointer remains valid
	// This is not good style. Again, do we really need this UdpSocketSingleton? Actually, a module already is a kind of singleton.
	// Daniel Pustka, 2011-03-08: The purpose of the singleton is to not destroy the socket (see above)
	m_pSocket.reset();

	Module<ArtModuleKey, ArtComponentKey, ArtModule, ArtComponent>::stopModule();
}


ArtModule::~ArtModule()
{}


void ArtModule::HandleReceive (const boost::system::error_code err, size_t length)
{
	// save the timestamp as soon as possible
	Ubitrack::Measurement::Timestamp timestamp = Ubitrack::Measurement::now();

	if ( m_running )
	{
		// subtract the approximate processing time of the cameras and DTrack (19ms)
		// (daniel) better synchronize the DTrack controller to a common NTP server and use "ts" fields directly.
		//          This should work well at least with ARTtrack2/3 cameras (not necessarily ARTtrack/TP)
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
					trySendPose( id, ArtComponentKey::target_6d, qual, rot, mat, timestamp );
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
					trySendPose( id, ArtComponentKey::target_6d_flystick, qual, rot, mat, timestamp );
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
					trySendPose( id, ArtComponentKey::target_6d_measurement_tool, qual, rot, mat, timestamp );
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
					trySendPose( id, ArtComponentKey::target_6d_measurement_tool_reference, qual, rot, mat, timestamp );
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

					ArtComponentKey::FingerSide fingerSide = (side==0)?(ArtComponentKey::side_left):(ArtComponentKey::side_right);

					// read and send hand pose
					sscanf (recordC,
							"[%lf %lf %lf][%lf %lf %lf %lf %lf %lf %lf %lf %lf]%n",
							&rot[0], &rot[1], &rot[2],
							&mat[0], &mat[3], &mat[6], &mat[1], &mat[4], &mat[7], &mat[2], &mat[5], &mat[8],
							&readChars );
					lastpos += readChars;
					recordC += readChars;

					trySendPose( id, ArtComponentKey::target_finger, qual, rot, mat, timestamp, ArtComponentKey::finger_hand, fingerSide );

					// read and send thumb
					sscanf (recordC,
							"[%lf %lf %lf][%lf %lf %lf %lf %lf %lf %lf %lf %lf][%lf %lf %lf %lf %lf %lf]%n",
							&rot[0], &rot[1], &rot[2],
							&mat[0], &mat[3], &mat[6], &mat[1], &mat[4], &mat[7], &mat[2], &mat[5], &mat[8],
							&phalanx[0], &phalanx[1], &phalanx[2], &phalanx[3], &phalanx[4], &phalanx[5],
							&readChars );
					lastpos += readChars;
					recordC += readChars;

					trySendPose( id, ArtComponentKey::target_finger, qual, rot, mat, timestamp, ArtComponentKey::finger_thumb, fingerSide );

					// read and send index
					sscanf (recordC,
							"[%lf %lf %lf][%lf %lf %lf %lf %lf %lf %lf %lf %lf][%lf %lf %lf %lf %lf %lf]%n",
							&rot[0], &rot[1], &rot[2],
							&mat[0], &mat[3], &mat[6], &mat[1], &mat[4], &mat[7], &mat[2], &mat[5], &mat[8],
							&phalanx[0], &phalanx[1], &phalanx[2], &phalanx[3], &phalanx[4], &phalanx[5],
							&readChars );
					lastpos += readChars;
					recordC += readChars;

					trySendPose( id, ArtComponentKey::target_finger, qual, rot, mat, timestamp, ArtComponentKey::finger_index, fingerSide );

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

					trySendPose( id, ArtComponentKey::target_finger, qual, rot, mat, timestamp, ArtComponentKey::finger_middle, fingerSide );
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
		boost::bind (&ArtModule::HandleReceive, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));

}


void ArtModule::trySendPose( boost::shared_ptr< std::vector< Ubitrack::Math::Vector < 3 > > > cloud, Ubitrack::Measurement::Timestamp ts )
{
	// 3D Cloud always has ID 1 for now..
	ArtComponentKey key( 0, ArtComponentKey::target_3dcloud );

	if ( hasComponent( key ) )
	{
		Ubitrack::Measurement::PositionList pc( ts, cloud );

		getComponent( key )->getCloudPort().send( pc );
	}
// 	else
// 	{
// 		std::cout << ":-(" << std::endl;
// 		for (ComponentMap::iterator it = m_componentMap.begin();
// 			 it != m_componentMap.end(); ++it)
// 		{
// 			boost::shared_ptr< ArtComponent > cc(it->second);
// 			ArtComponentKey keyc = cc->getKey();
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

void ArtModule::trySendPose( int id, ArtComponentKey::TargetType type, double qual, double* rot, double* mat, Ubitrack::Measurement::Timestamp ts )
{
    // negative quality indicates that the body is not tracked at all
    if ( qual < 0.0 ) {
		LOG4CPP_INFO( logger, "Bad quality for id " << id+1 );
        return;
	}

    // on the network the IDs are 0 based, in the DTrack software they are 1 based..
    ArtComponentKey key( id+1, type );

    // check for component
    if ( hasComponent( key ) )
    {
        // generate pose
        Ubitrack::Math::Vector< 3 > position ( rot[0]/1000.0, rot[1]/1000.0, rot[2]/1000.0 );
        Ubitrack::Math::Matrix< 3, 3 > rotMatrix ( mat );
        Ubitrack::Math::Quaternion rotation ( rotMatrix );
        Ubitrack::Measurement::Pose pose( ts, Ubitrack::Math::Pose( rotation, position ) );

        //send it to the component
		LOG4CPP_TRACE( logger, "Sending pose for id " << id+1 << " using " << getComponent( key )->getName() << ": " << pose );
        getComponent( key )->getPort().send( pose );
    }
	else {
		LOG4CPP_TRACE( logger, "No component for body id " << id+1 );
	}
}


		/* send Finger Pose */
void ArtModule::trySendPose( int id, ArtComponentKey::TargetType type, double qual, double* rot, double* mat, Ubitrack::Measurement::Timestamp ts, ArtComponentKey::FingerType f, ArtComponentKey::FingerSide s )
{
    // negative quality indicates that the body is not tracked at all
    if ( qual < 0.0 )
        return;

    // on the network the IDs are 0 based, in the DTrack software they are 1 based..
    ArtComponentKey key( id+1, type, s );

    // check for component
    if ( hasComponent( key ) )
    {
        // generate pose
        Ubitrack::Math::Vector< 3 > position ( rot[0]/1000.0, rot[1]/1000.0, rot[2]/1000.0 );
        Ubitrack::Math::Matrix< 3, 3 > rotMatrix ( mat );
        Ubitrack::Math::Quaternion rotation ( rotMatrix );
        Ubitrack::Measurement::Pose pose( ts, Ubitrack::Math::Pose( rotation, position ) );

        //send it to the component
        getComponent( key )->getFingerPort( f ).send( pose );
    }
}

// register module at factory
UBITRACK_REGISTER_COMPONENT( ComponentFactory* const cf ) {
	cf->registerModule< ArtModule > ( "ArtTracker" );
}

} } // namespace Ubitrack::Drivers
