add_library(flirt STATIC
    listfile.cc
    module_templates.cc
    vme_config.cc
    analysis_config.cc
    extract.cc
    pipeline.cc
    integrity.cc
    run_summary.cc
    listfile_scan.cc
    library_log.cc
    )
target_include_directories(flirt PUBLIC ${CMAKE_SOURCE_DIR}/src
    ${MVME_SRC} ${MVME_EXT}/nlohmann-json)
# The declared CRC-32 and size of ZIP entries come from mesytec-mvlc's minizip-ng
# reader (ZipReader::getRawReaderHandle). libmesytec-mvlc exports minizip-ng's
# symbols, so only its headers are needed; linking the static library again
# would duplicate it.
target_include_directories(flirt PRIVATE $<TARGET_PROPERTY:minizip,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(flirt PUBLIC mvme_a2 mvme_multi_event_splitter mesytec-mvlc Qt5::Core
    PRIVATE ZLIB::ZLIB flirt_common)
target_compile_options(flirt PRIVATE -Wall -Wextra)

# mvme_root_client's output logic; kept apart so the extraction library has no ROOT.
add_library(flirt_root STATIC root_output.cc root_paths.cc)
target_include_directories(flirt_root PUBLIC ${CMAKE_SOURCE_DIR}/src ${MVME_SRC}
    PRIVATE ${MVME_ROOT_CLIENT_DIR} ${MVME_EXT} ${MVME_EXT}/nlohmann-json)
target_link_libraries(flirt_root PUBLIC flirt mvme_root_event ROOT::Core ROOT::RIO ROOT::Tree ROOT::Hist)
target_compile_options(flirt_root PRIVATE -Wall -Wextra)
