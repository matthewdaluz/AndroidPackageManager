/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: repo_index.cpp
 * Purpose: Implement sources.list parsing, repository index downloads, and
 * Packages parsing. Last Modified: November 22nd, 2025. - 10:30 PM Eastern
 * Time. Author: Matthew DaLuz - RedHead Founder
 *
 * APM is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * APM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with APM. If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "repo_index.hpp"
#include "config.hpp"
#include "downloader.hpp"
#include "fs.hpp"
#include "gpg_verify.hpp"
#include "logger.hpp"
#include "md5.hpp"
#include "release_parser.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <vector>
#include <zlib.h>

namespace apm::repo {

namespace {

using Clock = std::chrono::steady_clock;

} // namespace

static std::uint64_t safeFileSize(const std::string &path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0)
    return 0;
  if (st.st_size < 0)
    return 0;
  return static_cast<std::uint64_t>(st.st_size);
}

static void
emitRepoProgress(const RepoUpdateProgressCallback &cb, RepoUpdateStage stage,
                 const RepoSource *source, const std::string *component,
                 const std::string &remote, const std::string &local,
                 std::uint64_t current, std::uint64_t total, double dlSpeed,
                 double ulSpeed, bool finished, const std::string &desc) {
  if (!cb || !source)
    return;

  RepoUpdateProgress progress;
  progress.stage = stage;
  progress.repoUri = source->uri;
  progress.dist = source->dist;
  if (component)
    progress.component = *component;
  progress.remotePath = remote;
  progress.localPath = local;
  progress.description = desc;
  progress.currentBytes = current;
  progress.totalBytes = total;
  progress.downloadSpeed = dlSpeed;
  progress.uploadSpeed = ulSpeed;
  progress.finished = finished;

  cb(progress);
}

struct ParseProgressEmitter {
  RepoUpdateProgressCallback cb;
  const RepoSource *source = nullptr;
  const std::string *component = nullptr;
  std::string sourcePath;
  std::string destPath;
  std::string description;
  Clock::time_point lastTime = Clock::now();
  std::uint64_t lastBytes = 0;
};

static void emitParseProgress(ParseProgressEmitter &emitter,
                              std::uint64_t current, std::uint64_t total,
                              bool finished) {
  if (!emitter.cb || !emitter.source || !emitter.component)
    return;

  if (total == 0)
    total = 1;

  auto now = Clock::now();
  double dt = std::chrono::duration<double>(now - emitter.lastTime).count();
  if (dt <= 0.0)
    dt = 1e-6;

  double rate =
      (static_cast<double>(current) - static_cast<double>(emitter.lastBytes)) /
      dt;

  emitter.lastTime = now;
  emitter.lastBytes = current;

  RepoUpdateProgress progress;
  progress.stage = RepoUpdateStage::ParsePackages;
  progress.repoUri = emitter.source->uri;
  progress.dist = emitter.source->dist;
  progress.component = *emitter.component;
  progress.remotePath = emitter.sourcePath;
  progress.localPath = emitter.destPath;
  progress.description = emitter.description;
  progress.currentBytes = current > total ? total : current;
  progress.totalBytes = total;
  progress.downloadSpeed = rate;
  progress.uploadSpeed = 0.0;
  progress.finished = finished;

  emitter.cb(progress);
}

// ---------------------------------------------------------
// Basic string helpers
// ---------------------------------------------------------

static inline void ltrim(std::string &s) {
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
            return !std::isspace(ch);
          }));
}

static inline void rtrim(std::string &s) {
  s.erase(std::find_if(s.rbegin(), s.rend(),
                       [](unsigned char ch) { return !std::isspace(ch); })
              .base(),
          s.end());
}

static inline void trim(std::string &s) {
  ltrim(s);
  rtrim(s);
}

static inline std::string toLower(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

static std::vector<std::string> splitAndTrim(const std::string &input,
                                             char delim) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream ss(input);

  while (std::getline(ss, current, delim)) {
    trim(current);
    if (!current.empty()) {
      parts.push_back(current);
    }
  }

  return parts;
}

static bool containsCaseInsensitive(const std::string &text,
                                    const std::string &needle) {
  if (text.empty() || needle.empty())
    return false;

  auto it = std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                        [](unsigned char ch1, unsigned char ch2) {
                          return std::tolower(ch1) == std::tolower(ch2);
                        });
  return it != text.end();
}

static RepoFormat detectRepoFormat(const RepoSource &src,
                                   const std::string &rawLine) {
  constexpr const char *kMarker = "termux";

  if (containsCaseInsensitive(src.uri, kMarker) ||
      containsCaseInsensitive(src.dist, kMarker) ||
      containsCaseInsensitive(rawLine, kMarker)) {
    return RepoFormat::Termux;
  }

  return RepoFormat::Debian;
}

static bool detectTermuxRepo(const RepoSource &src,
                             const std::string &rawLine) {
  if (containsCaseInsensitive(src.uri, "packages.termux.dev")) {
    return true;
  }

  if (containsCaseInsensitive(rawLine, "data/data/com.termux/files/usr")) {
    return true;
  }

  return false;
}

static std::string mapArchToTermux(const std::string &arch) {
  if (arch.empty())
    return arch;

  std::string lower = arch;
  std::transform(
      lower.begin(), lower.end(), lower.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (lower == "arm64" || lower == "aarch64")
    return "aarch64";
  if (lower == "armhf" || lower == "arm")
    return "arm";
  if (lower == "amd64" || lower == "x86_64")
    return "x86_64";
  if (lower == "i386" || lower == "i686" || lower == "x86")
    return "i686";
  if (lower == "all")
    return "all";

  return arch;
}

static std::string resolveRepoArch(const RepoSource &src,
                                   const std::string &defaultArch) {
  std::string arch = src.arch.empty() ? defaultArch : src.arch;

  if (src.format == RepoFormat::Termux || src.isTermuxRepo) {
    std::string termuxArch = mapArchToTermux(arch);
    if (termuxArch != arch) {
      apm::logger::info("resolveRepoArch: mapping Debian arch '" + arch +
                        "' to Termux arch '" + termuxArch + "'");
    }
    arch = termuxArch;
  }

  return arch;
}

static bool isDirectoryPath(const std::string &path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) {
    return false;
  }
  return S_ISDIR(st.st_mode);
}

