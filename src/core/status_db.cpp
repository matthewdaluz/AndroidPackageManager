/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: status_db.cpp
 * Purpose: Implement reading/writing of the dpkg-style status database.
 * Last Modified: November 22nd, 2025. - 10:30 PM Eastern Time.
 * Author: Matthew DaLuz - RedHead Founder
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

#include "status_db.hpp"

#include "config.hpp"
#include "fs.hpp"
#include "logger.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace apm::status {

// -------------------- string helpers --------------------

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

// Parse "Depends" field into a vector of package names (first alt only)
static std::vector<std::string>
parseDependsField(const std::string &dependsRaw) {
  std::vector<std::string> result;
  if (dependsRaw.empty())
    return result;

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
    if (name.empty())
      continue;

    auto colonPos = name.find(':');
    if (colonPos != std::string::npos) {
      name = name.substr(0, colonPos);
      trim(name);
      if (name.empty())
        continue;
    }

    result.push_back(name);
  }

  return result;
}

static bool parseBool(const std::string &raw) {
  std::string s = raw;
  for (auto &c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return (s == "1" || s == "yes" || s == "true");
}

// -------------------- stanza parser --------------------

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

// -------------------- core load/write --------------------

// Parse a dpkg-style status file from disk into the InstalledDb map.
bool loadStatusFile(const std::string &path, InstalledDb &out,
                    std::string *errorMsg) {
  out.clear();

  if (!apm::fs::pathExists(path)) {
    apm::logger::info("status_db: no status file yet at " + path +
                      " (starting with empty DB)");
    return true;
  }

  std::string content;
  if (!apm::fs::readFile(path, content)) {
    if (errorMsg)
      *errorMsg = "Failed to read status file: " + path;
    apm::logger::error("status_db: cannot read " + path);
    return false;
  }

  std::vector<std::unordered_map<std::string, std::string>> stanzas;
  parseStanzas(content, stanzas);

  for (const auto &fields : stanzas) {
    auto getField = [&](const std::string &key) -> std::string {
      auto it = fields.find(key);
      if (it != fields.end()) {
        return it->second;
      }
      return {};
    };

    InstalledPackage pkg;
    pkg.name = getField("Package");
    pkg.version = getField("Version");
    pkg.architecture = getField("Architecture");
    pkg.status = getField("Status");
    pkg.installRoot = getField("Installed-Root");
    pkg.repoUri = getField("Repo");
    pkg.repoDist = getField("Repo-Dist");
    pkg.repoComponent = getField("Repo-Component");
    pkg.installPrefix = getField("Install-Prefix");

    std::string dependsRaw = getField("Depends");
    if (!dependsRaw.empty()) {
      pkg.depends = parseDependsField(dependsRaw);
    }

    std::string autoRaw = getField("Auto-Installed");
    if (!autoRaw.empty()) {
      pkg.autoInstalled = parseBool(autoRaw);
    } else {
      pkg.autoInstalled = false;
    }

    std::string termuxRaw = getField("Termux-Package");
    if (!termuxRaw.empty()) {
      pkg.termuxPackage = parseBool(termuxRaw);
    }

    if (pkg.name.empty()) {
      continue;
    }

    out[pkg.name] = std::move(pkg);
  }

  apm::logger::info("status_db: loaded " + std::to_string(out.size()) +
                    " installed package(s) from " + path);
  return true;
}

// Serialize the InstalledDb map back to disk so CLI/daemon stay in sync.
bool writeStatusFile(const std::string &path, const InstalledDb &db,
                     std::string *errorMsg) {
  apm::fs::createDirs(apm::config::APM_ROOT);

  std::string tmpPath = path + ".tmp";
  std::ostringstream out;

  bool first = true;
  for (const auto &it : db) {
    const InstalledPackage &pkg = it.second;
    if (!first) {
      out << "\n";
    }
    first = false;

    out << "Package: " << pkg.name << "\n";
    if (!pkg.version.empty())
      out << "Version: " << pkg.version << "\n";
    if (!pkg.architecture.empty())
      out << "Architecture: " << pkg.architecture << "\n";
    if (!pkg.status.empty())
      out << "Status: " << pkg.status << "\n";
    if (!pkg.installRoot.empty())
      out << "Installed-Root: " << pkg.installRoot << "\n";
    if (!pkg.repoUri.empty())
      out << "Repo: " << pkg.repoUri << "\n";
    if (!pkg.repoDist.empty())
      out << "Repo-Dist: " << pkg.repoDist << "\n";
    if (!pkg.repoComponent.empty())
      out << "Repo-Component: " << pkg.repoComponent << "\n";
    if (!pkg.installPrefix.empty())
      out << "Install-Prefix: " << pkg.installPrefix << "\n";
    if (pkg.termuxPackage) {
      out << "Termux-Package: yes\n";
    }

    if (!pkg.depends.empty()) {
      out << "Depends: ";
      for (std::size_t i = 0; i < pkg.depends.size(); ++i) {
        if (i > 0)
          out << ", ";
        out << pkg.depends[i];
      }
      out << "\n";
    }

    if (pkg.autoInstalled) {
      out << "Auto-Installed: yes\n";
    }

    out << "\n";
  }

  std::string data = out.str();

  if (!apm::fs::writeFile(tmpPath, data)) {
    if (errorMsg)
      *errorMsg = "Failed to write temp status file: " + tmpPath;
    apm::logger::error("status_db: cannot write " + tmpPath);
    return false;
  }

  if (::rename(tmpPath.c_str(), path.c_str()) < 0) {
    ::unlink(tmpPath.c_str());
    if (errorMsg)
      *errorMsg = "Failed to rename " + tmpPath + " -> " + path;
    apm::logger::error("status_db: rename failed " + tmpPath + " -> " + path);
    return false;
  }

  apm::logger::info("status_db: wrote " + std::to_string(db.size()) +
                    " entries to " + path);
  return true;
}

// -------------------- helpers using default path --------------------

// Convenience wrapper targeting the default status file path.
bool loadStatus(InstalledDb &out, std::string *errorMsg) {
  return loadStatusFile(apm::config::STATUS_FILE, out, errorMsg);
}

// Mirror the given DB to disk under the default status file path.
bool writeStatus(const InstalledDb &db, std::string *errorMsg) {
  return writeStatusFile(apm::config::STATUS_FILE, db, errorMsg);
}

// -------------------- convenience APIs --------------------

// Lookup helper that optionally returns the installed metadata for `name`.
bool isInstalled(const std::string &name, InstalledPackage *pkgOut,
                 std::string *errorMsg) {
  InstalledDb db;
  if (!loadStatus(db, errorMsg)) {
    return false;
  }

  auto it = db.find(name);
  if (it == db.end()) {
    return false;
  }

  if (pkgOut) {
    *pkgOut = it->second;
  }
  return true;
}

// Update the status DB entry for a package (creates or replaces existing
// stanzas).
bool recordInstalled(const InstalledPackage &pkg, std::string *errorMsg) {
  InstalledDb db;
  if (!loadStatus(db, errorMsg)) {
    return false;
  }

  db[pkg.name] = pkg;
  if (!writeStatus(db, errorMsg)) {
    return false;
  }

  return true;
}

// Remove the package entry from the status DB.
bool removeInstalled(const std::string &name, std::string *errorMsg) {
  InstalledDb db;
  if (!loadStatus(db, errorMsg)) {
    return false;
  }

  auto it = db.find(name);
  if (it == db.end()) {
    return true; // not installed
  }

  db.erase(it);
  if (!writeStatus(db, errorMsg)) {
    return false;
  }

  return true;
}

} // namespace apm::status
