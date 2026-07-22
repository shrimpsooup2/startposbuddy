cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(StartposBuddy VERSION 1.0.0)

add_library(${PROJECT_NAME} SHARED src/main.cpp)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# miniz: cross-platform gzip/zlib. RobTop's ZipUtils::(de)compressString is
# inlined on Windows and therefore not callable, so we bring our own.
CPMAddPackage(
    NAME miniz
    GITHUB_REPOSITORY richgel999/miniz
    VERSION 3.0.2
    OPTIONS "BUILD_EXAMPLES OFF" "BUILD_TESTS OFF" "BUILD_FUZZERS OFF" "AMALGAMATE_SOURCES OFF" "INSTALL_PROJECT OFF"
)
target_link_libraries(${PROJECT_NAME} miniz)

setup_geode_mod(${PROJECT_NAME})
