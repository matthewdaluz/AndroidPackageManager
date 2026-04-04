# APM - Android Package Manager
#
# RedHead Industries - Technologies Branch
# Copyright (C) 2025 RedHead Industries
#
# File: CMakeLists.txt
# Purpose: Configure the build for APM components (CLI, apmd, amsd) and dependencies.
# Last Modified: December 4th, 2025. - 09:07 AM Eastern Time
# Author: Matthew DaLuz - RedHead Founder
#
# APM is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# APM is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with APM. If not, see <https://www.gnu.org/licenses/>.
#

cmake_minimum_required(VERSION 3.14)
project(APM)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_SYSTEM_VERSION 29)
set(CMAKE_ANDROID_API 29)

# Emulator mode for x86_64 Linux PC builds
option(APM_EMULATOR_MODE "Build APM in emulator mode for x86_64 Linux" OFF)
if(APM_EMULATOR_MODE)
  add_compile_definitions(APM_EMULATOR_MODE=1)
  message(STATUS "APM Emulator Mode: ENABLED")
else()
  message(STATUS "APM Emulator Mode: DISABLED")
endif()


include(FetchContent)

if(POLICY CMP0077)
  cmake_policy(SET CMP0077 NEW)
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
endif()

find_package(Git QUIET)

function(apm_require_program program_path pretty_name reason)
  if(NOT program_path)
    message(FATAL_ERROR "${pretty_name} is required ${reason}. Install the prerequisites listed in README.md.")
  endif()
endfunction()

# ======================================================
# Android NDK Setup
# ======================================================
if(ANDROID)
  set(ANDROID_SDK_ROOT "$ENV{ANDROID_SDK_ROOT}" CACHE PATH "Path to Android SDK" FORCE)
  if(NOT ANDROID_SDK_ROOT OR ANDROID_SDK_ROOT STREQUAL "")
    set(ANDROID_SDK_ROOT "/opt/android-sdk" CACHE PATH "Path to Android SDK" FORCE)
  endif()

  # Allow user to provide explicit NDK root, otherwise derive from SDK root
  set(ANDROID_NDK_ROOT "$ENV{ANDROID_NDK_ROOT}" CACHE PATH "Path to Android NDK" FORCE)
  if(NOT ANDROID_NDK_ROOT OR ANDROID_NDK_ROOT STREQUAL "")
    # Try common env fallbacks
    if(DEFINED ENV{ANDROID_NDK_HOME} AND NOT "$ENV{ANDROID_NDK_HOME}" STREQUAL "")
      set(ANDROID_NDK_ROOT "$ENV{ANDROID_NDK_HOME}" CACHE PATH "Path to Android NDK" FORCE)
    endif()
  endif()

  # If still not set, attempt to auto-detect latest NDK under /opt/android-sdk/ndk/*
  if((NOT ANDROID_NDK_ROOT OR ANDROID_NDK_ROOT STREQUAL "") AND EXISTS "${ANDROID_SDK_ROOT}/ndk")
    file(GLOB _APM_NDK_DIRS LIST_DIRECTORIES TRUE "${ANDROID_SDK_ROOT}/ndk/*")
    if(_APM_NDK_DIRS)
      list(SORT _APM_NDK_DIRS)
      list(REVERSE _APM_NDK_DIRS)
      list(GET _APM_NDK_DIRS 0 _APM_NDK_CANDIDATE)
      set(ANDROID_NDK_ROOT "${_APM_NDK_CANDIDATE}" CACHE PATH "Auto-detected Android NDK" FORCE)
    endif()
  endif()

  if(NOT ANDROID_NDK_ROOT OR NOT EXISTS "${ANDROID_NDK_ROOT}")
    message(FATAL_ERROR "Android NDK not found. Set ANDROID_NDK_ROOT or ensure it exists under ${ANDROID_SDK_ROOT}/ndk/")
  endif()

  # Set CMake's Android NDK variables
  set(CMAKE_ANDROID_NDK "${ANDROID_NDK_ROOT}" CACHE PATH "Android NDK root" FORCE)
  # Harmonize API level with ANDROID_PLATFORM if provided by toolchain invocation
  if(DEFINED ANDROID_PLATFORM)
    string(REPLACE "android-" "" _APM_API "${ANDROID_PLATFORM}")
    set(CMAKE_ANDROID_API "${_APM_API}" CACHE STRING "Target Android API level" FORCE)
  else()
    set(CMAKE_ANDROID_API 34 CACHE STRING "Target Android API level" FORCE)
  endif()
