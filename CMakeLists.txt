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

# ---- live data: S7CommPlusDriver (third_party/s7commplus, LGPL-3.0) --------
# Separate assemblies loaded at run time, plus the OpenSSL 3 DLLs. The driver
# is C# 7.3: it needs the Roslyn csc of Visual Studio / Build Tools.
option(TC_LIVE_DATA "Build the live-data driver (S7CommPlusDriver + OpenSSL 3)" ON)
set(S7P_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/s7commplus/src)
set(TC_OPENSSL_DIR "${S7P_DIR}/S7CommPlusDriver/OpenSSL-dll-x64" CACHE PATH
    "Folder with libcrypto-3-x64.dll and libssl-3-x64.dll (default: the copy in the driver repository)")
set(LIVE_FILES "")
if(TC_LIVE_DATA)
    if(NOT EXISTS ${S7P_DIR}/S7CommPlusDriver/S7CommPlusConnection.cs)
        message(FATAL_ERROR "third_party/s7commplus is missing: run 'git submodule update --init' "
                            "(or configure with -DTC_LIVE_DATA=OFF)")
    endif()
    if(NOT ROSLYN_CSC)
        set(_pf86 "ProgramFiles(x86)")
        find_program(VSWHERE_EXE vswhere.exe PATHS "$ENV{${_pf86}}/Microsoft Visual Studio/Installer" NO_DEFAULT_PATH)
        if(VSWHERE_EXE)
            execute_process(COMMAND ${VSWHERE_EXE} -latest -products * -requires Microsoft.Component.MSBuild
                                    -find "MSBuild\\**\\Bin\\Roslyn\\csc.exe"
                            OUTPUT_VARIABLE _roslyn OUTPUT_STRIP_TRAILING_WHITESPACE)
            string(REGEX REPLACE "[\r\n].*" "" _roslyn "${_roslyn}")
        endif()
        if(_roslyn AND EXISTS "${_roslyn}")
            set(ROSLYN_CSC "${_roslyn}" CACHE FILEPATH "Roslyn csc.exe (C# 7.3) for the live-data driver")
        else()
            message(FATAL_ERROR "Roslyn csc.exe not found (Visual Studio or Build Tools with MSBuild): pass "
                                "-DROSLYN_CSC=<path to csc.exe> or configure with -DTC_LIVE_DATA=OFF")
        endif()
    endif()
    foreach(dll libcrypto-3-x64.dll libssl-3-x64.dll)
        if(NOT EXISTS ${TC_OPENSSL_DIR}/${dll})
            message(FATAL_ERROR "${dll} not found in TC_OPENSSL_DIR (${TC_OPENSSL_DIR})")
        endif()
    endforeach()

    file(GLOB ZLIB_SRC CONFIGURE_DEPENDS ${S7P_DIR}/Zlib.net/*.cs)
    file(GLOB_RECURSE S7P_SRC CONFIGURE_DEPENDS ${S7P_DIR}/S7CommPlusDriver/*.cs)
    set(S7P_OUT ${CMAKE_CURRENT_BINARY_DIR}/s7commplus)
    set(ZLIB_DLL ${S7P_OUT}/zlib.net.dll)
    set(S7P_DLL ${S7P_OUT}/S7CommPlusDriver.dll)
    set(_zlib_native "")
    foreach(f ${ZLIB_SRC})
        file(TO_NATIVE_PATH "${f}" n)
        list(APPEND _zlib_native "${n}")
    endforeach()
    set(_s7p_native "")
    foreach(f ${S7P_SRC})
        file(TO_NATIVE_PATH "${f}" n)
        list(APPEND _s7p_native "${n}")
    endforeach()
    file(TO_NATIVE_PATH "${ZLIB_DLL}" ZLIB_DLL_NATIVE)
    file(TO_NATIVE_PATH "${S7P_DLL}" S7P_DLL_NATIVE)
    set(_csc_opts -nologo -target:library -optimize+ -langversion:7.3 -platform:x64 -deterministic -nowarn:1668)
    add_custom_command(
        OUTPUT ${ZLIB_DLL} ${S7P_DLL}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${S7P_OUT}
        COMMAND ${CMAKE_COMMAND} -E env LIB= ${ROSLYN_CSC} ${_csc_opts} -out:${ZLIB_DLL_NATIVE} ${_zlib_native}
        COMMAND ${CMAKE_COMMAND} -E env LIB= ${ROSLYN_CSC} ${_csc_opts} -define:TRACE -define:_WIN64 -r:${ZLIB_DLL_NATIVE} -r:System.Net.Http.dll
                -out:${S7P_DLL_NATIVE} ${_s7p_native}
        DEPENDS ${ZLIB_SRC} ${S7P_SRC}
        COMMENT "Compiling S7CommPlusDriver.dll and zlib.net.dll (live data) with Roslyn csc"
        VERBATIM)
    add_custom_target(s7commplus ALL DEPENDS ${ZLIB_DLL} ${S7P_DLL})
    set(LIVE_FILES ${S7P_DLL} ${ZLIB_DLL} ${TC_OPENSSL_DIR}/libcrypto-3-x64.dll ${TC_OPENSSL_DIR}/libssl-3-x64.dll)
endif()

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
    src/tia/live.c
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
    src/tools/t_live_data.c
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
target_link_libraries(tccore PRIVATE cjson mxml ole32 oleaut32 advapi32 shell32 shlwapi credui version)
if(MSVC)
    target_compile_options(tccore PRIVATE /W4 /wd4100)
    target_compile_definitions(tccore PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()
add_dependencies(tccore bridge)
if(TC_LIVE_DATA)
    add_dependencies(tccore s7commplus)
endif()
add_custom_command(TARGET tccore POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BRIDGE_DLL} ${LIVE_FILES} $<TARGET_FILE_DIR:tccore>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/NOTICE
            ${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md $<TARGET_FILE_DIR:tccore>
    VERBATIM)
if(TC_LIVE_DATA)
    # LGPL-3.0 section 4: the GPL and LGPL texts accompany the binaries.
    add_custom_command(TARGET tccore POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${S7P_DIR}/../LICENSE
                $<TARGET_FILE_DIR:tccore>/S7CommPlusDriver.LICENSE.txt
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/third_party/licenses/GPL-3.0.txt
                $<TARGET_FILE_DIR:tccore>/GPL-3.0.txt
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${S7P_DIR}/Zlib.net/LICENSE
                $<TARGET_FILE_DIR:tccore>/zlib.net.LICENSE.txt
        VERBATIM)
endif()

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
