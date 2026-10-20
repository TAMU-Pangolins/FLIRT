# flirt-tui (docs/design.md, "TUI"): the
# conversion screen in the terminal; runs <its own directory>/flirt-convert
# through flirt_ui's JobManager. FTXUI (external/FTXUI, submodule, v6.1.9);
# no Qt, ROOT or mvme.
set(FTXUI_DIR ${CMAKE_SOURCE_DIR}/external/FTXUI)
if (NOT EXISTS ${FTXUI_DIR}/CMakeLists.txt)
    message(FATAL_ERROR "flirt-tui needs FTXUI, a git submodule, which is missing. Run\n"
        "  git submodule update --init external/FTXUI\n"
        "in ${CMAKE_SOURCE_DIR}, or configure with -DFLIRT_BUILD_TUI=OFF to build without flirt-tui.")
endif()
set(FTXUI_QUIET ON CACHE BOOL "" FORCE)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS_FUZZER OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
# SYSTEM: its headers give no warnings in our build.
add_subdirectory(${FTXUI_DIR} ${CMAKE_BINARY_DIR}/FTXUI EXCLUDE_FROM_ALL SYSTEM)

# The screen's state, drawing and event handling, without a terminal (a
# test can render the real screen code).
add_library(flirt_tui_core STATIC text.cc settings.cc model.cc browser.cc view.cc view_parts.cc view_boxes.cc view_right.cc
    view_dialogs.cc input.cc input_dialogs.cc mouse.cc wave_model.cc wave_view.cc wave_input.cc info_model.cc info_queue.cc
    plot_layout.cc wave_play.cc wave_full.cc wave_grid.cc picker_view.cc picker_input.cc dither_find_model.cc)
target_include_directories(flirt_tui_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(flirt_tui_core PUBLIC flirt_ui ftxui::component ftxui::dom ftxui::screen)
target_compile_options(flirt_tui_core PRIVATE -Wall -Wextra)

add_executable(flirt-tui flirt_tui.cc)
target_include_directories(flirt-tui PRIVATE ${CMAKE_BINARY_DIR}/flirt_generated)
target_link_libraries(flirt-tui PRIVATE flirt_tui_core)
target_compile_options(flirt-tui PRIVATE -Wall -Wextra)
add_dependencies(flirt-tui flirt_version)
# Next to flirt-convert, which it runs.
set_target_properties(flirt-tui PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${FLIRT_PREFIX}/bin)