static bool hasSuffix(const std::string &s, const std::string &suffix) {
  if (s.size() < suffix.size())
    return false;
  return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// ---------------------------------------------------------
// Depends parsing
// ---------------------------------------------------------

static std::vector<std::string>
parseDependsField(const std::string &dependsRaw) {
  std::vector<std::string> result;
  if (dependsRaw.empty()) {
    return result;
  }

  auto groups = splitAndTrim(dependsRaw, ',');

  for (const auto &group : groups) {
    if (group.empty())
      continue;

    std::string firstAlt = group;
    auto pipePos = firstAlt.find('|');
    if (pipePos != std::string::npos) {
      firstAlt = firstAlt.substr(0, pipePos);
      trim(firstAlt);
    }

    std::string name;
    for (char ch : firstAlt) {
      if (std::isspace(static_cast<unsigned char>(ch)) || ch == '(') {
        break;
      }
      name.push_back(ch);
    }

    trim(name);
    if (name.empty()) {
      continue;
    }

    // Multi-arch dependencies may look like "libc6:any"; only keep the
    // package portion so we can match it against repository entries.
    auto colonPos = name.find(':');
    if (colonPos != std::string::npos) {
      name = name.substr(0, colonPos);
      trim(name);
      if (name.empty()) {
        continue;
      }
    }

    result.push_back(name);
  }

  return result;
}

static bool hasTermuxDependency(const std::vector<std::string> &deps) {
  static const std::vector<std::string> kTermuxDeps = {
      "libandroid-glob", "libandroid-support", "termux-tools"};

  for (const auto &dep : deps) {
    for (const auto &needle : kTermuxDeps) {
      if (containsCaseInsensitive(dep, needle)) {
        return true;
      }
    }
  }

  return false;
}

static bool detectTermuxPackage(const PackageEntry &pkg) {
  if (containsCaseInsensitive(pkg.filename, "data/data/com.termux/files/usr")) {
    return true;
  }

  if (hasTermuxDependency(pkg.depends)) {
    return true;
  }

  return false;
}

// ---------------------------------------------------------
// RFC822-like stanza parser (Packages format)
// ---------------------------------------------------------

static void parseStanzas(
    const std::string &content,
    std::vector<std::unordered_map<std::string, std::string>> &outStanzas) {
  outStanzas.clear();

  std::istringstream in(content);
  std::string line;
  std::unordered_map<std::string, std::string> fields;
  std::string currentKey;

  auto flushCurrent = [&]() {
    if (!fields.empty()) {
      outStanzas.push_back(fields);
      fields.clear();
      currentKey.clear();
    }
  };

  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    if (line.empty()) {
      flushCurrent();
      continue;
    }

    if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
      if (!currentKey.empty()) {
        std::string value = line;
        trim(value);
        auto it = fields.find(currentKey);
        if (it != fields.end()) {
          it->second.append("\n");
          it->second.append(value);
        }
      }
      continue;
    }

    auto colonPos = line.find(':');
    if (colonPos == std::string::npos) {
      continue;
    }

    std::string key = line.substr(0, colonPos);
    std::string value = line.substr(colonPos + 1);

    trim(key);
    trim(value);

    if (!key.empty()) {
      currentKey = key;
      fields[key] = value;
    }
  }

  flushCurrent();
}

// ---------------------------------------------------------
// Packages parsing API
// ---------------------------------------------------------

// Parse the textual Packages file content into strongly typed PackageEntry
// structs.
bool parsePackagesString(const std::string &content, PackageList &out,
                         std::string * /*errorMsg*/) {
  out.clear();

  if (content.empty()) {
    return true;
  }

  std::vector<std::unordered_map<std::string, std::string>> stanzas;
  parseStanzas(content, stanzas);

  for (const auto &fields : stanzas) {
    PackageEntry pkg;
    pkg.rawFields = fields;

    auto getField = [&](const std::string &key) -> std::string {
      auto it = fields.find(key);
      if (it != fields.end()) {
        return it->second;
      }
      return {};
    };

    pkg.packageName = getField("Package");
    pkg.version = getField("Version");
    pkg.architecture = getField("Architecture");
    pkg.filename = getField("Filename");
    pkg.sha256 = getField("SHA256");

    const std::string dependsRaw = getField("Depends");
    pkg.depends = parseDependsField(dependsRaw);
    pkg.isTermuxPackage = detectTermuxPackage(pkg);

    if (pkg.packageName.empty()) {
      continue;
    }

    out.push_back(std::move(pkg));
  }

  apm::logger::info("parsePackagesString: parsed " +
                    std::to_string(out.size()) + " package entries");
  return true;
}

// Parse a Packages file on disk into a vector of PackageEntry structs.
bool parsePackagesFile(const std::string &path, PackageList &out,
                       std::string *errorMsg) {
  std::string content;
  if (!apm::fs::readFile(path, content)) {
    if (errorMsg)
      *errorMsg = "Failed to read Packages file: " + path;
    apm::logger::error("parsePackagesFile: cannot read " + path);
    return false;
  }

  return parsePackagesString(content, out, errorMsg);
}

// Locate the first PackageEntry that matches the given name/arch combination.
const PackageEntry *findPackage(const PackageList &list,
                                const std::string &name,
                                const std::string &arch) {
  if (name.empty())
    return nullptr;

  const PackageEntry *fallbackAll = nullptr;

  for (const auto &pkg : list) {
    if (pkg.packageName != name)
      continue;

    if (!arch.empty()) {
      if (pkg.architecture == arch) {
        return &pkg;
      }
      if (pkg.architecture == "all" && fallbackAll == nullptr) {
        fallbackAll = &pkg;
      }
    } else {
      return &pkg;
    }
  }

  if (fallbackAll) {
    return fallbackAll;
  }

  return nullptr;
}

