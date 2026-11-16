# Native codecs keep extraction independent of the compression tools (and
# compression features of tar) installed on the Android device.
function(apm_add_archive_codecs)
  set(BUILD_SHARED_LIBS OFF)
  set(BUILD_TESTING OFF)
  set(XZ_THREADS no)
  set(XZ_NLS OFF)
  # Only liblzma is built; command-line sandbox probes are irrelevant here.
  set(XZ_SANDBOX no)
  set(XZ_TOOL_XZ OFF)
  set(XZ_TOOL_XZDEC OFF)
  set(XZ_TOOL_LZMADEC OFF)
  set(XZ_TOOL_LZMAINFO OFF)
  set(XZ_TOOL_SCRIPTS OFF)
  FetchContent_Declare(apm_xz
    URL https://github.com/tukaani-project/xz/releases/download/v5.8.4/xz-5.8.4.tar.gz
    URL_HASH SHA256=0014c7886930454fe8bd4228665b51af55eeae560ea135c9c4cd33f55b2591d9
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(apm_xz)
  # Upstream exposes these only to its own targets in build trees.
  target_include_directories(liblzma SYSTEM INTERFACE
    "$<BUILD_INTERFACE:${apm_xz_SOURCE_DIR}/src/liblzma/api>")

  set(ZSTD_BUILD_PROGRAMS OFF)
  set(ZSTD_BUILD_TESTS OFF)
  set(ZSTD_BUILD_SHARED OFF)
  set(ZSTD_BUILD_STATIC ON)
  set(ZSTD_BUILD_DICTBUILDER OFF)
  set(ZSTD_LEGACY_SUPPORT OFF)
  set(ZSTD_MULTITHREAD_SUPPORT OFF)
  FetchContent_Declare(apm_zstd
    URL https://github.com/meta/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR build/cmake)
  FetchContent_MakeAvailable(apm_zstd)

  FetchContent_Declare(apm_bzip2
    URL https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz
    URL_HASH SHA256=ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(apm_bzip2)
  add_library(apm_bzip2_codec STATIC
    ${apm_bzip2_SOURCE_DIR}/blocksort.c
    ${apm_bzip2_SOURCE_DIR}/huffman.c
    ${apm_bzip2_SOURCE_DIR}/crctable.c
    ${apm_bzip2_SOURCE_DIR}/randtable.c
    ${apm_bzip2_SOURCE_DIR}/compress.c
    ${apm_bzip2_SOURCE_DIR}/decompress.c
    ${apm_bzip2_SOURCE_DIR}/bzlib.c)
  target_include_directories(apm_bzip2_codec SYSTEM PUBLIC ${apm_bzip2_SOURCE_DIR})

  add_library(apm_archive_codecs INTERFACE)
  target_link_libraries(apm_archive_codecs INTERFACE
    ZLIB::ZLIB liblzma libzstd_static apm_bzip2_codec)
endfunction()
apm_add_archive_codecs()
