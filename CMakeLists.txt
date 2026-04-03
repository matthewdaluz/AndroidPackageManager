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
include_directories(SYSTEM /usr/include /usr/include/openssl)
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
find_program(PATCH_EXECUTABLE patch)

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

set(APM_USE_SYSTEM_CURL_DEFAULT ON)
if(ANDROID)
  set(APM_USE_SYSTEM_CURL_DEFAULT OFF)
endif()
option(APM_USE_SYSTEM_CURL "Prefer system-installed libcurl" ${APM_USE_SYSTEM_CURL_DEFAULT})

if(APM_USE_SYSTEM_CURL)
  find_package(CURL QUIET)
endif()

set(APM_BUNDLED_TLS_LIBS "")
if(NOT CURL_FOUND)
  message(STATUS "Falling back to bundled libcurl and mbedTLS")
  set(APM_USE_SYSTEM_CURL OFF CACHE BOOL "Prefer system-installed libcurl" FORCE)
  apm_require_program("${GIT_EXECUTABLE}" "git" "to fetch the bundled curl/mbedTLS sources")
  apm_require_program("${PATCH_EXECUTABLE}" "patch" "to apply bundled curl/mbedTLS patches")

  set(_APM_PREV_ENABLE_TESTING_DEFINED FALSE)
  set(_APM_PREV_ENABLE_TESTING_CACHE FALSE)
  if(DEFINED CACHE{ENABLE_TESTING})
    set(_APM_PREV_ENABLE_TESTING_CACHE TRUE)
    set(_APM_PREV_ENABLE_TESTING "${CACHE{ENABLE_TESTING}}")
  elseif(DEFINED ENABLE_TESTING)
    set(_APM_PREV_ENABLE_TESTING_DEFINED TRUE)
    set(_APM_PREV_ENABLE_TESTING "${ENABLE_TESTING}")
  endif()

  set(_APM_PREV_ENABLE_PROGRAMS_DEFINED FALSE)
  set(_APM_PREV_ENABLE_PROGRAMS_CACHE FALSE)
  if(DEFINED CACHE{ENABLE_PROGRAMS})
    set(_APM_PREV_ENABLE_PROGRAMS_CACHE TRUE)
    set(_APM_PREV_ENABLE_PROGRAMS "${CACHE{ENABLE_PROGRAMS}}")
  elseif(DEFINED ENABLE_PROGRAMS)
    set(_APM_PREV_ENABLE_PROGRAMS_DEFINED TRUE)
    set(_APM_PREV_ENABLE_PROGRAMS "${ENABLE_PROGRAMS}")
  endif()

  set(_APM_PREV_BUILD_SHARED_LIBS_DEFINED FALSE)
  set(_APM_PREV_BUILD_SHARED_LIBS_CACHE FALSE)
  if(DEFINED CACHE{BUILD_SHARED_LIBS})
    set(_APM_PREV_BUILD_SHARED_LIBS_CACHE TRUE)
    set(_APM_PREV_BUILD_SHARED_LIBS "${CACHE{BUILD_SHARED_LIBS}}")
  elseif(DEFINED BUILD_SHARED_LIBS)
    set(_APM_PREV_BUILD_SHARED_LIBS_DEFINED TRUE)
    set(_APM_PREV_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
  endif()

  set(ENABLE_TESTING OFF)
  set(ENABLE_PROGRAMS OFF)
  set(BUILD_SHARED_LIBS OFF)
  set(MBEDTLS_FATAL_WARNINGS OFF)
  set(DISABLE_PACKAGE_CONFIG_AND_INSTALL OFF)

  set(_APM_MBEDTLS_PATCH "${CMAKE_SOURCE_DIR}/cmake/patches/mbedtls-disable-tls13.patch")

  FetchContent_Declare(
    mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/v3.6.0/mbedtls-3.6.0.tar.bz2
    URL_HASH SHA256=3ecf94fcfdaacafb757786a01b7538a61750ebd85c4b024f56ff8ba1490fcd38
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PATCH_COMMAND ${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
                  patch -p1 -N -i "${_APM_MBEDTLS_PATCH}"
  )
  FetchContent_MakeAvailable(mbedtls)
  set(MbedTLS_DIR "${mbedtls_BINARY_DIR}" CACHE PATH "Path to bundled mbedTLS" FORCE)
  set(_APM_MBEDTLS_LIBDIR "${mbedtls_BINARY_DIR}/library")
  set(MBEDTLS_INCLUDE_DIRS "${mbedtls_SOURCE_DIR}/include" CACHE PATH "Bundled mbedTLS includes" FORCE)
  set(MBEDTLS_LIBRARY "${_APM_MBEDTLS_LIBDIR}/libmbedtls.a" CACHE FILEPATH "Bundled mbedtls library" FORCE)
  set(MBEDX509_LIBRARY "${_APM_MBEDTLS_LIBDIR}/libmbedx509.a" CACHE FILEPATH "Bundled mbedx509 library" FORCE)
  set(MBEDCRYPTO_LIBRARY "${_APM_MBEDTLS_LIBDIR}/libmbedcrypto.a" CACHE FILEPATH "Bundled mbedcrypto library" FORCE)
  set(MBEDTLS_LIBRARIES "${MBEDTLS_LIBRARY};${MBEDX509_LIBRARY};${MBEDCRYPTO_LIBRARY}" CACHE STRING "Bundled mbedTLS libraries" FORCE)
  set(APM_BUNDLED_TLS_LIBS ${MBEDTLS_LIBRARIES})

  set(BUILD_CURL_EXE OFF)
  set(ENABLE_ARES OFF)
  set(ENABLE_THREADED_RESOLVER OFF)
  set(HTTP_ONLY ON)
  set(CURL_USE_MBEDTLS ON)
  set(CURL_USE_OPENSSL OFF)
  set(CURL_ZLIB ON)
  set(CURL_BROTLI OFF)
  set(CURL_ZSTD OFF)
  set(CURL_DISABLE_LDAP ON)
  set(CURL_DISABLE_LDAPS ON)
  set(CURL_DISABLE_RTSP ON)
  set(CURL_DISABLE_DICT ON)
  set(CURL_DISABLE_GOPHER ON)
  set(CURL_DISABLE_IMAP ON)
  set(CURL_DISABLE_POP3 ON)
  set(CURL_DISABLE_SMTP ON)
  set(CURL_DISABLE_TELNET ON)
  set(CURL_DISABLE_TFTP ON)
  set(CURL_DISABLE_FTP ON)
  set(CURL_DISABLE_SMB ON)
  set(CURL_DISABLE_MQTT ON)

  set(_APM_CURL_PATCH "${CMAKE_SOURCE_DIR}/cmake/patches/curl-mbedtls-rng.patch")

  FetchContent_Declare(
    curl
    URL https://curl.se/download/curl-8.7.1.tar.xz
    URL_HASH SHA256=6fea2aac6a4610fbd0400afb0bcddbe7258a64c63f1f68e5855ebc0c659710cd
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PATCH_COMMAND ${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
                  patch -p1 -N -i "${_APM_CURL_PATCH}"
  )
  FetchContent_MakeAvailable(curl)

  if(NOT TARGET CURL::libcurl AND TARGET libcurl)
    add_library(CURL::libcurl ALIAS libcurl)
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
  unset(_APM_PREV_ENABLE_PROGRAMS_CACHE)
  unset(_APM_PREV_ENABLE_PROGRAMS)
  unset(_APM_PREV_ENABLE_PROGRAMS_DEFINED)
  unset(_APM_PREV_BUILD_SHARED_LIBS_CACHE)
  unset(_APM_PREV_BUILD_SHARED_LIBS)
  unset(_APM_PREV_BUILD_SHARED_LIBS_DEFINED)
  unset(_APM_MBEDTLS_LIBDIR)
endif()

# Ensure mbedTLS is available even when using the system curl.
if(NOT DEFINED MBEDTLS_LIBRARIES OR MBEDTLS_LIBRARIES STREQUAL "")
  apm_require_program("${PATCH_EXECUTABLE}" "patch" "to apply bundled mbedTLS patches")

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

  set(ENABLE_TESTING OFF)
  set(ENABLE_PROGRAMS OFF)
  set(BUILD_SHARED_LIBS OFF)
  set(MBEDTLS_FATAL_WARNINGS OFF)
  set(DISABLE_PACKAGE_CONFIG_AND_INSTALL OFF)

  set(_APM_MBEDTLS_PATCH "${CMAKE_SOURCE_DIR}/cmake/patches/mbedtls-disable-tls13.patch")

  FetchContent_Declare(
    mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/v3.6.0/mbedtls-3.6.0.tar.bz2
    URL_HASH SHA256=3ecf94fcfdaacafb757786a01b7538a61750ebd85c4b024f56ff8ba1490fcd38
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PATCH_COMMAND ${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
                  patch -p1 -N -i "${_APM_MBEDTLS_PATCH}"
  )
  FetchContent_MakeAvailable(mbedtls)
  set(_APM_MBEDTLS_LIBDIR "${mbedtls_BINARY_DIR}/library")
  set(MBEDTLS_INCLUDE_DIRS "${mbedtls_SOURCE_DIR}/include" CACHE PATH "Bundled mbedTLS includes" FORCE)
  set(MBEDTLS_LIBRARY "${_APM_MBEDTLS_LIBDIR}/libmbedtls.a" CACHE FILEPATH "Bundled mbedtls library" FORCE)
  set(MBEDX509_LIBRARY "${_APM_MBEDTLS_LIBDIR}/libmbedx509.a" CACHE FILEPATH "Bundled mbedx509 library" FORCE)
  set(MBEDCRYPTO_LIBRARY "${_APM_MBEDTLS_LIBDIR}/libmbedcrypto.a" CACHE FILEPATH "Bundled mbedcrypto library" FORCE)
  set(MBEDTLS_LIBRARIES "${MBEDTLS_LIBRARY};${MBEDX509_LIBRARY};${MBEDCRYPTO_LIBRARY}" CACHE STRING "Bundled mbedTLS libraries" FORCE)
  if(NOT APM_BUNDLED_TLS_LIBS)
    set(APM_BUNDLED_TLS_LIBS ${MBEDTLS_LIBRARIES})
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
  unset(_APM_PREV_ENABLE_PROGRAMS_CACHE)
  unset(_APM_PREV_ENABLE_PROGRAMS)
  unset(_APM_PREV_ENABLE_PROGRAMS_DEFINED)
  unset(_APM_PREV_BUILD_SHARED_LIBS_CACHE)
  unset(_APM_PREV_BUILD_SHARED_LIBS)
  unset(_APM_PREV_BUILD_SHARED_LIBS_DEFINED)
  unset(_APM_MBEDTLS_LIBDIR)
endif()


# ======================================================
# BoringSSL/OpenSSL
# ======================================================
set(_APM_BORINGSSL_ROOT "${CMAKE_SOURCE_DIR}/prebuilt/boringssl")
set(_APM_BORINGSSL_INCLUDE_DIR "${_APM_BORINGSSL_ROOT}/include")
set(_APM_BORINGSSL_LIBDIR "")

if(ANDROID)
  if(NOT DEFINED ANDROID_ABI OR ANDROID_ABI STREQUAL "")
    message(FATAL_ERROR "ANDROID_ABI must be defined for Android builds so the correct prebuilt BoringSSL archive can be selected.")
  endif()

  set(_APM_BORINGSSL_LIBDIR "${_APM_BORINGSSL_ROOT}/build-${ANDROID_ABI}")
  if(NOT EXISTS "${_APM_BORINGSSL_LIBDIR}/libssl.a"
      OR NOT EXISTS "${_APM_BORINGSSL_LIBDIR}/libcrypto.a"
      OR NOT EXISTS "${_APM_BORINGSSL_INCLUDE_DIR}")
    message(FATAL_ERROR
      "Prebuilt BoringSSL for Android ABI ${ANDROID_ABI} was not found.\n"
      "Expected:\n"
      "  ${_APM_BORINGSSL_LIBDIR}/libssl.a\n"
      "  ${_APM_BORINGSSL_LIBDIR}/libcrypto.a\n"
      "  ${_APM_BORINGSSL_INCLUDE_DIR}/\n")
  endif()

  message(STATUS "Using prebuilt BoringSSL for ${ANDROID_ABI} from ${_APM_BORINGSSL_LIBDIR}")
  set(OPENSSL_SSL_LIBRARY "${_APM_BORINGSSL_LIBDIR}/libssl.a" CACHE FILEPATH "Prebuilt BoringSSL libssl" FORCE)
  set(OPENSSL_CRYPTO_LIBRARY "${_APM_BORINGSSL_LIBDIR}/libcrypto.a" CACHE FILEPATH "Prebuilt BoringSSL libcrypto" FORCE)
  include_directories(SYSTEM "${_APM_BORINGSSL_INCLUDE_DIR}")
endif()

if(NOT OPENSSL_SSL_LIBRARY OR NOT OPENSSL_CRYPTO_LIBRARY)
  find_library(OPENSSL_SSL_LIBRARY NAMES ssl)
  find_library(OPENSSL_CRYPTO_LIBRARY NAMES crypto)
endif()

if((NOT OPENSSL_SSL_LIBRARY OR NOT OPENSSL_CRYPTO_LIBRARY)
    AND EXISTS "${_APM_BORINGSSL_ROOT}/libssl.a"
    AND EXISTS "${_APM_BORINGSSL_ROOT}/libcrypto.a")
  # Legacy flat prebuilt layout fallback.
  message(STATUS "Using prebuilt BoringSSL from ${_APM_BORINGSSL_ROOT}")
  set(OPENSSL_SSL_LIBRARY "${_APM_BORINGSSL_ROOT}/libssl.a" CACHE FILEPATH "Prebuilt BoringSSL libssl" FORCE)
  set(OPENSSL_CRYPTO_LIBRARY "${_APM_BORINGSSL_ROOT}/libcrypto.a" CACHE FILEPATH "Prebuilt BoringSSL libcrypto" FORCE)
  if(EXISTS "${_APM_BORINGSSL_INCLUDE_DIR}")
    include_directories(SYSTEM "${_APM_BORINGSSL_INCLUDE_DIR}")
  endif()
endif()
if(NOT OPENSSL_SSL_LIBRARY OR NOT OPENSSL_CRYPTO_LIBRARY)
  message(FATAL_ERROR 
    "OpenSSL/BoringSSL libraries not found.\n"
    "\n"
    "To build BoringSSL for this project:\n"
    "  1. Clone BoringSSL:\n"
    "       git clone https://github.com/google/boringssl.git\n"
    "       cd boringssl\n"
    "\n"
    "  2. Build BoringSSL:\n"
    "       cmake -DCMAKE_BUILD_TYPE=Release .. -B build\n"
    "       cmake --build build -j$(nproc)\n"
    "\n"
    "  3. Copy the built libraries to the prebuilt directory.\n"
    "     Android builds expect ABI-specific archives under:\n"
    "       ${CMAKE_SOURCE_DIR}/prebuilt/boringssl/build-${ANDROID_ABI}\n"
    "     with shared headers under:\n"
    "       ${CMAKE_SOURCE_DIR}/prebuilt/boringssl/include\n"
    "\n"
    "  4. Re-run CMake to detect the prebuilt libraries.\n"
    "\n"
    "Alternatively, install system OpenSSL development packages (e.g., libssl-dev).")
endif()


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
    src/util/crypto/sha256.cpp
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
    $<IF:$<BOOL:${ANDROID}>,, /usr/include>
    src/thirdparty/include
    include
)

target_link_libraries(apm PRIVATE
    CURL::libcurl
    ZLIB::ZLIB
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${APM_BUNDLED_TLS_LIBS}
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
    src/apmd/request_dispatcher.cpp
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
    src/util/crypto/sha256.cpp

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
    $<IF:$<BOOL:${ANDROID}>,, /usr/include>
    src/thirdparty/include
    include
)


target_link_libraries(apmd PRIVATE
    CURL::libcurl
    ZLIB::ZLIB
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${APM_BUNDLED_TLS_LIBS}
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
    $<IF:$<BOOL:${ANDROID}>,, /usr/include>
    src/thirdparty/include
    include
)

target_link_libraries(amsd PRIVATE
    CURL::libcurl
    ZLIB::ZLIB
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${APM_BUNDLED_TLS_LIBS}
)

if(TARGET mbedcrypto)
  add_dependencies(apm mbedcrypto)
  add_dependencies(apmd mbedcrypto)
  add_dependencies(amsd mbedcrypto)
endif()

if(TARGET mbedx509)
  add_dependencies(apm mbedx509)
  add_dependencies(apmd mbedx509)
  add_dependencies(amsd mbedx509)
endif()

if(TARGET mbedtls)
  add_dependencies(apm mbedtls)
  add_dependencies(apmd mbedtls)
  add_dependencies(amsd mbedtls)
endif()
