cmake_minimum_required(VERSION 3.20)
project(Raven LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The native Raven runtime currently requires Linux")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RAVEN_SANITIZERS "Enable address and undefined behavior checks" OFF)
if(RAVEN_SANITIZERS)
    # The Skia library is built without RTTI, so dynamic type checks cannot apply to code that links it.
    add_compile_options(-fsanitize=address,undefined -fno-sanitize=vptr -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(RAVEN_FIXTURE_CC x86_64-w64-mingw32-gcc)
find_program(RAVEN_MINGW32 i686-w64-mingw32-gcc)
# The headers of MinGW-w64 12 lack declarations the Windows components use, such as E_BOUNDS and webservices.h.
set(RAVEN_MINGW_COMPILERS)
foreach(compiler RAVEN_FIXTURE_CC RAVEN_MINGW32)
    if(${compiler})
        list(APPEND RAVEN_MINGW_COMPILERS ${${compiler}})
    endif()
endforeach()
foreach(compiler ${RAVEN_MINGW_COMPILERS})
    execute_process(COMMAND ${compiler} -E -dM -include _mingw.h -x c /dev/null OUTPUT_VARIABLE MINGW_MACROS ERROR_QUIET)
    string(REGEX MATCH "#define __MINGW64_VERSION_MAJOR ([0-9]+)" MINGW_VERSION "${MINGW_MACROS}")
    if(MINGW_VERSION AND CMAKE_MATCH_1 LESS 13)
        message(FATAL_ERROR "${compiler} uses the headers of MinGW-w64 ${CMAKE_MATCH_1}; Raven's Windows components need MinGW-w64 13 or newer")
    endif()
endforeach()
set(RAVEN_TESTDATA_DIR "${CMAKE_BINARY_DIR}/testdata" CACHE PATH "Generated and downloaded test fixtures")
set(RAVEN_MINESWEEPER "" CACHE FILEPATH "Optional user-supplied Minesweeper executable")
file(GLOB_RECURSE RAVEN_FIXTURE_INPUTS CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/testdata/d3d11/*.c ${PROJECT_SOURCE_DIR}/testdata/d3d11/*.h
    ${PROJECT_SOURCE_DIR}/testdata/d3d12/*.c ${PROJECT_SOURCE_DIR}/testdata/d3d12/*.h
    ${PROJECT_SOURCE_DIR}/testdata/d3d9test/*.c ${PROJECT_SOURCE_DIR}/testdata/benchmark/*.c
    ${PROJECT_SOURCE_DIR}/testdata/native_loader/*.hex)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/scripts/provision-fixtures.py ${RAVEN_FIXTURE_INPUTS})
if(RAVEN_FIXTURE_CC)
    set(FIXTURE_ARGUMENTS --cc ${RAVEN_FIXTURE_CC})
else()
    set(FIXTURE_ARGUMENTS --data-only)
endif()
execute_process(COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/provision-fixtures.py
    --output ${RAVEN_TESTDATA_DIR} ${FIXTURE_ARGUMENTS} RESULT_VARIABLE FIXTURE_RESULT)
if(NOT FIXTURE_RESULT EQUAL 0)
    message(FATAL_ERROR "Test fixture provisioning failed")
endif()
add_subdirectory(native)
