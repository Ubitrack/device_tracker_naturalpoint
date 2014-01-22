set(the_description "The UbiTrack NatNet Tracking Component")
ut_add_component(natnet DEPS utcore utdataflow )

UT_OPTION(USE_NATNETSDK "Use NatNet SDK" ON IF WIN32 )

SET(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
IF(WIN32 AND DEFINED EXTERNAL_LIBRARIES_DIR)
	set(NATNETSDK_ROOT "${EXTERNAL_LIBRARIES_DIR}/NatNetSDK")
ENDIF(WIN32 AND DEFINED EXTERNAL_LIBRARIES_DIR)

IF(USE_NATNETSDK)
  FIND_PACKAGE(NatNetSDK)
ENDIF(USE_NATNETSDK)

IF(NATNETSDK_FOUND)
  ut_component_include_directories("src/natnet" ${NATNETSDK_INCLUDE_DIR} ${UBITRACK_CORE_DEPS_INCLUDE_DIR})
  ut_glob_component_sources(HEADERS "src/natnet/NatNetModule.h" SOURCES "src/natnet/NatNetModule.cpp")
  ut_create_single_component(${NATNETSDK_LIBRARY})
ELSE(NATNETSDK_FOUND)
  ut_component_include_directories("src/natnet" ${UBITRACK_CORE_DEPS_INCLUDE_DIR})
  ut_glob_component_sources(HEADERS "src/natnet/NatNetPacketClientModule.h" SOURCES "src/natnet/NatNetPacketClientModule.cpp")
  ut_create_single_component()
ENDIF(NATNETSDK_FOUND)

ut_install_utql_patterns()
