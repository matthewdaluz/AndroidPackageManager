// SPDX-License-Identifier: GPL-3.0-or-later
// APM - Android Package Manager

#include "app_repo.hpp"

#include "downloader.hpp"
#include "fs.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fnmatch.h>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace apm::apprepo {
namespace {
constexpr std::uint64_t kMetadataLimit = 16 * 1024 * 1024;
constexpr std::uint64_t kApkLimit = 1024ULL * 1024 * 1024;

bool fail(std::string *error, const std::string &message) {
  if (error)
    *error = message;
  return false;
}
std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
std::string lower(std::string value) {
  for (auto &c : value)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}
bool plainText(const std::string &value) {
  return std::none_of(value.begin(), value.end(),
                      [](unsigned char c) { return c < 32 || c == 127; });
}
bool safeId(const std::string &id) {
  return !id.empty() && id.size() <= 64 && id.find("..") == std::string::npos &&
         (std::islower(static_cast<unsigned char>(id[0])) ||
          std::isdigit(static_cast<unsigned char>(id[0]))) &&
         std::all_of(id.begin(), id.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '-';
         });
}
std::vector<std::string> split(const std::string &value) {
  std::vector<std::string> result;
  std::istringstream in(value);
  std::string part;
  while (std::getline(in, part, ',')) {
    part = trim(part);
    if (part.empty())
      throw std::runtime_error("Empty item in comma-separated list");
    result.push_back(part);
  }
  return result;
}
std::string encode(const std::string &value) {
  const char *hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
      out += static_cast<char>(c);
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

// A bounded JSON reader for GitHub metadata. Numbers retain their text so asset
// sizes are never rounded through a floating-point representation.
struct Json {
  enum class Kind {
    Null,
    String,
    Number,
    Bool,
    Object,
    Array
  } kind = Kind::Null;
  std::string text;
  std::map<std::string, Json> object;
  std::vector<Json> array;
  const Json &get(const std::string &key) const {
    static const Json empty;
    auto it = object.find(key);
    return it == object.end() ? empty : it->second;
  }
  std::string string() const {
    return kind == Kind::String ? text : std::string{};
  }
  bool boolean() const { return kind == Kind::Bool && text == "true"; }
};
class JsonReader {
public:
  explicit JsonReader(const std::string &input) : input_(input) {}
  Json read() {
    if (input_.size() > kMetadataLimit)
      throw std::runtime_error("Release metadata is too large");
    Json result = value(0);
    ws();
    if (pos_ != input_.size())
      bad();
    return result;
  }

private:
  const std::string &input_;
  std::size_t pos_ = 0;
  [[noreturn]] void bad() const {
    throw std::runtime_error("Malformed release JSON at byte " +
                             std::to_string(pos_));
  }
  void ws() {
    while (pos_ < input_.size() &&
           (input_[pos_] == ' ' || input_[pos_] == '\n' ||
            input_[pos_] == '\r' || input_[pos_] == '\t'))
      ++pos_;
  }
  bool take(char c) {
    ws();
    if (pos_ < input_.size() && input_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }
  unsigned hex4() {
    unsigned result = 0;
    for (int i = 0; i < 4; ++i) {
      if (pos_ == input_.size())
        bad();
      unsigned char c = input_[pos_++];
      unsigned n = c >= '0' && c <= '9'   ? c - '0'
                   : c >= 'a' && c <= 'f' ? c - 'a' + 10
                   : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                          : 16;
      if (n == 16)
        bad();
      result = result * 16 + n;
    }
    return result;
  }
  static void utf8(std::string &out, unsigned c) {
    if (c < 0x80)
      out += static_cast<char>(c);
    else if (c < 0x800) {
      out += static_cast<char>(0xc0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 63));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xe0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 63));
      out += static_cast<char>(0x80 | (c & 63));
    } else {
      out += static_cast<char>(0xf0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 63));
      out += static_cast<char>(0x80 | ((c >> 6) & 63));
      out += static_cast<char>(0x80 | (c & 63));
    }
  }
  std::string string() {
    if (!take('"'))
      bad();
    std::string out;
    while (pos_ < input_.size()) {
      unsigned char c = input_[pos_++];
      if (c == '"')
        return out;
      if (c < 32)
        bad();
      if (c != '\\') {
        out += static_cast<char>(c);
        continue;
      }
      if (pos_ == input_.size())
        bad();
      switch (input_[pos_++]) {
      case '"':
        out += '"';
        break;
      case '\\':
        out += '\\';
        break;
      case '/':
        out += '/';
        break;
      case 'b':
        out += '\b';
        break;
      case 'f':
        out += '\f';
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      case 'u': {
        unsigned cp = hex4();
        if (cp >= 0xd800 && cp <= 0xdbff) {
          if (pos_ + 2 > input_.size() || input_.substr(pos_, 2) != "\\u")
            bad();
          pos_ += 2;
          unsigned low = hex4();
          if (low < 0xdc00 || low > 0xdfff)
            bad();
          cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
        } else if (cp >= 0xdc00 && cp <= 0xdfff)
          bad();
        utf8(out, cp);
        break;
      }
      default:
        bad();
      }
    }
    bad();
  }
  Json value(unsigned depth) {
    if (depth > 64)
      bad();
    ws();
    if (pos_ == input_.size())
      bad();
    Json out;
    if (input_[pos_] == '"') {
      out.kind = Json::Kind::String;
      out.text = string();
      return out;
    }
    if (take('{')) {
      out.kind = Json::Kind::Object;
      if (take('}'))
        return out;
      do {
        auto key = string();
        if (!take(':'))
          bad();
        if (!out.object.emplace(key, value(depth + 1)).second)
          bad();
      } while (take(','));
      if (!take('}'))
        bad();
      return out;
    }
    if (take('[')) {
      out.kind = Json::Kind::Array;
      if (take(']'))
        return out;
      do {
        out.array.push_back(value(depth + 1));
      } while (take(','));
      if (!take(']'))
        bad();
      return out;
    }
    for (const char *literal : {"true", "false", "null"}) {
      const std::string word(literal);
      if (input_.compare(pos_, word.size(), word) == 0) {
        pos_ += word.size();
        out.kind = word == "null" ? Json::Kind::Null : Json::Kind::Bool;
        out.text = word;
        return out;
      }
    }
    const std::size_t begin = pos_;
    if (input_[pos_] == '-')
      ++pos_;
    if (pos_ == input_.size())
      bad();
    if (input_[pos_] == '0')
      ++pos_;
    else {
      if (input_[pos_] < '1' || input_[pos_] > '9')
        bad();
      while (pos_ < input_.size() &&
             std::isdigit(static_cast<unsigned char>(input_[pos_])))
        ++pos_;
    }
    if (pos_ < input_.size() && input_[pos_] == '.') {
      ++pos_;
      auto p = pos_;
      while (pos_ < input_.size() &&
             std::isdigit(static_cast<unsigned char>(input_[pos_])))
        ++pos_;
      if (p == pos_)
        bad();
    }
    if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-'))
        ++pos_;
      auto p = pos_;
      while (pos_ < input_.size() &&
             std::isdigit(static_cast<unsigned char>(input_[pos_])))
        ++pos_;
      if (p == pos_)
        bad();
    }
    out.kind = Json::Kind::Number;
    out.text = input_.substr(begin, pos_ - begin);
    return out;
  }
};

