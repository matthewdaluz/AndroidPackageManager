// SPDX-License-Identifier: GPL-3.0-or-later
// APM - Android Package Manager

#pragma once
#include <string>
#include <vector>

namespace apm::apk {
struct ApkMetadata {
  std::string packageName;
  std::vector<std::string> requestedPermissions;
};
// Reads AndroidManifest.xml from the APK ZIP and parses binary Android XML.
// Does not execute tools or extract files into the filesystem.
bool readApkMetadata(const std::string &path, ApkMetadata &out,
                     std::string *errorMsg = nullptr);
} // namespace apm::apk