// ---------------------------------------------------------
// sources.list parsing
// ---------------------------------------------------------

// Parse one sources.list-style file into RepoSourceList (append).
static bool parseSingleSourcesFile(const std::string &path,
                                   RepoSourceList &out) {
  std::string content;
  if (!apm::fs::readFile(path, content)) {
    apm::logger::error("loadSourcesList: cannot read " + path);
    return false;
  }

  std::istringstream in(content);
  std::string line;
  size_t lineNo = 0;
  bool any = false;

  while (std::getline(in, line)) {
    ++lineNo;

    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    std::string trimmed = line;
    trim(trimmed);

    if (trimmed.empty())
      continue;
    if (!trimmed.empty() && trimmed[0] == '#')
      continue;

    std::istringstream ls(trimmed);

    std::string type;
    if (!(ls >> type)) {
      continue;
    }

    RepoSource src;
    src.type = type;

    std::string token;
    if (!(ls >> token)) {
      apm::logger::warn("loadSourcesList: malformed line " +
                        std::to_string(lineNo) + " in " + path);
      continue;
    }

    std::string uri;
    std::string dist;

    // Optional "[...]" options block (we care about arch=)
    if (!token.empty() && token[0] == '[') {
      std::string options = token;
      if (options.back() != ']') {
        std::string t;
        while (ls >> t) {
          options.push_back(' ');
          options.append(t);
          if (!t.empty() && t.back() == ']')
            break;
        }
      }

      // Strip [ ]
      if (!options.empty() && options.front() == '[')
        options.erase(0, 1);
      if (!options.empty() && options.back() == ']')
        options.pop_back();

      std::istringstream os(options);
      std::string opt;
      while (os >> opt) {
        auto eqPos = opt.find('=');
        if (eqPos == std::string::npos)
          continue;

        std::string key = opt.substr(0, eqPos);
        std::string val = opt.substr(eqPos + 1);
        trim(key);
        trim(val);

        std::string keyLower = key;
        std::transform(
            keyLower.begin(), keyLower.end(), keyLower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (keyLower == "arch" || keyLower == "architectures") {
          // Take first architecture if multiple
          auto commaPos = val.find(',');
          if (commaPos != std::string::npos) {
            val = val.substr(0, commaPos);
          }
          src.arch = val;
        } else if (keyLower == "trusted") {
          std::string valLower = val;
          std::transform(valLower.begin(), valLower.end(), valLower.begin(),
                         [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                         });

          if (valLower == "required") {
            src.trustPolicy = RepoTrustPolicy::Require;
          } else if (valLower == "yes" || valLower == "true" ||
                     valLower == "1") {
            src.trustPolicy = RepoTrustPolicy::Skip;
          } else {
            apm::logger::warn("loadSourcesList: unknown trusted value '" + val +
                              "' on line " + std::to_string(lineNo) + " in " +
                              path + " (defaulting to verification)");
            src.trustPolicy = RepoTrustPolicy::Default;
          }
        }
      }

      if (!(ls >> uri >> dist)) {
        apm::logger::warn("loadSourcesList: missing URI/dist on line " +
                          std::to_string(lineNo) + " in " + path);
        continue;
      }
    } else {
      // No options: token is URI
      uri = token;
      if (!(ls >> dist)) {
        apm::logger::warn("loadSourcesList: missing dist on line " +
                          std::to_string(lineNo) + " in " + path);
        continue;
      }
    }

    src.uri = uri;
    src.dist = dist;

    std::string component;
    while (ls >> component) {
      src.components.push_back(component);
    }

    src.format = detectRepoFormat(src, trimmed);
    src.isTermuxRepo =
        (src.format == RepoFormat::Termux) || detectTermuxRepo(src, trimmed);
    if (src.format == RepoFormat::Termux) {
      apm::logger::info("loadSourcesList: detected Termux repository: " +
                        src.uri);
    } else if (src.isTermuxRepo) {
      apm::logger::info("loadSourcesList: flagged Termux repository: " +
                        src.uri);
    }

    if (src.type != "deb") {
      // Ignore deb-src etc for now
      continue;
    }
    if (src.uri.empty() || src.dist.empty() || src.components.empty()) {
      apm::logger::warn("loadSourcesList: incomplete entry on line " +
                        std::to_string(lineNo) + " in " + path);
      continue;
    }

    out.push_back(std::move(src));
    any = true;
  }

  if (!any) {
    apm::logger::warn("loadSourcesList: no valid entries in " + path);
  } else {
    apm::logger::info("loadSourcesList: loaded entries from " + path);
  }

  return any;
}

// Parse either a single sources.list file or a directory containing
// sources.list + sources.list.d/*.list.
bool loadSourcesList(const std::string &path, RepoSourceList &out,
                     std::string *errorMsg) {
  out.clear();

  if (isDirectoryPath(path)) {
    // Directory mode: mimic /etc/apt
    std::string mainFile = apm::fs::joinPath(path, "sources.list");
    std::string dDir = apm::fs::joinPath(path, "sources.list.d");

    bool any = false;

    if (apm::fs::pathExists(mainFile)) {
      if (parseSingleSourcesFile(mainFile, out)) {
        any = true;
      }
    }

    if (isDirectoryPath(dDir)) {
      auto files = apm::fs::listDir(dDir, false);
      for (const auto &name : files) {
        if (!hasSuffix(name, ".list"))
          continue;
        std::string full = apm::fs::joinPath(dDir, name);
        if (parseSingleSourcesFile(full, out)) {
          any = true;
        }
      }
    }

    if (!any) {
      if (errorMsg) {
        *errorMsg = "No valid sources in directory: " + path;
      }
      apm::logger::warn("loadSourcesList: no valid sources in dir " + path);
      return false;
    }

    apm::logger::info("loadSourcesList: total " + std::to_string(out.size()) +
                      " sources loaded from dir " + path);
    return true;
  }

  // Single file mode
  if (!parseSingleSourcesFile(path, out)) {
    if (errorMsg) {
      *errorMsg = "No valid sources in " + path;
    }
    return false;
  }

  return true;
}

// ---------------------------------------------------------
// Indices download & mapping
// ---------------------------------------------------------

static std::string sanitizeForFilename(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
        c == '.') {
      out.push_back(c);
    } else {
      out.push_back('_');
    }
  }
  return out;
}

