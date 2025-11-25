/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: policy_parser.hpp
 * Purpose: Define a minimal SELinux policy loader/editor used by apm-policy.
 * Last Modified: February 18th, 2025.
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

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace apm::selinux {

// ---------------------------------------------------------------------------
// Basic constants pulled from the upstream SELinux headers.
// ---------------------------------------------------------------------------

constexpr std::uint32_t POLICYDB_MAGIC = 0xf97cff8f;
constexpr std::uint32_t POLICYDB_VERSION_BOUNDARY = 24;
constexpr std::uint32_t POLICYDB_VERSION_AVTAB = 20;
constexpr std::uint32_t POLICYDB_VERSION_POLCAP = 22;
constexpr std::uint32_t POLICYDB_VERSION_PERMISSIVE = 23;
constexpr std::uint32_t POLICYDB_VERSION_MAX = 35;

constexpr std::uint32_t SYM_NUM = 8;
constexpr std::uint32_t SYM_COMMONS = 0;
constexpr std::uint32_t SYM_CLASSES = 1;
constexpr std::uint32_t SYM_TYPES = 3;

constexpr std::uint16_t AVTAB_ALLOWED = 0x0001;
constexpr std::uint16_t AVTAB_AUDITALLOW = 0x0002;
constexpr std::uint16_t AVTAB_AUDITDENY = 0x0004;
constexpr std::uint16_t AVTAB_XPERMS_ALLOWED = 0x0100;
constexpr std::uint16_t AVTAB_XPERMS_AUDITALLOW = 0x0200;
constexpr std::uint16_t AVTAB_XPERMS_DONTAUDIT = 0x0400;
constexpr std::uint16_t AVTAB_XPERMS = 0x0F00;
constexpr std::uint16_t AVTAB_ENABLED = 0x8000;

constexpr std::uint32_t TYPEDATUM_PROPERTY_PRIMARY = 0x0001;
constexpr std::uint32_t TYPEDATUM_PROPERTY_ATTRIBUTE = 0x0002;

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

struct EBitmapNode {
  std::uint32_t startBit{};
  std::uint64_t map{};
};

struct EBitmap {
  std::uint32_t highBit{};
  std::vector<EBitmapNode> nodes;
};

struct CommonInfo {
  std::string name;
  std::uint32_t value{};
  std::unordered_map<std::string, std::uint32_t> perms;
};

struct ClassInfo {
  std::string name;
  std::uint32_t value{};
  std::string commonName;
  std::unordered_map<std::string, std::uint32_t> perms;
};

struct TypeInfo {
  std::string name;
  std::uint32_t value{};
  std::uint32_t properties{};
  std::uint32_t bounds{};
  bool isAttribute{};
};

struct AvtabXperms {
  std::uint8_t specified{};
  std::uint8_t driver{};
  std::uint32_t perms[8]{};
};

struct AvRuleEntry {
  std::uint16_t src{};
  std::uint16_t tgt{};
  std::uint16_t tclass{};
  std::uint16_t specified{};
  std::uint32_t data{};
  std::optional<AvtabXperms> xperms;
};

struct PolicyData {
  std::uint32_t version{};
  bool mls{};

  std::vector<std::uint8_t> prefix;
  std::vector<std::uint8_t> symtabRaw[SYM_NUM];

  std::vector<CommonInfo> commons;
  std::unordered_map<std::string, std::size_t> commonIndex;

  std::vector<ClassInfo> classes;
  std::unordered_map<std::string, std::size_t> classIndex;

  std::vector<TypeInfo> types;
  std::unordered_map<std::string, std::size_t> typeIndex;

  std::vector<AvRuleEntry> avtab;
  std::vector<std::uint8_t> midBlock;

  std::vector<EBitmap> typeAttrMap;
};

// ---------------------------------------------------------------------------
// Policy loader/editor
// ---------------------------------------------------------------------------

class PolicyParser {
public:
  bool load(const std::string &path, std::string &error);
  bool save(const std::string &path, std::string &error) const;

  bool ensureType(const std::string &name, bool isAttribute,
                  std::string &error);
  bool assignAttribute(const std::string &typeName,
                       const std::string &attributeName, std::string &error);
  bool addAvRule(const std::string &srcName, const std::string &tgtName,
                 const std::string &className,
                 const std::vector<std::string> &perms, bool dontAudit,
                 std::string &error);

  const PolicyData &data() const { return data_; }

private:
  PolicyData data_;

  // Helpers for rule expansion.
  std::vector<std::uint16_t>
  expandTypeSet(const std::string &name, std::string &error) const;
  std::uint16_t findClass(const std::string &name,
                          std::string &error) const;
  std::uint32_t buildPermMask(const ClassInfo &cls,
                              const std::vector<std::string> &perms,
                              std::string &error) const;

  void rebuildTypeIndex();
  bool ensureAttrMapSize();
};

} // namespace apm::selinux
