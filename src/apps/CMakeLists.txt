add_executable(flirt-convert flirt_convert.cc batch.cc batch_text.cc progress.cc watch.cc dither_offset.cc)
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

# flirt-info: what listfiles record about their runs, without converting
# them. The flirt library only: no ROOT.
add_executable(flirt-info flirt_info.cc)
target_link_libraries(flirt-info PRIVATE flirt)
target_compile_options(flirt-info PRIVATE -Wall -Wextra)
add_dependencies(flirt-info mvme_templates)
set_target_properties(flirt-info PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${FLIRT_PREFIX}/bin)

# flirt-waveforms: reads a waveforms file for flirt-tui's waveform view, in a
# process of its own (docs/design.md, "TUI", "Waveform view"). ROOT Core, RIO
# and Tree, flirt_common and the nlohmann headers only: no Qt, mvme or flirt
# library.
add_executable(flirt-waveforms flirt_waveforms.cc waveform_reader.cc)
target_include_directories(flirt-waveforms SYSTEM PRIVATE ${MVME_EXT}/nlohmann-json)
target_link_libraries(flirt-waveforms PRIVATE flirt_common ROOT::Core ROOT::RIO ROOT::Tree)
target_compile_options(flirt-waveforms PRIVATE -Wall -Wextra)
set_target_properties(flirt-waveforms PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${FLIRT_PREFIX}/bin)
