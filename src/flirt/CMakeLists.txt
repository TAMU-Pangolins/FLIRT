add_library(flirt STATIC
    listfile.cc
    )
target_include_directories(flirt PUBLIC ${CMAKE_SOURCE_DIR}/src
    ${MVME_SRC} ${MVME_EXT}/nlohmann-json)
target_link_libraries(flirt PUBLIC mvme_a2 mvme_multi_event_splitter mesytec-mvlc Qt5::Core
    PRIVATE flirt_common)
target_compile_options(flirt PRIVATE -Wall -Wextra)