endif()

# ======================================================
# ZLIB Setup - Simplified Approach
# ======================================================
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
  message(STATUS "ZLIB not found, downloading and building...")
  apm_require_program("${GIT_EXECUTABLE}" "git" "to fetch the bundled zlib source")

  # Use a known version that has good CMake support
  FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG v1.3.1
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )

  FetchContent_GetProperties(zlib)
  if(NOT zlib_POPULATED)
    FetchContent_Populate(zlib)

    # Build as static library
    set(BUILD_SHARED_LIBS OFF)
    set(SKIP_INSTALL_ALL ON)

    # Add zlib subdirectory
    add_subdirectory(${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})

    # Create the alias target
    if(TARGET zlibstatic AND NOT TARGET ZLIB::ZLIB)
      add_library(ZLIB::ZLIB ALIAS zlibstatic)
    elseif(TARGET zlib AND NOT TARGET ZLIB::ZLIB)
      add_library(ZLIB::ZLIB ALIAS zlib)
    endif()

    if(TARGET ZLIB::ZLIB)
      # Help any downstream find_package(ZLIB) invocations (e.g. from curl)
      set(ZLIB_LIBRARY ZLIB::ZLIB CACHE STRING "Bundled ZLIB target" FORCE)
      set(ZLIB_LIBRARIES ZLIB::ZLIB CACHE STRING "Bundled ZLIB target" FORCE)
      set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR}" CACHE PATH "Bundled ZLIB include" FORCE)
      set(ZLIB_INCLUDE_DIRS "${ZLIB_INCLUDE_DIR};${zlib_BINARY_DIR}" CACHE STRING "Bundled ZLIB include dirs" FORCE)
      set(ZLIB_FOUND TRUE CACHE BOOL "Bundled ZLIB built" FORCE)
      message(STATUS "ZLIB built successfully")
    else()
      message(FATAL_ERROR "Failed to create ZLIB::ZLIB target")
    endif()
  endif()
else()
  message(STATUS "ZLIB found: ${ZLIB_VERSION}")
endif()

function(apm_resolve_boringssl_libdir out_var)
  if(ANDROID)
    if(NOT DEFINED ANDROID_ABI OR ANDROID_ABI STREQUAL "")
      message(FATAL_ERROR "ANDROID_ABI must be defined for Android builds so the correct prebuilt BoringSSL archive can be selected.")
    endif()
    set(_apm_boringssl_arch "${ANDROID_ABI}")
  else()
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _apm_host_arch)
    if(_apm_host_arch STREQUAL "x86_64" OR _apm_host_arch STREQUAL "amd64")
      set(_apm_boringssl_arch "x86_64")
    elseif(_apm_host_arch STREQUAL "x86" OR _apm_host_arch MATCHES "^i[3-6]86$")
      set(_apm_boringssl_arch "x86")
    else()
      message(FATAL_ERROR
        "Unsupported host architecture '${CMAKE_SYSTEM_PROCESSOR}' for prebuilt BoringSSL.\n"
        "Supported desktop/emulator prebuilt directories are build-x86_64 and build-x86.")
    endif()
  endif()

  set(${out_var} "${APM_BORINGSSL_ROOT}/build-${_apm_boringssl_arch}" PARENT_SCOPE)
endfunction()

set(APM_BORINGSSL_ROOT "${CMAKE_SOURCE_DIR}/prebuilt/boringssl")
set(APM_BORINGSSL_INCLUDE_DIR "${APM_BORINGSSL_ROOT}/include")
apm_resolve_boringssl_libdir(APM_BORINGSSL_LIBDIR)

set(APM_BORINGSSL_SSL_LIBRARY "${APM_BORINGSSL_LIBDIR}/libssl.a")
set(APM_BORINGSSL_CRYPTO_LIBRARY "${APM_BORINGSSL_LIBDIR}/libcrypto.a")

