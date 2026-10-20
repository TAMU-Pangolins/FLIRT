add_library(flirt STATIC
    listfile.cc
    module_templates.cc
    vme_config.cc
    analysis_config.cc
    extract.cc
    pipeline.cc
    listfile_scan.cc
    library_log.cc
    )
target_include_directories(flirt PUBLIC ${CMAKE_SOURCE_DIR}/src
    ${MVME_SRC} ${MVME_EXT}/nlohmann-json)
target_link_libraries(flirt PUBLIC mvme_a2 mvme_multi_event_splitter mesytec-mvlc Qt5::Core
    PRIVATE flirt_common)
target_compile_options(flirt PRIVATE -Wall -Wextra)

# mvme_root_client's output logic; kept apart so the extraction library has no ROOT.
add_library(flirt_root STATIC root_output.cc root_paths.cc)
target_include_directories(flirt_root PUBLIC ${CMAKE_SOURCE_DIR}/src ${MVME_SRC}
    PRIVATE ${MVME_ROOT_CLIENT_DIR} ${MVME_EXT} ${MVME_EXT}/nlohmann-json)
target_link_libraries(flirt_root PUBLIC flirt mvme_root_event ROOT::Core ROOT::RIO ROOT::Tree ROOT::Hist)
target_compile_options(flirt_root PRIVATE -Wall -Wextra)
