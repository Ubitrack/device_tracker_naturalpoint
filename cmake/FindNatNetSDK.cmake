# - Try to find NatNetSDK
# Once done this will define
#
#  NATNETSDK_FOUND - system has NATNET
#  NATNETSDK_INCLUDE_DIR - the NATNET include directory
#  NATNETSDK_LIBRARY - Link these to use NATNET

FIND_LIBRARY (NATNETSDK_LIBRARY NAMES NatNetLib
    PATHS 
	${NATNETSDK_ROOT}/lib
    ENV LD_LIBRARY_PATH
    ENV LIBRARY_PATH
    /usr/lib64
    /usr/lib
    /usr/local/lib64
    /usr/local/lib
    /opt/local/lib
    )
FIND_PATH (NATNETSDK_INCLUDE_DIR NatNetClient.h
    PATHS
	${NATNETSDK_ROOT}/include
    ENV CPATH
    /usr/include
    /usr/local/include
    /opt/local/include
    PATH_SUFFIXES natnet
    )

IF(NATNETSDK_INCLUDE_DIR AND NATNETSDK_LIBRARY)
    SET(NATNETSDK_FOUND TRUE)
ENDIF(NATNETSDK_INCLUDE_DIR AND NATNETSDK_LIBRARY)

IF(NATNETSDK_FOUND)
  IF(NOT NATNETSDK_FIND_QUIETLY)
    MESSAGE(STATUS "NATNETSDK Found: ${NATNETSDK_LIBRARY}")
  ENDIF(NOT NATNETSDK_FIND_QUIETLY)
ELSE(NATNETSDK_FOUND)
  IF(NATNETSDK_FIND_REQUIRED)
    MESSAGE(FATAL_ERROR "Could not find NATNETSDK")
  ENDIF(NATNETSDK_FIND_REQUIRED)
ENDIF(NATNETSDK_FOUND)

MARK_AS_ADVANCED(NATNETSDK_INCLUDE_DIR, NATNETSDK_LIBRARY)