if(NOT EXISTS "${APM_BORINGSSL_SSL_LIBRARY}"
    OR NOT EXISTS "${APM_BORINGSSL_CRYPTO_LIBRARY}"
    OR NOT EXISTS "${APM_BORINGSSL_INCLUDE_DIR}/openssl/base.h")
  if(ANDROID)
    set(_apm_boringssl_expected
      "  ${APM_BORINGSSL_SSL_LIBRARY}\n"
      "  ${APM_BORINGSSL_CRYPTO_LIBRARY}\n"
      "  ${APM_BORINGSSL_INCLUDE_DIR}/openssl/base.h")
    set(_apm_boringssl_hint
      "Android builds require ABI-specific BoringSSL prebuilts under prebuilt/boringssl/build-${ANDROID_ABI}.")
  else()
    set(_apm_boringssl_expected
      "  ${APM_BORINGSSL_SSL_LIBRARY}\n"
      "  ${APM_BORINGSSL_CRYPTO_LIBRARY}\n"
      "  ${APM_BORINGSSL_INCLUDE_DIR}/openssl/base.h")
    set(_apm_boringssl_hint
      "Desktop/emulator builds require host prebuilts under prebuilt/boringssl/build-x86_64 or build-x86.")
  endif()

  message(FATAL_ERROR
    "Required BoringSSL prebuilts were not found.\n"
    "Expected:\n${_apm_boringssl_expected}\n"
    "${_apm_boringssl_hint}")
endif()

message(STATUS "Using prebuilt BoringSSL from ${APM_BORINGSSL_LIBDIR}")
include_directories(SYSTEM "${APM_BORINGSSL_INCLUDE_DIR}")

# These cache entries allow bundled curl's FindOpenSSL module to resolve the
# BoringSSL prebuilts without probing host OpenSSL installations.
set(OPENSSL_ROOT_DIR "${APM_BORINGSSL_ROOT}" CACHE PATH "APM BoringSSL root" FORCE)
set(OPENSSL_INCLUDE_DIR "${APM_BORINGSSL_INCLUDE_DIR}" CACHE PATH "APM BoringSSL includes" FORCE)
set(OPENSSL_SSL_LIBRARY "${APM_BORINGSSL_SSL_LIBRARY}" CACHE FILEPATH "APM BoringSSL libssl" FORCE)
set(OPENSSL_CRYPTO_LIBRARY "${APM_BORINGSSL_CRYPTO_LIBRARY}" CACHE FILEPATH "APM BoringSSL libcrypto" FORCE)
set(OPENSSL_LIBRARIES "${OPENSSL_SSL_LIBRARY};${OPENSSL_CRYPTO_LIBRARY}" CACHE STRING "APM BoringSSL libraries" FORCE)
set(OPENSSL_USE_STATIC_LIBS TRUE CACHE BOOL "Link BoringSSL statically" FORCE)

message(STATUS "Configuring bundled libcurl with BoringSSL")

set(_APM_PREV_ENABLE_TESTING_CACHE FALSE)
set(_APM_PREV_ENABLE_TESTING_DEFINED FALSE)
if(DEFINED CACHE{ENABLE_TESTING})
  set(_APM_PREV_ENABLE_TESTING_CACHE TRUE)
  set(_APM_PREV_ENABLE_TESTING "${CACHE{ENABLE_TESTING}}")
elseif(DEFINED ENABLE_TESTING)
  set(_APM_PREV_ENABLE_TESTING_DEFINED TRUE)
  set(_APM_PREV_ENABLE_TESTING "${ENABLE_TESTING}")
endif()

set(_APM_PREV_ENABLE_PROGRAMS_CACHE FALSE)
set(_APM_PREV_ENABLE_PROGRAMS_DEFINED FALSE)
if(DEFINED CACHE{ENABLE_PROGRAMS})
  set(_APM_PREV_ENABLE_PROGRAMS_CACHE TRUE)
  set(_APM_PREV_ENABLE_PROGRAMS "${CACHE{ENABLE_PROGRAMS}}")
elseif(DEFINED ENABLE_PROGRAMS)
  set(_APM_PREV_ENABLE_PROGRAMS_DEFINED TRUE)
  set(_APM_PREV_ENABLE_PROGRAMS "${ENABLE_PROGRAMS}")
endif()

set(_APM_PREV_BUILD_SHARED_LIBS_CACHE FALSE)
set(_APM_PREV_BUILD_SHARED_LIBS_DEFINED FALSE)
if(DEFINED CACHE{BUILD_SHARED_LIBS})
  set(_APM_PREV_BUILD_SHARED_LIBS_CACHE TRUE)
  set(_APM_PREV_BUILD_SHARED_LIBS "${CACHE{BUILD_SHARED_LIBS}}")
