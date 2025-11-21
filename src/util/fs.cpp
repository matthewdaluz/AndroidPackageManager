/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: fs.cpp
 * Purpose: Implement filesystem helpers for path inspection, reading/writing, and recursive cleanup.
 * Last Modified: November 18th, 2025. - 3:00 PM Eastern Time.
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

#include "fs.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

namespace apm::fs {

// Internal helper to stat a path without duplicating errno handling in every
// callsite.
static bool statPath(const std::string &path, struct stat &st) {
  if (path.empty())
    return false;
  return ::stat(path.c_str(), &st) == 0;
}

bool pathExists(const std::string &path) {
  struct stat st{};
  return statPath(path, st);
}

bool isFile(const std::string &path) {
  struct stat st{};
  if (!statPath(path, st))
    return false;
  return S_ISREG(st.st_mode);
}

bool isDirectory(const std::string &path) {
  struct stat st{};
  if (!statPath(path, st))
    return false;
  return S_ISDIR(st.st_mode);
}

bool createDir(const std::string &path, unsigned int mode) {
  if (path.empty())
    return false;

  // Try to create the directory
  if (::mkdir(path.c_str(), static_cast<mode_t>(mode)) == 0) {
    return true;
  }

  // If it already exists as a directory, treat as success
  if (errno == EEXIST && isDirectory(path)) {
    return true;
  }

  return false;
}

// Recursive mkdir implementation similar to `mkdir -p`.
bool createDirs(const std::string &path, unsigned int mode) {
  if (path.empty())
    return false;

  // If it already exists as a directory, we're done.
  if (isDirectory(path))
    return true;

  // Find parent directory
  auto pos = path.find_last_of('/');
  if (pos != std::string::npos) {
    std::string parent = path.substr(0, pos);
    if (!parent.empty() && !isDirectory(parent)) {
      if (!createDirs(parent, mode)) {
        return false;
      }
    }
  }

  return createDir(path, mode);
}

bool readFile(const std::string &path, std::string &out) {
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in.is_open()) {
    return false;
  }

  std::ostringstream ss;
  ss << in.rdbuf();
  if (!in.good() && !in.eof()) {
    return false;
  }

  out = ss.str();
  return true;
}

bool writeFile(const std::string &path, const std::string &content,
               bool createParents) {
  if (path.empty())
    return false;

  if (createParents) {
    auto pos = path.find_last_of('/');
    if (pos != std::string::npos) {
      std::string parent = path.substr(0, pos);
      if (!parent.empty()) {
        if (!createDirs(parent)) {
          return false;
        }
      }
    }
  }

  std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    return false;
  }

  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  return out.good();
}

bool appendFile(const std::string &path, const std::string &content,
                bool createParents) {
  if (path.empty())
    return false;

  if (createParents) {
    auto pos = path.find_last_of('/');
    if (pos != std::string::npos) {
      std::string parent = path.substr(0, pos);
      if (!parent.empty()) {
        if (!createDirs(parent)) {
          return false;
        }
      }
    }
  }

  std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::app);
  if (!out.is_open()) {
    return false;
  }

  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  return out.good();
}

bool removeFile(const std::string &path) {
  if (path.empty())
    return false;

  if (::unlink(path.c_str()) == 0) {
    return true;
  }

  // If the file is already gone, we consider that success.
  if (errno == ENOENT) {
    return true;
  }

  return false;
}

// Recursively delete a directory tree while ignoring missing components.
bool removeDirRecursive(const std::string &path) {
  if (path.empty())
    return false;

  // If the path does not exist, treat as success.
  if (!pathExists(path))
    return true;

  DIR *dir = ::opendir(path.c_str());
  if (!dir) {
    // If it's not a directory, try to remove as file.
    return removeFile(path);
  }

  struct dirent *entry;
  while ((entry = ::readdir(dir)) != nullptr) {
    const char *name = entry->d_name;

    // Skip . and ..
    if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
      continue;
    }

    std::string child = joinPath(path, name);

    struct stat st{};
    if (::lstat(child.c_str(), &st) != 0) {
      // Can't stat, try to remove anyway
      removeFile(child);
      continue;
    }

    if (S_ISDIR(st.st_mode)) {
      // Recurse into subdirectory
      removeDirRecursive(child);
    } else {
      // Remove file or symlink
      removeFile(child);
    }
  }

  ::closedir(dir);

  // Finally remove the now-empty directory itself.
  if (::rmdir(path.c_str()) == 0) {
    return true;
  }

  // If it's already gone, fine.
  if (errno == ENOENT) {
    return true;
  }

  return false;
}

// Enumerate directory entries (names only). Optionally skip dot files.
std::vector<std::string> listDir(const std::string &path, bool includeHidden) {
  std::vector<std::string> entries;

  DIR *dir = ::opendir(path.c_str());
  if (!dir) {
    return entries;
  }

  struct dirent *entry;
  while ((entry = ::readdir(dir)) != nullptr) {
    const char *name = entry->d_name;

    // Skip . and ..
    if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
      continue;
    }

    if (!includeHidden && name[0] == '.') {
      continue;
    }

    entries.emplace_back(name);
  }

  ::closedir(dir);

  std::sort(entries.begin(), entries.end());
  return entries;
}

// Join two path segments and avoid duplicate slashes unless the right side is
// already absolute.
std::string joinPath(const std::string &left, const std::string &right) {
  if (right.empty())
    return left;
  if (right.front() == '/')
    return right; // treat right as absolute

  if (left.empty())
    return right;

  if (left.back() == '/') {
    return left + right;
  }
  return left + "/" + right;
}

bool isRegularFile(const std::string &path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0)
    return false;
  return S_ISREG(st.st_mode);
}

// Lightweight mkdir -p helper used by the APK installer for Magisk overlay
// scaffolding.
bool mkdirs(const std::string &path) {
  if (path.empty())
    return false;

  // If it already exists AND is a directory → done
  if (isDirectory(path))
    return true;

  // Parent directory first
  auto slash = path.find_last_of('/');
  if (slash != std::string::npos) {
    std::string parent = path.substr(0, slash);
    if (!parent.empty() && parent != "/") {
      if (!mkdirs(parent)) {
        return false;
      }
    }
  }

  // Create this directory
  if (::mkdir(path.c_str(), 0755) != 0) {
    if (errno == EEXIST) {
      return isDirectory(path);
    }
    return false;
  }

  return true;
}

} // namespace apm::fs