bool readMetadataFile(const std::string &path, std::string &data,
                      std::string *error) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0 || st.st_size < 0 ||
      static_cast<std::uint64_t>(st.st_size) > kMetadataLimit)
    return fail(error, "Missing or oversized app metadata; run 'apm update'");
  return fs::readFile(path, data) ||
         fail(error, "Cannot read app metadata: " + path);
}
} // namespace

bool parseSourceFile(const std::string &path, repo::RepoSource &out,
                     std::string *error) {
  out = {};
  std::string data;
  if (!readMetadataFile(path, data, error))
    return false;
  if (data.size() > 64 * 1024)
    return fail(error, ".apprepo file exceeds 64 KiB");
  try {
    std::map<std::string, std::string> fields;
    const std::set<std::string> keys = {
        "id",      "name", "description",       "url", "apks", "avoidkeywords",
        "usetags", "tag",  "includeprereleases"};
    std::istringstream in(data);
    std::string line;
    while (std::getline(in, line)) {
      line = trim(line);
      if (line.empty() || line[0] == '#')
        continue;
      auto eq = line.find('=');
      if (eq == std::string::npos)
        throw std::runtime_error("Expected Key=Value in .apprepo");
      auto key = lower(trim(line.substr(0, eq)));
      auto v = trim(line.substr(eq + 1));
      if (!keys.count(key))
        throw std::runtime_error("Unknown .apprepo key: " + key);
      if (!v.empty() && (v.front() == '\'' || v.front() == '"')) {
        if (v.size() < 2 || v.back() != v.front())
          throw std::runtime_error("Unclosed quote for " + key);
        v = v.substr(1, v.size() - 2);
      }
      if (!plainText(v) || !fields.emplace(key, v).second)
        throw std::runtime_error("Invalid or duplicate .apprepo field: " + key);
    }
    auto &app = out.app;
    app.id = fields["id"];
    app.name = fields["name"];
    app.description = fields["description"];
    if (!safeId(app.id))
      throw std::runtime_error("ID must use lowercase letters, numbers, dots "
                               "or hyphens (1-64 characters)");
    if (app.name.empty())
      throw std::runtime_error("Name is required");
    std::string url = fields["url"];
    while (!url.empty() && url.back() == '/')
      url.pop_back();
    const std::string prefix = "https://github.com/";
    if (url.compare(0, prefix.size(), prefix) != 0)
      throw std::runtime_error("URL must be an HTTPS GitHub releases URL");
    const auto parts = url.substr(prefix.size());
    auto slash = parts.find('/');
    auto releases =
        parts.find("/releases", slash == std::string::npos ? 0 : slash + 1);
    if (slash == std::string::npos || releases == std::string::npos ||
        parts.substr(releases) != "/releases")
      throw std::runtime_error(
          "URL must have the form https://github.com/OWNER/REPO/releases/");
    app.owner = parts.substr(0, slash);
    app.repository = parts.substr(slash + 1, releases - slash - 1);
    for (const auto *v : {&app.owner, &app.repository}) {
      if (v->empty() || *v == "." || *v == ".." ||
          !std::all_of(v->begin(), v->end(), [](unsigned char c) {
            return std::isalnum(c) || c == '-' || c == '_' || c == '.';
          }))
        throw std::runtime_error("Invalid GitHub owner or repository");
    }
    app.patterns = split(fields["apks"]);
    if (app.patterns.empty() || app.patterns.size() > 32)
      throw std::runtime_error("APKs must list 1-32 filename patterns");
    for (const auto &pattern : app.patterns) {
      if (pattern.size() < 5 || pattern.substr(pattern.size() - 4) != ".apk" ||
          pattern.find_first_of("/\\[]") != std::string::npos ||
          pattern.find("..") != std::string::npos)
        throw std::runtime_error(
            "APKs patterns must be APK basenames using only * or ? wildcards");
    }
    if (!fields["avoidkeywords"].empty())
      app.avoidKeywords = split(lower(fields["avoidkeywords"]));
    auto boolean = [&](const std::string &key, bool fallback) {
      if (!fields.count(key))
        return fallback;
      auto v = lower(fields[key]);
      if (v != "true" && v != "false")
        throw std::runtime_error(key + " must be true or false");
      return v == "true";
    };
    app.useTags = boolean("usetags", true);
    app.includePrereleases = boolean("includeprereleases", false);
    app.tag = fields["tag"];
    if (app.includePrereleases && !app.useTags && app.tag.empty())
      throw std::runtime_error(
          "IncludePrereleases requires UseTags=true or an explicit Tag");
    crypto::SHA256 hash;
    hash.update(data);
    app.configHash = hash.finalHex();
    out.type = "app";
    out.uri = url;
    out.format = repo::RepoFormat::App;
    out.components = {"apps"};
    return true;
  } catch (const std::exception &e) {
    return fail(error, path + ": " + e.what());
  }
}

