// SPDX-License-Identifier: GPL-3.0-or-later
#include "archive_decompressor.hpp"
#include "fs.hpp"

#include <array>
#include <bzlib.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <lzma.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <zlib.h>
#include <zstd.h>

namespace apm::archive {
namespace {
enum class Format { Plain, Gzip, Xz, Lzma, Bzip2, Zstd };
enum class Result { More, End, Error };
constexpr std::uint64_t kDecoderMemoryLimit = 256ULL * 1024 * 1024;
const char *name(Format format) {
  switch (format) {
  case Format::Gzip:
    return "gzip";
  case Format::Xz:
    return "XZ";
  case Format::Lzma:
    return "LZMA";
  case Format::Bzip2:
    return "bzip2";
  case Format::Zstd:
    return "Zstandard";
  default:
    return "tar";
  }
}
bool fail(std::string *error, const std::string &message) {
  if (error)
    *error = message;
  return false;
}

class Decoder {
public:
  explicit Decoder(Format format) : format_(format) {}
  ~Decoder() { clear(); }
  Decoder(const Decoder &) = delete;
  Decoder &operator=(const Decoder &) = delete;
  std::string error;
  bool start() {
    clear();
    error.clear();
    switch (format_) {
    case Format::Gzip:
      gzip_ = {};
      initialized_ = inflateInit2(&gzip_, 15 + 16) == Z_OK;
      break;
    case Format::Xz:
    case Format::Lzma: {
      lzma_ = LZMA_STREAM_INIT;
      const auto result = format_ == Format::Xz
                              ? lzma_stream_decoder(&lzma_, kDecoderMemoryLimit,
                                                    LZMA_CONCATENATED)
                              : lzma_alone_decoder(&lzma_, kDecoderMemoryLimit);
      initialized_ = result == LZMA_OK;
      break;
    }
    case Format::Bzip2:
      bzip_ = {};
      initialized_ = BZ2_bzDecompressInit(&bzip_, 0, 0) == BZ_OK;
      break;
    case Format::Zstd:
      zstd_ = ZSTD_createDStream();
      if (zstd_) {
        const auto status = ZSTD_initDStream(zstd_);
        initialized_ = !ZSTD_isError(status);
        if (initialized_)
          initialized_ = !ZSTD_isError(
              ZSTD_DCtx_setParameter(zstd_, ZSTD_d_windowLogMax, 28));
      }
      break;
    default:
      break;
    }
    if (!initialized_)
      error = "cannot initialize decoder";
    return initialized_;
  }
  Result decode(const unsigned char *&input, std::size_t &remaining,
                unsigned char *output, std::size_t capacity,
                std::size_t &produced, bool eof) {
    produced = 0;
    switch (format_) {
    case Format::Gzip: {
      gzip_.next_in = const_cast<Bytef *>(input);
      gzip_.avail_in = static_cast<uInt>(remaining);
      gzip_.next_out = output;
      gzip_.avail_out = static_cast<uInt>(capacity);
      const int status = inflate(&gzip_, eof ? Z_FINISH : Z_NO_FLUSH);
      input = gzip_.next_in;
      remaining = gzip_.avail_in;
      produced = capacity - gzip_.avail_out;
      if (status == Z_STREAM_END)
        return Result::End;
      if (status == Z_OK || status == Z_BUF_ERROR)
        return Result::More;
      error = gzip_.msg ? gzip_.msg : "invalid gzip stream";
      return Result::Error;
    }
    case Format::Xz:
    case Format::Lzma: {
      lzma_.next_in = input;
      lzma_.avail_in = remaining;
      lzma_.next_out = output;
      lzma_.avail_out = capacity;
      const auto status = lzma_code(&lzma_, eof ? LZMA_FINISH : LZMA_RUN);
      input = lzma_.next_in;
      remaining = lzma_.avail_in;
      produced = capacity - lzma_.avail_out;
      if (status == LZMA_STREAM_END)
        return Result::End;
      if (status == LZMA_OK || status == LZMA_BUF_ERROR)
        return Result::More;
      error = status == LZMA_MEMLIMIT_ERROR
                  ? "decoder memory limit exceeded (256 MiB)"
                  : "invalid or unsupported stream (decoder status " +
                        std::to_string(status) + ")";
      return Result::Error;
    }
    case Format::Bzip2: {
      bzip_.next_in =
          reinterpret_cast<char *>(const_cast<unsigned char *>(input));
      bzip_.avail_in = static_cast<unsigned>(remaining);
      bzip_.next_out = reinterpret_cast<char *>(output);
      bzip_.avail_out = static_cast<unsigned>(capacity);
      const int status = BZ2_bzDecompress(&bzip_);
      input = reinterpret_cast<unsigned char *>(bzip_.next_in);
      remaining = bzip_.avail_in;
      produced = capacity - bzip_.avail_out;
      if (status == BZ_STREAM_END)
        return Result::End;
      if (status == BZ_OK)
        return Result::More;
      error = "invalid bzip2 stream (decoder status " + std::to_string(status) +
              ")";
      return Result::Error;
    }
    case Format::Zstd: {
      ZSTD_inBuffer in{input, remaining, 0};
      ZSTD_outBuffer out{output, capacity, 0};
      const auto status = ZSTD_decompressStream(zstd_, &out, &in);
      input += in.pos;
      remaining -= in.pos;
      produced = out.pos;
      if (ZSTD_isError(status)) {
        error = ZSTD_getErrorName(status);
        return Result::Error;
      }
      return status == 0 ? Result::End : Result::More;
    }
    default:
      error = "unknown compression";
      return Result::Error;
    }
  }

private:
  Format format_;
  bool initialized_ = false;
  z_stream gzip_{};
  lzma_stream lzma_ = LZMA_STREAM_INIT;
  bz_stream bzip_{};
  ZSTD_DStream *zstd_ = nullptr;
  void clear() {
    if (initialized_) {
      if (format_ == Format::Gzip)
        inflateEnd(&gzip_);
      if (format_ == Format::Bzip2)
        BZ2_bzDecompressEnd(&bzip_);
    }
    lzma_end(&lzma_);
    if (zstd_) {
      ZSTD_freeDStream(zstd_);
      zstd_ = nullptr;
    }
    initialized_ = false;
  }
};

Format detect(const std::array<unsigned char, 8> &magic, std::size_t size,
              const std::string &path) {
  if (size >= 2 && magic[0] == 0x1f && magic[1] == 0x8b)
    return Format::Gzip;
  if (size >= 6 && std::memcmp(magic.data(),
                               "\xfd"
                               "7zXZ\0",
                               6) == 0)
    return Format::Xz;
  if (size >= 3 && std::memcmp(magic.data(), "BZh", 3) == 0)
    return Format::Bzip2;
  if (size >= 4 && ((magic[0] == 0x28 && magic[1] == 0xb5 && magic[2] == 0x2f &&
                     magic[3] == 0xfd) ||
                    ((magic[0] & 0xf0) == 0x50 && magic[1] == 0x2a &&
                     magic[2] == 0x4d && magic[3] == 0x18)))
    return Format::Zstd;
  // The legacy .lzma-alone format has no unique magic.
  if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".lzma") == 0)
    return Format::Lzma;
  return Format::Plain;
}
} // namespace

