cmake_minimum_required(VERSION 3.20)
project(page_index VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dependencies through pkg-config: ICU's common library (icu-uc) and Snowball's libstemmer_c
# (libstemmer, tested with 3.2.0). Set PKG_CONFIG_PATH to their lib/pkgconfig directories.
find_package(PkgConfig REQUIRED)
pkg_check_modules(ICU_UC REQUIRED IMPORTED_TARGET icu-uc)
pkg_check_modules(LIBSTEMMER REQUIRED IMPORTED_TARGET libstemmer)

add_library(page_index STATIC page_index_build.cpp page_index_query.cpp)
target_include_directories(page_index PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include/page_index>)
target_link_libraries(page_index PUBLIC PkgConfig::ICU_UC PkgConfig::LIBSTEMMER)
set_target_properties(page_index PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
if(MSVC)
    target_compile_options(page_index PRIVATE /utf-8)
endif()

option(PAGE_INDEX_TOOLS "Build the test tool page_index_tool" ON)
if(PAGE_INDEX_TOOLS)
    add_executable(page_index_tool tools/page_index_tool.cpp)
    target_link_libraries(page_index_tool PRIVATE page_index)
endif()

# Installed: lib/libpage_index.a (page_index.lib), include/page_index/*.h, lib/pkgconfig/page_index.pc.
# Consumers include <page_index/page_index_query.h>. page_index.pc names icu-uc and libstemmer in
# Requires, so a static link gets them too.
include(GNUInstallDirs)
install(TARGETS page_index ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES page_index_build.h page_index_query.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/page_index)
if(PAGE_INDEX_TOOLS)
    install(TARGETS page_index_tool RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

set(pc_file "${CMAKE_CURRENT_BINARY_DIR}/page_index.pc")
file(WRITE "${pc_file}"
    "prefix=${CMAKE_INSTALL_PREFIX}\n"
    "exec_prefix=\${prefix}\n"
    "libdir=\${exec_prefix}/${CMAKE_INSTALL_LIBDIR}\n"
    "includedir=\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}\n"
    "\n"
    "Name: page_index\n"
    "Description: Small full-text search for help pages and websites (static library)\n"
    "URL: https://github.com/llothar/microcat_page_search\n"
    "Version: ${PROJECT_VERSION}\n"
    "Requires: icu-uc libstemmer\n"
    "Libs: -L\${libdir} -lpage_index\n"
    "Cflags: -I\${includedir}\n")
install(FILES "${pc_file}" DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
