cmake_minimum_required(VERSION 3.20)
project(nfsu2_decomp CXX)

set(CMAKE_CXX_STANDARD 20)
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "nfsu2 is built as 32-bit x86: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/linux-i386.cmake")
endif()
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
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_compile_definitions(bware PUBLIC NFSU2_32BIT=1)
endif()
target_include_directories(bware SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/compat ${NFSMW}/src ${NFSMW}/include ${NFSMW}/src/Speed/Indep/Libs/allocator/1.5.0
    ${NFSMW}/src/Speed/Indep/Libs/csis/dev/include ${NFSMW}/src/Speed/Indep/Libs/realcore/6.24.00/include/common
    ${NFSMW}/src/Speed/Indep/Libs)
target_compile_options(bware PUBLIC -include ${CMAKE_SOURCE_DIR}/compat/nfsu2_prelude.h
    -fno-lifetime-dse -fsanitize=return -fno-sanitize-recover=return PRIVATE -w)
# Upstream left some functions as empty stubs; abort loudly if one is reached (see PATCHES.md).
target_link_options(bware PUBLIC -fsanitize=return)

enable_testing()

add_executable(bware_test tests/bware_test.cpp src/platform/linux/bFunkStub.cpp src/platform/linux/PlatformSystem.cpp)
target_include_directories(bware_test PRIVATE src)
target_link_libraries(bware_test PRIVATE bware)
add_test(NAME bware COMMAND bware_test)

# Career save format: best-time direction key (0x4fb4ac) and v1 compatibility.
add_executable(profile_test tests/profile_test.cpp src/game/indep/Career/Profile.cpp src/game/indep/Career/Records.cpp src/game/indep/Career/Memcard.cpp)
target_include_directories(profile_test PRIVATE src)
add_test(NAME profile COMMAND profile_test)

# EA FEng front-end engine, reused from nfsmw. UG2 packages are the same format (version 0x20000).
file(GLOB FENG_SOURCES ${NFSMW}/src/Speed/Indep/Src/FEng/*.cpp)
add_library(feng STATIC ${FENG_SOURCES})
target_link_libraries(feng PUBLIC bware)
target_compile_options(feng PRIVATE -w)

add_executable(feng_test tests/feng_test.cpp src/platform/linux/bFunkStub.cpp src/platform/linux/PlatformSystem.cpp src/game/indep/FEng/FEngMissing.cpp)
target_include_directories(feng_test PRIVATE src)
target_link_libraries(feng_test PRIVATE feng)

file(GLOB_RECURSE NFSU2_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(nfsu2 ${NFSU2_SOURCES})
target_include_directories(nfsu2 PRIVATE include src)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswresample)
find_package(OpenGL REQUIRED)
target_link_libraries(nfsu2 PRIVATE SDL2::SDL2 bware feng PkgConfig::FFMPEG OpenGL::GL)
target_compile_options(nfsu2 PRIVATE -Wall -Wextra)


add_executable(texture_test tests/texture_test.cpp src/game/indep/Texture/TexturePack.cpp src/game/bWare/bCompression.cpp src/game/bWare/bHuffman.cpp)
target_include_directories(texture_test PRIVATE src)
target_link_libraries(texture_test PRIVATE SDL2::SDL2)

add_executable(huff_test tests/huff_test.cpp src/game/bWare/bCompression.cpp src/game/bWare/bHuffman.cpp)
target_include_directories(huff_test PRIVATE src)
