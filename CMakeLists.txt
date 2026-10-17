cmake_minimum_required(VERSION 3.20)
project(nfsu2_decomp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(SDL2 REQUIRED)

# EA bWare engine core, reused from the nfsmw decompilation (third_party/nfsmw, CC0).
set(NFSMW ${CMAKE_SOURCE_DIR}/third_party/nfsmw)
set(BWARE_FILES bSlotPool bList Strings bChunk bMemory bPrintf bMath bCrc32 bVector bMatrix bDebug)
list(TRANSFORM BWARE_FILES PREPEND ${NFSMW}/src/Speed/Indep/bWare/Src/)
list(TRANSFORM BWARE_FILES APPEND .cpp)
add_library(bware STATIC ${BWARE_FILES})
target_compile_definitions(bware PUBLIC EA_PLATFORM_WIN32)
target_include_directories(bware PUBLIC
    ${CMAKE_SOURCE_DIR}/compat ${NFSMW}/src ${NFSMW}/include ${NFSMW}/src/Speed/Indep/Libs/allocator/1.5.0)
target_compile_options(bware PUBLIC -include ${CMAKE_SOURCE_DIR}/compat/nfsu2_prelude.h
    PRIVATE -w -fsanitize=return -fno-sanitize-recover=return)
# Upstream left some functions as empty stubs; abort loudly if one is reached (see PATCHES.md).
target_link_options(bware PUBLIC -fsanitize=return)

file(GLOB_RECURSE NFSU2_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(nfsu2 ${NFSU2_SOURCES})
target_include_directories(nfsu2 PRIVATE include src)
target_link_libraries(nfsu2 PRIVATE SDL2::SDL2 bware)
target_compile_options(nfsu2 PRIVATE -Wall -Wextra)

enable_testing()

add_executable(bware_test tests/bware_test.cpp src/platform/linux/bFunkStub.cpp src/platform/linux/PlatformSystem.cpp)
target_include_directories(bware_test PRIVATE src)
target_link_libraries(bware_test PRIVATE bware)
add_test(NAME bware COMMAND bware_test)
