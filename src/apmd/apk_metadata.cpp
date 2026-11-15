// SPDX-License-Identifier: GPL-3.0-or-later
// APM - Android Package Manager

#include "apk_metadata.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <zlib.h>

namespace apm::apk {
namespace {
using Bytes = std::vector<unsigned char>;
constexpr std::size_t kManifestLimit = 4 * 1024 * 1024;
[[noreturn]] void bad(const std::string &message) {
  throw std::runtime_error(message);
}
std::uint16_t u16(const Bytes &v, std::size_t at) {
  if (at > v.size() || v.size() - at < 2)
    bad("Truncated APK data");
  return v[at] | (static_cast<std::uint16_t>(v[at + 1]) << 8);
}
std::uint32_t u32(const Bytes &v, std::size_t at) {
  return u16(v, at) | (static_cast<std::uint32_t>(u16(v, at + 2)) << 16);
}
Bytes read(std::ifstream &file, std::uint64_t at, std::size_t count) {
  Bytes out(count);
  file.clear();
  file.seekg(static_cast<std::streamoff>(at));
  if (!file.read(reinterpret_cast<char *>(out.data()),
                 static_cast<std::streamsize>(count)))
    bad("Cannot read APK archive");
  return out;
}
Bytes manifest(std::ifstream &file) {
  file.seekg(0, std::ios::end);
  auto length = file.tellg();
  if (length < 22 || length > 1024LL * 1024 * 1024)
    bad("Invalid APK archive size");
  const auto total = static_cast<std::uint64_t>(length);
  auto tail =
      read(file, total - std::min<std::uint64_t>(total, 65557),
           static_cast<std::size_t>(std::min<std::uint64_t>(total, 65557)));
  std::size_t end = tail.size() - 22;
  while (u32(tail, end) != 0x06054b50 ||
         end + 22 + u16(tail, end + 20) != tail.size()) {
    if (!end)
      bad("APK ZIP directory not found");
    --end;
  }
  if (u16(tail, end + 4) || u16(tail, end + 6) ||
      u16(tail, end + 8) != u16(tail, end + 10))
    bad("Multi-disk APK archives are unsupported");
  const auto count = u16(tail, end + 10);
  const std::uint64_t centralSize = u32(tail, end + 12),
                      centralOffset = u32(tail, end + 16);
  if (count == 0xffff || centralSize > total ||
      centralOffset > total - centralSize)
    bad("Invalid APK ZIP directory");
  std::uint64_t pos = centralOffset;
  Bytes result;
  bool found = false;
  for (unsigned i = 0; i < count; ++i) {
    if (pos > centralOffset + centralSize ||
        centralOffset + centralSize - pos < 46)
      bad("Truncated APK ZIP directory");
    const auto entry = read(file, pos, 46);
    if (u32(entry, 0) != 0x02014b50)
      bad("Invalid APK ZIP entry");
    const auto nameSize = u16(entry, 28), extra = u16(entry, 30),
               comment = u16(entry, 32);
    const std::uint64_t next = pos + 46 + nameSize + extra + comment;
    if (next > centralOffset + centralSize)
      bad("APK ZIP entry exceeds directory");
    const auto name = read(file, pos + 46, nameSize);
    if (std::string(name.begin(), name.end()) == "AndroidManifest.xml") {
      if (found)
        bad("APK contains duplicate manifests");
      found = true;
      const auto packed = u32(entry, 20), unpacked = u32(entry, 24),
                 localOffset = u32(entry, 42);
      const auto method = u16(entry, 10);
      if ((u16(entry, 8) & 1) || !unpacked || unpacked > kManifestLimit ||
          packed > kManifestLimit * 2 || (method != 0 && method != 8))
        bad("Unsupported or oversized APK manifest");
      if (localOffset > centralOffset || centralOffset - localOffset < 30)
        bad("Invalid APK manifest offset");
      const auto local = read(file, localOffset, 30);
      if (u32(local, 0) != 0x04034b50 || u16(local, 8) != method ||
          (u16(local, 6) & 1))
        bad("Invalid APK local entry");
      const std::uint64_t payload = static_cast<std::uint64_t>(localOffset) +
                                    30 + u16(local, 26) + u16(local, 28);
      if (payload > centralOffset || packed > centralOffset - payload)
        bad("APK manifest exceeds archive bounds");
      auto compressed = read(file, payload, packed);
      result.resize(unpacked);
      if (method == 0) {
        if (packed != unpacked)
          bad("Invalid stored manifest size");
        result = std::move(compressed);
      } else {
        z_stream stream{};
        stream.next_in = compressed.data();
        stream.avail_in = packed;
        stream.next_out = result.data();
        stream.avail_out = unpacked;
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
          bad("Cannot initialize APK decompression");
        const int status = inflate(&stream, Z_FINISH);
        const bool complete = status == Z_STREAM_END &&
                              stream.total_out == unpacked &&
                              stream.total_in == packed;
        inflateEnd(&stream);
        if (!complete)
          bad("Invalid compressed APK manifest");
      }
      if (crc32(0, result.data(), static_cast<uInt>(result.size())) !=
          u32(entry, 16))
        bad("APK manifest CRC mismatch");
    }
    pos = next;
  }
  if (!found)
    bad("APK has no AndroidManifest.xml");
  return result;
}
void appendUtf8(std::string &out, unsigned cp) {
  if (cp < 0x80)
    out += static_cast<char>(cp);
  else if (cp < 0x800) {
    out += static_cast<char>(0xc0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 63));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xe0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 63));
    out += static_cast<char>(0x80 | (cp & 63));
  } else {
    out += static_cast<char>(0xf0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 63));
    out += static_cast<char>(0x80 | ((cp >> 6) & 63));
    out += static_cast<char>(0x80 | (cp & 63));
  }
}
std::vector<std::string> stringPool(const Bytes &data) {
  const auto header = u16(data, 2);
  const auto count = u32(data, 8), flags = u32(data, 16), start = u32(data, 20);
  if (header < 28 || header > data.size() ||
      count > (data.size() - header) / 4 || start > data.size())
    bad("Invalid APK string pool");
  std::vector<std::string> out;
  out.reserve(count);
  for (unsigned i = 0; i < count; ++i) {
    std::size_t at =
        static_cast<std::size_t>(start) + u32(data, header + i * 4);
    std::string value;
    if (flags & 0x100) {
      auto len8 = [&]() -> unsigned {
        if (at >= data.size())
          bad("Invalid UTF-8 manifest string");
        unsigned n = data[at++];
        if (n & 0x80) {
          if (at == data.size())
            bad("Invalid manifest string length");
          n = ((n & 0x7f) << 8) | data[at++];
        }
        return n;
      };
      len8();
      const auto bytes = len8();
      if (at >= data.size() || bytes >= data.size() - at ||
          data[at + bytes] != 0)
        bad("Invalid manifest string bounds");
      value.assign(reinterpret_cast<const char *>(data.data() + at), bytes);
    } else {
      unsigned n = u16(data, at);
      at += 2;
      if (n & 0x8000) {
        n = ((n & 0x7fff) << 16) | u16(data, at);
        at += 2;
      }
      if (at > data.size() || n >= (data.size() - at) / 2 ||
          u16(data, at + n * 2) != 0)
        bad("Invalid UTF-16 manifest string");
      for (unsigned j = 0; j < n; ++j) {
        unsigned cp = u16(data, at + j * 2);
        if (cp >= 0xd800 && cp <= 0xdbff) {
          if (++j == n)
            bad("Invalid UTF-16 surrogate");
          const auto low = u16(data, at + j * 2);
          if (low < 0xdc00 || low > 0xdfff)
            bad("Invalid UTF-16 surrogate");
          cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
        } else if (cp >= 0xdc00 && cp <= 0xdfff)
          bad("Invalid UTF-16 surrogate");
        appendUtf8(value, cp);
      }
    }
    out.push_back(std::move(value));
  }
  return out;
}
bool packageId(const std::string &v) {
  if (v.empty() || v.size() > 255 || v.find('.') == std::string::npos)
    return false;
  bool start = true;
  for (unsigned char c : v) {
    if (c == '.') {
      if (start)
        return false;
      start = true;
    } else {
      if (!(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') && c != '_' &&
          !(c >= '0' && c <= '9' && !start))
        return false;
      start = false;
    }
  }
  return !start;
}
} // namespace

