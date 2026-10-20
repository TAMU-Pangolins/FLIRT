# Helpers the FLIRT programs share: plain C++, no Qt, ROOT or mvme
# (platform.*: the Linux/macOS differences).
add_library(flirt_common STATIC names.cc utf8.cc platform.cc)
target_include_directories(flirt_common PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_options(flirt_common PRIVATE -Wall -Wextra)
