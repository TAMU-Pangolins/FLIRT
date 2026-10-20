# Targets built from the pinned mvme sources, without mvme's own src/CMakeLists
# (which would pull in libmvme, Qt Widgets, Qwt and graphviz). Nothing under
# external/ is modified.

find_package(Qt5Core REQUIRED)
find_package(Boost REQUIRED)
find_package(ZLIB REQUIRED)

set(MVME_SRC ${MVME_DIR}/src)
set(MVME_EXT ${MVME_DIR}/external)

# Header-only externals, defined as in external/mvme/external/CMakeLists.txt.
add_library(mvme_pcg INTERFACE)
target_include_directories(mvme_pcg SYSTEM INTERFACE ${MVME_EXT}/pcg-cpp-0.98/include)
add_library(mvme_zstr INTERFACE)
target_include_directories(mvme_zstr SYSTEM INTERFACE ${MVME_EXT}/zstr/src)
add_library(mvme_exprtk INTERFACE)
target_include_directories(mvme_exprtk SYSTEM INTERFACE ${MVME_EXT}/exprtk)
add_library(mvme_cpp11_on_multicore INTERFACE)
target_include_directories(mvme_cpp11_on_multicore SYSTEM INTERFACE ${MVME_EXT}/cpp11-on-multicore)
add_library(mvme_range_v3 INTERFACE)
target_include_directories(mvme_range_v3 SYSTEM INTERFACE ${MVME_EXT}/range-v3-0.12.0/include)

# mvme generates these with GenerateExportHeader; everything here is linked
# statically, so the macros expand to nothing.
set(MVME_GEN_DIR ${CMAKE_BINARY_DIR}/mvme_generated)
foreach(name a2_export libmvme_export libmvme_mdpp_decode_export)
    string(TOUPPER ${name} guard)
    string(REPLACE "_EXPORT" "" macro_base ${guard})
    file(WRITE ${MVME_GEN_DIR}/${name}.h.tmp
        "#ifndef ${guard}_H\n#define ${guard}_H\n"
        "#define LIBA2_EXPORT\n#define LIBMVME_EXPORT\n#define LIBMVME_MDPP_DECODE_EXPORT\n"
        "#define LIBMVME_NO_EXPORT\n#define LIBMVME_MDPP_DECODE_NO_EXPORT\n#endif\n")
    configure_file(${MVME_GEN_DIR}/${name}.h.tmp ${MVME_GEN_DIR}/${name}.h COPYONLY)
endforeach()

# liba2: the analysis extractor engine (external/mvme/src/analysis/a2/CMakeLists.txt),
# plus the sources it needs from mdpp-sampling and util. QtCore is the only Qt use.
add_library(mvme_a2 STATIC
    ${MVME_SRC}/analysis/a2/a2.cc
    ${MVME_SRC}/analysis/a2/a2_exprtk.cc
    ${MVME_SRC}/analysis/a2/listfilter.cc
    ${MVME_SRC}/mdpp-sampling/mdpp_decode.cc
    ${MVME_SRC}/util/variablify.cc
    )
target_include_directories(mvme_a2 PUBLIC
    ${MVME_SRC} ${MVME_SRC}/analysis/a2 ${MVME_GEN_DIR})
# a2/util/bits.h:32 includes <x86intrin.h> unconditionally; on other hosts
# (Apple silicon) an empty stand-in is found first (see the header).
if (NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i.86)$")
    target_include_directories(mvme_a2 BEFORE PUBLIC ${CMAKE_SOURCE_DIR}/cmake/non_x86_include)
endif()
target_link_libraries(mvme_a2
    PUBLIC mesytec-mvlc Qt5::Core Boost::boost mvme_pcg mvme_cpp11_on_multicore mvme_zstr
    PRIVATE mvme_exprtk mvme_range_v3 ZLIB::ZLIB)
target_compile_options(mvme_a2 PRIVATE -w)
set_target_properties(mvme_a2 PROPERTIES POSITION_INDEPENDENT_CODE ON AUTOMOC OFF)

# The multi-event splitter (external/mvme/src/multi_event_splitter.cc).
add_library(mvme_multi_event_splitter STATIC ${MVME_SRC}/multi_event_splitter.cc)
target_include_directories(mvme_multi_event_splitter PUBLIC ${MVME_SRC} ${MVME_GEN_DIR})
target_link_libraries(mvme_multi_event_splitter PUBLIC mesytec-mvlc)
target_compile_options(mvme_multi_event_splitter PRIVATE -w)

# mvme's module templates, installed where mvme looks for them relative to its
# binary: <prefix>/templates (template_system.cc:201-205). The multi-event
# splitter takes each module's header filters from here (vme_config.cc:520-528).
file(GLOB_RECURSE MVME_TEMPLATE_FILES CONFIGURE_DEPENDS ${MVME_DIR}/templates/*)
add_custom_command(
    OUTPUT ${FLIRT_PREFIX}/templates.stamp
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${FLIRT_PREFIX}/templates
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${MVME_DIR}/templates ${FLIRT_PREFIX}/templates
    COMMAND ${CMAKE_COMMAND} -E touch ${FLIRT_PREFIX}/templates.stamp
    DEPENDS ${MVME_TEMPLATE_FILES}
    COMMENT "Installing mvme module templates")
add_custom_target(mvme_templates ALL DEPENDS ${FLIRT_PREFIX}/templates.stamp)
