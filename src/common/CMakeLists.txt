# Helpers the FLIRT programs share: plain C++, no Qt, ROOT or mvme
# (platform.*: the Linux/macOS differences; failures.*: a run that ended badly, in plain words;
# cores.*: how many listfiles flirt-info and flirt-tui read at once;
# compression.*: the ROOT files' compression choices).
add_library(flirt_common STATIC names.cc estimate.cc utf8.cc platform.cc failures.cc cores.cc compression.cc)
target_include_directories(flirt_common PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_options(flirt_common PRIVATE -Wall -Wextra)
