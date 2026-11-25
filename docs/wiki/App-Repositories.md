# App repositories

An `.apprepo` file describes an APK bundle published through GitHub or GitLab.com releases.
The bundle's `ID` is used by `apm app-list`, `apm app-install`,
`apm app-info`, and `apm app-remove`. Debian and Termux packages continue to use `apm install`.
App and Debian package IDs have separate installed-state namespaces.

## MicroG example

The complete example is in [`Repositories/AppRepos/MicroG.apprepo`](../../Repositories/AppRepos/MicroG.apprepo):

```ini
ID="microg"
Name="MicroG (System-Wide)"
Description="Repository file to install MicroG system-wide via the GMSCore repository."
URL=https://github.com/microg/GmsCore/releases/
APKs="com.google.android.gms-*.apk,com.android.vending-*.apk"
AvoidKeywords="asc,hw"
UseTags=true
```

```sh
apm add-repo Repositories/AppRepos/MicroG.apprepo
apm update
apm search microg
apm app-info microg
apm app-list microg
apm app-install microg --select-release
```

`apm app-list <id> <release>` lists the matching APK filenames and byte sizes
for an exact release tag. Omit the tag to see a menu drawn from up to
100 recent releases. Enter a menu number or type an exact tag, including a tag
outside the menu. A release pinned with `Tag` is the only version available for
that repository. Explicit tag lookups and the picker fetch metadata from the provider
and work as soon as the repository is added.

`apm app-install <id> <release>` installs from a specific tag. Use
`--select-release` in place of the tag to open the same release picker. An
invocation with just the ID uses the cached release from `apm update`. If
there are multiple APKs, enter their numbers separated by commas or spaces
(such as `1,3`), or press Enter to select all. Review the selected release,
APKs, and destination before confirming. The default destination is
`/system/app`. For the MicroG example, the `-hw` variants and detached
signature files are excluded.

Installing the APKs provides the system app payload. MicroG functionality also
depends on the ROM's signature-spoofing support, permissions, and compatibility
with existing Google packages. APM does not patch the framework or replace
existing Google system apps. Follow the project's [installation instructions](https://github.com/microg/GmsCore/wiki/Installation)
and check MicroG's self-check after reboot.

## F-Droid example

