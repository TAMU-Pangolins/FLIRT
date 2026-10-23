# mvme_root_client's event base classes (MVMEExperiment/MVMEEvent/MVMEModule) and
# their ROOT dictionary, built from the unmodified client sources the way the
# client's Makefile does (mvme_root_client/Makefile:29-33).
#
# Everything lands in an mvme-like prefix, so the experiment code FLIRT
# generates can also be built by users with `MVME=<prefix> make`:
#   <prefix>/bin/flirt-convert
#   <prefix>/include/mvme/mvme_root_event_objects{,_LinkDef}.h
#   <prefix>/lib/libmvme_root_event.so (+ _rdict.pcm, .rootmap)

find_package(ROOT REQUIRED COMPONENTS Core RIO Tree Hist)
find_program(ROOTCLING rootcling HINTS ${ROOT_BINDIR} REQUIRED)

set(MVME_ROOT_CLIENT_DIR ${MVME_SRC}/event_server/clients/mvme_root_client)
set(FLIRT_PREFIX_LIB ${FLIRT_PREFIX}/lib)
set(FLIRT_PREFIX_INC ${FLIRT_PREFIX}/include/mvme)

foreach(header mvme_root_event_objects.h mvme_root_event_objects_LinkDef.h)
    configure_file(${MVME_ROOT_CLIENT_DIR}/${header} ${FLIRT_PREFIX_INC}/${header} COPYONLY)
endforeach()

add_custom_command(
    OUTPUT ${FLIRT_PREFIX_LIB}/mvme_root_event_rdict.cxx
           ${FLIRT_PREFIX_LIB}/mvme_root_event_rdict_rdict.pcm
           ${FLIRT_PREFIX_LIB}/mvme_root_event.rootmap
    COMMAND ${ROOTCLING} -f mvme_root_event_rdict.cxx
            -rml libmvme_root_event.so -rmf mvme_root_event.rootmap
            -I${FLIRT_PREFIX}/include
            mvme/mvme_root_event_objects.h mvme/mvme_root_event_objects_LinkDef.h
    WORKING_DIRECTORY ${FLIRT_PREFIX_LIB}
    DEPENDS ${FLIRT_PREFIX_INC}/mvme_root_event_objects.h
            ${FLIRT_PREFIX_INC}/mvme_root_event_objects_LinkDef.h
    COMMENT "rootcling: mvme_root_event dictionary"
    VERBATIM)

add_library(mvme_root_event SHARED
    ${MVME_ROOT_CLIENT_DIR}/mvme_root_event_objects.cxx
    ${FLIRT_PREFIX_LIB}/mvme_root_event_rdict.cxx)
target_include_directories(mvme_root_event PUBLIC ${FLIRT_PREFIX_INC} ${FLIRT_PREFIX}/include)
target_link_libraries(mvme_root_event PUBLIC ROOT::Core ROOT::RIO ROOT::Tree)
target_compile_options(mvme_root_event PRIVATE -O3)
set_target_properties(mvme_root_event PROPERTIES
    OUTPUT_NAME mvme_root_event
    PREFIX "lib" SUFFIX ".so"
    LIBRARY_OUTPUT_DIRECTORY ${FLIRT_PREFIX_LIB})
