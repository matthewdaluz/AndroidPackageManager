/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: fs.hpp
 * Purpose: Declare filesystem utility helpers shared across components.
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

#pragma once

#include <string>
#include <vector>

namespace apm::fs {

// -------------------------------------------------
// Basic path introspection
// -------------------------------------------------

// Returns true if the path exists (file or directory).
bool pathExists(const std::string &path);

// Returns true if the path exists and is a regular file.
bool isFile(const std::string &path);

// Returns true if the path exists and is a directory.
bool isDirectory(const std::string &path);

// -------------------------------------------------
// Directory creation
// -------------------------------------------------

// Create a single directory. Succeeds if it already exists as a directory.
// mode is a standard POSIX permission mask (e.g. 0755).
bool createDir(const std::string &path, unsigned int mode = 0755);

// Recursively create all directories in the path (similar to `mkdir -p`).
bool createDirs(const std::string &path, unsigned int mode = 0755);

// -------------------------------------------------
// File operations
// -------------------------------------------------

// Read entire file into 'out'. Returns false on failure.
bool readFile(const std::string &path, std::string &out);

// Overwrite file with 'content'. Optionally creates parent directories.
bool writeFile(const std::string &path, const std::string &content,
               bool createParents = true);

// Append 'content' to file. Optionally creates parent directories.
bool appendFile(const std::string &path, const std::string &content,
                bool createParents = true);

// Remove a single file. Returns true if removed or not present.
bool removeFile(const std::string &path);

// Recursively delete a directory tree. Returns true if the directory
// does not exist at the end (whether it was there or not).
bool removeDirRecursive(const std::string &path);

// -------------------------------------------------
// Directory listing
// -------------------------------------------------

// List entries in a directory (names only, not full paths).
// If includeHidden == false, entries starting with '.' are skipped.
std::vector<std::string> listDir(const std::string &path,
                                 bool includeHidden = false);

// -------------------------------------------------
// Path helpers
// -------------------------------------------------

// Join two path segments with a single '/' between them.
// If 'right' is absolute (starts with '/'), it is returned as-is.
std::string joinPath(const std::string &left, const std::string &right);

// -------------------------------------------------
// APK Installation
// -------------------------------------------------

bool isRegularFile(const std::string &path);
bool mkdirs(const std::string &path);

} // namespace apm::fs