static std::string makePackagesFilename(const RepoSource &src,
                                        const std::string &component,
                                        const std::string &arch,
                                        const std::string &listsDir) {
  std::string key = src.uri + "_" + src.dist + "_" + component + "_" + arch;
  key = sanitizeForFilename(key);
  return apm::fs::joinPath(listsDir, key + "_Packages");
}

static std::string makePackagesExtractDir(const std::string &fileBase) {
  return fileBase + "_dir";
}

static std::string makePackagesPlainPath(const std::string &fileBase) {
  return apm::fs::joinPath(makePackagesExtractDir(fileBase), "Packages");
}

static bool ensureFreshPackagesDir(const std::string &dir) {
  if (apm::fs::pathExists(dir)) {
    if (!apm::fs::removeDirRecursive(dir)) {
      apm::logger::error("ensureFreshPackagesDir: failed to remove " + dir);
      return false;
    }
  }

  if (!apm::fs::createDirs(dir)) {
    apm::logger::error("ensureFreshPackagesDir: failed to create " + dir);
    return false;
  }

  return true;
}

static bool copyPackagesFile(const std::string &src, const std::string &dst,
                             std::string *errorMsg,
                             ParseProgressEmitter *progressEmitter = nullptr) {
  std::ifstream in(src, std::ios::binary);
  if (!in.is_open()) {
    if (errorMsg) {
      *errorMsg = "cannot open source '" + src + "': " + std::strerror(errno);
    }
    return false;
  }

  std::ofstream out(dst, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    if (errorMsg) {
      *errorMsg =
          "cannot open destination '" + dst + "': " + std::strerror(errno);
    }
    return false;
  }

  const std::uint64_t total = std::max<std::uint64_t>(safeFileSize(src), 1);
  std::uint64_t copied = 0;
  std::vector<char> buffer(64 * 1024);

  while (in) {
    in.read(buffer.data(), buffer.size());
    std::streamsize got = in.gcount();
    if (got <= 0)
      break;
    out.write(buffer.data(), got);
    if (!out.good()) {
      if (errorMsg) {
        *errorMsg = "failed to write to '" + dst + "'";
      }
      return false;
    }
    copied += static_cast<std::uint64_t>(got);
    if (progressEmitter) {
      emitParseProgress(*progressEmitter,
                        std::min<std::uint64_t>(copied, total), total, false);
    }
  }

  if (!in.eof() && in.fail()) {
    if (errorMsg) {
      *errorMsg = "failed to read from '" + src + "'";
    }
    return false;
  }

  if (progressEmitter) {
    emitParseProgress(*progressEmitter, total, total, true);
  }

  return true;
}

static bool decompressGzipFile(const std::string &src, const std::string &dst,
                               std::string *errorMsg,
                               ParseProgressEmitter *progressEmitter) {
  std::ifstream in(src, std::ios::binary);
  if (!in.is_open()) {
    if (errorMsg) {
      *errorMsg = "cannot open source '" + src + "': " + std::strerror(errno);
    }
    return false;
  }

  std::ofstream out(dst, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    if (errorMsg) {
      *errorMsg =
          "cannot open destination '" + dst + "': " + std::strerror(errno);
    }
    return false;
  }

  z_stream strm{};
  strm.zalloc = Z_NULL;
  strm.zfree = Z_NULL;
  strm.opaque = Z_NULL;

  if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) {
    if (errorMsg) {
      *errorMsg = "inflateInit2() failed for '" + src + "'";
    }
    return false;
  }

  const size_t kChunk = 64 * 1024;
  std::vector<unsigned char> inBuf(kChunk);
  std::vector<unsigned char> outBuf(kChunk);
  std::uint64_t processed = 0;
  const std::uint64_t total = std::max<std::uint64_t>(safeFileSize(src), 1);
  bool reachedEnd = false;
  bool ok = true;

  while (ok && !reachedEnd) {
    in.read(reinterpret_cast<char *>(inBuf.data()), kChunk);
    std::streamsize have = in.gcount();
    if (have <= 0) {
      if (in.eof()) {
        if (!reachedEnd) {
          ok = false;
          if (errorMsg) {
            *errorMsg = "unexpected EOF while reading '" + src + "'";
          }
        }
      } else {
        ok = false;
        if (errorMsg) {
          *errorMsg = "failed to read from '" + src + "'";
        }
      }
      break;
    }

    strm.next_in = inBuf.data();
    strm.avail_in = static_cast<uInt>(have);
    processed += static_cast<std::uint64_t>(have);

    while (strm.avail_in > 0) {
      strm.next_out = outBuf.data();
      strm.avail_out = kChunk;

      int ret = inflate(&strm, Z_NO_FLUSH);
      if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
        ok = false;
        if (errorMsg) {
          *errorMsg = "inflate() failed for '" + src + "'";
        }
        break;
      }

      size_t produced = kChunk - strm.avail_out;
      if (produced > 0) {
        out.write(reinterpret_cast<const char *>(outBuf.data()), produced);
        if (!out.good()) {
          ok = false;
          if (errorMsg) {
            *errorMsg = "failed to write to '" + dst + "'";
          }
          break;
        }
      }

      if (ret == Z_STREAM_END) {
        reachedEnd = true;
        break;
      }
    }

    if (progressEmitter) {
      emitParseProgress(*progressEmitter,
                        std::min<std::uint64_t>(processed, total), total,
                        false);
    }

    if (!ok || reachedEnd)
      break;
  }

  int endRet = inflateEnd(&strm);
  if (ok && endRet != Z_OK) {
    ok = false;
    if (errorMsg) {
      *errorMsg = "inflateEnd() failed for '" + src + "'";
    }
  }

  if (progressEmitter) {
    emitParseProgress(*progressEmitter, total, total, true);
  }

  if (!ok || !reachedEnd) {
    return false;
  }

  return true;
}

