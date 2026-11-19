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
set(HVF_BASE_saxpy 0x1b0000000)
set(HVF_BASE_dot 0x1c0000000)
set(HVF_BASE_copy 0x1d0000000)
set(HVF_BASE_ldot 0x1e0000000)
set(HVF_BASE_lpoly 0x1f0000000)
set(HVF_BASE_fsaxpy 0x200000000)
set(HVF_BASE_bytecount 0x210000000)
set(HVF_BASE_strscan 0x220000000)
set(HVF_BASE_bitdeposit 0x230000000)
foreach(WORKLOAD entry crc32 fnv histogram saxpy dot copy ldot lpoly fsaxpy bytecount strscan bitdeposit)
    set(GUEST_FLAGS "")
    if(WORKLOAD STREQUAL "entry")
        set(SOURCE ${RAVEN_TESTDATA}/native_cpu/entry.c)
        set(DEFINES -DBENCH_FUNCTION=raven_entry -DBENCH_RESULT=unsigned)
    elseif(WORKLOAD MATCHES "^(saxpy|dot|copy)$")
        string(TOUPPER ${WORKLOAD} WORKLOAD_DEFINE)
        set(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/testdata/sse_workloads.c)
        set(DEFINES -DBENCH_FUNCTION=workload -DWORKLOAD_${WORKLOAD_DEFINE})
    elseif(WORKLOAD MATCHES "^(fsaxpy|bytecount)$")
        string(TOUPPER ${WORKLOAD} WORKLOAD_DEFINE)
        set(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/testdata/avx_workloads.c)
        set(DEFINES -DBENCH_FUNCTION=workload -DWORKLOAD_${WORKLOAD_DEFINE})
        set(GUEST_FLAGS -mavx2 -mfma -mno-popcnt)
    elseif(WORKLOAD MATCHES "^(strscan|bitdeposit)$")
        string(TOUPPER ${WORKLOAD} WORKLOAD_DEFINE)
        set(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/testdata/avx_workloads.c)
        set(DEFINES -DBENCH_FUNCTION=workload -DWORKLOAD_${WORKLOAD_DEFINE})
        set(GUEST_FLAGS -msse4.2 -mbmi2 -mno-popcnt)
    elseif(WORKLOAD MATCHES "^(ldot|lpoly)$")
        string(TOUPPER ${WORKLOAD} WORKLOAD_DEFINE)
        set(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/testdata/x87_workloads.c)
        set(DEFINES -DBENCH_FUNCTION=workload -DWORKLOAD_${WORKLOAD_DEFINE})
    else()
        string(TOUPPER ${WORKLOAD} WORKLOAD_DEFINE)
        set(SOURCE ${TRANSLATE_TESTDATA}/workloads.c)
        set(DEFINES -DBENCH_FUNCTION=workload -DWORKLOAD_${WORKLOAD_DEFINE})
    endif()
    add_custom_command(OUTPUT ${GUEST}/hvf_${WORKLOAD}.exe
        COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector ${DEFINES} ${GUEST_FLAGS}
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
# An ARM64EC DLL without CRT (chpe.o from mingwex gives the __os_arm64x_* slots the thunks use) and an x64 client that imports it
# through an import library made for x64 from the .def.
find_program(RAVEN_MINGW_EC arm64ec-w64-mingw32-clang REQUIRED)
find_program(RAVEN_DLLTOOL llvm-dlltool REQUIRED)
add_custom_command(OUTPUT ${GUEST}/ec_library.dll
    COMMAND ${RAVEN_MINGW_EC} -O2 -nostdlib -shared -Wl,--entry,ec_library_entry,--image-base,0x190000000,--no-insert-timestamp
        -o ${GUEST}/ec_library.dll ${CMAKE_CURRENT_SOURCE_DIR}/testdata/ec_library.c -lmingwex
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/testdata/ec_library.c VERBATIM)
add_custom_command(OUTPUT ${GUEST}/libec_library.a
    COMMAND ${RAVEN_DLLTOOL} -m i386:x86-64 -d ${CMAKE_CURRENT_SOURCE_DIR}/testdata/ec_library.def -l ${GUEST}/libec_library.a
        -D ec_library.dll
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/testdata/ec_library.def VERBATIM)
add_custom_command(OUTPUT ${GUEST}/ec_client.exe
    COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -Wl,--entry,raven_entry,--image-base,0x1a0000000,--no-insert-timestamp
        -o ${GUEST}/ec_client.exe ${CMAKE_CURRENT_SOURCE_DIR}/testdata/ec_client.c ${GUEST}/libec_library.a
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/testdata/ec_client.c ${GUEST}/libec_library.a VERBATIM)
list(APPEND HVF_GUESTS ${GUEST}/ec_library.dll ${GUEST}/ec_client.exe)
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
add_test(NAME hvf_spin COMMAND hvf_spin_test ${GUEST} 3)
set_tests_properties(hvf_spin PROPERTIES TIMEOUT 60)

add_executable(hvf_bench hvf_bench.cpp)
target_link_libraries(hvf_bench PRIVATE raven_hvf)
add_dependencies(hvf_bench hvf_guests)
raven_hvf_sign(hvf_bench)

add_executable(hvf_vm_cost hvf_vm_cost.cpp)
target_link_libraries(hvf_vm_cost PRIVATE raven_hvf)
add_dependencies(hvf_vm_cost hvf_guests)
raven_hvf_sign(hvf_vm_cost)

add_executable(hvf_shared_test hvf_shared_test.cpp $<TARGET_OBJECTS:hvf_native_crc32_result>)
target_link_libraries(hvf_shared_test PRIVATE raven_hvf)
add_dependencies(hvf_shared_test hvf_guests)
raven_hvf_sign(hvf_shared_test)
add_test(NAME hvf_shared COMMAND hvf_shared_test ${GUEST} 20)
set_tests_properties(hvf_shared PROPERTIES TIMEOUT 120)

add_executable(hvf_code_change_test hvf_code_change_test.cpp)
target_link_libraries(hvf_code_change_test PRIVATE raven_hvf)
add_dependencies(hvf_code_change_test hvf_guests)
raven_hvf_sign(hvf_code_change_test)
add_test(NAME hvf_code_change COMMAND hvf_code_change_test ${GUEST} 10)
set_tests_properties(hvf_code_change PROPERTIES TIMEOUT 120)

add_executable(hvf_code_race_test hvf_code_race_test.cpp)
target_link_libraries(hvf_code_race_test PRIVATE raven_hvf)
add_dependencies(hvf_code_race_test hvf_guests)
raven_hvf_sign(hvf_code_race_test)
add_test(NAME hvf_code_race COMMAND hvf_code_race_test ${GUEST})
set_tests_properties(hvf_code_race PROPERTIES TIMEOUT 60)

# A known gap (docs/macos/hvf/README.md, open items): passes only on the line that reports it, so an error fails it and so does the gap
# being closed, which should then make this an ordinary test.
add_executable(hvf_same_block_test hvf_same_block_test.cpp)
target_link_libraries(hvf_same_block_test PRIVATE raven_hvf)
add_dependencies(hvf_same_block_test hvf_guests)
raven_hvf_sign(hvf_same_block_test)
add_test(NAME hvf_same_block COMMAND hvf_same_block_test ${GUEST})
set_tests_properties(hvf_same_block PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "known gap: patch_next stored 2 into its own next instruction and ran the old one")

add_executable(hvf_granule_test hvf_granule_test.cpp)
target_link_libraries(hvf_granule_test PRIVATE raven_hvf)
add_dependencies(hvf_granule_test hvf_guests)
raven_hvf_sign(hvf_granule_test)
add_test(NAME hvf_granule COMMAND hvf_granule_test ${GUEST})
set_tests_properties(hvf_granule PROPERTIES TIMEOUT 60)

add_executable(hvf_two_runtimes_test hvf_two_runtimes_test.cpp)
target_link_libraries(hvf_two_runtimes_test PRIVATE raven_hvf)
add_dependencies(hvf_two_runtimes_test hvf_guests)
raven_hvf_sign(hvf_two_runtimes_test)
add_test(NAME hvf_two_runtimes COMMAND hvf_two_runtimes_test ${GUEST})
set_tests_properties(hvf_two_runtimes PROPERTIES TIMEOUT 60)

add_library(hvf_ec_library_host OBJECT testdata/ec_library.c)
target_compile_definitions(hvf_ec_library_host PRIVATE EC_LIBRARY_HOST)
target_compile_options(hvf_ec_library_host PRIVATE -O2)
add_executable(hvf_arm64ec_test hvf_arm64ec_test.cpp $<TARGET_OBJECTS:hvf_ec_library_host>)
target_link_libraries(hvf_arm64ec_test PRIVATE raven_hvf)
add_dependencies(hvf_arm64ec_test hvf_guests)
raven_hvf_sign(hvf_arm64ec_test)
add_test(NAME hvf_arm64ec COMMAND hvf_arm64ec_test ${GUEST})
set_tests_properties(hvf_arm64ec PROPERTIES TIMEOUT 60)

# CPUID and XGETBV in a vCPU, by the rules translate_cpuid checks in this process.
add_executable(hvf_cpuid_test hvf_cpuid_test.cpp)
target_link_libraries(hvf_cpuid_test PRIVATE raven_hvf)
raven_hvf_sign(hvf_cpuid_test)
add_test(NAME hvf_cpuid COMMAND hvf_cpuid_test)
set_tests_properties(hvf_cpuid PROPERTIES TIMEOUT 30)

# The SSE oracle's cases in a vCPU, against the results the translate build recorded under Rosetta.
add_executable(hvf_sse_test hvf_sse_test.cpp)
target_link_libraries(hvf_sse_test PRIVATE raven_hvf)
add_dependencies(hvf_sse_test x86_sse_oracle)
raven_hvf_sign(hvf_sse_test)
add_test(NAME hvf_sse COMMAND hvf_sse_test ${CMAKE_CURRENT_BINARY_DIR}/translate/semantics/sse_oracle.bin)
set_tests_properties(hvf_sse PROPERTIES TIMEOUT 120)

# The AVX oracle's cases in a vCPU, with the same silicon results as translate_x86_avx.
add_executable(hvf_avx_test hvf_avx_test.cpp)
target_link_libraries(hvf_avx_test PRIVATE raven_hvf)
add_dependencies(hvf_avx_test x86_avx_oracle)
raven_hvf_sign(hvf_avx_test)
add_test(NAME hvf_avx COMMAND hvf_avx_test ${CMAKE_CURRENT_BINARY_DIR}/translate/semantics/avx_oracle.bin
    ${CMAKE_CURRENT_SOURCE_DIR}/../translate/testdata/x86_avx_silicon.txt)
set_tests_properties(hvf_avx PROPERTIES TIMEOUT 120)

# A 256-bit instruction whose memory operand faults leaves the upper halves and memory as they were.
add_executable(hvf_avx_fault_test hvf_avx_fault_test.cpp)
target_link_libraries(hvf_avx_fault_test PRIVATE raven_hvf)
raven_hvf_sign(hvf_avx_fault_test)
add_test(NAME hvf_avx_fault COMMAND hvf_avx_fault_test)
set_tests_properties(hvf_avx_fault PROPERTIES TIMEOUT 30)

# The x87 oracle's cases in a vCPU, with the same silicon results as translate_x87.
add_executable(hvf_x87_test hvf_x87_test.cpp)
target_link_libraries(hvf_x87_test PRIVATE raven_hvf)
add_dependencies(hvf_x87_test x87_oracle)
raven_hvf_sign(hvf_x87_test)
add_test(NAME hvf_x87 COMMAND hvf_x87_test ${CMAKE_CURRENT_BINARY_DIR}/translate/semantics/x87_oracle.bin
    ${CMAKE_CURRENT_SOURCE_DIR}/../translate/testdata/x87_silicon.txt)
set_tests_properties(hvf_x87 PROPERTIES TIMEOUT 120)
