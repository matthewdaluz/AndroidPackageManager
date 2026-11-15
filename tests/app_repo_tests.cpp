#include "apk_metadata.hpp"
#include "app_installer.hpp"
#include "app_repo.hpp"
#include "config.hpp"
#include "downloader.hpp"
#include "fs.hpp"
#include "protocol.hpp"
#include "sha256.hpp"
#include "status_db.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
std::string fixtures;
int downloads = 0;
bool failDownload = false, corruptDownload = false;
void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
std::string read(const std::string &path) {
  std::string data;
  require(apm::fs::readFile(path, data), "read " + path);
  return data;
}
std::string fixture(const std::string &name) {
  return apm::fs::joinPath(fixtures, name);
}
void write(const std::string &path, const std::string &data) {
  require(apm::fs::writeFile(path, data), "write " + path);
}
std::string hash(const std::string &path) {
  std::string result, error;
  require(apm::crypto::sha256File(path, result, &error), error);
  return result;
}
} // namespace

// Deterministic download fixtures exercise the real install transaction without
// contacting GitHub or modifying an Android installation.
namespace apm::net {
bool downloadFile(const std::string &url, const std::string &dest,
                  std::string *error, TransferProgressCallback cb,
                  DownloadOptions options) {
  ++downloads;
  require(options.httpsOnly && options.maxBytes,
          "app downloads must be bounded HTTPS");
  if (failDownload) {
    if (error)
      *error = "fixture download failure";
    return false;
  }
  const auto name = url.find("api.github.com") != std::string::npos
                        ? "releases.json"
                        : url.substr(url.find_last_of('/') + 1);
  const auto data =
      corruptDownload ? std::string("corrupt fixture") : read(fixture(name));
  write(dest, data);
  if (cb) {
    TransferProgress p;
    p.url = url;
    p.destination = dest;
    p.downloadedBytes = data.size();
    p.downloadTotal = data.size();
    p.finished = true;
    cb(p);
  }
  return true;
}
bool downloadFiles(const std::vector<DownloadRequest> &,
                   std::vector<DownloadResult> &, std::size_t, std::string *) {
  return false;
}
} // namespace apm::net

