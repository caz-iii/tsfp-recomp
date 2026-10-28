cmake_minimum_required(VERSION 3.16)
project(tsfp-recomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find dependencies
find_package(SDL3 QUIET)
find_package(OpenGL REQUIRED)

# Glob all source files recursively
file(GLOB_RECURSE SOURCES
    "src/*.cpp"
)

# Include headers
include_directories(include)

# Build as executable
add_executable(tsfp-recomp ${SOURCES})

# Link libraries
target_link_libraries(tsfp-recomp PRIVATE
    OpenGL::GL
)

if(SDL3_FOUND)
    target_link_libraries(tsfp-recomp PRIVATE SDL3::SDL3)
endif()

# Compiler warnings
target_compile_options(tsfp-recomp PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W3>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
)
