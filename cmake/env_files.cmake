# Writes <build>/env.sh (bash, zsh) and <build>/env.csh (tcsh), which put this
# build's programs, and <build>-compare's compare when that folder exists, on
# PATH for the current shell session. The paths go in single quotes; a ' in
# them is written as '\'' (both shells read that as one ').
string(REPLACE "'" "'\\''" FLIRT_ENV_BIN "${FLIRT_PREFIX}/bin")
string(REPLACE "'" "'\\''" FLIRT_ENV_COMPARE "${CMAKE_BINARY_DIR}-compare")
configure_file(${CMAKE_CURRENT_LIST_DIR}/env.sh.in ${CMAKE_BINARY_DIR}/env.sh @ONLY)
configure_file(${CMAKE_CURRENT_LIST_DIR}/env.csh.in ${CMAKE_BINARY_DIR}/env.csh @ONLY)
