add_executable(flirt-convert flirt_convert.cc batch.cc batch_text.cc progress.cc)
# flirt_common comes with flirt (named again, macOS ld warns of a duplicate).
target_link_libraries(flirt-convert PRIVATE flirt flirt_root)
target_compile_definitions(flirt-convert PRIVATE MVME_GIT_DESCRIBE="${MVME_GIT_DESCRIBE}")
target_include_directories(flirt-convert PRIVATE ${CMAKE_BINARY_DIR}/flirt_generated)
add_dependencies(flirt-convert mvme_templates flirt_version)
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
