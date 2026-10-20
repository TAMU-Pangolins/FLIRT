# Writes the build identity header for the conversion report:
#   cmake -DSOURCE_DIR=<repo> -DOUTPUT=<flirt_version.h> -P flirt_version.cmake
# Runs on every build; the header is rewritten only when its text changes, so a
# new commit or a dirty tree changes FLIRT_GIT_DESCRIBE without a reconfigure.
execute_process(COMMAND git describe --always --dirty --tags WORKING_DIRECTORY ${SOURCE_DIR}
    OUTPUT_VARIABLE describe OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

set(text "#define FLIRT_GIT_DESCRIBE \"${describe}\"\n")
set(old "")
if (EXISTS ${OUTPUT})
    file(READ ${OUTPUT} old)
endif()
if (NOT old STREQUAL text)
    file(WRITE ${OUTPUT} "${text}")
endif()