namespace {
std::string releaseApiBase(const repo::RepoSource &source) {
  return "https://api.github.com/repos/" + source.app.owner + "/" +
         source.app.repository + "/releases";
}

bool validTag(const std::string &tag) {
  return !tag.empty() && tag.size() <= 128 && plainText(tag) &&
         tag != "." && tag != ".." && tag.find(' ') == std::string::npos;
}

bool eligibleRelease(const Json &release, const repo::RepoSource &source) {
  if (release.kind != Json::Kind::Object ||
      release.get("draft").kind != Json::Kind::Bool ||
      release.get("prerelease").kind != Json::Kind::Bool)
    throw std::runtime_error("Invalid release flags in metadata");
  const auto tag = release.get("tag_name").string();
  return !release.get("draft").boolean() &&
         (source.app.includePrereleases || !release.get("prerelease").boolean()) &&
         !tag.empty() && (source.app.tag.empty() || tag == source.app.tag);
}

} // namespace

std::string releaseApiUrl(const repo::RepoSource &source) {
  const auto base = releaseApiBase(source);
  if (!source.app.tag.empty())
    return base + "/tags/" + encode(source.app.tag);
  return base + (source.app.useTags ? "?per_page=100" : "/latest");
}
std::string cachePath(const repo::RepoSource &source,
                      const std::string &listsDir) {
  return fs::joinPath(listsDir, "apprepo-" + source.app.id + "-" +
                                    source.app.configHash + ".json");
}

