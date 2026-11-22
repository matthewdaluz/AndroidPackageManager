// SPDX-License-Identifier: GPL-3.0-or-later
// APM - Android Package Manager

#pragma once

#include "repo_index.hpp"
#include <string>
#include <vector>

namespace apm::apprepo {

// .apprepo files are declarative data. No shell expansion or commands are run.
bool parseSourceFile(const std::string &path, repo::RepoSource &out,
                     std::string *errorMsg = nullptr);
std::string releaseApiUrl(const repo::RepoSource &source);
std::string cachePath(const repo::RepoSource &source,
                      const std::string &listsDir);

// Also used for cached metadata: all asset names, URLs and digests are
// validated.
bool parseReleaseMetadata(const std::string &json,
                          const repo::RepoSource &source,
                          repo::PackageEntry &out,
                          std::string *errorMsg = nullptr);
bool fetchRelease(const repo::RepoSource &source, const std::string &tag,
                  repo::PackageEntry &out,
                  std::string *errorMsg = nullptr);
bool listReleases(const repo::RepoSource &source, std::vector<std::string> &tags,
                  std::string *errorMsg = nullptr);
bool update(const repo::RepoSource &source, const std::string &listsDir,
            std::string *errorMsg = nullptr,
            repo::RepoUpdateProgressCallback progressCb = {});
bool loadCached(const repo::RepoSource &source, const std::string &listsDir,
                repo::RepoIndex &out, std::string *errorMsg = nullptr);

} // namespace apm::apprepo