bool readApkMetadata(const std::string &path, ApkMetadata &out,
                     std::string *error) {
  out = {};
  try {
    std::ifstream file(path, std::ios::binary);
    if (!file)
      bad("Cannot open APK: " + path);
    const auto xml = manifest(file);
    if (u16(xml, 0) != 3 || u16(xml, 2) < 8 || u32(xml, 4) != xml.size())
      bad("APK manifest is not binary Android XML");
    std::vector<std::string> strings;
    auto text = [&](std::uint32_t index) -> const std::string & {
      if (index >= strings.size())
        bad("Invalid APK manifest string reference");
      return strings[index];
    };
    std::set<std::string> permissions;
    for (std::size_t pos = u16(xml, 2); pos < xml.size();) {
      const auto type = u16(xml, pos), header = u16(xml, pos + 2);
      const auto size = u32(xml, pos + 4);
      if (header < 8 || size < header || size > xml.size() - pos)
        bad("Invalid APK XML chunk");
      if (type == 1) {
        if (!strings.empty())
          bad("Duplicate APK string pool");
        strings =
            stringPool(Bytes(xml.begin() + pos, xml.begin() + pos + size));
      } else if (type == 0x0102) {
        const auto ext = pos + header;
        if (header < 16 || size - header < 20)
          bad("Invalid APK XML element");
        const auto element = text(u32(xml, ext + 4));
        const auto stride = u16(xml, ext + 10), count = u16(xml, ext + 12),
                   offset = u16(xml, ext + 8);
        if (offset < 20 || stride < 20 || offset > size - header ||
            static_cast<std::uint64_t>(count) * stride > size - header - offset)
          bad("Invalid APK XML attributes");
        for (unsigned i = 0; i < count; ++i) {
          const auto attr = ext + offset + i * stride;
          const auto name = text(u32(xml, attr + 4));
          const auto raw = u32(xml, attr + 8);
          if (raw == 0xffffffff && xml[attr + 15] != 3)
            continue;
          const auto value =
              text(raw == 0xffffffff ? u32(xml, attr + 16) : raw);
          if (element == "manifest" && name == "package" &&
              u32(xml, attr) == 0xffffffff) {
            if (!out.packageName.empty())
              bad("Duplicate APK package name");
            out.packageName = value;
          }
          if ((element == "uses-permission" ||
               element == "uses-permission-sdk-23") &&
              name == "name" && u32(xml, attr) != 0xffffffff &&
              text(u32(xml, attr)) ==
                  "http://schemas.android.com/apk/res/android")
            permissions.insert(value);
        }
      }
      pos += size;
    }
    if (!packageId(out.packageName))
      bad("APK manifest has no valid application package ID");
    out.requestedPermissions.assign(permissions.begin(), permissions.end());
    return true;
  } catch (const std::exception &e) {
    out = {};
    if (error)
      *error = e.what();
    return false;
  }
}
} // namespace apm::apk
