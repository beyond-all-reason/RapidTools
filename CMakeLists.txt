cmake_minimum_required(VERSION 3.21)

set(RAPIDTOOLS_VERSION "0.0.0" CACHE STRING "Version of RapidTools")
project(RapidTools
    VERSION ${RAPIDTOOLS_VERSION}
    LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Only Linux is supported, so we can fully depend on pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(zlib REQUIRED IMPORTED_TARGET zlib)
pkg_check_modules(lua51 REQUIRED IMPORTED_TARGET lua51)
pkg_check_modules(libgit2 REQUIRED IMPORTED_TARGET libgit2)
pkg_check_modules(libzip REQUIRED IMPORTED_TARGET libzip)

add_subdirectory(External/md5)

add_library(Rapid
    Rapid/ArchiveEntry.cpp
    Rapid/BitArray.cpp
    Rapid/Crc32.cpp
    Rapid/FileEntry.cpp
    Rapid/Gzip.cpp
    Rapid/Hex.cpp
    Rapid/Last.cpp
    Rapid/LastGit.cpp
    Rapid/Lua.cpp
    Rapid/Marshal.cpp
    Rapid/Md5.cpp
    Rapid/Optional.cpp
    Rapid/PoolArchive.cpp
    Rapid/PoolFile.cpp
    Rapid/ScopeGuard.cpp
    Rapid/Store.cpp
    Rapid/String.cpp
    Rapid/TempFile.cpp
    Rapid/Versions.cpp
    Rapid/Zip.cpp
    Rapid/ZipFile.cpp
)
target_link_libraries(Rapid
    PUBLIC md5 PkgConfig::lua51 PkgConfig::zlib PkgConfig::libzip
)
target_include_directories(Rapid
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

add_executable(rapid-addzip Tools/AddZip.cpp)
target_link_libraries(rapid-addzip Rapid)

add_executable(rapid-makezip Tools/MakeZip.cpp)
target_link_libraries(rapid-makezip Rapid)

add_executable(rapid-buildgit Tools/BuildGit.cpp)
target_link_libraries(rapid-buildgit Rapid PkgConfig::libgit2)

add_executable(rapid-streamer Tools/Streamer.cpp)
target_link_libraries(rapid-streamer Rapid)

include(GNUInstallDirs)

install(TARGETS rapid-addzip rapid-makezip rapid-buildgit rapid-streamer Rapid)

if(PROJECT_IS_TOP_LEVEL)
    add_subdirectory(Packaging)
endif()
