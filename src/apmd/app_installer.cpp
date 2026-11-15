// SPDX-License-Identifier: GPL-3.0-or-later
// APM - Android Package Manager

#include "app_installer.hpp"
#include "ams/module_info.hpp"
#include "apk_metadata.hpp"
#include "config.hpp"
#include "downloader.hpp"
#include "fs.hpp"
#include "security.hpp"
#include "sha256.hpp"
#include "status_db.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace apm::apps {
namespace {
struct Transaction {
  std::string path;
  ~Transaction() {
    if (!path.empty())
      fs::removeDirRecursive(path);
  }
};
bool error(InstallResult &out, const std::string &message) {
  out.ok = false;
  out.message = message;
  return false;
}
std::string quoted(const std::string &s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '\\' || c == '"')
      out += '\\';
    if (c < 32) {
      const char *hex = "0123456789abcdef";
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 15];
    } else
      out += static_cast<char>(c);
  }
  return out + '"';
}
bool permissionName(const std::string &s) {
  return !s.empty() && s.size() < 256 && s.find('.') != std::string::npos &&
         s.front() != '.' && s.back() != '.' &&
         s.find("..") == std::string::npos &&
         std::all_of(
             s.begin(), s.end(),
             [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_' || c == '.';
             });
}
std::string planHash(const repo::PackageEntry &bundle,
                     const InstallOptions &options) {
  crypto::SHA256 h;
  h.update(bundle.packageName + "\n" + bundle.version + "\n" +
           options.destination + "\n");
  auto permissions = options.permissions;
  std::sort(permissions.begin(), permissions.end());
  for (const auto &p : permissions)
    h.update(p + "\n");
  for (const auto &a : bundle.appAssets)
    h.update(a.name + "\n" + a.url + "\n" + a.sha256 + "\n" +
             std::to_string(a.size) + "\n");
  return h.finalHex();
}
bool write(const std::string &path, const std::string &content) {
  return fs::writeFile(path, content) && ::chmod(path.c_str(), 0644) == 0;
}
bool copy(const std::string &from, const std::string &to) {
  std::ifstream in(from, std::ios::binary);
  std::ofstream out(to, std::ios::binary | std::ios::trunc);
  if (!in || !out)
    return false;
  out << in.rdbuf();
  out.flush();
  if (!in.good() || !out.good())
    return false;
  out.close();
  return !out.fail() && ::chmod(to.c_str(), 0644) == 0;
}
bool present(const std::string &path) {
  struct stat st{};
  return ::lstat(path.c_str(), &st) == 0;
}
bool owned(const std::string &module, const std::string &id) {
  struct stat root{}, markerInfo{};
  const auto path = fs::joinPath(module, ".apprepo-owner");
  std::string marker;
  return ::lstat(module.c_str(), &root) == 0 && S_ISDIR(root.st_mode) &&
         ::lstat(path.c_str(), &markerInfo) == 0 &&
         S_ISREG(markerInfo.st_mode) && fs::readFile(path, marker) &&
         marker == id + "\n";
}
std::string systemError() { return std::strerror(errno); }
} // namespace

std::string statusKey(const std::string &id) { return "app:" + id; }

