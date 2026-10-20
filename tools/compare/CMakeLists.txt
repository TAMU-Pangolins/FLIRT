cmake_minimum_required(VERSION 3.16)
project(flirt_compare CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Deliberately ROOT only: the comparator must share nothing with the converter.
find_package(ROOT REQUIRED COMPONENTS Core RIO Tree Hist)

add_executable(compare main.cc checks_file.cc checks_tree.cc)
target_link_libraries(compare PRIVATE ROOT::Core ROOT::RIO ROOT::Tree ROOT::Hist)
target_compile_options(compare PRIVATE -Wall -Wextra)
