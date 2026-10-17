cmake_minimum_required(VERSION 3.20)
project(nfsu2_decomp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(SDL2 REQUIRED)

file(GLOB_RECURSE NFSU2_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(nfsu2 ${NFSU2_SOURCES})
target_include_directories(nfsu2 PRIVATE include src)
target_link_libraries(nfsu2 PRIVATE SDL2::SDL2)
target_compile_options(nfsu2 PRIVATE -Wall -Wextra)

enable_testing()
