add_executable(flirt-convert flirt_convert.cc)
# flirt_common comes with flirt (named again, macOS ld warns of a duplicate).
target_link_libraries(flirt-convert PRIVATE flirt flirt_root)
add_dependencies(flirt-convert mvme_templates)
# Lives in the mvme-like prefix next to libmvme_root_event.so (cmake/mvme_root_event.cmake).
# $ORIGIN is @loader_path on macOS.
if (APPLE)
    set(FLIRT_CONVERT_RPATH "@loader_path/../lib")
else()
    set(FLIRT_CONVERT_RPATH "$ORIGIN/../lib")
endif()
set_target_properties(flirt-convert PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${FLIRT_PREFIX}/bin
    BUILD_RPATH "${FLIRT_CONVERT_RPATH}")