namespace {
repo::PackageEntry parseRelease(const Json &release,
                                const repo::RepoSource &source) {
  repo::PackageEntry out;
  out.version = release.get("tag_name").string();
  if (!validTag(out.version))
    throw std::runtime_error("Invalid release tag");
  const auto &assets = release.get("assets");
  if (assets.kind != Json::Kind::Array)
    throw std::runtime_error("Release has no assets array");
  const std::string allowedUrl = "https://github.com/" + source.app.owner +
                                 "/" + source.app.repository +
                                 "/releases/download/";
  std::set<std::string> selected;
  for (const auto &pattern : source.app.patterns) {
    for (const auto &asset : assets.array) {
      const auto name = asset.get("name").string();
      if (name.size() < 5 || name.substr(name.size() - 4) != ".apk" ||
          ::fnmatch(pattern.c_str(), name.c_str(), 0) != 0 ||
          selected.count(name))
        continue;
      const auto lowered = lower(name);
      if (std::any_of(source.app.avoidKeywords.begin(),
                      source.app.avoidKeywords.end(), [&](const auto &word) {
                        return lowered.find(word) != std::string::npos;
                      }))
        continue;

      repo::AppAsset item;
      item.name = name;
      item.url = asset.get("browser_download_url").string();
      auto digest = lower(asset.get("digest").string());
      if (!plainText(item.name) || item.name != trim(item.name) ||
          item.name.find_first_of("/\\") != std::string::npos ||
          item.name.find("..") != std::string::npos || item.name.size() > 255)
        throw std::runtime_error("Unsafe APK asset name");
      if (!plainText(item.url) ||
          item.url.compare(0, allowedUrl.size(), allowedUrl) != 0 ||
          item.url.find("/../") != std::string::npos)
        throw std::runtime_error(
            "APK download URL must belong to the configured GitHub repository");
      if (digest.size() != 71 || digest.compare(0, 7, "sha256:") != 0 ||
          !std::all_of(digest.begin() + 7, digest.end(),
                       [](unsigned char c) { return std::isxdigit(c); }))
        throw std::runtime_error(
            "APK asset requires a SHA256 digest from GitHub: " + item.name);
      item.sha256 = digest.substr(7);
      const auto &size = asset.get("size");
      const auto parsed = std::from_chars(
          size.text.data(), size.text.data() + size.text.size(), item.size);
      if (size.kind != Json::Kind::Number || parsed.ec != std::errc{} ||
          parsed.ptr != size.text.data() + size.text.size() || !item.size ||
          item.size > kApkLimit)
        throw std::runtime_error("Invalid APK asset size");
      selected.insert(item.name);
      out.appAssets.push_back(std::move(item));
    }
  }
  if (out.appAssets.empty())
    throw std::runtime_error("Release " + out.version + " has no matching APKs");

  out.packageName = source.app.id;
  out.architecture = "all";
  out.isAppPackage = true;
  out.repoUri = source.uri;
  out.repoComponent = "apps";
  out.rawFields["Description"] = source.app.description.empty()
                                     ? source.app.name
                                     : source.app.description;
  out.rawFields["Name"] = source.app.name;
  out.rawFields["Type"] = "apprepo";
  auto &names = out.rawFields["APKs"];
  for (const auto &a : out.appAssets) {
    if (!names.empty())
      names += ", ";
    names += a.name;
  }
  return out;
}
} // namespace

bool parseReleaseMetadata(const std::string &data,
                          const repo::RepoSource &source,
                          repo::PackageEntry &out, std::string *error) {
  out = {};
  try {
    const auto json = JsonReader(data).read();
    const Json *release = nullptr;
    if (json.kind == Json::Kind::Array) {
      for (const auto &v : json.array) {
        if (eligibleRelease(v, source) &&
            (!release || v.get("published_at").string() >
                             release->get("published_at").string()))
          release = &v;
      }
    } else if (eligibleRelease(json, source)) {
      release = &json;
    }
    if (!release)
      throw std::runtime_error(
          "No eligible published release (check Tag and IncludePrereleases)");
    out = parseRelease(*release, source);
    return true;
  } catch (const std::exception &e) {
    return fail(error, source.app.id + ": " + e.what());
  }
}

