# Copyright (C) 2025 Signal Slot Inc.
# Copyright (C) 2026 Alexandros Theodotou <alex@zrythm.org>
# SPDX-License-Identifier: LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

cmake_minimum_required(VERSION 3.16.0)

include(.cmake.conf)
project(QtMcp
    VERSION "${QT_REPO_MODULE_VERSION}"
    DESCRIPTION "Qt Mcp Libraries"
    HOMEPAGE_URL "https://github.com/signal-slot/qtmcp"
    LANGUAGES CXX C
)

# Qt installations that ship Qt6BuildInternals (Qt's own installers, a
# build-tree install) use Qt's module build system below. Package managers
# that install Qt through their own packaging (Conan) do not ship it; for
# those the else() branch compiles the same sources with plain CMake and
# installs an ordinary, relocatable CMake package.
find_package(Qt6 6.8 CONFIG QUIET COMPONENTS BuildInternals)

if(Qt6BuildInternals_FOUND)

find_package(Qt6 6.8 CONFIG REQUIRED COMPONENTS
    Core
)
find_package(Qt6 6.8 CONFIG OPTIONAL_COMPONENTS
    Gui
    Widgets
    Network
)

qt_internal_project_setup()

qt_build_repo()

# Qt MCP is an independent library that happens to use Qt's module build
# system; offer the modules under the library's own namespace as well
# (QtMcp::Common/Client/Server) so consumers do not have to reach into Qt6::.
include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/QtMcpConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cmake/QtMcpConfig.cmake
    INSTALL_DESTINATION ${INSTALL_LIBDIR}/cmake/QtMcp
)
# The QtMcp package carries the MCP-revision-based release version, not the
# Qt-shaped module version. API only grows across revisions, hence
# AnyNewerVersion.
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/cmake/QtMcpConfigVersion.cmake
    VERSION ${QTMCP_VERSION}
    COMPATIBILITY AnyNewerVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/cmake/QtMcpConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/cmake/QtMcpConfigVersion.cmake
    DESTINATION ${INSTALL_LIBDIR}/cmake/QtMcp
)

else()

# ---------------------------------------------------------------------------
# Plain CMake build (no QtBuildInternals). Builds the server-side modules
# only: McpCommon, McpServer and the Streamable HTTP backend, as ordinary
# libraries plus a find_package(QtMcp) package.
# ---------------------------------------------------------------------------

