cmake_minimum_required(VERSION 3.30)
project(qumirdb VERSION 0.5.1)
enable_testing()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(QDB_BUILD_SERVICE "Build QumirDB web service" OFF)
option(QDB_BUILD_DAEMON "Build QumirDB daemon" OFF)
option(QDB_BUILD_TESTS "Build QumirDB tests" OFF)
# Static LLVM archives add ~50 MB to every binary linking the codegen; the shared
# libLLVM trades that for a runtime dependency on the LLVM package.
option(QDB_LLVM_LINK_SHARED "Link the shared libLLVM instead of static LLVM archives" OFF)
set(QUMIR_LLVM_LINK_SHARED ${QDB_LLVM_LINK_SHARED})
set(QDB_BUILD_NUMBER "0" CACHE STRING "Build number, becomes the Debian revision after the hyphen")

# The upstream version is bumped by hand above; the build number rides along as the
# Debian revision, so the package version reads 0.1.0-42.
set(QDB_DEB_VERSION "${PROJECT_VERSION}-${QDB_BUILD_NUMBER}")

# Only major.minor names the install tree, so patch releases replace files in place.
set(QDB_RELEASE "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
set(QDB_DATA_DIR "/var/lib/qumirdb-data" CACHE STRING "Writable data directory, created by the administrator")

# Build number 0 means a working copy; anything else comes from CI and names a package.
string(TIMESTAMP QDB_BUILD_DATE "%Y-%m-%d" UTC)
if(QDB_BUILD_NUMBER STREQUAL "0")
    set(QDB_VERSION_STRING "dev,${QDB_BUILD_DATE}")
else()
    execute_process(
        COMMAND git rev-parse --short=7 HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE QDB_GIT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(QDB_GIT_SHA)
        set(QDB_VERSION_STRING "${PROJECT_VERSION}-${QDB_BUILD_NUMBER},${QDB_GIT_SHA},${QDB_BUILD_DATE}")
    else()
        set(QDB_VERSION_STRING "${PROJECT_VERSION}-${QDB_BUILD_NUMBER},${QDB_BUILD_DATE}")
    endif()
endif()
message(STATUS "QumirDB version string: ${QDB_VERSION_STRING}")
set(QDB_PRIVATE_DIR "lib/qumirdb-${QDB_RELEASE}")
set(QDB_PRIVATE_BINDIR "${QDB_PRIVATE_DIR}/bin")
set(QDB_MODULES_DIR "${QDB_PRIVATE_DIR}/modules")

add_subdirectory(externals)
add_subdirectory(qdb)
add_subdirectory(bin)
if(QDB_BUILD_TESTS)
    add_subdirectory(test)
endif()
if(QDB_BUILD_SERVICE)
    add_subdirectory(service)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(cmake/QdbPackaging.cmake)
endif()