bool fetchRelease(const repo::RepoSource &source, const std::string &tag,
                  repo::PackageEntry &out,
                  std::string *error) {
  if (!validTag(tag))
    return fail(error, "Invalid release tag");
  if (!source.app.tag.empty() && tag != source.app.tag)
    return fail(error, source.app.id + " is pinned to release " + source.app.tag);
  std::string data, downloadError;
  if (!net::downloadFile(releaseApiBase(source) + "/tags/" + encode(tag), {},
                         &downloadError, {}, {true, kMetadataLimit, &data}))
    return fail(error, "Cannot fetch release " + tag + " for " + source.app.id +
                           ": " + downloadError);
  auto requested = source;
  requested.app.tag = tag;
  return parseReleaseMetadata(data, requested, out, error);
}

bool listReleases(const repo::RepoSource &source, std::vector<std::string> &tags,
                  std::string *error) {
  tags.clear();
  if (!source.app.tag.empty()) {
    repo::PackageEntry release;
    if (!fetchRelease(source, source.app.tag, release, error))
      return false;
    tags.push_back(release.version);
    return true;
  }

  std::string data, downloadError;
  if (!net::downloadFile(releaseApiBase(source) + "?per_page=100", {},
                         &downloadError, {}, {true, kMetadataLimit, &data}))
    return fail(error, "Cannot fetch releases for " + source.app.id + ": " +
                           downloadError);
  try {
    const auto json = JsonReader(data).read();
    if (json.kind != Json::Kind::Array)
      throw std::runtime_error("Expected a release list");
    std::vector<std::pair<std::string, std::string>> releases;
    for (const auto &release : json.array) {
      if (!eligibleRelease(release, source))
        continue;
      try {
        auto entry = parseRelease(release, source);
        releases.emplace_back(release.get("published_at").string(),
                              std::move(entry.version));
      } catch (const std::runtime_error &) {
        // Releases without usable APK metadata cannot be installed.
      }
    }
    std::stable_sort(releases.begin(), releases.end(),
                     [](const auto &a, const auto &b) { return a.first > b.first; });
    std::set<std::string> seen;
    for (const auto &release : releases)
      if (seen.insert(release.second).second)
        tags.push_back(release.second);
    return true;
  } catch (const std::exception &e) {
    return fail(error, source.app.id + ": " + e.what());
  }
}

bool update(const repo::RepoSource &source, const std::string &listsDir,
            std::string *error, repo::RepoUpdateProgressCallback cb) {
  const auto dest = cachePath(source, listsDir);
  const auto temporary = dest + ".tmp";
  auto progress = [&](const net::TransferProgress &tp) {
    if (!cb)
      return;
    repo::RepoUpdateProgress p;
    p.stage = repo::RepoUpdateStage::DownloadPackages;
    p.repoUri = source.uri;
    p.component = "apps";
    p.remotePath = tp.url;
    p.localPath = tp.destination;
    p.description = source.app.name + " release metadata";
    p.currentBytes = tp.downloadedBytes;
    p.totalBytes = tp.downloadTotal;
    p.downloadSpeed = tp.downloadSpeedBytesPerSec;
    p.uploadSpeed = tp.uploadSpeedBytesPerSec;
    p.finished = tp.finished;
    cb(p);
  };
  if (!net::downloadFile(releaseApiUrl(source), temporary, error, progress,
                         {true, kMetadataLimit})) {
    fs::removeFile(temporary);
    return false;
  }
  std::string data;
  repo::PackageEntry entry;
  if (!readMetadataFile(temporary, data, error) ||
      !parseReleaseMetadata(data, source, entry, error)) {
    fs::removeFile(temporary);
    return false;
  }
  if (::chmod(temporary.c_str(), 0644) != 0 ||
      ::rename(temporary.c_str(), dest.c_str()) != 0) {
    fs::removeFile(temporary);
    return fail(error, "Cannot publish app metadata cache");
  }
  return true;
}
bool loadCached(const repo::RepoSource &source, const std::string &listsDir,
                repo::RepoIndex &out, std::string *error) {
  out = {};
  out.source = source;
  out.component = "apps";
  out.arch = "all";
  out.packagesPath = cachePath(source, listsDir);
  std::string data;
  repo::PackageEntry entry;
  if (!readMetadataFile(out.packagesPath, data, error) ||
      !parseReleaseMetadata(data, source, entry, error))
    return false;
  out.packages.push_back(std::move(entry));
  return true;
}
} // namespace apm::apprepo