bool install(const repo::PackageEntry &bundle, const InstallOptions &options,
             InstallResult &out, install::InstallProgressCallback cb) {
  out = {};
  std::string validation;
  if (!bundle.isAppPackage || bundle.appAssets.empty() ||
      !security::validatePackageName(bundle.packageName, &validation))
    return error(out, "Invalid app repository bundle");
  if (options.destination != "app" && options.destination != "priv-app")
    return error(out, "App destination must be app or priv-app");
  if (options.permissions.size() > 256)
    return error(out, "Too many permission choices");
  for (const auto &p : options.permissions)
    if (!permissionName(p))
      return error(out, "Invalid privileged permission: " + p);
  if (options.destination != "priv-app" && !options.permissions.empty())
    return error(out, "Privileged permissions require /system/priv-app");
  out.plan = planHash(bundle, options);
  if (!options.expectedPlan.empty() && options.expectedPlan != out.plan)
    return error(
        out, "App install plan changed; run app-install again to review it");
  status::InstalledDb db;
  std::string err;
  if (!status::loadStatus(db, &err))
    return error(out, "Cannot load installed app status: " + err);
  const std::string moduleName = "apm-app-" + bundle.packageName;
  const std::string moduleRoot =
      fs::joinPath(config::getModulesDir(), moduleName);
  auto existing = db.find(statusKey(bundle.packageName));
  ams::ModuleState existingState;
  const bool complete =
      existing != db.end() && !existing->second.appIds.empty() &&
      std::all_of(existing->second.appIds.begin(),
                  existing->second.appIds.end(),
                  [&](const auto &id) {
                    return fs::isFile(fs::joinPath(
                        moduleRoot, "overlay/system/" + options.destination +
                                        "/" + id + "/base.apk"));
                  }) &&
      ams::readModuleState(fs::joinPath(moduleRoot, "state.json"),
                           existingState, nullptr) &&
      existingState.enabled;
  if (existing != db.end() && existing->second.appPlan == out.plan &&
      complete && owned(moduleRoot, bundle.packageName)) {
    out.ok = true;
    out.message = bundle.packageName + " " + bundle.version +
                  " is already staged with these options";
    return true;
  }
  if (present(moduleRoot) && !owned(moduleRoot, bundle.packageName))
    return error(
        out,
        "AMS module already exists and is not owned by this app repository: " +
            moduleName);
  std::ostringstream plan;
  plan << "App bundle: " << bundle.packageName << " " << bundle.version
       << "\nDestination: /system/" << options.destination
       << "\nAPKs (size and SHA256 checked before staging):\n";
  for (const auto &a : bundle.appAssets)
    plan << "  " << a.name << " (" << a.size << " bytes)\n";
  if (!options.permissions.empty()) {
    plan << "Privileged permission allowlist:\n";
    for (const auto &p : options.permissions)
      plan << "  " << p << "\n";
  }
  plan << "Reboot required to apply the system app bundle.";
  out.changed = true;
  if (options.simulate) {
    out.ok = true;
    out.message = plan.str();
    return true;
  }
  if (!config::isEmulatorMode() && ::geteuid() != 0)
    return error(out, "System app installation requires apmd to run as root");

  const auto transactions =
      fs::joinPath(config::getApmRoot(), "app-transactions");
  if (!fs::createDirs(transactions, 0700) ||
      !fs::createDirs(config::getModulesDir()))
    return error(out, "Cannot create app transaction directories");
  std::string temp = fs::joinPath(transactions, "install-XXXXXX");
  std::vector<char> pattern(temp.begin(), temp.end());
  pattern.push_back('\0');
  char *created = ::mkdtemp(pattern.data());
  if (!created)
    return error(out, "Cannot create app transaction: " + systemError());
  Transaction transaction{created};
  const auto staged = fs::joinPath(transaction.path, "module");
  const auto overlay =
      fs::joinPath(staged, "overlay/system/" + options.destination);
  if (!fs::createDirs(overlay))
    return error(out, "Cannot stage app module");
  for (const auto &target : {"system", "vendor", "product"}) {
    if (!fs::createDirs(fs::joinPath(staged, std::string("workdir/") + target)))
      return error(out, "Cannot stage module workdir");
  }

  std::set<std::string> packageIds, usedPermissions;
  std::ostringstream permissionXml;
  permissionXml
      << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<permissions>\n";
  for (const auto &asset : bundle.appAssets) {
    const auto downloaded = fs::joinPath(transaction.path, asset.name);
    auto progress = [&](const net::TransferProgress &tp) {
      if (!cb)
        return;
      install::InstallProgress p;
      p.packageName = bundle.packageName;
      p.url = tp.url;
      p.destination = tp.destination;
      p.downloadedBytes = tp.downloadedBytes;
      p.totalBytes = tp.downloadTotal;
      p.uploadedBytes = tp.uploadedBytes;
      p.uploadTotal = tp.uploadTotal;
      p.downloadSpeedBytesPerSec = tp.downloadSpeedBytesPerSec;
      p.uploadSpeedBytesPerSec = tp.uploadSpeedBytesPerSec;
      p.finished = tp.finished;
      cb(p);
    };
    if (!net::downloadFile(asset.url, downloaded, &err, progress,
                           {true, asset.size}))
      return error(out, "APK download failed (bundle unchanged): " +
                            asset.name + ": " + err);
    struct stat st{};
    std::string digest;
    if (::stat(downloaded.c_str(), &st) != 0 || st.st_size < 0 ||
        static_cast<std::uint64_t>(st.st_size) != asset.size ||
        !crypto::sha256File(downloaded, digest, &err) || digest != asset.sha256)
      return error(out, "APK size/SHA256 mismatch (bundle unchanged): " +
                            asset.name);
    apk::ApkMetadata metadata;
    if (!apk::readApkMetadata(downloaded, metadata, &err))
      return error(out, "Invalid APK " + asset.name + ": " + err);
    if (!packageIds.insert(metadata.packageName).second)
      return error(out,
                   "Bundle contains multiple APKs for " + metadata.packageName);
    for (const auto &item : db) {
      if (item.first == statusKey(bundle.packageName) ||
          !item.second.appPackage)
        continue;
      if (std::find(item.second.appIds.begin(), item.second.appIds.end(),
                    metadata.packageName) != item.second.appIds.end())
        return error(out, "APK package is already managed by app bundle " +
                              item.second.name + ": " + metadata.packageName);
    }
    const auto appDir = fs::joinPath(overlay, metadata.packageName);
    if (!fs::createDirs(appDir) ||
        !copy(downloaded, fs::joinPath(appDir, "base.apk")))
      return error(out, "Cannot stage APK: " + asset.name);
    if (options.destination == "priv-app") {
      permissionXml << "  <privapp-permissions package=\""
                    << metadata.packageName << "\">\n";
      for (const auto &p : metadata.requestedPermissions) {
        if (!permissionName(p))
          continue;
        const bool allowed =
            std::find(options.permissions.begin(), options.permissions.end(),
                      p) != options.permissions.end();
        if (allowed)
          usedPermissions.insert(p);
        permissionXml << "    <" << (allowed ? "permission" : "deny-permission")
                      << " name=\"" << p << "\"/>\n";
      }
      permissionXml << "  </privapp-permissions>\n";
    }
  }
  for (const auto &p : options.permissions)
    if (!usedPermissions.count(p))
      return error(
          out, "Permission is not requested by any APK in this bundle: " + p);
  permissionXml << "</permissions>\n";
  if (options.destination == "priv-app" &&
      !write(fs::joinPath(
                 staged, "overlay/system/etc/permissions/privapp-permissions-" +
                             moduleName + ".xml"),
             permissionXml.str()))
    return error(out, "Cannot stage privileged permission allowlist");
  const std::string info =
      "{\n\"name\":" + quoted(moduleName) +
      ",\n\"version\":" + quoted(bundle.version) +
      ",\n\"author\":\"APM\",\n\"description\":" +
      quoted("App repository bundle: " + bundle.packageName) +
      ",\n\"mount\":true,\n\"post_fs_data\":false,\n\"service\":false,"
      "\n\"install-sh\":false\n}\n";
  if (!write(fs::joinPath(staged, "module-info.json"), info) ||
      !write(fs::joinPath(staged, ".apprepo-owner"), bundle.packageName + "\n"))
    return error(out, "Cannot write app module metadata");
  ams::ModuleState state;
  if (present(moduleRoot))
    ams::readModuleState(fs::joinPath(moduleRoot, "state.json"), state,
                         nullptr);
  if (state.installedAt.empty())
    state.installedAt = ams::makeIsoTimestamp();
  state.updatedAt = ams::makeIsoTimestamp();
  state.enabled = true;
  state.lastError.clear();
  if (!ams::writeModuleState(fs::joinPath(staged, "state.json"), state, &err) ||
      ::chmod(fs::joinPath(staged, "state.json").c_str(), 0644) != 0)
    return error(out, "Cannot write app module state: " + err);

  const bool hadModule = present(moduleRoot);
  const auto backup = fs::joinPath(transaction.path, "previous-module");
  if (hadModule && ::rename(moduleRoot.c_str(), backup.c_str()) != 0)
    return error(out, "Cannot back up app module: " + systemError());
  if (::rename(staged.c_str(), moduleRoot.c_str()) != 0) {
    const auto reason = systemError();
    if (hadModule && ::rename(backup.c_str(), moduleRoot.c_str()) != 0) {
      transaction.path.clear();
      return error(out,
                   "Cannot restore app module; recovery copy retained at " +
                       backup);
    }
    return error(out, "Cannot publish app module: " + reason);
  }
  status::InstalledPackage record;
  record.name = bundle.packageName;
  record.version = bundle.version;
  record.architecture = "all";
  record.status = "install ok installed";
  record.installRoot = moduleRoot;
  record.repoUri = bundle.repoUri;
  record.repoComponent = "apps";
  record.appPackage = true;
  record.appDestination = options.destination;
  record.appPlan = out.plan;
  record.appIds.assign(packageIds.begin(), packageIds.end());
  record.appPermissions = options.permissions;
  db[statusKey(bundle.packageName)] = record;
  if (!status::writeStatus(db, &err)) {
    const auto failed = fs::joinPath(transaction.path, "failed-module");
    if (::rename(moduleRoot.c_str(), failed.c_str()) != 0 ||
        (hadModule && ::rename(backup.c_str(), moduleRoot.c_str()) != 0)) {
      transaction.path.clear();
      return error(out, "Cannot restore app module after status failure; "
                        "recovery directory: " +
                            backup + "; " + err);
    }
    return error(out,
                 "Cannot persist app status; previous module restored: " + err);
  }
  out.ok = true;
  out.message = "Staged " + bundle.packageName + " " + bundle.version + " (" +
                std::to_string(packageIds.size()) + " APKs) in /system/" +
                options.destination + ". Reboot required.";
  return true;
}

