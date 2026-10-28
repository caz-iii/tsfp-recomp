cmake_minimum_required(VERSION 3.20)
project(tsfp-recomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Inputs supplied by the user (never shipped): the retail main.dol, and the
# decomp's symbols.txt for function boundaries and names.
set(TSFP_DOL "${CMAKE_SOURCE_DIR}/../tsfp-decomp/orig/G3FE69/sys/main.dol" CACHE FILEPATH "Retail GameCube main.dol")
set(TSFP_SYMBOLS "${CMAKE_SOURCE_DIR}/../tsfp-decomp/config/G3FE69/symbols.txt" CACHE FILEPATH "decomp-toolkit symbols.txt")
set(TSFP_HLE_LIST "${CMAKE_SOURCE_DIR}/src/hle/hle_functions.txt" CACHE FILEPATH "Functions replaced by native code")
set(TSFP_GEN_FILES 64 CACHE STRING "Number of generated translation units")

# ---------------------------------------------------------------------------
# Host tool: PowerPC -> C++ static recompiler.
add_executable(ppcrecomp tools/ppcrecomp/ppcrecomp.cpp)

# Generated code lives in the build tree only: it is derived from the retail
# executable and must not be committed.
set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${GEN_DIR}")
set(GEN_SOURCES "${GEN_DIR}/table.cpp")
math(EXPR _last "${TSFP_GEN_FILES} - 1")
foreach(i RANGE ${_last})
    string(LENGTH "${i}" _len)
    if(_len EQUAL 1)
        set(_n "00${i}")
    elseif(_len EQUAL 2)
        set(_n "0${i}")
    else()
        set(_n "${i}")
    endif()
    list(APPEND GEN_SOURCES "${GEN_DIR}/recomp_${_n}.cpp")
endforeach()

add_custom_command(
    OUTPUT ${GEN_SOURCES} "${GEN_DIR}/funcs.h"
    COMMAND ppcrecomp "${TSFP_DOL}" "${TSFP_SYMBOLS}" "${GEN_DIR}" --hle "${TSFP_HLE_LIST}" --files ${TSFP_GEN_FILES}
    DEPENDS ppcrecomp "${TSFP_DOL}" "${TSFP_SYMBOLS}" "${TSFP_HLE_LIST}"
    COMMENT "Recompiling main.dol"
    VERBATIM)

# ---------------------------------------------------------------------------
# SDL3 (window, GL context, input, audio). Prebuilt VC package, pinned + hashed.
include(FetchContent)
if(WIN32 AND MSVC)
    FetchContent_Declare(sdl3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-devel-3.4.18-VC.zip
        URL_HASH SHA256=78d84602ae616cfe26b33a73b7d3b9b6b56e8a6650e53d92c9c6ca938f747acd)
    FetchContent_MakeAvailable(sdl3)
    set(SDL3_DIR "${sdl3_SOURCE_DIR}/cmake")
endif()
find_package(SDL3 REQUIRED CONFIG)
find_package(OpenGL REQUIRED)

# ---------------------------------------------------------------------------
# The game.
file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
add_executable(tsfp ${RUNTIME_SOURCES} ${GEN_SOURCES})
target_include_directories(tsfp PRIVATE src "${GEN_DIR}")
target_link_libraries(tsfp PRIVATE SDL3::SDL3 OpenGL::GL)
if(WIN32)
    add_custom_command(TARGET tsfp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:tsfp>)
endif()

if(MSVC)
    target_compile_options(tsfp PRIVATE /W3 /bigobj /fp:precise)
    # Generated code: optimise, but skip warnings it can't avoid (unused labels etc.).
    set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/W0;/O2")
    target_compile_definitions(tsfp PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(tsfp PRIVATE -Wall -Wextra -ffp-contract=off)
    set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;-O2")
endif()