PreparedTar::~PreparedTar() {
  if (!temporaryDirectory.empty())
    fs::removeDirRecursive(temporaryDirectory);
}

bool prepareTar(const std::string &inputPath, const std::string &cacheDirectory,
                std::uint64_t maxBytes, PreparedTar &output,
                std::string *error) {
  output.path.clear();
  if (!output.temporaryDirectory.empty())
    fs::removeDirRecursive(output.temporaryDirectory);
  output.temporaryDirectory.clear();
  std::ifstream input(inputPath, std::ios::binary);
  if (!input)
    return fail(error, "Cannot open archive: " + inputPath);
  std::array<unsigned char, 8> magic{};
  input.read(reinterpret_cast<char *>(magic.data()), magic.size());
  const auto count = input.gcount();
  if (input.bad())
    return fail(error, "Cannot read archive: " + inputPath);
  if (count == 8 && std::memcmp(magic.data(), "!<arch>\n", 8) == 0)
    return fail(error, "A .deb is an ar archive; extract its control/data "
                       "members before passing them to tar");
  const auto format = detect(magic, static_cast<std::size_t>(count), inputPath);
  if (format == Format::Plain) {
    output.path = inputPath;
    return true;
  }
  input.clear();
  input.seekg(0);
  if (!fs::createDirs(cacheDirectory))
    return fail(error,
                "Cannot create archive cache directory: " + cacheDirectory);
  const auto pattern = fs::joinPath(cacheDirectory, "extract-XXXXXX");
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  char *created = ::mkdtemp(buffer.data());
  if (!created)
    return fail(error, "Cannot create private decompression directory: " +
                           std::string(std::strerror(errno)));
  output.temporaryDirectory = created;
  const auto plainPath = fs::joinPath(output.temporaryDirectory, "archive.tar");
  std::ofstream plain(plainPath, std::ios::binary | std::ios::trunc);
  if (!plain || ::chmod(plainPath.c_str(), 0600) != 0)
    return fail(error, "Cannot create temporary tar file");
  Decoder decoder(format);
  if (!decoder.start())
    return fail(error, std::string(name(format)) + ": " + decoder.error);
  std::array<unsigned char, 65536> compressed{}, decoded{};
  const unsigned char *next = compressed.data();
  std::size_t remaining = 0;
  std::uint64_t written = 0;
  bool eof = false, finished = false;
  while (true) {
    if (!remaining && !eof) {
      input.read(reinterpret_cast<char *>(compressed.data()),
                 compressed.size());
      remaining = static_cast<std::size_t>(input.gcount());
      next = compressed.data();
      if (input.bad() || (!input && !input.eof()))
        return fail(error, "Cannot read compressed archive: " + inputPath);
      eof = input.eof();
    }
    if (finished) {
      // gzip permits zero padding after a stream. Keep accepting archives
      // produced by tools that pad compressed files to block boundaries.
      if (format == Format::Gzip) {
        while (remaining && *next == 0) {
          ++next;
          --remaining;
        }
        if (!remaining && !eof)
          continue;
      }
      if (!remaining && eof)
        break;
      if (!decoder.start())
        return fail(error, decoder.error);
      finished = false;
    }
    const auto before = remaining;
    std::size_t produced = 0;
    const auto status = decoder.decode(next, remaining, decoded.data(),
                                       decoded.size(), produced, eof);
    if (status == Result::Error)
      return fail(error, std::string(name(format)) +
                             " decompression failed: " + decoder.error);
    if (maxBytes && (written > maxBytes || produced > maxBytes - written))
      return fail(error,
                  "Decompressed archive exceeds the configured byte limit");
    plain.write(reinterpret_cast<const char *>(decoded.data()),
                static_cast<std::streamsize>(produced));
    if (!plain)
      return fail(error,
                  "Cannot write decompressed tar (check available storage)");
    written += produced;
    if (status == Result::End)
      finished = true;
    else if (!produced && before == remaining)
      return fail(error, std::string(name(format)) +
                             " archive is truncated or corrupt");
  }
  plain.close();
  if (!plain)
    return fail(error,
                "Cannot finish decompressed tar (check available storage)");
  output.path = plainPath;
  return true;
}
} // namespace apm::archive
