# FLIRT sources
add_subdirectory(common)
add_subdirectory(flirt)
add_subdirectory(apps)

# flirt-tui (docs/design.md, "TUI"): FLIRT's terminal user interface.
# flirt-convert does not depend on it.
option(FLIRT_BUILD_TUI "Build flirt-tui" ON)
if (FLIRT_BUILD_TUI)
    add_subdirectory(ui)
    add_subdirectory(tui)
endif()
