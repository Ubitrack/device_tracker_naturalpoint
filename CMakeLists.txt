set(the_description "The UbiTrack NatNet Tracking Component")
ut_add_component(natnet DEPS utcore utdataflow )
ut_component_include_directories("src/natnet" ${TINYXML_INCLUDE_DIR} ${LOG4CPP_INCLUDE_DIR} ${BOOSTBINDINGS_INCLUDE_DIR} ${LAPACK_INCLUDE_DIR} ${Boost_INCLUDE_DIR})
ut_glob_component_sources(HEADERS "src/natnet/*.h" SOURCES "src/natnet/*.cpp")
ut_create_single_component()
ut_install_utql_patterns()