static bool preparePackagesTextFile(
    const std::string &fileBase, const std::string &sourcePath,
    const std::string &suffix, std::string &outPlainPath,
    const RepoSource *source = nullptr, const std::string *component = nullptr,
    RepoUpdateProgressCallback progressCb = {}) {
  const std::string targetDir = makePackagesExtractDir(fileBase);
  if (!ensureFreshPackagesDir(targetDir)) {
    return false;
  }

  const std::string destPath = makePackagesPlainPath(fileBase);
  std::string err;
  bool ok = false;
  ParseProgressEmitter emitter;
  if (progressCb && source && component) {
    emitter.cb = progressCb;
    emitter.source = source;
    emitter.component = component;
    emitter.sourcePath = sourcePath;
    emitter.destPath = destPath;
    emitter.description = "Parsing " + *component + " (" + source->dist + ")";
  }

  ParseProgressEmitter *emitterPtr =
      (progressCb && source && component) ? &emitter : nullptr;

  if (suffix == ".xz") {
    err = "Packages.xz indexes are disabled on Android - expecting Packages.gz";
    ok = false;
  } else if (suffix == ".gz") {
    ok = decompressGzipFile(sourcePath, destPath, &err, emitterPtr);
  } else {
    ok = copyPackagesFile(sourcePath, destPath, &err, emitterPtr);
  }

  if (!ok) {
    apm::logger::warn("preparePackagesTextFile: unable to unpack " +
                      sourcePath + (err.empty() ? "" : (": " + err)));
    return false;
  }

  apm::logger::info("Prepared Packages text: " + destPath);
  outPlainPath = destPath;
  return true;
}

static std::string makeDistBaseUrl(const RepoSource &src) {
  std::string url = src.uri;
  if (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  url += "/dists/";
  url += src.dist;
  return url;
}

static std::string makeReleaseUrl(const RepoSource &src) {
  return makeDistBaseUrl(src) + "/Release";
}

static std::string makeInReleaseUrl(const RepoSource &src) {
  return makeDistBaseUrl(src) + "/InRelease";
}

static std::string makeReleaseGpgUrl(const RepoSource &src) {
  return makeDistBaseUrl(src) + "/Release.gpg";
}

static std::string makeReleaseFilename(const RepoSource &src,
                                       const std::string &listsDir) {
  std::string key = src.uri + "_" + src.dist;
  key = sanitizeForFilename(key);
  return apm::fs::joinPath(listsDir, key + "_Release");
}

static std::string makeReleaseGpgFilename(const RepoSource &src,
                                          const std::string &listsDir) {
  std::string key = src.uri + "_" + src.dist;
  key = sanitizeForFilename(key);
  return apm::fs::joinPath(listsDir, key + "_Release.gpg");
}

static std::string makeInReleaseFilename(const RepoSource &src,
                                         const std::string &listsDir) {
  std::string key = src.uri + "_" + src.dist;
  key = sanitizeForFilename(key);
  return apm::fs::joinPath(listsDir, key + "_InRelease");
}

static std::string normalizeToCrLf(const std::string &text) {
  std::string out;
  out.reserve(text.size() + text.size() / 4 + 2);

  for (char c : text) {
    if (c == '\r')
      continue;
    if (c == '\n') {
      out.append("\r\n");
    } else {
      out.push_back(c);
    }
  }

  return out;
}

struct InReleaseParts {
  std::string releaseText;
  std::string signatureBlock;
};

static bool parseInReleaseFile(const std::string &path, InReleaseParts &out,
                               std::string *errorMsg) {
  std::string content;
  if (!apm::fs::readFile(path, content)) {
    if (errorMsg)
      *errorMsg = "Failed to read InRelease: " + path;
    return false;
  }

  std::istringstream in(content);
  std::string line;
  bool sawBegin = false;
  bool inHeaders = false;
  bool inSig = false;
  std::ostringstream releaseBuf;
  std::ostringstream sigBuf;

  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    if (!sawBegin) {
      if (line == "-----BEGIN PGP SIGNED MESSAGE-----") {
        sawBegin = true;
        inHeaders = true;
      }
      continue;
    }

    if (inSig) {
      sigBuf << line << "\n";
      continue;
    }

    if (line == "-----BEGIN PGP SIGNATURE-----") {
      inSig = true;
      sigBuf << line << "\n";
      continue;
    }

    if (inHeaders) {
      if (line.empty()) {
        inHeaders = false;
      }
      continue;
    }

    if (!line.empty() && line.size() >= 2 && line[0] == '-' && line[1] == ' ') {
      line.erase(0, 2);
    }

    releaseBuf << line << "\n";
  }

  if (!sawBegin) {
    if (errorMsg)
      *errorMsg = "InRelease missing cleartext signature header";
    return false;
  }

  std::string sigBlock = sigBuf.str();
  if (sigBlock.find("-----END PGP SIGNATURE-----") == std::string::npos) {
    if (errorMsg)
      *errorMsg = "InRelease missing signature trailer";
    return false;
  }

  out.releaseText = normalizeToCrLf(releaseBuf.str());
  out.signatureBlock = std::move(sigBlock);
  return true;
}

