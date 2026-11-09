add_library(raven_pe STATIC pe/pe.cpp pe/loaded_image.cpp pe/module_set.cpp pe/resources.cpp)
target_include_directories(raven_pe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(raven_pe PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

add_library(raven_pixels STATIC graphics/icon_bitmap.cpp graphics/icon_group.cpp graphics/stretch.cpp)
target_include_directories(raven_pixels PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(raven_pixels PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
add_executable(icon_bitmap_test graphics/icon_bitmap_test.cpp)
target_link_libraries(icon_bitmap_test PRIVATE raven_pixels raven_pe)
add_test(NAME native_icon_dib_contract COMMAND icon_bitmap_test
    ${RAVEN_TESTDATA_DIR}/npp/notepad++.exe ${CMAKE_CURRENT_BINARY_DIR}/icon-output)
set_tests_properties(native_icon_dib_contract PROPERTIES TIMEOUT 30)
add_executable(icon_group_test graphics/icon_group_test.cpp)
target_link_libraries(icon_group_test PRIVATE raven_pixels raven_pe)
add_test(NAME native_icon_group_contract COMMAND icon_group_test ${RAVEN_TESTDATA_DIR}/npp/notepad++.exe)
set_tests_properties(native_icon_group_contract PROPERTIES TIMEOUT 30)
add_executable(stretch_test graphics/stretch_test.cpp)
target_include_directories(stretch_test PRIVATE ${PROJECT_SOURCE_DIR}/testdata/native_loader)
target_link_libraries(stretch_test PRIVATE raven_pixels)
add_test(NAME native_blackonwhite_contract COMMAND stretch_test
    ${RAVEN_TESTDATA_DIR}/native_loader/blackonwhite_reference.bin)
set_tests_properties(native_blackonwhite_contract PROPERTIES TIMEOUT 30)

add_library(raven_shader STATIC shader/dxbc.cpp shader/hlsl_lexer.cpp shader/hlsl_parser.cpp shader/hlsl_codegen.cpp
    shader/hlsl_compile.cpp shader/d3dbc.cpp shader/hlsl_effect.cpp shader/hlsl_effect9.cpp shader/hlsl_legacy.cpp shader/sm4.cpp)
target_include_directories(raven_shader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(raven_shader PRIVATE -Wall -Wextra)
add_executable(hlsl_compile_test shader/hlsl_compile_test.cpp)
target_link_libraries(hlsl_compile_test PRIVATE raven_shader)
add_test(NAME native_hlsl_compiler COMMAND hlsl_compile_test ${PROJECT_SOURCE_DIR}/testdata/hlsl ${CMAKE_CURRENT_BINARY_DIR}/hlsl-output)
set_tests_properties(native_hlsl_compiler PROPERTIES TIMEOUT 120)

# The manifest of lavapipe, the software Vulkan driver tests run on: lvp_icd.json on Debian and Ubuntu, lvp_icd.x86_64.json on
# Fedora and Arch.
find_file(RAVEN_LAVAPIPE_ICD NAMES lvp_icd.json lvp_icd.x86_64.json PATHS /usr/share/vulkan/icd.d NO_DEFAULT_PATH)
find_package(PNG 1.6.59 QUIET)
find_package(OpenSSL QUIET)
find_package(Freetype QUIET)
if(PNG_FOUND)
    target_sources(raven_pixels PRIVATE graphics/png_codec.cpp)
    target_compile_definitions(raven_pixels PUBLIC RAVEN_PNG=1)
    target_link_libraries(raven_pixels PUBLIC PNG::PNG)
    add_executable(png_codec_test graphics/png_codec_test.cpp)
    target_link_libraries(png_codec_test PRIVATE raven_pixels raven_pe)
    add_test(NAME native_png_codec_contract COMMAND png_codec_test
        ${RAVEN_TESTDATA_DIR}/npp/notepad++.exe ${CMAKE_CURRENT_BINARY_DIR}/png-output)
    set_tests_properties(native_png_codec_contract PROPERTIES TIMEOUT 30)
else()
    # Tests that decode PNG images need libpng 1.6.59 or newer; without it they are not built, so say so instead of staying silent.
    message(WARNING "libpng 1.6.59 or newer not found: PNG decoding and the tests that depend on it are disabled. "
        "Run scripts/provision-libpng.py and configure with the PNG_LIBRARY_RELEASE and PNG_PNG_INCLUDE_DIR it prints.")
    add_test(NAME native_png_dependent_tests_unavailable COMMAND ${CMAKE_COMMAND} -E echo
        "libpng 1.6.59 or newer is missing: PNG-dependent tests were not built (see scripts/provision-libpng.py)")
    set_tests_properties(native_png_dependent_tests_unavailable PROPERTIES SKIP_REGULAR_EXPRESSION "libpng 1.6.59 or newer is missing")
endif()

add_executable(raven main.cpp)
target_link_libraries(raven PRIVATE raven_pe)

add_executable(pe_test pe/pe_test.cpp)
target_link_libraries(pe_test PRIVATE raven_pe)
add_test(NAME pe_contracts COMMAND pe_test)
set_tests_properties(pe_contracts PROPERTIES TIMEOUT 10)

add_executable(module_set_test pe/module_set_test.cpp)
target_link_libraries(module_set_test PRIVATE raven_pe)
add_test(NAME module_set_contracts COMMAND module_set_test)
set_tests_properties(module_set_contracts PROPERTIES TIMEOUT 10)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    enable_language(ASM)
    add_executable(raven-bootstrap cpu/bootstrap.S)
    target_compile_options(raven-bootstrap PRIVATE -fno-sanitize=all)
    target_link_options(raven-bootstrap PRIVATE -fno-sanitize=all -nostdlib -static
        -Wl,-no-pie,-Ttext-segment=0x700000000000,-e,_start,--build-id=none)
    add_library(raven_cpu STATIC cpu/native_cpu.cpp nt/normalization.cpp nt/environment.cpp nt/system.cpp nt/firmware.cpp nt/interfaces.cpp nt/providers.cpp nt/api.cpp nt/unwind.cpp nt/seh.cpp nt/apiset.cpp nt/system_exports.cpp nt/text.cpp nt/paths.cpp nt/file_api.cpp nt/ansi_api.cpp nt/shlwapi_api.cpp nt/registry_api.cpp nt/shell_api.cpp nt/platform_api.cpp nt/hash.cpp nt/version_api.cpp nt/system_api.cpp nt/process_api.cpp nt/actctx.cpp nt/security_api.cpp nt/access_token.cpp nt/event.cpp nt/mutex.cpp nt/child_api.cpp nt/child_process.cpp nt/winsock_api.cpp nt/host_api.cpp nt/pipe.cpp nt/job_api.cpp nt/wave_api.cpp nt/mmio_api.cpp nt/inherit.cpp nt/handle_channel.cpp nt/elevation.cpp nt/snapshot.cpp nt/restart.cpp nt/service_api.cpp nt/crypt32_api.cpp nt/bcrypt_api.cpp nt/authenticode.cpp nt/process_info_api.cpp nt/font_service.cpp nt/shader_service.cpp nt/canvas_service.cpp nt/sync_api.cpp nt/threads.cpp nt/thread_api.cpp nt/text_api.cpp nt/console.cpp nt/console_api.cpp nt/memory_api.cpp nt/guest_allocator.cpp nt/module_api.cpp nt/loader.cpp nt/tls.cpp nt/exceptions.cpp nt/resources.cpp nt/memory.cpp nt/objects.cpp nt/object_api.cpp nt/heap.cpp desktop/objects.cpp desktop/nonclient.cpp desktop/api.cpp desktop/surface.cpp desktop/windows.cpp desktop/system.cpp desktop/user_api.cpp desktop/window_manager.cpp desktop/window_ansi.cpp desktop/menu_manager.cpp desktop/shared_pixels.cpp desktop/foreign_surface.cpp desktop/user_ext.cpp desktop/scrollbar.cpp desktop/window_long.cpp desktop/child_frame.cpp desktop/gdi.cpp desktop/gdi_api.cpp desktop/gdi_text.cpp desktop/theme.cpp)
    target_include_directories(raven_cpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    set(NLS_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/upstream/windows-codepages)
    file(SHA256 ${NLS_SOURCE}/c_1252.txt NLS_TEXT_HASH)
    if(NOT NLS_TEXT_HASH STREQUAL "d606b8bf22f35178703d201cf81c45783bc211f0437f0d17db61c13fe53f1c69")
        message(FATAL_ERROR "Pinned CP1252 data differs")
    endif()
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/nls-generated)
    add_executable(raven-nls-tables nt/nls_tables.cpp)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/nls-generated/nls-cp1252.h
        COMMAND raven-nls-tables ${NLS_SOURCE}/c_1252.txt ${CMAKE_CURRENT_BINARY_DIR}/nls-generated/c_1252.nls
        COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_BINARY_DIR}/nls-generated/c_1252.nls
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/nls-generated/nls-cp1252.h -P ${NLS_SOURCE}/embed.cmake
        DEPENDS raven-nls-tables ${NLS_SOURCE}/c_1252.txt ${NLS_SOURCE}/embed.cmake VERBATIM)
    # The other Windows single-byte code pages, which rich text and ANSI programs written for other regions convert through.
    set(NLS_SINGLE_BYTE_PAGES 1250 1251 1253 1254 1255 1256 1257 1258)
    set(NLS_SINGLE_BYTE_HASHES
        d6479e3a622ae1c80eced54ce11551f6e7bf40f68d1f45ba54fb274a5e9c592e 7ced69ac085f129fc0ba78f847d0519bc47092c1625a33e35828bfed4972005a
        15a6b99022011f70fad05221d7be4c6fccc5213bf7c75c7146bb565808e6c8d9 736c96e1f8135e7db4bec13517193ba42a81f773e6aa94df1b50c0ca25e02549
        e73e5fcf861fc9612688dc5abc11c348a998aa9e64420b296bb9b5c6990fa3d8 554a127084d0d30e94fc6f9b2d21c616900c78bde9498c7e4bdc14ae1e4a83bf
        efbee49b9e1094a46adc96ad8149d989aa5ab3e2c0fbaf1564516da9b49f42ab 6956c82004b36fbf480aeeba541e0abbe329a2c42964353e4e54baf1d227242b)
    set(NLS_SINGLE_BYTE_TEXTS "")
    foreach(PAGE HASH IN ZIP_LISTS NLS_SINGLE_BYTE_PAGES NLS_SINGLE_BYTE_HASHES)
        file(SHA256 ${NLS_SOURCE}/c_${PAGE}.txt PAGE_HASH)
        if(NOT PAGE_HASH STREQUAL HASH)
            message(FATAL_ERROR "Pinned code page ${PAGE} data differs")
        endif()
        list(APPEND NLS_SINGLE_BYTE_TEXTS ${NLS_SOURCE}/c_${PAGE}.txt)
    endforeach()
    # The OEM pages of a console: Unicode's tables as published.
    set(NLS_OEM_PAGES 437 850)
    set(NLS_OEM_HASHES 6bad4dabcdf5940227c7d81fab130dcb18a77850b5d79de28b5dc4e047b0aaac ffdcc3c1c72f1aef600a63547100ef3dc452a09ad84923d382085519751c7479)
    foreach(PAGE HASH IN ZIP_LISTS NLS_OEM_PAGES NLS_OEM_HASHES)
        file(SHA256 ${NLS_SOURCE}/CP${PAGE}.TXT PAGE_HASH)
        if(NOT PAGE_HASH STREQUAL HASH)
            message(FATAL_ERROR "Pinned Unicode code page ${PAGE} differs")
        endif()
        list(APPEND NLS_SINGLE_BYTE_TEXTS ${NLS_SOURCE}/CP${PAGE}.TXT)
    endforeach()
    list(APPEND NLS_SINGLE_BYTE_PAGES ${NLS_OEM_PAGES})
    list(JOIN NLS_SINGLE_BYTE_PAGES "," NLS_SINGLE_BYTE_ARGUMENT)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/nls-generated/nls-sbcs.h
        COMMAND ${CMAKE_COMMAND} -DSOURCE=${NLS_SOURCE} -DPAGES=${NLS_SINGLE_BYTE_ARGUMENT}
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/nls-generated/nls-sbcs.h -P ${NLS_SOURCE}/sbcs.cmake
        DEPENDS ${NLS_SINGLE_BYTE_TEXTS} ${NLS_SOURCE}/sbcs.cmake VERBATIM)
    # The Windows double-byte pages: Japanese, Simplified Chinese, Korean and Traditional Chinese, from Unicode's best-fit tables.
    set(DBCS_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/upstream/windows-bestfit)
    set(NLS_DOUBLE_BYTE_PAGES 932 936 949 950)
    set(NLS_DOUBLE_BYTE_HASHES
        1b272514615653acd8031a22ea7fd2229a2b517afed26af606e8e2b5cbe85408 fceff446a5a8e70b9543c9a659757e6a36278101033900b879d99415303a8687
        41cb5a618fd0a6ba859d8ba78e3621573700426ea5da67cd680855b3d3a7de73 2fe67e3872ede8eb2358c4ca084146dcb5b88b6ac71e640d262785c40bc3fa59)
    set(NLS_DOUBLE_BYTE_TEXTS "")
    foreach(PAGE HASH IN ZIP_LISTS NLS_DOUBLE_BYTE_PAGES NLS_DOUBLE_BYTE_HASHES)
        file(SHA256 ${DBCS_SOURCE}/bestfit${PAGE}.txt PAGE_HASH)
        if(NOT PAGE_HASH STREQUAL HASH)
            message(FATAL_ERROR "Pinned Unicode code page ${PAGE} differs")
        endif()
        list(APPEND NLS_DOUBLE_BYTE_TEXTS ${DBCS_SOURCE}/bestfit${PAGE}.txt)
    endforeach()
    add_executable(raven-dbcs-tables nt/dbcs_tables.cpp)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/nls-generated/nls-dbcs.h
        COMMAND raven-dbcs-tables ${CMAKE_CURRENT_BINARY_DIR}/nls-generated/nls-dbcs.h ${NLS_DOUBLE_BYTE_TEXTS}
        DEPENDS raven-dbcs-tables ${NLS_DOUBLE_BYTE_TEXTS} VERBATIM)
    # The case tables of l_intl.nls, which the .NET Framework 2.0 runtime maps from the system directory of either width.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/l_intl.nls ${CMAKE_CURRENT_BINARY_DIR}/x86/l_intl.nls
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/generate-intl-nls.py ${CMAKE_CURRENT_BINARY_DIR}/l_intl.nls
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/x86
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/l_intl.nls ${CMAKE_CURRENT_BINARY_DIR}/x86/l_intl.nls
            DEPENDS ${PROJECT_SOURCE_DIR}/scripts/generate-intl-nls.py VERBATIM)
        add_custom_target(intl_nls ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/l_intl.nls ${CMAKE_CURRENT_BINARY_DIR}/x86/l_intl.nls)
        # The runtime's Windows Runtime metadata, which prefixes keep in System32\WinMetadata for .NET programs that call the Windows Runtime.
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/WinMetadata/Windows.winmd
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/WinMetadata
            COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/scripts/generate-winmd.py ${PROJECT_SOURCE_DIR}/native/guest/raven-winmd/windows.types
                ${CMAKE_CURRENT_BINARY_DIR}/WinMetadata/Windows.winmd
            DEPENDS ${PROJECT_SOURCE_DIR}/scripts/generate-winmd.py ${PROJECT_SOURCE_DIR}/native/guest/raven-winmd/windows.types VERBATIM)
        add_custom_target(windows_winmd ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/WinMetadata/Windows.winmd)
    endif()
    target_sources(raven_cpu PRIVATE nt/nls.cpp nt/multibyte.cpp nt/startup.cpp nt/launch.cpp nt/file.cpp nt/prefix.cpp nt/registry.cpp nt/advapi_api.cpp nt/msvcrt_api.cpp nt/profile_api.cpp nt/x86_process.cpp nt/x86_thread_api.cpp nt/x86_compat_api.cpp nt/x86_loader.cpp nt/x86_system_api.cpp nt/registry_words.cpp nt/x86_tls.cpp nt/x86_com.cpp nt/x86_dsound.cpp nt/x86_d3d9.cpp ${CMAKE_CURRENT_BINARY_DIR}/nls-generated/nls-cp1252.h
        ${CMAKE_CURRENT_BINARY_DIR}/nls-generated/nls-sbcs.h ${CMAKE_CURRENT_BINARY_DIR}/nls-generated/nls-dbcs.h)
    # The browser version of the runtime's WebView2 runtime, which the registry names and the runtime's folder carries: the Chromium
    # version of the page engine it draws with.
    file(READ ${PROJECT_SOURCE_DIR}/native/guest/raven-webengine/cef.json cef_manifest)
    string(JSON cef_release GET ${cef_manifest} version)
    string(REGEX REPLACE ".*chromium-" "" RAVEN_WEBVIEW_VERSION ${cef_release})
    target_compile_definitions(raven_cpu PRIVATE RAVEN_WEBVIEW_VERSION=u"${RAVEN_WEBVIEW_VERSION}")
    file(SHA256 ${CMAKE_CURRENT_SOURCE_DIR}/upstream/unicode/cp1252-decomposition.inc DECOMPOSITION_HASH)
    if(NOT DECOMPOSITION_HASH STREQUAL "dc36d1023c7fcd96043122aae2854b6f67febb7debfe4be8780aadbb5a9ba30b")
        message(FATAL_ERROR "Pinned CP1252 decompositions differ")
    endif()
    add_executable(authenticode_test nt/authenticode_test.cpp)
    target_link_libraries(authenticode_test PRIVATE raven_cpu)
    add_executable(launch_test nt/launch_test.cpp)
    target_link_libraries(launch_test PRIVATE raven_cpu)
    add_test(NAME native_launch_arguments_contract COMMAND launch_test)
    set_tests_properties(native_launch_arguments_contract PROPERTIES TIMEOUT 15)
    target_include_directories(raven_cpu PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/nls-generated)
    add_executable(nls_test nt/nls_test.cpp)
    target_link_libraries(nls_test PRIVATE raven_cpu)
    add_test(NAME native_ansi_code_page COMMAND nls_test ${RAVEN_TESTDATA_DIR}/native_loader/cp1252_reference.bin)
    set_tests_properties(native_ansi_code_page PROPERTIES TIMEOUT 15)
    target_sources(raven_cpu PRIVATE desktop/brush.cpp desktop/bitblt.cpp)
    target_link_libraries(raven_cpu PUBLIC raven_pe raven_pixels raven_shader)
    # The certificate chain functions of crypt32 are answered by the host's X.509 verifier when OpenSSL is there.
    if(OPENSSL_FOUND)
        target_link_libraries(raven_cpu PUBLIC OpenSSL::Crypto)
        target_compile_definitions(raven_cpu PRIVATE RAVEN_OPENSSL=1)
        # The Schannel package of SSPI is a TLS client over the same library.
        target_sources(raven_cpu PRIVATE nt/schannel_api.cpp nt/ntlm.cpp)
        add_executable(ntlm_test nt/ntlm_test.cpp)
        target_link_libraries(ntlm_test PRIVATE raven_cpu)
        add_test(NAME native_ntlm_contract COMMAND ntlm_test)
        set_tests_properties(native_ntlm_contract PROPERTIES TIMEOUT 15)
        # WinINet's HTTP client uses it for HTTPS.
        target_sources(raven_cpu PRIVATE nt/wininet_api.cpp)
        target_link_libraries(raven_cpu PUBLIC OpenSSL::SSL)
    endif()
    # Font files are read and rasterized for the DirectWrite component with FreeType when it is there.
    if(FREETYPE_FOUND)
        target_link_libraries(raven_cpu PUBLIC Freetype::Freetype)
        # The Skia library that some programs link after it needs FreeType too; keep the library whatever the link order drops.
        target_link_options(raven_cpu INTERFACE LINKER:--no-as-needed)
        target_compile_definitions(raven_cpu PRIVATE RAVEN_FREETYPE=1)
    endif()
    set(RAVEN_SKIA_SOURCE "" CACHE PATH "Pinned Skia source directory")
    set(RAVEN_SKIA_LIBRARY "" CACHE FILEPATH "Font-enabled CPU Skia library")
    set(RAVEN_FONT_DIRECTORY "" CACHE PATH "Qualified regular font directory")
    if(RAVEN_SKIA_SOURCE OR RAVEN_SKIA_LIBRARY OR RAVEN_FONT_DIRECTORY)
        if(NOT EXISTS "${RAVEN_SKIA_SOURCE}/include/core/SkFont.h" OR NOT EXISTS "${RAVEN_SKIA_LIBRARY}" OR NOT IS_DIRECTORY "${RAVEN_FONT_DIRECTORY}")
            message(FATAL_ERROR "Skia text requires source, CPU library and qualified font directory")
        endif()
        set(FONT_FILES LiberationSans-Regular.ttf LiberationMono-Regular.ttf LiberationSerif-Regular.ttf)
        set(FONT_HASHES 075e71be4c58beda172ecb574a799308f04c3d054c6a5b7434954d3b0f1b28d8
            d18235f1e874771f4eaec3ddd6ddb4db849d334bdc6930c5c2185d9344834e62
            350f4ffd773cae53560c63dca3d89a5070595614fde12f772bf43feb63d81064 CACHE STRING "SHA-256 of the qualified Sans, Mono and Serif fonts")
        foreach(INDEX RANGE 0 2)
            list(GET FONT_FILES ${INDEX} FONT_FILE)
            list(GET FONT_HASHES ${INDEX} FONT_HASH)
            file(SHA256 "${RAVEN_FONT_DIRECTORY}/${FONT_FILE}" ACTUAL_FONT_HASH)
            if(NOT ACTUAL_FONT_HASH STREQUAL FONT_HASH)
                message(FATAL_ERROR "Qualified font checksum differs: ${FONT_FILE}")
            endif()
        endforeach()
        find_package(Freetype REQUIRED)
        find_package(Threads REQUIRED)
        # Skia's boolean path operations, which the CPU library is built without: the Direct2D component combines geometries with them.
        file(GLOB RAVEN_SKIA_PATHOPS ${RAVEN_SKIA_SOURCE}/src/pathops/*.cpp)
        add_library(raven_text STATIC graphics/text.cpp graphics/canvas.cpp ${RAVEN_SKIA_PATHOPS})
        set_source_files_properties(${RAVEN_SKIA_PATHOPS} PROPERTIES COMPILE_OPTIONS "-w")
        target_include_directories(raven_text PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
        target_include_directories(raven_text SYSTEM PRIVATE ${RAVEN_SKIA_SOURCE})
        target_compile_definitions(raven_text PRIVATE SK_RELEASE)
        target_compile_options(raven_text PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
        target_link_libraries(raven_text PUBLIC ${RAVEN_SKIA_LIBRARY} Freetype::Freetype Threads::Threads ${CMAKE_DL_LIBS})
        target_sources(raven_cpu PRIVATE desktop/text.cpp)
        target_link_libraries(raven_cpu PUBLIC raven_text)
        target_compile_definitions(raven_cpu PUBLIC RAVEN_SKIA=1 PRIVATE RAVEN_FONT_DIRECTORY="${RAVEN_FONT_DIRECTORY}")
    else()
        # Without Skia the build still succeeds, but GDI text is left out and a desktop program stops at its first text call.
        message(WARNING "Skia and fonts not configured: GDI text is not built, and desktop programs stop at their first text call, such as "
            "CreateFontW or GetTextMetricsW, with \"unimplemented native API\". Run scripts/provision-text.py and configure again with "
            "RAVEN_SKIA_SOURCE, RAVEN_SKIA_LIBRARY and RAVEN_FONT_DIRECTORY, as the README shows.")
    endif()
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        # Commands that no hand-written handler covers are marshaled from the Khronos registry when it is installed.
        find_file(RAVEN_VK_XML vk.xml PATHS /usr/share/vulkan/registry /usr/local/share/vulkan/registry)
        find_file(RAVEN_VULKAN_CORE_HEADER vulkan/vulkan_core.h PATHS ${Vulkan_INCLUDE_DIRS} /usr/include)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        set(VULKAN_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/vulkan-generated)
        if(RAVEN_VK_XML)
            set(VULKAN_CHAIN_REGISTRY ${RAVEN_VK_XML})
        endif()
        add_custom_command(OUTPUT ${VULKAN_GENERATED_DIR}/vulkan_chain_table.inc
            COMMAND ${CMAKE_COMMAND} -E make_directory ${VULKAN_GENERATED_DIR}
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/generate-vulkan-chains.py
                ${RAVEN_VULKAN_CORE_HEADER} ${VULKAN_GENERATED_DIR}/vulkan_chain_table.inc ${VULKAN_CHAIN_REGISTRY}
            DEPENDS ${PROJECT_SOURCE_DIR}/scripts/generate-vulkan-chains.py ${RAVEN_VULKAN_CORE_HEADER} ${VULKAN_CHAIN_REGISTRY} VERBATIM)
        add_custom_target(vulkan_chains_generated DEPENDS ${VULKAN_GENERATED_DIR}/vulkan_chain_table.inc)
        target_include_directories(raven_cpu PRIVATE ${VULKAN_GENERATED_DIR})
        add_dependencies(raven_cpu vulkan_chains_generated)
        if(RAVEN_VK_XML AND RAVEN_VULKAN_CORE_HEADER AND Python3_Interpreter_FOUND)
            add_custom_command(OUTPUT ${VULKAN_GENERATED_DIR}/vulkan_marshal_generated.inc ${VULKAN_GENERATED_DIR}/vulkan_handle_types.inc
                    ${VULKAN_GENERATED_DIR}/vulkan_generated_exports.def ${VULKAN_GENERATED_DIR}/vulkan_all_exports.def
                COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/generate-vulkan-marshal.py ${RAVEN_VK_XML}
                    ${PROJECT_SOURCE_DIR}/testdata/native_cpu/vulkan.def ${VULKAN_GENERATED_DIR} ${RAVEN_VULKAN_CORE_HEADER}
                DEPENDS ${PROJECT_SOURCE_DIR}/scripts/generate-vulkan-marshal.py ${RAVEN_VK_XML} ${PROJECT_SOURCE_DIR}/testdata/native_cpu/vulkan.def VERBATIM)
            add_custom_target(vulkan_marshal_generated DEPENDS ${VULKAN_GENERATED_DIR}/vulkan_marshal_generated.inc)
            set(VULKAN_GENERATED_SOURCES graphics/vulkan_marshal.cpp graphics/vulkan_generated.cpp graphics/vulkan_foreign.cpp)
        endif()
        target_sources(raven_cpu PRIVATE graphics/vulkan_bridge.cpp graphics/vulkan_resources.cpp graphics/shared_gpu_memory.cpp graphics/vulkan_enumerate.cpp graphics/vulkan_queries.cpp)
        # 32-bit programs reach the bridge through tables built with an i686 compiler, which also lays out Vulkan structures for them.
        find_program(RAVEN_MINGW32_VULKAN i686-w64-mingw32-gcc)
        if(VULKAN_GENERATED_SOURCES AND RAVEN_MINGW32_VULKAN)
            get_filename_component(RAVEN_VULKAN_HEADER_DIR ${RAVEN_VULKAN_CORE_HEADER} DIRECTORY)
            get_filename_component(RAVEN_VULKAN_INCLUDE_ROOT ${RAVEN_VULKAN_HEADER_DIR} DIRECTORY)
            file(COPY ${RAVEN_VULKAN_HEADER_DIR} DESTINATION ${VULKAN_GENERATED_DIR}/mingw-include)
            if(EXISTS ${RAVEN_VULKAN_INCLUDE_ROOT}/vk_video)
                file(COPY ${RAVEN_VULKAN_INCLUDE_ROOT}/vk_video DESTINATION ${VULKAN_GENERATED_DIR}/mingw-include)
            endif()
            add_custom_command(OUTPUT ${VULKAN_GENERATED_DIR}/vulkan_thunk32.inc ${VULKAN_GENERATED_DIR}/vulkan_thunk32_exports.def
                COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/generate-vulkan-thunk32.py ${RAVEN_VK_XML} ${RAVEN_VULKAN_CORE_HEADER}
                    ${VULKAN_GENERATED_DIR}/vulkan_thunk32.inc ${RAVEN_MINGW32_VULKAN} ${VULKAN_GENERATED_DIR}/mingw-include
                DEPENDS ${PROJECT_SOURCE_DIR}/scripts/generate-vulkan-thunk32.py ${PROJECT_SOURCE_DIR}/scripts/generate-vulkan-layout32.py
                    ${PROJECT_SOURCE_DIR}/scripts/generate-vulkan-marshal.py ${RAVEN_VK_XML} VERBATIM)
            add_custom_target(vulkan_thunk32_generated DEPENDS ${VULKAN_GENERATED_DIR}/vulkan_thunk32.inc)
            target_sources(raven_cpu PRIVATE graphics/vulkan_thunk32.cpp graphics/vulkan_thunk32_tables.cpp)
            target_include_directories(raven_cpu PRIVATE ${VULKAN_GENERATED_DIR})
            target_compile_definitions(raven_cpu PUBLIC RAVEN_VULKAN_THUNK32=1)
            add_dependencies(raven_cpu vulkan_thunk32_generated)
            add_executable(vulkan_thunk32_test graphics/vulkan_thunk32_test.cpp graphics/vulkan_thunk32_tables.cpp)
            target_include_directories(vulkan_thunk32_test PRIVATE ${VULKAN_GENERATED_DIR})
            target_link_libraries(vulkan_thunk32_test PRIVATE Vulkan::Vulkan)
            add_dependencies(vulkan_thunk32_test vulkan_thunk32_generated)
            add_test(NAME native_vulkan_thunk32_tables COMMAND vulkan_thunk32_test)
            set_tests_properties(native_vulkan_thunk32_tables PROPERTIES TIMEOUT 30)
        endif()
        if(VULKAN_GENERATED_SOURCES)
            target_sources(raven_cpu PRIVATE ${VULKAN_GENERATED_SOURCES})
            target_include_directories(raven_cpu PRIVATE ${VULKAN_GENERATED_DIR})
            target_compile_definitions(raven_cpu PUBLIC RAVEN_VULKAN_GENERATED=1)
            add_dependencies(raven_cpu vulkan_marshal_generated)
        endif()
        target_link_libraries(raven_cpu PUBLIC Vulkan::Vulkan)
        target_compile_definitions(raven_cpu PUBLIC RAVEN_VULKAN=1)
        # Compute shaders from the compiler run on the CPU driver and their results are checked.
        if(RAVEN_LAVAPIPE_ICD)
            add_executable(hlsl_run_test shader/hlsl_run_test.cpp)
            target_link_libraries(hlsl_run_test PRIVATE raven_shader Vulkan::Vulkan)
            add_test(NAME native_hlsl_execution COMMAND hlsl_run_test ${PROJECT_SOURCE_DIR}/testdata/hlsl/run ${CMAKE_CURRENT_BINARY_DIR}/hlsl-run-output)
            set_tests_properties(native_hlsl_execution PROPERTIES TIMEOUT 120
                ENVIRONMENT "VK_DRIVER_FILES=${RAVEN_LAVAPIPE_ICD};LP_NUM_THREADS=2")
        endif()
    endif()
    target_compile_options(raven_cpu PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    target_link_libraries(raven PRIVATE raven_cpu)
    find_package(PkgConfig QUIET)
    target_sources(raven_cpu PRIVATE desktop/keyboard.cpp)
    target_sources(raven_cpu PRIVATE desktop/window_data.cpp)
    target_sources(raven_cpu PRIVATE desktop/clip.cpp)
    target_sources(raven_cpu PRIVATE desktop/pen.cpp desktop/caps.cpp desktop/load.cpp desktop/accelerator.cpp)
    target_sources(raven_cpu PRIVATE desktop/frame.cpp desktop/menu_loop.cpp)
    target_sources(raven_cpu PRIVATE desktop/class_info.cpp desktop/colors.cpp desktop/hierarchy.cpp desktop/menu.cpp desktop/menu_api.cpp desktop/extract.cpp graphics/icon_file.cpp nt/dependencies.cpp)
    target_sources(raven_cpu PRIVATE desktop/region.cpp)
    if(PkgConfig_FOUND)
        option(RAVEN_DESKTOP_WAYLAND "Build the Wayland desktop when its libraries are found" ON)
        option(RAVEN_DESKTOP_X11 "Build the experimental X11 desktop when its libraries are found" ON)
        pkg_check_modules(HOST_OPENGL QUIET IMPORTED_TARGET egl gl glu)
        if(HOST_OPENGL_FOUND)
            target_sources(raven_cpu PRIVATE graphics/opengl_bridge.cpp)
            target_link_libraries(raven_cpu PUBLIC PkgConfig::HOST_OPENGL)
            target_compile_definitions(raven_cpu PUBLIC RAVEN_OPENGL=1)
            if(RAVEN_DESKTOP_WAYLAND)
                pkg_check_modules(HOST_WAYLAND_EGL QUIET IMPORTED_TARGET wayland-egl)
                if(HOST_WAYLAND_EGL_FOUND)
                    target_link_libraries(raven_cpu PUBLIC PkgConfig::HOST_WAYLAND_EGL)
                    target_compile_definitions(raven_cpu PUBLIC RAVEN_OPENGL_WAYLAND=1)
                endif()
            endif()
        endif()
        pkg_check_modules(XKBCOMMON QUIET IMPORTED_TARGET xkbcommon)
        pkg_check_modules(PIXMAN QUIET IMPORTED_TARGET pixman-1)
        if(PIXMAN_FOUND)
            target_link_libraries(raven_cpu PUBLIC PkgConfig::PIXMAN)
            target_compile_definitions(raven_cpu PRIVATE RAVEN_PIXMAN=1)
            add_executable(region_test desktop/region_test.cpp)
            target_link_libraries(region_test PRIVATE raven_cpu)
            add_test(NAME native_update_regions COMMAND region_test)
            set_tests_properties(native_update_regions PROPERTIES TIMEOUT 15)
        else()
            # Without pixman the build still succeeds, but every program that clips or paints a window region stops at run time.
            message(WARNING "pixman-1 not found: window regions are not built, and desktop programs that use them stop with "
                "\"native region support not built\". Install the pixman development package and configure again.")
        endif()
        if(XKBCOMMON_FOUND)
            target_link_libraries(raven_cpu PUBLIC PkgConfig::XKBCOMMON)
            target_compile_definitions(raven_cpu PRIVATE RAVEN_XKB=1)
            add_executable(keyboard_map_test desktop/keyboard_test.cpp)
            target_link_libraries(keyboard_map_test PRIVATE raven_cpu)
            add_test(NAME native_keyboard_layouts COMMAND keyboard_map_test)
            set_tests_properties(native_keyboard_layouts PROPERTIES TIMEOUT 15)
        else()
            message(WARNING "xkbcommon not found: keyboard layouts are not built, and desktop programs stop with "
                "\"native keyboard keymap support not built\". Install the xkbcommon development package and configure again.")
        endif()
        if(RAVEN_DESKTOP_X11 AND XKBCOMMON_FOUND)
            pkg_check_modules(XCB_CLIENT QUIET IMPORTED_TARGET xcb xcb-xkb xcb-xfixes xcb-xinput xkbcommon-x11)
            if(XCB_CLIENT_FOUND)
                # The graphics bridges create their surfaces on the same connection, so the definition reaches them too.
                target_sources(raven_cpu PRIVATE desktop/xcb.cpp)
                target_link_libraries(raven_cpu PUBLIC PkgConfig::XCB_CLIENT)
                target_compile_definitions(raven_cpu PUBLIC RAVEN_XCB=1)
            endif()
        endif()
        if(RAVEN_DESKTOP_WAYLAND)
            pkg_check_modules(WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)
            pkg_get_variable(WAYLAND_PROTOCOLS_DATA wayland-protocols pkgdatadir)
            find_program(WAYLAND_SCANNER wayland-scanner)
        endif()
        if(RAVEN_DESKTOP_WAYLAND AND WAYLAND_CLIENT_FOUND AND WAYLAND_SCANNER AND EXISTS "${WAYLAND_PROTOCOLS_DATA}/stable/xdg-shell/xdg-shell.xml")
            enable_language(C)
            set(XDG_PROTOCOL ${WAYLAND_PROTOCOLS_DATA}/stable/xdg-shell/xdg-shell.xml)
            add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h
                COMMAND ${WAYLAND_SCANNER} client-header ${XDG_PROTOCOL} ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h
                DEPENDS ${XDG_PROTOCOL} VERBATIM)
            add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c
                COMMAND ${WAYLAND_SCANNER} private-code ${XDG_PROTOCOL} ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c
                DEPENDS ${XDG_PROTOCOL} VERBATIM)
            # Relative pointer motion, confinement and logical output geometry for the Windows desktop.
            set(RAVEN_WAYLAND_PROTOCOL_SOURCES)
            foreach(protocol relative-pointer pointer-constraints xdg-output)
                set(PROTOCOL_XML ${WAYLAND_PROTOCOLS_DATA}/unstable/${protocol}/${protocol}-unstable-v1.xml)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${protocol}-unstable-v1-client-protocol.h
                    COMMAND ${WAYLAND_SCANNER} client-header ${PROTOCOL_XML} ${CMAKE_CURRENT_BINARY_DIR}/${protocol}-unstable-v1-client-protocol.h
                    DEPENDS ${PROTOCOL_XML} VERBATIM)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${protocol}-unstable-v1-protocol.c
                    COMMAND ${WAYLAND_SCANNER} private-code ${PROTOCOL_XML} ${CMAKE_CURRENT_BINARY_DIR}/${protocol}-unstable-v1-protocol.c
                    DEPENDS ${PROTOCOL_XML} VERBATIM)
                list(APPEND RAVEN_WAYLAND_PROTOCOL_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/${protocol}-unstable-v1-client-protocol.h
                    ${CMAKE_CURRENT_BINARY_DIR}/${protocol}-unstable-v1-protocol.c)
            endforeach()
            # The size the surfaces graphics drivers render to are shown at: their window's, whatever size the driver's buffers are.
            set(RAVEN_WAYLAND_VIEWPORTER OFF)
            if(EXISTS "${WAYLAND_PROTOCOLS_DATA}/stable/viewporter/viewporter.xml")
                set(RAVEN_WAYLAND_VIEWPORTER ON)
                set(PROTOCOL_XML ${WAYLAND_PROTOCOLS_DATA}/stable/viewporter/viewporter.xml)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/viewporter-client-protocol.h
                    COMMAND ${WAYLAND_SCANNER} client-header ${PROTOCOL_XML} ${CMAKE_CURRENT_BINARY_DIR}/viewporter-client-protocol.h
                    DEPENDS ${PROTOCOL_XML} VERBATIM)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/viewporter-protocol.c
                    COMMAND ${WAYLAND_SCANNER} private-code ${PROTOCOL_XML} ${CMAKE_CURRENT_BINARY_DIR}/viewporter-protocol.c
                    DEPENDS ${PROTOCOL_XML} VERBATIM)
                list(APPEND RAVEN_WAYLAND_PROTOCOL_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/viewporter-client-protocol.h
                    ${CMAKE_CURRENT_BINARY_DIR}/viewporter-protocol.c)
            endif()
            # Pointer shapes by name; the protocol refers to tablet tools, so their interfaces are generated with it.
            set(RAVEN_WAYLAND_CURSOR_SHAPE OFF)
            if(EXISTS "${WAYLAND_PROTOCOLS_DATA}/staging/cursor-shape/cursor-shape-v1.xml" AND EXISTS "${WAYLAND_PROTOCOLS_DATA}/stable/tablet/tablet-v2.xml")
                set(RAVEN_WAYLAND_CURSOR_SHAPE ON)
                foreach(protocol staging/cursor-shape/cursor-shape-v1 stable/tablet/tablet-v2)
                    get_filename_component(protocol_name ${protocol} NAME)
                    set(PROTOCOL_XML ${WAYLAND_PROTOCOLS_DATA}/${protocol}.xml)
                    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${protocol_name}-client-protocol.h
                        COMMAND ${WAYLAND_SCANNER} client-header ${PROTOCOL_XML} ${CMAKE_CURRENT_BINARY_DIR}/${protocol_name}-client-protocol.h
                        DEPENDS ${PROTOCOL_XML} VERBATIM)
                    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${protocol_name}-protocol.c
                        COMMAND ${WAYLAND_SCANNER} private-code ${PROTOCOL_XML} ${CMAKE_CURRENT_BINARY_DIR}/${protocol_name}-protocol.c
                        DEPENDS ${PROTOCOL_XML} VERBATIM)
                    list(APPEND RAVEN_WAYLAND_PROTOCOL_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/${protocol_name}-client-protocol.h
                        ${CMAKE_CURRENT_BINARY_DIR}/${protocol_name}-protocol.c)
                endforeach()
            endif()
            add_library(raven_wayland STATIC desktop/wayland.cpp
                ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c
                ${RAVEN_WAYLAND_PROTOCOL_SOURCES})
            if(RAVEN_WAYLAND_CURSOR_SHAPE)
                target_compile_definitions(raven_wayland PRIVATE RAVEN_WAYLAND_CURSOR_SHAPE=1)
            endif()
            if(RAVEN_WAYLAND_VIEWPORTER)
                target_compile_definitions(raven_wayland PRIVATE RAVEN_WAYLAND_VIEWPORTER=1)
            endif()
            target_include_directories(raven_wayland PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
            target_link_libraries(raven_wayland PUBLIC PkgConfig::WAYLAND_CLIENT)
            target_compile_options(raven_wayland PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
            target_link_libraries(raven PRIVATE raven_wayland)
            target_compile_definitions(raven PRIVATE RAVEN_WAYLAND=1)
            # The presentation against a compositor inside the test, which hands the keyboard to popups the way GNOME's does.
            pkg_check_modules(WAYLAND_SERVER QUIET IMPORTED_TARGET wayland-server)
            if(WAYLAND_SERVER_FOUND)
                find_package(Threads REQUIRED)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-server-protocol.h
                    COMMAND ${WAYLAND_SCANNER} server-header ${XDG_PROTOCOL} ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-server-protocol.h
                    DEPENDS ${XDG_PROTOCOL} VERBATIM)
                add_executable(wayland_presentation_test desktop/wayland_test.cpp ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-server-protocol.h)
                target_include_directories(wayland_presentation_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
                target_link_libraries(wayland_presentation_test PRIVATE raven_wayland PkgConfig::WAYLAND_SERVER Threads::Threads)
                add_test(NAME native_wayland_popup_focus COMMAND wayland_presentation_test)
                set_tests_properties(native_wayland_popup_focus PROPERTIES TIMEOUT 15)
            endif()
        endif()
    endif()
    target_compile_definitions(raven PRIVATE RAVEN_NATIVE_CPU=1)
    add_dependencies(raven raven-bootstrap)
    add_executable(native_cpu_test cpu/native_cpu_test.cpp)
    target_link_libraries(native_cpu_test PRIVATE raven_cpu)
    add_dependencies(native_cpu_test raven-bootstrap)
    add_test(NAME native_cpu_contracts COMMAND native_cpu_test $<TARGET_FILE:raven-bootstrap>)
    add_test(NAME native_memory_contracts COMMAND native_cpu_test $<TARGET_FILE:raven-bootstrap> --memory)
    add_test(NAME native_parallel_memory_contracts COMMAND native_cpu_test $<TARGET_FILE:raven-bootstrap> --memory)
    set_tests_properties(native_memory_contracts native_parallel_memory_contracts PROPERTIES TIMEOUT 15)
    set_tests_properties(native_parallel_memory_contracts PROPERTIES ENVIRONMENT RAVEN_PARALLEL=1)
    add_test(NAME native_parallel_stop_contract COMMAND native_cpu_test $<TARGET_FILE:raven-bootstrap> --parallel-stops)
    set_tests_properties(native_parallel_stop_contract PROPERTIES TIMEOUT 15 ENVIRONMENT RAVEN_PARALLEL=1)
    add_executable(native_x86_test cpu/native_x86_test.cpp)
    target_link_libraries(native_x86_test PRIVATE raven_cpu)
    add_dependencies(native_x86_test raven-bootstrap)
    add_test(NAME native_x86_contracts COMMAND native_x86_test $<TARGET_FILE:raven-bootstrap>)
    set_tests_properties(native_x86_contracts PROPERTIES TIMEOUT 30)
    set_tests_properties(native_cpu_contracts PROPERTIES TIMEOUT 15)
    add_executable(shared_mapping_test cpu/shared_mapping_test.cpp)
    target_link_libraries(shared_mapping_test PRIVATE raven_cpu)
    add_dependencies(shared_mapping_test raven-bootstrap)
    add_test(NAME native_shared_mapping_contract COMMAND shared_mapping_test $<TARGET_FILE:raven-bootstrap>)
    set_tests_properties(native_shared_mapping_contract PROPERTIES TIMEOUT 15)
    if(Vulkan_FOUND)
        add_executable(shared_gpu_memory_test graphics/shared_gpu_memory_test.cpp)
        target_link_libraries(shared_gpu_memory_test PRIVATE raven_cpu)
        add_dependencies(shared_gpu_memory_test raven-bootstrap)
        add_test(NAME native_shared_gpu_memory_contract COMMAND shared_gpu_memory_test $<TARGET_FILE:raven-bootstrap>)
        set_tests_properties(native_shared_gpu_memory_contract PROPERTIES TIMEOUT 20 FAIL_REGULAR_EXPRESSION "Validation Error:|VUID-")
        if(RAVEN_LAVAPIPE_ICD)
            add_test(NAME native_shared_gpu_memory_software COMMAND shared_gpu_memory_test $<TARGET_FILE:raven-bootstrap>)
            set_tests_properties(native_shared_gpu_memory_software PROPERTIES TIMEOUT 20 FAIL_REGULAR_EXPRESSION "Validation Error:|VUID-"
                ENVIRONMENT "VK_DRIVER_FILES=${RAVEN_LAVAPIPE_ICD};LP_NUM_THREADS=2;RAVEN_REQUIRE_GPU_MAPPING=1")
        endif()
    endif()
    add_executable(providers_test nt/providers_test.cpp)
    target_link_libraries(providers_test PRIVATE raven_cpu)
    add_dependencies(providers_test raven-bootstrap)
    add_test(NAME native_provider_contract COMMAND providers_test $<TARGET_FILE:raven-bootstrap>)
    set_tests_properties(native_provider_contract PROPERTIES TIMEOUT 15)
    add_test(NAME native_parallel_served_contract COMMAND providers_test $<TARGET_FILE:raven-bootstrap> --parallel-served)
    set_tests_properties(native_parallel_served_contract PROPERTIES TIMEOUT 15 ENVIRONMENT RAVEN_PARALLEL=1)
    add_executable(font_test desktop/font_test.cpp)
    target_link_libraries(font_test PRIVATE raven_cpu)
    add_test(NAME native_font_ownership COMMAND font_test)
    set_tests_properties(native_font_ownership PROPERTIES TIMEOUT 15)
    add_executable(compatible_bitmap_test desktop/compatible_bitmap_test.cpp)
    target_link_libraries(compatible_bitmap_test PRIVATE raven_cpu)
    add_dependencies(compatible_bitmap_test raven-bootstrap)
    add_test(NAME native_compatible_bitmap_contract COMMAND compatible_bitmap_test $<TARGET_FILE:raven-bootstrap>)
    set_tests_properties(native_compatible_bitmap_contract PROPERTIES TIMEOUT 15)
    add_executable(bitblt_test desktop/bitblt_test.cpp)
    target_link_libraries(bitblt_test PRIVATE raven_cpu)
    add_dependencies(bitblt_test raven-bootstrap)
    add_test(NAME native_bitblt_contract COMMAND bitblt_test $<TARGET_FILE:raven-bootstrap>)
    set_tests_properties(native_bitblt_contract PROPERTIES TIMEOUT 15)
    add_executable(brush_test desktop/brush_test.cpp)
    add_executable(window_data_test desktop/window_data_test.cpp)
    add_executable(class_info_test desktop/class_info_test.cpp)
    target_link_libraries(class_info_test PRIVATE raven_cpu)
    add_test(NAME native_class_info_ownership COMMAND class_info_test)
    set_tests_properties(native_class_info_ownership PROPERTIES TIMEOUT 15)
    target_link_libraries(window_data_test PRIVATE raven_cpu)
    add_test(NAME native_window_data_ownership COMMAND window_data_test)
    set_tests_properties(native_window_data_ownership PROPERTIES TIMEOUT 15)
    target_link_libraries(brush_test PRIVATE raven_cpu)
    add_test(NAME native_brush_ownership COMMAND brush_test)
    set_tests_properties(native_brush_ownership PROPERTIES TIMEOUT 15)
    add_executable(exceptions_test nt/exceptions_test.cpp)
    target_link_libraries(exceptions_test PRIVATE raven_cpu)
    add_dependencies(exceptions_test raven-bootstrap)
    add_test(NAME native_exception_contract COMMAND exceptions_test $<TARGET_FILE:raven-bootstrap>)
    set_tests_properties(native_exception_contract PROPERTIES TIMEOUT 15)
    add_executable(memory_test nt/memory_test.cpp)
    add_executable(firmware_test nt/firmware_test.cpp)
    target_link_libraries(firmware_test PRIVATE raven_cpu)
    add_test(NAME native_firmware_memory COMMAND firmware_test)
    add_executable(interfaces_test nt/interfaces_test.cpp)
    target_link_libraries(interfaces_test PRIVATE raven_cpu)
    add_test(NAME native_interface_table_contract COMMAND interfaces_test)
    set_tests_properties(native_interface_table_contract PROPERTIES TIMEOUT 15)
    target_link_libraries(memory_test PRIVATE raven_cpu)
    add_dependencies(memory_test raven-bootstrap)
    add_test(NAME native_memory_contract COMMAND memory_test $<TARGET_FILE:raven-bootstrap>)
    set_tests_properties(native_memory_contract PROPERTIES TIMEOUT 15)
    add_executable(objects_test nt/objects_test.cpp)
    target_link_libraries(objects_test PRIVATE raven_cpu)
    add_dependencies(objects_test raven-bootstrap)
    add_test(NAME native_object_contract COMMAND objects_test $<TARGET_FILE:raven-bootstrap>)
    set_tests_properties(native_object_contract PROPERTIES TIMEOUT 15)
    add_executable(handle_channel_test nt/handle_channel_test.cpp)
    target_link_libraries(handle_channel_test PRIVATE raven_cpu)
    add_test(NAME native_handle_channel_contract COMMAND handle_channel_test)
    set_tests_properties(native_handle_channel_contract PROPERTIES TIMEOUT 10)

    find_program(RAVEN_MINGW64 x86_64-w64-mingw32-gcc)
    find_program(RAVEN_WINDRES x86_64-w64-mingw32-windres)
    if(RAVEN_MINGW64)
        # These PE fixtures have no C runtime to supply compiler-generated memory operations.
        list(APPEND RAVEN_MINGW64 -minline-all-stringops)
        set(MULTIBYTE_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/multibyte-client.exe)
        set(MULTIBYTE_BOUNDARY ${CMAKE_CURRENT_BINARY_DIR}/multibyte-boundary.exe)
        add_custom_command(OUTPUT ${MULTIBYTE_CLIENT} ${MULTIBYTE_BOUNDARY}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -fno-builtin
                -Wl,--entry,raven_entry,--image-base,0x1c6000000,--no-insert-timestamp
                -o ${MULTIBYTE_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/multibyte_client.c -lkernel32
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -fno-builtin -DCONVERSION_BOUNDARY
                -Wl,--entry,raven_entry,--image-base,0x1c6000000,--no-insert-timestamp
                -o ${MULTIBYTE_BOUNDARY} ${PROJECT_SOURCE_DIR}/testdata/native_loader/multibyte_client.c -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/multibyte_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/multibyte_reference.h VERBATIM)
        add_custom_target(multibyte_pe_fixtures ALL DEPENDS ${MULTIBYTE_CLIENT} ${MULTIBYTE_BOUNDARY})
        add_executable(multibyte_test nt/multibyte_test.cpp)
        target_link_libraries(multibyte_test PRIVATE raven_cpu)
        add_dependencies(multibyte_test multibyte_pe_fixtures raven-bootstrap)
        add_test(NAME native_multibyte_contract COMMAND multibyte_test $<TARGET_FILE:raven-bootstrap> ${MULTIBYTE_CLIENT})
        add_test(NAME native_multibyte_cli COMMAND raven --run-process --execution=native ${MULTIBYTE_CLIENT})
        add_test(NAME native_multibyte_boundary COMMAND raven --run-process --execution=native ${MULTIBYTE_BOUNDARY})
        set_tests_properties(native_multibyte_contract PROPERTIES TIMEOUT 30)
        set_tests_properties(native_multibyte_cli native_multibyte_boundary PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(INTERFACE_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/interface-client.exe)
        add_custom_command(OUTPUT ${INTERFACE_CLIENT}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -fno-builtin
                -Wl,--entry,raven_entry,--image-base,0x1d7000000,--no-insert-timestamp
                -o ${INTERFACE_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/interface_client.c -liphlpapi -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/interface_client.c VERBATIM)
        add_custom_target(interface_pe_fixture ALL DEPENDS ${INTERFACE_CLIENT})
        add_test(NAME native_interface_table_client COMMAND raven --run-process --execution=native --timeout-ms=30000 ${INTERFACE_CLIENT})
        set_tests_properties(native_interface_table_client PROPERTIES TIMEOUT 40 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        # The records carry no pointer, so a 32-bit program reads the same rows; the fixture is built twice to check that it can.
        find_program(RAVEN_MINGW32_INTERFACES i686-w64-mingw32-gcc)
        if(RAVEN_MINGW32_INTERFACES)
            set(INTERFACE_CLIENT32 ${CMAKE_CURRENT_BINARY_DIR}/interface-client32.exe)
            add_custom_command(OUTPUT ${INTERFACE_CLIENT32}
                COMMAND ${RAVEN_MINGW32_INTERFACES} -O2 -nostdlib -fno-stack-protector -fno-builtin
                    -Wl,--entry,_raven_entry,--image-base,0x1d7000000,--no-insert-timestamp
                    -o ${INTERFACE_CLIENT32} ${PROJECT_SOURCE_DIR}/testdata/native_loader/interface_client.c -liphlpapi -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/interface_client.c VERBATIM)
            add_custom_target(interface_pe_fixture32 ALL DEPENDS ${INTERFACE_CLIENT32})
            add_test(NAME native_interface_table_client_x32 COMMAND raven --run-process --execution=native --timeout-ms=30000 ${INTERFACE_CLIENT32})
            set_tests_properties(native_interface_table_client_x32 PROPERTIES TIMEOUT 40 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        endif()
        set(PROTOCOL_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/protocol-client.exe)
        find_program(RAVEN_MINGW32_PROTOCOLS i686-w64-mingw32-gcc)
        add_custom_command(OUTPUT ${PROTOCOL_CLIENT}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -fno-builtin
                -Wl,--entry,raven_entry,--image-base,0x1d8000000,--no-insert-timestamp
                -o ${PROTOCOL_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/protocol_client.c -lws2_32 -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/protocol_client.c VERBATIM)
        add_custom_target(protocol_pe_fixture ALL DEPENDS ${PROTOCOL_CLIENT})
        add_test(NAME native_protocol_database_client COMMAND raven --run-process --execution=native --timeout-ms=30000 ${PROTOCOL_CLIENT})
        set_tests_properties(native_protocol_database_client PROPERTIES TIMEOUT 40 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        # The PROTOENT record keeps its pointers, so a 32-bit program reads it in the 32-bit layout; the fixture is built twice to check it.
        if(RAVEN_MINGW32_PROTOCOLS)
            set(PROTOCOL_CLIENT32 ${CMAKE_CURRENT_BINARY_DIR}/protocol-client32.exe)
            add_custom_command(OUTPUT ${PROTOCOL_CLIENT32}
                COMMAND ${RAVEN_MINGW32_PROTOCOLS} -O2 -nostdlib -fno-stack-protector -fno-builtin
                    -Wl,--entry,_raven_entry,--image-base,0x1d8000000,--no-insert-timestamp
                    -o ${PROTOCOL_CLIENT32} ${PROJECT_SOURCE_DIR}/testdata/native_loader/protocol_client.c -lws2_32 -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/protocol_client.c VERBATIM)
            add_custom_target(protocol_pe_fixture32 ALL DEPENDS ${PROTOCOL_CLIENT32})
            add_test(NAME native_protocol_database_client_x32 COMMAND raven --run-process --execution=native --timeout-ms=30000 ${PROTOCOL_CLIENT32})
            set_tests_properties(native_protocol_database_client_x32 PROPERTIES TIMEOUT 40 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        endif()
    endif()
    if(RAVEN_MINGW64 AND PIXMAN_FOUND)
        set(PEN_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/pen-client.exe)
        add_custom_command(OUTPUT ${PEN_CLIENT}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -fno-builtin
                -Wl,--entry,raven_entry,--image-base,0x1d5000000,--no-insert-timestamp
                -o ${PEN_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/pen_client.c -luser32 -lgdi32 -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/pen_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/pen_reference.h VERBATIM)
        add_custom_target(pen_pe_fixture ALL DEPENDS ${PEN_CLIENT})
        add_executable(drawing_test desktop/drawing_test.cpp)
        target_link_libraries(drawing_test PRIVATE raven_cpu)
        add_dependencies(drawing_test pen_pe_fixture raven-bootstrap)
        add_test(NAME native_pen_drawing COMMAND drawing_test $<TARGET_FILE:raven-bootstrap> ${PEN_CLIENT} --pen)
        add_test(NAME native_pen_drawing_cli COMMAND raven --run-process --execution=native --timeout-ms=30000 ${PEN_CLIENT})
        set_tests_properties(native_pen_drawing native_pen_drawing_cli PROPERTIES TIMEOUT 40)
        set_tests_properties(native_pen_drawing_cli PROPERTIES PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        if(TARGET raven_text)
            set(TEXT_OUTPUT_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/text-output-client.exe)
            add_custom_command(OUTPUT ${TEXT_OUTPUT_CLIENT}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -fno-builtin
                    -Wl,--entry,raven_entry,--image-base,0x1d6000000,--no-insert-timestamp
                    -o ${TEXT_OUTPUT_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/text_output_client.c -luser32 -lgdi32 -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/text_output_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/text_output_reference.h VERBATIM)
            add_custom_target(text_output_pe_fixture ALL DEPENDS ${TEXT_OUTPUT_CLIENT})
            add_dependencies(drawing_test text_output_pe_fixture)
            add_test(NAME native_text_output COMMAND drawing_test $<TARGET_FILE:raven-bootstrap> ${TEXT_OUTPUT_CLIENT} --text)
            add_test(NAME native_text_output_cli COMMAND raven --run-process --execution=native --timeout-ms=30000 ${TEXT_OUTPUT_CLIENT})
            set_tests_properties(native_text_output native_text_output_cli PROPERTIES TIMEOUT 40)
            set_tests_properties(native_text_output_cli PROPERTIES PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        endif()
        set(CLIP_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/clip-client.exe)
        set(CLIP_PAINT_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/clip-paint-client.exe)
        add_custom_command(OUTPUT ${CLIP_PAINT_CLIENT}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -fno-builtin
                -Wl,--entry,raven_entry,--image-base,0x1d1000000,--no-insert-timestamp
                -o ${CLIP_PAINT_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/clip_paint_client.c -luser32 -lgdi32 -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/clip_paint_client.c VERBATIM)
        add_custom_command(OUTPUT ${CLIP_CLIENT}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -fno-builtin
                -Wl,--entry,raven_entry,--image-base,0x1d0000000,--no-insert-timestamp
                -o ${CLIP_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/clip_client.c -luser32 -lgdi32 -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/clip_client.c VERBATIM)
        # A window that resizes itself while it paints ends that paint and paints again.
        set(PAINT_RESIZE_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/paint-resize-client.exe)
        add_custom_command(OUTPUT ${PAINT_RESIZE_CLIENT}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -fno-builtin
                -Wl,--entry,raven_entry,--image-base,0x1d0000000,--no-insert-timestamp
                -o ${PAINT_RESIZE_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/paint_resize_client.c -luser32 -lgdi32 -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/paint_resize_client.c VERBATIM)
        add_custom_target(clip_pe_fixtures ALL DEPENDS ${CLIP_CLIENT} ${CLIP_PAINT_CLIENT} ${PAINT_RESIZE_CLIENT})
        add_test(NAME native_paint_resize COMMAND raven --run-process --execution=native --timeout-ms=30000 ${PAINT_RESIZE_CLIENT})
        set_tests_properties(native_paint_resize PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        add_executable(clip_test desktop/clip_test.cpp)
        target_link_libraries(clip_test PRIVATE raven_cpu)
        add_dependencies(clip_test clip_pe_fixtures raven-bootstrap)
        add_test(NAME native_clipping COMMAND clip_test $<TARGET_FILE:raven-bootstrap> ${CLIP_CLIENT})
        add_test(NAME native_clipping_cli COMMAND raven --run-process --execution=native ${CLIP_CLIENT})
        add_test(NAME native_control_clip_paint COMMAND clip_test $<TARGET_FILE:raven-bootstrap> ${CLIP_PAINT_CLIENT} --paint)
        set_tests_properties(native_clipping native_clipping_cli native_control_clip_paint PROPERTIES TIMEOUT 30)
        set_tests_properties(native_clipping_cli PROPERTIES PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
    endif()
    if(RAVEN_MINGW64)
        set(WINDOW_TEXT_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/window-text-client.exe)
        add_custom_command(OUTPUT ${WINDOW_TEXT_CLIENT}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -fno-builtin
                -Wl,--entry,raven_entry,--image-base,0x1cf000000,--no-insert-timestamp
                -o ${WINDOW_TEXT_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/window_text_client.c -luser32 -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/window_text_client.c VERBATIM)
        add_custom_target(window_text_pe_fixtures ALL DEPENDS ${WINDOW_TEXT_CLIENT})
        add_executable(window_text_test desktop/window_text_test.cpp)
        target_link_libraries(window_text_test PRIVATE raven_cpu)
        add_dependencies(window_text_test window_text_pe_fixtures raven-bootstrap)
        add_test(NAME native_window_text COMMAND window_text_test $<TARGET_FILE:raven-bootstrap> ${WINDOW_TEXT_CLIENT})
        add_test(NAME native_window_text_cli COMMAND raven --run-process --execution=native ${WINDOW_TEXT_CLIENT})
        set_tests_properties(native_window_text native_window_text_cli PROPERTIES TIMEOUT 30)
        set_tests_properties(native_window_text_cli PROPERTIES PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(COLOR_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/color-client.exe)
        add_custom_command(OUTPUT ${COLOR_CLIENT}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -fno-builtin
                -Wl,--entry,raven_entry,--image-base,0x1cc000000,--no-insert-timestamp
                -o ${COLOR_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/color_client.c -luser32 -lgdi32 -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/color_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/color_reference.h VERBATIM)
        add_custom_target(color_pe_fixtures ALL DEPENDS ${COLOR_CLIENT})
        add_executable(colors_test desktop/colors_test.cpp)
        target_link_libraries(colors_test PRIVATE raven_cpu)
        add_dependencies(colors_test color_pe_fixtures raven-bootstrap)
        add_test(NAME native_system_colors COMMAND colors_test $<TARGET_FILE:raven-bootstrap> ${COLOR_CLIENT})
        add_test(NAME native_system_colors_cli COMMAND raven --run-process --execution=native ${COLOR_CLIENT})
        set_tests_properties(native_system_colors native_system_colors_cli PROPERTIES TIMEOUT 30)
        set_tests_properties(native_system_colors_cli PROPERTIES PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
    endif()
    if(RAVEN_MINGW64 AND TARGET raven_text)
        set(CHILD_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/child-client.exe)
        set(CHILD_INPUT ${CMAKE_CURRENT_BINARY_DIR}/child-input.exe)
        add_custom_command(OUTPUT ${CHILD_CLIENT} ${CHILD_INPUT}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -fno-builtin
                -Wl,--entry,raven_entry,--image-base,0x1ca000000,--no-insert-timestamp
                -o ${CHILD_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/child_client.c -luser32 -lgdi32 -lkernel32
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -fno-builtin -DCHILD_INPUT
                -Wl,--entry,raven_entry,--image-base,0x1ca000000,--no-insert-timestamp
                -o ${CHILD_INPUT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/child_client.c -luser32 -lgdi32 -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/child_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/child_reference.h VERBATIM)
        set(CHILD_REJECT ${CMAKE_CURRENT_BINARY_DIR}/child-reject.exe)
        add_custom_command(OUTPUT ${CHILD_REJECT}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -fno-builtin
                -Wl,--entry,raven_entry,--image-base,0x1cb000000,--no-insert-timestamp
                -o ${CHILD_REJECT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/child_reject.c -luser32 -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/child_reject.c VERBATIM)
        add_custom_target(child_pe_fixtures ALL DEPENDS ${CHILD_CLIENT} ${CHILD_INPUT} ${CHILD_REJECT})
        add_executable(child_test desktop/child_test.cpp)
        target_link_libraries(child_test PRIVATE raven_cpu)
        add_dependencies(child_test child_pe_fixtures raven-bootstrap)
        add_test(NAME native_child_windows COMMAND child_test $<TARGET_FILE:raven-bootstrap> ${CHILD_CLIENT})
        add_test(NAME native_child_input COMMAND child_test $<TARGET_FILE:raven-bootstrap> ${CHILD_INPUT} --input)
        add_test(NAME native_child_rejection COMMAND child_test $<TARGET_FILE:raven-bootstrap> ${CHILD_REJECT} --reject)
        set_tests_properties(native_child_windows native_child_input native_child_rejection PROPERTIES TIMEOUT 30)
    endif()
    if(RAVEN_MINGW64 AND RAVEN_WINDRES AND PNG_FOUND)
        set(EXTRACT_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/extract-fixtures)
        file(MAKE_DIRECTORY ${EXTRACT_FIXTURES})
        file(WRITE ${EXTRACT_FIXTURES}/empty.bin "")
        file(WRITE ${EXTRACT_FIXTURES}/not-icon.txt "Not an icon file.\n")
        add_custom_command(OUTPUT ${EXTRACT_FIXTURES}/client.exe ${EXTRACT_FIXTURES}/icons.ico
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -fno-builtin
                -Wl,--entry,raven_entry,--image-base,0x1c7000000,--no-insert-timestamp
                -o ${EXTRACT_FIXTURES}/client.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/extract_client.c -luser32 -lgdi32 -lkernel32
            COMMAND ${CMAKE_COMMAND} -E copy ${RAVEN_TESTDATA_DIR}/native_loader/icon_resources.ico ${EXTRACT_FIXTURES}/icons.ico
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/extract_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/extract_reference.h ${RAVEN_TESTDATA_DIR}/native_loader/icon_resources.ico VERBATIM)
        add_custom_target(extract_pe_fixtures ALL DEPENDS ${EXTRACT_FIXTURES}/client.exe ${EXTRACT_FIXTURES}/icons.ico)
        add_executable(extract_test desktop/extract_test.cpp)
        target_link_libraries(extract_test PRIVATE raven_cpu)
        add_dependencies(extract_test extract_pe_fixtures desktop_pe_fixtures raven-bootstrap)
        add_test(NAME native_extract_icons_contract COMMAND extract_test $<TARGET_FILE:raven-bootstrap>
            ${EXTRACT_FIXTURES}/client.exe ${CMAKE_CURRENT_BINARY_DIR}/desktop-fixtures/icons.dll ${EXTRACT_FIXTURES}/icons.ico)
        set_tests_properties(native_extract_icons_contract PROPERTIES TIMEOUT 30)
    endif()
    if(RAVEN_MINGW64 AND RAVEN_WINDRES)
        set(MENU_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/menu-fixtures)
        add_custom_command(OUTPUT ${MENU_FIXTURES}/client.exe ${MENU_FIXTURES}/boundary.exe ${MENU_FIXTURES}/menus.dll
            COMMAND ${CMAKE_COMMAND} -E make_directory ${MENU_FIXTURES}
            COMMAND ${RAVEN_WINDRES} ${PROJECT_SOURCE_DIR}/testdata/native_loader/menu_resources.rc ${MENU_FIXTURES}/resources.o
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -shared -fno-stack-protector
                -Wl,--entry,0,--image-base,0x1c2000000,--no-insert-timestamp
                -o ${MENU_FIXTURES}/menus.dll ${MENU_FIXTURES}/resources.o
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x1c3000000,--no-insert-timestamp
                -o ${MENU_FIXTURES}/client.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/menu_client.c -lkernel32 -luser32
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -DMENU_BOUNDARY
                -Wl,--entry,raven_entry,--image-base,0x1c3000000,--no-insert-timestamp
                -o ${MENU_FIXTURES}/boundary.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/menu_client.c -lkernel32 -luser32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/menu_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/menu_resources.rc VERBATIM)
        add_custom_target(menu_pe_fixtures ALL DEPENDS ${MENU_FIXTURES}/client.exe ${MENU_FIXTURES}/boundary.exe ${MENU_FIXTURES}/menus.dll)
        add_executable(menu_test desktop/menu_test.cpp)
        target_link_libraries(menu_test PRIVATE raven_cpu)
        add_dependencies(menu_test menu_pe_fixtures)
        add_test(NAME native_menu_ownership COMMAND menu_test ${MENU_FIXTURES}/menus.dll)
        add_test(NAME native_menu_cli COMMAND raven --run-process --execution=native ${MENU_FIXTURES}/client.exe)
        add_test(NAME native_menu_boundary COMMAND raven --run-process --execution=native ${MENU_FIXTURES}/boundary.exe)
        set_tests_properties(native_menu_ownership PROPERTIES TIMEOUT 15)
        set_tests_properties(native_menu_cli native_menu_boundary PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
    endif()
    if(RAVEN_MINGW64 AND OPENSSL_FOUND)
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/bcrypt-client.exe
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-builtin -fno-stack-protector -Wl,--entry,raven_entry,--no-insert-timestamp
                -o ${CMAKE_CURRENT_BINARY_DIR}/bcrypt-client.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/bcrypt_client.c -lbcrypt -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/bcrypt_client.c VERBATIM)
        add_custom_target(bcrypt_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/bcrypt-client.exe)
        add_test(NAME native_bcrypt COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/bcrypt-client.exe)
        set_tests_properties(native_bcrypt PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/capi-session-client.exe
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-builtin -fno-stack-protector -Wl,--entry,raven_entry,--no-insert-timestamp
                -o ${CMAKE_CURRENT_BINARY_DIR}/capi-session-client.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/capi_session_client.c -ladvapi32 -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/capi_session_client.c VERBATIM)
        add_custom_target(capi_session_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/capi-session-client.exe)
        add_test(NAME native_capi_session_keys COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/capi-session-client.exe)
        set_tests_properties(native_capi_session_keys PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/capi-signature-client.exe
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-builtin -fno-stack-protector -Wl,--entry,raven_entry,--no-insert-timestamp
                -o ${CMAKE_CURRENT_BINARY_DIR}/capi-signature-client.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/capi_signature_client.c -ladvapi32 -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/capi_signature_client.c VERBATIM)
        add_custom_target(capi_signature_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/capi-signature-client.exe)
        add_test(NAME native_capi_signatures COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/capi-signature-client.exe)
        set_tests_properties(native_capi_signatures PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/checksum-client.exe
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-builtin -fno-stack-protector -Wl,--entry,raven_entry,--no-insert-timestamp
                -o ${CMAKE_CURRENT_BINARY_DIR}/checksum-client.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/checksum_client.c -limagehlp -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/checksum_client.c VERBATIM)
        add_custom_target(checksum_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/checksum-client.exe)
        add_test(NAME native_image_checksum COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/checksum-client.exe)
        set_tests_properties(native_image_checksum PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
    endif()
    if(RAVEN_MINGW64)
        set(NCCREATE_CHILD ${CMAKE_CURRENT_BINARY_DIR}/nccreate-child.exe)
        add_custom_command(OUTPUT ${NCCREATE_CHILD}
            COMMAND ${RAVEN_MINGW64} -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--no-insert-timestamp
                -o ${NCCREATE_CHILD} ${PROJECT_SOURCE_DIR}/testdata/native_loader/nccreate_child_client.c -lkernel32 -luser32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/nccreate_child_client.c VERBATIM)
        add_custom_target(nccreate_child_fixture ALL DEPENDS ${NCCREATE_CHILD})
        add_test(NAME native_nccreate_child COMMAND raven --run-process --execution=native ${NCCREATE_CHILD})
        set_tests_properties(native_nccreate_child PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(CLASS_MODULE ${CMAKE_CURRENT_BINARY_DIR}/class-module.dll)
        add_custom_command(OUTPUT ${CLASS_MODULE}
            COMMAND ${RAVEN_MINGW64} -shared -nostdlib -fno-stack-protector
                -Wl,--entry,DllMain,--image-base,0x1be000000,--no-insert-timestamp
                -o ${CLASS_MODULE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/class_module.c -lkernel32 -luser32 -lgdi32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/class_module.c VERBATIM)
        add_custom_target(class_module_fixture ALL DEPENDS ${CLASS_MODULE})
        foreach(CLASS_INFO_VARIANT IN ITEMS positive boundary)
            set(CLASS_INFO ${CMAKE_CURRENT_BINARY_DIR}/class-info-${CLASS_INFO_VARIANT}.exe)
            set(CLASS_INFO_OPTIONS)
            if(CLASS_INFO_VARIANT STREQUAL boundary)
                set(CLASS_INFO_OPTIONS -DRAVEN_CLASS_INFO_BOUNDARY)
            endif()
            add_custom_command(OUTPUT ${CLASS_INFO}
                COMMAND ${RAVEN_MINGW64} -nostdlib -fno-stack-protector ${CLASS_INFO_OPTIONS}
                    -Wl,--entry,raven_entry,--image-base,0x1bc000000,--no-insert-timestamp
                    -o ${CLASS_INFO} ${PROJECT_SOURCE_DIR}/testdata/native_loader/class_info_client.c -lkernel32 -luser32 -lgdi32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/class_info_client.c VERBATIM)
            add_custom_target(class_info_${CLASS_INFO_VARIANT} ALL DEPENDS ${CLASS_INFO} ${CLASS_MODULE})
            add_test(NAME native_class_info_${CLASS_INFO_VARIANT} COMMAND raven --run-process --execution=native ${CLASS_INFO})
            set_tests_properties(native_class_info_${CLASS_INFO_VARIANT} PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        endforeach()
        foreach(WINDOW_DATA_VARIANT IN ITEMS positive boundary)
            set(WINDOW_DATA ${CMAKE_CURRENT_BINARY_DIR}/window-data-${WINDOW_DATA_VARIANT}.exe)
            set(WINDOW_DATA_OPTIONS)
            if(WINDOW_DATA_VARIANT STREQUAL boundary)
                set(WINDOW_DATA_OPTIONS -DRAVEN_WINDOW_DATA_BOUNDARY)
            endif()
            add_custom_command(OUTPUT ${WINDOW_DATA}
                COMMAND ${RAVEN_MINGW64} -nostdlib -fno-stack-protector ${WINDOW_DATA_OPTIONS}
                    -Wl,--entry,raven_entry,--image-base,0x1ba000000,--no-insert-timestamp
                    -o ${WINDOW_DATA} ${PROJECT_SOURCE_DIR}/testdata/native_loader/window_data_client.c -lkernel32 -luser32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/window_data_client.c VERBATIM)
            add_custom_target(window_data_${WINDOW_DATA_VARIANT} ALL DEPENDS ${WINDOW_DATA})
            add_test(NAME native_window_data_${WINDOW_DATA_VARIANT} COMMAND raven --run-process --execution=native ${WINDOW_DATA})
            set_tests_properties(native_window_data_${WINDOW_DATA_VARIANT} PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        endforeach()
        set(CLASS_BACKGROUND ${CMAKE_CURRENT_BINARY_DIR}/class-background.exe)
        add_custom_command(OUTPUT ${CLASS_BACKGROUND}
            COMMAND ${RAVEN_MINGW64} -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x1b8000000,--no-insert-timestamp
                -o ${CLASS_BACKGROUND} ${PROJECT_SOURCE_DIR}/testdata/native_loader/class_background_client.c -lkernel32 -luser32 -lgdi32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/class_background_client.c VERBATIM)
        add_custom_target(class_background_fixture ALL DEPENDS ${CLASS_BACKGROUND})
        add_executable(class_background_test desktop/class_background_test.cpp)
        target_link_libraries(class_background_test PRIVATE raven_cpu)
        add_dependencies(class_background_test class_background_fixture raven-bootstrap)
        add_test(NAME native_class_background COMMAND class_background_test $<TARGET_FILE:raven-bootstrap> ${CLASS_BACKGROUND} ${CMAKE_CURRENT_BINARY_DIR}/class-background.bgra)
        set_tests_properties(native_class_background PROPERTIES TIMEOUT 20)
        set(CLASS_BACKGROUND_BOUNDARY ${CMAKE_CURRENT_BINARY_DIR}/class-background-boundary.exe)
        add_custom_command(OUTPUT ${CLASS_BACKGROUND_BOUNDARY}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -DRAVEN_CLASS_BACKGROUND_BOUNDARY
                -Wl,--entry,raven_entry,--image-base,0x1b8000000,--no-insert-timestamp
                -o ${CLASS_BACKGROUND_BOUNDARY} ${PROJECT_SOURCE_DIR}/testdata/native_loader/class_background_client.c -lkernel32 -luser32 -lgdi32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/class_background_client.c VERBATIM)
        add_custom_target(class_background_boundary ALL DEPENDS ${CLASS_BACKGROUND_BOUNDARY})
        add_test(NAME native_class_background_boundary COMMAND raven --run-process --execution=native ${CLASS_BACKGROUND_BOUNDARY})
        set_tests_properties(native_class_background_boundary PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(EXIT_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/exit-fixtures)
        file(MAKE_DIRECTORY ${EXIT_FIXTURES})
        set(EXIT_OUTPUTS "")
        foreach(EXIT_DLL_CASE basic attach tls repeat fault)
            set(EXIT_DLL_DEFINE "")
            if(NOT EXIT_DLL_CASE STREQUAL "basic")
                string(TOUPPER "${EXIT_DLL_CASE}" EXIT_DLL_MODE)
                set(EXIT_DLL_DEFINE -DRAVEN_EXIT_${EXIT_DLL_MODE})
            endif()
            add_custom_command(OUTPUT ${EXIT_FIXTURES}/exit-${EXIT_DLL_CASE}.dll
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -shared -fno-stack-protector ${EXIT_DLL_DEFINE}
                    -Wl,--entry,DllMain,--image-base,0x1cf000000,--no-insert-timestamp
                    -o ${EXIT_FIXTURES}/exit-${EXIT_DLL_CASE}.dll ${PROJECT_SOURCE_DIR}/testdata/native_loader/exit_module.c -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/exit_module.c VERBATIM)
            list(APPEND EXIT_OUTPUTS ${EXIT_FIXTURES}/exit-${EXIT_DLL_CASE}.dll)
        endforeach()
        foreach(EXIT_MODE RANGE 0 8)
            add_custom_command(OUTPUT ${EXIT_FIXTURES}/exit-${EXIT_MODE}.exe
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -DRAVEN_EXIT_MODE=${EXIT_MODE}
                    -Wl,--entry,raven_entry,--image-base,0x1ce000000,--no-insert-timestamp
                    -o ${EXIT_FIXTURES}/exit-${EXIT_MODE}.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/exit_client.c -lkernel32 -luser32 -lgdi32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/exit_client.c VERBATIM)
            list(APPEND EXIT_OUTPUTS ${EXIT_FIXTURES}/exit-${EXIT_MODE}.exe)
        endforeach()
        add_custom_target(exit_fixtures ALL DEPENDS ${EXIT_OUTPUTS})
        add_executable(exit_test nt/exit_test.cpp)
        target_link_libraries(exit_test PRIVATE raven_cpu)
        add_dependencies(exit_test exit_fixtures raven-bootstrap)
        add_test(NAME native_process_exit COMMAND exit_test $<TARGET_FILE:raven-bootstrap> ${EXIT_FIXTURES})
        set_tests_properties(native_process_exit PROPERTIES TIMEOUT 30)
        set(TLS_ORDER_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/tls-order-fixtures)
        file(MAKE_DIRECTORY ${TLS_ORDER_FIXTURES})
        add_custom_command(OUTPUT ${TLS_ORDER_FIXTURES}/tls-order.dll
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -shared -fno-stack-protector
                -Wl,--entry,DllMain,--image-base,0x1bd000000,--no-insert-timestamp
                -o ${TLS_ORDER_FIXTURES}/tls-order.dll ${PROJECT_SOURCE_DIR}/testdata/native_loader/tls_order_module.c -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/tls_order_module.c VERBATIM)
        foreach(TLS_ORDER_CASE unload shutdown)
            set(TLS_ORDER_DEFINE "")
            if(TLS_ORDER_CASE STREQUAL "shutdown")
                set(TLS_ORDER_DEFINE -DRAVEN_TLS_ORDER_SHUTDOWN)
            endif()
            add_custom_command(OUTPUT ${TLS_ORDER_FIXTURES}/${TLS_ORDER_CASE}.exe
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector ${TLS_ORDER_DEFINE}
                    -Wl,--entry,raven_entry,--image-base,0x1bc000000,--no-insert-timestamp
                    -o ${TLS_ORDER_FIXTURES}/${TLS_ORDER_CASE}.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/tls_order_client.c -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/tls_order_client.c VERBATIM)
        endforeach()
        add_custom_target(tls_order_fixtures ALL DEPENDS ${TLS_ORDER_FIXTURES}/tls-order.dll ${TLS_ORDER_FIXTURES}/unload.exe ${TLS_ORDER_FIXTURES}/shutdown.exe)
        add_executable(tls_order_test nt/tls_order_test.cpp)
        target_link_libraries(tls_order_test PRIVATE raven_cpu)
        add_dependencies(tls_order_test tls_order_fixtures raven-bootstrap)
        add_test(NAME native_tls_detach_order COMMAND tls_order_test $<TARGET_FILE:raven-bootstrap> ${TLS_ORDER_FIXTURES})
        set_tests_properties(native_tls_detach_order PROPERTIES TIMEOUT 15)
        set(COMPATIBLE_BITMAP_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/compatible-bitmap.exe)
        add_custom_command(OUTPUT ${COMPATIBLE_BITMAP_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x1ba000000,--no-insert-timestamp
                -o ${COMPATIBLE_BITMAP_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/compatible_bitmap_client.c -lgdi32 -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/compatible_bitmap_client.c VERBATIM)
        add_custom_target(compatible_bitmap_fixture ALL DEPENDS ${COMPATIBLE_BITMAP_FIXTURE})
        add_test(NAME native_compatible_bitmap_cli COMMAND raven --run-process --execution=native ${COMPATIBLE_BITMAP_FIXTURE})
        set_tests_properties(native_compatible_bitmap_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(BITBLT_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/bitblt-fixture.exe)
        add_custom_command(OUTPUT ${BITBLT_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x1b8000000,--no-insert-timestamp
                -o ${BITBLT_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/bitblt_client.c -lgdi32 -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/bitblt_client.c VERBATIM)
        add_custom_target(bitblt_pe_fixture ALL DEPENDS ${BITBLT_FIXTURE})
        add_test(NAME native_bitblt_cli COMMAND raven --run-process --execution=native ${BITBLT_FIXTURE})
        set_tests_properties(native_bitblt_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(BRUSH_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/brush-fixture.exe)
        add_custom_command(OUTPUT ${BRUSH_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x1b6000000,--no-insert-timestamp
                -o ${BRUSH_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/brush_client.c -lgdi32 -luser32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/brush_client.c VERBATIM)
        add_custom_target(brush_pe_fixture ALL DEPENDS ${BRUSH_FIXTURE})
        add_test(NAME native_brush_cli COMMAND raven --run-process --execution=native ${BRUSH_FIXTURE})
        set_tests_properties(native_brush_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        if(TARGET raven_text)
            set(TEXT_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/text-fixture.exe)
            add_custom_command(OUTPUT ${TEXT_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1b0000000,--no-insert-timestamp
                    -o ${TEXT_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/text_client.c -lgdi32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/text_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/text_reference.h VERBATIM)
            add_custom_target(text_pe_fixture ALL DEPENDS ${TEXT_FIXTURE})
            set(TEXT_BOUNDARY ${CMAKE_CURRENT_BINARY_DIR}/text-boundary.exe)
            add_custom_command(OUTPUT ${TEXT_BOUNDARY}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -DRAVEN_TEXT_BOUNDARY
                    -Wl,--entry,raven_entry,--image-base,0x1b0000000,--no-insert-timestamp
                    -o ${TEXT_BOUNDARY} ${PROJECT_SOURCE_DIR}/testdata/native_loader/text_client.c -lgdi32 -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/text_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/text_reference.h VERBATIM)
            add_custom_target(text_boundary_fixture ALL DEPENDS ${TEXT_BOUNDARY})
            add_test(NAME native_text_boundary COMMAND raven --run-process --execution=native --timeout-ms=30000 ${TEXT_BOUNDARY})
            set_tests_properties(native_text_boundary PROPERTIES TIMEOUT 40 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            # The fallback faces are optional host fonts, so the test only exists where both are installed.
            if(EXISTS /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf AND EXISTS /usr/local/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc)
                set(TEXT_FALLBACK ${CMAKE_CURRENT_BINARY_DIR}/text-fallback.exe)
                add_custom_command(OUTPUT ${TEXT_FALLBACK}
                    COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                        -Wl,--entry,raven_entry,--image-base,0x1db000000,--no-insert-timestamp
                        -o ${TEXT_FALLBACK} ${PROJECT_SOURCE_DIR}/testdata/native_loader/text_fallback_client.c -lgdi32 -lkernel32
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/text_fallback_client.c VERBATIM)
                add_custom_target(text_fallback_fixture ALL DEPENDS ${TEXT_FALLBACK})
                add_test(NAME native_text_fallback COMMAND raven --run-process --execution=native --timeout-ms=30000 ${TEXT_FALLBACK})
                set_tests_properties(native_text_fallback PROPERTIES TIMEOUT 40 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            endif()
            add_test(NAME native_text_cli COMMAND raven --run-process --execution=native --timeout-ms=30000 ${TEXT_FIXTURE})
            set_tests_properties(native_text_cli PROPERTIES TIMEOUT 40 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            add_executable(text_client_test desktop/text_client_test.cpp)
            target_link_libraries(text_client_test PRIVATE raven_cpu)
            add_dependencies(text_client_test text_pe_fixture raven-bootstrap)
            add_test(NAME native_text_pixels COMMAND text_client_test $<TARGET_FILE:raven-bootstrap> ${TEXT_FIXTURE} ${CMAKE_CURRENT_BINARY_DIR}/text-output)
            set_tests_properties(native_text_pixels PROPERTIES TIMEOUT 40)
            set(ATOM_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/atom-client.exe)
            add_custom_command(OUTPUT ${ATOM_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O1 -o ${ATOM_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/atom_client.c -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/atom_client.c VERBATIM)
            add_custom_target(atom_fixture ALL DEPENDS ${ATOM_FIXTURE})
            add_test(NAME native_atoms COMMAND raven --run-process --execution=native ${ATOM_FIXTURE})
            set_tests_properties(native_atoms PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native process exited with status 0")
            set(LOCAL_REUSE_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/local-reuse-client.exe)
            add_custom_command(OUTPUT ${LOCAL_REUSE_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O1 -o ${LOCAL_REUSE_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/local_reuse_client.c -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/local_reuse_client.c VERBATIM)
            add_custom_target(local_reuse_fixture ALL DEPENDS ${LOCAL_REUSE_FIXTURE})
            add_test(NAME native_local_reuse COMMAND raven --run-process --execution=native ${LOCAL_REUSE_FIXTURE})
            set_tests_properties(native_local_reuse PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native process exited with status 0")
            set(SLOT_REUSE_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/slot-reuse-client.exe)
            add_custom_command(OUTPUT ${SLOT_REUSE_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O1 -o ${SLOT_REUSE_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/slot_reuse_client.c -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/slot_reuse_client.c VERBATIM)
            add_custom_target(slot_reuse_fixture ALL DEPENDS ${SLOT_REUSE_FIXTURE})
            add_test(NAME native_slot_reuse COMMAND raven --run-process --execution=native ${SLOT_REUSE_FIXTURE})
            set_tests_properties(native_slot_reuse PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native process exited with status 0")
            set(ZONE_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/zone-client.exe)
            add_custom_command(OUTPUT ${ZONE_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O1 -o ${ZONE_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/zone_client.c -lurlmon -lole32 -luuid
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/zone_client.c VERBATIM)
            add_custom_target(zone_fixture ALL DEPENDS ${ZONE_FIXTURE})
            add_test(NAME native_zones COMMAND raven --run-process --execution=native ${ZONE_FIXTURE})
            set_tests_properties(native_zones PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native process exited with status 0")
            set(TEXT_PAINT ${CMAKE_CURRENT_BINARY_DIR}/text-paint.exe)
            add_custom_command(OUTPUT ${TEXT_PAINT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1b4000000,--no-insert-timestamp
                    -o ${TEXT_PAINT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/text_paint_client.c -lkernel32 -luser32 -lgdi32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/text_paint_client.c VERBATIM)
            add_custom_target(text_paint_fixture ALL DEPENDS ${TEXT_PAINT})
            add_executable(text_paint_test desktop/text_paint_test.cpp)
            target_link_libraries(text_paint_test PRIVATE raven_cpu)
            add_dependencies(text_paint_test text_paint_fixture raven-bootstrap)
            add_test(NAME native_text_paint COMMAND text_paint_test $<TARGET_FILE:raven-bootstrap> ${TEXT_PAINT} ${CMAKE_CURRENT_BINARY_DIR}/text-paint.bgra)
            set_tests_properties(native_text_paint PROPERTIES TIMEOUT 20)
        endif()
        set(FONT_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/font-fixture.exe)
        add_custom_command(OUTPUT ${FONT_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x1ab000000,--no-insert-timestamp
                -o ${FONT_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/font_client.c -lgdi32 -luser32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/font_client.c VERBATIM)
        add_custom_target(font_pe_fixture ALL DEPENDS ${FONT_FIXTURE})
        add_test(NAME native_font_cli COMMAND raven --run-process --execution=native ${FONT_FIXTURE})
        set_tests_properties(native_font_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(FONT_BOUNDARY ${CMAKE_CURRENT_BINARY_DIR}/font-boundary.exe)
        add_custom_command(OUTPUT ${FONT_BOUNDARY}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -DRAVEN_FONT_BOUNDARY
                -Wl,--entry,raven_entry,--image-base,0x1ab000000,--no-insert-timestamp
                -o ${FONT_BOUNDARY} ${PROJECT_SOURCE_DIR}/testdata/native_loader/font_client.c -lgdi32 -luser32 -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/font_client.c VERBATIM)
        add_custom_target(font_boundary_fixture ALL DEPENDS ${FONT_BOUNDARY})
        add_test(NAME native_font_boundary COMMAND raven --run-process --execution=native ${FONT_BOUNDARY})
        set_tests_properties(native_font_boundary PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(MEMORY_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/memory-fixture.exe)
        add_custom_command(OUTPUT ${MEMORY_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x196000000
                -o ${MEMORY_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/memory_client.c -lkernel32 -lntdll
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/memory_client.c VERBATIM)
        add_custom_target(memory_pe_fixture ALL DEPENDS ${MEMORY_FIXTURE})
        add_test(NAME native_memory_cli COMMAND raven --run-process --execution=native ${MEMORY_FIXTURE})
        set_tests_properties(native_memory_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(STRING_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/string-fixture.exe)
        add_custom_command(OUTPUT ${STRING_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x197000000
                -o ${STRING_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/string_client.c -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/string_client.c VERBATIM)
        add_custom_target(string_pe_fixture ALL DEPENDS ${STRING_FIXTURE})
        add_test(NAME native_string_cli COMMAND raven --run-process --execution=native ${STRING_FIXTURE})
        set_tests_properties(native_string_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(CLOCK_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/clock-fixture.exe)
        add_custom_command(OUTPUT ${CLOCK_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x197100000
                -o ${CLOCK_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/clock_client.c -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/clock_client.c VERBATIM)
        add_custom_target(clock_pe_fixture ALL DEPENDS ${CLOCK_FIXTURE})
        add_test(NAME native_clock_cli COMMAND raven --run-process --execution=native ${CLOCK_FIXTURE})
        set_tests_properties(native_clock_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        add_test(NAME native_parallel_clock_cli COMMAND raven --run-process --execution=native ${CLOCK_FIXTURE})
        set_tests_properties(native_parallel_clock_cli PROPERTIES TIMEOUT 15 ENVIRONMENT RAVEN_PARALLEL=1 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(SCREEN_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/screen-fixture.exe)
        add_custom_command(OUTPUT ${SCREEN_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x197200000
                -o ${SCREEN_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/screen_client.c -lgdi32 -luser32 -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/screen_client.c VERBATIM)
        add_custom_target(screen_pe_fixture ALL DEPENDS ${SCREEN_FIXTURE})
        add_test(NAME native_screen_dc_cli COMMAND raven --run-process --execution=native ${SCREEN_FIXTURE})
        set_tests_properties(native_screen_dc_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(WINDOW_STATE_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/window-state-fixture.exe)
        add_custom_command(OUTPUT ${WINDOW_STATE_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x197300000
                -o ${WINDOW_STATE_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/window_state_client.c -luser32 -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/window_state_client.c VERBATIM)
        add_custom_target(window_state_pe_fixture ALL DEPENDS ${WINDOW_STATE_FIXTURE})
        add_test(NAME native_window_state_cli COMMAND raven --run-process --execution=native ${WINDOW_STATE_FIXTURE})
        set_tests_properties(native_window_state_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(LOCALE_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/locale-fixture.exe)
        add_custom_command(OUTPUT ${LOCALE_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x197400000
                -o ${LOCALE_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/locale_client.c -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/locale_client.c VERBATIM)
        add_custom_target(locale_pe_fixture ALL DEPENDS ${LOCALE_FIXTURE})
        add_test(NAME native_locale_cli COMMAND raven --run-process --execution=native ${LOCALE_FIXTURE})
        set_tests_properties(native_locale_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(MENU_POSITION_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/menu-position-fixture.exe)
        add_custom_command(OUTPUT ${MENU_POSITION_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x197500000
                -o ${MENU_POSITION_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/menu_position_client.c -luser32 -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/menu_position_client.c VERBATIM)
        add_custom_target(menu_position_pe_fixture ALL DEPENDS ${MENU_POSITION_FIXTURE})
        add_test(NAME native_menu_position_cli COMMAND raven --run-process --execution=native ${MENU_POSITION_FIXTURE})
        set_tests_properties(native_menu_position_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(BITMAP_OBJECT_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/bitmap-object-fixture.exe)
        add_custom_command(OUTPUT ${BITMAP_OBJECT_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x197600000
                -o ${BITMAP_OBJECT_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/bitmap_object_client.c -lgdi32 -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/bitmap_object_client.c VERBATIM)
        add_custom_target(bitmap_object_pe_fixture ALL DEPENDS ${BITMAP_OBJECT_FIXTURE})
        add_test(NAME native_bitmap_object_cli COMMAND raven --run-process --execution=native ${BITMAP_OBJECT_FIXTURE})
        set_tests_properties(native_bitmap_object_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(PURE_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/pure-fixture.exe)
        add_custom_command(OUTPUT ${PURE_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x197700000
                -o ${PURE_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/pure_client.c -luxtheme -lshlwapi -lgdi32 -luser32 -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/pure_client.c VERBATIM)
        add_custom_target(pure_pe_fixture ALL DEPENDS ${PURE_FIXTURE})
        add_test(NAME native_pure_cli COMMAND raven --run-process --execution=native ${PURE_FIXTURE})
        set_tests_properties(native_pure_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(THEME_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/theme-fixture.exe)
        add_custom_command(OUTPUT ${THEME_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -Wl,--entry,raven_entry,--no-insert-timestamp
                -o ${THEME_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/theme_client.c -luxtheme -lgdi32 -luser32 -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/theme_client.c VERBATIM)
        add_custom_target(theme_pe_fixture ALL DEPENDS ${THEME_FIXTURE})
        add_test(NAME native_theme_dark_cli COMMAND raven --run-process --execution=native ${THEME_FIXTURE})
        set_tests_properties(native_theme_dark_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(DC_ATTRIBUTE_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/dc-attribute-fixture.exe)
        add_custom_command(OUTPUT ${DC_ATTRIBUTE_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x197800000
                -o ${DC_ATTRIBUTE_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/dc_attribute_client.c -lgdi32 -luser32 -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/dc_attribute_client.c VERBATIM)
        add_custom_target(dc_attribute_pe_fixture ALL DEPENDS ${DC_ATTRIBUTE_FIXTURE})
        add_test(NAME native_dc_attribute_cli COMMAND raven --run-process --execution=native ${DC_ATTRIBUTE_FIXTURE})
        set_tests_properties(native_dc_attribute_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(TRANSPARENT_CHILD_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/transparent-child-fixture.exe)
        add_custom_command(OUTPUT ${TRANSPARENT_CHILD_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -Wl,--entry,raven_entry,--no-insert-timestamp
                -o ${TRANSPARENT_CHILD_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/transparent_child_client.c -lgdi32 -luser32 -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/transparent_child_client.c VERBATIM)
        add_custom_target(transparent_child_pe_fixture ALL DEPENDS ${TRANSPARENT_CHILD_FIXTURE})
        add_test(NAME native_transparent_child_cli COMMAND raven --run-process --execution=native ${TRANSPARENT_CHILD_FIXTURE})
        set_tests_properties(native_transparent_child_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(RELEASE_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/release-fixture.exe)
        add_custom_command(OUTPUT ${RELEASE_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x197900000
                -o ${RELEASE_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/release_client.c -lgdi32 -luser32 -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/release_client.c VERBATIM)
        add_custom_target(release_pe_fixture ALL DEPENDS ${RELEASE_FIXTURE})
        add_test(NAME native_release_cli COMMAND raven --run-process --execution=native ${RELEASE_FIXTURE})
        set_tests_properties(native_release_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(SYSTEM_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/system-fixture.exe)
        add_custom_command(OUTPUT ${SYSTEM_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x198000000
                -o ${SYSTEM_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/system_client.c -lkernel32 -lntdll
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/system_client.c VERBATIM)
        add_custom_target(system_pe_fixture ALL DEPENDS ${SYSTEM_FIXTURE})
        add_test(NAME native_system_cli COMMAND raven --run-process --execution=native ${SYSTEM_FIXTURE})
        set_tests_properties(native_system_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(EVENT_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/event-fixture.exe)
        add_custom_command(OUTPUT ${EVENT_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x199000000
                -o ${EVENT_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/event_client.c -lkernel32 -lntdll
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/event_client.c VERBATIM)
        add_custom_target(event_pe_fixture ALL DEPENDS ${EVENT_FIXTURE})
        add_test(NAME native_event_cli COMMAND raven --run-process --execution=native ${EVENT_FIXTURE})
        set_tests_properties(native_event_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(WINDOW_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/window-fixture.exe)
        add_custom_command(OUTPUT ${WINDOW_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -nostdlib
                -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                -o ${WINDOW_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/window_client.c -lkernel32 -luser32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/window_client.c VERBATIM)
        add_custom_target(window_pe_fixture ALL DEPENDS ${WINDOW_FIXTURE})
        set(OUTER_SURFACE_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/outer-surface.exe)
        add_custom_command(OUTPUT ${OUTER_SURFACE_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x1a4000000
                -o ${OUTER_SURFACE_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/outer_surface_client.c -lkernel32 -luser32 -lgdi32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/outer_surface_client.c VERBATIM)
        add_custom_target(outer_surface_pe_fixture ALL DEPENDS ${OUTER_SURFACE_FIXTURE})
        add_test(NAME native_outer_surface_cli COMMAND raven --run-process --execution=native ${OUTER_SURFACE_FIXTURE})
        set_tests_properties(native_outer_surface_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(NONCLIENT_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/nonclient-metrics.exe)
        set(NONCLIENT_BOUNDARY ${CMAKE_CURRENT_BINARY_DIR}/nonclient-metrics-boundary.exe)
        add_custom_command(OUTPUT ${NONCLIENT_FIXTURE} ${NONCLIENT_BOUNDARY}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                -o ${NONCLIENT_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/nonclient_client.c -lkernel32 -luser32
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -DRAVEN_NONCLIENT_BOUNDARY
                -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                -o ${NONCLIENT_BOUNDARY} ${PROJECT_SOURCE_DIR}/testdata/native_loader/nonclient_client.c -lkernel32 -luser32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/nonclient_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/nonclient_reference.h VERBATIM)
        add_custom_target(nonclient_pe_fixture ALL DEPENDS ${NONCLIENT_FIXTURE} ${NONCLIENT_BOUNDARY})
        add_executable(nonclient_test desktop/nonclient_test.cpp)
        target_link_libraries(nonclient_test PRIVATE raven_cpu)
        add_dependencies(nonclient_test nonclient_pe_fixture raven-bootstrap)
        add_test(NAME native_nonclient_contract COMMAND nonclient_test $<TARGET_FILE:raven-bootstrap> ${NONCLIENT_FIXTURE})
        add_test(NAME native_nonclient_cli COMMAND raven --run-process --execution=native ${NONCLIENT_FIXTURE})
        add_test(NAME native_nonclient_boundary COMMAND raven --run-process --execution=native ${NONCLIENT_BOUNDARY})
        set_tests_properties(native_nonclient_contract PROPERTIES TIMEOUT 15)
        set_tests_properties(native_nonclient_cli native_nonclient_boundary PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(CREATION_DC_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/creation-dc.exe)
        add_custom_command(OUTPUT ${CREATION_DC_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                -o ${CREATION_DC_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/creation_dc_client.c -lkernel32 -luser32 -lgdi32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/creation_dc_client.c VERBATIM)
        add_custom_target(creation_dc_fixture ALL DEPENDS ${CREATION_DC_FIXTURE})
        add_executable(creation_dc_test desktop/creation_dc_test.cpp)
        target_link_libraries(creation_dc_test PRIVATE raven_cpu)
        add_dependencies(creation_dc_test creation_dc_fixture raven-bootstrap)
        add_test(NAME native_creation_dc COMMAND creation_dc_test $<TARGET_FILE:raven-bootstrap> ${CREATION_DC_FIXTURE})
        set_tests_properties(native_creation_dc PROPERTIES TIMEOUT 15)
        set(CREATION_DC_BOUNDARY ${CMAKE_CURRENT_BINARY_DIR}/creation-dc-boundary.exe)
        add_custom_command(OUTPUT ${CREATION_DC_BOUNDARY}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -DRAVEN_DC_BOUNDARY
                -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                -o ${CREATION_DC_BOUNDARY} ${PROJECT_SOURCE_DIR}/testdata/native_loader/creation_dc_client.c -lkernel32 -luser32 -lgdi32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/creation_dc_client.c VERBATIM)
        add_custom_target(creation_dc_boundary_fixture ALL DEPENDS ${CREATION_DC_BOUNDARY})
        add_dependencies(creation_dc_test creation_dc_boundary_fixture)
        add_test(NAME native_creation_dc_boundary COMMAND creation_dc_test $<TARGET_FILE:raven-bootstrap> ${CREATION_DC_BOUNDARY} boundary)
        set_tests_properties(native_creation_dc_boundary PROPERTIES TIMEOUT 15)
        if(TARGET raven_text)
            set(OUTER_SURFACE_VISIBLE ${CMAKE_CURRENT_BINARY_DIR}/outer-surface-visible.exe)
            add_custom_command(OUTPUT ${OUTER_SURFACE_VISIBLE}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -DVISIBLE_SURFACE
                    -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                    -o ${OUTER_SURFACE_VISIBLE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/outer_surface_client.c -lkernel32 -luser32 -lgdi32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/outer_surface_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/icon_data.h VERBATIM)
            add_custom_target(outer_surface_visible_fixture ALL DEPENDS ${OUTER_SURFACE_VISIBLE})
            set(DIALOG_FRAME_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/dialog-frame.exe)
            add_custom_command(OUTPUT ${DIALOG_FRAME_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                    -o ${DIALOG_FRAME_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/dialog_frame_client.c -lkernel32 -luser32 -lgdi32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/dialog_frame_client.c VERBATIM)
            add_custom_target(dialog_frame_fixture ALL DEPENDS ${DIALOG_FRAME_FIXTURE})
            add_executable(dialog_frame_test desktop/dialog_frame_test.cpp)
            target_link_libraries(dialog_frame_test PRIVATE raven_cpu)
            add_dependencies(dialog_frame_test dialog_frame_fixture raven-bootstrap)
            add_test(NAME native_dialog_frame COMMAND dialog_frame_test $<TARGET_FILE:raven-bootstrap> ${DIALOG_FRAME_FIXTURE})
            set_tests_properties(native_dialog_frame PROPERTIES TIMEOUT 15)
            set(DESTROYED_FRAME_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/destroyed-frame.exe)
            add_custom_command(OUTPUT ${DESTROYED_FRAME_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -DDESTROYED_FRAME
                    -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                    -o ${DESTROYED_FRAME_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/dialog_frame_client.c -lkernel32 -luser32 -lgdi32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/dialog_frame_client.c VERBATIM)
            add_custom_target(destroyed_frame_fixture ALL DEPENDS ${DESTROYED_FRAME_FIXTURE})
            add_dependencies(dialog_frame_test destroyed_frame_fixture)
            add_test(NAME native_destroyed_child_frame COMMAND dialog_frame_test $<TARGET_FILE:raven-bootstrap> ${DESTROYED_FRAME_FIXTURE} destroyed)
            set_tests_properties(native_destroyed_child_frame PROPERTIES TIMEOUT 15)
            set(BUTTON_CORNER_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/button-corner.exe)
            add_custom_command(OUTPUT ${BUTTON_CORNER_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                    -o ${BUTTON_CORNER_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/button_corner_client.c -lkernel32 -luser32 -lgdi32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/button_corner_client.c VERBATIM)
            add_custom_target(button_corner_fixture ALL DEPENDS ${BUTTON_CORNER_FIXTURE})
            add_test(NAME native_button_parent_corners COMMAND raven --run-process --execution=native ${BUTTON_CORNER_FIXTURE})
            set_tests_properties(native_button_parent_corners PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            set(ITALIC_FACE_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/italic-face.exe)
            add_custom_command(OUTPUT ${ITALIC_FACE_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                    -o ${ITALIC_FACE_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/italic_face_client.c -lkernel32 -lgdi32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/italic_face_client.c VERBATIM)
            add_custom_target(italic_face_fixture ALL DEPENDS ${ITALIC_FACE_FIXTURE})
            add_test(NAME native_italic_face_enumeration COMMAND raven --run-process --execution=native ${ITALIC_FACE_FIXTURE})
            set_tests_properties(native_italic_face_enumeration PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            set(TOKEN_MEMBERSHIP_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/token-membership.exe)
            add_custom_command(OUTPUT ${TOKEN_MEMBERSHIP_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                    -o ${TOKEN_MEMBERSHIP_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/token_membership_client.c -lkernel32 -ladvapi32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/token_membership_client.c VERBATIM)
            add_custom_target(token_membership_fixture ALL DEPENDS ${TOKEN_MEMBERSHIP_FIXTURE})
            add_test(NAME native_token_membership COMMAND raven --run-process --execution=native ${TOKEN_MEMBERSHIP_FIXTURE})
            set_tests_properties(native_token_membership PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            set(SECTION_VIEWS_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/section-views.exe)
            add_custom_command(OUTPUT ${SECTION_VIEWS_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                    -o ${SECTION_VIEWS_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/section_views_client.c -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/section_views_client.c VERBATIM)
            add_custom_target(section_views_fixture ALL DEPENDS ${SECTION_VIEWS_FIXTURE})
            add_test(NAME native_section_views_share_pages COMMAND raven --run-process --execution=native ${SECTION_VIEWS_FIXTURE})
            set_tests_properties(native_section_views_share_pages PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            set(POSITION_CHANGED_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/position-changed.exe)
            add_custom_command(OUTPUT ${POSITION_CHANGED_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                    -o ${POSITION_CHANGED_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/position_changed_client.c -lkernel32 -luser32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/position_changed_client.c VERBATIM)
            add_custom_target(position_changed_fixture ALL DEPENDS ${POSITION_CHANGED_FIXTURE})
            add_test(NAME native_position_changed_messages COMMAND raven --run-process --execution=native ${POSITION_CHANGED_FIXTURE})
            set_tests_properties(native_position_changed_messages PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            set(STRETCH_WORLD_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/stretch-world.exe)
            add_custom_command(OUTPUT ${STRETCH_WORLD_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                    -o ${STRETCH_WORLD_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/stretch_world_client.c -lkernel32 -lgdi32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/stretch_world_client.c VERBATIM)
            add_custom_target(stretch_world_fixture ALL DEPENDS ${STRETCH_WORLD_FIXTURE})
            add_test(NAME native_stretch_dibits_world COMMAND raven --run-process --execution=native ${STRETCH_WORLD_FIXTURE})
            set_tests_properties(native_stretch_dibits_world PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            set(MAXIMIZED_EDGES_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/maximized-edges.exe)
            add_custom_command(OUTPUT ${MAXIMIZED_EDGES_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                    -o ${MAXIMIZED_EDGES_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/maximized_edges_client.c -lkernel32 -luser32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/maximized_edges_client.c VERBATIM)
            add_custom_target(maximized_edges_fixture ALL DEPENDS ${MAXIMIZED_EDGES_FIXTURE})
            add_test(NAME native_maximized_edges COMMAND raven --run-process --execution=native ${MAXIMIZED_EDGES_FIXTURE})
            set_tests_properties(native_maximized_edges PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            set(UNCOVERED_SIBLING_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/uncovered-sibling.exe)
            add_custom_command(OUTPUT ${UNCOVERED_SIBLING_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                    -o ${UNCOVERED_SIBLING_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/uncovered_sibling_client.c -lkernel32 -luser32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/uncovered_sibling_client.c VERBATIM)
            add_custom_target(uncovered_sibling_fixture ALL DEPENDS ${UNCOVERED_SIBLING_FIXTURE})
            add_test(NAME native_uncovered_sibling_repaint COMMAND raven --run-process --execution=native ${UNCOVERED_SIBLING_FIXTURE})
            set_tests_properties(native_uncovered_sibling_repaint PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            set(OWNER_ACTIVATION_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/owner-activation.exe)
            add_custom_command(OUTPUT ${OWNER_ACTIVATION_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                    -o ${OWNER_ACTIVATION_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/owner_activation_client.c -lkernel32 -luser32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/owner_activation_client.c VERBATIM)
            add_custom_target(owner_activation_fixture ALL DEPENDS ${OWNER_ACTIVATION_FIXTURE})
            add_test(NAME native_owner_activation COMMAND raven --run-process --execution=native ${OWNER_ACTIVATION_FIXTURE})
            set_tests_properties(native_owner_activation PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            set(THREAD_BACKGROUND_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/thread-background.exe)
            add_custom_command(OUTPUT ${THREAD_BACKGROUND_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                    -o ${THREAD_BACKGROUND_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/thread_background_client.c -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/thread_background_client.c VERBATIM)
            add_custom_target(thread_background_fixture ALL DEPENDS ${THREAD_BACKGROUND_FIXTURE})
            add_test(NAME native_thread_background_mode COMMAND raven --run-process --execution=native ${THREAD_BACKGROUND_FIXTURE})
            set_tests_properties(native_thread_background_mode PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            set(INHERIT_LIST_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/inherit-list.exe)
            add_custom_command(OUTPUT ${INHERIT_LIST_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                    -o ${INHERIT_LIST_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/inherit_list_client.c -luser32 -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/inherit_list_client.c VERBATIM)
            add_custom_target(inherit_list_fixture ALL DEPENDS ${INHERIT_LIST_FIXTURE})
            add_test(NAME native_inherit_list COMMAND raven --run-process --execution=native ${INHERIT_LIST_FIXTURE})
            set_tests_properties(native_inherit_list PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            set(MENU_TRACK_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/menu-track.exe)
            add_custom_command(OUTPUT ${MENU_TRACK_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                    -o ${MENU_TRACK_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/menu_track_client.c -lkernel32 -luser32 -lgdi32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/menu_track_client.c VERBATIM)
            add_custom_target(menu_track_fixture ALL DEPENDS ${MENU_TRACK_FIXTURE})
            add_executable(menu_track_test desktop/menu_track_test.cpp)
            target_link_libraries(menu_track_test PRIVATE raven_cpu)
            add_dependencies(menu_track_test menu_track_fixture raven-bootstrap)
            add_test(NAME native_menu_submenu_click COMMAND menu_track_test $<TARGET_FILE:raven-bootstrap> ${MENU_TRACK_FIXTURE})
            set_tests_properties(native_menu_submenu_click PROPERTIES TIMEOUT 15)
            add_executable(outer_surface_test desktop/outer_surface_test.cpp)
            target_link_libraries(outer_surface_test PRIVATE raven_cpu)
            add_dependencies(outer_surface_test outer_surface_visible_fixture raven-bootstrap)
            add_test(NAME native_outer_surface_visible COMMAND outer_surface_test $<TARGET_FILE:raven-bootstrap> ${OUTER_SURFACE_VISIBLE})
            set_tests_properties(native_outer_surface_visible PROPERTIES TIMEOUT 15)
            set(OUTER_SURFACE_INPUT ${CMAKE_CURRENT_BINARY_DIR}/outer-surface-input.exe)
            add_custom_command(OUTPUT ${OUTER_SURFACE_INPUT}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -DVISIBLE_SURFACE -DINPUT_SURFACE
                    -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                    -o ${OUTER_SURFACE_INPUT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/outer_surface_client.c -lkernel32 -luser32 -lgdi32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/outer_surface_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/icon_data.h VERBATIM)
            add_custom_target(outer_surface_input_fixture ALL DEPENDS ${OUTER_SURFACE_INPUT})
            add_dependencies(outer_surface_test outer_surface_input_fixture)
            add_test(NAME native_outer_surface_input COMMAND outer_surface_test $<TARGET_FILE:raven-bootstrap> ${OUTER_SURFACE_INPUT} input)
            set_tests_properties(native_outer_surface_input PROPERTIES TIMEOUT 15)
            set(OUTER_SURFACE_PAINT ${CMAKE_CURRENT_BINARY_DIR}/outer-surface-paint.exe)
            add_custom_command(OUTPUT ${OUTER_SURFACE_PAINT}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -DVISIBLE_SURFACE -DPAINT_SURFACE
                    -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                    -o ${OUTER_SURFACE_PAINT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/outer_surface_client.c -lkernel32 -luser32 -lgdi32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/outer_surface_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/icon_data.h VERBATIM)
            add_custom_target(outer_surface_paint_fixture ALL DEPENDS ${OUTER_SURFACE_PAINT})
            add_dependencies(outer_surface_test outer_surface_paint_fixture)
            add_test(NAME native_outer_surface_paint COMMAND outer_surface_test $<TARGET_FILE:raven-bootstrap> ${OUTER_SURFACE_PAINT} capture ${CMAKE_CURRENT_BINARY_DIR}/outer-surface-paint.bgra)
            set_tests_properties(native_outer_surface_paint PROPERTIES TIMEOUT 15)
            set(FRAME_HOVER_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/frame-hover.exe)
            add_custom_command(OUTPUT ${FRAME_HOVER_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                    -o ${FRAME_HOVER_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/frame_hover_client.c -lkernel32 -luser32 -lgdi32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/frame_hover_client.c VERBATIM)
            add_custom_target(frame_hover_fixture ALL DEPENDS ${FRAME_HOVER_FIXTURE})
            add_executable(frame_hover_test desktop/frame_hover_test.cpp)
            target_link_libraries(frame_hover_test PRIVATE raven_cpu)
            add_dependencies(frame_hover_test frame_hover_fixture raven-bootstrap)
            add_test(NAME native_frame_hover COMMAND frame_hover_test $<TARGET_FILE:raven-bootstrap> ${FRAME_HOVER_FIXTURE})
            set_tests_properties(native_frame_hover PROPERTIES TIMEOUT 15)
            set(FRAME_RESTORE_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/frame-restore.exe)
            add_custom_command(OUTPUT ${FRAME_RESTORE_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                    -o ${FRAME_RESTORE_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/frame_restore_client.c -lkernel32 -luser32 -lgdi32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/frame_restore_client.c VERBATIM)
            add_custom_target(frame_restore_fixture ALL DEPENDS ${FRAME_RESTORE_FIXTURE})
            add_executable(frame_restore_test desktop/frame_restore_test.cpp)
            target_link_libraries(frame_restore_test PRIVATE raven_cpu)
            add_dependencies(frame_restore_test frame_restore_fixture raven-bootstrap)
            add_test(NAME native_frame_restore COMMAND frame_restore_test $<TARGET_FILE:raven-bootstrap> ${FRAME_RESTORE_FIXTURE})
            set_tests_properties(native_frame_restore PROPERTIES TIMEOUT 30)
        endif()
        set(INITIAL_MESSAGE_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/initial-message-fixtures)
        add_custom_command(OUTPUT ${INITIAL_MESSAGE_FIXTURES}/client.exe ${INITIAL_MESSAGE_FIXTURES}/exit.exe
            COMMAND ${CMAKE_COMMAND} -E make_directory ${INITIAL_MESSAGE_FIXTURES}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x1a4000000
                -o ${INITIAL_MESSAGE_FIXTURES}/client.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/initial_messages_client.c -lkernel32 -luser32
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -DEXIT_DURING_SIZE
                -Wl,--entry,raven_entry,--image-base,0x1a4000000
                -o ${INITIAL_MESSAGE_FIXTURES}/exit.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/initial_messages_client.c -lkernel32 -luser32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/initial_messages_client.c VERBATIM)
        add_custom_target(initial_message_pe_fixtures ALL DEPENDS ${INITIAL_MESSAGE_FIXTURES}/client.exe ${INITIAL_MESSAGE_FIXTURES}/exit.exe)
        add_executable(initial_messages_test desktop/initial_messages_test.cpp)
        target_link_libraries(initial_messages_test PRIVATE raven_cpu)
        add_dependencies(initial_messages_test initial_message_pe_fixtures raven-bootstrap)
        add_test(NAME native_initial_messages_contract COMMAND initial_messages_test $<TARGET_FILE:raven-bootstrap> ${INITIAL_MESSAGE_FIXTURES})
        set_tests_properties(native_initial_messages_contract PROPERTIES TIMEOUT 15)
        add_test(NAME native_initial_messages_cli COMMAND raven --run-process --execution=native ${INITIAL_MESSAGE_FIXTURES}/client.exe)
        set_tests_properties(native_initial_messages_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(COORDINATE_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/coordinate-fixture.exe)
        add_custom_command(OUTPUT ${COORDINATE_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x1a4000000
                -o ${COORDINATE_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/coordinate_client.c -lkernel32 -luser32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/coordinate_client.c VERBATIM)
        add_custom_target(coordinate_pe_fixture ALL DEPENDS ${COORDINATE_FIXTURE})
        add_executable(coordinates_test desktop/coordinates_test.cpp)
        target_link_libraries(coordinates_test PRIVATE raven_cpu)
        add_dependencies(coordinates_test coordinate_pe_fixture raven-bootstrap)
        add_test(NAME native_coordinate_contract COMMAND coordinates_test $<TARGET_FILE:raven-bootstrap> ${COORDINATE_FIXTURE})
        set_tests_properties(native_coordinate_contract PROPERTIES TIMEOUT 15)
        add_test(NAME native_coordinate_cli COMMAND raven --run-process --execution=native ${COORDINATE_FIXTURE})
        set_tests_properties(native_coordinate_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        add_test(NAME native_window_cli COMMAND raven --run-process --execution=native ${WINDOW_FIXTURE})
        set_tests_properties(native_window_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(PAINT_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/paint-fixture.exe)
        add_custom_command(OUTPUT ${PAINT_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -nostdlib
                -Wl,--entry,raven_entry,--image-base,0x1a5000000,--no-insert-timestamp
                -o ${PAINT_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/paint_client.c -lkernel32 -luser32 -lgdi32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/paint_client.c VERBATIM)
        add_custom_target(paint_pe_fixture ALL DEPENDS ${PAINT_FIXTURE})
        set(CLOSE_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/close-fixture.exe)
        add_custom_command(OUTPUT ${CLOSE_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -nostdlib -DRAVEN_CLOSE_LOOP
                -Wl,--entry,raven_entry,--image-base,0x1a5000000,--no-insert-timestamp
                -o ${CLOSE_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/paint_client.c -lkernel32 -luser32 -lgdi32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/paint_client.c VERBATIM)
        add_custom_target(close_pe_fixture ALL DEPENDS ${CLOSE_FIXTURE})
        set(POINTER_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/pointer-fixture.exe)
        add_custom_command(OUTPUT ${POINTER_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -nostdlib -DRAVEN_CLOSE_LOOP -DRAVEN_POINTER_LOOP
                -Wl,--entry,raven_entry,--image-base,0x1a5000000,--no-insert-timestamp
                -o ${POINTER_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/paint_client.c -lkernel32 -luser32 -lgdi32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/paint_client.c VERBATIM)
        add_custom_target(pointer_pe_fixture ALL DEPENDS ${POINTER_FIXTURE})
        set(KEYBOARD_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/keyboard-fixture.exe)
        add_custom_command(OUTPUT ${KEYBOARD_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -nostdlib -DRAVEN_CLOSE_LOOP -DRAVEN_KEYBOARD_LOOP
                -Wl,--entry,raven_entry,--image-base,0x1a5000000,--no-insert-timestamp
                -o ${KEYBOARD_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/paint_client.c -lkernel32 -luser32 -lgdi32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/paint_client.c VERBATIM)
        add_custom_target(keyboard_pe_fixture ALL DEPENDS ${KEYBOARD_FIXTURE})
        set(REPAINT_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/repaint-fixture.exe)
        add_custom_command(OUTPUT ${REPAINT_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -nostdlib -DRAVEN_CLOSE_LOOP -DRAVEN_REPAINT_LOOP
                -Wl,--entry,raven_entry,--image-base,0x1a5000000,--no-insert-timestamp
                -o ${REPAINT_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/paint_client.c -lkernel32 -luser32 -lgdi32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/paint_client.c VERBATIM)
        add_custom_target(repaint_pe_fixture ALL DEPENDS ${REPAINT_FIXTURE})
        if(PIXMAN_FOUND)
            add_executable(repaint_test desktop/repaint_test.cpp)
            target_link_libraries(repaint_test PRIVATE raven_cpu)
            add_dependencies(repaint_test repaint_pe_fixture raven-bootstrap)
            add_test(NAME native_repaint_client COMMAND repaint_test $<TARGET_FILE:raven-bootstrap> ${REPAINT_FIXTURE})
            set_tests_properties(native_repaint_client PROPERTIES TIMEOUT 15)
        endif()
        if(XKBCOMMON_FOUND)
            add_executable(keyboard_client_test desktop/keyboard_client_test.cpp)
            target_link_libraries(keyboard_client_test PRIVATE raven_cpu)
            set(FOCUS_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/keyboard-focus.exe)
            add_custom_command(OUTPUT ${FOCUS_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -nostdlib -DRAVEN_CLOSE_LOOP -DRAVEN_KEYBOARD_LOOP -DRAVEN_FOCUS_LOOP
                    -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                    -o ${FOCUS_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/paint_client.c -lkernel32 -luser32 -lgdi32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/paint_client.c VERBATIM)
            add_custom_target(keyboard_focus_pe_fixture ALL DEPENDS ${FOCUS_FIXTURE})
            add_dependencies(keyboard_client_test keyboard_pe_fixture keyboard_focus_pe_fixture raven-bootstrap)
            foreach(FOCUS_CASE held duplicate outofrange nofocus)
                add_test(NAME native_keyboard_focus_${FOCUS_CASE} COMMAND keyboard_client_test $<TARGET_FILE:raven-bootstrap> ${FOCUS_FIXTURE} ${FOCUS_CASE})
                set_tests_properties(native_keyboard_focus_${FOCUS_CASE} PROPERTIES TIMEOUT 15)
            endforeach()
            add_test(NAME native_keyboard_client COMMAND keyboard_client_test $<TARGET_FILE:raven-bootstrap> ${KEYBOARD_FIXTURE})
            set_tests_properties(native_keyboard_client PROPERTIES TIMEOUT 15)
            add_test(NAME native_keyboard_remap COMMAND keyboard_client_test $<TARGET_FILE:raven-bootstrap> ${KEYBOARD_FIXTURE} remap)
            set_tests_properties(native_keyboard_remap PROPERTIES TIMEOUT 15)
            add_executable(wheel_target_test desktop/wheel_target_test.cpp)
            target_link_libraries(wheel_target_test PRIVATE raven_cpu)
            set(WHEEL_TARGET_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/wheel-target.exe)
            add_custom_command(OUTPUT ${WHEEL_TARGET_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                    -o ${WHEEL_TARGET_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/wheel_target_client.c -lkernel32 -luser32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/wheel_target_client.c VERBATIM)
            add_custom_target(wheel_target_fixture ALL DEPENDS ${WHEEL_TARGET_FIXTURE})
            add_dependencies(wheel_target_test wheel_target_fixture raven-bootstrap)
            add_test(NAME native_wheel_target COMMAND wheel_target_test $<TARGET_FILE:raven-bootstrap> ${WHEEL_TARGET_FIXTURE})
            set_tests_properties(native_wheel_target PROPERTIES TIMEOUT 15)
            add_executable(input_thread_test desktop/input_thread_test.cpp)
            target_link_libraries(input_thread_test PRIVATE raven_cpu)
            set(INPUT_THREAD_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/input-thread.exe)
            add_custom_command(OUTPUT ${INPUT_THREAD_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                    -o ${INPUT_THREAD_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/input_thread_client.c -lkernel32 -luser32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/input_thread_client.c VERBATIM)
            add_custom_target(input_thread_fixture ALL DEPENDS ${INPUT_THREAD_FIXTURE})
            add_dependencies(input_thread_test input_thread_fixture raven-bootstrap)
            add_test(NAME native_input_thread COMMAND input_thread_test $<TARGET_FILE:raven-bootstrap> ${INPUT_THREAD_FIXTURE})
            set_tests_properties(native_input_thread PROPERTIES TIMEOUT 20)
            add_executable(modifier_keys_test desktop/modifier_keys_test.cpp)
            target_link_libraries(modifier_keys_test PRIVATE raven_cpu)
            set(MODIFIER_KEYS_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/modifier-keys.exe)
            add_custom_command(OUTPUT ${MODIFIER_KEYS_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1a4000000,--no-insert-timestamp
                    -o ${MODIFIER_KEYS_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/modifier_keys_client.c -lkernel32 -luser32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/modifier_keys_client.c VERBATIM)
            add_custom_target(modifier_keys_fixture ALL DEPENDS ${MODIFIER_KEYS_FIXTURE})
            add_dependencies(modifier_keys_test modifier_keys_fixture raven-bootstrap)
            add_test(NAME native_modifier_keys COMMAND modifier_keys_test $<TARGET_FILE:raven-bootstrap> ${MODIFIER_KEYS_FIXTURE})
            set_tests_properties(native_modifier_keys PROPERTIES TIMEOUT 15)
        endif()
        set(BITBLT_PAINT ${CMAKE_CURRENT_BINARY_DIR}/bitblt-paint.exe)
        add_custom_command(OUTPUT ${BITBLT_PAINT}
            COMMAND ${RAVEN_MINGW64} -nostdlib -DRAVEN_BITBLT_PAINT
                -Wl,--entry,raven_entry,--image-base,0x1b9000000,--no-insert-timestamp
                -o ${BITBLT_PAINT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/paint_client.c -lkernel32 -luser32 -lgdi32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/paint_client.c VERBATIM)
        add_custom_target(bitblt_paint_fixture ALL DEPENDS ${BITBLT_PAINT})
        set(COMPATIBLE_PAINT ${CMAKE_CURRENT_BINARY_DIR}/compatible-bitmap-paint.exe)
        add_custom_command(OUTPUT ${COMPATIBLE_PAINT}
            COMMAND ${RAVEN_MINGW64} -nostdlib -DRAVEN_BITBLT_PAINT -DRAVEN_COMPATIBLE_PAINT
                -Wl,--entry,raven_entry,--image-base,0x1bb000000,--no-insert-timestamp
                -o ${COMPATIBLE_PAINT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/paint_client.c -lkernel32 -luser32 -lgdi32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/paint_client.c VERBATIM)
        add_custom_target(compatible_bitmap_paint_fixture ALL DEPENDS ${COMPATIBLE_PAINT})
        add_executable(paint_test desktop/paint_test.cpp)
        target_link_libraries(paint_test PRIVATE raven_cpu)
        add_dependencies(paint_test compatible_bitmap_paint_fixture bitblt_paint_fixture paint_pe_fixture close_pe_fixture pointer_pe_fixture raven-bootstrap)
        add_test(NAME native_compatible_bitmap_paint COMMAND paint_test $<TARGET_FILE:raven-bootstrap> ${COMPATIBLE_PAINT} ${CLOSE_FIXTURE} ${POINTER_FIXTURE})
        set_tests_properties(native_compatible_bitmap_paint PROPERTIES TIMEOUT 15)
        add_test(NAME native_bitblt_paint COMMAND paint_test $<TARGET_FILE:raven-bootstrap> ${BITBLT_PAINT} ${CLOSE_FIXTURE} ${POINTER_FIXTURE})
        set_tests_properties(native_bitblt_paint PROPERTIES TIMEOUT 15)
        add_test(NAME native_paint COMMAND paint_test $<TARGET_FILE:raven-bootstrap> ${PAINT_FIXTURE} ${CLOSE_FIXTURE} ${POINTER_FIXTURE})
        set_tests_properties(native_paint PROPERTIES TIMEOUT 15)
        set(MESSAGE_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/message-fixture.exe)
        add_custom_command(OUTPUT ${MESSAGE_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -nostdlib
                -Wl,--entry,raven_entry,--image-base,0x1a6000000,--no-insert-timestamp
                -o ${MESSAGE_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/message_client.c -lkernel32 -luser32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/message_client.c VERBATIM)
        add_custom_target(message_pe_fixture ALL DEPENDS ${MESSAGE_FIXTURE})
        add_test(NAME native_message_cli COMMAND raven --run-process --execution=native ${MESSAGE_FIXTURE})
        set_tests_properties(native_message_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        find_program(RAVEN_MINGW32 i686-w64-mingw32-gcc)
        if(RAVEN_MINGW32)
            list(APPEND RAVEN_MINGW32 -minline-all-stringops)
        endif()
        add_executable(queue_test desktop/queue_test.cpp)
        target_link_libraries(queue_test PRIVATE raven_cpu)
        add_dependencies(queue_test raven-bootstrap)
        add_executable(raw_buffer_test desktop/raw_buffer_test.cpp)
        target_link_libraries(raw_buffer_test PRIVATE raven_cpu)
        add_dependencies(raw_buffer_test raven-bootstrap)
        foreach(QUEUE_ARCH IN ITEMS 64 32)
            set(QUEUE_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/queue-${QUEUE_ARCH}.exe)
            if(QUEUE_ARCH EQUAL 64)
                set(QUEUE_COMPILER ${RAVEN_MINGW64})
                set(QUEUE_ENTRY raven_entry)
            else()
                set(QUEUE_COMPILER ${RAVEN_MINGW32})
                set(QUEUE_ENTRY _raven_entry)
            endif()
            if(QUEUE_COMPILER)
                add_custom_command(OUTPUT ${QUEUE_FIXTURE}
                    COMMAND ${QUEUE_COMPILER} -nostdlib -Wl,--entry,${QUEUE_ENTRY},--no-insert-timestamp
                        -o ${QUEUE_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/queue_client.c -lkernel32 -luser32
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/queue_client.c VERBATIM)
                add_custom_target(queue_${QUEUE_ARCH}_fixture ALL DEPENDS ${QUEUE_FIXTURE})
                add_dependencies(queue_test queue_${QUEUE_ARCH}_fixture)
                add_test(NAME native_queue_x${QUEUE_ARCH} COMMAND queue_test $<TARGET_FILE:raven-bootstrap> ${QUEUE_FIXTURE})
                set_tests_properties(native_queue_x${QUEUE_ARCH} PROPERTIES TIMEOUT 15)
                add_test(NAME native_queue_x${QUEUE_ARCH}_parallel COMMAND queue_test $<TARGET_FILE:raven-bootstrap> ${QUEUE_FIXTURE})
                set_tests_properties(native_queue_x${QUEUE_ARCH}_parallel PROPERTIES TIMEOUT 15 ENVIRONMENT RAVEN_PARALLEL=1)
                set(RAW_BUFFER_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/raw-buffer-${QUEUE_ARCH}.exe)
                add_custom_command(OUTPUT ${RAW_BUFFER_FIXTURE}
                    COMMAND ${QUEUE_COMPILER} -nostdlib -O1 -fno-builtin -fno-stack-protector
                        -Wl,--entry,${QUEUE_ENTRY},--no-insert-timestamp
                        -o ${RAW_BUFFER_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/raw_buffer_client.c -lkernel32 -luser32 -lgcc
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/raw_buffer_client.c VERBATIM)
                add_custom_target(raw_buffer_${QUEUE_ARCH}_fixture ALL DEPENDS ${RAW_BUFFER_FIXTURE})
                add_dependencies(raw_buffer_test raw_buffer_${QUEUE_ARCH}_fixture)
                add_test(NAME native_raw_buffer_x${QUEUE_ARCH} COMMAND raw_buffer_test $<TARGET_FILE:raven-bootstrap> ${RAW_BUFFER_FIXTURE})
                set_tests_properties(native_raw_buffer_x${QUEUE_ARCH} PROPERTIES TIMEOUT 15)
            endif()
        endforeach()
        # SPI_GETHIGHCONTRAST answers both pointer widths without writing past the structure.
        foreach(CONTRAST_ARCH IN ITEMS 64 32)
            set(CONTRAST_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/high-contrast-${CONTRAST_ARCH}.exe)
            if(CONTRAST_ARCH EQUAL 64)
                set(CONTRAST_COMPILER ${RAVEN_MINGW64})
                set(CONTRAST_ENTRY raven_entry)
            else()
                set(CONTRAST_COMPILER ${RAVEN_MINGW32})
                set(CONTRAST_ENTRY _raven_entry)
            endif()
            if(CONTRAST_COMPILER)
                add_custom_command(OUTPUT ${CONTRAST_FIXTURE}
                    COMMAND ${CONTRAST_COMPILER} -nostdlib -O1 -fno-builtin -fno-stack-protector
                        -Wl,--entry,${CONTRAST_ENTRY},--no-insert-timestamp
                        -o ${CONTRAST_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/high_contrast_client.c -luser32 -lkernel32
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/high_contrast_client.c VERBATIM)
                add_custom_target(high_contrast_${CONTRAST_ARCH}_fixture ALL DEPENDS ${CONTRAST_FIXTURE})
                add_test(NAME native_high_contrast_x${CONTRAST_ARCH} COMMAND raven --run-process --execution=native ${CONTRAST_FIXTURE})
                set_tests_properties(native_high_contrast_x${CONTRAST_ARCH} PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            endif()
        endforeach()
        set(DEFAULT_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/default-fixture.exe)
        add_custom_command(OUTPUT ${DEFAULT_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -nostdlib
                -Wl,--entry,raven_entry,--image-base,0x1a8000000,--no-insert-timestamp
                -o ${DEFAULT_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/default_client.c -lkernel32 -luser32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/default_client.c VERBATIM)
        add_custom_target(default_pe_fixture ALL DEPENDS ${DEFAULT_FIXTURE})
        add_test(NAME native_default_cli COMMAND raven --run-process --execution=native ${DEFAULT_FIXTURE})
        set_tests_properties(native_default_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(FOCUS_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/focus-fixture.exe)
        add_custom_command(OUTPUT ${FOCUS_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -nostdlib
                -Wl,--entry,raven_entry,--image-base,0x1a9000000,--no-insert-timestamp
                -o ${FOCUS_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/focus_client.c -lkernel32 -luser32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/focus_client.c VERBATIM)
        add_custom_target(focus_pe_fixture ALL DEPENDS ${FOCUS_FIXTURE})
        add_test(NAME native_focus_cli COMMAND raven --run-process --execution=native ${FOCUS_FIXTURE})
        set_tests_properties(native_focus_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(GEOMETRY_NEGATIVE ${CMAKE_CURRENT_BINARY_DIR}/geometry-negative.exe)
        add_custom_command(OUTPUT ${GEOMETRY_NEGATIVE}
            COMMAND ${RAVEN_MINGW64} -nostdlib
                -Wl,--entry,raven_entry,--image-base,0x1aa000000,--no-insert-timestamp
                -o ${GEOMETRY_NEGATIVE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/geometry_negative.c -lkernel32 -luser32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/geometry_negative.c VERBATIM)
        add_custom_target(geometry_negative_fixture ALL DEPENDS ${GEOMETRY_NEGATIVE})
        add_test(NAME native_geometry_boundary COMMAND raven --run-process --execution=native ${GEOMETRY_NEGATIVE})
        set_tests_properties(native_geometry_boundary PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        # The code signing roots Windows trusts (scripts/provision-roots.py), beside the runtime in roots/: signatures of installers and
        # their packages are checked against them.
        set(RAVEN_ROOT_CERTIFICATE_DIRECTORY "" CACHE PATH "Directory holding the root certificates downloaded by scripts/provision-roots.py")
        if(RAVEN_ROOT_CERTIFICATE_DIRECTORY)
            file(GLOB root_certificates ${RAVEN_ROOT_CERTIFICATE_DIRECTORY}/*.crt)
            set(root_copies)
            foreach(certificate ${root_certificates})
                get_filename_component(certificate_name ${certificate} NAME)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/roots/${certificate_name}
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/roots
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${certificate} ${CMAKE_CURRENT_BINARY_DIR}/roots/${certificate_name}
                    DEPENDS ${certificate} VERBATIM)
                list(APPEND root_copies ${CMAKE_CURRENT_BINARY_DIR}/roots/${certificate_name})
            endforeach()
            add_custom_target(root_certificates ALL DEPENDS ${root_copies})
        else()
            message(WARNING "RAVEN_ROOT_CERTIFICATE_DIRECTORY is not set: installers that check the signatures of Microsoft packages fail with 0x800b010a. Run scripts/provision-roots.py and configure again with its output directory.")
        endif()
        # The Windows Installer components (scripts/provision-installer.py): msi, cabinet, sxs and structured storage, for each program width.
        set(RAVEN_INSTALLER_COMPONENT_DIR "" CACHE PATH "Directory holding the installer components built by scripts/provision-installer.py")
        set(RAVEN_INSTALLER32_COMPONENT_DIR "" CACHE PATH "Directory holding the 32-bit installer components built by scripts/provision-installer.py --arch x86")
        find_program(RAVEN_WIXL wixl)
        find_program(RAVEN_MSIBUILD msibuild)
        find_program(RAVEN_MINGW32_INSTALLER i686-w64-mingw32-gcc)
        if(RAVEN_MINGW32_INSTALLER)
            list(APPEND RAVEN_MINGW32_INSTALLER -minline-all-stringops)
        endif()
        set(MSI_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/msi-fixtures)
        foreach(width 64 32)
            if(width STREQUAL 64)
                set(installer_dir ${RAVEN_INSTALLER_COMPONENT_DIR})
                set(installer_compiler ${RAVEN_MINGW64})
                set(installer_entry raven_entry)
                set(installer_arch x64)
            else()
                set(installer_dir ${RAVEN_INSTALLER32_COMPONENT_DIR})
                set(installer_compiler ${RAVEN_MINGW32_INSTALLER})
                set(installer_entry _raven_entry)
                set(installer_arch x86)
            endif()
            if(NOT installer_dir)
                continue()
            endif()
            # The components are copied whenever they change, whether or not the runtime itself was rebuilt.
            set(installer_copies)
            foreach(component msi.dll cabinet.dll sxs.dll rpcrt4.dll raven-storage.dll msiexec.exe msxml3.dll atl100.dll oleaut32.dll
                libjpeg.dll libtiff.dll propsys.dll mlang.dll windowscodecs.dll gdiplus.dll urlmon.dll riched20.dll msftedit.dll msvfw32.dll advpack.dll raven-setupapi.dll xmllite.dll
                raven-appx.dll mspatcha.dll vbscript.dll scrrun.dll wshom.ocx jscript.dll)
                if(NOT EXISTS "${installer_dir}/${component}")
                    message(FATAL_ERROR "Configured installer component does not exist: ${installer_dir}/${component}")
                endif()
                if(width STREQUAL 64)
                    set(copy ${CMAKE_CURRENT_BINARY_DIR}/${component})
                else()
                    set(copy ${CMAKE_CURRENT_BINARY_DIR}/x86/${component})
                endif()
                get_filename_component(copy_directory ${copy} DIRECTORY)
                add_custom_command(OUTPUT ${copy}
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${copy_directory}
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${installer_dir}/${component} ${copy}
                    DEPENDS ${installer_dir}/${component} VERBATIM)
                list(APPEND installer_copies ${copy})
            endforeach()
            add_custom_target(installer_components_${width} ALL DEPENDS ${installer_copies})
            # The packaging API on packages appx_fixtures.py writes: the manifest, payload files and block map of a package, its ZIP64 form,
            # a package and a bundle told apart, a part the block map leaves out, and content that differs from the block map.
            find_package(Python3 COMPONENTS Interpreter QUIET)
            if(Python3_Interpreter_FOUND AND installer_compiler)
                set(appx_fixture ${CMAKE_CURRENT_BINARY_DIR}/appx-fixtures/${installer_arch})
                add_custom_command(OUTPUT ${appx_fixture}/appx-client.exe ${appx_fixture}/appx-good.msix
                    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/appx_fixtures.py ${appx_fixture}
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -I${PROJECT_SOURCE_DIR}/native/guest/raven-appx
                        -Wl,--entry,${installer_entry},--no-insert-timestamp -o ${appx_fixture}/appx-client.exe
                        ${PROJECT_SOURCE_DIR}/testdata/native_loader/appx_client.c -lole32 -lshlwapi -lkernel32 -luuid
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/appx_fixtures.py ${PROJECT_SOURCE_DIR}/testdata/native_loader/appx_client.c
                        ${PROJECT_SOURCE_DIR}/native/guest/raven-appx/appxpackaging.h VERBATIM)
                add_custom_target(appx_fixture_${installer_arch} ALL DEPENDS ${appx_fixture}/appx-client.exe ${appx_fixture}/appx-good.msix)
                add_test(NAME native_appx_packaging_${installer_arch} COMMAND raven --run-process --execution=native ${appx_fixture}/appx-client.exe)
                set_tests_properties(native_appx_packaging_${installer_arch} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                # Delta patches mspatcha_fixtures.py writes, applied through the mspatcha exports by name, handle and buffer.
                set(mspatcha_fixture ${CMAKE_CURRENT_BINARY_DIR}/mspatcha-fixtures/${installer_arch})
                add_custom_command(OUTPUT ${mspatcha_fixture}/mspatcha-client.exe ${mspatcha_fixture}/delta.pa19
                    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/mspatcha_fixtures.py ${mspatcha_fixture}
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin
                        -Wl,--entry,${installer_entry},--no-insert-timestamp -o ${mspatcha_fixture}/mspatcha-client.exe
                        ${PROJECT_SOURCE_DIR}/testdata/native_loader/mspatcha_client.c -lkernel32
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/mspatcha_fixtures.py ${PROJECT_SOURCE_DIR}/testdata/native_loader/mspatcha_client.c
                    VERBATIM)
                add_custom_target(mspatcha_fixture_${installer_arch} ALL DEPENDS ${mspatcha_fixture}/mspatcha-client.exe ${mspatcha_fixture}/delta.pa19)
                add_test(NAME native_mspatcha_${installer_arch} COMMAND raven --run-process --execution=native ${mspatcha_fixture}/mspatcha-client.exe)
                set_tests_properties(native_mspatcha_${installer_arch} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                # The VBScript engine as a script host drives it: vbscript_tests.vbs through a site with a named item, then an expression, the
                # script's globals through IDispatch and the errors the site is told of.
                set(vbscript_fixture ${CMAKE_CURRENT_BINARY_DIR}/vbscript-fixtures/${installer_arch})
                add_custom_command(OUTPUT ${vbscript_fixture}/vbscript-client.exe ${vbscript_fixture}/vbscript_tests.vbs
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${vbscript_fixture}
                    COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/testdata/native_loader/vbscript_tests.vbs ${vbscript_fixture}/vbscript_tests.vbs
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin
                        -Wl,--entry,${installer_entry},--no-insert-timestamp -o ${vbscript_fixture}/vbscript-client.exe
                        ${PROJECT_SOURCE_DIR}/testdata/native_loader/vbscript_client.c -lole32 -loleaut32 -luuid -luser32 -lmsvcrt -lkernel32
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/vbscript_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/vbscript_tests.vbs
                    VERBATIM)
                add_custom_target(vbscript_fixture_${installer_arch} ALL DEPENDS ${vbscript_fixture}/vbscript-client.exe ${vbscript_fixture}/vbscript_tests.vbs)
                add_test(NAME native_vbscript_${installer_arch} COMMAND raven --run-process --execution=native ${vbscript_fixture}/vbscript-client.exe)
                set_tests_properties(native_vbscript_${installer_arch} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                # The script host runs scrrun_tests.vbs: the WScript object, the Dictionary and the file system through the scripting runtime;
                # then wshom_tests.vbs: the shell and network objects. Each prints how many checks passed, or which failed.
                if(installer_arch STREQUAL "x86")
                    set(script_host ${CMAKE_CURRENT_BINARY_DIR}/x86/cscript.exe)
                else()
                    set(script_host ${CMAKE_CURRENT_BINARY_DIR}/cscript.exe)
                endif()
                add_custom_command(OUTPUT ${vbscript_fixture}/scrrun_tests.vbs
                    COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/testdata/native_loader/scrrun_tests.vbs ${vbscript_fixture}/scrrun_tests.vbs
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/scrrun_tests.vbs VERBATIM)
                add_custom_target(scrrun_fixture_${installer_arch} ALL DEPENDS ${vbscript_fixture}/scrrun_tests.vbs)
                add_test(NAME native_cscript_${installer_arch} COMMAND raven --run-process --execution=native --stdio=inherit ${script_host} --
                    //nologo Z:${vbscript_fixture}/scrrun_tests.vbs first /named:value "two words")
                set_tests_properties(native_cscript_${installer_arch} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "out line\r?\necho 1 True\r?\n[0-9]+ checks passed" FAIL_REGULAR_EXPRESSION "FAIL|failed")
                add_custom_command(OUTPUT ${vbscript_fixture}/wshom_tests.vbs
                    COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/testdata/native_loader/wshom_tests.vbs ${vbscript_fixture}/wshom_tests.vbs
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/wshom_tests.vbs VERBATIM)
                add_custom_target(wshom_fixture_${installer_arch} ALL DEPENDS ${vbscript_fixture}/wshom_tests.vbs)
                add_test(NAME native_wshom_${installer_arch} COMMAND raven --run-process --execution=native --stdio=inherit ${script_host} --
                    //nologo Z:${vbscript_fixture}/wshom_tests.vbs)
                set_tests_properties(native_wshom_${installer_arch} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "[0-9]+ checks passed" FAIL_REGULAR_EXPRESSION "FAIL|failed")
                # The JScript engine under the same host: the language, its built-in objects and the host's objects.
                add_custom_command(OUTPUT ${vbscript_fixture}/jscript_tests.js
                    COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/testdata/native_loader/jscript_tests.js ${vbscript_fixture}/jscript_tests.js
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/jscript_tests.js VERBATIM)
                add_custom_target(jscript_fixture_${installer_arch} ALL DEPENDS ${vbscript_fixture}/jscript_tests.js)
                add_test(NAME native_jscript_${installer_arch} COMMAND raven --run-process --execution=native --stdio=inherit ${script_host} --
                    //nologo Z:${vbscript_fixture}/jscript_tests.js)
                set_tests_properties(native_jscript_${installer_arch} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "[0-9]+ checks passed" FAIL_REGULAR_EXPRESSION "FAIL|failed")
            endif()
            # A package made by wixl, read back as a database: tables, SQL views and the summary information stream; then installed.
            if(RAVEN_WIXL AND installer_compiler)
                set(fixture ${MSI_FIXTURES}/${installer_arch})
                add_custom_command(OUTPUT ${fixture}/basic.msi ${fixture}/msi-database.exe ${fixture}/com-marshal.exe ${fixture}/local-server.exe ${fixture}/typelib-marshal.exe ${fixture}/msi-install.exe ${fixture}/msi-patch.exe ${fixture}/msiexec-client.exe
                    ${fixture}/clicktorun/msi-clicktorun.exe ${fixture}/msi-ui.exe
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${fixture}
                    COMMAND ${RAVEN_WIXL} -a ${installer_arch} -o ${fixture}/basic.msi msi_basic.wxs
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${installer_entry},--no-insert-timestamp
                        -o ${fixture}/msi-database.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_database_client.c -lmsi -lkernel32
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${installer_entry},--no-insert-timestamp
                        -o ${fixture}/com-marshal.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/com_marshal_client.c -lole32 -luuid -luser32 -lkernel32
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${installer_entry},--no-insert-timestamp
                        -o ${fixture}/local-server.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/local_server_client.c -lole32 -luuid -luser32 -ladvapi32 -lkernel32
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${installer_entry},--no-insert-timestamp
                        -o ${fixture}/typelib-marshal.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/typelib_marshal_client.c -loleaut32 -lole32 -luuid -luser32 -ladvapi32 -lkernel32
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${installer_entry},--no-insert-timestamp
                        -o ${fixture}/msi-install.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_install_client.c -lmsi -ladvapi32 -lkernel32
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe -Wl,--entry,${installer_entry},--no-insert-timestamp
                        -o ${fixture}/msi-patch.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_patch_client.c -lmsi -lole32 -ladvapi32 -lkernel32
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${installer_entry},--no-insert-timestamp
                        -o ${fixture}/msiexec-client.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/msiexec_client.c -lmsi -lkernel32
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${installer_entry},--no-insert-timestamp
                        -o ${fixture}/msi-ui.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_ui_client.c -lmsi -luser32 -ladvapi32 -lkernel32
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${fixture}/clicktorun
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${installer_entry},--no-insert-timestamp
                        -o ${fixture}/clicktorun/msi-clicktorun.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_clicktorun_client.c -lmsi -ladvapi32 -lkernel32
                    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/testdata/native_loader
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/com_marshal_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/local_server_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/typelib_marshal_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_basic.wxs ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_payload.txt ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_database_client.c
                        ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_install_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_patch_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/msiexec_client.c
                        ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_clicktorun_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_ui_client.c VERBATIM)
                add_custom_target(msi_database_fixture_${installer_arch} ALL DEPENDS ${fixture}/basic.msi ${fixture}/msi-database.exe ${fixture}/local-server.exe ${fixture}/typelib-marshal.exe ${fixture}/msi-install.exe ${fixture}/msi-patch.exe
                    ${fixture}/msiexec-client.exe ${fixture}/clicktorun/msi-clicktorun.exe ${fixture}/msi-ui.exe)
                # A Click-to-Run product known only from its package manifest: a component's product and the files it publishes by qualifier.
                add_test(NAME native_msi_clicktorun_${installer_arch} COMMAND raven --run-process --execution=native --timeout-ms=60000
                    ${fixture}/clicktorun/msi-clicktorun.exe)
                set_tests_properties(native_msi_clicktorun_${installer_arch} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                    ENVIRONMENT RAVEN_PREFIX=${fixture}/clicktorun-prefix)
                add_test(NAME native_msi_database_${installer_arch} COMMAND raven --run-process --execution=native --timeout-ms=60000 ${fixture}/msi-database.exe)
                set_tests_properties(native_msi_database_${installer_arch} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                    ENVIRONMENT RAVEN_PREFIX=${fixture}/prefix)
                # The same package installed without a user interface and removed again: files, registry, product registration.
                add_test(NAME native_com_marshal_${installer_arch} COMMAND raven --run-process --execution=native --timeout-ms=60000 ${fixture}/com-marshal.exe)
                set_tests_properties(native_com_marshal_${installer_arch} PROPERTIES TIMEOUT 90 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                # A class served by another program, started from its LocalServer32 command and reached through its registered class object.
                add_test(NAME native_com_local_server_${installer_arch} COMMAND raven --run-process --execution=native --timeout-ms=60000 ${fixture}/local-server.exe)
                set_tests_properties(native_com_local_server_${installer_arch} PROPERTIES TIMEOUT 90 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                # An interface only a type library describes, called across processes with strings, variants, arrays and a callback.
                add_test(NAME native_com_typelib_marshal_${installer_arch} COMMAND raven --run-process --execution=native --timeout-ms=60000 ${fixture}/typelib-marshal.exe)
                set_tests_properties(native_com_typelib_marshal_${installer_arch} PROPERTIES TIMEOUT 90 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                # Maintenance: a patch built on the spot, applied, kept and removed; searches, features, source lists, an administrative image.
                add_test(NAME native_msi_patch_${installer_arch} COMMAND raven --run-process --execution=native --timeout-ms=120000 ${fixture}/msi-patch.exe)
                set_tests_properties(native_msi_patch_${installer_arch} PROPERTIES TIMEOUT 150 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                    ENVIRONMENT RAVEN_PREFIX=${fixture}/patch-prefix)
                add_test(NAME native_msi_install_${installer_arch} COMMAND raven --run-process --execution=native --timeout-ms=60000 ${fixture}/msi-install.exe)
                set_tests_properties(native_msi_install_${installer_arch} PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                    ENVIRONMENT RAVEN_PREFIX=${fixture}/install-prefix)
                # The same package with its own dialogs at the full user interface level: another thread cancels the wizard once, then
                # goes through it, and what it chose is what the package installs.
                add_test(NAME native_msi_ui_${installer_arch} COMMAND raven --run-process --execution=native --timeout-ms=60000 ${fixture}/msi-ui.exe)
                set_tests_properties(native_msi_ui_${installer_arch} PROPERTIES TIMEOUT 90 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                    ENVIRONMENT RAVEN_PREFIX=${fixture}/ui-prefix)
                # DLL custom actions run in the custom action server of the package's width: an immediate one records a property, a deferred
                # one fails on request and the installer rolls the installation back.
                if(installer_arch STREQUAL x86)
                    set(action_link -Wl,--entry,_DllMain@12,--kill-at,--no-insert-timestamp)
                else()
                    set(action_link -Wl,--entry,DllMain,--no-insert-timestamp)
                endif()
                file(MAKE_DIRECTORY ${fixture}/custom)
                add_custom_command(OUTPUT ${fixture}/custom/custom.msi ${fixture}/custom/msi-custom.exe
                    COMMAND ${installer_compiler} -O2 -nostdlib -shared -fno-stack-protector ${action_link}
                        -o ${fixture}/custom/msi_action.dll ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_action.c -lmsi -ladvapi32 -lkernel32
                    COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_payload.txt ${fixture}/custom/msi_payload.txt
                    COMMAND ${RAVEN_WIXL} -a ${installer_arch} -o ${fixture}/custom/custom.msi ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_custom.wxs
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${installer_entry},--no-insert-timestamp
                        -o ${fixture}/custom/msi-custom.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_custom_client.c -lmsi -ladvapi32 -lkernel32
                    WORKING_DIRECTORY ${fixture}/custom
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_action.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_custom.wxs
                        ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_custom_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_payload.txt VERBATIM)
                add_custom_target(msi_custom_fixture_${installer_arch} ALL DEPENDS ${fixture}/custom/custom.msi ${fixture}/custom/msi-custom.exe)
                add_test(NAME native_msi_custom_actions_${installer_arch} COMMAND raven --run-process --execution=native --timeout-ms=120000
                    ${fixture}/custom/msi-custom.exe)
                set_tests_properties(native_msi_custom_actions_${installer_arch} PROPERTIES TIMEOUT 300 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                    ENVIRONMENT RAVEN_PREFIX=${fixture}/custom-prefix)
                # The same actions once process ids take six hex digits, as on a desktop that has run for a while: the custom action server
                # finds the pipe the installer names after its process id. Skipped where no process id namespace of our own is allowed.
                add_test(NAME native_msi_custom_actions_large_pid_${installer_arch} COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/scripts/native-msi-large-pid.py $<TARGET_FILE:raven> ${fixture}/custom/msi-custom.exe)
                set_tests_properties(native_msi_custom_actions_large_pid_${installer_arch} PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
                    PASS_REGULAR_EXPRESSION "PASS: " ENVIRONMENT RAVEN_PREFIX=${fixture}/large-pid-prefix)
                # A 64-bit package whose action library is 32-bit, as many real ones are: the action runs in the 32-bit custom action server.
                if(installer_arch STREQUAL x64 AND RAVEN_MINGW32_INSTALLER AND RAVEN_INSTALLER32_COMPONENT_DIR)
                    file(MAKE_DIRECTORY ${fixture}/wow64)
                    add_custom_command(OUTPUT ${fixture}/wow64/custom.msi ${fixture}/wow64/msi-custom.exe
                        COMMAND ${RAVEN_MINGW32_INSTALLER} -O2 -nostdlib -shared -fno-stack-protector -Wl,--entry,_DllMain@12,--kill-at,--no-insert-timestamp
                            -o ${fixture}/wow64/msi_action.dll ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_action.c -lmsi -ladvapi32 -lkernel32
                        COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_payload.txt ${fixture}/wow64/msi_payload.txt
                        COMMAND ${RAVEN_WIXL} -a x64 -o ${fixture}/wow64/custom.msi ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_custom.wxs
                        COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -DACTION_VIEW=KEY_WOW64_32KEY
                            -Wl,--entry,${installer_entry},--no-insert-timestamp -o ${fixture}/wow64/msi-custom.exe
                            ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_custom_client.c -lmsi -ladvapi32 -lkernel32
                        WORKING_DIRECTORY ${fixture}/wow64
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_action.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_custom.wxs
                            ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_custom_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_payload.txt VERBATIM)
                    add_custom_target(msi_wow64_custom_fixture ALL DEPENDS ${fixture}/wow64/custom.msi ${fixture}/wow64/msi-custom.exe)
                    add_test(NAME native_msi_wow64_custom_actions COMMAND raven --run-process --execution=native --timeout-ms=120000
                        ${fixture}/wow64/msi-custom.exe)
                    set_tests_properties(native_msi_wow64_custom_actions PROPERTIES TIMEOUT 300 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                        ENVIRONMENT RAVEN_PREFIX=${fixture}/wow64-prefix)
                endif()
                # VBScript and JScript custom actions. wixl writes no script actions, so msibuild turns its library actions into ones that run a
                # function of the script in the Binary table (6, deferred without impersonation 3078; 5 for JScript) and into ones whose target
                # holds the script itself (38, and 37 for JScript).
                if(RAVEN_MSIBUILD)
                    file(MAKE_DIRECTORY ${fixture}/script)
                    add_custom_command(OUTPUT ${fixture}/script/script.msi ${fixture}/script/msi-script.exe
                        COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_payload.txt ${fixture}/script/msi_payload.txt
                        COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_script.vbs ${fixture}/script/msi_script.vbs
                        COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_script.js ${fixture}/script/msi_script.js
                        COMMAND ${RAVEN_WIXL} -a ${installer_arch} -o ${fixture}/script/script.msi ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_script.wxs
                        COMMAND ${RAVEN_MSIBUILD} ${fixture}/script/script.msi -q [=[UPDATE `CustomAction` SET `Type` = 6 WHERE `Type` = 1]=]
                        COMMAND ${RAVEN_MSIBUILD} ${fixture}/script/script.msi -q [=[UPDATE `CustomAction` SET `Type` = 3078 WHERE `Type` = 3073]=]
                        COMMAND ${RAVEN_MSIBUILD} ${fixture}/script/script.msi -q
                            [=[UPDATE `CustomAction` SET `Type` = 38, `Source` = '', `Target` = 'Session.Property("INLINE_RESULT") = "inline " & (6 * 7)' WHERE `Action` = 'Inline']=]
                        COMMAND ${RAVEN_MSIBUILD} ${fixture}/script/script.msi -q [=[UPDATE `CustomAction` SET `Type` = 5 WHERE `Action` = 'JMain']=]
                        COMMAND ${RAVEN_MSIBUILD} ${fixture}/script/script.msi -q
                            [=[UPDATE `CustomAction` SET `Type` = 37, `Source` = '', `Target` = 'Session.Property("JINLINE_RESULT") = "inline " + (6 * 7)' WHERE `Action` = 'JInline']=]
                        COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${installer_entry},--no-insert-timestamp
                            -o ${fixture}/script/msi-script.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_script_client.c -lmsi -ladvapi32 -lkernel32
                        WORKING_DIRECTORY ${fixture}/script
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_script.wxs ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_script.vbs
                            ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_script.js ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_script_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/msi_payload.txt VERBATIM)
                    add_custom_target(msi_script_fixture_${installer_arch} ALL DEPENDS ${fixture}/script/script.msi ${fixture}/script/msi-script.exe)
                    add_test(NAME native_msi_script_actions_${installer_arch} COMMAND raven --run-process --execution=native --timeout-ms=120000
                        ${fixture}/script/msi-script.exe)
                    set_tests_properties(native_msi_script_actions_${installer_arch} PROPERTIES TIMEOUT 300 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                        ENVIRONMENT RAVEN_PREFIX=${fixture}/script-prefix)
                endif()
                # The XML document object through COM: the prefix registers the component's classes, the program finds and makes one.
                add_custom_command(OUTPUT ${fixture}/msxml.exe
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${installer_entry},--no-insert-timestamp
                        -o ${fixture}/msxml.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/msxml_client.c -lole32 -loleaut32 -luuid -lkernel32
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/msxml_client.c VERBATIM)
                add_custom_target(msxml_fixture_${installer_arch} ALL DEPENDS ${fixture}/msxml.exe)
                add_test(NAME native_msxml_${installer_arch} COMMAND raven --run-process --execution=native --timeout-ms=60000 ${fixture}/msxml.exe)
                set_tests_properties(native_msxml_${installer_arch} PROPERTIES TIMEOUT 300 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                    ENVIRONMENT RAVEN_PREFIX=${fixture}/msxml-prefix)
                # Pictures through the imaging component and GDI+, as bootstrappers load their themes: decoded, converted and made a bitmap.
                add_custom_command(OUTPUT ${fixture}/imaging.exe
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${installer_entry},--no-insert-timestamp
                        -o ${fixture}/imaging.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/imaging_client.c -lwindowscodecs -lgdiplus -lole32 -luuid -lgdi32
                        -lkernel32
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/imaging_client.c VERBATIM)
                add_custom_target(imaging_fixture_${installer_arch} ALL DEPENDS ${fixture}/imaging.exe)
                add_test(NAME native_imaging_${installer_arch} COMMAND raven --run-process --execution=native --timeout-ms=60000 ${fixture}/imaging.exe)
                set_tests_properties(native_imaging_${installer_arch} PROPERTIES TIMEOUT 300 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                    ENVIRONMENT RAVEN_PREFIX=${fixture}/imaging-prefix)
                # Rich edit labels made transparent paint only their text over the parent, and ordinary ones their own background.
                add_custom_command(OUTPUT ${fixture}/richedit.exe
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${installer_entry},--no-insert-timestamp
                        -o ${fixture}/richedit.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/richedit_client.c -luser32 -lgdi32 -lkernel32
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/richedit_client.c VERBATIM)
                add_custom_target(richedit_fixture_${installer_arch} ALL DEPENDS ${fixture}/richedit.exe)
                add_test(NAME native_richedit_${installer_arch} COMMAND raven --run-process --execution=native --timeout-ms=60000 ${fixture}/richedit.exe)
                set_tests_properties(native_richedit_${installer_arch} PROPERTIES TIMEOUT 300 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                    ENVIRONMENT RAVEN_PREFIX=${fixture}/richedit-prefix)
                # Messages from the program's own message table, with inserts from a va_list, as installers write their logs.
                list(GET installer_compiler 0 installer_driver)
                # The tools beside the compiler, or on the path when it is reached through a wrapper directory such as ccache's.
                get_filename_component(installer_bin ${installer_driver} DIRECTORY)
                get_filename_component(installer_name ${installer_driver} NAME)
                string(REGEX REPLACE "gcc$" "windmc" message_name ${installer_name})
                string(REGEX REPLACE "gcc$" "windres" resource_name ${installer_name})
                find_program(RAVEN_WINDMC_${installer_arch} ${message_name} HINTS ${installer_bin})
                find_program(RAVEN_WINDRES_${installer_arch} ${resource_name} HINTS ${installer_bin})
                set(message_compiler ${RAVEN_WINDMC_${installer_arch}})
                set(resource_compiler ${RAVEN_WINDRES_${installer_arch}})
                file(MAKE_DIRECTORY ${fixture}/format)
                add_custom_command(OUTPUT ${fixture}/format.exe
                    COMMAND ${message_compiler} -h ${fixture}/format -r ${fixture}/format ${PROJECT_SOURCE_DIR}/testdata/native_loader/format_messages.mc
                    COMMAND ${resource_compiler} -O coff -I ${fixture}/format -i ${PROJECT_SOURCE_DIR}/testdata/native_loader/format_languages.rc -o ${fixture}/format/messages.o
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${installer_entry},--no-insert-timestamp
                        -o ${fixture}/format.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/format_client.c ${fixture}/format/messages.o -lkernel32
                    WORKING_DIRECTORY ${fixture}/format
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/format_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/format_languages.rc ${PROJECT_SOURCE_DIR}/testdata/native_loader/format_messages.mc VERBATIM)
                add_custom_target(format_fixture_${installer_arch} ALL DEPENDS ${fixture}/format.exe)
                add_test(NAME native_format_message_${installer_arch} COMMAND raven --run-process --execution=native --timeout-ms=60000 ${fixture}/format.exe)
                set_tests_properties(native_format_message_${installer_arch} PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                    ENVIRONMENT RAVEN_PREFIX=${fixture}/format-prefix)
                # A library loaded by path from a folder of the program's names that path, and a child process opens the parent to watch it.
                file(MAKE_DIRECTORY ${fixture}/module-path/plugins)
                add_custom_command(OUTPUT ${fixture}/module-path/module-path.exe ${fixture}/module-path/plugins/module_plugin.dll
                    COMMAND ${installer_compiler} -O2 -nostdlib -shared -fno-stack-protector ${action_link}
                        -o ${fixture}/module-path/plugins/module_plugin.dll ${PROJECT_SOURCE_DIR}/testdata/native_loader/module_plugin.c -lkernel32
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${installer_entry},--no-insert-timestamp
                        -o ${fixture}/module-path/module-path.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/module_path_client.c -llz32 -luser32 -lkernel32 -lgcc
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/module_plugin.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/module_path_client.c VERBATIM)
                add_custom_target(module_path_fixture_${installer_arch} ALL DEPENDS ${fixture}/module-path/module-path.exe)
                add_test(NAME native_module_path_${installer_arch} COMMAND raven --run-process --execution=native --timeout-ms=60000
                    ${fixture}/module-path/module-path.exe)
                set_tests_properties(native_module_path_${installer_arch} PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                    ENVIRONMENT RAVEN_PREFIX=${fixture}/module-path-prefix)
                # Side-by-side classes and visual styles: a program with the common controls 6 manifest gets the version 6 controls, contexts
                # activate from a manifest file, and the styles draw the accent default button.
                file(MAKE_DIRECTORY ${fixture}/actctx)
                add_custom_command(OUTPUT ${fixture}/actctx/actctx.exe ${fixture}/actctx/actctx.manifest
                    COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/testdata/native_loader/actctx.manifest ${fixture}/actctx/actctx.manifest
                    COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/testdata/native_loader/actctx_classes.manifest
                        ${PROJECT_SOURCE_DIR}/testdata/native_loader/Raven.Test.Managed.manifest ${fixture}/actctx
                    COMMAND ${resource_compiler} -O coff -i ${PROJECT_SOURCE_DIR}/testdata/native_loader/actctx.rc -o ${fixture}/actctx/actctx_res.o
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${installer_entry},--no-insert-timestamp
                        -o ${fixture}/actctx/actctx.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/actctx_client.c ${fixture}/actctx/actctx_res.o
                        -lcomctl32 -luxtheme -lole32 -luuid -lgdi32 -luser32 -lkernel32
                    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/testdata/native_loader
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/actctx_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/actctx.manifest
                        ${PROJECT_SOURCE_DIR}/testdata/native_loader/actctx_classes.manifest ${PROJECT_SOURCE_DIR}/testdata/native_loader/Raven.Test.Managed.manifest
                        ${PROJECT_SOURCE_DIR}/testdata/native_loader/actctx.rc VERBATIM)
                add_custom_target(actctx_fixture_${installer_arch} ALL DEPENDS ${fixture}/actctx/actctx.exe)
                add_test(NAME native_activation_contexts_${installer_arch} COMMAND raven --run-process --execution=native --timeout-ms=60000
                    ${fixture}/actctx/actctx.exe)
                set_tests_properties(native_activation_contexts_${installer_arch} PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                    ENVIRONMENT RAVEN_PREFIX=${fixture}/actctx-prefix)
                # Settings from the registry: a run switches the visual styles off as the control panel does, the next run starts without them.
                add_custom_command(OUTPUT ${fixture}/settings.exe
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${installer_entry},--no-insert-timestamp
                        -o ${fixture}/settings.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/settings_client.c -luxtheme -ladvapi32 -lkernel32
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/settings_client.c VERBATIM)
                add_custom_target(settings_fixture_${installer_arch} ALL DEPENDS ${fixture}/settings.exe)
                add_test(NAME native_settings_reset_${installer_arch} COMMAND ${CMAKE_COMMAND} -E rm -rf ${fixture}/settings-prefix)
                add_test(NAME native_settings_${installer_arch} COMMAND sh -c "unset RAVEN_THEME; \"$1\" --run-process --execution=native --timeout-ms=60000 \"$2\" -- --write && unset RAVEN_THEME && \"$1\" --run-process --execution=native --timeout-ms=60000 \"$2\"" settings $<TARGET_FILE:raven> ${fixture}/settings.exe)
                set_tests_properties(native_settings_reset_${installer_arch} PROPERTIES FIXTURES_SETUP settings_${installer_arch})
                set_tests_properties(native_settings_${installer_arch} PROPERTIES TIMEOUT 120 FIXTURES_REQUIRED settings_${installer_arch}
                    PASS_REGULAR_EXPRESSION "Native entry returned 0 .*Native entry returned 0 " ENVIRONMENT RAVEN_PREFIX=${fixture}/settings-prefix)
                # What the setups of older frameworks rely on: executable heaps, read-only folders, the ANSI event log, the 64-bit .NET switch,
                # unquoted program paths, service settings, critical section fields, data file loads, bounded formatting and the shell functions
                # installers take by number.
                add_custom_command(OUTPUT ${fixture}/legacy.exe
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${installer_entry},--no-insert-timestamp
                        -o ${fixture}/legacy.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/legacy_client.c -lshlwapi -luser32 -ladvapi32 -lkernel32
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/legacy_client.c VERBATIM)
                add_custom_target(legacy_fixture_${installer_arch} ALL DEPENDS ${fixture}/legacy.exe)
                add_test(NAME native_legacy_setup_services_${installer_arch} COMMAND raven --run-process --execution=native --timeout-ms=60000 ${fixture}/legacy.exe)
                set_tests_properties(native_legacy_setup_services_${installer_arch} PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                    ENVIRONMENT RAVEN_PREFIX=${fixture}/legacy-prefix)
                # The session of services, where the Windows Installer server runs custom actions: its window station is not visible.
                add_test(NAME native_service_session_${installer_arch} COMMAND raven --run-process --execution=native --timeout-ms=60000 --noninteractive
                    ${fixture}/legacy.exe -- --service)
                set_tests_properties(native_service_session_${installer_arch} PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                    ENVIRONMENT RAVEN_PREFIX=${fixture}/service-prefix)
                # What a Visual C++ redistributable leaves for programs, checked by the redistributable workflow below.
                add_custom_command(OUTPUT ${fixture}/redist-check.exe
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${installer_entry},--no-insert-timestamp
                        -o ${fixture}/redist-check.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/redist_check.c -ladvapi32 -lkernel32
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/redist_check.c VERBATIM)
                add_custom_target(redist_check_fixture_${installer_arch} ALL DEPENDS ${fixture}/redist-check.exe)
                # A protected system: an installer writes its own kernel32 and msvcr120 into the system directory; both files are accepted and
                # their version reads as written, kernel32 stays the system's library and the redistributable loads from the new file.
                if(installer_arch STREQUAL x86)
                    set(library_entry _DllMain@12)
                else()
                    set(library_entry DllMain)
                endif()
                file(MAKE_DIRECTORY ${fixture}/protect)
                add_custom_command(OUTPUT ${fixture}/protect/protect.exe ${fixture}/protect/fake-library.dll
                    COMMAND ${resource_compiler} -O coff -i ${PROJECT_SOURCE_DIR}/testdata/native_loader/fake_library.rc -o ${fixture}/protect/fake_library_res.o
                    COMMAND ${installer_compiler} -O2 -shared -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${library_entry},--no-insert-timestamp
                        -o ${fixture}/protect/fake-library.dll ${PROJECT_SOURCE_DIR}/testdata/native_loader/fake_library.c ${fixture}/protect/fake_library_res.o
                        -lkernel32
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${installer_entry},--no-insert-timestamp
                        -o ${fixture}/protect/protect.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/protect_client.c -lversion -lkernel32
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/fake_library.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/fake_library.rc
                        ${PROJECT_SOURCE_DIR}/testdata/native_loader/protect_client.c VERBATIM)
                add_custom_target(protect_fixture_${installer_arch} ALL DEPENDS ${fixture}/protect/protect.exe)
                add_test(NAME native_protected_system_${installer_arch} COMMAND sh -c "rm -rf \"$3\" && RAVEN_PREFIX=\"$3\" \"$1\" --run-process --execution=native --timeout-ms=60000 \"$2\" -- --install && RAVEN_PREFIX=\"$3\" \"$1\" --run-process --execution=native --timeout-ms=60000 \"$2\"" protect $<TARGET_FILE:raven> ${fixture}/protect/protect.exe ${fixture}/protect-prefix)
                set_tests_properties(native_protected_system_${installer_arch} PROPERTIES TIMEOUT 120
                    PASS_REGULAR_EXPRESSION "Native entry returned 0 .*Native entry returned 0 ")
                # Work left for a restart: moves and deletions MoveFileEx delays, and a RunOnce entry, carried out by `raven --reboot`.
                add_custom_command(OUTPUT ${fixture}/pending.exe
                    COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${installer_entry},--no-insert-timestamp
                        -o ${fixture}/pending.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/pending_client.c -ladvapi32 -lkernel32
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/pending_client.c VERBATIM)
                add_custom_target(pending_fixture_${installer_arch} ALL DEPENDS ${fixture}/pending.exe)
                # The restart runs its programs without a window, wherever the tests are started from.
                add_test(NAME native_pending_restart_${installer_arch} COMMAND sh -c "set -e; unset WAYLAND_DISPLAY; rm -rf \"$3\"; export RAVEN_PREFIX=\"$3\"; \"$1\" --run-process --execution=native --timeout-ms=60000 \"$2\" -- --schedule | grep -q \"Native entry returned 0 \"; \"$1\" --reboot; \"$1\" --run-process --execution=native --timeout-ms=60000 \"$2\" | grep -q \"Native entry returned 0 \"; echo PASS: delayed moves and RunOnce carried out at the restart" pending $<TARGET_FILE:raven> ${fixture}/pending.exe ${fixture}/pending-prefix)
                set_tests_properties(native_pending_restart_${installer_arch} PROPERTIES TIMEOUT 300 PASS_REGULAR_EXPRESSION "PASS: delayed moves")
                # Prefix snapshots: an installer runs, the prefix is kept before it, and one command takes the prefix back to it.
                if(installer_arch STREQUAL x86)
                    file(MAKE_DIRECTORY ${fixture}/snapshot)
                    add_custom_command(OUTPUT ${fixture}/snapshot/setup-sample.exe ${fixture}/snapshot/snapshot-check.exe
                        COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${installer_entry},--no-insert-timestamp
                            -o ${fixture}/snapshot/setup-sample.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/snapshot_client.c -ladvapi32 -lkernel32
                        COMMAND ${CMAKE_COMMAND} -E copy ${fixture}/snapshot/setup-sample.exe ${fixture}/snapshot/snapshot-check.exe
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/snapshot_client.c VERBATIM)
                    add_custom_target(snapshot_fixture ALL DEPENDS ${fixture}/snapshot/setup-sample.exe)
                    add_test(NAME native_prefix_snapshot COMMAND sh -c "set -e; rm -rf \"$3\" \"$3.snapshots\"; export RAVEN_PREFIX=\"$3\"; \"$1\" --run-process --execution=native --timeout-ms=60000 \"$2/setup-sample.exe\" -- --install | grep -q \"Native entry returned 0 \"; \"$1\" --list-snapshots | grep -q install-setup-sample; \"$1\" --run-process --execution=native --timeout-ms=60000 \"$2/snapshot-check.exe\" -- --present | grep -q \"Native entry returned 0 \"; \"$1\" --restore-prefix; \"$1\" --run-process --execution=native --timeout-ms=60000 \"$2/snapshot-check.exe\" | grep -q \"Native entry returned 0 \"; echo PASS: snapshot taken before the installation and restored" snapshot $<TARGET_FILE:raven> ${fixture}/snapshot ${fixture}/snapshot-prefix)
                    set_tests_properties(native_prefix_snapshot PROPERTIES TIMEOUT 300 PASS_REGULAR_EXPRESSION "PASS: snapshot taken")
                endif()
                # Programs built against each Visual C++ runtime from 2005 to 2013: the 2005 and 2008 ones name their side-by-side assembly
                # in the manifest, the later ones find their library in the system directory.
                if(installer_arch STREQUAL x86)
                    set(crt_architecture x86)
                    set(crt_system syswow64)
                else()
                    set(crt_architecture amd64)
                    set(crt_system system32)
                endif()
                set(crt_outputs)
                foreach(crt_version 80 90 100 110 120)
                    set(crt_objects)
                    if(crt_version STREQUAL 80 OR crt_version STREQUAL 90)
                        if(crt_version STREQUAL 80)
                            set(crt_assembly 8.0.50727.762)
                        else()
                            set(crt_assembly 9.0.21022.8)
                        endif()
                        configure_file(${PROJECT_SOURCE_DIR}/testdata/native_loader/crt_check.manifest.in ${fixture}/crt-vc${crt_version}.manifest @ONLY)
                        file(WRITE ${fixture}/crt-vc${crt_version}.rc "1 24 \"crt-vc${crt_version}.manifest\"\n")
                        add_custom_command(OUTPUT ${fixture}/crt-vc${crt_version}_res.o
                            COMMAND ${resource_compiler} -O coff -i ${fixture}/crt-vc${crt_version}.rc -o ${fixture}/crt-vc${crt_version}_res.o
                            WORKING_DIRECTORY ${fixture}
                            DEPENDS ${fixture}/crt-vc${crt_version}.rc ${fixture}/crt-vc${crt_version}.manifest VERBATIM)
                        set(crt_objects ${fixture}/crt-vc${crt_version}_res.o)
                        set(crt_place "\\\\winsxs\\\\${crt_architecture}_microsoft.vc${crt_version}.crt_1fc8b3b9a1e18e3b_")
                    else()
                        set(crt_place "\\\\windows\\\\${crt_system}\\\\msvcr${crt_version}.dll")
                    endif()
                    add_custom_command(OUTPUT ${fixture}/crt-vc${crt_version}.exe
                        COMMAND ${installer_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,${installer_entry},--no-insert-timestamp
                            "-DCRT_NAME=L\"msvcr${crt_version}.dll\"" "-DCRT_PLACE=L\"${crt_place}\""
                            -o ${fixture}/crt-vc${crt_version}.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/crt_check.c ${crt_objects}
                            -lmsvcr${crt_version} -luser32 -lkernel32
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/crt_check.c ${crt_objects} VERBATIM)
                    list(APPEND crt_outputs ${fixture}/crt-vc${crt_version}.exe)
                endforeach()
                add_custom_target(crt_check_fixture_${installer_arch} ALL DEPENDS ${crt_outputs})
                # msiexec found by name in the system directory, installing and removing the package from the command line.
                add_test(NAME native_msiexec_${installer_arch} COMMAND raven --run-process --execution=native --timeout-ms=60000 ${fixture}/msiexec-client.exe)
                set_tests_properties(native_msiexec_${installer_arch} PROPERTIES TIMEOUT 300 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                    ENVIRONMENT RAVEN_PREFIX=${fixture}/msiexec-prefix)
            endif()
        endforeach()
        if(RAVEN_INSTALLER32_COMPONENT_DIR AND RAVEN_MINGW32_INSTALLER)
            set(SYSTEM_COMPONENT_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/system-component-client.exe)
            add_custom_command(OUTPUT ${SYSTEM_COMPONENT_CLIENT}
                COMMAND ${RAVEN_MINGW32_INSTALLER} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,_raven_entry,--no-insert-timestamp -o ${SYSTEM_COMPONENT_CLIENT}
                    ${PROJECT_SOURCE_DIR}/testdata/native_loader/system_component_client.c -lkernel32 -lwintrust
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/system_component_client.c VERBATIM)
            add_custom_target(system_component_fixture ALL DEPENDS ${SYSTEM_COMPONENT_CLIENT})
            add_test(NAME native_system_components_x32 COMMAND raven --run-process --execution=native ${SYSTEM_COMPONENT_CLIENT})
            set_tests_properties(native_system_components_x32 PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/system-components-prefix)
        endif()
        if(RAVEN_MINGW32_INSTALLER)
            set(FOLD_VOLUME_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/fold-volume-client.exe)
            add_custom_command(OUTPUT ${FOLD_VOLUME_CLIENT}
                COMMAND ${RAVEN_MINGW32_INSTALLER} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,_raven_entry,--no-insert-timestamp -o ${FOLD_VOLUME_CLIENT}
                    ${PROJECT_SOURCE_DIR}/testdata/native_loader/fold_volume_client.c -lkernel32 -lntdll
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/fold_volume_client.c VERBATIM)
            add_custom_target(fold_volume_fixture ALL DEPENDS ${FOLD_VOLUME_CLIENT})
            add_test(NAME native_fold_volume_x32 COMMAND raven --run-process --execution=native ${FOLD_VOLUME_CLIENT})
            set_tests_properties(native_fold_volume_x32 PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/fold-volume-prefix)
        endif()
        # SQLite as Windows ships it, winsqlite3.dll (scripts/provision-sqlite.py), for 64-bit programs.
        set(RAVEN_SQLITE_COMPONENT_DIR "" CACHE PATH "Directory holding winsqlite3.dll built by scripts/provision-sqlite.py")
        if(RAVEN_SQLITE_COMPONENT_DIR)
            if(NOT EXISTS "${RAVEN_SQLITE_COMPONENT_DIR}/winsqlite3.dll")
                message(FATAL_ERROR "Configured SQLite component does not exist: ${RAVEN_SQLITE_COMPONENT_DIR}/winsqlite3.dll")
            endif()
            add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/winsqlite3.dll
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RAVEN_SQLITE_COMPONENT_DIR}/winsqlite3.dll ${CMAKE_CURRENT_BINARY_DIR}/winsqlite3.dll
                DEPENDS ${RAVEN_SQLITE_COMPONENT_DIR}/winsqlite3.dll VERBATIM)
            add_custom_target(sqlite_component ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/winsqlite3.dll)
            set(SQLITE_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/sqlite-client.exe)
            add_custom_command(OUTPUT ${SQLITE_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,raven_entry,--no-insert-timestamp
                    -o ${SQLITE_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/sqlite_client.c -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/sqlite_client.c VERBATIM)
            add_custom_target(sqlite_fixture ALL DEPENDS ${SQLITE_CLIENT} sqlite_component)
            add_test(NAME native_sqlite_x64 COMMAND raven --run-process --execution=native ${SQLITE_CLIENT})
            set_tests_properties(native_sqlite_x64 PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/sqlite-prefix)
        endif()
        set(RAVEN_CONTROLS_COMPONENT_DIR "" CACHE PATH "Directory holding the common controls component built by scripts/provision-controls.py")
        # The same components built for 32-bit programs (scripts/provision-controls.py --arch x86); they sit in a directory beside the runtime.
        set(RAVEN_CONTROLS32_COMPONENT_DIR "" CACHE PATH "Directory holding the 32-bit components built by scripts/provision-controls.py --arch x86")
        if(RAVEN_CONTROLS32_COMPONENT_DIR)
            foreach(component comctl32.dll raven-user32.dll raven-shell.dll)
                if(NOT EXISTS "${RAVEN_CONTROLS32_COMPONENT_DIR}/${component}")
                    message(FATAL_ERROR "Configured 32-bit controls component does not exist: ${component}")
                endif()
            endforeach()
            add_custom_target(raven_controls32_components
                COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:raven>/x86
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RAVEN_CONTROLS32_COMPONENT_DIR}/comctl32.dll $<TARGET_FILE_DIR:raven>/x86/comctl32.dll
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RAVEN_CONTROLS32_COMPONENT_DIR}/raven-user32.dll $<TARGET_FILE_DIR:raven>/x86/raven-user32.dll
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RAVEN_CONTROLS32_COMPONENT_DIR}/raven-shell.dll $<TARGET_FILE_DIR:raven>/x86/raven-shell.dll
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RAVEN_CONTROLS32_COMPONENT_DIR}/control.exe $<TARGET_FILE_DIR:raven>/x86/control.exe
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RAVEN_CONTROLS32_COMPONENT_DIR}/regedit.exe $<TARGET_FILE_DIR:raven>/x86/regedit.exe)
            # Run at every build, so a component provisioned again reaches the runtime without relinking it.
            add_dependencies(raven raven_controls32_components)
        endif()
        if(RAVEN_CONTROLS_COMPONENT_DIR)
            if(NOT EXISTS "${RAVEN_CONTROLS_COMPONENT_DIR}/comctl32.dll")
                message(FATAL_ERROR "Configured controls component does not exist")
            endif()
            add_custom_target(raven_controls_components
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RAVEN_CONTROLS_COMPONENT_DIR}/comctl32.dll $<TARGET_FILE_DIR:raven>/comctl32.dll
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RAVEN_CONTROLS_COMPONENT_DIR}/raven-user32.dll $<TARGET_FILE_DIR:raven>/raven-user32.dll
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RAVEN_CONTROLS_COMPONENT_DIR}/raven-shell.dll $<TARGET_FILE_DIR:raven>/raven-shell.dll
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RAVEN_CONTROLS_COMPONENT_DIR}/control.exe $<TARGET_FILE_DIR:raven>/control.exe
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RAVEN_CONTROLS_COMPONENT_DIR}/dism.exe $<TARGET_FILE_DIR:raven>/dism.exe
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RAVEN_CONTROLS_COMPONENT_DIR}/fondue.exe $<TARGET_FILE_DIR:raven>/fondue.exe
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RAVEN_CONTROLS_COMPONENT_DIR}/regedit.exe $<TARGET_FILE_DIR:raven>/regedit.exe
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RAVEN_CONTROLS_COMPONENT_DIR}/raven-dwrite.dll $<TARGET_FILE_DIR:raven>/raven-dwrite.dll
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RAVEN_CONTROLS_COMPONENT_DIR}/raven-d2d1.dll $<TARGET_FILE_DIR:raven>/raven-d2d1.dll
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RAVEN_CONTROLS_COMPONENT_DIR}/raven-uianimation.dll $<TARGET_FILE_DIR:raven>/raven-uianimation.dll
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RAVEN_CONTROLS_COMPONENT_DIR}/ucrtbase.dll $<TARGET_FILE_DIR:raven>/ucrtbase.dll)
            # Run at every build, so a component provisioned again reaches the runtime without relinking it.
            add_dependencies(raven raven_controls_components)
            set(EDIT_TEXT_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/edit-text.exe)
            add_custom_command(OUTPUT ${EDIT_TEXT_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib
                    -Wl,--entry,raven_entry,--image-base,0x1da000000,--no-insert-timestamp
                    -o ${EDIT_TEXT_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/edit_text_client.c -lkernel32 -luser32 -lgdi32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/edit_text_client.c VERBATIM)
            add_custom_target(edit_text_fixture ALL DEPENDS ${EDIT_TEXT_CLIENT})
            add_test(NAME native_edit_text_contract COMMAND raven --run-process --execution=native ${EDIT_TEXT_CLIENT})
            set_tests_properties(native_edit_text_contract PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            # DirectWrite: fonts, a face, metrics, tables, an outline and a rendered glyph run, through the public interfaces.
            set(DWRITE_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/dwrite-client.exe)
            add_custom_command(OUTPUT ${DWRITE_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1dd000000,--no-insert-timestamp
                    -o ${DWRITE_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/dwrite_client.c -ldwrite -lkernel32 -lgcc
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/dwrite_client.c VERBATIM)
            add_custom_target(dwrite_fixture ALL DEPENDS ${DWRITE_CLIENT})
            if(FREETYPE_FOUND AND EXISTS "${RAVEN_CONTROLS_COMPONENT_DIR}/raven-dwrite.dll")
                add_test(NAME native_dwrite COMMAND raven --run-process --execution=native --timeout-ms=20000 ${DWRITE_CLIENT})
                set_tests_properties(native_dwrite PROPERTIES TIMEOUT 40 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            endif()
            # Pipes: overlapped transfers that report to a port, cancellation, message mode, a blocking read on a thread and anonymous pipes.
            set(PIPE_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/pipe-client.exe)
            add_custom_command(OUTPUT ${PIPE_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1de000000,--no-insert-timestamp
                    -o ${PIPE_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/pipe_client.c -lkernel32 -lgcc
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/pipe_client.c VERBATIM)
            add_custom_target(pipe_fixture ALL DEPENDS ${PIPE_CLIENT})
            add_test(NAME native_pipes COMMAND raven --run-process --execution=native --timeout-ms=30000 ${PIPE_CLIENT})
            set_tests_properties(native_pipes PROPERTIES TIMEOUT 40 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            # Host services: known folders, the computer name, adapters, data protection, security checks and job objects.
            set(PLATFORM_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/platform-client.exe)
            add_custom_command(OUTPUT ${PLATFORM_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1df000000,--no-insert-timestamp
                    -o ${PLATFORM_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/platform_client.c
                    -lkernel32 -lshell32 -liphlpapi -ladvapi32 -lcrypt32 -lole32 -luuid -lws2_32 -lwinmm -lntdll -lgcc
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/platform_client.c VERBATIM)
            add_custom_target(platform_fixture ALL DEPENDS ${PLATFORM_CLIENT})
            add_test(NAME native_platform_services COMMAND raven --run-process --execution=native --timeout-ms=30000 ${PLATFORM_CLIENT})
            set_tests_properties(native_platform_services PROPERTIES TIMEOUT 40 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            set(ETW_CLIENTS)
            set(PROCESS_ENUM_CLIENTS)
            set(WORKSTATION_CLIENTS)
            set(INITIALIZE_SPY_CLIENTS)
            foreach(width 64 32)
                if(width EQUAL 64)
                    set(etw_compiler ${RAVEN_MINGW64})
                    set(etw_entry raven_entry)
                else()
                    set(etw_compiler ${RAVEN_MINGW32})
                    set(etw_entry _raven_entry)
                endif()
                if(etw_compiler)
                    set(etw_client ${CMAKE_CURRENT_BINARY_DIR}/etw-controller${width}.exe)
                    add_custom_command(OUTPUT ${etw_client}
                        COMMAND ${etw_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -mno-stack-arg-probe
                            -Wl,--entry,${etw_entry},--image-base,0x1e400000,--no-insert-timestamp
                            -o ${etw_client} ${PROJECT_SOURCE_DIR}/testdata/native_loader/etw_controller_client.c -lkernel32 -ladvapi32
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/etw_controller_client.c VERBATIM)
                    list(APPEND ETW_CLIENTS ${etw_client})
                    add_test(NAME native_etw_controller_x${width} COMMAND raven --run-process --execution=native ${etw_client})
                    set_tests_properties(native_etw_controller_x${width} PROPERTIES TIMEOUT 20 ENVIRONMENT RAVEN_PARALLEL=1
                        PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    set(enum_client ${CMAKE_CURRENT_BINARY_DIR}/process-enum${width}.exe)
                    add_custom_command(OUTPUT ${enum_client}
                        COMMAND ${etw_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -mno-stack-arg-probe
                            -Wl,--entry,${etw_entry},--image-base,0x1e500000,--no-insert-timestamp
                            -o ${enum_client} ${PROJECT_SOURCE_DIR}/testdata/native_loader/process_enum_client.c -lkernel32
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/process_enum_client.c VERBATIM)
                    list(APPEND PROCESS_ENUM_CLIENTS ${enum_client})
                    add_test(NAME native_process_enumeration_x${width} COMMAND raven --run-process --execution=native ${enum_client})
                    set_tests_properties(native_process_enumeration_x${width} PROPERTIES TIMEOUT 20 ENVIRONMENT RAVEN_PARALLEL=1
                        PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    # The workstation information installers read through netapi32.
                    set(workstation_client ${CMAKE_CURRENT_BINARY_DIR}/workstation-client${width}.exe)
                    add_custom_command(OUTPUT ${workstation_client}
                        COMMAND ${etw_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -mno-stack-arg-probe
                            -Wl,--entry,${etw_entry},--image-base,0x1e600000,--no-insert-timestamp
                            -o ${workstation_client} ${PROJECT_SOURCE_DIR}/testdata/native_loader/workstation_client.c -lkernel32 -lnetapi32
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/workstation_client.c VERBATIM)
                    list(APPEND WORKSTATION_CLIENTS ${workstation_client})
                    add_test(NAME native_workstation_info_x${width} COMMAND raven --run-process --execution=native ${workstation_client})
                    set_tests_properties(native_workstation_info_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    # The spies programs register to hear of each COM initialization on their threads.
                    set(initialize_spy_client ${CMAKE_CURRENT_BINARY_DIR}/initialize-spy${width}.exe)
                    add_custom_command(OUTPUT ${initialize_spy_client}
                        COMMAND ${etw_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -mno-stack-arg-probe
                            -Wl,--entry,${etw_entry},--image-base,0x1e700000,--no-insert-timestamp
                            -o ${initialize_spy_client} ${PROJECT_SOURCE_DIR}/testdata/native_loader/initialize_spy_client.c -lole32 -luuid -lkernel32
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/initialize_spy_client.c VERBATIM)
                    list(APPEND INITIALIZE_SPY_CLIENTS ${initialize_spy_client})
                    add_test(NAME native_initialize_spy_x${width} COMMAND raven --run-process --execution=native ${initialize_spy_client})
                    set_tests_properties(native_initialize_spy_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    # Values written to Unicode and ANSI profiles, as installers' page files are.
                    set(profile_client ${CMAKE_CURRENT_BINARY_DIR}/profile-client${width}.exe)
                    add_custom_command(OUTPUT ${profile_client}
                        COMMAND ${etw_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -mno-stack-arg-probe
                            -Wl,--entry,${etw_entry},--image-base,0x1ea00000,--no-insert-timestamp
                            -o ${profile_client} ${PROJECT_SOURCE_DIR}/testdata/native_loader/profile_client.c -lkernel32
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/profile_client.c VERBATIM)
                    list(APPEND PROFILE_CLIENTS ${profile_client})
                    add_test(NAME native_profile_encoding_x${width} COMMAND raven --run-process --execution=native ${profile_client})
                    set_tests_properties(native_profile_encoding_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                endif()
            endforeach()
            add_custom_target(etw_controller_fixtures ALL DEPENDS ${ETW_CLIENTS})
            add_custom_target(process_enum_fixtures ALL DEPENDS ${PROCESS_ENUM_CLIENTS})
            add_custom_target(workstation_fixtures ALL DEPENDS ${WORKSTATION_CLIENTS})
            add_custom_target(initialize_spy_fixtures ALL DEPENDS ${INITIALIZE_SPY_CLIENTS})
            add_custom_target(profile_fixtures ALL DEPENDS ${PROFILE_CLIENTS})
            set(SOCKET_MESSAGE_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/socket-message-client.exe)
            add_custom_command(OUTPUT ${SOCKET_MESSAGE_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--no-insert-timestamp -o ${SOCKET_MESSAGE_CLIENT}
                    ${PROJECT_SOURCE_DIR}/testdata/native_loader/socket_message_client.c -lkernel32 -lws2_32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/socket_message_client.c VERBATIM)
            add_custom_target(socket_message_fixture ALL DEPENDS ${SOCKET_MESSAGE_CLIENT})
            add_test(NAME native_socket_messages_x64 COMMAND raven --run-process --execution=native ${SOCKET_MESSAGE_CLIENT})
            set_tests_properties(native_socket_messages_x64 PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            # WinHTTP against a server of the test's own, internationalized domain names, and the task allocator as an IMalloc.
            if(OPENSSL_FOUND)
                set(WINHTTP_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/winhttp-client.exe)
                add_custom_command(OUTPUT ${WINHTTP_CLIENT}
                    COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                        -Wl,--entry,raven_entry,--no-insert-timestamp -o ${WINHTTP_CLIENT}
                        ${PROJECT_SOURCE_DIR}/testdata/native_loader/winhttp_client.c -lwinhttp -lnormaliz -lole32 -luuid -lws2_32 -lkernel32
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/winhttp_client.c VERBATIM)
                add_custom_target(winhttp_fixture ALL DEPENDS ${WINHTTP_CLIENT})
                add_test(NAME native_winhttp_x64 COMMAND raven --run-process --execution=native ${WINHTTP_CLIENT})
                set_tests_properties(native_winhttp_x64 PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            endif()
            find_program(RAVEN_OPENSSL_TOOL openssl)
            if(OPENSSL_FOUND AND RAVEN_OPENSSL_TOOL AND Python3_EXECUTABLE)
                set(SCHANNEL_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/schannel-client.exe)
                add_custom_command(OUTPUT ${SCHANNEL_CLIENT}
                    COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                        -Wl,--entry,raven_entry,--no-insert-timestamp -o ${SCHANNEL_CLIENT}
                        ${PROJECT_SOURCE_DIR}/testdata/native_loader/schannel_client.c -lkernel32 -lws2_32 -lsecur32
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/schannel_client.c VERBATIM)
                add_custom_target(schannel_fixture ALL DEPENDS ${SCHANNEL_CLIENT})
                add_test(NAME native_schannel_x64 COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/scripts/native-schannel.py $<TARGET_FILE:raven> ${SCHANNEL_CLIENT} ${CMAKE_CURRENT_BINARY_DIR})
                set_tests_properties(native_schannel_x64 PROPERTIES TIMEOUT 60)
            endif()
            find_program(RAVEN_MINGW32 i686-w64-mingw32-gcc)
            # Windows authentication between two programs of the machine, through the Negotiate package, at both widths.
            if(OPENSSL_FOUND)
                foreach(width 64 32)
                    set(ntlm_client ${CMAKE_CURRENT_BINARY_DIR}/ntlm-client${width}.exe)
                    if(width EQUAL 64)
                        set(ntlm_compiler ${RAVEN_MINGW64})
                        set(ntlm_entry raven_entry)
                    else()
                        set(ntlm_compiler ${RAVEN_MINGW32})
                        set(ntlm_entry _raven_entry)
                    endif()
                    if(ntlm_compiler)
                        add_custom_command(OUTPUT ${ntlm_client}
                            COMMAND ${ntlm_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${ntlm_entry},--no-insert-timestamp
                                -o ${ntlm_client} ${PROJECT_SOURCE_DIR}/testdata/native_loader/ntlm_client.c -lsecur32 -ladvapi32 -lkernel32
                            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/ntlm_client.c VERBATIM)
                        add_custom_target(ntlm_fixture${width} ALL DEPENDS ${ntlm_client})
                        add_test(NAME native_ntlm_x${width} COMMAND raven --run-process --execution=native ${ntlm_client})
                        set_tests_properties(native_ntlm_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    endif()
                endforeach()
            endif()
            set(REMOTE_PROCESS_CLIENTS)
            set(SHARED_WAIT_CLIENTS)
            set(PREFIX_SETUP_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/prefix-setup-client.exe)
            add_custom_command(OUTPUT ${PREFIX_SETUP_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--no-insert-timestamp -o ${PREFIX_SETUP_CLIENT}
                    ${PROJECT_SOURCE_DIR}/testdata/native_loader/prefix_setup_client.c -lkernel32 -ladvapi32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/prefix_setup_client.c VERBATIM)
            add_custom_target(prefix_setup_fixture ALL DEPENDS ${PREFIX_SETUP_CLIENT})
            if(Python3_EXECUTABLE)
                add_test(NAME native_prefix_setup_children COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/scripts/native-prefix-setup.py $<TARGET_FILE:raven> ${PREFIX_SETUP_CLIENT} ${CMAKE_CURRENT_BINARY_DIR})
                set_tests_properties(native_prefix_setup_children PROPERTIES TIMEOUT 60)
                add_test(NAME native_prefix_setup_children_parallel COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/scripts/native-prefix-setup.py $<TARGET_FILE:raven> ${PREFIX_SETUP_CLIENT} ${CMAKE_CURRENT_BINARY_DIR})
                set_tests_properties(native_prefix_setup_children_parallel PROPERTIES TIMEOUT 60 ENVIRONMENT RAVEN_PARALLEL=1)
            endif()
            set(TOKEN_CLIENTS)
            set(EVENT_CLIENTS)
            set(PEER_PROCESS_CLIENTS)
            set(RAW_DEVICE_CLIENTS)
            set(DIRECTORY_CLIENTS)
            foreach(width 64 32)
                if(width EQUAL 64)
                    set(remote_compiler ${RAVEN_MINGW64})
                    set(remote_entry raven_entry)
                    set(remote_base 0x1e8000000)
                    set(token_base 0x1e9000000)
                else()
                    set(remote_compiler ${RAVEN_MINGW32})
                    set(remote_entry _raven_entry)
                    set(remote_base 0x1e800000)
                    set(token_base 0x1e900000)
                endif()
                if(remote_compiler)
                    set(shared_wait_client ${CMAKE_CURRENT_BINARY_DIR}/shared-wait${width}.exe)
                    add_custom_command(OUTPUT ${shared_wait_client}
                        COMMAND ${remote_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector
                            -Wl,--entry,${remote_entry},--image-base,${remote_base},--no-insert-timestamp
                            -o ${shared_wait_client} ${PROJECT_SOURCE_DIR}/testdata/native_loader/shared_wait_client.c -lkernel32 -lgcc
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/shared_wait_client.c VERBATIM)
                    list(APPEND SHARED_WAIT_CLIENTS ${shared_wait_client})
                    add_test(NAME native_shared_infinite_wait_x${width} COMMAND raven --run-process --execution=native --timeout-ms=15000 ${shared_wait_client})
                    set_tests_properties(native_shared_infinite_wait_x${width} PROPERTIES TIMEOUT 25
                        ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/shared-wait-prefix${width};RAVEN_PARALLEL=1"
                        PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    if(Python3_Interpreter_FOUND)
                        add_test(NAME native_shared_wait_guards_x${width} COMMAND ${Python3_EXECUTABLE}
                            ${PROJECT_SOURCE_DIR}/scripts/native-wait-guards.py $<TARGET_FILE:raven> ${shared_wait_client})
                        set_tests_properties(native_shared_wait_guards_x${width} PROPERTIES TIMEOUT 30
                            ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/shared-wait-prefix${width};RAVEN_PARALLEL=1")
                    endif()
                    set(remote_client ${CMAKE_CURRENT_BINARY_DIR}/remote-process${width}.exe)
                    add_custom_command(OUTPUT ${remote_client}
                        COMMAND ${remote_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector
                            -Wl,--entry,${remote_entry},--image-base,${remote_base},--no-insert-timestamp
                            -o ${remote_client} ${PROJECT_SOURCE_DIR}/testdata/native_loader/remote_process_client.c -lkernel32 -lgcc
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/remote_process_client.c VERBATIM)
                    list(APPEND REMOTE_PROCESS_CLIENTS ${remote_client})
                    add_test(NAME native_remote_process_x${width} COMMAND raven --run-process --execution=native --timeout-ms=45000 ${remote_client})
                    set_tests_properties(native_remote_process_x${width} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                        ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/remote-process-prefix${width})
                    set(token_client ${CMAKE_CURRENT_BINARY_DIR}/token${width}.exe)
                    add_custom_command(OUTPUT ${token_client}
                        COMMAND ${remote_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector
                            -Wl,--entry,${remote_entry},--image-base,${token_base},--no-insert-timestamp
                            -o ${token_client} ${PROJECT_SOURCE_DIR}/testdata/native_loader/token_client.c -ladvapi32 -lkernel32 -lgcc
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/token_client.c VERBATIM)
                    list(APPEND TOKEN_CLIENTS ${token_client})
                    add_test(NAME native_process_token_x${width} COMMAND raven --run-process --execution=native --timeout-ms=45000 ${token_client})
                    set_tests_properties(native_process_token_x${width} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                        ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/token-prefix${width})
                    set(event_client ${CMAKE_CURRENT_BINARY_DIR}/event${width}.exe)
                    add_custom_command(OUTPUT ${event_client}
                        COMMAND ${remote_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector
                            -Wl,--entry,${remote_entry},--no-insert-timestamp
                            -o ${event_client} ${PROJECT_SOURCE_DIR}/testdata/native_loader/process_event_client.c -ladvapi32 -lkernel32 -lgcc
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/process_event_client.c VERBATIM)
                    list(APPEND EVENT_CLIENTS ${event_client})
                    add_test(NAME native_process_event_x${width} COMMAND raven --run-process --execution=native --timeout-ms=45000 ${event_client})
                    set_tests_properties(native_process_event_x${width} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                        ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/event-prefix${width})
                    set(peer_client ${CMAKE_CURRENT_BINARY_DIR}/peer-process${width}.exe)
                    add_custom_command(OUTPUT ${peer_client}
                        COMMAND ${remote_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector
                            -Wl,--entry,${remote_entry},--no-insert-timestamp
                            -o ${peer_client} ${PROJECT_SOURCE_DIR}/testdata/native_loader/peer_process_client.c -ladvapi32 -lkernel32 -lgcc
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/peer_process_client.c VERBATIM)
                    list(APPEND PEER_PROCESS_CLIENTS ${peer_client})
                    set(raw_client ${CMAKE_CURRENT_BINARY_DIR}/raw-device${width}.exe)
                    add_custom_command(OUTPUT ${raw_client}
                        COMMAND ${remote_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector
                            -Wl,--entry,${remote_entry},--no-insert-timestamp
                            -o ${raw_client} ${PROJECT_SOURCE_DIR}/testdata/native_loader/raw_device_client.c -luser32 -lkernel32 -lgcc
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/raw_device_client.c VERBATIM)
                    list(APPEND RAW_DEVICE_CLIENTS ${raw_client})
                    set(directory_client ${CMAKE_CURRENT_BINARY_DIR}/directory${width}.exe)
                    add_custom_command(OUTPUT ${directory_client}
                        COMMAND ${remote_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector
                            -Wl,--entry,${remote_entry},--no-insert-timestamp
                            -o ${directory_client} ${PROJECT_SOURCE_DIR}/testdata/native_loader/directory_client.c -lkernel32 -lgcc
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/directory_client.c VERBATIM)
                    list(APPEND DIRECTORY_CLIENTS ${directory_client})
                    add_test(NAME native_directory_x${width} COMMAND raven --run-process --execution=native ${directory_client})
                    set_tests_properties(native_directory_x${width} PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                        ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/directory-prefix${width})
                    add_test(NAME native_raw_device_x${width} COMMAND raven --run-process --execution=native ${raw_client})
                    set_tests_properties(native_raw_device_x${width} PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    add_test(NAME native_peer_process_x${width} COMMAND raven --run-process --execution=native --timeout-ms=45000 ${peer_client})
                    set_tests_properties(native_peer_process_x${width} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                        ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/peer-process-prefix${width})
                endif()
            endforeach()
            add_custom_target(remote_process_fixtures ALL DEPENDS ${REMOTE_PROCESS_CLIENTS})
            add_custom_target(shared_wait_fixtures ALL DEPENDS ${SHARED_WAIT_CLIENTS})
            add_custom_target(token_fixtures ALL DEPENDS ${TOKEN_CLIENTS})
            add_custom_target(event_fixtures ALL DEPENDS ${EVENT_CLIENTS})
            add_custom_target(peer_process_fixtures ALL DEPENDS ${PEER_PROCESS_CLIENTS})
            add_custom_target(raw_device_fixtures ALL DEPENDS ${RAW_DEVICE_CLIENTS})
            add_custom_target(directory_fixtures ALL DEPENDS ${DIRECTORY_CLIENTS})
            set(UCRT_IO_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/ucrt-io.exe)
            add_custom_command(OUTPUT ${UCRT_IO_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1dc000000,--no-insert-timestamp
                    -o ${UCRT_IO_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/ucrt_io_client.c -lucrtbase -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/ucrt_io_client.c VERBATIM)
            add_custom_target(ucrt_io_fixture ALL DEPENDS ${UCRT_IO_CLIENT})
            # The C runtime entry points Visual C++ 2019 and 2022 programs and their libraries import, resolved and called by name.
            set(UCRT_VC2022_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/ucrt-vc2022.exe)
            add_custom_command(OUTPUT ${UCRT_VC2022_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--no-insert-timestamp
                    -o ${UCRT_VC2022_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/ucrt_vc2022_client.c -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/ucrt_vc2022_client.c VERBATIM)
            add_custom_target(ucrt_vc2022_fixture ALL DEPENDS ${UCRT_VC2022_CLIENT})
            # A library loaded by its path is found again by that path, by GetModuleHandleW and by GetModuleHandleExW pinning it.
            set(MODULE_PATH_LIBRARY ${CMAKE_CURRENT_BINARY_DIR}/module-path-library.dll)
            add_custom_command(OUTPUT ${MODULE_PATH_LIBRARY}
                COMMAND ${RAVEN_MINGW64} -shared -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_library_entry,--image-base,0x1ea600000,--no-insert-timestamp
                    -o ${MODULE_PATH_LIBRARY} ${PROJECT_SOURCE_DIR}/testdata/native_loader/module_path_library.c
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/module_path_library.c VERBATIM)
            set(MODULE_HANDLE_PATH_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/module-handle-path.exe)
            add_custom_command(OUTPUT ${MODULE_HANDLE_PATH_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1ea700000,--no-insert-timestamp
                    -o ${MODULE_HANDLE_PATH_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/module_handle_path_client.c -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/module_handle_path_client.c VERBATIM)
            add_custom_target(module_handle_path_fixture ALL DEPENDS ${MODULE_HANDLE_PATH_CLIENT} ${MODULE_PATH_LIBRARY})
            add_test(NAME native_module_handle_path COMMAND raven --run-process --execution=native ${MODULE_HANDLE_PATH_CLIENT})
            set_tests_properties(native_module_handle_path PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/module-handle-path-prefix")
            # A directory listed through GetFileInformationByHandleEx, entry by entry, then ERROR_NO_MORE_FILES; a restarting class starts over.
            set(DIRECTORY_BY_HANDLE_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/directory-by-handle.exe)
            add_custom_command(OUTPUT ${DIRECTORY_BY_HANDLE_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1ea100000,--no-insert-timestamp
                    -o ${DIRECTORY_BY_HANDLE_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/directory_by_handle_client.c -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/directory_by_handle_client.c VERBATIM)
            add_custom_target(directory_by_handle_fixture ALL DEPENDS ${DIRECTORY_BY_HANDLE_CLIENT})
            add_test(NAME native_directory_by_handle COMMAND raven --run-process --execution=native ${DIRECTORY_BY_HANDLE_CLIENT})
            set_tests_properties(native_directory_by_handle PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/directory-by-handle-prefix")
            # The clocks of KUSER_SHARED_DATA advance and agree with GetSystemTimeAsFileTime.
            set(SHARED_CLOCK_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/shared-clock.exe)
            add_custom_command(OUTPUT ${SHARED_CLOCK_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1ea400000,--no-insert-timestamp
                    -o ${SHARED_CLOCK_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/shared_clock_client.c -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/shared_clock_client.c VERBATIM)
            add_custom_target(shared_clock_fixture ALL DEPENDS ${SHARED_CLOCK_CLIENT})
            add_test(NAME native_shared_clock COMMAND raven --run-process --execution=native ${SHARED_CLOCK_CLIENT})
            set_tests_properties(native_shared_clock PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/shared-clock-prefix")
            # The daylight saving rules of a zone with daylight time, from the host zone the test sets.
            set(TIME_ZONE_RULES_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/time-zone-rules.exe)
            add_custom_command(OUTPUT ${TIME_ZONE_RULES_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1ea500000,--no-insert-timestamp
                    -o ${TIME_ZONE_RULES_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/time_zone_rules_client.c -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/time_zone_rules_client.c VERBATIM)
            add_custom_target(time_zone_rules_fixture ALL DEPENDS ${TIME_ZONE_RULES_CLIENT})
            add_test(NAME native_time_zone_rules COMMAND raven --run-process --execution=native ${TIME_ZONE_RULES_CLIENT})
            set_tests_properties(native_time_zone_rules PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/time-zone-rules-prefix;TZ=America/New_York")
            # NtCreateFile and NtOpenFile: dispositions, directories, relative names and the statuses of a clash and a missing name.
            set(NT_CREATE_FILE_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/nt-create-file.exe)
            add_custom_command(OUTPUT ${NT_CREATE_FILE_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1ea200000,--no-insert-timestamp
                    -o ${NT_CREATE_FILE_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/nt_create_file_client.c -lntdll -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/nt_create_file_client.c VERBATIM)
            add_custom_target(nt_create_file_fixture ALL DEPENDS ${NT_CREATE_FILE_CLIENT})
            add_test(NAME native_nt_create_file COMMAND raven --run-process --execution=native ${NT_CREATE_FILE_CLIENT})
            set_tests_properties(native_nt_create_file PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/nt-create-file-prefix")
            # Junctions through FSCTL_SET_REPARSE_POINT, read back and removed, and a file that is no reparse point.
            set(JUNCTION_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/junction.exe)
            add_custom_command(OUTPUT ${JUNCTION_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1ea900000,--no-insert-timestamp
                    -o ${JUNCTION_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/junction_client.c -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/junction_client.c VERBATIM)
            add_custom_target(junction_fixture ALL DEPENDS ${JUNCTION_CLIENT})
            add_test(NAME native_junction COMMAND raven --run-process --execution=native ${JUNCTION_CLIENT})
            set_tests_properties(native_junction PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/junction-prefix)
            # The fibers of a 64-bit program, which the guest RTL component serves for kernel32.
            set(FIBER_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/fiber64.exe)
            add_custom_command(OUTPUT ${FIBER_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1ea800000,--no-insert-timestamp
                    -o ${FIBER_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/fiber_client.c -lucrtbase -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/fiber_client.c VERBATIM)
            add_custom_target(fiber_fixture ALL DEPENDS ${FIBER_CLIENT})
            add_test(NAME native_fiber_x64 COMMAND raven --run-process --execution=native ${FIBER_CLIENT})
            set_tests_properties(native_fiber_x64 PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/fiber-prefix)
            # An environment block for CreateProcessW that ends at the end of a page with nothing mapped after it.
            set(CHILD_ENVIRONMENT_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/child-environment.exe)
            add_custom_command(OUTPUT ${CHILD_ENVIRONMENT_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1eb100000,--no-insert-timestamp
                    -o ${CHILD_ENVIRONMENT_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/child_environment_client.c -lntdll -lucrtbase -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/child_environment_client.c VERBATIM)
            add_custom_target(child_environment_fixture ALL DEPENDS ${CHILD_ENVIRONMENT_CLIENT})
            add_test(NAME native_child_environment_page_end COMMAND raven --run-process --execution=native ${CHILD_ENVIRONMENT_CLIENT})
            set_tests_properties(native_child_environment_page_end PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/child-environment-prefix)
            # The ntdll forms of kernel32 functions, ending with RtlExitUserProcess.
            set(NTDLL_ALIAS_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/ntdll-alias.exe)
            add_custom_command(OUTPUT ${NTDLL_ALIAS_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1eb200000,--no-insert-timestamp
                    -o ${NTDLL_ALIAS_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/ntdll_alias_client.c -lntdll -lucrtbase -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/ntdll_alias_client.c VERBATIM)
            add_custom_target(ntdll_alias_fixture ALL DEPENDS ${NTDLL_ALIAS_CLIENT})
            add_test(NAME native_ntdll_aliases COMMAND raven --run-process --execution=native ${NTDLL_ALIAS_CLIENT})
            set_tests_properties(native_ntdll_aliases PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native process exited with status 0"
                ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/ntdll-alias-prefix)
            # The native sleeps, waits and thread alerts.
            set(NATIVE_WAIT_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/native-wait.exe)
            add_custom_command(OUTPUT ${NATIVE_WAIT_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1eb300000,--no-insert-timestamp
                    -o ${NATIVE_WAIT_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/native_wait_client.c -lntdll -lucrtbase -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/native_wait_client.c VERBATIM)
            add_custom_target(native_wait_fixture ALL DEPENDS ${NATIVE_WAIT_CLIENT})
            add_test(NAME native_waits_and_alerts COMMAND raven --run-process --execution=native ${NATIVE_WAIT_CLIENT})
            set_tests_properties(native_waits_and_alerts PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/native-wait-prefix)
            # The basic information of a child that has ended.
            set(EXITED_CHILD_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/exited-child.exe)
            add_custom_command(OUTPUT ${EXITED_CHILD_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1eb400000,--no-insert-timestamp
                    -o ${EXITED_CHILD_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/exited_child_client.c -lntdll -lucrtbase -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/exited_child_client.c VERBATIM)
            add_custom_target(exited_child_fixture ALL DEPENDS ${EXITED_CHILD_CLIENT})
            add_test(NAME native_exited_child_information COMMAND raven --run-process --execution=native ${EXITED_CHILD_CLIENT})
            set_tests_properties(native_exited_child_information PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/exited-child-prefix)
            # dbghelp's symbol handler as far as walking a stack needs it.
            set(SYMBOL_HANDLER_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/symbol-handler.exe)
            add_custom_command(OUTPUT ${SYMBOL_HANDLER_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1eb700000,--no-insert-timestamp
                    -o ${SYMBOL_HANDLER_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/symbol_handler_client.c -ldbghelp -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/symbol_handler_client.c VERBATIM)
            add_custom_target(symbol_handler_fixture ALL DEPENDS ${SYMBOL_HANDLER_CLIENT})
            add_test(NAME native_symbol_handler COMMAND raven --run-process --execution=native ${SYMBOL_HANDLER_CLIENT})
            set_tests_properties(native_symbol_handler PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/symbol-handler-prefix)
            # ntdll's token calls: NtQueryInformationToken and NtAdjustPrivilegesToken.
            set(NATIVE_TOKEN_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/native-token.exe)
            add_custom_command(OUTPUT ${NATIVE_TOKEN_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1eb800000,--no-insert-timestamp
                    -o ${NATIVE_TOKEN_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/native_token_client.c -lntdll -ladvapi32 -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/native_token_client.c VERBATIM)
            add_custom_target(native_token_fixture ALL DEPENDS ${NATIVE_TOKEN_CLIENT})
            add_test(NAME native_token_calls COMMAND raven --run-process --execution=native ${NATIVE_TOKEN_CLIENT})
            set_tests_properties(native_token_calls PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/native-token-prefix)
            # LCMapString's lengths: nothing written after a counted source, the zero of a terminated one counted and written.
            set(LCMAP_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/lcmap.exe)
            add_custom_command(OUTPUT ${LCMAP_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1eba00000,--no-insert-timestamp
                    -o ${LCMAP_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/lcmap_client.c -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/lcmap_client.c VERBATIM)
            add_custom_target(lcmap_fixture ALL DEPENDS ${LCMAP_CLIENT})
            add_test(NAME native_lcmap_string_lengths COMMAND raven --run-process --execution=native ${LCMAP_CLIENT})
            set_tests_properties(native_lcmap_string_lengths PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/lcmap-prefix)
            # A private side-by-side assembly a library's own manifest depends on, in its folder beside the program (as Ruby ships its libraries).
            if(RAVEN_WINDRES)
                set(PRIVATE_ASSEMBLY_DIR ${CMAKE_CURRENT_BINARY_DIR}/private-assembly)
                file(MAKE_DIRECTORY ${PRIVATE_ASSEMBLY_DIR}/raven.private)
                set(PRIVATE_ASSEMBLY_CLIENT ${PRIVATE_ASSEMBLY_DIR}/private-assembly.exe)
                add_custom_command(OUTPUT ${PRIVATE_ASSEMBLY_CLIENT}
                    COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/testdata/native_loader/raven.private.manifest ${PRIVATE_ASSEMBLY_DIR}/raven.private/raven.private.manifest
                    COMMAND ${RAVEN_MINGW64} -shared -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,DllMainCRTStartup,--no-insert-timestamp
                        -o ${PRIVATE_ASSEMBLY_DIR}/raven.private/private_target.dll ${PROJECT_SOURCE_DIR}/testdata/native_loader/private_target.c -lkernel32
                    COMMAND ${RAVEN_WINDRES} private_middle.rc ${PRIVATE_ASSEMBLY_DIR}/private_middle_res.o
                    COMMAND ${RAVEN_MINGW64} -shared -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,DllMainCRTStartup,--no-insert-timestamp
                        -o ${PRIVATE_ASSEMBLY_DIR}/private_middle.dll ${PROJECT_SOURCE_DIR}/testdata/native_loader/private_middle.c
                        ${PRIVATE_ASSEMBLY_DIR}/private_middle_res.o ${PRIVATE_ASSEMBLY_DIR}/raven.private/private_target.dll -lkernel32
                    COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                        -Wl,--entry,raven_entry,--image-base,0x1ebb00000,--no-insert-timestamp
                        -o ${PRIVATE_ASSEMBLY_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/private_assembly_client.c ${PRIVATE_ASSEMBLY_DIR}/private_middle.dll -lkernel32
                    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/testdata/native_loader
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/private_assembly_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/private_middle.c
                        ${PROJECT_SOURCE_DIR}/testdata/native_loader/private_target.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/private_middle.rc
                        ${PROJECT_SOURCE_DIR}/testdata/native_loader/private_middle.manifest ${PROJECT_SOURCE_DIR}/testdata/native_loader/raven.private.manifest VERBATIM)
                add_custom_target(private_assembly_fixture ALL DEPENDS ${PRIVATE_ASSEMBLY_CLIENT})
                add_test(NAME native_private_assembly COMMAND raven --run-process --execution=native ${PRIVATE_ASSEMBLY_CLIENT})
                set_tests_properties(native_private_assembly PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                    ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/private-assembly-prefix)
            endif()
            # CreateProcess finds a program named alone in the folders of PATH.
            set(PATH_SEARCH_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/path-search.exe)
            add_custom_command(OUTPUT ${PATH_SEARCH_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1ebd00000,--no-insert-timestamp
                    -o ${PATH_SEARCH_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/path_search_client.c -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/path_search_client.c VERBATIM)
            add_custom_target(path_search_fixture ALL DEPENDS ${PATH_SEARCH_CLIENT})
            add_test(NAME native_create_process_path_search COMMAND raven --run-process --execution=native ${PATH_SEARCH_CLIENT})
            set_tests_properties(native_create_process_path_search PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/path-search-prefix)
            # The volume devices of drives and the NT name of an open folder.
            set(VOLUME_NAME_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/volume-name.exe)
            add_custom_command(OUTPUT ${VOLUME_NAME_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1eb500000,--no-insert-timestamp
                    -o ${VOLUME_NAME_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/volume_name_client.c -lntdll -lucrtbase -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/volume_name_client.c VERBATIM)
            add_custom_target(volume_name_fixture ALL DEPENDS ${VOLUME_NAME_CLIENT})
            add_test(NAME native_volume_names COMMAND raven --run-process --execution=native ${VOLUME_NAME_CLIENT})
            set_tests_properties(native_volume_names PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/volume-name-prefix)
            # The AFD readiness poll mio and wepoll use: an accept completes through the completion port, a cancelled poll ends as cancelled.
            set(AFD_POLL_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/afd-poll.exe)
            add_custom_command(OUTPUT ${AFD_POLL_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1ea300000,--no-insert-timestamp
                    -o ${AFD_POLL_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/afd_poll_client.c -lws2_32 -lntdll -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/afd_poll_client.c VERBATIM)
            add_custom_target(afd_poll_fixture ALL DEPENDS ${AFD_POLL_CLIENT})
            add_test(NAME native_afd_poll COMMAND raven --run-process --execution=native ${AFD_POLL_CLIENT})
            set_tests_properties(native_afd_poll PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/afd-poll-prefix")
            # ntdll's critical sections from the guest RTL, and the PEB's free FastPebLock.
            set(NTDLL_LOCK_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/ntdll-lock.exe)
            add_custom_command(OUTPUT ${NTDLL_LOCK_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x1eb000000,--no-insert-timestamp
                    -o ${NTDLL_LOCK_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/ntdll_lock_client.c -lntdll -lucrtbase -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/ntdll_lock_client.c VERBATIM)
            add_custom_target(ntdll_lock_fixture ALL DEPENDS ${NTDLL_LOCK_CLIENT})
            add_test(NAME native_ntdll_critical_sections COMMAND raven --run-process --execution=native ${NTDLL_LOCK_CLIENT})
            set_tests_properties(native_ntdll_critical_sections PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/ntdll-lock-prefix)
            add_test(NAME native_ucrt_io_contract COMMAND raven --run-process --execution=native ${UCRT_IO_CLIENT})
            set_tests_properties(native_ucrt_io_contract PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/ucrt-prefix)
            add_test(NAME native_ucrt_vc2022_x64 COMMAND raven --run-process --execution=native ${UCRT_VC2022_CLIENT})
            set_tests_properties(native_ucrt_vc2022_x64 PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/ucrt-vc2022-prefix)
            foreach(width 64 32)
                if(width EQUAL 64)
                    set(crt_compiler ${RAVEN_MINGW64})
                    set(crt_entry raven_entry)
                else()
                    set(crt_compiler ${RAVEN_MINGW32})
                    set(crt_entry _raven_entry)
                endif()
                if(crt_compiler)
                    set(crt_client ${CMAKE_CURRENT_BINARY_DIR}/crt-errors${width}.exe)
                    add_custom_command(OUTPUT ${crt_client}
                        COMMAND ${crt_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector
                            -Wl,--entry,${crt_entry},--no-insert-timestamp
                            -o ${crt_client} ${PROJECT_SOURCE_DIR}/testdata/native_loader/crt_errors_client.c -lkernel32
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/crt_errors_client.c VERBATIM)
                    add_custom_target(crt_errors_fixture${width} ALL DEPENDS ${crt_client})
                    add_test(NAME native_crt_errors_x${width} COMMAND raven --run-process --execution=native ${crt_client})
                    set_tests_properties(native_crt_errors_x${width} PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                        ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/crt-errors-prefix${width})
                    set(shell_info_client ${CMAKE_CURRENT_BINARY_DIR}/shell-info${width}.exe)
                    add_custom_command(OUTPUT ${shell_info_client}
                        COMMAND ${crt_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector
                            -Wl,--entry,${crt_entry},--no-insert-timestamp
                            -o ${shell_info_client} ${PROJECT_SOURCE_DIR}/testdata/native_loader/shell_info_client.c -lshell32 -lole32 -luser32 -lgdi32 -lkernel32
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/shell_info_client.c VERBATIM)
                    add_custom_target(shell_info_fixture${width} ALL DEPENDS ${shell_info_client})
                    add_test(NAME native_shell_info_x${width} COMMAND raven --run-process --execution=native ${shell_info_client})
                    set_tests_properties(native_shell_info_x${width} PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                        ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/shell-info-prefix${width})
                    set(window_hooks_client ${CMAKE_CURRENT_BINARY_DIR}/window-hooks${width}.exe)
                    add_custom_command(OUTPUT ${window_hooks_client}
                        COMMAND ${crt_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector
                            -Wl,--entry,${crt_entry},--no-insert-timestamp
                            -o ${window_hooks_client} ${PROJECT_SOURCE_DIR}/testdata/native_loader/window_hooks_client.c -luser32 -lkernel32
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/window_hooks_client.c VERBATIM)
                    add_custom_target(window_hooks_fixture${width} ALL DEPENDS ${window_hooks_client})
                    add_test(NAME native_window_hooks_x${width} COMMAND raven --run-process --execution=native ${window_hooks_client})
                    set_tests_properties(native_window_hooks_x${width} PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                        ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/window-hooks-prefix${width})
                    set(memory_client ${CMAKE_CURRENT_BINARY_DIR}/installed-memory${width}.exe)
                    add_custom_command(OUTPUT ${memory_client}
                        COMMAND ${crt_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector
                            -Wl,--entry,${crt_entry},--no-insert-timestamp
                            -o ${memory_client} ${PROJECT_SOURCE_DIR}/testdata/native_loader/installed_memory_client.c -lkernel32
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/installed_memory_client.c VERBATIM)
                    add_custom_target(installed_memory_fixture${width} ALL DEPENDS ${memory_client})
                    add_test(NAME native_installed_memory_x${width} COMMAND raven --run-process --execution=native ${memory_client})
                    set_tests_properties(native_installed_memory_x${width} PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    set(package_client ${CMAKE_CURRENT_BINARY_DIR}/package-identity${width}.exe)
                    add_custom_command(OUTPUT ${package_client}
                        COMMAND ${crt_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector
                            -Wl,--entry,${crt_entry},--no-insert-timestamp
                            -o ${package_client} ${PROJECT_SOURCE_DIR}/testdata/native_loader/package_identity_client.c -lkernel32
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/package_identity_client.c VERBATIM)
                    add_custom_target(package_identity_fixture${width} ALL DEPENDS ${package_client})
                    add_test(NAME native_package_identity_x${width} COMMAND raven --run-process --execution=native ${package_client})
                    set_tests_properties(native_package_identity_x${width} PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    set(folder_client ${CMAKE_CURRENT_BINARY_DIR}/special-folder${width}.exe)
                    add_custom_command(OUTPUT ${folder_client}
                        COMMAND ${crt_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector
                            -Wl,--entry,${crt_entry},--no-insert-timestamp
                            -o ${folder_client} ${PROJECT_SOURCE_DIR}/testdata/native_loader/special_folder_client.c -lshell32 -lkernel32
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/special_folder_client.c VERBATIM)
                    add_custom_target(special_folder_fixture${width} ALL DEPENDS ${folder_client})
                    add_test(NAME native_special_folder_x${width} COMMAND raven --run-process --execution=native ${folder_client})
                    set_tests_properties(native_special_folder_x${width} PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                        ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/special-folder-prefix${width})
                    set(normalize_client ${CMAKE_CURRENT_BINARY_DIR}/normalize${width}.exe)
                    add_custom_command(OUTPUT ${normalize_client}
                        COMMAND ${crt_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector
                            -Wl,--entry,${crt_entry},--no-insert-timestamp
                            -o ${normalize_client} ${PROJECT_SOURCE_DIR}/testdata/native_loader/normalize_client.c -lnormaliz -lkernel32
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/normalize_client.c VERBATIM)
                    add_custom_target(normalize_fixture${width} ALL DEPENDS ${normalize_client})
                    add_test(NAME native_normalize_x${width} COMMAND raven --run-process --execution=native ${normalize_client})
                    set_tests_properties(native_normalize_x${width} PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    set(shell_folder_client ${CMAKE_CURRENT_BINARY_DIR}/shell-folder${width}.exe)
                    add_custom_command(OUTPUT ${shell_folder_client}
                        COMMAND ${crt_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector
                            -Wl,--entry,${crt_entry},--no-insert-timestamp
                            -o ${shell_folder_client} ${PROJECT_SOURCE_DIR}/testdata/native_loader/shell_folder_client.c -lshell32 -lole32 -lkernel32 -luuid
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/shell_folder_client.c VERBATIM)
                    add_custom_target(shell_folder_fixture${width} ALL DEPENDS ${shell_folder_client})
                    add_test(NAME native_shell_folder_x${width} COMMAND raven --run-process --execution=native ${shell_folder_client})
                    set_tests_properties(native_shell_folder_x${width} PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                        ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/shell-folder-prefix${width})
                    set(snapshot_client ${CMAKE_CURRENT_BINARY_DIR}/thread-snapshot${width}.exe)
                    add_custom_command(OUTPUT ${snapshot_client}
                        COMMAND ${crt_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector
                            -Wl,--entry,${crt_entry},--no-insert-timestamp
                            -o ${snapshot_client} ${PROJECT_SOURCE_DIR}/testdata/native_loader/thread_snapshot_client.c -lkernel32
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/thread_snapshot_client.c VERBATIM)
                    add_custom_target(thread_snapshot_fixture${width} ALL DEPENDS ${snapshot_client})
                    # Started with arguments, as by hand, so that the snapshot must name it by its program and not by those.
                    add_test(NAME native_thread_snapshot_x${width} COMMAND raven --run-process --execution=native ${snapshot_client} -- an argument)
                    set_tests_properties(native_thread_snapshot_x${width} PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                        ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/thread-snapshot-prefix${width})
                    # What an updater asks of the machine and of the program it hands over to before it installs a newer version.
                    set(updater_client ${CMAKE_CURRENT_BINARY_DIR}/updater${width}.exe)
                    add_custom_command(OUTPUT ${updater_client}
                        COMMAND ${crt_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector
                            -Wl,--entry,${crt_entry},--no-insert-timestamp
                            -o ${updater_client} ${PROJECT_SOURCE_DIR}/testdata/native_loader/updater_client.c -lkernel32 -luser32 -ladvapi32 -lshlwapi -lshell32 -lcrypt32
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/updater_client.c VERBATIM)
                    add_custom_target(updater_fixture${width} ALL DEPENDS ${updater_client})
                    add_test(NAME native_updater_x${width} COMMAND raven --run-process --execution=native ${updater_client})
                    set_tests_properties(native_updater_x${width} PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                        ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/updater-prefix${width})
                    # A launcher in a folder of its own, beside the one library it loads by name.
                    set(launcher_folder ${CMAKE_CURRENT_BINARY_DIR}/launcher${width})
                    file(MAKE_DIRECTORY ${launcher_folder})
                    if(width EQUAL 32)
                        set(launcher_library_entry -Wl,--entry,_DllMain@12,--kill-at,--no-insert-timestamp)
                    else()
                        set(launcher_library_entry -Wl,--entry,DllMain,--no-insert-timestamp)
                    endif()
                    add_custom_command(OUTPUT ${launcher_folder}/launcher.exe ${launcher_folder}/launcher_work.dll
                        COMMAND ${crt_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -shared ${launcher_library_entry}
                            -o ${launcher_folder}/launcher_work.dll ${PROJECT_SOURCE_DIR}/testdata/native_loader/launcher_work.c -lkernel32 -luser32
                        COMMAND ${crt_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector
                            -Wl,--entry,${crt_entry},--no-insert-timestamp
                            -o ${launcher_folder}/launcher.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/launcher_client.c -lkernel32
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/launcher_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/launcher_work.c VERBATIM)
                    add_custom_target(launcher_fixture${width} ALL DEPENDS ${launcher_folder}/launcher.exe ${launcher_folder}/launcher_work.dll)
                    add_test(NAME native_launcher_library_x${width} COMMAND raven --run-process --execution=native ${launcher_folder}/launcher.exe)
                    set_tests_properties(native_launcher_library_x${width} PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                        ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/launcher-prefix${width})
                endif()
            endforeach()
        endif()
        set(RAVEN_RTL_COMPONENT_PATH "" CACHE FILEPATH "Guest RTL component built by scripts/provision-rtl.py")
        if(RAVEN_RTL_COMPONENT_PATH)
            if(NOT EXISTS "${RAVEN_RTL_COMPONENT_PATH}")
                message(FATAL_ERROR "Configured native RTL component does not exist")
            endif()
            add_custom_target(raven_rtl_component
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RAVEN_RTL_COMPONENT_PATH} $<TARGET_FILE_DIR:raven>/raven-rtl.dll)
            # Run at every build, so a component provisioned again reaches the runtime without relinking it.
            add_dependencies(raven raven_rtl_component)
            set(HEAP_FIXTURE_DIR ${CMAKE_CURRENT_BINARY_DIR}/heap-fixtures)
            set(HEAP_PROBE ${HEAP_FIXTURE_DIR}/heap-probe.dll)
            set(HEAP_CLIENT ${HEAP_FIXTURE_DIR}/client.exe)
            add_custom_command(OUTPUT ${HEAP_PROBE} ${HEAP_FIXTURE_DIR}/heap-probe.lib
                COMMAND ${CMAKE_COMMAND} -E make_directory ${HEAP_FIXTURE_DIR}
                COMMAND ${RAVEN_MINGW64} -shared -nostdlib
                    -Wl,--entry,DllMain,--image-base,0x1a3000000,--no-insert-timestamp
                    -Wl,--out-implib,${HEAP_FIXTURE_DIR}/heap-probe.lib
                    -o ${HEAP_PROBE} ${PROJECT_SOURCE_DIR}/testdata/native_loader/heap_probe.c -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/heap_probe.c VERBATIM)
            add_custom_command(OUTPUT ${HEAP_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib
                    -Wl,--entry,raven_entry,--image-base,0x1a2000000,--no-insert-timestamp
                    -Xlinker --heap -Xlinker 196608,8192
                    -o ${HEAP_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/heap_client.c
                    ${HEAP_FIXTURE_DIR}/heap-probe.lib -lkernel32 -lntdll
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/heap_client.c ${HEAP_PROBE}
                    ${HEAP_FIXTURE_DIR}/heap-probe.lib VERBATIM)
            add_custom_target(heap_pe_fixture ALL DEPENDS ${HEAP_CLIENT})
            add_test(NAME native_heap_cli COMMAND raven --run-process --execution=native
                ${HEAP_CLIENT} ${HEAP_PROBE} ${RAVEN_RTL_COMPONENT_PATH})
            set_tests_properties(native_heap_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            set(HEAP_CAPACITY ${HEAP_FIXTURE_DIR}/capacity.exe)
            add_custom_command(OUTPUT ${HEAP_CAPACITY}
                COMMAND ${RAVEN_MINGW64} -nostdlib
                    -Wl,--entry,raven_entry,--image-base,0x1a2000000,--no-insert-timestamp
                    -o ${HEAP_CAPACITY} ${PROJECT_SOURCE_DIR}/testdata/native_loader/heap_capacity.c -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/heap_capacity.c VERBATIM)
            add_custom_target(heap_capacity_fixture ALL DEPENDS ${HEAP_CAPACITY})
            add_test(NAME native_heap_capacity_cli COMMAND raven --run-process --execution=native
                --timeout-ms=30000 ${HEAP_CAPACITY} ${RAVEN_RTL_COMPONENT_PATH})
            set_tests_properties(native_heap_capacity_cli PROPERTIES TIMEOUT 40 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        endif()
        if(PNG_FOUND AND RAVEN_WINDRES)
            set(DESKTOP_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/desktop-fixtures)
            add_custom_command(OUTPUT ${DESKTOP_FIXTURES}/client.exe ${DESKTOP_FIXTURES}/icons.dll
                COMMAND ${CMAKE_COMMAND} -E make_directory ${DESKTOP_FIXTURES}
                COMMAND ${RAVEN_WINDRES} -I ${RAVEN_TESTDATA_DIR}/native_loader -I ${PROJECT_SOURCE_DIR}/testdata/native_loader
                    ${PROJECT_SOURCE_DIR}/testdata/native_loader/icon_resources.rc ${DESKTOP_FIXTURES}/resources.o
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x193000000
                    -o ${DESKTOP_FIXTURES}/client.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/icon_client.c
                    -lkernel32 -luser32 -lgdi32 -lcomctl32
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -shared -fno-stack-protector
                    -Wl,--entry,0,--image-base,0x194000000
                    -o ${DESKTOP_FIXTURES}/icons.dll ${PROJECT_SOURCE_DIR}/testdata/native_loader/icon_module.c ${DESKTOP_FIXTURES}/resources.o
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/icon_client.c
                    ${PROJECT_SOURCE_DIR}/testdata/native_loader/icon_module.c
                    ${PROJECT_SOURCE_DIR}/testdata/native_loader/icon_resources.rc
                    ${RAVEN_TESTDATA_DIR}/native_loader/icon_resources.ico
                    ${PROJECT_SOURCE_DIR}/testdata/native_loader/icon_data.h VERBATIM)
            add_custom_target(desktop_pe_fixtures ALL DEPENDS ${DESKTOP_FIXTURES}/client.exe ${DESKTOP_FIXTURES}/icons.dll)
            add_executable(desktop_test desktop/desktop_test.cpp)
            target_include_directories(desktop_test PRIVATE ${PROJECT_SOURCE_DIR}/testdata/native_loader)
            target_link_libraries(desktop_test PRIVATE raven_cpu)
            add_dependencies(desktop_test desktop_pe_fixtures raven-bootstrap)
            add_test(NAME native_desktop_icon_contract COMMAND desktop_test $<TARGET_FILE:raven-bootstrap> ${DESKTOP_FIXTURES}
                ${RAVEN_TESTDATA_DIR}/npp/notepad++.exe ${CMAKE_CURRENT_BINARY_DIR}/desktop-output)
            set_tests_properties(native_desktop_icon_contract PROPERTIES TIMEOUT 120)
            add_test(NAME native_desktop_icon_cli COMMAND raven --run-process --execution=native ${DESKTOP_FIXTURES}/client.exe)
            set_tests_properties(native_desktop_icon_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            add_custom_command(OUTPUT ${DESKTOP_FIXTURES}/surface.exe
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x195000000
                    -o ${DESKTOP_FIXTURES}/surface.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/surface_client.c
                    -lkernel32 -luser32 -lgdi32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/surface_client.c
                    ${PROJECT_SOURCE_DIR}/testdata/native_loader/icon_data.h VERBATIM)
            add_custom_target(surface_pe_fixture ALL DEPENDS ${DESKTOP_FIXTURES}/surface.exe)
            add_test(NAME native_desktop_surface_cli COMMAND raven --run-process --execution=native ${DESKTOP_FIXTURES}/surface.exe)
            set_tests_properties(native_desktop_surface_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        endif()
        find_program(RAVEN_DLLTOOL x86_64-w64-mingw32-dlltool)
        if(Vulkan_FOUND AND RAVEN_DLLTOOL)
            set(VULKAN_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/vulkan-fixtures)
            # The import library lists every command the guest provider exports: the hand-written ones and the generated ones.
            if(VULKAN_GENERATED_SOURCES)
                set(VULKAN_FIXTURE_DEF ${VULKAN_GENERATED_DIR}/vulkan_all_exports.def)
            else()
                set(VULKAN_FIXTURE_DEF ${PROJECT_SOURCE_DIR}/testdata/native_cpu/vulkan.def)
            endif()
            add_custom_command(OUTPUT ${VULKAN_FIXTURES}/vulkan.a
                COMMAND ${CMAKE_COMMAND} -E make_directory ${VULKAN_FIXTURES}
                COMMAND ${RAVEN_DLLTOOL} -d ${VULKAN_FIXTURE_DEF}
                    -l ${VULKAN_FIXTURES}/vulkan.a
                DEPENDS ${VULKAN_FIXTURE_DEF} VERBATIM)
            # One target owns each import library, so the fixtures that link it wait for it instead of running dlltool side by side.
            add_custom_target(vulkan_import64 DEPENDS ${VULKAN_FIXTURES}/vulkan.a)
            add_custom_command(OUTPUT ${VULKAN_FIXTURES}/client.exe
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -mno-stack-arg-probe
                    -idirafter ${Vulkan_INCLUDE_DIRS} -I${PROJECT_SOURCE_DIR}/testdata/native_cpu
                    -Wl,--entry,raven_entry,--image-base,0x18e000000
                    -o ${VULKAN_FIXTURES}/client.exe ${PROJECT_SOURCE_DIR}/testdata/native_cpu/vulkan_client.c
                    ${VULKAN_FIXTURES}/vulkan.a
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_cpu/vulkan_client.c
                    ${PROJECT_SOURCE_DIR}/testdata/native_cpu/vulkan_compute_spirv.h ${PROJECT_SOURCE_DIR}/testdata/native_cpu/vulkan_triangle_spirv.h ${PROJECT_SOURCE_DIR}/testdata/native_cpu/vulkan_quad_spirv.h ${VULKAN_FIXTURES}/vulkan.a VERBATIM)
            add_custom_target(vulkan_pe_fixture ALL DEPENDS ${VULKAN_FIXTURES}/client.exe)
            add_dependencies(vulkan_pe_fixture vulkan_import64)
            add_executable(vulkan_bridge_test graphics/vulkan_bridge_test.cpp)
            target_link_libraries(vulkan_bridge_test PRIVATE raven_cpu)
            add_dependencies(vulkan_bridge_test vulkan_pe_fixture raven-bootstrap)
            add_test(NAME native_vulkan_contract COMMAND vulkan_bridge_test
                $<TARGET_FILE:raven-bootstrap> ${VULKAN_FIXTURES}/client.exe)
            set_tests_properties(native_vulkan_contract PROPERTIES TIMEOUT 20 FAIL_REGULAR_EXPRESSION "Validation Error:|VUID-")
            add_test(NAME native_vulkan_cli COMMAND raven --run-process --execution=native ${VULKAN_FIXTURES}/client.exe)
            set_tests_properties(native_vulkan_cli PROPERTIES TIMEOUT 20
                PASS_REGULAR_EXPRESSION "Native entry returned 0 " FAIL_REGULAR_EXPRESSION "Validation Error:|VUID-")
            if(VULKAN_GENERATED_SOURCES)
                add_custom_command(OUTPUT ${VULKAN_FIXTURES}/extensions64.exe
                    COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -mno-stack-arg-probe
                        -idirafter ${Vulkan_INCLUDE_DIRS} -I${PROJECT_SOURCE_DIR}/testdata/native_cpu
                        -Wl,--entry,raven_entry,--image-base,0x18e000000
                        -o ${VULKAN_FIXTURES}/extensions64.exe ${PROJECT_SOURCE_DIR}/testdata/native_cpu/vulkan_extensions_client.c
                        ${VULKAN_FIXTURES}/vulkan.a
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_cpu/vulkan_extensions_client.c
                        ${PROJECT_SOURCE_DIR}/testdata/native_cpu/vulkan_compute_spirv.h ${VULKAN_FIXTURES}/vulkan.a VERBATIM)
                add_custom_target(vulkan_extensions64_fixture ALL DEPENDS ${VULKAN_FIXTURES}/extensions64.exe)
                add_dependencies(vulkan_extensions64_fixture vulkan_import64)
                add_test(NAME native_vulkan_extensions_x64 COMMAND raven --run-process --execution=native ${VULKAN_FIXTURES}/extensions64.exe)
                set_tests_properties(native_vulkan_extensions_x64 PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 " FAIL_REGULAR_EXPRESSION "Validation Error:|VUID-")
                add_test(NAME native_vulkan_queued_extensions_x64 COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/scripts/native-queued-vulkan.py $<TARGET_FILE:raven> ${VULKAN_FIXTURES}/extensions64.exe)
                set_tests_properties(native_vulkan_queued_extensions_x64 PROPERTIES TIMEOUT 30)
                add_custom_command(OUTPUT ${VULKAN_FIXTURES}/foreign-present64.exe
                    COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -mno-stack-arg-probe
                        -idirafter ${Vulkan_INCLUDE_DIRS}
                        -Wl,--entry,raven_entry,--image-base,0x18e000000
                        -o ${VULKAN_FIXTURES}/foreign-present64.exe ${PROJECT_SOURCE_DIR}/testdata/native_cpu/foreign_present_client.c
                        ${VULKAN_FIXTURES}/vulkan.a -lkernel32
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_cpu/foreign_present_client.c ${VULKAN_FIXTURES}/vulkan.a VERBATIM)
                add_custom_target(vulkan_foreign64_fixture ALL DEPENDS ${VULKAN_FIXTURES}/foreign-present64.exe)
                add_dependencies(vulkan_foreign64_fixture vulkan_import64)
                add_executable(foreign_present_test graphics/foreign_present_test.cpp)
                target_link_libraries(foreign_present_test PRIVATE raven_cpu)
                add_dependencies(foreign_present_test vulkan_foreign64_fixture raven-bootstrap)
                add_test(NAME native_vulkan_foreign_present_x64 COMMAND foreign_present_test
                    $<TARGET_FILE:raven-bootstrap> ${VULKAN_FIXTURES}/foreign-present64.exe)
                # A prefix of its own keeps its window records apart from the other tests that publish windows.
                set_tests_properties(native_vulkan_foreign_present_x64 PROPERTIES TIMEOUT 60 FAIL_REGULAR_EXPRESSION "Validation Error:|VUID-"
                    ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/foreign-present-x64-prefix)
                add_custom_command(OUTPUT ${VULKAN_FIXTURES}/descriptor-buffer64.exe
                    COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -mno-stack-arg-probe
                        -idirafter ${Vulkan_INCLUDE_DIRS} -I${PROJECT_SOURCE_DIR}/testdata/native_cpu
                        -Wl,--entry,raven_entry,--image-base,0x18e000000
                        -o ${VULKAN_FIXTURES}/descriptor-buffer64.exe ${PROJECT_SOURCE_DIR}/testdata/native_cpu/descriptor_buffer_client.c
                        ${VULKAN_FIXTURES}/vulkan.a
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_cpu/descriptor_buffer_client.c
                        ${PROJECT_SOURCE_DIR}/testdata/native_cpu/vulkan_compute_spirv.h ${VULKAN_FIXTURES}/vulkan.a VERBATIM)
                add_custom_target(vulkan_descriptor64_fixture ALL DEPENDS ${VULKAN_FIXTURES}/descriptor-buffer64.exe)
                add_dependencies(vulkan_descriptor64_fixture vulkan_import64)
                add_test(NAME native_vulkan_descriptor_buffer_x64 COMMAND raven --run-process --execution=native ${VULKAN_FIXTURES}/descriptor-buffer64.exe)
                set_tests_properties(native_vulkan_descriptor_buffer_x64 PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 " FAIL_REGULAR_EXPRESSION "Validation Error:|VUID-")
                if(TARGET vulkan_thunk32_generated)
                    find_program(RAVEN_DLLTOOL32 i686-w64-mingw32-dlltool)
                    if(RAVEN_DLLTOOL32)
                        add_custom_command(OUTPUT ${VULKAN_FIXTURES}/vulkan32.a
                            COMMAND ${CMAKE_COMMAND} -E make_directory ${VULKAN_FIXTURES}
                            COMMAND ${RAVEN_DLLTOOL32} --kill-at -d ${VULKAN_GENERATED_DIR}/vulkan_thunk32_exports.def -l ${VULKAN_FIXTURES}/vulkan32.a
                            DEPENDS ${VULKAN_GENERATED_DIR}/vulkan_thunk32_exports.def VERBATIM)
                        add_custom_target(vulkan_import32 DEPENDS ${VULKAN_FIXTURES}/vulkan32.a)
                        add_custom_command(OUTPUT ${VULKAN_FIXTURES}/foreign-present32.exe
                            COMMAND ${RAVEN_MINGW32_VULKAN} -minline-all-stringops -O2 -nostdlib -fno-stack-protector -mno-stack-arg-probe
                                -I${VULKAN_GENERATED_DIR}/mingw-include
                                -Wl,--entry,_raven_entry,--image-base,0x400000
                                -o ${VULKAN_FIXTURES}/foreign-present32.exe ${PROJECT_SOURCE_DIR}/testdata/native_cpu/foreign_present_client.c
                                ${VULKAN_FIXTURES}/vulkan32.a -lkernel32
                            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_cpu/foreign_present_client.c ${VULKAN_FIXTURES}/vulkan32.a VERBATIM)
                        add_custom_target(vulkan_foreign32_fixture ALL DEPENDS ${VULKAN_FIXTURES}/foreign-present32.exe)
                        add_dependencies(vulkan_foreign32_fixture vulkan_import32)
                        add_dependencies(foreign_present_test vulkan_foreign32_fixture)
                        add_test(NAME native_vulkan_foreign_present_x32 COMMAND foreign_present_test
                            $<TARGET_FILE:raven-bootstrap> ${VULKAN_FIXTURES}/foreign-present32.exe)
                        # A prefix of its own keeps its window records apart from the other tests that publish windows.
                        set_tests_properties(native_vulkan_foreign_present_x32 PROPERTIES TIMEOUT 60 FAIL_REGULAR_EXPRESSION "Validation Error:|VUID-"
                            ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/foreign-present-x32-prefix)
                        add_custom_command(OUTPUT ${VULKAN_FIXTURES}/descriptor-buffer32.exe
                            COMMAND ${RAVEN_MINGW32_VULKAN} -minline-all-stringops -O2 -nostdlib -fno-stack-protector -mno-stack-arg-probe
                                -I${VULKAN_GENERATED_DIR}/mingw-include -I${PROJECT_SOURCE_DIR}/testdata/native_cpu
                                -Wl,--entry,_raven_entry,--image-base,0x400000
                                -o ${VULKAN_FIXTURES}/descriptor-buffer32.exe ${PROJECT_SOURCE_DIR}/testdata/native_cpu/descriptor_buffer_client.c
                                ${VULKAN_FIXTURES}/vulkan32.a
                            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_cpu/descriptor_buffer_client.c
                                ${PROJECT_SOURCE_DIR}/testdata/native_cpu/vulkan_compute_spirv.h ${VULKAN_FIXTURES}/vulkan32.a VERBATIM)
                        add_custom_target(vulkan_descriptor32_fixture ALL DEPENDS ${VULKAN_FIXTURES}/descriptor-buffer32.exe)
                        add_dependencies(vulkan_descriptor32_fixture vulkan_import32)
                        add_test(NAME native_vulkan_descriptor_buffer_x32 COMMAND raven --run-process --execution=native ${VULKAN_FIXTURES}/descriptor-buffer32.exe)
                        set_tests_properties(native_vulkan_descriptor_buffer_x32 PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 " FAIL_REGULAR_EXPRESSION "Validation Error:|VUID-")
                        add_custom_command(OUTPUT ${VULKAN_FIXTURES}/extensions32.exe
                            COMMAND ${RAVEN_MINGW32_VULKAN} -minline-all-stringops -O2 -nostdlib -fno-stack-protector -mno-stack-arg-probe
                                -I${VULKAN_GENERATED_DIR}/mingw-include -I${PROJECT_SOURCE_DIR}/testdata/native_cpu
                                -Wl,--entry,_raven_entry,--image-base,0x400000
                                -o ${VULKAN_FIXTURES}/extensions32.exe ${PROJECT_SOURCE_DIR}/testdata/native_cpu/vulkan_extensions_client.c
                                ${VULKAN_FIXTURES}/vulkan32.a
                            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_cpu/vulkan_extensions_client.c
                                ${PROJECT_SOURCE_DIR}/testdata/native_cpu/vulkan_compute_spirv.h ${VULKAN_FIXTURES}/vulkan32.a VERBATIM)
                        add_custom_target(vulkan_extensions32_fixture ALL DEPENDS ${VULKAN_FIXTURES}/extensions32.exe)
                        add_dependencies(vulkan_extensions32_fixture vulkan_import32)
                        add_test(NAME native_vulkan_extensions_x32 COMMAND raven --run-process --execution=native ${VULKAN_FIXTURES}/extensions32.exe)
                        set_tests_properties(native_vulkan_extensions_x32 PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 " FAIL_REGULAR_EXPRESSION "Validation Error:|VUID-")
                    endif()
                endif()
            endif()
        endif()
        set(CALLBACK_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/callback-fixtures)
        set(CALLBACK_SOURCES ${PROJECT_SOURCE_DIR}/testdata/native_cpu)
        add_custom_command(OUTPUT ${CALLBACK_FIXTURES}/ravenprobe.dll ${CALLBACK_FIXTURES}/probe.a
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CALLBACK_FIXTURES}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -shared -fno-stack-protector
                -Wl,--entry,0,--image-base,0x183000000
                -Wl,--out-implib,${CALLBACK_FIXTURES}/probe.a
                -o ${CALLBACK_FIXTURES}/ravenprobe.dll
                ${CALLBACK_SOURCES}/callback_probe.c ${CALLBACK_SOURCES}/callback_probe.def
            DEPENDS ${CALLBACK_SOURCES}/callback_probe.c ${CALLBACK_SOURCES}/callback_probe.def
            VERBATIM)
        add_custom_command(OUTPUT ${CALLBACK_FIXTURES}/client.exe
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x184000000
                -o ${CALLBACK_FIXTURES}/client.exe ${CALLBACK_SOURCES}/callback_client.c ${CALLBACK_FIXTURES}/probe.a
            DEPENDS ${CALLBACK_SOURCES}/callback_client.c ${CALLBACK_FIXTURES}/probe.a
            VERBATIM)
        add_custom_target(callback_pe_fixtures DEPENDS ${CALLBACK_FIXTURES}/client.exe ${CALLBACK_FIXTURES}/ravenprobe.dll)
        add_executable(callback_fixture_test cpu/callback_fixture_test.cpp)
        target_link_libraries(callback_fixture_test PRIVATE raven_cpu)
        add_dependencies(callback_fixture_test callback_pe_fixtures raven-bootstrap)
        add_test(NAME compiled_callback_contract COMMAND callback_fixture_test $<TARGET_FILE:raven-bootstrap>
            ${CALLBACK_FIXTURES}/client.exe ${CALLBACK_FIXTURES}/ravenprobe.dll)
        set_tests_properties(compiled_callback_contract PROPERTIES TIMEOUT 15)

        add_custom_command(OUTPUT ${CALLBACK_FIXTURES}/kernel32.dll ${CALLBACK_FIXTURES}/environment.a
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -shared -fno-stack-protector
                -Wl,--entry,0,--image-base,0x186000000
                -Wl,--out-implib,${CALLBACK_FIXTURES}/environment.a
                -o ${CALLBACK_FIXTURES}/kernel32.dll
                ${CALLBACK_SOURCES}/environment_probe.c ${CALLBACK_SOURCES}/environment_probe.def
            DEPENDS ${CALLBACK_SOURCES}/environment_probe.c ${CALLBACK_SOURCES}/environment_probe.def
            VERBATIM)
        add_custom_command(OUTPUT ${CALLBACK_FIXTURES}/environment.exe
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x185000000
                -o ${CALLBACK_FIXTURES}/environment.exe ${CALLBACK_SOURCES}/environment_client.c
                ${CALLBACK_FIXTURES}/environment.a
            DEPENDS ${CALLBACK_SOURCES}/environment_client.c ${CALLBACK_FIXTURES}/environment.a
            VERBATIM)
        add_custom_command(OUTPUT ${CALLBACK_FIXTURES}/api_loop.exe
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x187000000
                -o ${CALLBACK_FIXTURES}/api_loop.exe ${CALLBACK_SOURCES}/api_loop.c ${CALLBACK_FIXTURES}/environment.a
            DEPENDS ${CALLBACK_SOURCES}/api_loop.c ${CALLBACK_FIXTURES}/environment.a
            VERBATIM)
        add_custom_target(environment_pe_fixtures DEPENDS ${CALLBACK_FIXTURES}/environment.exe
            ${CALLBACK_FIXTURES}/kernel32.dll ${CALLBACK_FIXTURES}/api_loop.exe)
        add_executable(environment_test nt/environment_test.cpp)
        target_link_libraries(environment_test PRIVATE raven_cpu)
        add_dependencies(environment_test environment_pe_fixtures raven-bootstrap)
        add_test(NAME native_environment_contract COMMAND environment_test $<TARGET_FILE:raven-bootstrap>
            ${CALLBACK_FIXTURES}/environment.exe ${CALLBACK_FIXTURES}/kernel32.dll)
        set_tests_properties(native_environment_contract PROPERTIES TIMEOUT 15)
        add_test(NAME native_process_cli COMMAND raven --run-process --execution=native ${CALLBACK_FIXTURES}/environment.exe)
        set_tests_properties(native_process_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")

        set(MODULE_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/module-fixtures)
        set(LOADER_SOURCES ${PROJECT_SOURCE_DIR}/testdata/native_loader)
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/arguments.exe
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x18e000000
                -o ${CMAKE_CURRENT_BINARY_DIR}/arguments.exe ${LOADER_SOURCES}/arguments_client.c -lkernel32
            DEPENDS ${LOADER_SOURCES}/arguments_client.c VERBATIM)
        add_custom_target(arguments_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/arguments.exe)
        add_test(NAME native_guest_arguments_cli COMMAND raven --run-process --execution=native
            ${CMAKE_CURRENT_BINARY_DIR}/arguments.exe -- "" plain "with space" "a\"b" "C:\\tail\\" --)
        set_tests_properties(native_guest_arguments_cli PROPERTIES TIMEOUT 15)
        # Kernel contract fixtures check their own results and return a distinct non-zero code for each failed expectation.
        find_program(RAVEN_CLANG clang)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        function(raven_kernel_fixture name source base libraries)
            set(output ${CMAKE_CURRENT_BINARY_DIR}/${name}.exe)
            add_custom_command(OUTPUT ${output}
                COMMAND ${RAVEN_MINGW64} -O1 -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe
                    -Wl,--entry,raven_entry,--image-base,${base}
                    -o ${output} ${LOADER_SOURCES}/${source} ${libraries}
                DEPENDS ${LOADER_SOURCES}/${source} VERBATIM)
            add_custom_target(${name}_fixture ALL DEPENDS ${output})
        endfunction()
        raven_kernel_fixture(kernel-file kernel_files_client.c 0x1b1000000 "-lkernel32")
        raven_kernel_fixture(kernel-ansi kernel_ansi_client.c 0x1b5000000 "-lkernel32")
        raven_kernel_fixture(kernel-threads kernel_threads_client.c 0x1b2000000 "-lkernel32")
        raven_kernel_fixture(served-semaphore served_semaphore_client.c 0x1b8000000 "-lkernel32")
        # A run whose threads all wait past its time limit ends at that limit.
        raven_kernel_fixture(timeout-waits timeout_waits_client.c 0x1b8000000 "-lkernel32")
        add_test(NAME native_timeout_waits COMMAND raven --run-process --execution=native --timeout-ms=2000
            ${CMAKE_CURRENT_BINARY_DIR}/timeout-waits.exe)
        set_tests_properties(native_timeout_waits PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "native entry timed out"
            ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/timeout-waits-prefix")
        foreach(mode default parallel)
            add_test(NAME native_served_semaphore_${mode} COMMAND raven --run-process --execution=native
                ${CMAKE_CURRENT_BINARY_DIR}/served-semaphore.exe)
            set_tests_properties(native_served_semaphore_${mode} PROPERTIES TIMEOUT 30
                PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/served-semaphore-prefix-${mode}")
        endforeach()
        set_property(TEST native_served_semaphore_parallel APPEND PROPERTY ENVIRONMENT RAVEN_PARALLEL=1)
        raven_kernel_fixture(api-hook api_hook_client.c 0x1b6000000 "-lkernel32;-luser32")
        raven_kernel_fixture(guest-answers guest_answers_client.c 0x1bf000000 "-lkernel32;-luser32;-lole32")
        foreach(mode default parallel)
            add_test(NAME native_guest_answers_${mode} COMMAND raven --run-process --execution=native
                ${CMAKE_CURRENT_BINARY_DIR}/guest-answers.exe)
            set_tests_properties(native_guest_answers_${mode} PROPERTIES TIMEOUT 30
                PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/guest-answers-prefix-${mode}")
        endforeach()
        set_property(TEST native_guest_answers_parallel APPEND PROPERTY ENVIRONMENT RAVEN_PARALLEL=1)
        raven_kernel_fixture(queued-calls queued_calls_client.c 0x1b7000000 "")
        add_test(NAME native_queued_served_contract COMMAND providers_test $<TARGET_FILE:raven-bootstrap>
            --queued-served ${CMAKE_CURRENT_BINARY_DIR}/queued-calls.exe)
        set_tests_properties(native_queued_served_contract PROPERTIES TIMEOUT 30 ENVIRONMENT "RAVEN_PARALLEL=1;RAVEN_QUEUED_GRAPHICS=1;RAVEN_QUEUED_OPENGL=1")
        foreach(mode default parallel)
            add_test(NAME native_apc_nonalertable_${mode} COMMAND providers_test $<TARGET_FILE:raven-bootstrap> --apc-wait)
            set_tests_properties(native_apc_nonalertable_${mode} PROPERTIES TIMEOUT 10)
        endforeach()
        set_tests_properties(native_apc_nonalertable_parallel PROPERTIES ENVIRONMENT RAVEN_PARALLEL=1)
        add_test(NAME native_api_hook COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/api-hook.exe)
        set_tests_properties(native_api_hook PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        set(PARALLEL_CLIENTS)
        foreach(width 64 32)
            if(width EQUAL 64)
                set(parallel_compiler ${RAVEN_MINGW64})
                set(parallel_entry raven_entry)
                set(parallel_base 0x199800000)
            else()
                set(parallel_compiler ${RAVEN_MINGW32})
                set(parallel_entry _raven_entry)
                set(parallel_base 0x19980000)
            endif()
            if(parallel_compiler)
                foreach(kind guest suspend remote lifecycle served wait)
                    set(parallel_client ${CMAKE_CURRENT_BINARY_DIR}/parallel-${kind}${width}.exe)
                    add_custom_command(OUTPUT ${parallel_client}
                        COMMAND ${parallel_compiler} -O2 -nostdlib -fno-builtin -fno-stack-protector
                            -Wl,--entry,${parallel_entry},--image-base,${parallel_base},--disable-dynamicbase,--no-insert-timestamp
                            -o ${parallel_client} ${LOADER_SOURCES}/parallel_${kind}_client.c -lkernel32
                        DEPENDS ${LOADER_SOURCES}/parallel_${kind}_client.c VERBATIM)
                    list(APPEND PARALLEL_CLIENTS ${parallel_client})
                    if(kind STREQUAL lifecycle)
                        if(Python3_Interpreter_FOUND)
                            set(parallel_sampling)
                            if(width EQUAL 64)
                                set(parallel_sampling --sample)
                            endif()
                            add_test(NAME native_parallel_${kind}_x${width} COMMAND ${Python3_EXECUTABLE}
                                ${PROJECT_SOURCE_DIR}/scripts/native-parallel-lifecycle.py $<TARGET_FILE:raven> ${parallel_client} ${parallel_sampling})
                        endif()
                    else()
                        add_test(NAME native_parallel_${kind}_x${width} COMMAND raven --run-process --execution=native
                            --timeout-ms=12000 ${parallel_client})
                        set_tests_properties(native_parallel_${kind}_x${width} PROPERTIES PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    endif()
                    if(NOT kind STREQUAL lifecycle OR Python3_Interpreter_FOUND)
                        set_tests_properties(native_parallel_${kind}_x${width} PROPERTIES TIMEOUT 30
                            ENVIRONMENT "RAVEN_PARALLEL=1;RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/parallel-${kind}-prefix${width}")
                    endif()
                endforeach()
            endif()
        endforeach()
        add_custom_target(parallel_fixtures ALL DEPENDS ${PARALLEL_CLIENTS})
        raven_kernel_fixture(kernel-shlwapi kernel_shlwapi_client.c 0x1b4000000 "-lkernel32;-lshlwapi")
        raven_kernel_fixture(kernel-heap-stress kernel_heap_stress_client.c 0x1b6000000 "-lkernel32")
        add_test(NAME native_parallel_heap COMMAND raven --run-process --execution=native --timeout-ms=120000
            ${CMAKE_CURRENT_BINARY_DIR}/kernel-heap-stress.exe)
        set_tests_properties(native_parallel_heap PROPERTIES TIMEOUT 150 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
            ENVIRONMENT "RAVEN_PARALLEL=1;RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/parallel-heap-prefix")
        set(KERNEL_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/kernel-prefix)
        add_test(NAME native_kernel_files COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/kernel-file.exe)
        add_test(NAME native_kernel_ansi COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/kernel-ansi.exe)
        add_test(NAME native_kernel_threads COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/kernel-threads.exe)
        add_test(NAME native_kernel_threads_parallel COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/kernel-threads.exe)
        set_tests_properties(native_kernel_threads_parallel PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
            ENVIRONMENT "RAVEN_PARALLEL=1;RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/kernel-threads-parallel-prefix")
        add_test(NAME native_kernel_shlwapi COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/kernel-shlwapi.exe)
        add_test(NAME native_kernel_heap_stress COMMAND raven --run-process --execution=native --timeout-ms=120000 ${CMAKE_CURRENT_BINARY_DIR}/kernel-heap-stress.exe)
        set_tests_properties(native_kernel_files native_kernel_ansi native_kernel_threads native_kernel_shlwapi native_kernel_heap_stress PROPERTIES TIMEOUT 150
            PASS_REGULAR_EXPRESSION "Native entry returned 0 " ENVIRONMENT RAVEN_PREFIX=${KERNEL_PREFIX})
        add_test(NAME native_kernel_file_persistence COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check-file-persistence.py ${KERNEL_PREFIX})
        set_tests_properties(native_kernel_file_persistence PROPERTIES TIMEOUT 15 DEPENDS native_kernel_files)
        set(RAVEN_CLANG_MINGW_INCLUDE)
        if(RAVEN_CLANG)
            # Some distributions' clang does not search the MinGW headers the MinGW gcc uses: hand it the directory that holds windows.h.
            set(clang_probe ${CMAKE_CURRENT_BINARY_DIR}/clang-mingw-probe.c)
            file(WRITE ${clang_probe} "#include <windows.h>\n")
            execute_process(COMMAND ${RAVEN_CLANG} --target=x86_64-w64-mingw32 -fsyntax-only ${clang_probe}
                RESULT_VARIABLE clang_headers OUTPUT_QUIET ERROR_QUIET)
            if(NOT clang_headers EQUAL 0)
                execute_process(COMMAND ${RAVEN_MINGW64} -xc -E -Wp,-v ${clang_probe}
                    OUTPUT_QUIET ERROR_VARIABLE mingw_search)
                string(REGEX MATCHALL "\n [^\n]+" mingw_directories "${mingw_search}")
                foreach(directory ${mingw_directories})
                    string(STRIP "${directory}" directory)
                    if(EXISTS ${directory}/windows.h)
                        set(RAVEN_CLANG_MINGW_INCLUDE -isystem ${directory})
                        break()
                    endif()
                endforeach()
                execute_process(COMMAND ${RAVEN_CLANG} --target=x86_64-w64-mingw32 ${RAVEN_CLANG_MINGW_INCLUDE} -fsyntax-only ${clang_probe}
                    RESULT_VARIABLE clang_headers OUTPUT_QUIET ERROR_QUIET)
                if(NOT clang_headers EQUAL 0)
                    message(STATUS "clang cannot compile against the MinGW headers; the structured-exception test is skipped")
                endif()
            endif()
        endif()
        if(RAVEN_CLANG AND clang_headers EQUAL 0)
            # The structured-exception fixture needs the compiler's __try support; the program supplies its own scope-table handler.
            set(SEH_OBJECT ${CMAKE_CURRENT_BINARY_DIR}/kernel-seh.o)
            add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/kernel-seh.exe
                COMMAND ${RAVEN_CLANG} --target=x86_64-w64-mingw32 ${RAVEN_CLANG_MINGW_INCLUDE} -O0 -fms-extensions -fseh-exceptions -Xclang -fasync-exceptions -Wno-inconsistent-dllimport -fno-stack-protector -mno-stack-arg-probe -c
                    -o ${SEH_OBJECT} ${LOADER_SOURCES}/kernel_seh_client.c
                COMMAND ${RAVEN_MINGW64} -nostdlib -Wl,--entry,raven_entry,--image-base,0x1b3000000
                    -o ${CMAKE_CURRENT_BINARY_DIR}/kernel-seh.exe ${SEH_OBJECT} -lkernel32
                DEPENDS ${LOADER_SOURCES}/kernel_seh_client.c VERBATIM)
            add_custom_target(kernel_seh_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/kernel-seh.exe)
            add_test(NAME native_kernel_seh COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/kernel-seh.exe)
            set_tests_properties(native_kernel_seh PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT RAVEN_PREFIX=${KERNEL_PREFIX})
        endif()
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/startup-info.exe
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -ffreestanding -fno-builtin -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x18c000000
                -o ${CMAKE_CURRENT_BINARY_DIR}/startup-info.exe ${LOADER_SOURCES}/startup_info_client.c -lkernel32
            DEPENDS ${LOADER_SOURCES}/startup_info_client.c VERBATIM)
        add_custom_target(startup_info_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/startup-info.exe)
        add_executable(startup_test nt/startup_test.cpp)
        target_link_libraries(startup_test PRIVATE raven_cpu)
        add_dependencies(startup_test startup_info_fixture raven-bootstrap)
        add_test(NAME native_startup_information COMMAND startup_test $<TARGET_FILE:raven-bootstrap> ${CMAKE_CURRENT_BINARY_DIR}/startup-info.exe)
        set_tests_properties(native_startup_information PROPERTIES TIMEOUT 30)
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/file-client.exe
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--no-insert-timestamp
                -o ${CMAKE_CURRENT_BINARY_DIR}/file-client.exe ${LOADER_SOURCES}/file_client.c -lkernel32
            DEPENDS ${LOADER_SOURCES}/file_client.c VERBATIM)
        add_custom_target(file_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/file-client.exe)
        add_executable(file_test nt/file_test.cpp)
        target_link_libraries(file_test PRIVATE raven_cpu)
        add_dependencies(file_test file_fixture raven-bootstrap)
        add_test(NAME native_standard_files COMMAND file_test $<TARGET_FILE:raven-bootstrap>
            ${CMAKE_CURRENT_BINARY_DIR}/file-client.exe ${CMAKE_CURRENT_BINARY_DIR}/file-contract.bin)
        set_tests_properties(native_standard_files PROPERTIES TIMEOUT 15)
        # The console over a pseudo-terminal: modes, colours, the cursor, line and key input, CONOUT$, and Ctrl+C handlers.
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/console-client.exe
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-builtin -fno-stack-protector
                -Wl,--entry,raven_entry,--no-insert-timestamp
                -o ${CMAKE_CURRENT_BINARY_DIR}/console-client.exe ${LOADER_SOURCES}/console_client.c -lkernel32
            DEPENDS ${LOADER_SOURCES}/console_client.c VERBATIM)
        add_custom_target(console_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/console-client.exe)
        add_executable(console_test nt/console_test.cpp)
        target_link_libraries(console_test PRIVATE raven_cpu util)
        add_dependencies(console_test console_fixture raven)
        add_test(NAME native_console COMMAND console_test $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/console-client.exe)
        set_tests_properties(native_console PROPERTIES TIMEOUT 60)
        # The command processor, built for each width beside the runtime, which copies it into every prefix's system folders.
        set(RAVEN_CMD_SOURCE ${PROJECT_SOURCE_DIR}/native/guest/raven-cmd/cmd.c)
        set(RAVEN_CMD_FLAGS -O2 -nostdlib -fno-builtin -fno-tree-loop-distribute-patterns -fno-stack-protector)
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/cmd.exe
            COMMAND ${RAVEN_MINGW64} ${RAVEN_CMD_FLAGS} -Wl,--entry,raven_cmd_entry,--no-insert-timestamp
                -o ${CMAKE_CURRENT_BINARY_DIR}/cmd.exe ${RAVEN_CMD_SOURCE} -lkernel32 -lgcc
            DEPENDS ${RAVEN_CMD_SOURCE} VERBATIM)
        set(RAVEN_CMD_OUTPUTS ${CMAKE_CURRENT_BINARY_DIR}/cmd.exe)
        find_program(RAVEN_MINGW32 i686-w64-mingw32-gcc)
        if(RAVEN_MINGW32)
            add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86/cmd.exe
                COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/x86
                COMMAND ${RAVEN_MINGW32} ${RAVEN_CMD_FLAGS} -Wl,--entry,_raven_cmd_entry,--no-insert-timestamp
                    -o ${CMAKE_CURRENT_BINARY_DIR}/x86/cmd.exe ${RAVEN_CMD_SOURCE} -lkernel32 -lgcc
                DEPENDS ${RAVEN_CMD_SOURCE} VERBATIM)
            list(APPEND RAVEN_CMD_OUTPUTS ${CMAKE_CURRENT_BINARY_DIR}/x86/cmd.exe)
        endif()
        # The command-line tools a prefix has beside cmd, one program each as Windows has them, for each width; MORE is a .com.
        set(RAVEN_TOOLS_DIRECTORY ${PROJECT_SOURCE_DIR}/native/guest/raven-tools)
        set(RAVEN_TOOL_HEADERS ${RAVEN_TOOLS_DIRECTORY}/tool.h ${RAVEN_TOOLS_DIRECTORY}/processes.h)
        foreach(tool find findstr more sort where xcopy attrib timeout choice whoami hostname tasklist taskkill reg rundll32 regsvr32)
            set(tool_file ${tool}.exe)
            if(tool STREQUAL "more")
                set(tool_file more.com)
            endif()
            add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${tool_file}
                COMMAND ${RAVEN_MINGW64} ${RAVEN_CMD_FLAGS} -Wl,--entry,raven_tool_entry,--no-insert-timestamp
                    -o ${CMAKE_CURRENT_BINARY_DIR}/${tool_file} ${RAVEN_TOOLS_DIRECTORY}/${tool}.c -lkernel32 -ladvapi32 -lgcc
                DEPENDS ${RAVEN_TOOLS_DIRECTORY}/${tool}.c ${RAVEN_TOOL_HEADERS} VERBATIM)
            list(APPEND RAVEN_CMD_OUTPUTS ${CMAKE_CURRENT_BINARY_DIR}/${tool_file})
            if(RAVEN_MINGW32)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86/${tool_file}
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/x86
                    COMMAND ${RAVEN_MINGW32} ${RAVEN_CMD_FLAGS} -Wl,--entry,_raven_tool_entry,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/x86/${tool_file} ${RAVEN_TOOLS_DIRECTORY}/${tool}.c -lkernel32 -ladvapi32 -lgcc
                    DEPENDS ${RAVEN_TOOLS_DIRECTORY}/${tool}.c ${RAVEN_TOOL_HEADERS} VERBATIM)
                list(APPEND RAVEN_CMD_OUTPUTS ${CMAKE_CURRENT_BINARY_DIR}/x86/${tool_file})
            endif()
        endforeach()
        # The script host, for the console and with windows, from one source; it hosts the script engines through COM.
        set(RAVEN_WSH_LIBRARIES -lole32 -loleaut32 -luuid -luser32 -ladvapi32 -lkernel32 -lgcc)
        foreach(host cscript wscript)
            set(host_flags)
            if(host STREQUAL "wscript")
                set(host_flags -DWSH_WINDOWED -mwindows)
            endif()
            add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${host}.exe
                COMMAND ${RAVEN_MINGW64} ${RAVEN_CMD_FLAGS} ${host_flags} -Wl,--entry,raven_tool_entry,--no-insert-timestamp
                    -o ${CMAKE_CURRENT_BINARY_DIR}/${host}.exe ${RAVEN_TOOLS_DIRECTORY}/wsh.c ${RAVEN_WSH_LIBRARIES}
                DEPENDS ${RAVEN_TOOLS_DIRECTORY}/wsh.c ${RAVEN_TOOL_HEADERS} VERBATIM)
            list(APPEND RAVEN_CMD_OUTPUTS ${CMAKE_CURRENT_BINARY_DIR}/${host}.exe)
            if(RAVEN_MINGW32)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86/${host}.exe
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/x86
                    COMMAND ${RAVEN_MINGW32} ${RAVEN_CMD_FLAGS} ${host_flags} -Wl,--entry,_raven_tool_entry,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/x86/${host}.exe ${RAVEN_TOOLS_DIRECTORY}/wsh.c ${RAVEN_WSH_LIBRARIES}
                    DEPENDS ${RAVEN_TOOLS_DIRECTORY}/wsh.c ${RAVEN_TOOL_HEADERS} VERBATIM)
                list(APPEND RAVEN_CMD_OUTPUTS ${CMAKE_CURRENT_BINARY_DIR}/x86/${host}.exe)
            endif()
        endforeach()
        add_custom_target(raven_cmd ALL DEPENDS ${RAVEN_CMD_OUTPUTS})
        add_dependencies(raven raven_cmd)
        # rundll32 reaches a library's entry by name, its wide form first, with the rest of the command line.
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/rundll32-fixture.dll
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -shared -fno-stack-protector -Wl,--entry,raven_library_entry,--no-insert-timestamp
                -o ${CMAKE_CURRENT_BINARY_DIR}/rundll32-fixture.dll ${LOADER_SOURCES}/rundll32_fixture.c -lkernel32
            DEPENDS ${LOADER_SOURCES}/rundll32_fixture.c VERBATIM)
        add_custom_target(rundll32_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/rundll32-fixture.dll)
        add_test(NAME native_rundll32 COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/rundll32.exe -- rundll32-fixture.dll,Report 7)
        set_tests_properties(native_rundll32 PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "exited with status 47[^0-9]")
        add_test(NAME native_rundll32_wide COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/rundll32.exe -- rundll32-fixture.dll,Reported 7)
        set_tests_properties(native_rundll32_wide PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "exited with status 57[^0-9]")
        # regsvr32 tells the outcomes apart by its exit code.
        add_test(NAME native_regsvr32 COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/regsvr32.exe -- /s rundll32-fixture.dll)
        set_tests_properties(native_regsvr32 PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "exited with status 0[^0-9]")
        add_test(NAME native_regsvr32_install COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/regsvr32.exe -- /s /i:line /n rundll32-fixture.dll)
        set_tests_properties(native_regsvr32_install PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "exited with status 5[^0-9]")
        add_test(NAME native_regsvr32_missing COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/regsvr32.exe -- /s no-such-library.dll)
        set_tests_properties(native_regsvr32_missing PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "exited with status 3[^0-9]")
        # Batch files through CreateProcess, cmd /c, and commands piped to the prompt, for 64-bit and 32-bit callers.
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/cmd-client.exe
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-builtin -fno-tree-loop-distribute-patterns -fno-stack-protector
                -Wl,--entry,raven_entry,--no-insert-timestamp
                -o ${CMAKE_CURRENT_BINARY_DIR}/cmd-client.exe ${LOADER_SOURCES}/cmd_client.c -lkernel32 -lgcc
            DEPENDS ${LOADER_SOURCES}/cmd_client.c VERBATIM)
        set(RAVEN_CMD_CLIENTS ${CMAKE_CURRENT_BINARY_DIR}/cmd-client.exe)
        add_test(NAME native_cmd COMMAND raven --run-process --execution=native --timeout-ms=60000 ${CMAKE_CURRENT_BINARY_DIR}/cmd-client.exe)
        set_tests_properties(native_cmd PROPERTIES TIMEOUT 90 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        if(RAVEN_MINGW32)
            add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86-cmd-client.exe
                COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-builtin -fno-tree-loop-distribute-patterns -fno-stack-protector
                    -Wl,--entry,_raven_entry,--no-insert-timestamp
                    -o ${CMAKE_CURRENT_BINARY_DIR}/x86-cmd-client.exe ${LOADER_SOURCES}/cmd_client.c -lkernel32 -lgcc
                DEPENDS ${LOADER_SOURCES}/cmd_client.c VERBATIM)
            list(APPEND RAVEN_CMD_CLIENTS ${CMAKE_CURRENT_BINARY_DIR}/x86-cmd-client.exe)
            add_test(NAME native_cmd_x32 COMMAND raven --run-process --execution=native --timeout-ms=60000
                ${CMAKE_CURRENT_BINARY_DIR}/x86-cmd-client.exe)
            set_tests_properties(native_cmd_x32 PROPERTIES TIMEOUT 90 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        endif()
        add_custom_target(cmd_fixture ALL DEPENDS ${RAVEN_CMD_CLIENTS})
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            find_program(RAVEN_MINGW32 i686-w64-mingw32-gcc)
            if(RAVEN_MINGW32)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86-client.exe
                    COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector -fno-builtin
                        -Wl,--entry,_raven_entry,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/x86-client.exe ${LOADER_SOURCES}/x86_client.c -lkernel32
                    DEPENDS ${LOADER_SOURCES}/x86_client.c VERBATIM)
                add_custom_target(x86_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/x86-client.exe)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86-kernel-client.exe
                    COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector -fno-builtin
                        -Wl,--entry,_raven_entry,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/x86-kernel-client.exe ${LOADER_SOURCES}/x86_kernel_client.c -lkernel32
                    DEPENDS ${LOADER_SOURCES}/x86_kernel_client.c VERBATIM)
                add_custom_target(x86_kernel_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/x86-kernel-client.exe)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86-thread-client.exe
                    COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector -fno-builtin
                        -Wl,--entry,_raven_entry,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/x86-thread-client.exe ${LOADER_SOURCES}/x86_thread_client.c -lkernel32
                    DEPENDS ${LOADER_SOURCES}/x86_thread_client.c VERBATIM)
                add_custom_target(x86_thread_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/x86-thread-client.exe)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/kernel-threads32.exe
                    COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector -fno-builtin
                        -Wl,--entry,_raven_entry,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/kernel-threads32.exe ${LOADER_SOURCES}/kernel_threads_client.c -lkernel32
                    DEPENDS ${LOADER_SOURCES}/kernel_threads_client.c VERBATIM)
                add_custom_target(kernel_threads32_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/kernel-threads32.exe)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/served-semaphore32.exe
                    COMMAND ${RAVEN_MINGW32} -O1 -nostdlib -fno-stack-protector -fno-builtin
                        -Wl,--entry,_raven_entry,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/served-semaphore32.exe ${LOADER_SOURCES}/served_semaphore_client.c -lkernel32
                    DEPENDS ${LOADER_SOURCES}/served_semaphore_client.c VERBATIM)
                add_custom_target(served_semaphore32_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/served-semaphore32.exe)
                foreach(mode default parallel)
                    add_test(NAME native_served_semaphore_x32_${mode} COMMAND raven --run-process --execution=native
                        ${CMAKE_CURRENT_BINARY_DIR}/served-semaphore32.exe)
                    set_tests_properties(native_served_semaphore_x32_${mode} PROPERTIES TIMEOUT 30
                        PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                        ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/served-semaphore32-prefix-${mode}")
                endforeach()
                set_property(TEST native_served_semaphore_x32_parallel APPEND PROPERTY ENVIRONMENT RAVEN_PARALLEL=1)
                foreach(mode default parallel)
                    add_test(NAME native_kernel_threads_x32_${mode} COMMAND raven --run-process --execution=native
                        ${CMAKE_CURRENT_BINARY_DIR}/kernel-threads32.exe)
                    set_tests_properties(native_kernel_threads_x32_${mode} PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                        ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/kernel-threads32-${mode}-prefix")
                endforeach()
                set_tests_properties(native_kernel_threads_x32_parallel PROPERTIES ENVIRONMENT
                    "RAVEN_PARALLEL=1;RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/kernel-threads32-parallel-prefix")
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86-socket-message-client.exe
                    COMMAND ${RAVEN_MINGW32} -nostdlib -O1 -fno-builtin -fno-stack-protector
                        -Wl,--entry,_raven_entry,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/x86-socket-message-client.exe
                        ${LOADER_SOURCES}/socket_message_client.c -lkernel32 -lws2_32
                    DEPENDS ${LOADER_SOURCES}/socket_message_client.c VERBATIM)
                add_custom_target(x86_socket_message_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/x86-socket-message-client.exe)
                add_test(NAME native_socket_messages_x32 COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/x86-socket-message-client.exe)
                set_tests_properties(native_socket_messages_x32 PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                if(OPENSSL_FOUND)
                    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86-winhttp-client.exe
                        COMMAND ${RAVEN_MINGW32} -nostdlib -O1 -fno-builtin -fno-stack-protector
                            -Wl,--entry,_raven_entry,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/x86-winhttp-client.exe
                            ${LOADER_SOURCES}/winhttp_client.c -lwinhttp -lnormaliz -lole32 -luuid -lws2_32 -lkernel32
                        DEPENDS ${LOADER_SOURCES}/winhttp_client.c VERBATIM)
                    add_custom_target(x86_winhttp_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/x86-winhttp-client.exe)
                    add_test(NAME native_winhttp_x32 COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/x86-winhttp-client.exe)
                    set_tests_properties(native_winhttp_x32 PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                endif()
                # Dark visual style variants, the dark mode functions taken by number, and the dialog, menu and tooltip parts.
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86-theme-client.exe
                    COMMAND ${RAVEN_MINGW32} -nostdlib -O1 -fno-builtin -fno-stack-protector
                        -Wl,--entry,_raven_entry,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/x86-theme-client.exe
                        ${LOADER_SOURCES}/theme_client.c -luxtheme -lgdi32 -luser32 -lkernel32
                    DEPENDS ${LOADER_SOURCES}/theme_client.c VERBATIM)
                add_custom_target(x86_theme_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/x86-theme-client.exe)
                add_test(NAME native_theme_dark_x32 COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/x86-theme-client.exe)
                set_tests_properties(native_theme_dark_x32 PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                # AES-GMAC and the secp256k1 curve through CNG, as SSH clients use them.
                if(OPENSSL_FOUND)
                    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86-bcrypt-client.exe
                        COMMAND ${RAVEN_MINGW32} -nostdlib -O1 -fno-builtin -fno-stack-protector
                            -Wl,--entry,_raven_entry,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/x86-bcrypt-client.exe
                            ${LOADER_SOURCES}/bcrypt_client.c -lbcrypt -lkernel32
                        DEPENDS ${LOADER_SOURCES}/bcrypt_client.c VERBATIM)
                    add_custom_target(x86_bcrypt_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/x86-bcrypt-client.exe)
                    add_test(NAME native_bcrypt_x32 COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/x86-bcrypt-client.exe)
                    set_tests_properties(native_bcrypt_x32 PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                endif()
                add_test(NAME native_x86_threads COMMAND raven --run-process --execution=native
                    ${CMAKE_CURRENT_BINARY_DIR}/x86-thread-client.exe)
                set_tests_properties(native_x86_threads PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86-font-family-client.exe
                    COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector -fno-builtin
                        -Wl,--entry,_raven_entry,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/x86-font-family-client.exe ${LOADER_SOURCES}/x86_font_family_client.c -lgdi32 -lkernel32
                    DEPENDS ${LOADER_SOURCES}/x86_font_family_client.c VERBATIM)
                add_custom_target(x86_font_family_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/x86-font-family-client.exe)
                add_test(NAME native_x86_font_families COMMAND raven --run-process --execution=native --timeout-ms=60000
                    ${CMAKE_CURRENT_BINARY_DIR}/x86-font-family-client.exe)
                set_tests_properties(native_x86_font_families PROPERTIES TIMEOUT 90 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86-heap-client.exe
                    COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector -fno-builtin
                        -Wl,--entry,_raven_entry,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/x86-heap-client.exe ${LOADER_SOURCES}/x86_heap_client.c -lkernel32
                    DEPENDS ${LOADER_SOURCES}/x86_heap_client.c VERBATIM)
                add_custom_target(x86_heap_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/x86-heap-client.exe)
                add_test(NAME native_x86_heap COMMAND raven --run-process --execution=native
                    ${CMAKE_CURRENT_BINARY_DIR}/x86-heap-client.exe)
                set_tests_properties(native_x86_heap PROPERTIES TIMEOUT 30 ENVIRONMENT RAVEN_GUEST_HEAP=1 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86-version-client.exe
                    COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector -fno-builtin
                        -Wl,--entry,_raven_entry,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/x86-version-client.exe ${LOADER_SOURCES}/x86_version_client.c -lkernel32 -lversion
                    DEPENDS ${LOADER_SOURCES}/x86_version_client.c VERBATIM)
                add_custom_target(x86_version_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/x86-version-client.exe)
                add_test(NAME native_x86_version COMMAND raven --run-process --execution=native
                    ${CMAKE_CURRENT_BINARY_DIR}/x86-version-client.exe)
                set_tests_properties(native_x86_version PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                # The installer of a 32-bit redistributable library, whose system directory is SysWOW64.
                set(REDIST32_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/redist-fixtures32)
                add_custom_command(OUTPUT ${REDIST32_FIXTURES}/payload/ravenredist.dll ${REDIST32_FIXTURES}/ravenredist.a
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${REDIST32_FIXTURES}/payload
                    COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -shared -fno-stack-protector -Wl,--entry,_DllMain@12
                        -Wl,--out-implib,${REDIST32_FIXTURES}/ravenredist.a
                        -o ${REDIST32_FIXTURES}/payload/ravenredist.dll ${LOADER_SOURCES}/redist_payload.c
                    DEPENDS ${LOADER_SOURCES}/redist_payload.c VERBATIM)
                add_custom_command(OUTPUT ${REDIST32_FIXTURES}/redist-importer.exe
                    COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector -Wl,--entry,_raven_entry,--no-insert-timestamp
                        -o ${REDIST32_FIXTURES}/redist-importer.exe ${LOADER_SOURCES}/redist_importer.c ${REDIST32_FIXTURES}/ravenredist.a
                    DEPENDS ${LOADER_SOURCES}/redist_importer.c ${REDIST32_FIXTURES}/ravenredist.a VERBATIM)
                add_custom_command(OUTPUT ${REDIST32_FIXTURES}/redist-install.exe
                    COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,_raven_entry,--no-insert-timestamp
                        -o ${REDIST32_FIXTURES}/redist-install.exe ${LOADER_SOURCES}/redist_install_client.c -lkernel32
                    DEPENDS ${LOADER_SOURCES}/redist_install_client.c VERBATIM)
                add_custom_target(redist32_fixtures ALL DEPENDS ${REDIST32_FIXTURES}/payload/ravenredist.dll ${REDIST32_FIXTURES}/redist-importer.exe
                    ${REDIST32_FIXTURES}/redist-install.exe)
                add_test(NAME native_x86_redist_system_directory COMMAND raven --run-process --execution=native ${REDIST32_FIXTURES}/redist-install.exe)
                set_tests_properties(native_x86_redist_system_directory PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                    ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/redist-prefix32)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86-d3d9-client.exe
                    COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector -fno-builtin
                        -Wl,--entry,_raven_entry,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/x86-d3d9-client.exe ${LOADER_SOURCES}/x86_d3d9_client.c -lkernel32
                    DEPENDS ${LOADER_SOURCES}/x86_d3d9_client.c VERBATIM)
                add_custom_target(x86_d3d9_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/x86-d3d9-client.exe)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86-dsound-client.exe
                    COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector -fno-builtin
                        -Wl,--entry,_raven_entry,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/x86-dsound-client.exe ${LOADER_SOURCES}/x86_dsound_client.c -lkernel32 -luser32 -ldxguid
                    DEPENDS ${LOADER_SOURCES}/x86_dsound_client.c VERBATIM)
                add_custom_target(x86_dsound_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/x86-dsound-client.exe)
                add_test(NAME native_x86_dsound COMMAND raven --run-process --execution=native
                    ${CMAKE_CURRENT_BINARY_DIR}/x86-dsound-client.exe)
                set_tests_properties(native_x86_dsound PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                # The 32-bit C runtime component sits beside the runtime and is loaded when a 32-bit library imports ucrtbase or an API set.
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ucrtbase32.dll
                    COMMAND ${RAVEN_MINGW32} -c -O2 -w -ffreestanding -fno-builtin -ffile-prefix-map=${PROJECT_SOURCE_DIR}=raven
                        ${PROJECT_SOURCE_DIR}/native/guest/raven-crt/ucrtbase.c -o ${CMAKE_CURRENT_BINARY_DIR}/ucrtbase32.o
                    COMMAND ${RAVEN_MINGW32} -shared -nostdlib -Wl,--entry,_DllMain@12,--image-base,0x1fc00000,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/ucrtbase32.dll ${CMAKE_CURRENT_BINARY_DIR}/ucrtbase32.o ${PROJECT_SOURCE_DIR}/native/guest/raven-crt/ucrtbase32.def -lkernel32 -lgcc
                    DEPENDS ${PROJECT_SOURCE_DIR}/native/guest/raven-crt/ucrtbase32.def ${PROJECT_SOURCE_DIR}/native/guest/raven-crt/ucrtbase.c ${PROJECT_SOURCE_DIR}/native/guest/raven-crt/ucrtbase_float.c ${PROJECT_SOURCE_DIR}/native/guest/raven-crt/ucrtbase_io.c ${PROJECT_SOURCE_DIR}/native/guest/raven-crt/ucrtbase_legacy.c ${PROJECT_SOURCE_DIR}/native/guest/raven-crt/ucrtbase_msvcrt.c ${PROJECT_SOURCE_DIR}/native/guest/raven-crt/ucrtbase_cpp.c ${PROJECT_SOURCE_DIR}/native/guest/raven-crt/ucrtbase_cxx.c ${PROJECT_SOURCE_DIR}/native/guest/raven-crt/ucrtbase_more.c ${PROJECT_SOURCE_DIR}/native/guest/raven-crt/ucrtbase_fiber.c ${PROJECT_SOURCE_DIR}/native/guest/raven-crt/ucrtbase_office.c ${PROJECT_SOURCE_DIR}/native/guest/raven-crt/ucrtbase_vc2022.c VERBATIM)
                add_custom_target(x86_ucrt_component ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/ucrtbase32.dll)
                # mscoree.dll, the system's entry to the .NET Framework, for each width: it hands every call to the shim implementation the
                # installed Framework brings.
                set(mscoree_source ${PROJECT_SOURCE_DIR}/native/guest/raven-mscoree)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/mscoree.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/mscoree.dll
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/x86
                    COMMAND ${RAVEN_MINGW64} -O2 -shared -nostdlib -ffile-prefix-map=${PROJECT_SOURCE_DIR}=raven -Wl,--entry,DllMain,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/mscoree.dll ${mscoree_source}/mscoree.c ${mscoree_source}/mscoree64.def -lkernel32 -luser32 -ladvapi32
                    COMMAND ${RAVEN_MINGW32} -O2 -shared -nostdlib -ffile-prefix-map=${PROJECT_SOURCE_DIR}=raven -Wl,--entry,_DllMain@12,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/x86/mscoree.dll ${mscoree_source}/mscoree.c ${mscoree_source}/mscoree32.def -lkernel32 -luser32 -ladvapi32
                    DEPENDS ${mscoree_source}/mscoree.c ${mscoree_source}/mscoree64.def ${mscoree_source}/mscoree32.def VERBATIM)
                add_custom_target(mscoree_component ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/mscoree.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/mscoree.dll)
                # taskschd.dll, the Task Scheduler's classes, for each width: Raven code on the public headers that keeps tasks as XML under
                # System32\Tasks. Its trace of methods nobody wrote goes to the runtime through ravenhost.dll.
                set(taskschd_source ${PROJECT_SOURCE_DIR}/native/guest/raven-taskschd)
                find_program(RAVEN_DLLTOOL x86_64-w64-mingw32-dlltool)
                find_program(RAVEN_DLLTOOL32 i686-w64-mingw32-dlltool)
                set(taskschd_flags -O2 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -ffreestanding -fno-builtin -shared -nostdlib
                    -ffile-prefix-map=${PROJECT_SOURCE_DIR}=raven)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/taskschd.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/taskschd.dll
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/x86
                    COMMAND ${RAVEN_DLLTOOL} -d ${taskschd_source}/ravenhost64.def -l ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-taskschd64.a
                    COMMAND ${RAVEN_DLLTOOL32} -k -d ${taskschd_source}/ravenhost32.def -l ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-taskschd32.a
                    COMMAND ${RAVEN_MINGW64} ${taskschd_flags} -Wl,--entry,DllMain,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/taskschd.dll
                        ${taskschd_source}/taskschd.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c ${taskschd_source}/taskschd64.def
                        ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-taskschd64.a -loleaut32 -ladvapi32 -lkernel32 -lgcc
                    COMMAND ${RAVEN_MINGW32} ${taskschd_flags} -Wl,--entry,_DllMain@12,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/x86/taskschd.dll
                        ${taskschd_source}/taskschd.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c ${taskschd_source}/taskschd32.def
                        ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-taskschd32.a -loleaut32 -ladvapi32 -lkernel32 -lgcc
                    DEPENDS ${taskschd_source}/taskschd.c ${taskschd_source}/taskschd_tables.h ${taskschd_source}/taskschd64.def ${taskschd_source}/taskschd32.def
                        ${taskschd_source}/ravenhost64.def ${taskschd_source}/ravenhost32.def VERBATIM)
                add_custom_target(taskschd_component ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/taskschd.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/taskschd.dll)
                # webservices.dll, the XML reading of the Web Services API, for each width: Raven code on the public headers.
                set(webservices_source ${PROJECT_SOURCE_DIR}/native/guest/raven-webservices)
                set(webservices_flags -O2 -Wall -Wno-unused-parameter -ffreestanding -fno-builtin -shared -nostdlib -ffile-prefix-map=${PROJECT_SOURCE_DIR}=raven)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/webservices.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/webservices.dll
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/x86
                    COMMAND ${RAVEN_MINGW64} ${webservices_flags} -Wl,--entry,DllMain,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/webservices.dll
                        ${webservices_source}/webservices.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c -lkernel32 -lgcc
                    COMMAND ${RAVEN_MINGW32} ${webservices_flags} -Wl,--entry,_DllMain@12,--kill-at,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/x86/webservices.dll
                        ${webservices_source}/webservices.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c -lkernel32 -lgcc
                    DEPENDS ${webservices_source}/webservices.c VERBATIM)
                add_custom_target(webservices_component ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/webservices.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/webservices.dll)
                foreach(width 64 32)
                    set(webservices_client ${CMAKE_CURRENT_BINARY_DIR}/webservices-client${width}.exe)
                    if(width EQUAL 64)
                        set(webservices_compiler ${RAVEN_MINGW64})
                        set(webservices_entry raven_entry)
                    else()
                        set(webservices_compiler ${RAVEN_MINGW32})
                        set(webservices_entry _raven_entry)
                    endif()
                    add_custom_command(OUTPUT ${webservices_client}
                        COMMAND ${webservices_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${webservices_entry},--no-insert-timestamp
                            -o ${webservices_client} ${LOADER_SOURCES}/webservices_client.c -lwebservices -lkernel32
                        DEPENDS ${LOADER_SOURCES}/webservices_client.c VERBATIM)
                    add_custom_target(webservices_fixture${width} ALL DEPENDS ${webservices_client} webservices_component)
                    add_test(NAME native_webservices_x${width} COMMAND raven --run-process --execution=native ${webservices_client})
                    set_tests_properties(native_webservices_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    set(class_info_client ${CMAKE_CURRENT_BINARY_DIR}/class-info${width}.exe)
                    add_custom_command(OUTPUT ${class_info_client}
                        COMMAND ${webservices_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${webservices_entry},--no-insert-timestamp
                            -o ${class_info_client} ${LOADER_SOURCES}/class_info_client.c -luser32 -lkernel32
                        DEPENDS ${LOADER_SOURCES}/class_info_client.c VERBATIM)
                    add_custom_target(class_info_fixture${width} ALL DEPENDS ${class_info_client})
                    add_test(NAME native_class_info_x${width} COMMAND raven --run-process --execution=native ${class_info_client})
                    set_tests_properties(native_class_info_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    set(dbcs_client ${CMAKE_CURRENT_BINARY_DIR}/dbcs${width}.exe)
                    add_custom_command(OUTPUT ${dbcs_client}
                        COMMAND ${webservices_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${webservices_entry},--no-insert-timestamp
                            -o ${dbcs_client} ${LOADER_SOURCES}/dbcs_client.c -lkernel32
                        DEPENDS ${LOADER_SOURCES}/dbcs_client.c VERBATIM)
                    add_custom_target(dbcs_fixture${width} ALL DEPENDS ${dbcs_client})
                    add_test(NAME native_dbcs_x${width} COMMAND raven --run-process --execution=native ${dbcs_client})
                    set_tests_properties(native_dbcs_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    # In a folder of its own: the libraries beside a program count as its imports, and those of the build tree use comctl32.
                    set(late_controls_client ${CMAKE_CURRENT_BINARY_DIR}/late-comctl32-${width}/client.exe)
                    add_custom_command(OUTPUT ${late_controls_client}
                        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/late-comctl32-${width}
                        COMMAND ${webservices_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${webservices_entry},--no-insert-timestamp
                            -o ${late_controls_client} ${LOADER_SOURCES}/late_comctl32_client.c -lkernel32
                        DEPENDS ${LOADER_SOURCES}/late_comctl32_client.c VERBATIM)
                    add_custom_target(late_comctl32_fixture${width} ALL DEPENDS ${late_controls_client})
                    add_test(NAME native_late_comctl32_x${width} COMMAND raven --run-process --execution=native ${late_controls_client})
                    set_tests_properties(native_late_comctl32_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    # A library loaded after start from beside its program, as PDF-XChange Editor loads its core, binds what it imports from the
                    # directory services by number and from the HTTP server, audio compression and color management libraries by name, with
                    # ntdll's keyed events and, in 64-bit processes, the path function of the path API set that allocates its result.
                    set(late_imports_dir ${CMAKE_CURRENT_BINARY_DIR}/late-imports-${width})
                    if(width EQUAL 64)
                        set(late_imports_dlltool ${RAVEN_DLLTOOL})
                        set(late_imports_entry DllMain)
                        set(late_imports_path -lpathcch)
                    else()
                        set(late_imports_dlltool ${RAVEN_DLLTOOL32} -k)
                        set(late_imports_entry _DllMain@12)
                        set(late_imports_path)
                    endif()
                    add_custom_command(OUTPUT ${late_imports_dir}/client.exe ${late_imports_dir}/late-imports.dll
                        COMMAND ${CMAKE_COMMAND} -E make_directory ${late_imports_dir}
                        COMMAND ${late_imports_dlltool} -d ${LOADER_SOURCES}/late_activeds${width}.def -l ${CMAKE_CURRENT_BINARY_DIR}/late-activeds${width}.a
                        COMMAND ${webservices_compiler} -shared -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${late_imports_entry},--no-insert-timestamp
                            -o ${late_imports_dir}/late-imports.dll ${LOADER_SOURCES}/late_imports.c ${CMAKE_CURRENT_BINARY_DIR}/late-activeds${width}.a
                            -lhttpapi -lmsacm32 -lmscms ${late_imports_path} -lntdll -lkernel32
                        COMMAND ${webservices_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${webservices_entry},--no-insert-timestamp
                            -o ${late_imports_dir}/client.exe ${LOADER_SOURCES}/late_imports_client.c -lkernel32
                        DEPENDS ${LOADER_SOURCES}/late_imports.c ${LOADER_SOURCES}/late_imports_client.c ${LOADER_SOURCES}/late_activeds${width}.def VERBATIM)
                    add_custom_target(late_imports_fixture${width} ALL DEPENDS ${late_imports_dir}/client.exe ${late_imports_dir}/late-imports.dll)
                    add_test(NAME native_late_system_imports_x${width} COMMAND raven --run-process --execution=native ${late_imports_dir}/client.exe)
                    set_tests_properties(native_late_system_imports_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    set(rotated_text_client ${CMAKE_CURRENT_BINARY_DIR}/rotated-text${width}.exe)
                    add_custom_command(OUTPUT ${rotated_text_client}
                        COMMAND ${webservices_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${webservices_entry},--no-insert-timestamp
                            -o ${rotated_text_client} ${LOADER_SOURCES}/rotated_text_client.c -lgdi32 -lkernel32
                        DEPENDS ${LOADER_SOURCES}/rotated_text_client.c VERBATIM)
                    add_custom_target(rotated_text_fixture${width} ALL DEPENDS ${rotated_text_client})
                    add_test(NAME native_rotated_text_x${width} COMMAND raven --run-process --execution=native ${rotated_text_client})
                    set_tests_properties(native_rotated_text_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    set(glyph_index_client ${CMAKE_CURRENT_BINARY_DIR}/glyph-index${width}.exe)
                    add_custom_command(OUTPUT ${glyph_index_client}
                        COMMAND ${webservices_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${webservices_entry},--no-insert-timestamp
                            -o ${glyph_index_client} ${LOADER_SOURCES}/glyph_index_client.c -lusp10 -lgdi32 -lkernel32
                        DEPENDS ${LOADER_SOURCES}/glyph_index_client.c VERBATIM)
                    add_custom_target(glyph_index_fixture${width} ALL DEPENDS ${glyph_index_client})
                    add_test(NAME native_glyph_index_x${width} COMMAND raven --run-process --execution=native ${glyph_index_client})
                    set_tests_properties(native_glyph_index_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    set(dialog_font_client ${CMAKE_CURRENT_BINARY_DIR}/dialog-font${width}.exe)
                    add_custom_command(OUTPUT ${dialog_font_client}
                        COMMAND ${webservices_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${webservices_entry},--no-insert-timestamp
                            -o ${dialog_font_client} ${LOADER_SOURCES}/dialog_font_client.c -lgdi32 -lkernel32
                        DEPENDS ${LOADER_SOURCES}/dialog_font_client.c VERBATIM)
                    add_custom_target(dialog_font_fixture${width} ALL DEPENDS ${dialog_font_client})
                    add_test(NAME native_dialog_font_x${width} COMMAND raven --run-process --execution=native ${dialog_font_client})
                    set_tests_properties(native_dialog_font_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    set(cross_window_client ${CMAKE_CURRENT_BINARY_DIR}/cross-window${width}.exe)
                    add_custom_command(OUTPUT ${cross_window_client}
                        COMMAND ${webservices_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${webservices_entry},--no-insert-timestamp
                            -o ${cross_window_client} ${LOADER_SOURCES}/cross_window_client.c -luser32 -lkernel32
                        DEPENDS ${LOADER_SOURCES}/cross_window_client.c VERBATIM)
                    add_custom_target(cross_window_fixture${width} ALL DEPENDS ${cross_window_client})
                    add_test(NAME native_cross_window_x${width} COMMAND raven --run-process --execution=native --timeout-ms=30000 ${cross_window_client})
                    set_tests_properties(native_cross_window_x${width} PROPERTIES TIMEOUT 40 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    set(toolbar_customize_client ${CMAKE_CURRENT_BINARY_DIR}/toolbar-customize${width}.exe)
                    add_custom_command(OUTPUT ${toolbar_customize_client}
                        COMMAND ${webservices_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${webservices_entry},--no-insert-timestamp
                            -o ${toolbar_customize_client} ${LOADER_SOURCES}/toolbar_customize_client.c -lcomctl32 -luser32 -lkernel32
                        DEPENDS ${LOADER_SOURCES}/toolbar_customize_client.c VERBATIM)
                    add_custom_target(toolbar_customize_fixture${width} ALL DEPENDS ${toolbar_customize_client})
                    add_test(NAME native_toolbar_customize_x${width} COMMAND raven --run-process --execution=native --timeout-ms=20000 ${toolbar_customize_client})
                    set_tests_properties(native_toolbar_customize_x${width} PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    if(width EQUAL 64)
                        set(chained_unwind_client ${CMAKE_CURRENT_BINARY_DIR}/chained-unwind64.exe)
                        add_custom_command(OUTPUT ${chained_unwind_client}
                            COMMAND ${webservices_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${webservices_entry},--no-insert-timestamp
                                -o ${chained_unwind_client} ${LOADER_SOURCES}/chained_unwind_client.c -lkernel32 -lntdll
                            DEPENDS ${LOADER_SOURCES}/chained_unwind_client.c VERBATIM)
                        add_custom_target(chained_unwind_fixture ALL DEPENDS ${chained_unwind_client})
                        add_test(NAME native_chained_unwind_x64 COMMAND raven --run-process --execution=native ${chained_unwind_client})
                        set_tests_properties(native_chained_unwind_x64 PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    endif()
                    # mlang comes with the installer components, whose import library the client links against.
                    if(width EQUAL 64)
                        set(mlang_dir ${RAVEN_INSTALLER_COMPONENT_DIR})
                    else()
                        set(mlang_dir ${RAVEN_INSTALLER32_COMPONENT_DIR})
                    endif()
                    if(mlang_dir AND EXISTS ${mlang_dir}/libmlang.a)
                        set(mlang_client ${CMAKE_CURRENT_BINARY_DIR}/mlang-japanese${width}.exe)
                        add_custom_command(OUTPUT ${mlang_client}
                            COMMAND ${webservices_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${webservices_entry},--no-insert-timestamp
                                -o ${mlang_client} ${LOADER_SOURCES}/mlang_client.c ${mlang_dir}/libmlang.a -lkernel32
                            DEPENDS ${LOADER_SOURCES}/mlang_client.c VERBATIM)
                        add_custom_target(mlang_japanese_fixture${width} ALL DEPENDS ${mlang_client})
                        add_test(NAME native_mlang_japanese_x${width} COMMAND raven --run-process --execution=native ${mlang_client})
                        set_tests_properties(native_mlang_japanese_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    endif()
                endforeach()
                # d3dcompiler_47.dll for each width: Raven code on the public headers that hands compiles to the runtime's HLSL compiler
                # through ravenhost.dll and reads reflection from the container itself.
                set(d3dcompiler_source ${PROJECT_SOURCE_DIR}/native/guest/raven-d3dcompiler)
                set(d3dcompiler_flags -O2 -Wall -Wno-unused-parameter -ffreestanding -fno-builtin -shared -nostdlib -ffile-prefix-map=${PROJECT_SOURCE_DIR}=raven)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/d3dcompiler_47.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/d3dcompiler_47.dll
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/x86
                    COMMAND ${RAVEN_DLLTOOL} -d ${d3dcompiler_source}/ravenhost64.def -l ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-d3dcompiler64.a
                    COMMAND ${RAVEN_DLLTOOL32} -k -d ${d3dcompiler_source}/ravenhost32.def -l ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-d3dcompiler32.a
                    COMMAND ${RAVEN_MINGW64} ${d3dcompiler_flags} -Wl,--entry,DllMain,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/d3dcompiler_47.dll
                        ${d3dcompiler_source}/d3dcompiler.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c
                        ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-d3dcompiler64.a -lkernel32 -lgcc
                    COMMAND ${RAVEN_MINGW32} ${d3dcompiler_flags} -Wl,--entry,_DllMain@12,--kill-at,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/x86/d3dcompiler_47.dll
                        ${d3dcompiler_source}/d3dcompiler.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c
                        ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-d3dcompiler32.a -lkernel32 -lgcc
                    DEPENDS ${d3dcompiler_source}/d3dcompiler.c ${d3dcompiler_source}/ravenhost64.def ${d3dcompiler_source}/ravenhost32.def VERBATIM)
                add_custom_target(d3dcompiler_component ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/d3dcompiler_47.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/d3dcompiler_47.dll)
                foreach(width 64 32)
                    set(d3dcompiler_client ${CMAKE_CURRENT_BINARY_DIR}/d3dcompiler-client${width}.exe)
                    if(width EQUAL 64)
                        set(d3dcompiler_compiler ${RAVEN_MINGW64})
                        set(d3dcompiler_entry raven_entry)
                    else()
                        set(d3dcompiler_compiler ${RAVEN_MINGW32})
                        set(d3dcompiler_entry _raven_entry)
                    endif()
                    add_custom_command(OUTPUT ${d3dcompiler_client}
                        COMMAND ${d3dcompiler_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${d3dcompiler_entry},--no-insert-timestamp
                            -o ${d3dcompiler_client} ${LOADER_SOURCES}/d3dcompiler_client.c -ld3dcompiler_47 -lkernel32
                        DEPENDS ${LOADER_SOURCES}/d3dcompiler_client.c VERBATIM)
                    add_custom_target(d3dcompiler_fixture${width} ALL DEPENDS ${d3dcompiler_client} d3dcompiler_component)
                    add_test(NAME native_d3dcompiler_x${width} COMMAND raven --run-process --execution=native ${d3dcompiler_client})
                    set_tests_properties(native_d3dcompiler_x${width} PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                endforeach()
                # raven-accprop.dll, the AccPropServices class of oleacc for each width: Raven code on the public headers.
                set(accprop_source ${PROJECT_SOURCE_DIR}/native/guest/raven-accprop)
                set(accprop_flags -O2 -Wall -Wno-unused-parameter -ffreestanding -fno-builtin -shared -nostdlib -ffile-prefix-map=${PROJECT_SOURCE_DIR}=raven)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/raven-accprop.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-accprop.dll
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/x86
                    COMMAND ${RAVEN_MINGW64} ${accprop_flags} -Wl,--entry,DllMain,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/raven-accprop.dll
                        ${accprop_source}/accprop.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c -loleaut32 -lole32 -luuid -lkernel32 -lgcc
                    COMMAND ${RAVEN_MINGW32} ${accprop_flags} -Wl,--entry,_DllMain@12,--kill-at,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-accprop.dll
                        ${accprop_source}/accprop.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c -loleaut32 -lole32 -luuid -lkernel32 -lgcc
                    DEPENDS ${accprop_source}/accprop.c VERBATIM)
                add_custom_target(accprop_component ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/raven-accprop.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-accprop.dll)
                # icu.dll with icuuc.dll and icuin.dll forwarding to it, for each width: Raven code on the public documentation and the system's text functions.
                # The 32-bit forwarders have no relocations, and the linker's default base for icuin.dll falls where images cannot be placed: both bases are fixed.
                set(icu_source ${PROJECT_SOURCE_DIR}/native/guest/raven-icu)
                set(icu_files ${icu_source}/dllmain.c ${icu_source}/common.c ${icu_source}/text.c ${icu_source}/breaks.c ${icu_source}/regex.c)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/icu.dll ${CMAKE_CURRENT_BINARY_DIR}/icuuc.dll ${CMAKE_CURRENT_BINARY_DIR}/icuin.dll
                        ${CMAKE_CURRENT_BINARY_DIR}/x86/icu.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/icuuc.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/icuin.dll
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/x86
                    COMMAND ${RAVEN_MINGW64} ${accprop_flags} -Wl,--entry,DllMain,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/icu.dll
                        ${icu_files} ${icu_source}/icu.def ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c -lkernel32 -lgcc
                    COMMAND ${RAVEN_MINGW64} ${accprop_flags} -Wl,--entry,DllMain,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/icuuc.dll ${icu_source}/stub.c ${icu_source}/icuuc.def
                    COMMAND ${RAVEN_MINGW64} ${accprop_flags} -Wl,--entry,DllMain,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/icuin.dll ${icu_source}/stub.c ${icu_source}/icuin.def
                    COMMAND ${RAVEN_MINGW32} ${accprop_flags} -Wl,--entry,_DllMain@12,--enable-stdcall-fixup,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/x86/icu.dll
                        ${icu_files} ${icu_source}/icu.def ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c -lkernel32 -lgcc
                    COMMAND ${RAVEN_MINGW32} ${accprop_flags} -Wl,--entry,_DllMain@12,--enable-stdcall-fixup,--no-insert-timestamp,--image-base,0x66bc0000 -o ${CMAKE_CURRENT_BINARY_DIR}/x86/icuuc.dll ${icu_source}/stub.c ${icu_source}/icuuc.def
                    COMMAND ${RAVEN_MINGW32} ${accprop_flags} -Wl,--entry,_DllMain@12,--enable-stdcall-fixup,--no-insert-timestamp,--image-base,0x66be0000 -o ${CMAKE_CURRENT_BINARY_DIR}/x86/icuin.dll ${icu_source}/stub.c ${icu_source}/icuin.def
                    DEPENDS ${icu_files} ${icu_source}/stub.c ${icu_source}/internal.h ${icu_source}/icu.def ${icu_source}/icuuc.def ${icu_source}/icuin.def VERBATIM)
                add_custom_target(icu_component ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/icu.dll ${CMAKE_CURRENT_BINARY_DIR}/icuuc.dll ${CMAKE_CURRENT_BINARY_DIR}/icuin.dll
                    ${CMAKE_CURRENT_BINARY_DIR}/x86/icu.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/icuuc.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/icuin.dll)
                # hlink.dll, the flat hyperlink functions for each width: Raven code on the public headers.
                set(hlink_source ${PROJECT_SOURCE_DIR}/native/guest/raven-hlink)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/hlink.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/hlink.dll
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/x86
                    COMMAND ${RAVEN_MINGW64} ${accprop_flags} -Wl,--entry,DllMain,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/hlink.dll
                        ${hlink_source}/hlink.c ${hlink_source}/hlink.def ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c -lole32 -lkernel32 -lgcc
                    COMMAND ${RAVEN_MINGW32} ${accprop_flags} -Wl,--entry,_DllMain@12,--enable-stdcall-fixup,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/x86/hlink.dll
                        ${hlink_source}/hlink.c ${hlink_source}/hlink.def ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c -lole32 -lkernel32 -lgcc
                    DEPENDS ${hlink_source}/hlink.c ${hlink_source}/hlink.def VERBATIM)
                add_custom_target(hlink_component ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/hlink.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/hlink.dll)
                # raven-d3d10_1.dll, Direct3D 10.1 device creation over the d3d10core.dll the runtime is given, for each width.
                set(d3d10_source ${PROJECT_SOURCE_DIR}/native/guest/raven-d3d10)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/raven-d3d10_1.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-d3d10_1.dll
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/x86
                    COMMAND ${RAVEN_MINGW64} ${accprop_flags} -Wl,--entry,DllMain,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/raven-d3d10_1.dll
                        ${d3d10_source}/d3d10_1.c -lkernel32 -lgcc
                    COMMAND ${RAVEN_MINGW32} ${accprop_flags} -Wl,--entry,_DllMain@12,--kill-at,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-d3d10_1.dll
                        ${d3d10_source}/d3d10_1.c -lkernel32 -lgcc
                    DEPENDS ${d3d10_source}/d3d10_1.c VERBATIM)
                add_custom_target(d3d10_component ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/raven-d3d10_1.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-d3d10_1.dll)
                foreach(width 64 32)
                    set(d3d10_client ${CMAKE_CURRENT_BINARY_DIR}/d3d10-client${width}.exe)
                    if(width EQUAL 64)
                        set(d3d10_compiler ${RAVEN_MINGW64})
                        set(d3d10_entry raven_entry)
                    else()
                        set(d3d10_compiler ${RAVEN_MINGW32})
                        set(d3d10_entry _raven_entry)
                    endif()
                    add_custom_command(OUTPUT ${d3d10_client}
                        COMMAND ${d3d10_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${d3d10_entry},--no-insert-timestamp
                            -o ${d3d10_client} ${LOADER_SOURCES}/d3d10_client.c -lkernel32
                        DEPENDS ${LOADER_SOURCES}/d3d10_client.c VERBATIM)
                    add_custom_target(d3d10_fixture${width} ALL DEPENDS ${d3d10_client} d3d10_component)
                    add_test(NAME native_d3d10_1_x${width} COMMAND raven --run-process --execution=native ${d3d10_client})
                    set_tests_properties(native_d3d10_1_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                endforeach()
                foreach(width 64 32)
                    set(accprop_client ${CMAKE_CURRENT_BINARY_DIR}/accprop-client${width}.exe)
                    if(width EQUAL 64)
                        set(accprop_compiler ${RAVEN_MINGW64})
                        set(accprop_entry raven_entry)
                    else()
                        set(accprop_compiler ${RAVEN_MINGW32})
                        set(accprop_entry _raven_entry)
                    endif()
                    add_custom_command(OUTPUT ${accprop_client}
                        COMMAND ${accprop_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${accprop_entry},--no-insert-timestamp
                            -o ${accprop_client} ${LOADER_SOURCES}/accprop_client.c -loleaut32 -luuid -lkernel32
                        DEPENDS ${LOADER_SOURCES}/accprop_client.c VERBATIM)
                    add_custom_target(accprop_fixture${width} ALL DEPENDS ${accprop_client} accprop_component)
                    add_test(NAME native_accprop_x${width} COMMAND raven --run-process --execution=native ${accprop_client})
                    set_tests_properties(native_accprop_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    set(icu_client ${CMAKE_CURRENT_BINARY_DIR}/icu-client${width}.exe)
                    add_custom_command(OUTPUT ${icu_client}
                        COMMAND ${accprop_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${accprop_entry},--no-insert-timestamp
                            -o ${icu_client} ${LOADER_SOURCES}/icu_client.c -lkernel32
                        DEPENDS ${LOADER_SOURCES}/icu_client.c VERBATIM)
                    add_custom_target(icu_fixture${width} ALL DEPENDS ${icu_client} icu_component)
                    add_test(NAME native_icu_x${width} COMMAND raven --run-process --execution=native ${icu_client})
                    set_tests_properties(native_icu_x${width} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    set(hlink_client ${CMAKE_CURRENT_BINARY_DIR}/hlink-client${width}.exe)
                    add_custom_command(OUTPUT ${hlink_client}
                        COMMAND ${accprop_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${accprop_entry},--no-insert-timestamp
                            -o ${hlink_client} ${LOADER_SOURCES}/hlink_client.c -lole32 -lkernel32
                        DEPENDS ${LOADER_SOURCES}/hlink_client.c VERBATIM)
                    add_custom_target(hlink_fixture${width} ALL DEPENDS ${hlink_client} hlink_component)
                    add_test(NAME native_hlink_x${width} COMMAND raven --run-process --execution=native ${hlink_client})
                    set_tests_properties(native_hlink_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    set(stream_client ${CMAKE_CURRENT_BINARY_DIR}/stream-client${width}.exe)
                    add_custom_command(OUTPUT ${stream_client}
                        COMMAND ${accprop_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${accprop_entry},--no-insert-timestamp
                            -o ${stream_client} ${LOADER_SOURCES}/stream_client.c -loleaut32 -luuid -lkernel32
                        DEPENDS ${LOADER_SOURCES}/stream_client.c VERBATIM)
                    add_custom_target(stream_fixture${width} ALL DEPENDS ${stream_client})
                    add_test(NAME native_global_stream_x${width} COMMAND raven --run-process --execution=native ${stream_client})
                    set_tests_properties(native_global_stream_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    set(errorinfo_client ${CMAKE_CURRENT_BINARY_DIR}/errorinfo-client${width}.exe)
                    add_custom_command(OUTPUT ${errorinfo_client}
                        COMMAND ${accprop_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${accprop_entry},--no-insert-timestamp
                            -o ${errorinfo_client} ${LOADER_SOURCES}/errorinfo_client.c -loleaut32 -luuid -lkernel32
                        DEPENDS ${LOADER_SOURCES}/errorinfo_client.c VERBATIM)
                    add_custom_target(errorinfo_fixture${width} ALL DEPENDS ${errorinfo_client})
                    add_test(NAME native_error_info_x${width} COMMAND raven --run-process --execution=native ${errorinfo_client})
                    set_tests_properties(native_error_info_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    set(msxml6_client ${CMAKE_CURRENT_BINARY_DIR}/msxml6-client${width}.exe)
                    add_custom_command(OUTPUT ${msxml6_client}
                        COMMAND ${accprop_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${accprop_entry},--no-insert-timestamp
                            -o ${msxml6_client} ${LOADER_SOURCES}/msxml6_client.c -lole32 -loleaut32 -luuid -lkernel32
                        DEPENDS ${LOADER_SOURCES}/msxml6_client.c VERBATIM)
                    add_custom_target(msxml6_fixture${width} ALL DEPENDS ${msxml6_client})
                    add_test(NAME native_msxml6_x${width} COMMAND raven --run-process --execution=native ${msxml6_client})
                    set_tests_properties(native_msxml6_x${width} PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    set(xmllite_client ${CMAKE_CURRENT_BINARY_DIR}/xmllite-client${width}.exe)
                    add_custom_command(OUTPUT ${xmllite_client}
                        COMMAND ${accprop_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${accprop_entry},--no-insert-timestamp
                            -o ${xmllite_client} ${LOADER_SOURCES}/xmllite_client.c -lxmllite -lole32 -luuid -lkernel32
                        DEPENDS ${LOADER_SOURCES}/xmllite_client.c VERBATIM)
                    add_custom_target(xmllite_fixture${width} ALL DEPENDS ${xmllite_client})
                    add_test(NAME native_xmllite_x${width} COMMAND raven --run-process --execution=native ${xmllite_client})
                    set_tests_properties(native_xmllite_x${width} PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    set(integrity_client ${CMAKE_CURRENT_BINARY_DIR}/integrity-client${width}.exe)
                    add_custom_command(OUTPUT ${integrity_client}
                        COMMAND ${accprop_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${accprop_entry},--no-insert-timestamp
                            -o ${integrity_client} ${LOADER_SOURCES}/integrity_client.c -ladvapi32 -lkernel32
                        DEPENDS ${LOADER_SOURCES}/integrity_client.c VERBATIM)
                    add_custom_target(integrity_fixture${width} ALL DEPENDS ${integrity_client})
                    add_test(NAME native_integrity_elevated_x${width} COMMAND raven --run-process --execution=native --elevated ${integrity_client})
                    set_tests_properties(native_integrity_elevated_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    add_test(NAME native_integrity_limited_x${width} COMMAND raven --run-process --execution=native ${integrity_client})
                    set_tests_properties(native_integrity_limited_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 10 ")
                    set(rpc_auth_client ${CMAKE_CURRENT_BINARY_DIR}/rpc-auth-client${width}.exe)
                    add_custom_command(OUTPUT ${rpc_auth_client}
                        COMMAND ${accprop_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${accprop_entry},--no-insert-timestamp
                            -o ${rpc_auth_client} ${LOADER_SOURCES}/rpc_auth_client.c -lrpcrt4 -lsecur32 -lkernel32
                        DEPENDS ${LOADER_SOURCES}/rpc_auth_client.c VERBATIM)
                    add_custom_target(rpc_auth_fixture${width} ALL DEPENDS ${rpc_auth_client})
                    add_test(NAME native_rpc_auth_x${width} COMMAND raven --run-process --execution=native ${rpc_auth_client})
                    set_tests_properties(native_rpc_auth_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    # A server manager that frees what an [in, out] parameter points to and hands back its own block, through stubs the
                    # interpreter runs, generated from the IDL at build time.
                    if(width EQUAL 64)
                        find_program(RAVEN_WIDL64 x86_64-w64-mingw32-widl)
                        set(rpc_replace_widl ${RAVEN_WIDL64})
                    else()
                        find_program(RAVEN_WIDL32 i686-w64-mingw32-widl)
                        set(rpc_replace_widl ${RAVEN_WIDL32})
                    endif()
                    if(rpc_replace_widl)
                        set(rpc_replace_stubs ${CMAKE_CURRENT_BINARY_DIR}/rpc-replace${width})
                        set(rpc_replace_client ${CMAKE_CURRENT_BINARY_DIR}/rpc-replace-client${width}.exe)
                        add_custom_command(OUTPUT ${rpc_replace_client}
                            COMMAND ${CMAKE_COMMAND} -E make_directory ${rpc_replace_stubs}
                            COMMAND ${CMAKE_COMMAND} -E chdir ${rpc_replace_stubs} ${rpc_replace_widl} -m${width} -Oif --prefix-server=server_
                                -h -c -s -o rpc_replace ${LOADER_SOURCES}/rpc_replace.idl
                            COMMAND ${accprop_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${accprop_entry},--no-insert-timestamp
                                -I${rpc_replace_stubs} -o ${rpc_replace_client} ${LOADER_SOURCES}/rpc_replace_client.c
                                ${rpc_replace_stubs}/rpc_replace_c.c ${rpc_replace_stubs}/rpc_replace_s.c -lrpcrt4 -lkernel32 -lntdll
                            DEPENDS ${LOADER_SOURCES}/rpc_replace.idl ${LOADER_SOURCES}/rpc_replace_client.c VERBATIM)
                        add_custom_target(rpc_replace_fixture${width} ALL DEPENDS ${rpc_replace_client})
                        add_test(NAME native_rpc_replace_x${width} COMMAND raven --run-process --execution=native ${rpc_replace_client})
                        set_tests_properties(native_rpc_replace_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    endif()
                    if(width EQUAL 64)
                        set(timer_queue_client ${CMAKE_CURRENT_BINARY_DIR}/timer-queue-client${width}.exe)
                        add_custom_command(OUTPUT ${timer_queue_client}
                            COMMAND ${accprop_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${accprop_entry},--no-insert-timestamp
                                -o ${timer_queue_client} ${LOADER_SOURCES}/timer_queue_client.c -lkernel32
                            DEPENDS ${LOADER_SOURCES}/timer_queue_client.c VERBATIM)
                        add_custom_target(timer_queue_fixture${width} ALL DEPENDS ${timer_queue_client})
                        add_test(NAME native_timer_queue_x${width} COMMAND raven --run-process --execution=native ${timer_queue_client})
                        set_tests_properties(native_timer_queue_x${width} PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                        set(restart_client ${CMAKE_CURRENT_BINARY_DIR}/restart-manager-client${width}.exe)
                        add_custom_command(OUTPUT ${restart_client}
                            COMMAND ${accprop_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${accprop_entry},--no-insert-timestamp
                                -o ${restart_client} ${LOADER_SOURCES}/restart_manager_client.c -lrstrtmgr -lkernel32
                            DEPENDS ${LOADER_SOURCES}/restart_manager_client.c VERBATIM)
                        add_custom_target(restart_manager_fixture${width} ALL DEPENDS ${restart_client})
                        add_test(NAME native_restart_manager_x${width} COMMAND raven --run-process --execution=native ${restart_client})
                        set_tests_properties(native_restart_manager_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    endif()
                endforeach()
                # raven-deployment.dll, the PackageManager class of the Windows Runtime for each width: Raven code on the public headers.
                set(deployment_source ${PROJECT_SOURCE_DIR}/native/guest/raven-deployment)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/raven-deployment.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-deployment.dll
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/x86
                    COMMAND ${RAVEN_MINGW64} ${accprop_flags} -Wl,--entry,DllMain,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/raven-deployment.dll
                        ${deployment_source}/deployment.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c -lruntimeobject -lkernel32 -lgcc
                    COMMAND ${RAVEN_MINGW32} ${accprop_flags} -Wl,--entry,_DllMain@12,--kill-at,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-deployment.dll
                        ${deployment_source}/deployment.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c -lruntimeobject -lkernel32 -lgcc
                    DEPENDS ${deployment_source}/deployment.c VERBATIM)
                add_custom_target(deployment_component ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/raven-deployment.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-deployment.dll)
                # raven-web.dll, the HTTP classes of the Windows Runtime (Windows.Web.Http, its Headers and Filters) and Windows.Foundation.Uri,
                # for each width: Raven code over WinINet, on the C declarations scripts/generate-winrt-header.py writes from Microsoft's
                # published metadata (native/guest/raven-web/windows.web.http.h and native/guest/raven-winrt/foundation.h).
                set(web_source ${PROJECT_SOURCE_DIR}/native/guest/raven-web)
                set(winrt_source ${PROJECT_SOURCE_DIR}/native/guest/raven-winrt)
                set(web_sources ${web_source}/http.c ${web_source}/headers.c ${web_source}/content.c ${web_source}/transport.c ${web_source}/uri.c
                    ${winrt_source}/winrt.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c)
                set(web_libraries -lruntimeobject -lwininet -lurlmon -lole32 -loleaut32 -lkernel32 -lgcc)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/raven-web.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-web.dll
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/x86
                    COMMAND ${RAVEN_MINGW64} ${accprop_flags} -Wl,--entry,DllMain,--gc-sections,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/raven-web.dll ${web_sources} ${web_libraries}
                    COMMAND ${RAVEN_MINGW32} ${accprop_flags} -Wl,--entry,_DllMain@12,--kill-at,--gc-sections,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-web.dll ${web_sources} ${web_libraries}
                    DEPENDS ${web_sources} ${web_source}/web.h ${winrt_source}/winrt.h ${winrt_source}/foundation.h ${web_source}/windows.web.http.h VERBATIM)
                add_custom_target(web_component ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/raven-web.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-web.dll)
                foreach(width 64 32)
                    set(web_client ${CMAKE_CURRENT_BINARY_DIR}/web-client${width}.exe)
                    if(width EQUAL 64)
                        set(web_compiler ${RAVEN_MINGW64})
                        set(web_entry raven_entry)
                    else()
                        set(web_compiler ${RAVEN_MINGW32})
                        set(web_entry _raven_entry)
                    endif()
                    add_custom_command(OUTPUT ${web_client}
                        COMMAND ${web_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -I${web_source}
                            -Wl,--entry,${web_entry},--no-insert-timestamp -o ${web_client} ${LOADER_SOURCES}/web_client.c
                            ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c -lruntimeobject -lws2_32 -lkernel32 -lgcc
                        DEPENDS ${LOADER_SOURCES}/web_client.c ${winrt_source}/foundation.h ${web_source}/windows.web.http.h VERBATIM)
                    add_custom_target(web_fixture${width} ALL DEPENDS ${web_client} web_component)
                    add_test(NAME native_web_http_x${width} COMMAND raven --run-process --execution=native ${web_client})
                    set_tests_properties(native_web_http_x${width} PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                endforeach()
                # raven-uisettings.dll, the UISettings class of the Windows Runtime for each width: Raven code on the public headers.
                set(uisettings_source ${PROJECT_SOURCE_DIR}/native/guest/raven-uisettings)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/raven-uisettings.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-uisettings.dll
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/x86
                    COMMAND ${RAVEN_MINGW64} ${accprop_flags} -Wl,--entry,DllMain,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/raven-uisettings.dll
                        ${uisettings_source}/uisettings.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c -lruntimeobject -luser32 -lkernel32 -lgcc
                    COMMAND ${RAVEN_MINGW32} ${accprop_flags} -Wl,--entry,_DllMain@12,--kill-at,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-uisettings.dll
                        ${uisettings_source}/uisettings.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c -lruntimeobject -luser32 -lkernel32 -lgcc
                    DEPENDS ${uisettings_source}/uisettings.c VERBATIM)
                add_custom_target(uisettings_component ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/raven-uisettings.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-uisettings.dll)
                set(uisettings_client ${CMAKE_CURRENT_BINARY_DIR}/uisettings-client64.exe)
                add_custom_command(OUTPUT ${uisettings_client}
                    COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,raven_entry,--no-insert-timestamp
                        -o ${uisettings_client} ${LOADER_SOURCES}/uisettings_client.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c -luser32 -lkernel32 -lgcc
                    DEPENDS ${LOADER_SOURCES}/uisettings_client.c VERBATIM)
                add_custom_target(uisettings_fixture ALL DEPENDS ${uisettings_client} uisettings_component)
                add_test(NAME native_uisettings_x64 COMMAND raven --run-process --execution=native ${uisettings_client})
                set_tests_properties(native_uisettings_x64 PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                # raven-twinapi.dll, system classes of the Windows Runtime for each width: Raven code on public metadata.
                set(twinapi_source ${PROJECT_SOURCE_DIR}/native/guest/raven-twinapi)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/raven-twinapi.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-twinapi.dll
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/x86
                    COMMAND ${RAVEN_MINGW64} ${accprop_flags} -Wl,--entry,DllMain,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/raven-twinapi.dll
                        ${twinapi_source}/twinapi.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c -lruntimeobject -liphlpapi -ladvapi32 -lkernel32 -lgcc
                    COMMAND ${RAVEN_MINGW32} ${accprop_flags} -Wl,--entry,_DllMain@12,--kill-at,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-twinapi.dll
                        ${twinapi_source}/twinapi.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c -lruntimeobject -liphlpapi -ladvapi32 -lkernel32 -lgcc
                    DEPENDS ${twinapi_source}/twinapi.c VERBATIM)
                add_custom_target(twinapi_component ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/raven-twinapi.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-twinapi.dll)
                foreach(width 64 32)
                    set(twinapi_client ${CMAKE_CURRENT_BINARY_DIR}/twinapi-client${width}.exe)
                    if(width EQUAL 64)
                        set(twinapi_compiler ${RAVEN_MINGW64})
                        set(twinapi_entry raven_entry)
                    else()
                        set(twinapi_compiler ${RAVEN_MINGW32})
                        set(twinapi_entry _raven_entry)
                    endif()
                    add_custom_command(OUTPUT ${twinapi_client}
                        COMMAND ${twinapi_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${twinapi_entry},--no-insert-timestamp
                            -o ${twinapi_client} ${LOADER_SOURCES}/twinapi_client.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c -lole32 -lruntimeobject -lshlwapi -ladvapi32 -lkernel32 -lgcc
                        DEPENDS ${LOADER_SOURCES}/twinapi_client.c VERBATIM)
                    add_custom_target(twinapi_fixture${width} ALL DEPENDS ${twinapi_client} twinapi_component)
                    add_test(NAME native_twinapi_x${width} COMMAND raven --run-process --execution=native ${twinapi_client})
                    set_tests_properties(native_twinapi_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                endforeach()
                foreach(width 64 32)
                    set(deployment_client ${CMAKE_CURRENT_BINARY_DIR}/deployment-client${width}.exe)
                    if(width EQUAL 64)
                        set(deployment_compiler ${RAVEN_MINGW64})
                        set(deployment_entry raven_entry)
                    else()
                        set(deployment_compiler ${RAVEN_MINGW32})
                        set(deployment_entry _raven_entry)
                    endif()
                    add_custom_command(OUTPUT ${deployment_client}
                        COMMAND ${deployment_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${deployment_entry},--no-insert-timestamp
                            -o ${deployment_client} ${LOADER_SOURCES}/deployment_client.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c -loleaut32 -lkernel32 -lgcc
                        DEPENDS ${LOADER_SOURCES}/deployment_client.c VERBATIM)
                    add_custom_target(deployment_fixture${width} ALL DEPENDS ${deployment_client} deployment_component)
                    add_test(NAME native_deployment_x${width} COMMAND raven --run-process --execution=native ${deployment_client})
                    set_tests_properties(native_deployment_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                    # A program importing only the Windows Runtime's activation, through runtimeobject's API sets.
                    set(winrt_import_client ${CMAKE_CURRENT_BINARY_DIR}/winrt-import-client${width}.exe)
                    add_custom_command(OUTPUT ${winrt_import_client}
                        COMMAND ${deployment_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${deployment_entry},--no-insert-timestamp
                            -o ${winrt_import_client} ${LOADER_SOURCES}/winrt_import_client.c -lruntimeobject -lkernel32 -lgcc
                        DEPENDS ${LOADER_SOURCES}/winrt_import_client.c VERBATIM)
                    add_custom_target(winrt_import_fixture${width} ALL DEPENDS ${winrt_import_client} deployment_component)
                    add_test(NAME native_winrt_activation_imports_x${width} COMMAND raven --run-process --execution=native ${winrt_import_client})
                    set_tests_properties(native_winrt_activation_imports_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                endforeach()
                # raven-netlist.dll, the NetworkListManager class of netprofm for each width: Raven code on the public headers.
                set(netlist_source ${PROJECT_SOURCE_DIR}/native/guest/raven-netlist)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/raven-netlist.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-netlist.dll
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/x86
                    COMMAND ${RAVEN_MINGW64} ${accprop_flags} -Wl,--entry,DllMain,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/raven-netlist.dll
                        ${netlist_source}/netlist.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c -loleaut32 -liphlpapi -luuid -lkernel32 -lgcc
                    COMMAND ${RAVEN_MINGW32} ${accprop_flags} -Wl,--entry,_DllMain@12,--kill-at,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-netlist.dll
                        ${netlist_source}/netlist.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c -loleaut32 -liphlpapi -luuid -lkernel32 -lgcc
                    DEPENDS ${netlist_source}/netlist.c VERBATIM)
                add_custom_target(netlist_component ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/raven-netlist.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-netlist.dll)
                foreach(width 64 32)
                    set(netlist_client ${CMAKE_CURRENT_BINARY_DIR}/netlist-client${width}.exe)
                    if(width EQUAL 64)
                        set(netlist_compiler ${RAVEN_MINGW64})
                        set(netlist_entry raven_entry)
                    else()
                        set(netlist_compiler ${RAVEN_MINGW32})
                        set(netlist_entry _raven_entry)
                    endif()
                    add_custom_command(OUTPUT ${netlist_client}
                        COMMAND ${netlist_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${netlist_entry},--no-insert-timestamp
                            -o ${netlist_client} ${LOADER_SOURCES}/netlist_client.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c -loleaut32 -lkernel32 -lgcc
                        DEPENDS ${LOADER_SOURCES}/netlist_client.c VERBATIM)
                    add_custom_target(netlist_fixture${width} ALL DEPENDS ${netlist_client} netlist_component)
                    add_test(NAME native_netlist_x${width} COMMAND raven --run-process --execution=native ${netlist_client})
                    # A machine without an address beyond loopback answers 30, disconnected, which is as correct.
                    set_tests_properties(native_netlist_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned (0|30) ")
                endforeach()
                # raven-bits.dll, the background transfer manager of BITS for each width: Raven code on the public headers that downloads
                # through WinHTTP. Its trace of methods nobody wrote goes to the runtime through ravenhost.dll.
                set(bits_source ${PROJECT_SOURCE_DIR}/native/guest/raven-bits)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/raven-bits.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-bits.dll
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/x86
                    COMMAND ${RAVEN_DLLTOOL} -d ${taskschd_source}/ravenhost64.def -l ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-bits64.a
                    COMMAND ${RAVEN_DLLTOOL32} -k -d ${taskschd_source}/ravenhost32.def -l ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-bits32.a
                    COMMAND ${RAVEN_MINGW64} ${accprop_flags} -Wl,--entry,DllMain,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/raven-bits.dll
                        ${bits_source}/bits.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-bits64.a
                        -lwinhttp -lole32 -ladvapi32 -lkernel32 -lgcc
                    COMMAND ${RAVEN_MINGW32} ${accprop_flags} -Wl,--entry,_DllMain@12,--kill-at,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-bits.dll
                        ${bits_source}/bits.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-bits32.a
                        -lwinhttp -lole32 -ladvapi32 -lkernel32 -lgcc
                    DEPENDS ${bits_source}/bits.c ${taskschd_source}/ravenhost64.def ${taskschd_source}/ravenhost32.def VERBATIM)
                add_custom_target(bits_component ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/raven-bits.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-bits.dll)
                foreach(width 64 32)
                    set(bits_client ${CMAKE_CURRENT_BINARY_DIR}/bits-client${width}.exe)
                    if(width EQUAL 64)
                        set(bits_compiler ${RAVEN_MINGW64})
                        set(bits_entry raven_entry)
                    else()
                        set(bits_compiler ${RAVEN_MINGW32})
                        set(bits_entry _raven_entry)
                    endif()
                    add_custom_command(OUTPUT ${bits_client}
                        COMMAND ${bits_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${bits_entry},--no-insert-timestamp
                            -o ${bits_client} ${LOADER_SOURCES}/bits_client.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c
                            -lole32 -lws2_32 -lkernel32 -lgcc
                        DEPENDS ${LOADER_SOURCES}/bits_client.c VERBATIM)
                    add_custom_target(bits_fixture${width} ALL DEPENDS ${bits_client} bits_component)
                    add_test(NAME native_bits_x${width} COMMAND raven --run-process --execution=native ${bits_client})
                    set_tests_properties(native_bits_x${width} PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                endforeach()
                # raven-wmi.dll, the object path parser of wmiutils and the locator of wbemprox with the classes of root\cimv2, for each
                # width: Raven code on the public headers.
                set(wmi_source ${PROJECT_SOURCE_DIR}/native/guest/raven-wmi)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/raven-wmi.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-wmi.dll
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/x86
                    COMMAND ${RAVEN_DLLTOOL} -d ${taskschd_source}/ravenhost64.def -l ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-wmi64.a
                    COMMAND ${RAVEN_DLLTOOL32} -k -d ${taskschd_source}/ravenhost32.def -l ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-wmi32.a
                    COMMAND ${RAVEN_MINGW64} ${accprop_flags} -Wl,--entry,DllMain,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/raven-wmi.dll
                        ${wmi_source}/wmi.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-wmi64.a
                        -loleaut32 -lole32 -ladvapi32 -luser32 -lkernel32 -lgcc
                    COMMAND ${RAVEN_MINGW32} ${accprop_flags} -Wl,--entry,_DllMain@12,--kill-at,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-wmi.dll
                        ${wmi_source}/wmi.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-wmi32.a
                        -loleaut32 -lole32 -ladvapi32 -luser32 -lkernel32 -lgcc
                    DEPENDS ${wmi_source}/wmi.c ${taskschd_source}/ravenhost64.def ${taskschd_source}/ravenhost32.def VERBATIM)
                add_custom_target(wmi_component ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/raven-wmi.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-wmi.dll)
                foreach(width 64 32)
                    set(wmi_client ${CMAKE_CURRENT_BINARY_DIR}/wmi-client${width}.exe)
                    if(width EQUAL 64)
                        set(wmi_compiler ${RAVEN_MINGW64})
                        set(wmi_entry raven_entry)
                    else()
                        set(wmi_compiler ${RAVEN_MINGW32})
                        set(wmi_entry _raven_entry)
                    endif()
                    add_custom_command(OUTPUT ${wmi_client}
                        COMMAND ${wmi_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${wmi_entry},--no-insert-timestamp
                            -o ${wmi_client} ${LOADER_SOURCES}/wmi_client.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c
                            -loleaut32 -lole32 -luser32 -lkernel32 -lgcc
                        DEPENDS ${LOADER_SOURCES}/wmi_client.c VERBATIM)
                    add_custom_target(wmi_fixture${width} ALL DEPENDS ${wmi_client} wmi_component)
                    add_test(NAME native_wmi_x${width} COMMAND raven --run-process --execution=native ${wmi_client})
                    set_tests_properties(native_wmi_x${width} PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                endforeach()
                # raven-webengine.dll, the page engine of the runtime's web controls, and raven-cefhost.exe, its subprocess, built when
                # RAVEN_CEF_DIRECTORY names the output of scripts/provision-cef.py: the Chromium Embedded Framework's runtime stands
                # beside them in the runtime's cef folder.
                set(webengine_source ${PROJECT_SOURCE_DIR}/native/guest/raven-webengine)
                set(RAVEN_CEF_DIRECTORY "" CACHE PATH "Directory holding the CEF build provisioned by scripts/provision-cef.py")
                if(RAVEN_CEF_DIRECTORY)
                    if(NOT EXISTS "${RAVEN_CEF_DIRECTORY}/runtime/libcef.dll")
                        message(FATAL_ERROR "RAVEN_CEF_DIRECTORY must be the output of scripts/provision-cef.py")
                    endif()
                    set(cef_flags -DCEF_API_VERSION=15600 -I${RAVEN_CEF_DIRECTORY})
                    set(cef_folder ${CMAKE_CURRENT_BINARY_DIR}/cef)
                    file(GLOB cef_runtime RELATIVE ${RAVEN_CEF_DIRECTORY}/runtime ${RAVEN_CEF_DIRECTORY}/runtime/*)
                    set(cef_links "")
                    foreach(entry ${cef_runtime})
                        list(APPEND cef_links COMMAND ${CMAKE_COMMAND} -E create_symlink ${RAVEN_CEF_DIRECTORY}/runtime/${entry} ${cef_folder}/${entry})
                    endforeach()
                    add_custom_command(OUTPUT ${cef_folder}/raven-cefhost.exe
                        COMMAND ${CMAKE_COMMAND} -E make_directory ${cef_folder}
                        ${cef_links}
                        COMMAND ${RAVEN_MINGW64} -O1 -municode -mwindows ${cef_flags} -Wl,--no-insert-timestamp
                            -o ${cef_folder}/raven-cefhost.exe ${webengine_source}/cefhost.c ${RAVEN_CEF_DIRECTORY}/runtime/libcef.dll
                        DEPENDS ${webengine_source}/cefhost.c ${RAVEN_CEF_DIRECTORY}/runtime/libcef.dll VERBATIM)
                    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/raven-webengine.dll
                        COMMAND ${RAVEN_DLLTOOL} -d ${taskschd_source}/ravenhost64.def -l ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-webengine64.a
                        COMMAND ${RAVEN_MINGW64} ${accprop_flags} ${cef_flags} -Wl,--entry,DllMain,--no-insert-timestamp
                            -o ${CMAKE_CURRENT_BINARY_DIR}/raven-webengine.dll ${webengine_source}/engine.c ${webengine_source}/external.c
                            ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-webengine64.a
                            -loleaut32 -lole32 -lurlmon -luser32 -lgdi32 -lkernel32 -lgcc
                        DEPENDS ${webengine_source}/engine.c ${webengine_source}/external.c ${webengine_source}/engine_cef.h
                            ${webengine_source}/webengine.h ${taskschd_source}/ravenhost64.def VERBATIM)
                    add_custom_target(webengine_component ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/raven-webengine.dll ${cef_folder}/raven-cefhost.exe)
                endif()
                # EmbeddedBrowserWebView.dll, Raven's WebView2 runtime over the page engine, built when RAVEN_WEBVIEW2_SDK_DIRECTORY names the
                # output of scripts/provision-webview2-sdk.py. It stands where a program's WebView2Loader.dll looks for it, in a folder named
                # after its browser version.
                set(RAVEN_WEBVIEW2_SDK_DIRECTORY "" CACHE PATH "Directory holding the WebView2 SDK headers provisioned by scripts/provision-webview2-sdk.py")
                if(RAVEN_WEBVIEW2_SDK_DIRECTORY)
                    if(NOT EXISTS "${RAVEN_WEBVIEW2_SDK_DIRECTORY}/webview2/include/WebView2.h")
                        message(FATAL_ERROR "RAVEN_WEBVIEW2_SDK_DIRECTORY must be the output of scripts/provision-webview2-sdk.py")
                    endif()
                    set(webview_source ${PROJECT_SOURCE_DIR}/native/guest/raven-webview)
                    set(webview_folder ${CMAKE_CURRENT_BINARY_DIR}/webview/${RAVEN_WEBVIEW_VERSION}/EBWebView/x64)
                    string(REPLACE "." "," webview_numbers ${RAVEN_WEBVIEW_VERSION})
                    add_custom_command(OUTPUT ${webview_folder}/EmbeddedBrowserWebView.dll
                        COMMAND ${CMAKE_COMMAND} -E make_directory ${webview_folder}
                        COMMAND ${RAVEN_WINDRES} -DRAVEN_WEBVIEW_VERSION_TEXT=${RAVEN_WEBVIEW_VERSION} -DRAVEN_WEBVIEW_VERSION_NUMBERS=${webview_numbers}
                            ${webview_source}/webview.rc ${CMAKE_CURRENT_BINARY_DIR}/webview-version.o
                        COMMAND ${RAVEN_DLLTOOL} -d ${taskschd_source}/ravenhost64.def -l ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-webview64.a
                        COMMAND ${RAVEN_MINGW64} ${accprop_flags} -Wno-unknown-pragmas -DRAVEN_WEBVIEW_VERSION=L"${RAVEN_WEBVIEW_VERSION}"
                            -I${webview_source}/include -I${RAVEN_WEBVIEW2_SDK_DIRECTORY}/webview2/include -I${webengine_source}
                            -Wl,--entry,DllMain,--no-insert-timestamp -o ${webview_folder}/EmbeddedBrowserWebView.dll ${webview_source}/webview.c
                            ${CMAKE_CURRENT_BINARY_DIR}/webview-version.o ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c
                            ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-webview64.a -lole32 -luser32 -lkernel32 -lgcc
                        DEPENDS ${webview_source}/webview.c ${webview_source}/webview.rc ${webengine_source}/webengine.h ${taskschd_source}/ravenhost64.def
                        VERBATIM)
                    add_custom_target(webview_component ALL DEPENDS ${webview_folder}/EmbeddedBrowserWebView.dll)
                endif()
                # raven-ieframe.dll, the WebBrowser control of ieframe that hosts embed, for each width: Raven code on the public headers.
                # The control draws pages with raven-webengine.dll when the runtime has it.
                set(ieframe_source ${PROJECT_SOURCE_DIR}/native/guest/raven-ieframe)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/raven-ieframe.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-ieframe.dll
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/x86
                    COMMAND ${RAVEN_DLLTOOL} -d ${taskschd_source}/ravenhost64.def -l ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-ieframe64.a
                    COMMAND ${RAVEN_DLLTOOL32} -k -d ${taskschd_source}/ravenhost32.def -l ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-ieframe32.a
                    COMMAND ${RAVEN_MINGW64} ${accprop_flags} -I${webengine_source} -Wl,--entry,DllMain,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/raven-ieframe.dll ${ieframe_source}/ieframe.c ${ieframe_source}/engine.c
                        ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c
                        ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-ieframe64.a -loleaut32 -lole32 -lurlmon -luser32 -lgdi32 -lkernel32 -lgcc
                    COMMAND ${RAVEN_MINGW32} ${accprop_flags} -I${webengine_source} -Wl,--entry,_DllMain@12,--kill-at,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-ieframe.dll ${ieframe_source}/ieframe.c ${ieframe_source}/engine.c
                        ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c
                        ${CMAKE_CURRENT_BINARY_DIR}/libravenhost-ieframe32.a -loleaut32 -lole32 -lurlmon -luser32 -lgdi32 -lkernel32 -lgcc
                    DEPENDS ${ieframe_source}/ieframe.c ${ieframe_source}/engine.c ${webengine_source}/webengine.h ${taskschd_source}/ravenhost64.def
                        ${taskschd_source}/ravenhost32.def VERBATIM)
                add_custom_target(ieframe_component ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/raven-ieframe.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/raven-ieframe.dll)
                foreach(width 64 32)
                    set(ieframe_client ${CMAKE_CURRENT_BINARY_DIR}/ieframe-client${width}.exe)
                    if(width EQUAL 64)
                        set(ieframe_compiler ${RAVEN_MINGW64})
                        set(ieframe_entry raven_entry)
                    else()
                        set(ieframe_compiler ${RAVEN_MINGW32})
                        set(ieframe_entry _raven_entry)
                    endif()
                    add_custom_command(OUTPUT ${ieframe_client}
                        COMMAND ${ieframe_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${ieframe_entry},--no-insert-timestamp
                            -o ${ieframe_client} ${LOADER_SOURCES}/ieframe_client.c ${PROJECT_SOURCE_DIR}/native/guest/raven-dwrite/crt.c
                            -loleaut32 -lole32 -luser32 -lkernel32 -lgcc
                        DEPENDS ${LOADER_SOURCES}/ieframe_client.c VERBATIM)
                    add_custom_target(ieframe_fixture${width} ALL DEPENDS ${ieframe_client} ieframe_component)
                    add_test(NAME native_ieframe_x${width} COMMAND raven --run-process --execution=native ${ieframe_client})
                    set_tests_properties(native_ieframe_x${width} PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                endforeach()
                # avrt.dll and mfplat.dll: the multimedia scheduler a media library registers its threads with, and the Media Foundation
                # factories it makes its samples, types and buffers with, for each width.
                set(media_source ${PROJECT_SOURCE_DIR}/native/guest/raven-media)
                set(media_components ${CMAKE_CURRENT_BINARY_DIR}/avrt.dll ${CMAKE_CURRENT_BINARY_DIR}/mfplat.dll
                    ${CMAKE_CURRENT_BINARY_DIR}/x86/avrt.dll ${CMAKE_CURRENT_BINARY_DIR}/x86/mfplat.dll)
                add_custom_command(OUTPUT ${media_components}
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/x86
                    COMMAND ${RAVEN_MINGW64} ${accprop_flags} -Wl,--entry,DllMain,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/avrt.dll
                        ${media_source}/avrt.c -lkernel32
                    COMMAND ${RAVEN_MINGW64} ${accprop_flags} -Wl,--entry,DllMain,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/mfplat.dll
                        ${media_source}/mfplat.c -lkernel32
                    COMMAND ${RAVEN_MINGW32} ${accprop_flags} -Wl,--entry,_DllMain@12,--kill-at,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/x86/avrt.dll
                        ${media_source}/avrt.c -lkernel32
                    COMMAND ${RAVEN_MINGW32} ${accprop_flags} -Wl,--entry,_DllMain@12,--kill-at,--no-insert-timestamp -o ${CMAKE_CURRENT_BINARY_DIR}/x86/mfplat.dll
                        ${media_source}/mfplat.c -lkernel32
                    DEPENDS ${media_source}/avrt.c ${media_source}/mfplat.c VERBATIM)
                add_custom_target(media_component ALL DEPENDS ${media_components})
                # A media library imports both, as cri_ware_unity.dll does; the fixture does too, and asks each one for its answers.
                foreach(width 64 32)
                    set(media_client ${CMAKE_CURRENT_BINARY_DIR}/media-client${width}.exe)
                    if(width EQUAL 64)
                        set(media_compiler ${RAVEN_MINGW64})
                        set(media_entry raven_entry)
                    else()
                        set(media_compiler ${RAVEN_MINGW32})
                        set(media_entry _raven_entry)
                    endif()
                    add_custom_command(OUTPUT ${media_client}
                        COMMAND ${media_compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin
                            -Wl,--entry,${media_entry},--no-insert-timestamp
                            -o ${media_client} ${LOADER_SOURCES}/media_client.c -lavrt -lmfplat -lkernel32
                        DEPENDS ${LOADER_SOURCES}/media_client.c VERBATIM)
                    add_custom_target(media_fixture${width} ALL DEPENDS ${media_client} media_component)
                    add_test(NAME native_media_x${width} COMMAND raven --run-process --execution=native ${media_client})
                    set_tests_properties(native_media_x${width} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                endforeach()
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86-ucrt-client.exe
                    COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector -fno-builtin
                        -Wl,--entry,_raven_entry,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/x86-ucrt-client.exe ${LOADER_SOURCES}/x86_ucrt_client.c -lkernel32
                    DEPENDS ${LOADER_SOURCES}/x86_ucrt_client.c VERBATIM)
                add_custom_target(x86_ucrt_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/x86-ucrt-client.exe x86_ucrt_component)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86-ucrt-vc2022.exe
                    COMMAND ${RAVEN_MINGW32} -O1 -nostdlib -fno-stack-protector -fno-builtin
                        -Wl,--entry,_raven_entry,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/x86-ucrt-vc2022.exe ${LOADER_SOURCES}/ucrt_vc2022_client.c -lkernel32
                    DEPENDS ${LOADER_SOURCES}/ucrt_vc2022_client.c VERBATIM)
                add_custom_target(x86_ucrt_vc2022_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/x86-ucrt-vc2022.exe x86_ucrt_component)
                if(TARGET vulkan_thunk32_generated)
                    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86-vulkan-client.exe
                        COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector -fno-builtin
                            -Wl,--entry,_raven_entry,--no-insert-timestamp
                            -o ${CMAKE_CURRENT_BINARY_DIR}/x86-vulkan-client.exe ${LOADER_SOURCES}/x86_vulkan_client.c -lkernel32
                        DEPENDS ${LOADER_SOURCES}/x86_vulkan_client.c VERBATIM)
                    add_custom_target(x86_vulkan_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/x86-vulkan-client.exe)
                    # The loader's own messages say why no driver answers when the instance cannot be made.
                    add_test(NAME native_x86_vulkan_compute COMMAND ${CMAKE_COMMAND} -E env VK_DRIVER_FILES=${RAVEN_LAVAPIPE_ICD}
                        VK_LOADER_DEBUG=error,warn,driver
                        $<TARGET_FILE:raven> --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/x86-vulkan-client.exe)
                    set_tests_properties(native_x86_vulkan_compute PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                endif()
                set(RAVEN_DXVK_PE32_DIR "" CACHE PATH "Directory holding the 32-bit DXVK libraries (d3d9.dll)")
                if(RAVEN_DXVK_PE32_DIR AND EXISTS "${RAVEN_DXVK_PE32_DIR}/d3d9.dll")
                    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86-load-client.exe
                        COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector -fno-builtin
                            -Wl,--entry,_raven_entry,--no-insert-timestamp
                            -o ${CMAKE_CURRENT_BINARY_DIR}/x86-load-client.exe ${LOADER_SOURCES}/x86_load_client.c -lkernel32
                        DEPENDS ${LOADER_SOURCES}/x86_load_client.c VERBATIM)
                    add_custom_target(x86_load_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/x86-load-client.exe)
                    string(REPLACE "/" "\\" X86_DXVK_D3D9 "Z:${RAVEN_DXVK_PE32_DIR}/d3d9.dll")
                    add_test(NAME native_x86_dxvk_d3d9_load COMMAND raven --run-process --execution=native
                        ${CMAKE_CURRENT_BINARY_DIR}/x86-load-client.exe -- ${X86_DXVK_D3D9})
                    set_tests_properties(native_x86_dxvk_d3d9_load PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                endif()
                add_test(NAME native_x86_ucrt COMMAND raven --run-process --execution=native
                    ${CMAKE_CURRENT_BINARY_DIR}/x86-ucrt-client.exe)
                set_tests_properties(native_x86_ucrt PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                add_test(NAME native_ucrt_vc2022_x86 COMMAND raven --run-process --execution=native
                    ${CMAKE_CURRENT_BINARY_DIR}/x86-ucrt-vc2022.exe)
                set_tests_properties(native_ucrt_vc2022_x86 PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                    ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/ucrt-vc2022-prefix32)
                add_test(NAME native_x86_d3d9_objects COMMAND raven --run-process --execution=native
                    ${CMAKE_CURRENT_BINARY_DIR}/x86-d3d9-client.exe)
                set_tests_properties(native_x86_d3d9_objects PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                add_test(NAME native_x86_kernel_cli COMMAND raven --run-process --execution=native
                    ${CMAKE_CURRENT_BINARY_DIR}/x86-kernel-client.exe)
                set_tests_properties(native_x86_kernel_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                # The desktop contracts below are width-neutral; each fixture is the same source the x64 runtime qualifies.
                set(X86_DESKTOP_CLIENTS default window_text coordinate focus message)
                foreach(CLIENT ${X86_DESKTOP_CLIENTS})
                    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86-desktop-${CLIENT}.exe
                        COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector -fno-builtin
                            -Wl,--entry,_raven_entry,--no-insert-timestamp
                            -o ${CMAKE_CURRENT_BINARY_DIR}/x86-desktop-${CLIENT}.exe
                            ${LOADER_SOURCES}/${CLIENT}_client.c -lkernel32 -luser32 -lgdi32
                        DEPENDS ${LOADER_SOURCES}/${CLIENT}_client.c VERBATIM)
                    list(APPEND X86_DESKTOP_EXECUTABLES ${CMAKE_CURRENT_BINARY_DIR}/x86-desktop-${CLIENT}.exe)
                    add_test(NAME native_x86_desktop_${CLIENT} COMMAND raven --run-process --execution=native
                        ${CMAKE_CURRENT_BINARY_DIR}/x86-desktop-${CLIENT}.exe)
                    set_tests_properties(native_x86_desktop_${CLIENT} PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                endforeach()
                add_custom_target(x86_desktop_fixtures ALL DEPENDS ${X86_DESKTOP_EXECUTABLES})
                foreach(PHASE 1 2)
                    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/x86-registry-${PHASE}.exe
                        COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector -fno-builtin -DRAVEN_PHASE=${PHASE}
                            -Wl,--entry,_raven_entry,--no-insert-timestamp
                            -o ${CMAKE_CURRENT_BINARY_DIR}/x86-registry-${PHASE}.exe ${LOADER_SOURCES}/x86_registry_client.c -ladvapi32 -lkernel32
                        DEPENDS ${LOADER_SOURCES}/x86_registry_client.c VERBATIM)
                endforeach()
                add_custom_target(x86_registry_fixtures ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/x86-registry-1.exe ${CMAKE_CURRENT_BINARY_DIR}/x86-registry-2.exe)
                add_test(NAME native_x86_registry COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-x86-registry.py
                    $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/x86-registry-1.exe ${CMAKE_CURRENT_BINARY_DIR}/x86-registry-2.exe
                    ${CMAKE_CURRENT_BINARY_DIR}/registry-prefix)
                set_tests_properties(native_x86_registry PROPERTIES TIMEOUT 40)
                # Full application workflow: needs the Wayland presentation backend, the text engine and a headless compositor.
                find_program(RAVEN_LABWC labwc)
                find_program(RAVEN_GRIM grim)
                find_program(RAVEN_WLRCTL wlrctl)
                find_program(RAVEN_WLR_RANDR wlr-randr)
                # Screenshot analysis needs Pillow; take the first interpreter that provides it.
                set(RAVEN_PIL_PYTHON "")
                foreach(CANDIDATE ${Python3_EXECUTABLE} /usr/bin/python3)
                    if(NOT RAVEN_PIL_PYTHON AND EXISTS ${CANDIDATE})
                        execute_process(COMMAND ${CANDIDATE} -c "import PIL" RESULT_VARIABLE CANDIDATE_RESULT OUTPUT_QUIET ERROR_QUIET)
                        if(CANDIDATE_RESULT EQUAL 0)
                            set(RAVEN_PIL_PYTHON ${CANDIDATE})
                        endif()
                    endif()
                endforeach()
                # The experimental X11 desktop is checked on an isolated Xvfb server with Openbox; xdotool drives and probes it.
                find_program(RAVEN_XVFB Xvfb)
                find_program(RAVEN_OPENBOX openbox)
                find_program(RAVEN_XDOTOOL xdotool)
                set(RAVEN_X11_TESTS OFF)
                if(XCB_CLIENT_FOUND AND RAVEN_XVFB AND RAVEN_OPENBOX AND RAVEN_XDOTOOL AND RAVEN_PIL_PYTHON)
                    set(RAVEN_X11_TESTS ON)
                endif()
                if(TARGET raven_wayland AND RAVEN_SKIA_LIBRARY AND RAVEN_LABWC AND RAVEN_GRIM AND RAVEN_WLRCTL AND RAVEN_PIL_PYTHON
                   AND EXISTS "${RAVEN_MINESWEEPER}")
                    add_test(NAME native_minesweeper_workflow COMMAND ${RAVEN_PIL_PYTHON}
                        ${PROJECT_SOURCE_DIR}/scripts/native-minesweeper.py $<TARGET_FILE:raven>
                        "${RAVEN_MINESWEEPER}" ${CMAKE_CURRENT_BINARY_DIR}/minesweeper-evidence)
                    set_tests_properties(native_minesweeper_workflow PROPERTIES TIMEOUT 240 RESOURCE_LOCK raven_compositor)
                elseif(NOT EXISTS "${RAVEN_MINESWEEPER}")
                    add_test(NAME native_minesweeper_workflow COMMAND ${CMAKE_COMMAND} -E echo
                        "SKIP: set RAVEN_MINESWEEPER to a user-supplied Minesweeper executable")
                    set_tests_properties(native_minesweeper_workflow PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP:")
                endif()
                # Notepad++ across three fresh processes needs the guest controls component, keyboard input and a clipboard reader as well.
                find_program(RAVEN_WTYPE wtype)
                find_program(RAVEN_WLPASTE wl-paste)
                if(TARGET raven_wayland AND RAVEN_SKIA_LIBRARY AND RAVEN_LABWC AND RAVEN_GRIM AND RAVEN_WLRCTL AND RAVEN_WTYPE AND RAVEN_WLPASTE
                   AND RAVEN_PIL_PYTHON AND RAVEN_CONTROLS_COMPONENT_DIR AND EXISTS ${RAVEN_TESTDATA_DIR}/npp/notepad++.exe)
                    add_test(NAME native_notepadpp_workflow COMMAND ${RAVEN_PIL_PYTHON}
                        ${PROJECT_SOURCE_DIR}/scripts/native-npp.py $<TARGET_FILE:raven>
                        ${RAVEN_TESTDATA_DIR}/npp ${CMAKE_CURRENT_BINARY_DIR}/notepadpp-evidence)
                    set_tests_properties(native_notepadpp_workflow PROPERTIES TIMEOUT 1200 RESOURCE_LOCK raven_compositor)
                endif()
                # The compositor maximizes and restores a window with its own shortcut; the program hears SC_MAXIMIZE and SC_RESTORE.
                if(TARGET raven_wayland AND TARGET frame_restore_fixture AND RAVEN_LABWC AND RAVEN_WLRCTL AND RAVEN_WTYPE)
                    add_test(NAME native_host_maximize COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-host-maximize.py
                        $<TARGET_FILE:raven> ${FRAME_RESTORE_FIXTURE} ${CMAKE_CURRENT_BINARY_DIR}/host-maximize)
                    set_tests_properties(native_host_maximize PROPERTIES TIMEOUT 90 RESOURCE_LOCK raven_compositor)
                endif()
                find_program(RAVEN_XCLIP xclip)
                if(RAVEN_X11_TESTS AND RAVEN_SKIA_LIBRARY AND RAVEN_XCLIP AND RAVEN_CONTROLS_COMPONENT_DIR AND EXISTS ${RAVEN_TESTDATA_DIR}/npp/notepad++.exe)
                    add_test(NAME native_x11_notepadpp_workflow COMMAND ${RAVEN_PIL_PYTHON}
                        ${PROJECT_SOURCE_DIR}/scripts/native-npp.py $<TARGET_FILE:raven>
                        ${RAVEN_TESTDATA_DIR}/npp ${CMAKE_CURRENT_BINARY_DIR}/x11-notepadpp-evidence x11)
                    set_tests_properties(native_x11_notepadpp_workflow PROPERTIES TIMEOUT 1200)
                endif()
                # The Visual C++ 2015-2022 redistributables, downloaded from Microsoft into RAVEN_REDIST_DIRECTORY: installed with their progress
                # window from a fresh prefix, checked from a program, installed again and removed.
                set(RAVEN_REDIST_DIRECTORY "" CACHE PATH "Directory holding vc_redist.x64.exe and vc_redist.x86.exe as Microsoft publishes them")
                # Authenticode on the redistributables as Microsoft signs them, against the roots Windows trusts: each must verify, a changed
                # copy must not.
                if(RAVEN_REDIST_DIRECTORY AND RAVEN_ROOT_CERTIFICATE_DIRECTORY AND EXISTS ${RAVEN_REDIST_DIRECTORY}/vc_redist.x64.exe
                   AND EXISTS ${RAVEN_REDIST_DIRECTORY}/vc2013_x64.exe AND EXISTS ${RAVEN_REDIST_DIRECTORY}/vc2010_x86.exe
                   AND EXISTS ${RAVEN_REDIST_DIRECTORY}/vc2005_x86.exe)
                    add_test(NAME native_authenticode COMMAND authenticode_test ${RAVEN_REDIST_DIRECTORY}/vc_redist.x64.exe
                        ${RAVEN_REDIST_DIRECTORY}/vc2013_x64.exe ${RAVEN_REDIST_DIRECTORY}/vc2010_x86.exe ${RAVEN_REDIST_DIRECTORY}/vc2005_x86.exe)
                    set_tests_properties(native_authenticode PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "PASS: signatures verified")
                endif()
                foreach(redist_arch x64 x86)
                    set(redist_folder system32)
                    if(redist_arch STREQUAL x86)
                        set(redist_folder syswow64)
                    endif()
                    if(TARGET raven_wayland AND RAVEN_LABWC AND RAVEN_GRIM AND RAVEN_PIL_PYTHON AND RAVEN_REDIST_DIRECTORY
                       AND EXISTS ${RAVEN_REDIST_DIRECTORY}/vc_redist.${redist_arch}.exe AND TARGET redist_check_fixture_${redist_arch})
                        add_test(NAME native_vcredist_2022_${redist_arch} COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-redist.py
                            $<TARGET_FILE:raven> ${RAVEN_REDIST_DIRECTORY}/vc_redist.${redist_arch}.exe
                            ${CMAKE_CURRENT_BINARY_DIR}/msi-fixtures/${redist_arch}/redist-check.exe ${redist_folder}
                            ${CMAKE_CURRENT_BINARY_DIR}/vcredist-${redist_arch}-evidence)
                        set_tests_properties(native_vcredist_2022_${redist_arch} PROPERTIES TIMEOUT 1800 RESOURCE_LOCK raven_compositor
                            PASS_REGULAR_EXPRESSION "PASS: installed")
                    endif()
                endforeach()
                # The Visual C++ 2005 and 2008 redistributables (vc2005_x86.exe and vc2008_x86.exe in the same directory) in one prefix: each
                # program built against one of the runtimes loads its own from WinSxS, before and after the older package goes over the newer.
                if(TARGET raven_wayland AND RAVEN_LABWC AND RAVEN_GRIM AND RAVEN_PIL_PYTHON AND RAVEN_REDIST_DIRECTORY AND TARGET crt_check_fixture_x86
                   AND EXISTS ${RAVEN_REDIST_DIRECTORY}/vc2005_x86.exe AND EXISTS ${RAVEN_REDIST_DIRECTORY}/vc2008_x86.exe)
                    add_test(NAME native_vcredist_legacy_x86 COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-legacy-redist.py
                        $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/vcredist-legacy-evidence
                        ${RAVEN_REDIST_DIRECTORY}/vc2005_x86.exe ${CMAKE_CURRENT_BINARY_DIR}/msi-fixtures/x86/crt-vc80.exe
                        ${RAVEN_REDIST_DIRECTORY}/vc2008_x86.exe ${CMAKE_CURRENT_BINARY_DIR}/msi-fixtures/x86/crt-vc90.exe)
                    set_tests_properties(native_vcredist_legacy_x86 PROPERTIES TIMEOUT 1800 RESOURCE_LOCK raven_compositor
                        PASS_REGULAR_EXPRESSION "PASS: installed side by side")
                endif()
                # Every Visual C++ redistributable from 2005 to 2022 in both widths (scripts/native-redist-matrix.py): each alone and installed
                # again, then all together oldest first and newest first, with every program built against them running after each step.
                set(matrix_packages vc2005_x86.exe vc2005_x64.exe vc2008_x86.exe vc2008_x64.exe vc2010_x86.exe vc2010_x64.exe vc2012_x86.exe
                    vc2012_x64.exe vc2013_x86.exe vc2013_x64.exe vc_redist.x86.exe vc_redist.x64.exe)
                set(matrix_ready FALSE)
                if(TARGET raven_wayland AND RAVEN_LABWC AND RAVEN_GRIM AND RAVEN_PIL_PYTHON AND RAVEN_REDIST_DIRECTORY AND TARGET crt_check_fixture_x86
                   AND TARGET crt_check_fixture_x64 AND TARGET redist_check_fixture_x86 AND TARGET redist_check_fixture_x64)
                    set(matrix_ready TRUE)
                    foreach(package ${matrix_packages})
                        if(NOT EXISTS ${RAVEN_REDIST_DIRECTORY}/${package})
                            set(matrix_ready FALSE)
                        endif()
                    endforeach()
                endif()
                if(matrix_ready)
                    foreach(package ${matrix_packages})
                        string(REPLACE ".exe" "" package_name ${package})
                        string(REPLACE "." "_" package_name ${package_name})
                        add_test(NAME native_redist_alone_${package_name} COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-redist-matrix.py
                            $<TARGET_FILE:raven> ${RAVEN_REDIST_DIRECTORY} ${CMAKE_CURRENT_BINARY_DIR}/msi-fixtures
                            ${CMAKE_CURRENT_BINARY_DIR}/redist-matrix-evidence/${package_name} alone ${package})
                        set_tests_properties(native_redist_alone_${package_name} PROPERTIES TIMEOUT 1800 RESOURCE_LOCK raven_compositor
                            PASS_REGULAR_EXPRESSION "PASS: ")
                    endforeach()
                    foreach(order forward backward)
                        add_test(NAME native_redist_together_${order} COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-redist-matrix.py
                            $<TARGET_FILE:raven> ${RAVEN_REDIST_DIRECTORY} ${CMAKE_CURRENT_BINARY_DIR}/msi-fixtures
                            ${CMAKE_CURRENT_BINARY_DIR}/redist-matrix-evidence/together-${order} together ${order})
                        set_tests_properties(native_redist_together_${order} PROPERTIES TIMEOUT 7200 RESOURCE_LOCK raven_compositor
                            PASS_REGULAR_EXPRESSION "PASS: ")
                    endforeach()
                endif()
                # The DirectX End-User Runtimes of June 2010 (directx_Jun2010_redist.exe in the same directory), unpacked and installed as a
                # game's setup does (scripts/native-directx-redist.py); a program of each width then uses what they installed.
                if(TARGET raven_wayland AND RAVEN_LABWC AND RAVEN_GRIM AND RAVEN_PIL_PYTHON AND RAVEN_REDIST_DIRECTORY AND RAVEN_MINGW32
                   AND EXISTS ${RAVEN_REDIST_DIRECTORY}/directx_Jun2010_redist.exe)
                    set(directx_fixtures ${CMAKE_CURRENT_BINARY_DIR}/directx-fixtures)
                    add_custom_command(OUTPUT ${directx_fixtures}/directx-check64.exe ${directx_fixtures}/directx-check32.exe
                        COMMAND ${CMAKE_COMMAND} -E make_directory ${directx_fixtures}
                        COMMAND ${RAVEN_MINGW64} -O1 -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe -Wl,--entry,raven_entry
                            -o ${directx_fixtures}/directx-check64.exe ${LOADER_SOURCES}/directx_check.c -lkernel32
                        COMMAND ${RAVEN_MINGW32} -O1 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,_raven_entry
                            -o ${directx_fixtures}/directx-check32.exe ${LOADER_SOURCES}/directx_check.c -lkernel32
                        DEPENDS ${LOADER_SOURCES}/directx_check.c VERBATIM)
                    add_custom_target(directx_check_fixtures ALL DEPENDS ${directx_fixtures}/directx-check64.exe ${directx_fixtures}/directx-check32.exe)
                    add_test(NAME native_redist_directx_jun2010 COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-directx-redist.py
                        $<TARGET_FILE:raven> ${RAVEN_REDIST_DIRECTORY}/directx_Jun2010_redist.exe ${directx_fixtures}/directx-check64.exe
                        ${directx_fixtures}/directx-check32.exe ${CMAKE_CURRENT_BINARY_DIR}/directx-redist-evidence)
                    set_tests_properties(native_redist_directx_jun2010 PROPERTIES TIMEOUT 3600 RESOURCE_LOCK raven_compositor
                        PASS_REGULAR_EXPRESSION "PASS: ")
                endif()
                # OpenAL (oalinst.exe in the same directory), installed silent as games run it (scripts/native-openal-redist.py).
                if(TARGET raven_wayland AND RAVEN_LABWC AND RAVEN_GRIM AND RAVEN_PIL_PYTHON AND RAVEN_REDIST_DIRECTORY AND RAVEN_MINGW32
                   AND EXISTS ${RAVEN_REDIST_DIRECTORY}/oalinst.exe)
                    set(openal_fixtures ${CMAKE_CURRENT_BINARY_DIR}/openal-fixtures)
                    add_custom_command(OUTPUT ${openal_fixtures}/openal-check64.exe ${openal_fixtures}/openal-check32.exe
                        COMMAND ${CMAKE_COMMAND} -E make_directory ${openal_fixtures}
                        COMMAND ${RAVEN_MINGW64} -O1 -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe -Wl,--entry,raven_entry
                            -o ${openal_fixtures}/openal-check64.exe ${LOADER_SOURCES}/openal_check.c -lkernel32
                        COMMAND ${RAVEN_MINGW32} -O1 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,_raven_entry
                            -o ${openal_fixtures}/openal-check32.exe ${LOADER_SOURCES}/openal_check.c -lkernel32
                        DEPENDS ${LOADER_SOURCES}/openal_check.c VERBATIM)
                    add_custom_target(openal_check_fixtures ALL DEPENDS ${openal_fixtures}/openal-check64.exe ${openal_fixtures}/openal-check32.exe)
                    add_test(NAME native_redist_openal COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-openal-redist.py
                        $<TARGET_FILE:raven> ${RAVEN_REDIST_DIRECTORY}/oalinst.exe ${openal_fixtures}/openal-check64.exe
                        ${openal_fixtures}/openal-check32.exe ${CMAKE_CURRENT_BINARY_DIR}/openal-redist-evidence)
                    set_tests_properties(native_redist_openal PROPERTIES TIMEOUT 900 RESOURCE_LOCK raven_compositor PASS_REGULAR_EXPRESSION "PASS: ")
                endif()
                # The .NET Framework 4.8 from Microsoft's offline installer (scripts/native-dotnet.py): the message Windows shows without a
                # runtime, then the framework installed through our msiexec, a console program and a WinForms window built with Mono's mcs, and a WPF
                # window the script compiles against the WPF assemblies the framework installed. With the XNA 4.0 redistributable and the .NET 3.5
                # SP1 package given, XNA is installed and checked, and .NET 3.5 is enabled through dism.exe after Features on Demand asked for it.
                set(RAVEN_NETFX_INSTALLER "" CACHE FILEPATH "ndp48-x86-x64-allos-enu.exe as Microsoft publishes it")
                set(RAVEN_XNA_INSTALLER "" CACHE FILEPATH "xnafx40_redist.msi, the XNA Framework 4.0 redistributable, installed after the framework when given")
                set(RAVEN_NETFX35_PACKAGE "" CACHE FILEPATH "dotnetfx35.exe, the .NET Framework 3.5 SP1 full package, enabled through dism.exe when given")
                find_program(RAVEN_MCS mcs)
                if(TARGET raven_wayland AND RAVEN_LABWC AND RAVEN_GRIM AND RAVEN_WLRCTL AND RAVEN_PIL_PYTHON AND RAVEN_MCS AND RAVEN_NETFX_INSTALLER
                   AND EXISTS ${RAVEN_NETFX_INSTALLER})
                    set(dotnet_fixtures ${CMAKE_CURRENT_BINARY_DIR}/dotnet-fixtures)
                    add_custom_command(OUTPUT ${dotnet_fixtures}/hello64.exe ${dotnet_fixtures}/forms64.exe ${dotnet_fixtures}/xna64.exe ${dotnet_fixtures}/hello20.exe
                        COMMAND ${CMAKE_COMMAND} -E make_directory ${dotnet_fixtures}
                        COMMAND ${RAVEN_MCS} -sdk:4.5 -platform:x64 -out:${dotnet_fixtures}/hello64.exe ${PROJECT_SOURCE_DIR}/testdata/dotnet/hello.cs
                        COMMAND ${RAVEN_MCS} -sdk:4.5 -platform:x64 -target:winexe -r:System.Windows.Forms.dll -r:System.Drawing.dll
                            -out:${dotnet_fixtures}/forms64.exe ${PROJECT_SOURCE_DIR}/testdata/dotnet/forms.cs
                        COMMAND ${RAVEN_MCS} -sdk:4.5 -platform:x64 -out:${dotnet_fixtures}/xna64.exe ${PROJECT_SOURCE_DIR}/testdata/dotnet/xna.cs
                        COMMAND ${RAVEN_MCS} -sdk:2 -platform:x64 -out:${dotnet_fixtures}/hello20.exe ${PROJECT_SOURCE_DIR}/testdata/dotnet/hello.cs
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/dotnet/hello.cs ${PROJECT_SOURCE_DIR}/testdata/dotnet/forms.cs
                            ${PROJECT_SOURCE_DIR}/testdata/dotnet/xna.cs VERBATIM)
                    add_custom_target(dotnet_fixtures ALL DEPENDS ${dotnet_fixtures}/hello64.exe ${dotnet_fixtures}/forms64.exe ${dotnet_fixtures}/xna64.exe
                        ${dotnet_fixtures}/hello20.exe)
                    add_test(NAME native_dotnet_framework48 COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-dotnet.py
                        $<TARGET_FILE:raven> ${RAVEN_NETFX_INSTALLER} ${dotnet_fixtures} ${CMAKE_CURRENT_BINARY_DIR}/dotnet-evidence "${RAVEN_XNA_INSTALLER}" "${RAVEN_NETFX35_PACKAGE}")
                    set_tests_properties(native_dotnet_framework48 PROPERTIES TIMEOUT 7200 RESOURCE_LOCK raven_compositor PASS_REGULAR_EXPRESSION "PASS: ")
                    # The same installer run as bootstrappers run it, through its own Setup.exe, then a console program on what it installed.
                    add_test(NAME native_dotnet_setup48 COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-dotnet-setup.py
                        $<TARGET_FILE:raven> ${RAVEN_NETFX_INSTALLER} ${dotnet_fixtures} ${CMAKE_CURRENT_BINARY_DIR}/dotnet-setup-evidence)
                    set_tests_properties(native_dotnet_setup48 PROPERTIES TIMEOUT 3000 PASS_REGULAR_EXPRESSION "PASS: ")
                endif()
                # A self-contained .NET 8 program, published with the .NET SDK given: the runtime loads its libraries of managed code alone,
                # built for any CPU, and a managed exception and a runtime one are thrown and caught.
                set(RAVEN_DOTNET_SDK "" CACHE FILEPATH "the dotnet command of a .NET 8 SDK, which publishes testdata/dotnet/exceptions for win-x64")
                if(RAVEN_DOTNET_SDK AND EXISTS ${RAVEN_DOTNET_SDK})
                    set(dotnet8_fixture ${CMAKE_CURRENT_BINARY_DIR}/dotnet8-exceptions)
                    add_custom_command(OUTPUT ${dotnet8_fixture}/exceptions.exe
                        COMMAND ${CMAKE_COMMAND} -E env DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
                            ${RAVEN_DOTNET_SDK} publish ${PROJECT_SOURCE_DIR}/testdata/dotnet/exceptions/exceptions.csproj -c Release
                            -o ${dotnet8_fixture} -p:BaseIntermediateOutputPath=${dotnet8_fixture}-obj/ -p:BaseOutputPath=${dotnet8_fixture}-bin/
                        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/dotnet/exceptions/exceptions.csproj ${PROJECT_SOURCE_DIR}/testdata/dotnet/exceptions/Program.cs
                        VERBATIM)
                    add_custom_target(dotnet8_fixture ALL DEPENDS ${dotnet8_fixture}/exceptions.exe)
                    add_test(NAME native_dotnet8_exceptions COMMAND raven --run-process --execution=native --timeout-ms=60000 --stdio=inherit
                        ${dotnet8_fixture}/exceptions.exe)
                    set_tests_properties(native_dotnet8_exceptions PROPERTIES TIMEOUT 90
                        PASS_REGULAR_EXPRESSION "start 8\\.[^\n]* 64-bit\ncaught boom\ncaught index\ndone")
                endif()
                add_test(NAME native_x86_process COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/scripts/native-x86-process.py $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/x86-client.exe)
                set_tests_properties(native_x86_process PROPERTIES TIMEOUT 30)
                set(X86_TESTS native_x86_process native_x86_kernel_cli native_x86_resource_cli)
                foreach(CLIENT ${X86_DESKTOP_CLIENTS})
                    list(APPEND X86_TESTS native_x86_desktop_${CLIENT})
                endforeach()
                # PE32 processes always open a prefix; keep tests out of the user's home directory.
                foreach(NAME ${X86_TESTS})
                    if(TEST ${NAME})
                        set_property(TEST ${NAME} APPEND PROPERTY ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/x86-prefix)
                    endif()
                endforeach()
            endif()
            foreach(STATUS 0 73 256)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/return-${STATUS}.exe
                    COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -DRAVEN_RETURN_STATUS=${STATUS}
                        -Wl,--entry,raven_entry,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/return-${STATUS}.exe ${LOADER_SOURCES}/return_client.c
                    DEPENDS ${LOADER_SOURCES}/return_client.c VERBATIM)
            endforeach()
            add_custom_target(return_fixtures ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/return-0.exe
                ${CMAKE_CURRENT_BINARY_DIR}/return-73.exe ${CMAKE_CURRENT_BINARY_DIR}/return-256.exe)
            add_test(NAME native_process_return_status COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/scripts/native-process-status.py $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR})
            set_tests_properties(native_process_return_status PROPERTIES TIMEOUT 30)
            add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/wsprintf.exe
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--no-insert-timestamp
                    -o ${CMAKE_CURRENT_BINARY_DIR}/wsprintf.exe ${LOADER_SOURCES}/wsprintf_client.c -luser32 -lkernel32
                DEPENDS ${LOADER_SOURCES}/wsprintf_client.c VERBATIM)
            add_custom_target(wsprintf_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/wsprintf.exe)
            add_test(NAME native_wsprintf COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/wsprintf.exe)
            set_tests_properties(native_wsprintf PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/wsprintf-prefix)
            add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/display.exe
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--no-insert-timestamp
                    -o ${CMAKE_CURRENT_BINARY_DIR}/display.exe ${LOADER_SOURCES}/display_client.c -luser32 -lkernel32
                DEPENDS ${LOADER_SOURCES}/display_client.c VERBATIM)
            add_custom_target(display_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/display.exe)
            add_test(NAME native_display_enumeration COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/display.exe)
            set_tests_properties(native_display_enumeration PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/display-prefix)
            add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/clipboard.exe
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--no-insert-timestamp
                    -o ${CMAKE_CURRENT_BINARY_DIR}/clipboard.exe ${LOADER_SOURCES}/clipboard_client.c -lole32 -luser32 -lkernel32
                DEPENDS ${LOADER_SOURCES}/clipboard_client.c VERBATIM)
            add_custom_target(clipboard_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/clipboard.exe)
            add_test(NAME native_clipboard_formats COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/clipboard.exe)
            set_tests_properties(native_clipboard_formats PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/clipboard-prefix)
            add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/legacy_file.exe
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--no-insert-timestamp
                    -o ${CMAKE_CURRENT_BINARY_DIR}/legacy_file.exe ${LOADER_SOURCES}/legacy_file_client.c -lkernel32
                DEPENDS ${LOADER_SOURCES}/legacy_file_client.c VERBATIM)
            add_custom_target(legacy_file_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/legacy_file.exe)
            add_test(NAME native_legacy_file_api COMMAND raven --run-process --execution=native ${CMAKE_CURRENT_BINARY_DIR}/legacy_file.exe)
            set_tests_properties(native_legacy_file_api PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/legacy-file-prefix)
            set(RAVEN_OPENGL_PE64_DIR "" CACHE PATH "Directory holding Mesa opengl32.dll and its dependencies for 64-bit programs")
            if(HOST_OPENGL_FOUND AND (TARGET raven_wayland OR RAVEN_X11_TESTS))
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/opengl-host.exe
                    COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-builtin -fno-stack-protector
                        -Wl,--entry,raven_entry,--image-base,0x1f4000000,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/opengl-host.exe ${PROJECT_SOURCE_DIR}/testdata/native_cpu/opengl_host_client.c
                        -lopengl32 -lglu32 -lgdi32 -luser32 -lkernel32
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_cpu/opengl_host_client.c VERBATIM)
                add_custom_target(opengl_host_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/opengl-host.exe)
                if(RAVEN_X11_TESTS)
                    foreach(mode opengl opengl-maximized)
                        add_test(NAME native_x11_${mode} COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-x11.py
                            $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/opengl-host.exe ${mode} ${CMAKE_CURRENT_BINARY_DIR}/x11-${mode})
                        set_tests_properties(native_x11_${mode} PROPERTIES TIMEOUT 60)
                    endforeach()
                endif()
                if(TARGET raven_wayland AND RAVEN_PIL_PYTHON AND RAVEN_LABWC AND RAVEN_GRIM)
                    foreach(mode default parallel parallel_sync parallel_graphics queued)
                        add_test(NAME native_opengl_host_${mode} COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-opengl.py
                            $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/opengl-host.exe native
                            ${CMAKE_CURRENT_BINARY_DIR}/opengl-host-${mode})
                        set_tests_properties(native_opengl_host_${mode} PROPERTIES TIMEOUT 60)
                    endforeach()
                    foreach(elements queued forged offset count)
                        if(elements STREQUAL "queued")
                            set(element_option queued-elements)
                        elseif(elements STREQUAL "forged")
                            set(element_option forged-elements)
                        else()
                            set(element_option bad-element-${elements})
                        endif()
                        add_test(NAME native_opengl_elements_${elements} COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-opengl.py
                            $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/opengl-host.exe native
                            ${CMAKE_CURRENT_BINARY_DIR}/opengl-elements-${elements} ${element_option})
                        set_tests_properties(native_opengl_elements_${elements} PROPERTIES TIMEOUT 60
                            ENVIRONMENT "RAVEN_PARALLEL=1;RAVEN_QUEUED_GRAPHICS=1;RAVEN_QUEUED_OPENGL=1;RAVEN_PROFILE=1")
                    endforeach()
                    set_tests_properties(native_opengl_host_parallel PROPERTIES ENVIRONMENT RAVEN_PARALLEL=1)
                    set_tests_properties(native_opengl_host_parallel_sync PROPERTIES ENVIRONMENT "RAVEN_PARALLEL=1;RAVEN_SYNC_INDEXED_GL=1")
                    set_tests_properties(native_opengl_host_parallel_graphics PROPERTIES ENVIRONMENT "RAVEN_PARALLEL=1;RAVEN_QUEUED_GRAPHICS=1;RAVEN_PROFILE=1")
                    set_tests_properties(native_opengl_host_queued PROPERTIES ENVIRONMENT "RAVEN_PARALLEL=1;RAVEN_QUEUED_GRAPHICS=1;RAVEN_QUEUED_OPENGL=1;RAVEN_PROFILE=1")
                    add_test(NAME native_opengl_host_maximized COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-opengl.py
                        $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/opengl-host.exe native
                        ${CMAKE_CURRENT_BINARY_DIR}/opengl-host-maximized maximize)
                    set_tests_properties(native_opengl_host_maximized PROPERTIES TIMEOUT 60 ENVIRONMENT RAVEN_PARALLEL=1)
                    foreach(array color texture delete attribute vao context)
                        add_test(NAME native_opengl_array_${array} COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-opengl.py
                            $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/opengl-host.exe native
                            ${CMAKE_CURRENT_BINARY_DIR}/opengl-array-${array} array-${array})
                        set_tests_properties(native_opengl_array_${array} PROPERTIES TIMEOUT 60 ENVIRONMENT RAVEN_PARALLEL=1)
                    endforeach()
                endif()
            endif()
            set(RAVEN_WAYLAND_SCREEN_TESTS OFF)
            if(TARGET raven_wayland AND RAVEN_PIL_PYTHON AND RAVEN_LABWC AND RAVEN_GRIM AND RAVEN_WLR_RANDR)
                set(RAVEN_WAYLAND_SCREEN_TESTS ON)
            endif()
            if(RAVEN_WAYLAND_SCREEN_TESTS OR RAVEN_X11_TESTS)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/screen-metrics.exe
                    COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-builtin -fno-stack-protector
                        -Wl,--entry,raven_entry,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/screen-metrics.exe ${LOADER_SOURCES}/screen_metrics_client.c
                        -ldwmapi -lgdi32 -luser32 -lkernel32
                    DEPENDS ${LOADER_SOURCES}/screen_metrics_client.c VERBATIM)
                add_custom_target(screen_metrics_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/screen-metrics.exe)
                if(RAVEN_MINGW32)
                    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/screen-metrics32.exe
                        COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-builtin -fno-stack-protector
                            -Wl,--entry,_raven_entry,--no-insert-timestamp
                            -o ${CMAKE_CURRENT_BINARY_DIR}/screen-metrics32.exe ${LOADER_SOURCES}/screen_metrics_client.c
                            -ldwmapi -lgdi32 -luser32 -lkernel32
                        DEPENDS ${LOADER_SOURCES}/screen_metrics_client.c VERBATIM)
                    add_custom_target(screen_metrics32_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/screen-metrics32.exe)
                endif()
                if(RAVEN_X11_TESTS)
                    add_test(NAME native_x11_metrics COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-x11.py
                        $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/screen-metrics.exe metrics ${CMAKE_CURRENT_BINARY_DIR}/x11-metrics)
                    set_tests_properties(native_x11_metrics PROPERTIES TIMEOUT 60)
                    if(RAVEN_MINGW32)
                        add_test(NAME native_x11_metrics_x32 COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-x11.py
                            $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/screen-metrics32.exe metrics ${CMAKE_CURRENT_BINARY_DIR}/x11-metrics-x32)
                        set_tests_properties(native_x11_metrics_x32 PROPERTIES TIMEOUT 60 ENVIRONMENT RAVEN_PARALLEL=1)
                    endif()
                endif()
            endif()
            if(RAVEN_WAYLAND_SCREEN_TESTS)
                foreach(mode normal scaled rotated resize dark frames)
                    add_test(NAME native_screen_metrics_${mode} COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-screen-metrics.py
                        $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/screen-metrics.exe ${mode}
                        ${CMAKE_CURRENT_BINARY_DIR}/screen-metrics-${mode})
                    set_tests_properties(native_screen_metrics_${mode} PROPERTIES TIMEOUT 45)
                endforeach()
                add_test(NAME native_screen_metrics_parallel COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-screen-metrics.py
                    $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/screen-metrics.exe normal
                    ${CMAKE_CURRENT_BINARY_DIR}/screen-metrics-parallel)
                set_tests_properties(native_screen_metrics_parallel PROPERTIES TIMEOUT 45 ENVIRONMENT RAVEN_PARALLEL=1)
                if(RAVEN_MINGW32)
                    foreach(mode normal resize frames)
                        add_test(NAME native_screen_metrics_x32_${mode} COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-screen-metrics.py
                            $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/screen-metrics32.exe ${mode}
                            ${CMAKE_CURRENT_BINARY_DIR}/screen-metrics-x32-${mode})
                        set_tests_properties(native_screen_metrics_x32_${mode} PROPERTIES TIMEOUT 45 ENVIRONMENT RAVEN_PARALLEL=1)
                    endforeach()
                endif()
            endif()
            if(RAVEN_OPENGL_PE64_DIR AND TARGET vulkan_bridge_test AND TARGET raven_wayland)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/opengl.exe
                    COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -Wl,--entry,raven_entry,--no-insert-timestamp
                        -o ${CMAKE_CURRENT_BINARY_DIR}/opengl.exe ${PROJECT_SOURCE_DIR}/testdata/native_cpu/opengl_client.c
                        -lopengl32 -lgdi32 -luser32 -lkernel32
                    DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_cpu/opengl_client.c VERBATIM)
                add_custom_target(opengl_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/opengl.exe)
                if(RAVEN_PIL_PYTHON AND RAVEN_LABWC AND RAVEN_GRIM)
                    add_test(NAME native_opengl_present COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-opengl.py
                        $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/opengl.exe ${RAVEN_OPENGL_PE64_DIR}
                        ${CMAKE_CURRENT_BINARY_DIR}/opengl-present)
                    set_tests_properties(native_opengl_present PROPERTIES TIMEOUT 60)
                endif()
            endif()
            # The DXVK 64-bit DLLs are provisioned outside the tree (scripts/provision-graphics.py); the test needs a Vulkan driver too.
            set(RAVEN_DXVK_PE64_DIR "" CACHE PATH "Directory holding DXVK d3d11.dll, dxgi.dll and d3d10core.dll for 64-bit programs")
            if(RAVEN_DXVK_PE64_DIR AND EXISTS ${RAVEN_DXVK_PE64_DIR}/d3d11.dll AND TARGET vulkan_bridge_test)
                set(RAVEN_VULKAN_ICD "")
                if(RAVEN_LAVAPIPE_ICD)
                    set(RAVEN_VULKAN_ICD VK_DRIVER_FILES=${RAVEN_LAVAPIPE_ICD})
                endif()
                add_test(NAME native_d3d11_triangle COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11.py
                    $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d11/triangle.exe ${RAVEN_DXVK_PE64_DIR}
                    ${CMAKE_CURRENT_BINARY_DIR}/d3d11-triangle)
                # The software driver's worker threads hold allocations at exit; leak checking still covers everything else.
                set(RAVEN_LSAN LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/testdata/lsan.supp)
                set_tests_properties(native_d3d11_triangle PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                add_test(NAME native_d3d11_deferred COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11.py
                    $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d11/deferred.exe ${RAVEN_DXVK_PE64_DIR}
                    ${CMAKE_CURRENT_BINARY_DIR}/d3d11-deferred)
                set_tests_properties(native_d3d11_deferred PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                add_test(NAME native_d3d11_geometry COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11-geometry.py
                    $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d11/geometry.exe ${RAVEN_DXVK_PE64_DIR}
                    ${CMAKE_CURRENT_BINARY_DIR}/d3d11-geometry)
                set_tests_properties(native_d3d11_geometry PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                if(EXISTS /dev/dri/renderD128)
                    add_test(NAME native_d3d11_geometry_gpu COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11-geometry.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d11/geometry.exe ${RAVEN_DXVK_PE64_DIR}
                        ${CMAKE_CURRENT_BINARY_DIR}/d3d11-geometry-gpu gpu)
                    set_tests_properties(native_d3d11_geometry_gpu PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_LSAN}")
                endif()
                add_test(NAME native_d3d11_tessellation COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11-tessellation.py
                    $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d11/tessellation.exe ${RAVEN_DXVK_PE64_DIR}
                    ${CMAKE_CURRENT_BINARY_DIR}/d3d11-tessellation)
                set_tests_properties(native_d3d11_tessellation PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                if(EXISTS /dev/dri/renderD128)
                    add_test(NAME native_d3d11_tessellation_gpu COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11-tessellation.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d11/tessellation.exe ${RAVEN_DXVK_PE64_DIR}
                        ${CMAKE_CURRENT_BINARY_DIR}/d3d11-tessellation-gpu gpu)
                    set_tests_properties(native_d3d11_tessellation_gpu PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_LSAN}")
                endif()
                # Direct3D 12 through vkd3d-proton: device creation only (dependency path); needs its own DLL directory.
                set(RAVEN_VKD3D_PE64_DIR "" CACHE PATH "Directory holding vkd3d-proton d3d12.dll and d3d12core.dll for 64-bit programs")
                if(RAVEN_VKD3D_PE64_DIR AND EXISTS ${RAVEN_VKD3D_PE64_DIR}/d3d12core.dll)
                    add_test(NAME native_d3d12_device COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/device.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                        ${CMAKE_CURRENT_BINARY_DIR}/d3d12-device)
                    set_tests_properties(native_d3d12_device PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                    add_test(NAME native_d3d12_fences COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-fences.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/fences.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                        ${CMAKE_CURRENT_BINARY_DIR}/d3d12-fences)
                    set_tests_properties(native_d3d12_fences PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                    add_test(NAME native_d3d12_clear COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-clear.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/clear.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                        ${CMAKE_CURRENT_BINARY_DIR}/d3d12-clear)
                    set_tests_properties(native_d3d12_clear PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                    add_test(NAME native_d3d12_draw COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-draw.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/draw.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                        ${CMAKE_CURRENT_BINARY_DIR}/d3d12-draw)
                    set_tests_properties(native_d3d12_draw PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                    add_test(NAME native_d3d12_bundle COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-bundle.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/bundle.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                        ${CMAKE_CURRENT_BINARY_DIR}/d3d12-bundle)
                    set_tests_properties(native_d3d12_bundle PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                    if(EXISTS /dev/dri/renderD128)
                        add_test(NAME native_d3d12_bundle_gpu COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-bundle.py
                            $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/bundle.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                            ${CMAKE_CURRENT_BINARY_DIR}/d3d12-bundle-gpu gpu)
                        set_tests_properties(native_d3d12_bundle_gpu PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_LSAN}")
                    endif()
                    add_test(NAME native_d3d12_geometry COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-stages.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/geometry12.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                        ${CMAKE_CURRENT_BINARY_DIR}/d3d12-geometry)
                    set_tests_properties(native_d3d12_geometry PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                    if(EXISTS /dev/dri/renderD128)
                        add_test(NAME native_d3d12_geometry_gpu COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-stages.py
                            $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/geometry12.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                            ${CMAKE_CURRENT_BINARY_DIR}/d3d12-geometry-gpu gpu)
                        set_tests_properties(native_d3d12_geometry_gpu PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_LSAN}")
                    endif()
                    add_test(NAME native_d3d12_tessellation COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-stages.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/tessellation12.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                        ${CMAKE_CURRENT_BINARY_DIR}/d3d12-tessellation)
                    set_tests_properties(native_d3d12_tessellation PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                    if(EXISTS /dev/dri/renderD128)
                        add_test(NAME native_d3d12_tessellation_gpu COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-stages.py
                            $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/tessellation12.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                            ${CMAKE_CURRENT_BINARY_DIR}/d3d12-tessellation-gpu gpu)
                        set_tests_properties(native_d3d12_tessellation_gpu PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_LSAN}")
                    endif()
                    # Ray tracing runs on the software driver only (the development machine's GPU offers none); no ray tracing there skips the test.
                    add_test(NAME native_d3d12_raytrace COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-raytrace.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/raytrace.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                        ${CMAKE_CURRENT_BINARY_DIR}/d3d12-raytrace)
                    set_tests_properties(native_d3d12_raytrace PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                    add_test(NAME native_d3d12_rayquery COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-raytrace.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/rayquery.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                        ${CMAKE_CURRENT_BINARY_DIR}/d3d12-rayquery)
                    set_tests_properties(native_d3d12_rayquery PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                    add_test(NAME native_d3d12_rtanyhit COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-raytrace.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/rtanyhit.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                        ${CMAKE_CURRENT_BINARY_DIR}/d3d12-rtanyhit)
                    set_tests_properties(native_d3d12_rtanyhit PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                    add_test(NAME native_d3d12_rtprocedural COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-raytrace.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/rtprocedural.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                        ${CMAKE_CURRENT_BINARY_DIR}/d3d12-rtprocedural)
                    set_tests_properties(native_d3d12_rtprocedural PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                    add_test(NAME native_d3d12_rtscene COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-raytrace.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/rtscene.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                        ${CMAKE_CURRENT_BINARY_DIR}/d3d12-rtscene)
                    set_tests_properties(native_d3d12_rtscene PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                    add_test(NAME native_d3d12_wave COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-wave.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/wave.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                        ${CMAKE_CURRENT_BINARY_DIR}/d3d12-wave)
                    set_tests_properties(native_d3d12_wave PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                    if(EXISTS /dev/dri/renderD128)
                        add_test(NAME native_d3d12_wave_gpu COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-wave.py
                            $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/wave.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                            ${CMAKE_CURRENT_BINARY_DIR}/d3d12-wave-gpu gpu)
                        set_tests_properties(native_d3d12_wave_gpu PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 ENVIRONMENT "${RAVEN_LSAN}")
                    endif()
                    add_test(NAME native_d3d12_textured COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-textured.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/textured.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                        ${CMAKE_CURRENT_BINARY_DIR}/d3d12-textured)
                    set_tests_properties(native_d3d12_textured PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                    if(EXISTS /dev/dri/renderD128)
                        add_test(NAME native_d3d12_textured_gpu COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-textured.py
                            $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/textured.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                            ${CMAKE_CURRENT_BINARY_DIR}/d3d12-textured-gpu gpu)
                        set_tests_properties(native_d3d12_textured_gpu PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_LSAN}")
                    endif()
                    add_test(NAME native_d3d12_features COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-features.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/features.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                        ${CMAKE_CURRENT_BINARY_DIR}/d3d12-features)
                    set_tests_properties(native_d3d12_features PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                    if(EXISTS /dev/dri/renderD128)
                        add_test(NAME native_d3d12_features_gpu COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-features.py
                            $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/features.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                            ${CMAKE_CURRENT_BINARY_DIR}/d3d12-features-gpu gpu)
                        set_tests_properties(native_d3d12_features_gpu PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_LSAN}")
                    endif()
                    add_test(NAME native_d3d12_compute COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-compute.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/compute.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                        ${CMAKE_CURRENT_BINARY_DIR}/d3d12-compute)
                    set_tests_properties(native_d3d12_compute PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                    if(EXISTS /dev/dri/renderD128)
                        add_test(NAME native_d3d12_compute_gpu COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-compute.py
                            $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/compute.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                            ${CMAKE_CURRENT_BINARY_DIR}/d3d12-compute-gpu gpu)
                        set_tests_properties(native_d3d12_compute_gpu PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_LSAN}")
                    endif()
                    if(EXISTS /dev/dri/renderD128)
                        add_test(NAME native_d3d12_draw_gpu COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-draw.py
                            $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/draw.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                            ${CMAKE_CURRENT_BINARY_DIR}/d3d12-draw-gpu gpu)
                        set_tests_properties(native_d3d12_draw_gpu PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_LSAN}")
                    endif()
                    if(TARGET raven_wayland AND RAVEN_SKIA_LIBRARY AND RAVEN_LABWC AND RAVEN_GRIM AND RAVEN_WLRCTL AND RAVEN_PIL_PYTHON)
                        add_test(NAME native_d3d12_present COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-present.py
                            $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/window12.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                            ${CMAKE_CURRENT_BINARY_DIR}/d3d12-present)
                        set_tests_properties(native_d3d12_present PROPERTIES TIMEOUT 240 RESOURCE_LOCK raven_compositor ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                        if(EXISTS /dev/dri/renderD128)
                            add_test(NAME native_d3d12_present_gpu COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-present.py
                                $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/window12.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                                ${CMAKE_CURRENT_BINARY_DIR}/d3d12-present-gpu gpu)
                            set_tests_properties(native_d3d12_present_gpu PROPERTIES TIMEOUT 240 RESOURCE_LOCK raven_compositor ENVIRONMENT "${RAVEN_LSAN}")
                        endif()
                        add_test(NAME native_d3d12_present_vsync COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-present.py
                            $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/window12_vsync.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                            ${CMAKE_CURRENT_BINARY_DIR}/d3d12-present-vsync)
                        set_tests_properties(native_d3d12_present_vsync PROPERTIES TIMEOUT 240 RESOURCE_LOCK raven_compositor ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                        add_test(NAME native_d3d12_fullscreen COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-fullscreen.py
                            $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/fullscreen12.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                            ${CMAKE_CURRENT_BINARY_DIR}/d3d12-fullscreen)
                        set_tests_properties(native_d3d12_fullscreen PROPERTIES TIMEOUT 900 RESOURCE_LOCK raven_compositor ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                        if(EXISTS /dev/dri/renderD128)
                            add_test(NAME native_d3d12_fullscreen_gpu COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-fullscreen.py
                                $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/fullscreen12.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                                ${CMAKE_CURRENT_BINARY_DIR}/d3d12-fullscreen-gpu gpu)
                            set_tests_properties(native_d3d12_fullscreen_gpu PROPERTIES TIMEOUT 900 RESOURCE_LOCK raven_compositor ENVIRONMENT "${RAVEN_LSAN}")
                        endif()
                    endif()
                    if(EXISTS /dev/dri/renderD128)
                        add_test(NAME native_d3d12_clear_gpu COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12-clear.py
                            $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/clear.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                            ${CMAKE_CURRENT_BINARY_DIR}/d3d12-clear-gpu gpu)
                        set_tests_properties(native_d3d12_clear_gpu PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_LSAN}")
                    endif()
                    if(EXISTS /dev/dri/renderD128)
                        add_test(NAME native_d3d12_device_gpu COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d12.py
                            $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d12/device.exe ${RAVEN_VKD3D_PE64_DIR} ${RAVEN_DXVK_PE64_DIR}
                            ${CMAKE_CURRENT_BINARY_DIR}/d3d12-device-gpu gpu)
                        set_tests_properties(native_d3d12_device_gpu PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_LSAN}")
                    endif()
                endif()
                add_test(NAME native_d3d11_textured COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11-textured.py
                    $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d11/textured.exe ${RAVEN_DXVK_PE64_DIR} ${CMAKE_CURRENT_BINARY_DIR}/d3d11-textured)
                set_tests_properties(native_d3d11_textured PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                if(EXISTS /dev/dri/renderD128)
                    add_test(NAME native_d3d11_textured_gpu COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11-textured.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d11/textured.exe ${RAVEN_DXVK_PE64_DIR} ${CMAKE_CURRENT_BINARY_DIR}/d3d11-textured-gpu gpu)
                    set_tests_properties(native_d3d11_textured_gpu PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_LSAN}")
                endif()
                add_test(NAME native_d3d11_features COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11-features.py
                    $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d11/features.exe ${RAVEN_DXVK_PE64_DIR} ${CMAKE_CURRENT_BINARY_DIR}/d3d11-features)
                set_tests_properties(native_d3d11_features PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                if(EXISTS /dev/dri/renderD128)
                    add_test(NAME native_d3d11_features_gpu COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11-features.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d11/features.exe ${RAVEN_DXVK_PE64_DIR} ${CMAKE_CURRENT_BINARY_DIR}/d3d11-features-gpu gpu)
                    set_tests_properties(native_d3d11_features_gpu PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_LSAN}")
                endif()
                add_test(NAME native_d3d11_compute COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11-compute.py
                    $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d11/compute.exe ${RAVEN_DXVK_PE64_DIR} ${CMAKE_CURRENT_BINARY_DIR}/d3d11-compute)
                set_tests_properties(native_d3d11_compute PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                if(EXISTS /dev/dri/renderD128)
                    add_test(NAME native_d3d11_compute_gpu COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11-compute.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d11/compute.exe ${RAVEN_DXVK_PE64_DIR} ${CMAKE_CURRENT_BINARY_DIR}/d3d11-compute-gpu gpu)
                    set_tests_properties(native_d3d11_compute_gpu PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_LSAN}")
                endif()
                add_test(NAME native_d3d11_msaa COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11-msaa.py
                    $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d11/msaa.exe ${RAVEN_DXVK_PE64_DIR} ${CMAKE_CURRENT_BINARY_DIR}/d3d11-msaa)
                set_tests_properties(native_d3d11_msaa PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                if(EXISTS /dev/dri/renderD128)
                    add_test(NAME native_d3d11_msaa_gpu COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11-msaa.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d11/msaa.exe ${RAVEN_DXVK_PE64_DIR} ${CMAKE_CURRENT_BINARY_DIR}/d3d11-msaa-gpu gpu)
                    set_tests_properties(native_d3d11_msaa_gpu PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_LSAN}")
                endif()
                if(RAVEN_X11_TESTS)
                    add_test(NAME native_x11_d3d11_present COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-x11.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d11/window.exe present ${CMAKE_CURRENT_BINARY_DIR}/x11-d3d11-present ${RAVEN_DXVK_PE64_DIR})
                    set_tests_properties(native_x11_d3d11_present PROPERTIES TIMEOUT 240 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                endif()
                # Presenting to a window also needs the Wayland backend, an isolated compositor and a capture tool.
                if(TARGET raven_wayland AND RAVEN_SKIA_LIBRARY AND RAVEN_LABWC AND RAVEN_GRIM AND RAVEN_WLRCTL AND RAVEN_PIL_PYTHON)
                    add_test(NAME native_d3d11_present COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11-present.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d11/window.exe ${RAVEN_DXVK_PE64_DIR}
                        ${CMAKE_CURRENT_BINARY_DIR}/d3d11-present)
                    set_tests_properties(native_d3d11_present PROPERTIES TIMEOUT 240 RESOURCE_LOCK raven_compositor ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                    add_test(NAME native_d3d11_children COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11-children.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d11/children.exe ${RAVEN_DXVK_PE64_DIR}
                        ${CMAKE_CURRENT_BINARY_DIR}/d3d11-children)
                    set_tests_properties(native_d3d11_children PROPERTIES TIMEOUT 240 RESOURCE_LOCK raven_compositor ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                    if(EXISTS /dev/dri/renderD128)
                        add_test(NAME native_d3d11_present_gpu COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11-present.py
                            $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d11/window.exe ${RAVEN_DXVK_PE64_DIR}
                            ${CMAKE_CURRENT_BINARY_DIR}/d3d11-present-gpu gpu)
                        set_tests_properties(native_d3d11_present_gpu PROPERTIES TIMEOUT 240 RESOURCE_LOCK raven_compositor ENVIRONMENT "${RAVEN_LSAN}")
                    endif()
                    add_test(NAME native_d3d11_fullscreen COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11-fullscreen.py
                        $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d11/fullscreen.exe ${RAVEN_DXVK_PE64_DIR}
                        ${CMAKE_CURRENT_BINARY_DIR}/d3d11-fullscreen)
                    set_tests_properties(native_d3d11_fullscreen PROPERTIES TIMEOUT 900 RESOURCE_LOCK raven_compositor ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                    if(EXISTS /dev/dri/renderD128)
                        add_test(NAME native_d3d11_fullscreen_gpu COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11-fullscreen.py
                            $<TARGET_FILE:raven> ${RAVEN_TESTDATA_DIR}/d3d11/fullscreen.exe ${RAVEN_DXVK_PE64_DIR}
                            ${CMAKE_CURRENT_BINARY_DIR}/d3d11-fullscreen-gpu gpu)
                        set_tests_properties(native_d3d11_fullscreen_gpu PROPERTIES TIMEOUT 900 RESOURCE_LOCK raven_compositor ENVIRONMENT "${RAVEN_LSAN}")
                    endif()
                    # A 32-bit program drawing through 32-bit DXVK, the 32-bit Vulkan loader and the same bridge.
                    if(RAVEN_MINGW32 AND RAVEN_DXVK_PE32_DIR AND EXISTS "${RAVEN_DXVK_PE32_DIR}/d3d9.dll" AND TARGET vulkan_thunk32_generated)
                        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/d3d9-window.exe
                            COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector -fno-builtin
                                -Wl,--entry,_raven_entry,--no-insert-timestamp
                                -o ${CMAKE_CURRENT_BINARY_DIR}/d3d9-window.exe ${PROJECT_SOURCE_DIR}/testdata/d3d9/window.c -lkernel32 -luser32
                            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/d3d9/window.c VERBATIM)
                        add_custom_target(d3d9_window_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/d3d9-window.exe)
                        add_test(NAME native_d3d9_present COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-d3d9-present.py
                            $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/d3d9-window.exe ${RAVEN_DXVK_PE32_DIR}
                            ${CMAKE_CURRENT_BINARY_DIR}/d3d9-present)
                        set_tests_properties(native_d3d9_present PROPERTIES TIMEOUT 480 RESOURCE_LOCK raven_compositor ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                    endif()
                    # The built-in controls drawn by a 64-bit and a 32-bit build of the same program must be the same picture.
                    if(RAVEN_MINGW32 AND RAVEN_MINGW64 AND RAVEN_CONTROLS_COMPONENT_DIR AND RAVEN_CONTROLS32_COMPONENT_DIR)
                        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/controls-probe64.exe ${CMAKE_CURRENT_BINARY_DIR}/controls-probe32.exe
                            COMMAND ${RAVEN_MINGW64} -O1 -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe
                                -Wl,--entry,raven_entry,--image-base,0x1b9000000
                                -o ${CMAKE_CURRENT_BINARY_DIR}/controls-probe64.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/controls_probe.c -lkernel32 -luser32 -lgdi32
                            COMMAND ${RAVEN_MINGW32} -O1 -nostdlib -fno-stack-protector -fno-builtin
                                -Wl,--entry,_raven_entry,--no-insert-timestamp
                                -o ${CMAKE_CURRENT_BINARY_DIR}/controls-probe32.exe ${PROJECT_SOURCE_DIR}/testdata/native_loader/controls_probe.c -lkernel32 -luser32 -lgdi32
                            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/controls_probe.c VERBATIM)
                        add_custom_target(controls_probe_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/controls-probe64.exe ${CMAKE_CURRENT_BINARY_DIR}/controls-probe32.exe)
                        add_test(NAME native_x86_controls_match COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-controls-probe.py
                            $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/controls-probe64.exe ${CMAKE_CURRENT_BINARY_DIR}/controls-probe32.exe
                            ${CMAKE_CURRENT_BINARY_DIR}/controls-match)
                        set_tests_properties(native_x86_controls_match PROPERTIES TIMEOUT 400 RESOURCE_LOCK raven_compositor)
                    endif()
                    # The Direct3D 11 workloads built for 32 bits, through the 32-bit DXVK that sits beside them as the program's own libraries.
                    if(RAVEN_MINGW32 AND RAVEN_DXVK_PE32_DIR AND EXISTS "${RAVEN_DXVK_PE32_DIR}/d3d11.dll" AND TARGET vulkan_thunk32_generated)
                        foreach(workload triangle geometry tessellation window fullscreen)
                            add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/d3d11-${workload}32.exe
                                COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector -fno-builtin
                                    -Wl,--entry,_entry@0,--no-insert-timestamp
                                    -o ${CMAKE_CURRENT_BINARY_DIR}/d3d11-${workload}32.exe ${PROJECT_SOURCE_DIR}/testdata/d3d11/${workload}.c
                                    -ld3d11 -ldxgi -ldxguid -luuid -luser32 -lkernel32
                                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/d3d11/${workload}.c ${PROJECT_SOURCE_DIR}/testdata/d3d11/tri_shaders.h
                                    ${PROJECT_SOURCE_DIR}/testdata/d3d11/gs_shaders.h ${PROJECT_SOURCE_DIR}/testdata/d3d11/tess_shaders.h VERBATIM)
                            add_custom_target(d3d11_${workload}32_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/d3d11-${workload}32.exe)
                        endforeach()
                        add_test(NAME native_x86_d3d11_triangle COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11.py
                            $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/d3d11-triangle32.exe ${RAVEN_DXVK_PE32_DIR}
                            ${CMAKE_CURRENT_BINARY_DIR}/x86-d3d11-triangle)
                        add_test(NAME native_x86_d3d11_geometry COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11-geometry.py
                            $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/d3d11-geometry32.exe ${RAVEN_DXVK_PE32_DIR}
                            ${CMAKE_CURRENT_BINARY_DIR}/x86-d3d11-geometry)
                        add_test(NAME native_x86_d3d11_tessellation COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11-tessellation.py
                            $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/d3d11-tessellation32.exe ${RAVEN_DXVK_PE32_DIR}
                            ${CMAKE_CURRENT_BINARY_DIR}/x86-d3d11-tessellation)
                        set_tests_properties(native_x86_d3d11_triangle native_x86_d3d11_geometry native_x86_d3d11_tessellation
                            PROPERTIES TIMEOUT 300 ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                        # Presentation to a Wayland window needs the isolated compositor.
                        add_test(NAME native_x86_d3d11_present COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11-present.py
                            $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/d3d11-window32.exe ${RAVEN_DXVK_PE32_DIR}
                            ${CMAKE_CURRENT_BINARY_DIR}/x86-d3d11-present)
                        set_tests_properties(native_x86_d3d11_present PROPERTIES TIMEOUT 480 RESOURCE_LOCK raven_compositor ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                        if(EXISTS /dev/dri/renderD128)
                            add_test(NAME native_x86_d3d11_present_gpu COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11-present.py
                                $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/d3d11-window32.exe ${RAVEN_DXVK_PE32_DIR}
                                ${CMAKE_CURRENT_BINARY_DIR}/x86-d3d11-present-gpu gpu)
                            set_tests_properties(native_x86_d3d11_present_gpu PROPERTIES TIMEOUT 480 RESOURCE_LOCK raven_compositor ENVIRONMENT "${RAVEN_LSAN}")
                        endif()
                        add_test(NAME native_x86_d3d11_fullscreen COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-d3d11-fullscreen.py
                            $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/d3d11-fullscreen32.exe ${RAVEN_DXVK_PE32_DIR}
                            ${CMAKE_CURRENT_BINARY_DIR}/x86-d3d11-fullscreen)
                        set_tests_properties(native_x86_d3d11_fullscreen PROPERTIES TIMEOUT 900 RESOURCE_LOCK raven_compositor ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                    endif()
                    # The same workload built for 64 bits, through the 64-bit DXVK.
                    if(RAVEN_MINGW64 AND RAVEN_DXVK_PE64_DIR)
                        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/d3d9-window64.exe
                            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -fno-builtin
                                -Wl,--entry,raven_entry,--no-insert-timestamp
                                -o ${CMAKE_CURRENT_BINARY_DIR}/d3d9-window64.exe ${PROJECT_SOURCE_DIR}/testdata/d3d9/window.c -lkernel32 -luser32
                            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/d3d9/window.c VERBATIM)
                        add_custom_target(d3d9_window64_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/d3d9-window64.exe)
                        add_test(NAME native_d3d9_present_x64 COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-d3d9-present.py
                            $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/d3d9-window64.exe ${RAVEN_DXVK_PE64_DIR}
                            ${CMAKE_CURRENT_BINARY_DIR}/d3d9-present-x64)
                        set_tests_properties(native_d3d9_present_x64 PROPERTIES TIMEOUT 480 RESOURCE_LOCK raven_compositor ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                        # Direct3D 8 reaches the same Vulkan through DXVK's d3d8.dll, which sits on its Direct3D 9.
                        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/d3d8-window64.exe
                            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -fno-builtin
                                -Wl,--entry,raven_entry,--no-insert-timestamp
                                -o ${CMAKE_CURRENT_BINARY_DIR}/d3d8-window64.exe ${PROJECT_SOURCE_DIR}/testdata/d3d8/window.c -lkernel32 -luser32
                            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/d3d8/window.c VERBATIM)
                        add_custom_target(d3d8_window64_fixture ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/d3d8-window64.exe)
                        add_test(NAME native_d3d8_present COMMAND ${RAVEN_PIL_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/native-d3d9-present.py
                            $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/d3d8-window64.exe ${RAVEN_DXVK_PE64_DIR}
                            ${CMAKE_CURRENT_BINARY_DIR}/d3d8-present)
                        set_tests_properties(native_d3d8_present PROPERTIES TIMEOUT 480 RESOURCE_LOCK raven_compositor ENVIRONMENT "${RAVEN_VULKAN_ICD};${RAVEN_LSAN}")
                    endif()
                endif()
            endif()
        endif()
        set(PARAMETER_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/parameter-fixtures)
        file(MAKE_DIRECTORY ${PARAMETER_FIXTURES})
        add_custom_command(OUTPUT ${PARAMETER_FIXTURES}/parameters.dll ${PARAMETER_FIXTURES}/parameters.a
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -shared -DRAVEN_PARAMETER_DLL
                -Wl,--entry,DllMain,--image-base,0x188000000,--out-implib,${PARAMETER_FIXTURES}/parameters.a
                -o ${PARAMETER_FIXTURES}/parameters.dll ${LOADER_SOURCES}/process_parameters_client.c -lkernel32
            DEPENDS ${LOADER_SOURCES}/process_parameters_client.c VERBATIM)
        add_custom_command(OUTPUT ${PARAMETER_FIXTURES}/parameters.exe
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x189000000
                -o ${PARAMETER_FIXTURES}/parameters.exe ${LOADER_SOURCES}/process_parameters_client.c
                ${PARAMETER_FIXTURES}/parameters.a -lkernel32
            DEPENDS ${LOADER_SOURCES}/process_parameters_client.c ${PARAMETER_FIXTURES}/parameters.a VERBATIM)
        add_custom_target(parameter_pe_fixtures ALL DEPENDS ${PARAMETER_FIXTURES}/parameters.exe ${PARAMETER_FIXTURES}/parameters.dll)
        add_executable(process_parameters_test nt/process_parameters_test.cpp)
        target_link_libraries(process_parameters_test PRIVATE raven_cpu)
        add_dependencies(process_parameters_test parameter_pe_fixtures raven-bootstrap)
        add_test(NAME native_process_parameters COMMAND process_parameters_test $<TARGET_FILE:raven-bootstrap> ${PARAMETER_FIXTURES})
        set_tests_properties(native_process_parameters PROPERTIES TIMEOUT 30)
        find_program(RAVEN_LLD_LINK NAMES lld-link lld-link-21)
        if(RAVEN_LLD_LINK)
            execute_process(COMMAND ${RAVEN_MINGW64} -print-file-name=libkernel32.a
                OUTPUT_VARIABLE RAVEN_KERNEL32_IMPORT OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
            set(DELAY_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/delay-fixtures)
            set(DELAY_HELPER ${LOADER_SOURCES}/delay_helper.c)
            add_custom_command(OUTPUT ${DELAY_FIXTURES}/helper.obj ${DELAY_FIXTURES}/client.obj ${DELAY_FIXTURES}/target.obj
                COMMAND ${CMAKE_COMMAND} -E make_directory ${DELAY_FIXTURES}
                COMMAND ${RAVEN_MINGW64} -O2 -fno-stack-protector -c ${DELAY_HELPER} -o ${DELAY_FIXTURES}/helper.obj
                COMMAND ${RAVEN_MINGW64} -O2 -fno-stack-protector -c ${LOADER_SOURCES}/delay_client.c -o ${DELAY_FIXTURES}/client.obj
                COMMAND ${RAVEN_MINGW64} -O2 -fno-stack-protector -DRAVEN_TLS_DLL -DRAVEN_TLS_DYNAMIC
                    -c ${LOADER_SOURCES}/tls_client.c -o ${DELAY_FIXTURES}/target.obj
                DEPENDS ${DELAY_HELPER} ${LOADER_SOURCES}/delay_client.c ${LOADER_SOURCES}/tls_client.c VERBATIM)
            add_custom_command(OUTPUT ${DELAY_FIXTURES}/delayed.dll ${DELAY_FIXTURES}/delayed.lib
                COMMAND ${RAVEN_LLD_LINK} /dll /entry:DllMain /nodefaultlib /base:0x190000000
                    /out:${DELAY_FIXTURES}/delayed.dll /implib:${DELAY_FIXTURES}/delayed.lib
                    /def:${LOADER_SOURCES}/delay_target.def ${DELAY_FIXTURES}/target.obj ${RAVEN_KERNEL32_IMPORT}
                DEPENDS ${DELAY_FIXTURES}/target.obj ${LOADER_SOURCES}/delay_target.def VERBATIM)
            add_custom_command(OUTPUT ${DELAY_FIXTURES}/client.exe
                COMMAND ${RAVEN_LLD_LINK} /entry:raven_entry /subsystem:console /nodefaultlib /base:0x192000000
                    /delayload:delayed.dll /out:${DELAY_FIXTURES}/client.exe
                    ${DELAY_FIXTURES}/client.obj ${DELAY_FIXTURES}/helper.obj ${DELAY_FIXTURES}/delayed.lib ${RAVEN_KERNEL32_IMPORT}
                DEPENDS ${DELAY_FIXTURES}/client.obj ${DELAY_FIXTURES}/helper.obj ${DELAY_FIXTURES}/delayed.lib VERBATIM)
            add_custom_target(delay_pe_fixtures ALL DEPENDS ${DELAY_FIXTURES}/client.exe ${DELAY_FIXTURES}/delayed.dll)
            # A program that delay-loads the setup API gets the component behind it as one that imports it does; its name must not look like an
            # installer's, which gets the component anyway.
            # lld-link delay-loads only through an import library of its own format, written here for the one function the client calls.
            add_custom_command(OUTPUT ${DELAY_FIXTURES}/cabinet-client.exe
                COMMAND ${RAVEN_MINGW64} -O1 -fno-builtin -fno-stack-protector -c ${LOADER_SOURCES}/delay_setupapi_client.c
                    -o ${DELAY_FIXTURES}/cabinet-client.obj
                COMMAND ${RAVEN_LLD_LINK} /lib /machine:x64 /def:${LOADER_SOURCES}/delay_setupapi.def /out:${DELAY_FIXTURES}/setupapi.lib
                COMMAND ${RAVEN_LLD_LINK} /entry:raven_entry /subsystem:console /nodefaultlib /base:0x192000000
                    /delayload:setupapi.dll /out:${DELAY_FIXTURES}/cabinet-client.exe
                    ${DELAY_FIXTURES}/cabinet-client.obj ${DELAY_FIXTURES}/helper.obj ${DELAY_FIXTURES}/setupapi.lib ${RAVEN_KERNEL32_IMPORT}
                DEPENDS ${LOADER_SOURCES}/delay_setupapi_client.c ${LOADER_SOURCES}/delay_setupapi.def ${DELAY_FIXTURES}/helper.obj VERBATIM)
            add_custom_target(delay_setupapi_fixture ALL DEPENDS ${DELAY_FIXTURES}/cabinet-client.exe)
            add_test(NAME native_delay_setupapi COMMAND raven --run-process --execution=native ${DELAY_FIXTURES}/cabinet-client.exe)
            set_tests_properties(native_delay_setupapi PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            # A queued copy that fails: skipped by default, fatal when its style forbids skipping.
            set(SETUPAPI_QUEUE_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/setupapi-queue.exe)
            add_custom_command(OUTPUT ${SETUPAPI_QUEUE_CLIENT}
                COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x193000000,--no-insert-timestamp
                    -o ${SETUPAPI_QUEUE_CLIENT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/setupapi_queue_client.c -lsetupapi -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/setupapi_queue_client.c VERBATIM)
            add_custom_target(setupapi_queue_fixture ALL DEPENDS ${SETUPAPI_QUEUE_CLIENT})
            add_test(NAME native_setupapi_copy_errors COMMAND raven --run-process --execution=native ${SETUPAPI_QUEUE_CLIENT})
            set_tests_properties(native_setupapi_copy_errors PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            add_executable(delay_loader_test nt/delay_loader_test.cpp)
            target_link_libraries(delay_loader_test PRIVATE raven_cpu)
            add_dependencies(delay_loader_test delay_pe_fixtures raven-bootstrap)
            add_test(NAME native_delay_loader_contract COMMAND delay_loader_test $<TARGET_FILE:raven-bootstrap> ${DELAY_FIXTURES})
            set_tests_properties(native_delay_loader_contract PROPERTIES TIMEOUT 15)
            add_test(NAME native_delay_loader_cli COMMAND raven --run-process --execution=native ${DELAY_FIXTURES}/client.exe)
            set_tests_properties(native_delay_loader_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            # C++ exceptions with the Microsoft compiler's tables against the runtime's msvcrt.dll, in both widths: clang emits them for the
            # Windows targets, lld-link links the program; the 32-bit import library keeps the stdcall decoration of _CxxThrowException.
            find_program(RAVEN_DLLTOOL32 i686-w64-mingw32-dlltool)
            if(RAVEN_CLANG AND RAVEN_MINGW32 AND RAVEN_DLLTOOL32)
                execute_process(COMMAND ${RAVEN_MINGW64} -print-file-name=libmsvcrt.a
                    OUTPUT_VARIABLE RAVEN_MSVCRT_IMPORT OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
                execute_process(COMMAND ${RAVEN_MINGW32} -print-file-name=libkernel32.a
                    OUTPUT_VARIABLE RAVEN_KERNEL32_IMPORT32 OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
                set(CXX_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/cxx-fixtures)
                add_custom_command(OUTPUT ${CXX_FIXTURES}/cxx-exceptions64.exe ${CXX_FIXTURES}/cxx-exceptions32.exe
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${CXX_FIXTURES}
                    COMMAND ${RAVEN_CLANG} --target=x86_64-pc-windows-msvc -O1 -fexceptions -fcxx-exceptions -c
                        ${LOADER_SOURCES}/cxx_exceptions_client.cpp -o ${CXX_FIXTURES}/client64.obj
                    COMMAND ${RAVEN_LLD_LINK} /nologo /entry:raven_entry /subsystem:console /nodefaultlib /out:${CXX_FIXTURES}/cxx-exceptions64.exe
                        ${CXX_FIXTURES}/client64.obj ${RAVEN_MSVCRT_IMPORT} ${RAVEN_KERNEL32_IMPORT}
                    COMMAND ${RAVEN_DLLTOOL32} -k -d ${LOADER_SOURCES}/cxx_msvcrt32.def -l ${CXX_FIXTURES}/msvcrt32.lib
                    COMMAND ${RAVEN_CLANG} --target=i686-pc-windows-msvc -O1 -fexceptions -fcxx-exceptions -c
                        ${LOADER_SOURCES}/cxx_exceptions_client.cpp -o ${CXX_FIXTURES}/client32.obj
                    COMMAND ${RAVEN_LLD_LINK} /nologo /entry:raven_entry /subsystem:console /nodefaultlib /safeseh:no
                        /out:${CXX_FIXTURES}/cxx-exceptions32.exe ${CXX_FIXTURES}/client32.obj ${CXX_FIXTURES}/msvcrt32.lib ${RAVEN_KERNEL32_IMPORT32}
                    DEPENDS ${LOADER_SOURCES}/cxx_exceptions_client.cpp ${LOADER_SOURCES}/cxx_msvcrt32.def VERBATIM)
                add_custom_target(cxx_exception_fixtures ALL DEPENDS ${CXX_FIXTURES}/cxx-exceptions64.exe ${CXX_FIXTURES}/cxx-exceptions32.exe)
                foreach(width 64 32)
                    add_test(NAME native_cxx_exceptions_x${width} COMMAND raven --run-process --execution=native ${CXX_FIXTURES}/cxx-exceptions${width}.exe)
                    set_tests_properties(native_cxx_exceptions_x${width} PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                        ENVIRONMENT RAVEN_PREFIX=${KERNEL_PREFIX})
                endforeach()
            endif()
            # The same through GCC's exceptions, which resume the unwind with a new exception at each frame that has locals to destroy.
            find_program(RAVEN_MINGW64_CXX x86_64-w64-mingw32-g++)
            if(RAVEN_MINGW64_CXX)
                set(GCC_EXCEPTIONS_CLIENT ${CMAKE_CURRENT_BINARY_DIR}/gcc-exceptions.exe)
                add_custom_command(OUTPUT ${GCC_EXCEPTIONS_CLIENT}
                    COMMAND ${RAVEN_MINGW64_CXX} -O1 -static -o ${GCC_EXCEPTIONS_CLIENT} ${LOADER_SOURCES}/gcc_exceptions_client.cpp
                    DEPENDS ${LOADER_SOURCES}/gcc_exceptions_client.cpp VERBATIM)
                add_custom_target(gcc_exceptions_fixture ALL DEPENDS ${GCC_EXCEPTIONS_CLIENT})
                add_test(NAME native_gcc_exceptions COMMAND raven --run-process --execution=native --timeout-ms=60000 ${GCC_EXCEPTIONS_CLIENT})
                set_tests_properties(native_gcc_exceptions PROPERTIES TIMEOUT 90 PASS_REGULAR_EXPRESSION "exited with status 0[^0-9]"
                    ENVIRONMENT RAVEN_PREFIX=${KERNEL_PREFIX})
            endif()
        endif()
        find_program(RAVEN_WINDRES x86_64-w64-mingw32-windres)
        find_program(RAVEN_MINGW32 i686-w64-mingw32-gcc)
        find_program(RAVEN_WINDRES32 i686-w64-mingw32-windres)
        if(RAVEN_WINDRES)
            set(STRING_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/string-fixtures)
            add_custom_command(OUTPUT ${STRING_FIXTURES}/strings.o
                COMMAND ${CMAKE_COMMAND} -E make_directory ${STRING_FIXTURES}
                COMMAND ${RAVEN_WINDRES} ${LOADER_SOURCES}/string_resources.rc ${STRING_FIXTURES}/strings.o
                DEPENDS ${LOADER_SOURCES}/string_resources.rc VERBATIM)
            add_custom_command(OUTPUT ${STRING_FIXTURES}/client.exe ${STRING_FIXTURES}/strings.dll
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x192000000
                    -o ${STRING_FIXTURES}/client.exe ${LOADER_SOURCES}/string_client.c ${STRING_FIXTURES}/strings.o -lkernel32 -luser32
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -shared -fno-stack-protector
                    -Wl,--entry,0,--image-base,0x190000000
                    -o ${STRING_FIXTURES}/strings.dll ${LOADER_SOURCES}/resource_module.c ${STRING_FIXTURES}/strings.o
                DEPENDS ${STRING_FIXTURES}/strings.o ${LOADER_SOURCES}/string_client.c ${LOADER_SOURCES}/resource_module.c VERBATIM)
            add_custom_target(string_pe_fixtures ALL DEPENDS ${STRING_FIXTURES}/client.exe ${STRING_FIXTURES}/strings.dll)
            add_executable(strings_test desktop/strings_test.cpp)
            target_link_libraries(strings_test PRIVATE raven_cpu)
            add_dependencies(strings_test string_pe_fixtures raven-bootstrap)
            add_test(NAME native_string_resource_contract COMMAND strings_test $<TARGET_FILE:raven-bootstrap> ${STRING_FIXTURES})
            set_tests_properties(native_string_resource_contract PROPERTIES TIMEOUT 15)
            add_test(NAME native_string_resource_cli COMMAND raven --run-process --execution=native ${STRING_FIXTURES}/client.exe)
            set_tests_properties(native_string_resource_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            if(RAVEN_MINGW32 AND RAVEN_WINDRES32)
                add_custom_command(OUTPUT ${STRING_FIXTURES}/x86/pointer.exe
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${STRING_FIXTURES}/x86
                    COMMAND ${RAVEN_WINDRES32} ${LOADER_SOURCES}/string_resources.rc ${STRING_FIXTURES}/x86/strings.o
                    COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector -Wl,--entry,_raven_entry
                        -o ${STRING_FIXTURES}/x86/pointer.exe ${LOADER_SOURCES}/string_pointer_client.c ${STRING_FIXTURES}/x86/strings.o -luser32
                    DEPENDS ${LOADER_SOURCES}/string_resources.rc ${LOADER_SOURCES}/string_pointer_client.c VERBATIM)
                add_custom_target(string_pe32_fixture ALL DEPENDS ${STRING_FIXTURES}/x86/pointer.exe)
                add_test(NAME native_x86_string_pointer_cli COMMAND raven --run-process --execution=native ${STRING_FIXTURES}/x86/pointer.exe)
                set_tests_properties(native_x86_string_pointer_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                    ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/x86-prefix)
            endif()
            set(STOCK_ICON_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/stock-icon-fixtures)
            add_custom_command(OUTPUT ${STOCK_ICON_FIXTURES}/client.exe
                COMMAND ${CMAKE_COMMAND} -E make_directory ${STOCK_ICON_FIXTURES}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -Wl,--entry,raven_entry
                    -o ${STOCK_ICON_FIXTURES}/client.exe ${LOADER_SOURCES}/stock_icon_client.c -lshell32 -luser32 -lgdi32
                DEPENDS ${LOADER_SOURCES}/stock_icon_client.c VERBATIM)
            add_custom_target(stock_icon_pe_fixture ALL DEPENDS ${STOCK_ICON_FIXTURES}/client.exe)
            add_test(NAME native_stock_icon_cli COMMAND raven --run-process --execution=native ${STOCK_ICON_FIXTURES}/client.exe)
            set_tests_properties(native_stock_icon_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            if(RAVEN_MINGW32)
                add_custom_command(OUTPUT ${STOCK_ICON_FIXTURES}/x86/client.exe
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${STOCK_ICON_FIXTURES}/x86
                    COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector -Wl,--entry,_raven_entry
                        -o ${STOCK_ICON_FIXTURES}/x86/client.exe ${LOADER_SOURCES}/stock_icon_client.c -lshell32 -luser32 -lgdi32
                    DEPENDS ${LOADER_SOURCES}/stock_icon_client.c VERBATIM)
                add_custom_target(stock_icon_pe32_fixture ALL DEPENDS ${STOCK_ICON_FIXTURES}/x86/client.exe)
                add_test(NAME native_x86_stock_icon_cli COMMAND raven --run-process --execution=native ${STOCK_ICON_FIXTURES}/x86/client.exe)
                set_tests_properties(native_x86_stock_icon_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                    ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/x86-prefix)
            endif()
            set(RESOURCE_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/resource-fixtures)
            add_custom_command(OUTPUT ${RESOURCE_FIXTURES}/resources.o
                COMMAND ${CMAKE_COMMAND} -E make_directory ${RESOURCE_FIXTURES}
                COMMAND ${RAVEN_WINDRES} ${LOADER_SOURCES}/resources.rc ${RESOURCE_FIXTURES}/resources.o
                DEPENDS ${LOADER_SOURCES}/resources.rc VERBATIM)
            add_custom_command(OUTPUT ${RESOURCE_FIXTURES}/client.exe ${RESOURCE_FIXTURES}/resources.dll
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x192000000
                    -o ${RESOURCE_FIXTURES}/client.exe ${LOADER_SOURCES}/resource_client.c ${RESOURCE_FIXTURES}/resources.o -lkernel32
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -shared -fno-stack-protector
                    -Wl,--entry,0,--image-base,0x190000000
                    -o ${RESOURCE_FIXTURES}/resources.dll ${LOADER_SOURCES}/resource_module.c ${RESOURCE_FIXTURES}/resources.o
                DEPENDS ${RESOURCE_FIXTURES}/resources.o ${LOADER_SOURCES}/resource_client.c ${LOADER_SOURCES}/resource_module.c VERBATIM)
            add_custom_target(resource_pe_fixtures ALL DEPENDS ${RESOURCE_FIXTURES}/client.exe ${RESOURCE_FIXTURES}/resources.dll)
            add_executable(resources_test nt/resources_test.cpp)
            target_link_libraries(resources_test PRIVATE raven_cpu)
            add_dependencies(resources_test resource_pe_fixtures raven-bootstrap)
            add_test(NAME native_resource_contract COMMAND resources_test $<TARGET_FILE:raven-bootstrap> ${RESOURCE_FIXTURES})
            set_tests_properties(native_resource_contract PROPERTIES TIMEOUT 15)
            add_test(NAME native_resource_cli COMMAND raven --run-process --execution=native ${RESOURCE_FIXTURES}/client.exe)
            set_tests_properties(native_resource_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            if(RAVEN_MINGW32 AND RAVEN_WINDRES32)
                add_custom_command(OUTPUT ${RESOURCE_FIXTURES}/resources32.o ${RESOURCE_FIXTURES}/client32.exe
                    COMMAND ${RAVEN_WINDRES32} ${LOADER_SOURCES}/resources.rc ${RESOURCE_FIXTURES}/resources32.o
                    COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector
                        -Wl,--entry,_raven_entry -o ${RESOURCE_FIXTURES}/client32.exe
                        ${LOADER_SOURCES}/resource_client.c ${RESOURCE_FIXTURES}/resources32.o -lkernel32
                    DEPENDS ${RESOURCE_FIXTURES}/resources.o ${LOADER_SOURCES}/resource_client.c ${LOADER_SOURCES}/resources.rc VERBATIM)
                file(MAKE_DIRECTORY ${RESOURCE_FIXTURES}/x86)
                add_custom_command(OUTPUT ${RESOURCE_FIXTURES}/x86/resources.dll
                    COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -shared -fno-stack-protector
                        -Wl,--entry,0 -o ${RESOURCE_FIXTURES}/x86/resources.dll
                        ${LOADER_SOURCES}/resource_module.c ${RESOURCE_FIXTURES}/resources32.o
                    DEPENDS ${RESOURCE_FIXTURES}/resources32.o ${LOADER_SOURCES}/resource_module.c VERBATIM)
                add_custom_target(resource_pe32_fixture ALL DEPENDS ${RESOURCE_FIXTURES}/client32.exe ${RESOURCE_FIXTURES}/x86/resources.dll)
                add_test(NAME native_x86_resource_cli COMMAND raven --run-process --execution=native
                    ${RESOURCE_FIXTURES}/client32.exe ${RESOURCE_FIXTURES}/x86/resources.dll)
                set_tests_properties(native_x86_resource_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
                add_executable(pe_resources_test pe/resources_test.cpp)
                target_link_libraries(pe_resources_test PRIVATE raven_pe)
                add_dependencies(pe_resources_test resource_pe_fixtures resource_pe32_fixture)
                add_test(NAME pe_resource_contract COMMAND pe_resources_test ${RESOURCE_FIXTURES}/client.exe ${RESOURCE_FIXTURES}/client32.exe)
                set_tests_properties(pe_resource_contract PROPERTIES TIMEOUT 10)
            endif()
        endif()
        set(DYNAMIC_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/dynamic-fixtures)
        file(MAKE_DIRECTORY ${DYNAMIC_FIXTURES})
        add_custom_command(OUTPUT ${DYNAMIC_FIXTURES}/tlsprobe.dll ${DYNAMIC_FIXTURES}/tlsprobe.a
            COMMAND ${CMAKE_COMMAND} -E make_directory ${DYNAMIC_FIXTURES}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -shared -fno-stack-protector
                -DRAVEN_TLS_DLL -DRAVEN_TLS_DYNAMIC -Wl,--entry,DllMain,--image-base,0x190000000
                -Wl,--out-implib,${DYNAMIC_FIXTURES}/tlsprobe.a
                -o ${DYNAMIC_FIXTURES}/tlsprobe.dll ${LOADER_SOURCES}/tls_client.c -lkernel32
            DEPENDS ${LOADER_SOURCES}/tls_client.c VERBATIM)
        foreach(DYNAMIC_DLL parent reject fault loop detach_fault detach_loop)
            set(DYNAMIC_OPTIONS)
            if(DYNAMIC_DLL STREQUAL reject)
                set(DYNAMIC_OPTIONS -DRAVEN_DYNAMIC_REJECT)
            elseif(DYNAMIC_DLL STREQUAL fault)
                set(DYNAMIC_OPTIONS -DRAVEN_DYNAMIC_FAULT)
            elseif(DYNAMIC_DLL STREQUAL loop)
                set(DYNAMIC_OPTIONS -DRAVEN_DYNAMIC_LOOP)
            elseif(DYNAMIC_DLL STREQUAL detach_fault)
                set(DYNAMIC_OPTIONS -DRAVEN_DYNAMIC_DETACH_FAULT)
            elseif(DYNAMIC_DLL STREQUAL detach_loop)
                set(DYNAMIC_OPTIONS -DRAVEN_DYNAMIC_DETACH_LOOP)
            endif()
            add_custom_command(OUTPUT ${DYNAMIC_FIXTURES}/${DYNAMIC_DLL}.dll
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -shared -fno-stack-protector ${DYNAMIC_OPTIONS}
                    -Wl,--entry,DllMain,--image-base,0x190000000
                    -o ${DYNAMIC_FIXTURES}/${DYNAMIC_DLL}.dll ${LOADER_SOURCES}/dynamic_parent.c
                    ${DYNAMIC_FIXTURES}/tlsprobe.a -lkernel32
                DEPENDS ${LOADER_SOURCES}/dynamic_parent.c ${DYNAMIC_FIXTURES}/tlsprobe.a VERBATIM)
        endforeach()
        add_custom_command(OUTPUT ${DYNAMIC_FIXTURES}/client.exe
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x192000000
                -o ${DYNAMIC_FIXTURES}/client.exe ${LOADER_SOURCES}/dynamic_client.c -lkernel32
            DEPENDS ${LOADER_SOURCES}/dynamic_client.c VERBATIM)
        add_custom_target(dynamic_pe_fixtures ALL DEPENDS ${DYNAMIC_FIXTURES}/client.exe
            ${DYNAMIC_FIXTURES}/tlsprobe.dll ${DYNAMIC_FIXTURES}/parent.dll ${DYNAMIC_FIXTURES}/reject.dll
            ${DYNAMIC_FIXTURES}/fault.dll ${DYNAMIC_FIXTURES}/loop.dll ${DYNAMIC_FIXTURES}/failure.exe
            ${DYNAMIC_FIXTURES}/detach_fault.dll ${DYNAMIC_FIXTURES}/detach_loop.dll)
        add_custom_command(OUTPUT ${DYNAMIC_FIXTURES}/failure.exe
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x192000000
                -o ${DYNAMIC_FIXTURES}/failure.exe ${LOADER_SOURCES}/dynamic_failure.c -lkernel32
            DEPENDS ${LOADER_SOURCES}/dynamic_failure.c VERBATIM)
        add_executable(dynamic_loader_test nt/dynamic_loader_test.cpp)
        target_link_libraries(dynamic_loader_test PRIVATE raven_cpu)
        add_dependencies(dynamic_loader_test dynamic_pe_fixtures raven-bootstrap)
        add_test(NAME native_dynamic_loader_contract COMMAND dynamic_loader_test $<TARGET_FILE:raven-bootstrap> ${DYNAMIC_FIXTURES})
        set_tests_properties(native_dynamic_loader_contract PROPERTIES TIMEOUT 15)
        add_test(NAME native_dynamic_loader_cli COMMAND raven --run-process --execution=native ${DYNAMIC_FIXTURES}/client.exe)
        set_tests_properties(native_dynamic_loader_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        # An installer puts a redistributable library in the system directory and copies over system libraries.
        set(REDIST_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/redist-fixtures)
        add_custom_command(OUTPUT ${REDIST_FIXTURES}/payload/ravenredist.dll ${REDIST_FIXTURES}/ravenredist.a
            COMMAND ${CMAKE_COMMAND} -E make_directory ${REDIST_FIXTURES}/payload
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -shared -fno-stack-protector -Wl,--entry,DllMain
                -Wl,--out-implib,${REDIST_FIXTURES}/ravenredist.a
                -o ${REDIST_FIXTURES}/payload/ravenredist.dll ${LOADER_SOURCES}/redist_payload.c
            DEPENDS ${LOADER_SOURCES}/redist_payload.c VERBATIM)
        add_custom_command(OUTPUT ${REDIST_FIXTURES}/redist-importer.exe
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -Wl,--entry,raven_entry
                -o ${REDIST_FIXTURES}/redist-importer.exe ${LOADER_SOURCES}/redist_importer.c ${REDIST_FIXTURES}/ravenredist.a
            DEPENDS ${LOADER_SOURCES}/redist_importer.c ${REDIST_FIXTURES}/ravenredist.a VERBATIM)
        add_custom_command(OUTPUT ${REDIST_FIXTURES}/redist-install.exe
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -fno-builtin -Wl,--entry,raven_entry
                -o ${REDIST_FIXTURES}/redist-install.exe ${LOADER_SOURCES}/redist_install_client.c -lkernel32
            DEPENDS ${LOADER_SOURCES}/redist_install_client.c VERBATIM)
        add_custom_target(redist_fixtures ALL DEPENDS ${REDIST_FIXTURES}/payload/ravenredist.dll ${REDIST_FIXTURES}/redist-importer.exe
            ${REDIST_FIXTURES}/redist-install.exe)
        add_test(NAME native_redist_system_directory COMMAND raven --run-process --execution=native ${REDIST_FIXTURES}/redist-install.exe)
        set_tests_properties(native_redist_system_directory PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
            ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/redist-prefix)
        set(TLS_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/tls-fixtures)
        set(TLS_FILES)
        foreach(TLS_VARIANT standard aligned collision)
            set(TLS_DLL_BASE 0x190000000)
            if(TLS_VARIANT STREQUAL standard)
                set(TLS_OUTPUT ${TLS_FIXTURES})
                set(TLS_OPTIONS)
            elseif(TLS_VARIANT STREQUAL aligned)
                set(TLS_OUTPUT ${TLS_FIXTURES}/aligned)
                set(TLS_OPTIONS -DRAVEN_TLS_ALIGNMENT=8192 -DRAVEN_TLS_ALIGN_FLAG=IMAGE_SCN_ALIGN_8192BYTES)
            else()
                set(TLS_OUTPUT ${TLS_FIXTURES}/collision)
                set(TLS_DLL_BASE 0x192000000)
                set(TLS_OPTIONS)
            endif()
            add_custom_command(OUTPUT ${TLS_OUTPUT}/tlsprobe.dll ${TLS_OUTPUT}/tlsprobe.a
                COMMAND ${CMAKE_COMMAND} -E make_directory ${TLS_OUTPUT}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -shared -fno-stack-protector -DRAVEN_TLS_DLL ${TLS_OPTIONS}
                    -Wl,--entry,DllMain,--image-base,${TLS_DLL_BASE} -Wl,--out-implib,${TLS_OUTPUT}/tlsprobe.a
                    -o ${TLS_OUTPUT}/tlsprobe.dll ${LOADER_SOURCES}/tls_client.c -lkernel32
                DEPENDS ${LOADER_SOURCES}/tls_client.c VERBATIM)
            add_custom_command(OUTPUT ${TLS_OUTPUT}/client.exe
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector ${TLS_OPTIONS}
                    -Wl,--entry,raven_entry,--image-base,0x192000000
                    -o ${TLS_OUTPUT}/client.exe ${LOADER_SOURCES}/tls_client.c ${TLS_OUTPUT}/tlsprobe.a -lkernel32
                DEPENDS ${LOADER_SOURCES}/tls_client.c ${TLS_OUTPUT}/tlsprobe.a VERBATIM)
            list(APPEND TLS_FILES ${TLS_OUTPUT}/client.exe ${TLS_OUTPUT}/tlsprobe.dll)
        endforeach()
        add_custom_target(tls_pe_fixtures DEPENDS ${TLS_FILES})
        add_executable(tls_test nt/tls_test.cpp)
        target_link_libraries(tls_test PRIVATE raven_cpu)
        add_dependencies(tls_test tls_pe_fixtures raven-bootstrap)
        add_test(NAME native_static_tls COMMAND tls_test $<TARGET_FILE:raven-bootstrap> ${TLS_FIXTURES} ${TLS_FIXTURES}/aligned ${TLS_FIXTURES}/collision)
        set_tests_properties(native_static_tls PROPERTIES TIMEOUT 20)
        add_test(NAME native_static_tls_cli COMMAND raven --run-process --execution=native
            ${TLS_FIXTURES}/client.exe ${TLS_FIXTURES}/tlsprobe.dll)
        set_tests_properties(native_static_tls_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        add_test(NAME native_collision_cli COMMAND raven --run-process --execution=native
            ${TLS_FIXTURES}/collision/client.exe ${TLS_FIXTURES}/collision/tlsprobe.dll)
        set_tests_properties(native_collision_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        if(RAVEN_MINGW32)
            set(TLS32_FIXTURES ${TLS_FIXTURES}/x86)
            add_custom_command(OUTPUT ${TLS32_FIXTURES}/tlsprobe.dll ${TLS32_FIXTURES}/tlsprobe.a
                COMMAND ${CMAKE_COMMAND} -E make_directory ${TLS32_FIXTURES}
                COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -shared -fno-stack-protector -DRAVEN_TLS_DLL
                    -Wl,--entry,_DllMain@12,--image-base,0x19000000 -Wl,--out-implib,${TLS32_FIXTURES}/tlsprobe.a
                    -o ${TLS32_FIXTURES}/tlsprobe.dll ${LOADER_SOURCES}/tls_client.c -lkernel32
                DEPENDS ${LOADER_SOURCES}/tls_client.c VERBATIM)
            add_custom_command(OUTPUT ${TLS32_FIXTURES}/z-client.exe
                COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,_raven_entry,--image-base,0x19200000
                    -o ${TLS32_FIXTURES}/z-client.exe ${LOADER_SOURCES}/tls_client.c ${TLS32_FIXTURES}/tlsprobe.a -lkernel32
                DEPENDS ${LOADER_SOURCES}/tls_client.c ${TLS32_FIXTURES}/tlsprobe.a VERBATIM)
            add_custom_target(tls32_pe_fixtures ALL DEPENDS ${TLS32_FIXTURES}/z-client.exe ${TLS32_FIXTURES}/tlsprobe.dll)
            add_test(NAME native_static_tls_x32 COMMAND raven --run-process --execution=native
                ${TLS32_FIXTURES}/z-client.exe ${TLS32_FIXTURES}/tlsprobe.dll)
            set_tests_properties(native_static_tls_x32 PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        endif()
        add_custom_command(OUTPUT ${MODULE_FIXTURES}/target.dll
            COMMAND ${CMAKE_COMMAND} -E make_directory ${MODULE_FIXTURES}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -shared -fno-stack-protector
                -Wl,--entry,0,--image-base,0x180000000 -o ${MODULE_FIXTURES}/target.dll
                ${LOADER_SOURCES}/target.c ${LOADER_SOURCES}/target.def
            DEPENDS ${LOADER_SOURCES}/target.c ${LOADER_SOURCES}/target.def VERBATIM)
        add_custom_command(OUTPUT ${MODULE_FIXTURES}/facade.dll ${MODULE_FIXTURES}/facade.a
            COMMAND ${CMAKE_COMMAND} -E make_directory ${MODULE_FIXTURES}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -shared -fno-stack-protector
                -Wl,--entry,0,--image-base,0x180100000 -Wl,--out-implib,${MODULE_FIXTURES}/facade.a
                -o ${MODULE_FIXTURES}/facade.dll ${LOADER_SOURCES}/target.c ${LOADER_SOURCES}/facade.def
            DEPENDS ${LOADER_SOURCES}/target.c ${LOADER_SOURCES}/facade.def VERBATIM)
        add_custom_command(OUTPUT ${MODULE_FIXTURES}/client.exe
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x188000000
                -o ${MODULE_FIXTURES}/client.exe ${CALLBACK_SOURCES}/module_api_client.c
                ${MODULE_FIXTURES}/facade.a -lkernel32
            DEPENDS ${CALLBACK_SOURCES}/module_api_client.c ${MODULE_FIXTURES}/facade.a VERBATIM)
        add_custom_target(module_api_pe_fixtures ALL DEPENDS ${MODULE_FIXTURES}/client.exe
            ${MODULE_FIXTURES}/facade.dll ${MODULE_FIXTURES}/target.dll)
        add_test(NAME native_module_api_cli COMMAND raven --run-process --execution=native
            ${MODULE_FIXTURES}/client.exe ${MODULE_FIXTURES}/facade.dll ${MODULE_FIXTURES}/target.dll)
        set_tests_properties(native_module_api_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        # A library loaded at run time whose exports forward to one that is not loaded yet.
        add_custom_command(OUTPUT ${MODULE_FIXTURES}/late_target.dll ${MODULE_FIXTURES}/late_facade.dll ${MODULE_FIXTURES}/late-forwarder.exe
            COMMAND ${CMAKE_COMMAND} -E make_directory ${MODULE_FIXTURES}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -shared -fno-stack-protector
                -Wl,--entry,0,--image-base,0x180200000 -o ${MODULE_FIXTURES}/late_target.dll
                ${LOADER_SOURCES}/target.c ${LOADER_SOURCES}/late_target.def
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -shared -fno-stack-protector
                -Wl,--entry,0,--image-base,0x180300000 -o ${MODULE_FIXTURES}/late_facade.dll
                ${LOADER_SOURCES}/target.c ${LOADER_SOURCES}/late_facade.def
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x188100000
                -o ${MODULE_FIXTURES}/late-forwarder.exe ${CALLBACK_SOURCES}/late_forwarder_client.c -lkernel32
            DEPENDS ${LOADER_SOURCES}/target.c ${LOADER_SOURCES}/late_target.def ${LOADER_SOURCES}/late_facade.def
                ${CALLBACK_SOURCES}/late_forwarder_client.c VERBATIM)
        add_custom_target(late_forwarder_fixtures ALL DEPENDS ${MODULE_FIXTURES}/late-forwarder.exe)
        add_test(NAME native_late_forwarder_cli COMMAND raven --run-process --execution=native ${MODULE_FIXTURES}/late-forwarder.exe)
        set_tests_properties(native_late_forwarder_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")

        set(LIFECYCLE_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/lifecycle-fixtures)
        add_custom_command(OUTPUT ${LIFECYCLE_FIXTURES}/base.dll ${LIFECYCLE_FIXTURES}/base.a
            COMMAND ${CMAKE_COMMAND} -E make_directory ${LIFECYCLE_FIXTURES}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -shared -fno-stack-protector
                -Wl,--entry,DllMain,--image-base,0x18a000000 -Wl,--out-implib,${LIFECYCLE_FIXTURES}/base.a
                -o ${LIFECYCLE_FIXTURES}/base.dll ${LOADER_SOURCES}/lifecycle_base.c -lkernel32
            DEPENDS ${LOADER_SOURCES}/lifecycle_base.c VERBATIM)
        add_custom_command(OUTPUT ${LIFECYCLE_FIXTURES}/dependent.dll ${LIFECYCLE_FIXTURES}/dependent.a
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -shared -fno-stack-protector
                -Wl,--entry,DllMain,--image-base,0x18b000000 -Wl,--out-implib,${LIFECYCLE_FIXTURES}/dependent.a
                -o ${LIFECYCLE_FIXTURES}/dependent.dll ${LOADER_SOURCES}/lifecycle_dependent.c
                ${LIFECYCLE_FIXTURES}/base.a -lkernel32
            DEPENDS ${LOADER_SOURCES}/lifecycle_dependent.c ${LIFECYCLE_FIXTURES}/base.a VERBATIM)
        add_custom_command(OUTPUT ${LIFECYCLE_FIXTURES}/client.exe
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x18d000000
                -o ${LIFECYCLE_FIXTURES}/client.exe ${LOADER_SOURCES}/lifecycle_client.c
                ${LIFECYCLE_FIXTURES}/base.a ${LIFECYCLE_FIXTURES}/dependent.a -lkernel32
            DEPENDS ${LOADER_SOURCES}/lifecycle_client.c ${LIFECYCLE_FIXTURES}/base.a
                ${LIFECYCLE_FIXTURES}/dependent.a VERBATIM)
        set(REJECTION_FILES)
        foreach(REJECTION reject fault loop)
            if(REJECTION STREQUAL reject)
                set(REJECTION_MODE 1)
            elseif(REJECTION STREQUAL fault)
                set(REJECTION_MODE 2)
            else()
                set(REJECTION_MODE 3)
            endif()
            add_custom_command(OUTPUT ${LIFECYCLE_FIXTURES}/${REJECTION}.dll
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -shared -fno-stack-protector
                    -DRAVEN_LOADER_MODE=${REJECTION_MODE} -Wl,--entry,DllMain,--image-base,0x18c000000
                    -o ${LIFECYCLE_FIXTURES}/${REJECTION}.dll ${LOADER_SOURCES}/lifecycle_reject.c ${LIFECYCLE_FIXTURES}/base.a
                DEPENDS ${LOADER_SOURCES}/lifecycle_reject.c ${LIFECYCLE_FIXTURES}/base.a VERBATIM)
            list(APPEND REJECTION_FILES ${LIFECYCLE_FIXTURES}/${REJECTION}.dll)
        endforeach()
        add_custom_target(lifecycle_pe_fixtures DEPENDS ${LIFECYCLE_FIXTURES}/client.exe
            ${LIFECYCLE_FIXTURES}/base.dll ${LIFECYCLE_FIXTURES}/dependent.dll ${REJECTION_FILES})
        add_executable(loader_test nt/loader_test.cpp)
        target_link_libraries(loader_test PRIVATE raven_cpu)
        add_dependencies(loader_test lifecycle_pe_fixtures raven-bootstrap)
        add_test(NAME native_dll_lifecycle COMMAND loader_test $<TARGET_FILE:raven-bootstrap> ${LIFECYCLE_FIXTURES})
        set_tests_properties(native_dll_lifecycle PROPERTIES TIMEOUT 15)
        add_test(NAME native_dll_cli COMMAND raven --run-process --execution=native
            ${LIFECYCLE_FIXTURES}/client.exe ${LIFECYCLE_FIXTURES}/dependent.dll ${LIFECYCLE_FIXTURES}/base.dll)
        set_tests_properties(native_dll_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        add_custom_command(OUTPUT ${LIFECYCLE_FIXTURES}/forward.dll ${LIFECYCLE_FIXTURES}/forward.a ${LIFECYCLE_FIXTURES}/forward-client.exe
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -shared -fno-stack-protector
                -Wl,--entry,0,--image-base,0x1c7000000,--no-insert-timestamp
                -Wl,--out-implib,${LIFECYCLE_FIXTURES}/forward.a
                -o ${LIFECYCLE_FIXTURES}/forward.dll ${LOADER_SOURCES}/startup_forward.c ${LOADER_SOURCES}/startup_forward.def
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x1c8000000,--no-insert-timestamp
                -o ${LIFECYCLE_FIXTURES}/forward-client.exe ${LOADER_SOURCES}/startup_forward_client.c ${LIFECYCLE_FIXTURES}/forward.a
            DEPENDS ${LOADER_SOURCES}/startup_forward.c ${LOADER_SOURCES}/startup_forward.def
                ${LOADER_SOURCES}/startup_forward_client.c VERBATIM)
        add_custom_target(startup_forward_fixture ALL DEPENDS ${LIFECYCLE_FIXTURES}/forward.dll ${LIFECYCLE_FIXTURES}/forward-client.exe)
        add_executable(dependencies_test nt/dependencies_test.cpp)
        target_link_libraries(dependencies_test PRIVATE raven_cpu)
        add_dependencies(dependencies_test lifecycle_pe_fixtures startup_forward_fixture raven-bootstrap)
        if(TARGET resource_pe32_fixture)
            add_dependencies(dependencies_test resource_pe32_fixture)
            add_test(NAME native_initial_dependency_contract COMMAND dependencies_test $<TARGET_FILE:raven-bootstrap>
                ${LIFECYCLE_FIXTURES} ${RESOURCE_FIXTURES}/client32.exe)
        else()
            add_test(NAME native_initial_dependency_contract COMMAND dependencies_test $<TARGET_FILE:raven-bootstrap> ${LIFECYCLE_FIXTURES})
        endif()
        add_test(NAME native_initial_dependency_cli COMMAND raven --run-process --execution=native ${LIFECYCLE_FIXTURES}/client.exe)
        add_test(NAME native_initial_forwarder_cli COMMAND raven --run-process --execution=native ${LIFECYCLE_FIXTURES}/forward-client.exe)
        add_test(NAME native_initial_dependency_prefer_local COMMAND dependencies_test $<TARGET_FILE:raven-bootstrap> ${LIFECYCLE_FIXTURES})
        set_tests_properties(native_initial_dependency_contract PROPERTIES TIMEOUT 15)
        set_tests_properties(native_initial_dependency_prefer_local PROPERTIES TIMEOUT 15 ENVIRONMENT RAVEN_PREFER_LOCAL_DLLS=1)
        set_tests_properties(native_initial_dependency_cli native_initial_forwarder_cli PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
    endif()
endif()

# A test that names no prefix gets one of its own in the build tree, so it neither reads the RAVEN_PREFIX of the shell that runs
# ctest nor writes into the user's default prefix.
get_property(native_tests DIRECTORY PROPERTY TESTS)
foreach(native_test IN LISTS native_tests)
    get_test_property(${native_test} ENVIRONMENT native_test_environment)
    if(NOT "${native_test_environment}" MATCHES "(^|;)RAVEN_PREFIX=")
        set_property(TEST ${native_test} APPEND PROPERTY ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/test-prefixes/${native_test}")
    endif()
endforeach()
