cmake_minimum_required(VERSION 3.16)
project(flirt LANGUAGES C CXX)

# Same defaults as the official mvme build (external/mvme/CMakeLists.txt:46-55).
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose Release or Debug" FORCE)
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# x86-64 without -mfma has no fused multiply-add, so the reference build
# rounds every a*b before a +c. Compilers for arm64 (Apple silicon) fuse them
# by default (-ffp-contract=on), which changes last bits, e.g. in liba2's
# calibration and extraction. Keep separate roundings for everything built here.
if (NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i.86)$")
    add_compile_options(-ffp-contract=off)
endif()

set(MVME_DIR ${CMAKE_SOURCE_DIR}/external/mvme)
# mvme-like install prefix inside the build tree (bin/, lib/, include/mvme/, templates/).
set(FLIRT_PREFIX ${CMAKE_BINARY_DIR}/prefix)

# mesytec-mvlc, as mvme builds it (external/mvme/external/CMakeLists.txt).
set(MVLC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MVLC_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(MVLC_BUILD_DEV_TOOLS OFF CACHE BOOL "" FORCE)
set(MVLC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${MVME_DIR}/external/mesytec-mvlc ${CMAKE_BINARY_DIR}/mesytec-mvlc EXCLUDE_FROM_ALL)
if (APPLE)
    # mesytec-mvlc has no USB implementation for macOS (Linux and Windows only,
    # src/mesytec-mvlc/CMakeLists.txt:66-81) and its FTDI library there is
    # x86_64 only (the linker ignores it, with a warning). FLIRT replays files
    # and never opens an MVLC. The unmodified Linux USB source (same FTDI D3XX
    # API, the file the Linux build uses) provides usb::Impl, which the library
    # references (mvlc_factory.cc:36-48, mvlc_dialog.cc:151); FLIRT's stand-ins
    # for the FTDI functions it calls report that there is no device.
    target_sources(mesytec-mvlc PRIVATE
        ${MVME_DIR}/external/mesytec-mvlc/src/mesytec-mvlc/mvlc_impl_usb_linux_x86_64.cc
        ${CMAKE_SOURCE_DIR}/src/macos/ftd3xx_stubs.cc)
else()
    # With gcc-toolset on Rocky Linux 8, the newer GCC copies the parts of the
    # C++ standard library that the system's GCC 8 library lacks, such as
    # std::filesystem, into each shared library (libstdc++_nonshared.a), and
    # its headers add pieces of std::filesystem (path::~path, path::extension).
    # Exported from libmesytec-mvlc.so, the first library flirt-convert loads,
    # they replaced the copies inside a ROOT built with GCC 8, whose layout
    # differs: ROOT 6.39 crashed as it started (TROOT::GetSharedLibDir). Keep
    # them private to the library (the version script hides std::filesystem).
    set(MVLC_VERSION_SCRIPT ${CMAKE_SOURCE_DIR}/cmake/mesytec_mvlc.map)
    target_link_options(mesytec-mvlc PRIVATE
        "LINKER:--exclude-libs,libstdc++_nonshared.a"
        "LINKER:--version-script=${MVLC_VERSION_SCRIPT}")
    set_property(TARGET mesytec-mvlc APPEND PROPERTY LINK_DEPENDS ${MVLC_VERSION_SCRIPT})
endif()

include(cmake/mvme_parts.cmake)
include(cmake/mvme_root_event.cmake)
include(cmake/env_files.cmake)

# Build identity for the conversion report. FLIRT's is regenerated on every
# build (flirt_version.h), mvme's pinned sources only change on a reconfigure.
set(FLIRT_VERSION_HEADER ${CMAKE_BINARY_DIR}/flirt_generated/flirt_version.h)
add_custom_target(flirt_version
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${FLIRT_VERSION_HEADER}
        -P ${CMAKE_SOURCE_DIR}/cmake/flirt_version.cmake
    BYPRODUCTS ${FLIRT_VERSION_HEADER}
    VERBATIM)
execute_process(COMMAND git describe --always --tags WORKING_DIRECTORY ${MVME_DIR}
    OUTPUT_VARIABLE MVME_GIT_DESCRIBE OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

add_subdirectory(src)