int main(int argc, char **argv) {
  try {
    require(argc == 3, "usage: tests fixtures source-root");
    fixtures = argv[1];
    std::string pattern = "/tmp/apm-app-tests-XXXXXX";
    std::vector<char> bytes(pattern.begin(), pattern.end());
    bytes.push_back('\0');
    char *directory = ::mkdtemp(bytes.data());
    require(directory, "mkdtemp");
    const std::string home(directory);
    struct Cleanup {
      std::string path;
      ~Cleanup() { apm::fs::removeDirRecursive(path); }
    } cleanup{home};
    require(::setenv("HOME", home.c_str(), 1) == 0, "set HOME");
    apm::config::setEmulatorMode(true);
    for (const auto &vector : std::map<std::string, std::string>{
             {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852"
                  "b855"},
             {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f"
                     "20015ad"},
             {std::string(1000000, 'a'), "cdc76e5c9914fb9281a1c7e284d73e67f1809"
                                         "a48a497200e046d39ccc7112cd0"}}) {
      apm::crypto::SHA256 sha;
      sha.update(vector.first);
      require(sha.finalHex() == vector.second, "SHA256 known vector");
    }
    apm::repo::RepoSource source;
    std::string err;
    const auto example =
        std::string(argv[2]) + "/examples/MicroG-Example.apprepo";
    require(apm::apprepo::parseSourceFile(example, source, &err), err);
    require(source.app.id == "microg" && source.app.patterns.size() == 2 &&
                source.app.avoidKeywords.size() == 2,
            "example parsing");
    require(
        apm::apprepo::releaseApiUrl(source) ==
            "https://api.github.com/repos/microg/GmsCore/releases?per_page=100",
        "release URL");
    const auto original = read(example);
    for (const auto &extra :
         {"\nUnknown=anything\n", "\nID=duplicate\n", "\nUseTags=maybe\n"}) {
      write(home + "/bad.apprepo", original + extra);
      apm::repo::RepoSource bad;
      require(!apm::apprepo::parseSourceFile(home + "/bad.apprepo", bad, &err),
              "invalid fields rejected");
    }
    auto invalidBoolean = original;
    const auto boolAt = invalidBoolean.find("UseTags=true");
    invalidBoolean.replace(boolAt, 12, "UseTags=maybe");
    write(home + "/bad.apprepo", invalidBoolean);
    apm::repo::RepoSource invalidSource;
    require(!apm::apprepo::parseSourceFile(home + "/bad.apprepo", invalidSource,
                                           &err),
            "malformed boolean rejected");
    auto malicious = original;
    auto at = malicious.find("ID=\"microg\"");
    malicious.replace(at, 11, "ID=\"../escape\"");
    write(home + "/bad.apprepo", malicious);
    apm::repo::RepoSource badSource;
    require(
        !apm::apprepo::parseSourceFile(home + "/bad.apprepo", badSource, &err),
        "path traversal ID rejected");
    auto pinned = source;
    pinned.app.tag = "v1/test";
    require(apm::apprepo::releaseApiUrl(pinned).find("/tags/v1%2Ftest") !=
                std::string::npos,
            "tag URL escaping");

    apm::repo::PackageEntry bundle;
    require(apm::apprepo::parseReleaseMetadata(read(fixture("releases.json")),
                                               source, bundle, &err),
            err);
    require(bundle.isAppPackage && bundle.packageName == "microg" &&
                bundle.appAssets.size() == 2 && bundle.version == "v1",
            "bundle selection");
    for (const auto &name : {"draft", "prerelease", "incomplete", "ambiguous",
                             "unsigned", "wrong-host"}) {
      apm::repo::PackageEntry entry;
      require(
          !apm::apprepo::parseReleaseMetadata(
              read(fixture(std::string(name) + ".json")), source, entry, &err),
          std::string("reject ") + name);
    }
    auto prereleases = source;
    prereleases.app.includePrereleases = true;
    apm::repo::PackageEntry entry;
    require(apm::apprepo::parseReleaseMetadata(read(fixture("prerelease.json")),
                                               prereleases, entry, &err),
            "opt-in prerelease");
    for (const auto &json :
         {"[] trailing", "{\"tag_name\":\"x\",\"tag_name\":\"y\"}", "[[[[",
          "{\"tag_name\":\"\\uD800\"}"})
      require(!apm::apprepo::parseReleaseMetadata(json, source, entry, &err),
              "malformed JSON rejected");

    const auto sourceDir = home + "/sources";
    write(sourceDir + "/microg.apprepo", original);
    write(sourceDir + "/debian.repo",
          "Type=deb\nURL=https://deb.debian.org/debian\nSuites=stable,main\n");
    apm::repo::RepoSourceList sources;
    require(apm::repo::loadSourcesList(sourceDir, sources, &err) &&
                sources.size() == 2,
            "mixed sources");
    write(sourceDir + "/duplicate.apprepo", original);
    require(!apm::repo::loadSourcesList(sourceDir, sources, &err),
            "duplicate app ID rejected");
    apm::fs::removeFile(sourceDir + "/duplicate.apprepo");

    for (const auto &name :
         {"com.google.android.gms-1.apk", "com.android.vending-1.apk"}) {
      apm::apk::ApkMetadata metadata;
      require(apm::apk::readApkMetadata(fixture(name), metadata, &err), err);
      require(metadata.requestedPermissions.size() == 2 &&
                  !metadata.packageName.empty(),
              "binary manifest permissions");
    }
    for (const auto &name : {"invalid.apk", "no-manifest.apk", "bad-xml.apk",
                             "oversized-manifest.apk", "bad-string-pool.apk"}) {
      apm::apk::ApkMetadata metadata;
      require(!apm::apk::readApkMetadata(fixture(name), metadata, &err),
              "invalid APK rejected");
    }

    const auto lists = apm::config::getListsDir();
    require(apm::fs::createDirs(lists), "lists");
    require(apm::apprepo::update(source, lists, &err), err);
    apm::repo::RepoIndex index;
    require(apm::apprepo::loadCached(source, lists, index, &err) &&
                index.packages.size() == 1,
            "cache round trip");
    const auto cache = read(apm::apprepo::cachePath(source, lists));
    failDownload = true;
    require(!apm::apprepo::update(source, lists, &err),
            "failed update reported");
    failDownload = false;
    require(read(apm::apprepo::cachePath(source, lists)) == cache,
            "failed update preserves cache");
    auto changedSource = source;
    changedSource.app.configHash = "changed";
    require(!apm::apprepo::loadCached(changedSource, lists, index, &err),
            "changed source cannot reuse stale cache");

    apm::apps::InstallOptions options;
    options.simulate = true;
    apm::apps::InstallResult result;
    const auto before = downloads;
    require(apm::apps::install(bundle, options, result), result.message);
    require(downloads == before && result.changed && !result.plan.empty(),
            "simulation does not download");
    options.expectedPlan = result.plan;
    options.simulate = false;
    require(apm::apps::install(bundle, options, result), result.message);
    const auto module = apm::config::getModulesDir() + "/apm-app-microg";
    const auto apk =
        module + "/overlay/system/app/com.google.android.gms/base.apk";
    require(hash(apk) == hash(fixture("com.google.android.gms-1.apk")),
            "installed APK intact");
    require(apm::apps::install(bundle, options, result) && !result.changed,
            "idempotent install");
    apm::status::InstalledDb db;
    require(apm::status::loadStatus(db, &err) && db.count("app:microg") &&
                db.at("app:microg").appIds.size() == 2,
            "app status persisted");
    apm::status::InstalledPackage deb;
    deb.name = "microg";
    deb.version = "debian-version";
    db["microg"] = deb;
    require(apm::status::writeStatus(db, &err), err);
    db.clear();
    require(apm::status::loadStatus(db, &err) && db.count("microg") &&
                db.count("app:microg"),
            "Debian and app namespaces coexist");

    auto newer = bundle;
    newer.version = "v2";
    options.expectedPlan.clear();
    options.simulate = true;
    require(apm::apps::install(newer, options, result), result.message);
    options.simulate = false;
    options.expectedPlan = result.plan;
    const auto previous = hash(apk);
    failDownload = true;
    require(!apm::apps::install(newer, options, result),
            "download failure aborts install");
    failDownload = false;
    require(hash(apk) == previous, "previous bundle survives download failure");
    corruptDownload = true;
    require(!apm::apps::install(newer, options, result),
            "hash failure aborts install");
    corruptDownload = false;
    require(hash(apk) == previous,
            "previous bundle survives integrity failure");
    options.expectedPlan = "stale-plan";
    require(!apm::apps::install(newer, options, result), "stale plan refused");

    options = {};
    options.destination = "priv-app";
    options.permissions = {"android.permission.FAKE_PACKAGE_SIGNATURE"};
    options.simulate = true;
    require(apm::apps::install(newer, options, result), result.message);
    options.simulate = false;
    options.expectedPlan = result.plan;
    require(apm::apps::install(newer, options, result), result.message);
    require(!apm::fs::pathExists(apk),
            "destination change removes obsolete app files");
    const auto permissions =
        read(module + "/overlay/system/etc/permissions/"
                      "privapp-permissions-apm-app-microg.xml");
    require(
        permissions.find(
            "<permission name=\"android.permission.FAKE_PACKAGE_SIGNATURE\"") !=
            std::string::npos,
        "explicit permission allowed");
    require(permissions.find(
                "<deny-permission name=\"android.permission.GET_ACCOUNTS\"") !=
                std::string::npos,
            "other privileged grants denied");
    options.permissions = {"android.permission.DOES_NOT_EXIST"};
    options.expectedPlan.clear();
    require(!apm::apps::install(newer, options, result),
            "unrequested permission rejected");
    options.permissions = {"android.permission.X\"/><evil"};
    require(!apm::apps::install(newer, options, result),
            "XML permission injection rejected");

    auto conflict = bundle;
    conflict.packageName = "another-bundle";
    options = {};
    require(!apm::apps::install(conflict, options, result),
            "duplicate Android package ownership rejected");
    require(!apm::fs::pathExists(apm::config::getModulesDir() +
                                 "/apm-app-another-bundle"),
            "conflicting bundle not published");

    const auto savedInfo = read(module + "/module-info.json");
    const auto blocked = apm::config::getStatusFile() + ".tmp";
    require(apm::fs::createDirs(blocked), "block status temp write");
    auto updateBundle = bundle;
    updateBundle.version = "v3";
    options = {};
    require(!apm::apps::install(updateBundle, options, result),
            "status failure aborts installation");
    require(read(module + "/module-info.json") == savedInfo,
            "status failure restores module");
    std::string message;
    require(!apm::apps::remove("microg", message),
            "status failure aborts removal");
    require(read(module + "/module-info.json") == savedInfo,
            "failed removal restores module");
    require(apm::fs::removeDirRecursive(blocked), "unblock status write");
    require(apm::apps::remove("microg", message), message);
    require(!apm::fs::pathExists(module), "app module removed");
    db.clear();
    require(apm::status::loadStatus(db, &err) && !db.count("app:microg") &&
                db.count("microg"),
            "app removal preserves Debian package");
    require(apm::fs::createDirs(module), "foreign module");
    write(module + "/foreign", "keep");
    options = {};
    require(!apm::apps::install(bundle, options, result) &&
                apm::fs::pathExists(module + "/foreign"),
            "foreign module preserved");

    require(apm::fs::removeDirRecursive(module), "remove foreign fixture");
    const auto unrelated = home + "/unrelated-module";
    write(unrelated + "/.apprepo-owner", "microg\n");
    write(unrelated + "/keep", "unrelated payload");
    require(::symlink(unrelated.c_str(), module.c_str()) == 0,
            "create module symlink fixture");
    require(!apm::apps::install(bundle, options, result),
            "symlink module cannot claim bundle ownership");
    require(read(unrelated + "/keep") == "unrelated payload",
            "symlink target preserved");
    require(::unlink(module.c_str()) == 0, "remove module symlink fixture");

    apm::ipc::Request request, parsed;
    request.type = apm::ipc::RequestType::AppInstall;
    request.packageName = "microg";
    request.rawFields["permissions"] =
        "android.permission.X\nandroid.permission.Y";
    require(apm::ipc::parseRequest(apm::ipc::serializeRequest(request), parsed,
                                   &err) &&
                parsed.type == request.type &&
                parsed.rawFields["permissions"] ==
                    request.rawFields["permissions"],
            "app IPC round trip");
    std::cout << "App repository, APK manifest, integrity, transaction, "
                 "permissions, namespace and protocol checks passed.\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