find_package(Qt6 6.8 CONFIG REQUIRED COMPONENTS
    Core
    Network
)
find_package(Qt6 6.8 CONFIG QUIET COMPONENTS
    CorePrivate
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(QTMCP_BUILD_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/build-include")
# Headers staged for installation: real files under their module directory,
# each under both the CamelCase class name and the original file name.
set(QTMCP_STAGE_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/stage-include")

# Header-name segments whose class names are not a mechanical CamelCase of
# the file name.
function(qtmcp_camelize filename out_var)
    get_filename_component(base "${filename}" NAME_WE)
    string(REPLACE "_" ";" segs "${base}")
    set(result "")
    foreach(seg IN LISTS segs)
        if(seg STREQUAL "qtmcp")
            set(cap "QtMcp")
        elseif(seg STREQUAL "qmcp")
            set(cap "QMcp")
        elseif(seg STREQUAL "jsonrpc")
            set(cap "JSONRPC")
        elseif(seg STREQUAL "sse")
            set(cap "SSE")
        elseif(seg STREQUAL "http")
            set(cap "HTTP")
        else()
            string(SUBSTRING "${seg}" 0 1 first)
            string(SUBSTRING "${seg}" 1 -1 rest)
            string(TOUPPER "${first}" first)
            set(cap "${first}${rest}")
        endif()
        string(APPEND result "${cap}")
    endforeach()
    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

# Exports header that qt_internal_add_module would have generated.
function(qtmcp_write_exports_header dir ns module)
    set(guard "QT${module}EXPORTS_H")
    string(TOUPPER "${guard}" guard)
    string(TOLOWER "qt${ns}exports.h" fname)
    set(content "
#ifndef ${guard}
#define ${guard}

#include <QtCore/qglobal.h>

#if defined(QT_BUILD_${module}_LIB)
#  define Q_${module}_EXPORT Q_DECL_EXPORT
#else
#  define Q_${module}_EXPORT Q_DECL_IMPORT
#endif

#endif
")
    file(WRITE "${dir}/Qt${ns}/${fname}" "${content}")
endfunction()

# For every header in srcdir: a forwarding header under the build include
# dir (both the CamelCase class name and the original file name), a real
# copy under the staging dir (same two names), and one umbrella header per
# module. The forwarding headers are never installed; the staged copies are.
function(qtmcp_generate_module_includes ns module srcdir)
    file(GLOB headers "${srcdir}/*.h")
    set(umbrella "")
    foreach(h IN LISTS headers)
        file(READ "${h}" content)
        set(cls "")
        string(REGEX MATCH "class[ \t]+Q_[A-Z_]+_EXPORT[ \t]+([A-Za-z0-9_]+)" m "${content}")
        if(m)
            set(cls "${CMAKE_MATCH_1}")
        endif()
        get_filename_component(fname "${h}" NAME)
        if(cls)
            set(names "${cls};${fname}")
        else()
            qtmcp_camelize("${fname}" cam)
            set(names "${cam};${fname}")
        endif()
        foreach(n IN LISTS names)
            file(WRITE "${QTMCP_BUILD_INCLUDE_DIR}/Qt${ns}/${n}" "#include \"${h}\"\n")
            if(NOT n MATCHES "\\.")
                string(APPEND umbrella "#include <Qt${ns}/${n}>\n")
            endif()
        endforeach()
        file(COPY "${h}" DESTINATION "${QTMCP_STAGE_INCLUDE_DIR}/Qt${ns}")
        if(cls AND NOT cls STREQUAL fname)
            file(COPY "${h}" DESTINATION "${QTMCP_STAGE_INCLUDE_DIR}/Qt${ns}-tmp")
            file(RENAME "${QTMCP_STAGE_INCLUDE_DIR}/Qt${ns}-tmp/${fname}"
                        "${QTMCP_STAGE_INCLUDE_DIR}/Qt${ns}/${cls}")
        endif()
    endforeach()
    file(WRITE "${QTMCP_BUILD_INCLUDE_DIR}/Qt${ns}/Qt${ns}" "${umbrella}")
    file(WRITE "${QTMCP_STAGE_INCLUDE_DIR}/Qt${ns}/Qt${ns}" "${umbrella}")
    qtmcp_write_exports_header("${QTMCP_BUILD_INCLUDE_DIR}" "${ns}" "${module}")
    qtmcp_write_exports_header("${QTMCP_STAGE_INCLUDE_DIR}" "${ns}" "${module}")
    file(REMOVE_RECURSE "${QTMCP_STAGE_INCLUDE_DIR}/Qt${ns}-tmp")
endfunction()

qtmcp_generate_module_includes(McpCommon MCPCOMMON "${CMAKE_CURRENT_SOURCE_DIR}/src/mcpcommon")
qtmcp_generate_module_includes(McpServer MCPSERVER "${CMAKE_CURRENT_SOURCE_DIR}/src/mcpserver")
# The Streamable HTTP backend headers install next to QtMcpServer's; the
# backend class is instantiated directly by embedders.
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/mcpserverbackend/streamablehttp/qmcpserverstreamablehttp.h"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/mcpserverbackend/streamablehttp/httpserver.h"
     DESTINATION "${QTMCP_STAGE_INCLUDE_DIR}/QtMcpServer")

file(GLOB MCPCOMMON_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/mcpcommon/*.cpp")
file(GLOB MCPCOMMON_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/src/mcpcommon/*.h")
add_library(McpCommon ${MCPCOMMON_SOURCES} ${MCPCOMMON_HEADERS})
set_target_properties(McpCommon PROPERTIES
    AUTOMOC ON
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME Common)
target_compile_definitions(McpCommon PRIVATE
    QT_BUILD_MCPCOMMON_LIB QT_NO_CONTEXTLESS_CONNECT)
target_include_directories(McpCommon PUBLIC
    "$<BUILD_INTERFACE:${QTMCP_BUILD_INCLUDE_DIR}>"
    "$<BUILD_INTERFACE:${QTMCP_BUILD_INCLUDE_DIR}/QtMcpCommon>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/QtMcpCommon>")
target_link_libraries(McpCommon PUBLIC Qt6::Core)

file(GLOB MCPSERVER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/mcpserver/*.cpp")
file(GLOB MCPSERVER_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/src/mcpserver/*.h")
add_library(McpServer ${MCPSERVER_SOURCES} ${MCPSERVER_HEADERS})
set_target_properties(McpServer PROPERTIES
    AUTOMOC ON
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME Server)
target_compile_definitions(McpServer PRIVATE
    QT_BUILD_MCPSERVER_LIB QT_NO_CONTEXTLESS_CONNECT)
target_include_directories(McpServer PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/mcpserver>"
    "$<BUILD_INTERFACE:${QTMCP_BUILD_INCLUDE_DIR}>"
    "$<BUILD_INTERFACE:${QTMCP_BUILD_INCLUDE_DIR}/QtMcpServer>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/QtMcpServer>")
target_link_libraries(McpServer PUBLIC McpCommon Qt6::Core Qt6::Network)
if(TARGET Qt6::CorePrivate)
    target_link_libraries(McpServer PRIVATE Qt6::CorePrivate)
endif()

set(QTMCP_SHI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/mcpserverbackend/streamablehttp")
add_library(McpServerStreamableHttp
    "${QTMCP_SHI_DIR}/qmcpserverstreamablehttp.h"
    "${QTMCP_SHI_DIR}/qmcpserverstreamablehttp.cpp"
    "${QTMCP_SHI_DIR}/httpserver.h"
    "${QTMCP_SHI_DIR}/httpserver.cpp")
set_target_properties(McpServerStreamableHttp PROPERTIES
    AUTOMOC ON
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    EXPORT_NAME StreamableHttp)
target_include_directories(McpServerStreamableHttp PUBLIC
    "$<BUILD_INTERFACE:${QTMCP_SHI_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/QtMcpServer>")
target_link_libraries(McpServerStreamableHttp PUBLIC McpServer McpCommon Qt6::Core Qt6::Network)

install(TARGETS McpCommon McpServer McpServerStreamableHttp
    EXPORT QtMcpTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(EXPORT QtMcpTargets
    NAMESPACE QtMcp::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QtMcp")
install(DIRECTORY "${QTMCP_STAGE_INCLUDE_DIR}/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/QtMcpPlainConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/QtMcpConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QtMcp")
# The QtMcp package carries the MCP-revision-based release version, not the
# Qt-shaped module version. API only grows across revisions, hence
# AnyNewerVersion.
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/QtMcpConfigVersion.cmake"
    VERSION "${QTMCP_VERSION}"
    COMPATIBILITY AnyNewerVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/QtMcpConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/QtMcpConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QtMcp")

# ---------------------------------------------------------------------------
# Auto tests. The Qt build system runs the suite through qt_build_repo();
# this path builds the tests that need no backend plugins against the targets
# above. The streamablehttp backend is linked in directly and the test
# constructs it through QMcpServer's external-backend constructor
# (QTMCP_EMBEDDED_BACKEND).
# ---------------------------------------------------------------------------
option(QTMCP_BUILD_TESTS "Build the auto tests" OFF)
if(QTMCP_BUILD_TESTS)
    find_package(Qt6 6.8 CONFIG REQUIRED COMPONENTS Network Test)
    enable_testing()

    function(qtmcp_add_test name sources)
        add_executable(${name} ${sources})
        set_target_properties(${name} PROPERTIES
            AUTOMOC ON
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON)
    endfunction()

    qtmcp_add_test(tst_qmcpabstracthttpserver
        tests/auto/mcpserver/qmcpabstracthttpserver/tst_qmcpabstracthttpserver.cpp)
    target_link_libraries(tst_qmcpabstracthttpserver PRIVATE McpServer Qt6::Network Qt6::Test)
    add_test(NAME tst_qmcpabstracthttpserver COMMAND tst_qmcpabstracthttpserver)

    qtmcp_add_test(tst_qmcpserversession
        tests/auto/mcpserver/qmcpserversession/tst_qmcpserversession.cpp)
    target_link_libraries(tst_qmcpserversession PRIVATE McpServer McpCommon Qt6::Test)
    add_test(NAME tst_qmcpserversession COMMAND tst_qmcpserversession)

    qtmcp_add_test(tst_streamablehttp
        tests/auto/mcpserver/streamablehttp/tst_streamablehttp.cpp)
    target_compile_definitions(tst_streamablehttp PRIVATE QTMCP_EMBEDDED_BACKEND)
    target_include_directories(tst_streamablehttp PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/mcpserverbackend/streamablehttp")
    target_link_libraries(tst_streamablehttp PRIVATE
        McpCommon McpServer McpServerStreamableHttp Qt6::Network Qt6::Test)
    add_test(NAME tst_streamablehttp COMMAND tst_streamablehttp)
endif()

endif()
