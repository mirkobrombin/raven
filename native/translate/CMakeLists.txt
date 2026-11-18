# The x86-64 to AArch64 translator. A project of its own until the build is split by host; then native/CMakeLists.txt adds this directory.
cmake_minimum_required(VERSION 3.20)
project(RavenTranslate LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(RAVEN_NATIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(RAVEN_TESTDATA ${CMAKE_CURRENT_SOURCE_DIR}/../../testdata)

add_library(raven_translate STATIC x86_decoder.cpp)
target_include_directories(raven_translate PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(raven_translate PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

# The guest programs the tests run, built as Raven's own build builds them (native/CMakeLists.txt, scripts/provision-fixtures.py).
find_program(RAVEN_MINGW64 x86_64-w64-mingw32-clang REQUIRED)
find_program(RAVEN_OBJDUMP llvm-objdump REQUIRED)
set(GUEST ${CMAKE_CURRENT_BINARY_DIR}/guest)
file(MAKE_DIRECTORY ${GUEST})
add_custom_command(OUTPUT ${GUEST}/return-73.exe
    COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -DRAVEN_RETURN_STATUS=73 -Wl,--entry,raven_entry,--no-insert-timestamp
        -o ${GUEST}/return-73.exe ${RAVEN_TESTDATA}/native_loader/return_client.c
    DEPENDS ${RAVEN_TESTDATA}/native_loader/return_client.c VERBATIM)
add_custom_command(OUTPUT ${GUEST}/entry.exe
    COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -Wl,--entry,raven_entry,--no-insert-timestamp
        -o ${GUEST}/entry.exe ${RAVEN_TESTDATA}/native_cpu/entry.c
    DEPENDS ${RAVEN_TESTDATA}/native_cpu/entry.c VERBATIM)
add_custom_command(OUTPUT ${GUEST}/startup.exe
    COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -Wl,--entry,entry,--no-insert-timestamp
        -o ${GUEST}/startup.exe ${RAVEN_TESTDATA}/benchmark/startup.c -lkernel32
    DEPENDS ${RAVEN_TESTDATA}/benchmark/startup.c VERBATIM)
add_custom_command(OUTPUT ${GUEST}/arguments.exe
    COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -Wl,--entry,raven_entry,--image-base,0x18e000000
        -o ${GUEST}/arguments.exe ${RAVEN_TESTDATA}/native_loader/arguments_client.c -lkernel32
    DEPENDS ${RAVEN_TESTDATA}/native_loader/arguments_client.c VERBATIM)
add_custom_command(OUTPUT ${GUEST}/api_loop.exe
    COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -Wl,--entry,raven_entry,--image-base,0x187000000
        -o ${GUEST}/api_loop.exe ${RAVEN_TESTDATA}/native_cpu/api_loop.c -lkernel32
    DEPENDS ${RAVEN_TESTDATA}/native_cpu/api_loop.c VERBATIM)
set(GUEST_PROGRAMS ${GUEST}/return-73.exe ${GUEST}/entry.exe ${GUEST}/startup.exe ${GUEST}/arguments.exe ${GUEST}/api_loop.exe)
add_custom_target(translate_guests ALL DEPENDS ${GUEST_PROGRAMS})

# The decoder's oracle: LLVM's disassembly of random instances, random bytes, the guest programs and, where it is installed, the
# Notepad++ x64 build the port tests with.
set(RAVEN_CORPUS_IMAGES "" CACHE STRING "More x64 PE files whose code the decoder is checked against")
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86_corpus.txt
    COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/x86_corpus.py ${RAVEN_MINGW64} ${RAVEN_OBJDUMP}
        ${CMAKE_CURRENT_BINARY_DIR}/x86_corpus.txt ${GUEST_PROGRAMS} ${RAVEN_CORPUS_IMAGES}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/x86_corpus.py ${GUEST_PROGRAMS} VERBATIM)
add_custom_target(x86_corpus ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/x86_corpus.txt)

add_executable(x86_decoder_test x86_decoder_test.cpp)
target_link_libraries(x86_decoder_test PRIVATE raven_translate)
add_dependencies(x86_decoder_test x86_corpus)
add_test(NAME translate_x86_decoder COMMAND x86_decoder_test ${CMAKE_CURRENT_BINARY_DIR}/x86_corpus.txt)
set_tests_properties(translate_x86_decoder PROPERTIES TIMEOUT 120)