elseif(DEFINED BUILD_SHARED_LIBS)
  set(_APM_PREV_BUILD_SHARED_LIBS_DEFINED TRUE)
  set(_APM_PREV_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
endif()

set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
set(ENABLE_ARES OFF CACHE BOOL "" FORCE)
set(ENABLE_THREADED_RESOLVER OFF CACHE BOOL "" FORCE)
set(HTTP_ONLY ON CACHE BOOL "" FORCE)
set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_ZLIB ON CACHE STRING "" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_RTSP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_DICT ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_GOPHER ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_IMAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_POP3 ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_SMTP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_TELNET ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_TFTP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_FTP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_SMB ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_MQTT ON CACHE BOOL "" FORCE)

set(_APM_PREV_DISABLE_PKGCONFIG_DEFINED FALSE)
set(_APM_PREV_DISABLE_PKGCONFIG FALSE)
if(DEFINED CMAKE_DISABLE_FIND_PACKAGE_PkgConfig)
  set(_APM_PREV_DISABLE_PKGCONFIG_DEFINED TRUE)
  set(_APM_PREV_DISABLE_PKGCONFIG "${CMAKE_DISABLE_FIND_PACKAGE_PkgConfig}")
endif()

# Prevent curl's nested FindOpenSSL/FindPkgConfig calls from importing host
# pkg-config link flags such as zstd when cross-compiling Android targets.
set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig ON)

FetchContent_Declare(
  curl
  URL https://curl.se/download/curl-8.7.1.tar.xz
  URL_HASH SHA256=6fea2aac6a4610fbd0400afb0bcddbe7258a64c63f1f68e5855ebc0c659710cd
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(curl)

if(NOT TARGET CURL::libcurl AND TARGET libcurl)
  add_library(CURL::libcurl ALIAS libcurl)
endif()

if(NOT TARGET CURL::libcurl)
  message(FATAL_ERROR "Bundled libcurl was configured, but target CURL::libcurl is unavailable.")
endif()

if(_APM_PREV_DISABLE_PKGCONFIG_DEFINED)
  set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig "${_APM_PREV_DISABLE_PKGCONFIG}")
else()
  unset(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig)
endif()

if(_APM_PREV_ENABLE_TESTING_CACHE)
  set(ENABLE_TESTING "${_APM_PREV_ENABLE_TESTING}" CACHE BOOL "" FORCE)
elseif(_APM_PREV_ENABLE_TESTING_DEFINED)
  set(ENABLE_TESTING "${_APM_PREV_ENABLE_TESTING}")
else()
  unset(ENABLE_TESTING CACHE)
endif()

if(_APM_PREV_ENABLE_PROGRAMS_CACHE)
  set(ENABLE_PROGRAMS "${_APM_PREV_ENABLE_PROGRAMS}" CACHE BOOL "" FORCE)
elseif(_APM_PREV_ENABLE_PROGRAMS_DEFINED)
  set(ENABLE_PROGRAMS "${_APM_PREV_ENABLE_PROGRAMS}")
else()
  unset(ENABLE_PROGRAMS CACHE)
endif()

if(_APM_PREV_BUILD_SHARED_LIBS_CACHE)
  set(BUILD_SHARED_LIBS "${_APM_PREV_BUILD_SHARED_LIBS}" CACHE BOOL "" FORCE)
elseif(_APM_PREV_BUILD_SHARED_LIBS_DEFINED)
  set(BUILD_SHARED_LIBS "${_APM_PREV_BUILD_SHARED_LIBS}")
else()
  unset(BUILD_SHARED_LIBS CACHE)
endif()

unset(_APM_PREV_ENABLE_TESTING_CACHE)
unset(_APM_PREV_ENABLE_TESTING)
unset(_APM_PREV_ENABLE_TESTING_DEFINED)
unset(_APM_PREV_DISABLE_PKGCONFIG)
unset(_APM_PREV_DISABLE_PKGCONFIG_DEFINED)
unset(_APM_PREV_ENABLE_PROGRAMS_CACHE)
unset(_APM_PREV_ENABLE_PROGRAMS)
unset(_APM_PREV_ENABLE_PROGRAMS_DEFINED)
unset(_APM_PREV_BUILD_SHARED_LIBS_CACHE)
unset(_APM_PREV_BUILD_SHARED_LIBS)
unset(_APM_PREV_BUILD_SHARED_LIBS_DEFINED)
unset(_apm_boringssl_expected)
unset(_apm_boringssl_hint)


