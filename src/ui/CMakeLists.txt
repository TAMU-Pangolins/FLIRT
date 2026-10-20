# flirt-tui's parts that do not draw (docs/design.md, "TUI"): the job running
# flirt-convert --json-events (job.*), the file browser (files.*), the start
# options -> flirt-convert's arguments (start_args.*),.
# nlohmann json, pthreads, flirt_common; no Qt, ROOT or mvme.
find_package(Threads REQUIRED)
add_library(flirt_ui STATIC job.cc files.cc start_args.cc)
target_include_directories(flirt_ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(flirt_ui SYSTEM PUBLIC ${MVME_EXT}/nlohmann-json)
target_link_libraries(flirt_ui PUBLIC flirt_common Threads::Threads)
target_compile_options(flirt_ui PRIVATE -Wall -Wextra)
