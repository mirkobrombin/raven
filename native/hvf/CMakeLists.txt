# x64 code translated by native/translate, run at EL0 in a Hypervisor.framework vCPU under a minimal guest kernel. A project of its own,
# as native/translate is, until the build is split by host. Hypervisor.framework on Apple silicon only: elsewhere this configures to
# nothing.
cmake_minimum_required(VERSION 3.20)
project(RavenHvf LANGUAGES C CXX)

if(NOT (CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$"))
    message(STATUS "native/hvf needs Darwin on arm64; nothing to build here")
    return()
endif()
enable_language(ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
enable_testing()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../translate ${CMAKE_CURRENT_BINARY_DIR}/translate)
set(RAVEN_TESTDATA ${CMAKE_CURRENT_SOURCE_DIR}/../../testdata)
set(TRANSLATE_TESTDATA ${CMAKE_CURRENT_SOURCE_DIR}/../translate/testdata)

add_library(raven_hvf STATIC vm.cpp vcpu.cpp vm_process.cpp kernel.S)
target_include_directories(raven_hvf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(raven_hvf PUBLIC raven_translate_test_process "-framework Hypervisor")
target_compile_options(raven_hvf PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic -Wconversion>)

# Hypervisor.framework needs com.apple.security.hypervisor and nothing else: guest page tables give the guest its low addresses, so no
# provisioning profile is involved. Ad hoc by default; RAVEN_HVF_SIGN_IDENTITY names a keychain identity instead.
set(RAVEN_HVF_SIGN_IDENTITY "-" CACHE STRING "codesign identity for binaries that create a VM")
function(raven_hvf_sign target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND codesign --force --sign ${RAVEN_HVF_SIGN_IDENTITY} --entitlements ${CMAKE_CURRENT_SOURCE_DIR}/hypervisor.entitlements
            $<TARGET_FILE:${target}>
        VERBATIM)
endfunction()

# The guest programs: each workload behind bench_loop, an image base apiece so that one VM holds them all.
set(GUEST ${CMAKE_CURRENT_BINARY_DIR}/guest)
file(MAKE_DIRECTORY ${GUEST})
set(HVF_GUESTS "")
set(HVF_BASE_entry 0x140000000)
set(HVF_BASE_crc32 0x150000000)
set(HVF_BASE_fnv 0x160000000)
set(HVF_BASE_histogram 0x170000000)
foreach(WORKLOAD entry crc32 fnv histogram)
    if(WORKLOAD STREQUAL "entry")
        set(SOURCE ${RAVEN_TESTDATA}/native_cpu/entry.c)
        set(DEFINES -DBENCH_FUNCTION=raven_entry -DBENCH_RESULT=unsigned)
    else()
        string(TOUPPER ${WORKLOAD} WORKLOAD_DEFINE)
        set(SOURCE ${TRANSLATE_TESTDATA}/workloads.c)
        set(DEFINES -DBENCH_FUNCTION=workload -DWORKLOAD_${WORKLOAD_DEFINE})
    endif()
    add_custom_command(OUTPUT ${GUEST}/hvf_${WORKLOAD}.exe
        COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector ${DEFINES}
            -Wl,--entry,bench_loop,--image-base,${HVF_BASE_${WORKLOAD}},--no-insert-timestamp -o ${GUEST}/hvf_${WORKLOAD}.exe
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/bench_loop.c ${SOURCE}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/testdata/bench_loop.c ${SOURCE} VERBATIM)
    list(APPEND HVF_GUESTS ${GUEST}/hvf_${WORKLOAD}.exe)

    # The same loop and workload built for this host: the baseline the benchmark compares with.
    add_executable(hvf_native_${WORKLOAD} testdata/bench_native.c testdata/bench_loop.c ${SOURCE})
    string(REPLACE "-D" "" NATIVE_DEFINES "${DEFINES}")
    target_compile_definitions(hvf_native_${WORKLOAD} PRIVATE ${NATIVE_DEFINES})
    target_compile_options(hvf_native_${WORKLOAD} PRIVATE -O2)
endforeach()
add_custom_command(OUTPUT ${GUEST}/hvf_client.exe
    COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -Wl,--entry,raven_entry,--image-base,0x180000000,--no-insert-timestamp
        -o ${GUEST}/hvf_client.exe ${CMAKE_CURRENT_SOURCE_DIR}/testdata/hvf_client.c
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/testdata/hvf_client.c VERBATIM)
list(APPEND HVF_GUESTS ${GUEST}/hvf_client.exe)
add_custom_target(hvf_guests ALL DEPENDS ${HVF_GUESTS})

# What the workloads return natively, for the test to compare with.
foreach(WORKLOAD crc32 fnv histogram)
    string(TOUPPER ${WORKLOAD} WORKLOAD_DEFINE)
    add_library(hvf_native_${WORKLOAD}_result OBJECT ${TRANSLATE_TESTDATA}/workloads.c)
    target_compile_definitions(hvf_native_${WORKLOAD}_result PRIVATE WORKLOAD_${WORKLOAD_DEFINE} workload=workload_${WORKLOAD})
    target_compile_options(hvf_native_${WORKLOAD}_result PRIVATE -O2)
endforeach()
add_library(hvf_native_entry_result OBJECT ${RAVEN_TESTDATA}/native_cpu/entry.c)
target_compile_definitions(hvf_native_entry_result PRIVATE raven_entry=native_entry)

add_executable(hvf_workloads_test hvf_workloads_test.cpp $<TARGET_OBJECTS:hvf_native_crc32_result>
    $<TARGET_OBJECTS:hvf_native_fnv_result> $<TARGET_OBJECTS:hvf_native_histogram_result> $<TARGET_OBJECTS:hvf_native_entry_result>)
target_link_libraries(hvf_workloads_test PRIVATE raven_hvf)
add_dependencies(hvf_workloads_test hvf_guests)
raven_hvf_sign(hvf_workloads_test)
add_test(NAME hvf_workloads COMMAND hvf_workloads_test ${GUEST})
set_tests_properties(hvf_workloads PROPERTIES TIMEOUT 60)

add_executable(hvf_code_write_test hvf_code_write_test.cpp)
target_link_libraries(hvf_code_write_test PRIVATE raven_hvf)
raven_hvf_sign(hvf_code_write_test)
add_test(NAME hvf_code_write COMMAND hvf_code_write_test 20000)
set_tests_properties(hvf_code_write PROPERTIES TIMEOUT 60)

add_executable(hvf_litmus_test hvf_litmus_test.cpp)
target_link_libraries(hvf_litmus_test PRIVATE raven_hvf)
add_dependencies(hvf_litmus_test hvf_guests)
raven_hvf_sign(hvf_litmus_test)
add_test(NAME hvf_litmus COMMAND hvf_litmus_test ${GUEST} 30 2000000)
set_tests_properties(hvf_litmus PROPERTIES TIMEOUT 120)

add_executable(hvf_spin_test hvf_spin_test.cpp)
target_link_libraries(hvf_spin_test PRIVATE raven_hvf)
add_dependencies(hvf_spin_test hvf_guests)
raven_hvf_sign(hvf_spin_test)
add_test(NAME hvf_spin COMMAND hvf_spin_test ${GUEST} 2)
set_tests_properties(hvf_spin PROPERTIES TIMEOUT 60)