// Download Release/Packages metadata for every repo/component combination and
// store them under listsDir. Invokes optional progress callbacks per stage.
bool updateFromSourcesList(const std::string &sourcesPath,
                           const std::string &listsDir,
                           const std::string &defaultArch,
                           std::string *summaryMsg, std::string *errorMsg,
                           RepoUpdateProgressCallback progressCb) {
  if (summaryMsg)
    summaryMsg->clear();
  RepoSourceList sources;
  if (!loadSourcesList(sourcesPath, sources, errorMsg)) {
    return false;
  }

  if (sources.empty()) {
    if (errorMsg)
      *errorMsg = "No valid sources in " + sourcesPath;
    return false;
  }

  if (!apm::fs::createDirs(listsDir)) {
    if (errorMsg)
      *errorMsg = "Failed to create lists directory: " + listsDir;
    apm::logger::error("updateFromSourcesList: cannot create " + listsDir);
    return false;
  }

  size_t ok = 0;

  for (const auto &src : sources) {
    const std::string archToUse = resolveRepoArch(src, defaultArch);
    auto makeDownloadCallback = [&](RepoUpdateStage stage,
                                    const std::string &desc,
                                    const std::string *componentPtr)
        -> apm::net::TransferProgressCallback {
      if (!progressCb)
        return {};
      const RepoSource *srcPtr = &src;
      return [progressCb, stage, srcPtr, componentPtr,
              desc](const apm::net::TransferProgress &tp) {
        emitRepoProgress(progressCb, stage, srcPtr, componentPtr, tp.url,
                         tp.destination, tp.downloadedBytes, tp.downloadTotal,
                         tp.downloadSpeedBytesPerSec, tp.uploadSpeedBytesPerSec,
                         tp.finished, desc);
      };
    };

    // ---------------------------------------------------------
    // 1) Download and verify Release (+ InRelease fallback)
    // ---------------------------------------------------------
    std::string relUrl = makeReleaseUrl(src);
    std::string inRelUrl = makeInReleaseUrl(src);
    std::string relGpgUrl = makeReleaseGpgUrl(src);

    std::string relPath = makeReleaseFilename(src, listsDir);
    std::string relGpgPath = makeReleaseGpgFilename(src, listsDir);
    std::string inRelPath = makeInReleaseFilename(src, listsDir);

    std::string dlErr;
    bool releaseReady = false;
    bool signatureReady = false;

    // Prefer InRelease (clear-signed) where available
    auto inRelCb = makeDownloadCallback(RepoUpdateStage::DownloadRelease,
                                        "InRelease", nullptr);
    if (apm::net::downloadFile(inRelUrl, inRelPath, &dlErr, inRelCb)) {
      InReleaseParts parts;
      std::string inErr;
      if (parseInReleaseFile(inRelPath, parts, &inErr)) {
        if (apm::fs::writeFile(relPath, parts.releaseText, false)) {
          releaseReady = true;
        } else {
          apm::logger::warn(
              "updateFromSourcesList: failed to store Release contents from "
              "InRelease for " +
              src.uri + " " + src.dist);
        }

        if (!parts.signatureBlock.empty()) {
          if (apm::fs::writeFile(relGpgPath, parts.signatureBlock, false)) {
            signatureReady = true;
          } else {
            apm::logger::warn(
                "updateFromSourcesList: failed to store inline signature for " +
                src.uri + " " + src.dist);
          }
        }
      } else {
        apm::logger::warn("updateFromSourcesList: unable to parse InRelease " +
                          inRelUrl + ": " + inErr);
      }
    } else if (!dlErr.empty()) {
      apm::logger::info("updateFromSourcesList: InRelease not available at " +
                        inRelUrl + ": " + dlErr);
    }

    // Fallback to Release + Release.gpg if needed
    if (!releaseReady) {
      dlErr.clear();
      auto relCb = makeDownloadCallback(RepoUpdateStage::DownloadRelease,
                                        "Release", nullptr);
      if (!apm::net::downloadFile(relUrl, relPath, &dlErr, relCb)) {
        apm::logger::warn("updateFromSourcesList: failed to download Release " +
                          relUrl + ": " + dlErr);
        continue;
      }
      releaseReady = true;

      dlErr.clear();
      auto relGpgCb = makeDownloadCallback(
          RepoUpdateStage::DownloadReleaseSignature, "Release.gpg", nullptr);
      if (apm::net::downloadFile(relGpgUrl, relGpgPath, &dlErr, relGpgCb)) {
        signatureReady = true;
      } else {
        apm::logger::warn("updateFromSourcesList: Release.gpg missing at " +
                          relGpgUrl + ": " + dlErr);
      }
    }

    if (!releaseReady) {
      apm::logger::warn("updateFromSourcesList: no Release metadata for " +
                        src.uri + " " + src.dist);
      continue;
    }

    bool shouldVerify = src.trustPolicy != RepoTrustPolicy::Skip;

    if (shouldVerify) {
      if (!signatureReady) {
        apm::logger::warn("updateFromSourcesList: no Release signature found "
                          "for " +
                          src.uri + " " + src.dist);
        continue;
      }

      std::string sigErr;
      if (!apm::crypto::verifyDetachedSignature(
              relPath, relGpgPath, apm::config::getTrustedKeysDir(), &sigErr)) {
        apm::logger::warn("updateFromSourcesList: Release signature "
                          "verification failed for " +
                          src.uri + " " + src.dist + ": " + sigErr);
        continue;
      }
    } else {
      apm::logger::info("updateFromSourcesList: trusted=yes for " + src.uri +
                        " " + src.dist +
                        " – skipping Release signature verification");
    }

    // Parse Release for SHA256 entries
    ReleaseInfo relInfo;
    std::string relParseErr;
    if (!parseReleaseFile(relPath, relInfo, &relParseErr)) {
      apm::logger::warn("updateFromSourcesList: failed to parse Release for " +
                        src.uri + " " + src.dist + ": " + relParseErr);
      // Skip this source entirely
      continue;
    }

    // ---------------------------------------------------------
    // 2) Handle each component under this source (parallel downloads)
    // ---------------------------------------------------------
    struct PackagesDownload {
      std::string component;
      std::string fileBase;
      std::string gzUrl;
      std::string plainUrl;
      std::string gzDest;
      std::string plainDest;
      apm::net::TransferProgressCallback gzProgressCb;
      apm::net::TransferProgressCallback plainProgressCb;
      bool succeeded = false;
      std::string downloadedPath;
    };

    std::vector<PackagesDownload> pkgPlans;
    pkgPlans.reserve(src.components.size());
    std::vector<apm::net::DownloadRequest> primaryRequests;
    primaryRequests.reserve(src.components.size());

    for (const auto &comp : src.components) {
      PackagesDownload plan;
      plan.component = comp;

      std::string pkgUrlBase =
          makeDistBaseUrl(src) + "/" + comp + "/binary-" + archToUse + "/";
      plan.fileBase = makePackagesFilename(src, comp, archToUse, listsDir);
      plan.gzUrl = pkgUrlBase + "Packages.gz";
      plan.plainUrl = pkgUrlBase + "Packages";
      plan.gzDest = plan.fileBase + ".gz";
      plan.plainDest = plan.fileBase;

      std::string gzDesc = comp + " [" + archToUse + "] Packages.gz";
      plan.gzProgressCb = makeDownloadCallback(
          RepoUpdateStage::DownloadPackages, gzDesc, &plan.component);

      std::string plainDesc = comp + " [" + archToUse + "] Packages";
      plan.plainProgressCb = makeDownloadCallback(
          RepoUpdateStage::DownloadPackages, plainDesc, &plan.component);

      apm::net::DownloadRequest req;
      req.url = plan.gzUrl;
      req.destination = plan.gzDest;
      req.progressCb = plan.gzProgressCb;
      primaryRequests.push_back(req);

      pkgPlans.push_back(std::move(plan));
    }

    std::vector<apm::net::DownloadResult> primaryResults;
    if (!primaryRequests.empty()) {
      apm::net::downloadFiles(primaryRequests, primaryResults, 3);
    }

    std::vector<apm::net::DownloadRequest> fallbackRequests;
    std::vector<std::size_t> fallbackIndices;
    fallbackRequests.reserve(pkgPlans.size());
    fallbackIndices.reserve(pkgPlans.size());

    for (std::size_t i = 0; i < pkgPlans.size(); ++i) {
      auto &plan = pkgPlans[i];
      bool gzSuccess = (i < primaryResults.size() && primaryResults[i].success);

      if (gzSuccess) {
        // Verify checksum from Release for Packages.gz (prefer SHA256, fallback
        // MD5).
        const std::string relName =
            plan.component + "/binary-" + archToUse + "/Packages.gz";
        std::string expected;
        bool verified = false;

        if (findSha256ForPath(relInfo, relName, expected)) {
          std::string actual;
          std::string hashErr;
          if (apm::crypto::sha256File(plan.gzDest, actual, &hashErr)) {
            if (toLower(actual) == toLower(expected)) {
              verified = true;
            } else {
              apm::logger::warn("SHA256 mismatch for " + relName);
            }
          } else {
            apm::logger::warn("SHA256 compute failed for " + plan.gzDest +
                              (hashErr.empty() ? "" : (": " + hashErr)));
          }
        }

        if (!verified && findMd5ForPath(relInfo, relName, expected)) {
          std::string actual;
          std::string hashErr;
          if (apm::crypto::md5File(plan.gzDest, actual, &hashErr)) {
            if (toLower(actual) == toLower(expected)) {
              verified = true;
            } else {
              apm::logger::warn("MD5Sum mismatch for " + relName);
            }
          } else {
            apm::logger::warn("MD5 compute failed for " + plan.gzDest +
                              (hashErr.empty() ? "" : (": " + hashErr)));
          }
        }

        if (verified) {
          std::string plainPackages;
          if (preparePackagesTextFile(plan.fileBase, plan.gzDest, ".gz",
                                      plainPackages, &src, &plan.component,
                                      progressCb)) {
            apm::logger::info("Downloaded: " + plan.gzUrl + " → " +
                              plan.gzDest);
            plan.downloadedPath = plainPackages;
            plan.succeeded = true;
            continue;
          }
          apm::logger::warn("Failed to unpack downloaded Packages file " +
                            plan.gzDest);
        } else {
          apm::logger::warn(
              "Skipping Packages.gz due to checksum verification failure: " +
              plan.gzUrl);
        }
      } else if (i < primaryResults.size()) {
        apm::logger::warn("Failed to download " + plan.gzUrl + ": " +
                          primaryResults[i].errorMsg);
      } else {
        apm::logger::warn("Failed to download " + plan.gzUrl);
      }

      fallbackIndices.push_back(i);
      apm::net::DownloadRequest req;
      req.url = plan.plainUrl;
      req.destination = plan.plainDest;
      req.progressCb = plan.plainProgressCb;
      fallbackRequests.push_back(std::move(req));
    }

    if (!fallbackRequests.empty()) {
      std::vector<apm::net::DownloadResult> fallbackResults;
      apm::net::downloadFiles(fallbackRequests, fallbackResults, 3);

      for (std::size_t j = 0; j < fallbackRequests.size(); ++j) {
        std::size_t planIndex =
            (j < fallbackIndices.size()) ? fallbackIndices[j] : pkgPlans.size();
        if (planIndex >= pkgPlans.size())
          continue;

        auto &plan = pkgPlans[planIndex];
        bool plainSuccess =
            (j < fallbackResults.size() && fallbackResults[j].success);

        if (plainSuccess) {
          // Verify checksum from Release for plain Packages.
          const std::string relName =
              plan.component + "/binary-" + archToUse + "/Packages";
          std::string expected;
          bool verified = false;

          if (findSha256ForPath(relInfo, relName, expected)) {
            std::string actual;
            std::string hashErr;
            if (apm::crypto::sha256File(plan.plainDest, actual, &hashErr)) {
              if (toLower(actual) == toLower(expected)) {
                verified = true;
              } else {
                apm::logger::warn("SHA256 mismatch for " + relName);
              }
            } else {
              apm::logger::warn("SHA256 compute failed for " + plan.plainDest +
                                (hashErr.empty() ? "" : (": " + hashErr)));
            }
          }

          if (!verified && findMd5ForPath(relInfo, relName, expected)) {
            std::string actual;
            std::string hashErr;
            if (apm::crypto::md5File(plan.plainDest, actual, &hashErr)) {
              if (toLower(actual) == toLower(expected)) {
                verified = true;
              } else {
                apm::logger::warn("MD5Sum mismatch for " + relName);
              }
            } else {
              apm::logger::warn("MD5 compute failed for " + plan.plainDest +
                                (hashErr.empty() ? "" : (": " + hashErr)));
            }
          }

          if (verified) {
            std::string plainPackages;
            if (preparePackagesTextFile(plan.fileBase, plan.plainDest, "",
                                        plainPackages, &src, &plan.component,
                                        progressCb)) {
              apm::logger::info("Downloaded: " + plan.plainUrl + " → " +
                                plan.plainDest);
              plan.downloadedPath = plainPackages;
              plan.succeeded = true;
              continue;
            }
            apm::logger::warn("Failed to unpack downloaded Packages file " +
                              plan.plainDest);
          } else {
            apm::logger::warn(
                "Skipping Packages due to checksum verification failure: " +
                plan.plainUrl);
          }
        } else if (j < fallbackResults.size()) {
          apm::logger::warn("Failed to download " + plan.plainUrl + ": " +
                            fallbackResults[j].errorMsg);
        } else {
          apm::logger::warn("Failed to download " + plan.plainUrl);
        }
      }
    }

    for (const auto &plan : pkgPlans) {
      if (!plan.succeeded) {
        apm::logger::warn(
            "updateFromSourcesList: could not obtain ANY Packages index for " +
            src.uri + " " + src.dist + " " + plan.component + " [" + archToUse +
            "]");
        continue;
      }

      apm::logger::info("Verified and prepared Packages index: " +
                        plan.downloadedPath);
      ok++;
    }
  }

  if (ok == 0) {
    if (errorMsg) {
      *errorMsg = "No valid repo indices found in " + listsDir +
                  " – run 'apm update' and try again";
    }
    apm::logger::warn("updateFromSourcesList: no valid indices built");
    return false;
  }

  apm::logger::info("updateFromSourcesList: built " + std::to_string(ok) +
                    " repo index(es)");
  if (summaryMsg) {
    *summaryMsg = "Built " + std::to_string(ok) + " repo index(es)";
  }
  return true;
}

