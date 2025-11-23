cmake_minimum_required(VERSION 3.14)
project(APM)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(POLICY CMP0077)
  cmake_policy(SET CMP0077 NEW)
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
endif()

# ======================================================
# Android NDK Setup
# ======================================================
if(ANDROID)
  set(ANDROID_NDK_PATH "$ENV{HOME}/Android/NDK" CACHE PATH "Path to Android NDK")
  
  if(NOT EXISTS "${ANDROID_NDK_PATH}")
    message(STATUS "Android NDK not found at ${ANDROID_NDK_PATH}, downloading...")
    
    set(ANDROID_NDK_URL "https://dl.google.com/android/repository/android-ndk-r27d-linux.zip")
    set(ANDROID_NDK_ZIP "${CMAKE_BINARY_DIR}/android-ndk-r27d-linux.zip")
    set(ANDROID_NDK_EXTRACT_DIR "$ENV{HOME}/Android")
    
    # Create Android directory if it doesn't exist
    file(MAKE_DIRECTORY "${ANDROID_NDK_EXTRACT_DIR}")
    
    # Download NDK
    file(DOWNLOAD
      ${ANDROID_NDK_URL}
      ${ANDROID_NDK_ZIP}
      SHOW_PROGRESS
      STATUS download_status
    )
    
    list(GET download_status 0 status_code)
    list(GET download_status 1 status_message)
    
    if(NOT status_code EQUAL 0)
      message(FATAL_ERROR "Failed to download Android NDK: ${status_message}")
    endif()
    
    # Extract NDK
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E tar xzf "${ANDROID_NDK_ZIP}"
      WORKING_DIRECTORY "${ANDROID_NDK_EXTRACT_DIR}"
      RESULT_VARIABLE extract_result
    )
    
    if(NOT extract_result EQUAL 0)
      message(FATAL_ERROR "Failed to extract Android NDK")
    endif()
    
    # Rename extracted folder to NDK
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E rename "android-ndk-r27d" "NDK"
      WORKING_DIRECTORY "${ANDROID_NDK_EXTRACT_DIR}"
      RESULT_VARIABLE rename_result
    )
    
    if(NOT rename_result EQUAL 0)
      message(FATAL_ERROR "Failed to rename Android NDK directory")
    endif()
    
    # Clean up downloaded zip
    file(REMOVE "${ANDROID_NDK_ZIP}")
    
    message(STATUS "Android NDK successfully installed to ${ANDROID_NDK_PATH}")
  else()
    message(STATUS "Android NDK found at ${ANDROID_NDK_PATH}")
  endif()
  
  # Set NDK path for CMake
  set(ANDROID_NDK "${ANDROID_NDK_PATH}" CACHE PATH "Path to Android NDK" FORCE)
endif()

# ======================================================
# ZLIB Setup - Simplified Approach
# ======================================================
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
  message(STATUS "ZLIB not found, downloading and building...")
  include(FetchContent)

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

if(NOT CURL_FOUND)
  message(STATUS "Falling back to bundled libcurl and mbedTLS")
  set(APM_USE_SYSTEM_CURL OFF CACHE BOOL "Prefer system-installed libcurl" FORCE)
  include(FetchContent)

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

# ======================================================
# APM Client Binary
# ======================================================
add_executable(apm
    src/apm/apm.cpp
    src/apm/ipc_client.cpp
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
    src/ams/module_info.cpp
    src/ams/module_manager.cpp
    src/util/crypto/sha256.cpp
    src/util/crypto/gpg_verify.cpp

    src/util/fs.cpp
    src/util/crypto/sha256.cpp
)

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

target_link_libraries(apm PRIVATE CURL::libcurl ZLIB::ZLIB)

# ======================================================
# APM Daemon Binary
# ======================================================
add_executable(apmd
    src/apmd/apmd.cpp
    src/apmd/ipc_server.cpp
    src/apmd/protocol.cpp
    src/apmd/install_manager.cpp
    src/apmd/export_path.cpp
    src/apmd/apk_installer.cpp

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
    src/ams/module_info.cpp
    src/ams/module_manager.cpp
    src/util/crypto/sha256.cpp
    src/util/crypto/gpg_verify.cpp

    src/util/fs.cpp
    src/util/crypto/sha256.cpp
)

target_include_directories(apmd PRIVATE
    src/apmd/include
    src/ams/include
    src/core/include
    src/util/include
    src/util/crypto
    src/apm/commands/include
    include
)

target_link_libraries(apmd PRIVATE CURL::libcurl ZLIB::ZLIB)