# ======================================================
# APM Client Binary
# ======================================================
set(APM_CLIENT_SOURCES
    src/apm/apm.cpp
    src/apm/ipc_client.cpp
    src/apm/transport.cpp
    src/core/config.cpp
    src/apmd/apk_installer.cpp
    src/apmd/export_path.cpp
    src/apm/commands/apk_install.cpp
    src/apm/commands/list.cpp
    src/apm/commands/info.cpp
    src/apm/commands/search.cpp

    src/apmd/protocol.cpp

    src/core/control_parser.cpp
    src/core/deb_extractor.cpp
    src/core/repo_index.cpp
    src/core/dependency.cpp
    src/core/logger.cpp
    src/core/tar_extractor.cpp
    src/core/downloader.cpp
    src/core/status_db.cpp
    src/core/manual_package.cpp
    src/core/release_parser.cpp
    src/core/security.cpp
    src/ams/module_info.cpp
    src/ams/module_manager.cpp
    src/util/crypto/sha256.cpp
    src/util/crypto/md5.cpp
    src/util/crypto/gpg_verify.cpp

    src/util/fs.cpp
    src/util/process_lock.cpp
)

add_executable(apm ${APM_CLIENT_SOURCES})

target_include_directories(apm PRIVATE
    src/apm/include
    src/apmd/include
    src/ams/include
    src/core/include
    src/util/include
    src/util/crypto
    src/apm/commands/include
    include
)

target_link_libraries(apm PRIVATE
    CURL::libcurl
    ZLIB::ZLIB
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
)

# ======================================================
# APM Daemon Binary
# ======================================================
set(APM_DAEMON_SOURCES
    src/apmd/apmd.cpp
    src/apmd/ipc_server.cpp
    src/core/config.cpp
    src/apm/transport.cpp
    src/apm/ipc_client.cpp
    src/apmd/protocol.cpp
    src/apmd/install_manager.cpp
    src/apmd/export_path.cpp
    src/apmd/apk_installer.cpp
    src/apmd/boringssl_client.cpp
    src/apmd/factory_reset.cpp

    src/core/control_parser.cpp
    src/core/deb_extractor.cpp
    src/core/repo_index.cpp
    src/core/dependency.cpp
    src/core/logger.cpp
    src/core/tar_extractor.cpp
    src/core/downloader.cpp
    src/core/status_db.cpp
    src/core/manual_package.cpp
    src/core/release_parser.cpp
    src/core/security.cpp
    src/ams/module_info.cpp
    src/ams/module_manager.cpp
    src/util/crypto/sha256.cpp
    src/util/crypto/md5.cpp
    src/util/crypto/gpg_verify.cpp

    src/util/fs.cpp
    src/util/process_lock.cpp

    src/apmd/security_manager.cpp
)

add_executable(apmd ${APM_DAEMON_SOURCES})

target_include_directories(apmd PRIVATE
    src/apmd/include
  src/apm/include
    src/ams/include
    src/core/include
    src/util/include
    src/util/crypto
    src/apm/commands/include
    include
)


target_link_libraries(apmd PRIVATE
    CURL::libcurl
    ZLIB::ZLIB
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
)

set(AMSD_DAEMON_SOURCES
    src/amsd/amsd.cpp
    src/amsd/ipc_server.cpp
    src/amsd/request_dispatcher.cpp
    src/amsd/security_manager.cpp
    src/apmd/protocol.cpp
    src/apmd/boringssl_client.cpp
    src/core/config.cpp
    src/core/logger.cpp
    src/core/security.cpp
    src/ams/module_info.cpp
    src/ams/module_manager.cpp
    src/util/fs.cpp
    src/util/process_lock.cpp
    src/util/crypto/sha256.cpp
    src/util/crypto/md5.cpp
    src/util/crypto/gpg_verify.cpp
)

add_executable(amsd ${AMSD_DAEMON_SOURCES})

target_include_directories(amsd PRIVATE
    src/amsd
    src/apmd/include
    src/ams/include
    src/core/include
    src/util/include
    src/util/crypto
    include
)

target_link_libraries(amsd PRIVATE
    CURL::libcurl
    ZLIB::ZLIB
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
)
