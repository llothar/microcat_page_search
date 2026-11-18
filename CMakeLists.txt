cmake_minimum_required(VERSION 3.20)
project(page_index LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Snowball libstemmer_c: point LIBSTEMMER_DIR at a built libstemmer_c tree (include/, libstemmer.a)
set(LIBSTEMMER_DIR "" CACHE PATH "Directory of a built libstemmer_c")
find_path(LIBSTEMMER_INCLUDE libstemmer.h HINTS ${LIBSTEMMER_DIR}/include REQUIRED)
find_library(LIBSTEMMER_LIBRARY NAMES stemmer libstemmer.a HINTS ${LIBSTEMMER_DIR} REQUIRED)
find_package(ICU REQUIRED COMPONENTS uc data)

add_library(page_index STATIC page_index_build.cpp page_index_query.cpp)
target_include_directories(page_index PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${LIBSTEMMER_INCLUDE})
target_link_libraries(page_index PUBLIC ICU::uc ICU::data ${LIBSTEMMER_LIBRARY})

option(PAGE_INDEX_TOOLS "Build the test tool" ON)
if(PAGE_INDEX_TOOLS)
    add_executable(page_index_tool tools/page_index_tool.cpp)
    target_link_libraries(page_index_tool PRIVATE page_index)
endif()