bool remove(const std::string &id, std::string &message) {
  std::string err;
  if (!security::validatePackageName(id, &err)) {
    message = err;
    return false;
  }
  if (!config::isEmulatorMode() && ::geteuid() != 0) {
    message = "System app removal requires root";
    return false;
  }
  status::InstalledDb db;
  if (!status::loadStatus(db, &err)) {
    message = err;
    return false;
  }
  auto it = db.find(statusKey(id));
  if (it == db.end()) {
    message = "App bundle is not installed: " + id;
    return false;
  }
  const auto module = fs::joinPath(config::getModulesDir(), "apm-app-" + id);
  if (present(module) && !owned(module, id)) {
    message = "Refusing to remove an AMS module not owned by this app bundle";
    return false;
  }
  Transaction transaction;
  std::string retired;
  const bool exists = present(module);
  if (exists) {
    const auto base = fs::joinPath(config::getApmRoot(), "app-transactions");
    if (!fs::createDirs(base, 0700)) {
      message = "Cannot create app removal transaction";
      return false;
    }
    const auto pattern = fs::joinPath(base, "remove-XXXXXX");
    std::vector<char> bytes(pattern.begin(), pattern.end());
    bytes.push_back('\0');
    char *created = ::mkdtemp(bytes.data());
    if (!created) {
      message = "Cannot create app removal transaction: " + systemError();
      return false;
    }
    transaction.path = created;
    retired = fs::joinPath(transaction.path, "module");
    if (::rename(module.c_str(), retired.c_str()) != 0) {
      message = "Cannot retire app module: " + systemError();
      return false;
    }
  }
  db.erase(it);
  if (!status::writeStatus(db, &err)) {
    if (exists && ::rename(retired.c_str(), module.c_str()) != 0) {
      transaction.path.clear();
      message =
          "Cannot restore app module; recovery copy at " + retired + "; " + err;
      return false;
    }
    message = err;
    return false;
  }
  message = "Removed staged app bundle " + id +
            ". Reboot required. Android user data and separately installed "
            "updates are preserved.";
  return true;
}
} // namespace apm::apps
