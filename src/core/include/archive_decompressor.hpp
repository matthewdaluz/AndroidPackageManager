// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>

namespace apm::archive {
struct PreparedTar {
  std::string path;
  std::string temporaryDirectory;
  PreparedTar() = default;
  PreparedTar(const PreparedTar &) = delete;
  PreparedTar &operator=(const PreparedTar &) = delete;
  ~PreparedTar();
};
// Leave plain tar files in place. Decode compressed inputs into a private
// temporary directory so system tar never needs an external decompressor.
bool prepareTar(const std::string &input, const std::string &cacheDirectory,
                std::uint64_t maxBytes, PreparedTar &output,
                std::string *errorMsg = nullptr);
} // namespace apm::archive