/**
 * Build repo indices from downloaded Release & Packages files.
 *
 * listsDir: directory containing Release, Release.gpg, Packages*
 * dbOut: vector of RepoIndex to fill
 * errorOut: optional string to receive errors
 */
// High-level wrapper that ensures metadata is downloaded, parses all Packages
// files, and returns a RepoIndexList the CLI/daemon reuse for lookups.
bool buildRepoIndices(const std::string &sourcesPath,
                      const std::string &listsDir,
                      const std::string &defaultArch, RepoIndexList &out,
                      std::string *errorMsg) {
  // Load sources.list entries
  RepoSourceList sources;
  if (!loadSourcesList(sourcesPath, sources, errorMsg)) {
    return false;
  }

  // Download Release + Packages (+ SHA256 verification)
  std::string summary;
  if (!updateFromSourcesList(sourcesPath, listsDir, defaultArch, &summary,
                             errorMsg)) {
    apm::logger::warn("buildRepoIndices: updateFromSourcesList failed");
    return false;
  }

  // Build list of RepoIndex results
  out.clear();

  for (const auto &src : sources) {
    // Resolve arch override
    const std::string archToUse = resolveRepoArch(src, defaultArch);

    for (const auto &comp : src.components) {
      RepoIndex idx;
      idx.source = src;
      idx.component = comp;
      idx.arch = archToUse;

      // Base path WITHOUT _Packages suffix
      std::string fileBase =
          makePackagesFilename(src, comp, archToUse, listsDir);

      std::string pkgFile;
      const std::string extractedPath = makePackagesPlainPath(fileBase);

      if (apm::fs::pathExists(extractedPath)) {
        pkgFile = extractedPath;
      } else {
        struct Candidate {
          const char *suffix;
        };

        static const Candidate suffixes[] = {{".gz"}, {""}};

        for (const auto &entry : suffixes) {
          std::string candidate = fileBase + entry.suffix;
          if (!apm::fs::pathExists(candidate)) {
            continue;
          }

          std::string preparedPath;
          if (preparePackagesTextFile(fileBase, candidate, entry.suffix,
                                      preparedPath)) {
            pkgFile = preparedPath;
            break;
          }

          if (entry.suffix[0] == '\0') {
            pkgFile = candidate;
            break;
          }
        }
      }

      if (pkgFile.empty()) {
        apm::logger::warn("buildRepoIndices: no Packages file found for " +
                          src.uri + " " + src.dist + " " + comp);
        continue;
      }

      idx.packagesPath = pkgFile;

      // Parse Packages file
      std::string parseErr;
      if (!parsePackagesFile(pkgFile, idx.packages, &parseErr)) {
        apm::logger::warn("buildRepoIndices: failed parsing " + pkgFile + ": " +
                          parseErr);
        continue;
      }

      // Fill repo metadata for each package
      bool anyTermuxPkg = false;
      for (auto &pkg : idx.packages) {
        pkg.repoUri = src.uri;
        pkg.repoDist = src.dist;
        pkg.repoComponent = comp;
        if (src.format == RepoFormat::Termux || src.isTermuxRepo) {
          pkg.isTermuxPackage = true;
        }
        if (pkg.isTermuxPackage) {
          anyTermuxPkg = true;
        }
      }

      idx.source.isTermuxRepo = src.isTermuxRepo || anyTermuxPkg;

      out.push_back(std::move(idx));
    }
  }

  apm::logger::info("buildRepoIndices: built " + std::to_string(out.size()) +
                    " repo index entries");

  return true;
}

} // namespace apm::repo
