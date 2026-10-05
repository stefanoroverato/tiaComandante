cmake_minimum_required(VERSION 3.20)

project(tiacomandante VERSION 0.1.0 LANGUAGES C)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "tiaComandante targets Windows x64 only (TIA Portal Openness is 64-bit)")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# Static CRT: no Visual C++ redistributable needed on the target PC.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    add_compile_options(/utf-8)
endif()

# ---- third-party libraries: git submodules (see third_party/README.md) --------
foreach(dep cjson/cJSON.c mxml/mxml-file.c)
    if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/${dep})
        message(FATAL_ERROR "third_party/${dep} is missing: run 'git submodule update --init' "
                            "(or clone with --recurse-submodules)")
    endif()
endforeach()

add_library(cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cjson)
if(MSVC)
    target_compile_definitions(cjson PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_library(mxml STATIC
    third_party/mxml/mxml-attr.c
    third_party/mxml/mxml-file.c
    third_party/mxml/mxml-get.c
    third_party/mxml/mxml-index.c
    third_party/mxml/mxml-node.c
    third_party/mxml/mxml-options.c
    third_party/mxml/mxml-private.c
    third_party/mxml/mxml-search.c
    third_party/mxml/mxml-set.c)
target_include_directories(mxml PUBLIC third_party/mxml PRIVATE third_party/mxml/vcnet)
if(MSVC)
    target_compile_options(mxml PRIVATE /W2)
endif()

# ---- .NET bridge, built with the csc.exe shipped with .NET Framework 4.8 ----
find_program(CSC_EXE csc.exe
    PATHS "$ENV{WINDIR}/Microsoft.NET/Framework64/v4.0.30319"
    NO_DEFAULT_PATH REQUIRED)
set(BRIDGE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/bridge/Bridge.cs)
set(BRIDGE_DLL ${CMAKE_CURRENT_BINARY_DIR}/bridge/TiaComandante.Bridge.dll)
file(TO_NATIVE_PATH "${BRIDGE_SRC}" BRIDGE_SRC_NATIVE)
file(TO_NATIVE_PATH "${BRIDGE_DLL}" BRIDGE_DLL_NATIVE)
add_custom_command(
    OUTPUT ${BRIDGE_DLL}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/bridge
    COMMAND ${CSC_EXE} -nologo -target:library -optimize+ -warnaserror+ -nowarn:1668
            -r:System.Core.dll -r:System.Web.Extensions.dll
            -out:${BRIDGE_DLL_NATIVE} ${BRIDGE_SRC_NATIVE}
    DEPENDS ${BRIDGE_SRC}
    COMMENT "Compiling TiaComandante.Bridge.dll with csc (.NET Framework 4.8)"
    VERBATIM)
add_custom_target(bridge ALL DEPENDS ${BRIDGE_DLL})

# ---- server --------------------------------------------------------------
configure_file(src/tc_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/tc_version.h @ONLY)

add_library(tccore SHARED
    src/core_main.c
    src/selftest.c
    src/app/config.c
    src/app/credentials.c
    src/app/devcatalog.c
    src/app/export_store.c
    src/app/stats.c
    src/clr/clrhost.c
    src/mcp/registry.c
    src/mcp/server.c
    src/mcp/tool.c
    src/tia/blockgen.c
    src/tia/members.c
    src/tia/online.c
    src/tia/session.c
    src/tia/tia_dyn.c
    src/tia/tia_env.c
    src/tia/tia_nav.c
    src/tia/tia_sw.c
    src/tia/simaticml.c
    src/tia/tagaddr.c
    src/tia/xlsx.c
    src/tools/tools_common.c
    src/tools/t_admin.c
    src/tools/t_alarm_text.c
    src/tools/t_blocks_read.c
    src/tools/t_blocks_write.c
    src/tools/t_db.c
    src/tools/t_dev.c
    src/tools/t_diagnostics.c
    src/tools/t_download_upload.c
    src/tools/t_folders.c
    src/tools/t_get_info.c
    src/tools/t_hardware.c
    src/tools/t_library.c
    src/tools/t_session.c
    src/tools/t_tag.c
    src/tools/t_techobj.c
    src/tools/t_udt.c
    src/tools/t_watch.c
    src/tools/t_xref.c
    src/util/fmt.c
    src/util/fs.c
    src/util/glob.c
    src/util/log.c
    src/util/strbuf.c
    src/util/utf.c)
target_include_directories(tccore PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated)
set_target_properties(tccore PROPERTIES OUTPUT_NAME tiacomandante-core)
target_link_libraries(tccore PRIVATE cjson mxml ole32 oleaut32 advapi32 shell32 shlwapi credui)
if(MSVC)
    target_compile_options(tccore PRIVATE /W4 /wd4100)
    target_compile_definitions(tccore PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()
add_dependencies(tccore bridge)
add_custom_command(TARGET tccore POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BRIDGE_DLL} $<TARGET_FILE_DIR:tccore>
    VERBATIM)

# Stable launcher: TIA Portal's Openness AllowList hashes the executable, so all
# logic lives in tiacomandante-core.dll and this exe should almost never change.
add_executable(tiacomandante src/launcher/launcher.c)
if(MSVC)
    target_compile_options(tiacomandante PRIVATE /W4 /Brepro)
    target_link_options(tiacomandante PRIVATE /Brepro)
    target_compile_definitions(tiacomandante PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
endif()
add_dependencies(tiacomandante tccore)

# ---- tests (ctest -C Release) -----------------------------------------------
enable_testing()
add_executable(test_mcp tests/test_mcp.c)
target_link_libraries(test_mcp PRIVATE cjson)
if(MSVC)
    target_compile_definitions(test_mcp PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
endif()
add_dependencies(test_mcp tiacomandante)
add_test(NAME mcp_protocol COMMAND test_mcp $<TARGET_FILE:tiacomandante>)
