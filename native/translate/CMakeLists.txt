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

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(RAVEN_TRANSLATE_PLATFORM platform_darwin.cpp)
else()
    set(RAVEN_TRANSLATE_PLATFORM platform_linux.cpp)
endif()
add_library(raven_translate STATIC x86_decoder.cpp a64_emitter.cpp translator.cpp code_cache.cpp runtime.cpp ${RAVEN_TRANSLATE_PLATFORM})
target_include_directories(raven_translate PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(raven_translate PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

# The PE parser is OS-neutral; LoadedImage is not (memfd, 4 KiB pages), so only the parser comes along.
add_library(raven_translate_pe STATIC ${RAVEN_NATIVE_DIR}/pe/pe.cpp ${RAVEN_NATIVE_DIR}/pe/resources.cpp)
target_include_directories(raven_translate_pe PUBLIC ${RAVEN_NATIVE_DIR})
add_library(raven_translate_test_process STATIC test_process.cpp)
target_link_libraries(raven_translate_test_process PUBLIC raven_translate raven_translate_pe)

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

# The emitter's oracle: the host assembler and LLVM's disassembler read the same instructions the emitter encodes.
add_executable(a64_emitter_test a64_emitter_test.cpp)
target_link_libraries(a64_emitter_test PRIVATE raven_translate)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/a64_cases.txt
    COMMAND a64_emitter_test --assembly ${CMAKE_CURRENT_BINARY_DIR}/a64_cases.S
    COMMAND ${CMAKE_C_COMPILER} -c -march=armv8.4-a+lse+rcpc+flagm -o ${CMAKE_CURRENT_BINARY_DIR}/a64_cases.o
        ${CMAKE_CURRENT_BINARY_DIR}/a64_cases.S
    COMMAND ${RAVEN_OBJDUMP} -d ${CMAKE_CURRENT_BINARY_DIR}/a64_cases.o > ${CMAKE_CURRENT_BINARY_DIR}/a64_cases.txt
    DEPENDS a64_emitter_test VERBATIM)
add_custom_target(a64_cases ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/a64_cases.txt)
add_test(NAME translate_a64_emitter COMMAND a64_emitter_test ${CMAKE_CURRENT_BINARY_DIR}/a64_cases.txt)
set_tests_properties(translate_a64_emitter PROPERTIES TIMEOUT 30)

add_executable(runtime_test runtime_test.cpp)
target_link_libraries(runtime_test PRIVATE raven_translate_test_process)
add_dependencies(runtime_test translate_guests)
add_test(NAME translate_runtime COMMAND runtime_test ${GUEST})
set_tests_properties(translate_runtime PROPERTIES TIMEOUT 60)

# The translator's semantics against an x86-64 CPU: on a Mac, Rosetta runs the x86-64 build of the oracle. Elsewhere this needs an
# x86-64 machine or emulator to produce the results file, and is not built.
if(APPLE)
    set(SEMANTICS ${CMAKE_CURRENT_BINARY_DIR}/semantics)
    file(MAKE_DIRECTORY ${SEMANTICS})
    add_custom_command(OUTPUT ${SEMANTICS}/x86_semantics_cases.s ${SEMANTICS}/x86_semantics_cases.h
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/x86_semantics.py ${SEMANTICS}/x86_semantics_cases.s
            ${SEMANTICS}/x86_semantics_cases.h
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/x86_semantics.py VERBATIM)
    add_custom_command(OUTPUT ${SEMANTICS}/oracle.bin
        COMMAND ${CMAKE_C_COMPILER} -arch x86_64 -O1 -I ${SEMANTICS} -o ${SEMANTICS}/x86_semantics_oracle
            ${CMAKE_CURRENT_SOURCE_DIR}/x86_semantics_oracle.c ${SEMANTICS}/x86_semantics_cases.s
        COMMAND ${SEMANTICS}/x86_semantics_oracle ${SEMANTICS}/oracle.bin
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/x86_semantics_oracle.c ${SEMANTICS}/x86_semantics_cases.s ${SEMANTICS}/x86_semantics_cases.h
        VERBATIM)
    add_custom_target(x86_semantics_oracle ALL DEPENDS ${SEMANTICS}/oracle.bin)
    add_executable(x86_semantics_test x86_semantics_test.cpp)
    target_link_libraries(x86_semantics_test PRIVATE raven_translate)
    add_dependencies(x86_semantics_test x86_semantics_oracle)
    add_test(NAME translate_x86_semantics COMMAND x86_semantics_test ${SEMANTICS}/oracle.bin)
    set_tests_properties(translate_x86_semantics PROPERTIES TIMEOUT 120)

    # entry.c's loop as a native arm64 program and as an x86-64 one Rosetta runs, for translate_bench to be compared with.
    add_executable(bench_entry_native bench_entry_host.c ${RAVEN_TESTDATA}/native_cpu/entry.c)
    target_compile_options(bench_entry_native PRIVATE -O2)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/bench_entry_rosetta
        COMMAND ${CMAKE_C_COMPILER} -arch x86_64 -O2 -o ${CMAKE_CURRENT_BINARY_DIR}/bench_entry_rosetta
            ${CMAKE_CURRENT_SOURCE_DIR}/bench_entry_host.c ${RAVEN_TESTDATA}/native_cpu/entry.c
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/bench_entry_host.c ${RAVEN_TESTDATA}/native_cpu/entry.c VERBATIM)
    add_custom_target(bench_entry_rosetta_program ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/bench_entry_rosetta)
endif()

add_executable(translate_bench translate_bench.cpp)
target_link_libraries(translate_bench PRIVATE raven_translate_test_process)
add_dependencies(translate_bench translate_guests)