[`Repositories/AppRepos/FDroid.apprepo`](../../Repositories/AppRepos/FDroid.apprepo)
uses the [F-Droid client releases](https://gitlab.com/fdroid/fdroidclient/-/releases).

```sh
apm add-repo Repositories/AppRepos/FDroid.apprepo
apm update
apm app-list fdroid
apm app-install fdroid --select-release
```

GitLab.com projects must publish matching APKs as release links to the same
project's public Generic Package Registry. This includes project package-file
download links, which require the package version to match the release tag, and
Generic Package Registry API download URLs with the URL-encoded project path
and their own package version.
APM looks up the file's size and SHA256 when updating or selecting a specific
release. The picker uses the release links to populate the menu.

## Destination and permissions prompt

```sh
apm app-install microg --install-as-system
```

Here `--install-as-system` opens an options prompt:

1. `/system/app`: standard system apps.
2. `/system/priv-app`: privileged system apps.

For `priv-app`, enter the permission names to allow (for example, `android.permission.GET_ACCOUNTS`) as a
comma-separated list. Leave the input empty to grant no privileged permissions.
The final install plan shows the choices before asking for confirmation.
APM reads each APK's binary `AndroidManifest.xml` and applies each choice only to
APKs that request that permission. A permission requested by no APK is rejected.
APM never infers package IDs from release filenames.

The generated module contains a same-partition
`/system/etc/permissions/privapp-permissions-apm-app-<id>.xml` file. Requested
permissions selected in the prompt receive `permission` entries; other
requested permissions receive `deny-permission` entries for privileged
grants. Normal and runtime permissions remain subject to Android's normal
permission handling. This follows Android's [privileged permission allowlist](https://source.android.com/docs/core/permissions/perms-allowlist)
format; it does not grant signature-only permissions or add signature-spoofing
support to a ROM.

## File format

Files contain `Key=Value` lines. Single and double quotes around complete values
are optional. Blank lines and full-line `#` comments are accepted. Values are
data: shell substitutions, environment expansion, and commands are never run.
Unknown keys, duplicate keys, malformed booleans, and duplicate app IDs are errors.

| Key | Meaning |
| --- | --- |
| `ID` | Required bundle ID, 1–64 characters: lowercase letters, numbers, dots and hyphens; starts with a letter or number; no `..`. |
| `Name` | Required display name. |
| `Description` | Optional search/description text. |
| `URL` | Required `https://github.com/OWNER/REPO/releases` or `https://gitlab.com/NAMESPACE/PROJECT/-/releases` URL. GitLab.com supports nested namespaces. A trailing slash is optional. |
| `APKs` | Required comma-separated APK filename patterns. `*` and `?` wildcards are supported; paths are forbidden. |
| `AvoidKeywords` | Optional comma-separated, case-insensitive substrings to exclude from APK filenames. |
| `UseTags` | `true` by default: inspect up to 100 releases and choose the most recently published eligible release by tag. `false`: use the provider's latest release endpoint. Git tags without release assets are not installable. |
| `Tag` | Optional exact release tag to pin; takes precedence over release discovery. |
| `IncludePrereleases` | GitHub only. Defaults to `false`. Set `true` to allow prereleases with `UseTags=true` or an explicit `Tag`. GitHub drafts and upcoming GitLab releases are excluded. GitLab's API treats published prerelease tags like other published releases. |

An APK is offered if it matches any configured pattern and passes the keyword
filter. Overlapping patterns list the same asset once. A release needs at least
one matching APK. If the default release has no matching APKs, the update fails
and the previously cached release remains available.

The provider must supply a SHA256 digest and a positive size for every selected asset.
APM verifies both against the downloaded file before publishing any module
changes. APM obtains these digests from the configured project over HTTPS,
including through redirects. Metadata is limited to 16 MiB and each APK to 1 GiB.
Providers apply their unauthenticated API rate limits.

## Installation and updates

App bundles are staged in their own enabled AMS module:

```text
/data/ams/modules/apm-app-<id>/
  module-info.json
  state.json
  overlay/system/app/<actual.android.package>/base.apk
```

Choosing `priv-app` changes the destination directory and adds the permissions
XML. A reboot is required for new, updated, moved, or removed system apps.
`apm list` labels staged app bundles and their destinations. The list records
staged installation state; it does not claim Android has loaded the app yet.

After `apm update`, repeat `apm app-install <id>` to stage a newer bundle. Use the
options flag again to select `priv-app` or custom permissions; an invocation
without the flag selects the default `/system/app` destination. An unchanged
release and identical options produce an already-staged result.
`apm upgrade` remains a Debian/Termux operation.

All selected APKs are downloaded, checked, and parsed before replacing the
bundle's module. A new selection replaces the staged set for that repository,
so previously staged APKs omitted from the selection are removed on reboot.
Select at most one APK per Android package when a release offers variants.
Failed downloads or validation leave the previous module intact.
Module replacement and installed-state updates roll back on write errors.
The confirmed plan is checked again before installation so a changed release or
options cannot be applied under an earlier confirmation. Existing unrelated
modules are never overwritten, and two managed app bundles cannot own the same
Android package ID.

## Removal

```sh
apm app-remove microg
apm remove-repo MicroG.apprepo
```

`app-remove` removes the staged AMS module and app bundle record. Reboot to apply
removal. Android user data and separately installed APK updates are preserved.
Removing a repository definition does not uninstall its apps. Use the filename
with its extension if both a `.repo` and `.apprepo` share a basename.

`app-install` and `app-remove` require an authenticated session and a root daemon
on Android. Emulator mode supports isolated staging tests without mounting
Android partitions or running Android's package manager.

## Regression checks

On a native build with host BoringSSL prebuilts:

```sh
cmake -S . -B build-tests -G Ninja -DAPM_EMULATOR_MODE=ON -DAPM_BUILD_TESTS=ON
cmake --build build-tests --parallel "$(nproc)"
ctest --test-dir build-tests --output-on-failure
```

The suite uses generated APK fixtures and a deterministic download stub. It
covers parser rejection, mixed sources, release selection, cache preservation,
SHA256 vectors, compressed/UTF-16 manifests, install planning, integrity checks,
permission XML, module rollback, and app/Debian state separation. These tests do
not replace boot and runtime validation on an Android device.
