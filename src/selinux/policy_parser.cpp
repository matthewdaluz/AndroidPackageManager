/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: policy_parser.cpp
 * Purpose: Implement a minimal SELinux binary policy reader/writer used by
 * apm-policy.
 *
 * Last Modified: November 24th, 2025. - 10:20 AM Eastern Time
 *
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

#include "policy_parser.hpp"

#include "fs.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>
#include <limits>

namespace apm::selinux {

namespace {

constexpr std::uint32_t MAPSIZE = 64;

// ---------------------------------------------------------------------------
// Byte helpers
// ---------------------------------------------------------------------------

class ByteReader {
public:
  explicit ByteReader(const std::vector<std::uint8_t> &data,
                      std::size_t off = 0)
      : bytes(data), offset(off) {}

  bool readU8(std::uint8_t &out) {
    if (offset >= bytes.size())
      return false;
    out = bytes[offset++];
    return true;
  }

  bool readU16(std::uint16_t &out) {
    if (offset + 2 > bytes.size())
      return false;
    out = static_cast<std::uint16_t>(bytes[offset]) |
          static_cast<std::uint16_t>(bytes[offset + 1]) << 8;
    offset += 2;
    return true;
  }

  bool readU32(std::uint32_t &out) {
    if (offset + 4 > bytes.size())
      return false;
    out = static_cast<std::uint32_t>(bytes[offset]) |
          (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
          (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
          (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
    offset += 4;
    return true;
  }

  bool readString(std::size_t len, std::string &out) {
    if (offset + len > bytes.size())
      return false;
    out.assign(reinterpret_cast<const char *>(&bytes[offset]), len);
    offset += len;
    return true;
  }

  bool skip(std::size_t len) {
    if (offset + len > bytes.size())
      return false;
    offset += len;
    return true;
  }

  std::size_t position() const { return offset; }
  std::size_t remaining() const { return bytes.size() - offset; }

private:
  const std::vector<std::uint8_t> &bytes;
  std::size_t offset;
};

class ByteWriter {
public:
  void writeU8(std::uint8_t value) { data.push_back(value); }

  void writeU16(std::uint16_t value) {
    data.push_back(static_cast<std::uint8_t>(value & 0xFF));
    data.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
  }

  void writeU32(std::uint32_t value) {
    data.push_back(static_cast<std::uint8_t>(value & 0xFF));
    data.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    data.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    data.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
  }

  void writeBytes(const std::vector<std::uint8_t> &bytes) {
    data.insert(data.end(), bytes.begin(), bytes.end());
  }

  void writeBytes(const std::string &bytes) {
    data.insert(data.end(), bytes.begin(), bytes.end());
  }

  const std::vector<std::uint8_t> &bytes() const { return data; }
  std::vector<std::uint8_t> &&take() { return std::move(data); }

private:
  std::vector<std::uint8_t> data;
};

std::uint32_t peekU32LE(const std::vector<std::uint8_t> &data,
                        std::size_t offset) {
  if (offset + 4 > data.size())
    return 0;
  return static_cast<std::uint32_t>(data[offset]) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

// ---------------------------------------------------------------------------
// Bitmap helpers
// ---------------------------------------------------------------------------

bool readEbitmap(ByteReader &reader, EBitmap &out) {
  out.nodes.clear();
  out.highBit = 0;

  std::uint32_t mapSize = 0;
  std::uint32_t highBit = 0;
  std::uint32_t count = 0;
  if (!reader.readU32(mapSize) || !reader.readU32(highBit) ||
      !reader.readU32(count)) {
    return false;
  }

  if (mapSize != MAPSIZE) {
    return false;
  }

  out.highBit = highBit;
  if (highBit == 0) {
    return true;
  }

  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint32_t start = 0;
    std::uint64_t map = 0;
    if (!reader.readU32(start))
      return false;
    std::uint32_t low = 0, high = 0;
    if (!reader.readU32(low) || !reader.readU32(high))
      return false;
    map = static_cast<std::uint64_t>(low) |
          (static_cast<std::uint64_t>(high) << 32);

    if (map == 0)
      return false;

    out.nodes.push_back({start, map});
  }

  return true;
}

void ensureHighBit(EBitmap &map, std::uint32_t bit) {
  std::uint32_t ceilBit = ((bit / MAPSIZE) + 1) * MAPSIZE;
  if (ceilBit > map.highBit)
    map.highBit = ceilBit;
}

bool setBit(EBitmap &map, std::uint32_t bit, bool value) {
  std::uint32_t bucket = (bit / MAPSIZE) * MAPSIZE;
  auto it = std::find_if(
      map.nodes.begin(), map.nodes.end(),
      [bucket](const EBitmapNode &n) { return n.startBit == bucket; });

  if (it == map.nodes.end()) {
    if (!value)
      return true;
    EBitmapNode node{};
    node.startBit = bucket;
    node.map = 0;
    map.nodes.push_back(node);
    it = std::prev(map.nodes.end());
    std::sort(map.nodes.begin(), map.nodes.end(),
              [](const EBitmapNode &a, const EBitmapNode &b) {
                return a.startBit < b.startBit;
              });
    it = std::find_if(
        map.nodes.begin(), map.nodes.end(),
        [bucket](const EBitmapNode &n) { return n.startBit == bucket; });
  }

  std::uint32_t offset = bit - bucket;
  if (offset >= MAPSIZE)
    return false;

  if (value) {
    it->map |= (1ULL << offset);
    ensureHighBit(map, bit);
  } else {
    it->map &= ~(1ULL << offset);
    if (it->map == 0) {
      map.nodes.erase(it);
      if (map.nodes.empty()) {
        map.highBit = 0;
      }
    }
  }
  return true;
}

bool getBit(const EBitmap &map, std::uint32_t bit) {
  for (const auto &node : map.nodes) {
    if (bit >= node.startBit && bit < node.startBit + MAPSIZE) {
      std::uint32_t offset = bit - node.startBit;
      return (node.map >> offset) & 1ULL;
    }
  }
  return false;
}

void writeEbitmap(const EBitmap &map, ByteWriter &writer) {
  writer.writeU32(MAPSIZE);
  writer.writeU32(map.highBit);
  writer.writeU32(static_cast<std::uint32_t>(map.nodes.size()));
  for (const auto &node : map.nodes) {
    writer.writeU32(node.startBit);
    writer.writeU32(static_cast<std::uint32_t>(node.map & 0xFFFFFFFFULL));
    writer.writeU32(
        static_cast<std::uint32_t>((node.map >> 32) & 0xFFFFFFFFULL));
  }
}

// ---------------------------------------------------------------------------
// Symtab readers we care about
// ---------------------------------------------------------------------------

bool readCommon(ByteReader &reader, CommonInfo &out) {
  std::uint32_t len = 0, value = 0, permPrim = 0, permCount = 0;
  if (!reader.readU32(len) || !reader.readU32(value) ||
      !reader.readU32(permPrim) || !reader.readU32(permCount)) {
    return false;
  }

  if (!reader.readString(len, out.name))
    return false;
  out.value = value;

  for (std::uint32_t i = 0; i < permCount; ++i) {
    std::uint32_t permLen = 0, permVal = 0;
    if (!reader.readU32(permLen) || !reader.readU32(permVal))
      return false;
    std::string permName;
    if (!reader.readString(permLen, permName))
      return false;
    std::string key = permName;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    out.perms.emplace(key, permVal);
  }

  return true;
}

bool readClass(ByteReader &reader, std::uint32_t policyVersion,
               const std::vector<CommonInfo> &commons, ClassInfo &out) {
  std::uint32_t len = 0, len2 = 0, value = 0, permPrim = 0, permCount = 0;
  std::uint32_t ncons = 0;
  if (!reader.readU32(len) || !reader.readU32(len2) || !reader.readU32(value) ||
      !reader.readU32(permPrim) || !reader.readU32(permCount) ||
      !reader.readU32(ncons)) {
    return false;
  }

  out.value = value;
  if (!reader.readString(len, out.name))
    return false;

  if (len2) {
    if (!reader.readString(len2, out.commonName))
      return false;
  }

  for (std::uint32_t i = 0; i < permCount; ++i) {
    std::uint32_t permLen = 0, permVal = 0;
    if (!reader.readU32(permLen) || !reader.readU32(permVal))
      return false;
    std::string permName;
    if (!reader.readString(permLen, permName))
      return false;
    std::string key = permName;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    out.perms.emplace(key, permVal);
  }

  // Constraints
  for (std::uint32_t i = 0; i < ncons; ++i) {
    std::uint32_t permsMask = 0, exprCount = 0;
    if (!reader.readU32(permsMask) || !reader.readU32(exprCount))
      return false;
    for (std::uint32_t j = 0; j < exprCount; ++j) {
      std::uint32_t exprType = 0, attr = 0, op = 0;
      if (!reader.readU32(exprType) || !reader.readU32(attr) ||
          !reader.readU32(op)) {
        return false;
      }
      if (exprType == 4) { // CEXPR_NAMES
        EBitmap names;
        if (!readEbitmap(reader, names))
          return false;
        if (policyVersion >= 29) {
          // type_set_t: two ebitmaps + flags
          EBitmap types;
          if (!readEbitmap(reader, types))
            return false;
          EBitmap negset;
          if (!readEbitmap(reader, negset))
            return false;
          std::uint32_t flags = 0;
          if (!reader.readU32(flags))
            return false;
        }
      }
    }
  }

  // Validatetrans constraints
  if (policyVersion >= 19) {
    std::uint32_t vtCount = 0;
    if (!reader.readU32(vtCount))
      return false;
    for (std::uint32_t i = 0; i < vtCount; ++i) {
      std::uint32_t permsMask = 0, exprCount = 0;
      if (!reader.readU32(permsMask) || !reader.readU32(exprCount))
        return false;
      for (std::uint32_t j = 0; j < exprCount; ++j) {
        std::uint32_t exprType = 0, attr = 0, op = 0;
        if (!reader.readU32(exprType) || !reader.readU32(attr) ||
            !reader.readU32(op)) {
          return false;
        }
        if (exprType == 4) {
          EBitmap names;
          if (!readEbitmap(reader, names))
            return false;
          if (policyVersion >= 29) {
            EBitmap types;
            if (!readEbitmap(reader, types))
              return false;
            EBitmap negset;
            if (!readEbitmap(reader, negset))
              return false;
            std::uint32_t flags = 0;
            if (!reader.readU32(flags))
              return false;
          }
        }
      }
    }
  }

  if (policyVersion >= 27) {
    // default_user, default_role, default_range
    std::uint32_t tmp = 0;
    if (!reader.readU32(tmp) || !reader.readU32(tmp) || !reader.readU32(tmp))
      return false;
  }

  if (policyVersion >= 28) {
    std::uint32_t tmp = 0;
    if (!reader.readU32(tmp))
      return false;
  }

  return true;
}

bool readRole(ByteReader &reader, std::uint32_t policyVersion) {
  std::uint32_t len = 0, value = 0, bounds = 0;
  std::uint32_t toRead = policyVersion >= POLICYDB_VERSION_BOUNDARY ? 3 : 2;
  if (!reader.readU32(len) || !reader.readU32(value))
    return false;
  if (toRead == 3 && !reader.readU32(bounds))
    return false;
  if (!reader.skip(len))
    return false;

  EBitmap dominates;
  if (!readEbitmap(reader, dominates))
    return false;
  EBitmap types;
  if (!readEbitmap(reader, types))
    return false;
  return true;
}

bool readUser(ByteReader &reader, std::uint32_t policyVersion, bool mls) {
  std::uint32_t len = 0, value = 0, bounds = 0;
  std::uint32_t toRead = policyVersion >= POLICYDB_VERSION_BOUNDARY ? 3 : 2;
  if (!reader.readU32(len) || !reader.readU32(value))
    return false;
  if (toRead == 3 && !reader.readU32(bounds))
    return false;
  if (!reader.skip(len))
    return false;

  EBitmap roles;
  if (!readEbitmap(reader, roles))
    return false;

  if (mls) {
    std::uint32_t items = 0;
    if (!reader.readU32(items))
      return false;
    if (items == 0 || items > 2)
      return false;
    for (std::uint32_t i = 0; i < items; ++i) {
      std::uint32_t sens = 0;
      if (!reader.readU32(sens))
        return false;
    }
    EBitmap lowCats;
    if (!readEbitmap(reader, lowCats))
      return false;
    EBitmap highCats;
    if (!readEbitmap(reader, highCats))
      return false;
    std::uint32_t dfltSens = 0;
    if (!reader.readU32(dfltSens))
      return false;
    EBitmap dfltCats;
    if (!readEbitmap(reader, dfltCats))
      return false;
  }
  return true;
}

bool readBool(ByteReader &reader, std::uint32_t policyVersion) {
  std::uint32_t val = 0, state = 0, len = 0;
  if (!reader.readU32(val) || !reader.readU32(state) || !reader.readU32(len))
    return false;
  if (!reader.skip(len))
    return false;
  return true;
}

bool readSens(ByteReader &reader) {
  std::uint32_t len = 0, isalias = 0;
  if (!reader.readU32(len) || !reader.readU32(isalias))
    return false;
  if (!reader.skip(len))
    return false;
  std::uint32_t sensVal = 0;
  if (!reader.readU32(sensVal))
    return false;
  EBitmap cats;
  if (!readEbitmap(reader, cats))
    return false;
  return true;
}

bool readCat(ByteReader &reader) {
  std::uint32_t len = 0, value = 0, isalias = 0;
  if (!reader.readU32(len) || !reader.readU32(value) ||
      !reader.readU32(isalias))
    return false;
  if (!reader.skip(len))
    return false;
  return true;
}

bool readType(ByteReader &reader, std::uint32_t policyVersion, TypeInfo &out) {
  std::uint32_t len = 0, value = 0, properties = 0, bounds = 0;
  if (policyVersion < POLICYDB_VERSION_BOUNDARY) {
    return false; // unsupported legacy format
  }

  if (!reader.readU32(len) || !reader.readU32(value) ||
      !reader.readU32(properties) || !reader.readU32(bounds)) {
    return false;
  }

  out.value = value;
  out.properties = properties;
  out.bounds = bounds;
  out.isAttribute = (properties & TYPEDATUM_PROPERTY_ATTRIBUTE) != 0;
  if (!reader.readString(len, out.name))
    return false;
  return true;
}

// ---------------------------------------------------------------------------
// Avtab parsing/writing
// ---------------------------------------------------------------------------

bool readAvtabEntry(ByteReader &reader, std::uint32_t policyVersion,
                    AvRuleEntry &out) {
  std::uint16_t src = 0, tgt = 0, cls = 0, specified = 0;
  if (!reader.readU16(src) || !reader.readU16(tgt) || !reader.readU16(cls) ||
      !reader.readU16(specified)) {
    return false;
  }

  out.src = src;
  out.tgt = tgt;
  out.tclass = cls;
  out.specified = specified;

  if ((specified & AVTAB_XPERMS) != 0) {
    AvtabXperms x{};
    std::uint8_t spec = 0, driver = 0;
    if (!reader.readU8(spec) || !reader.readU8(driver))
      return false;
    x.specified = spec;
    x.driver = driver;
    for (int i = 0; i < 8; ++i) {
      std::uint32_t permWord = 0;
      if (!reader.readU32(permWord))
        return false;
      x.perms[i] = permWord;
    }
    out.xperms = x;
  } else {
    if (!reader.readU32(out.data))
      return false;
  }
  return true;
}

void writeAvtabEntry(const AvRuleEntry &entry, ByteWriter &writer) {
  writer.writeU16(entry.src);
  writer.writeU16(entry.tgt);
  writer.writeU16(entry.tclass);
  writer.writeU16(entry.specified);
  if (entry.xperms.has_value()) {
    const auto &xp = entry.xperms.value();
    writer.writeU8(xp.specified);
    writer.writeU8(xp.driver);
    for (int i = 0; i < 8; ++i) {
      writer.writeU32(xp.perms[i]);
    }
  } else {
    writer.writeU32(entry.data);
  }
}

} // namespace

// ---------------------------------------------------------------------------
// PolicyParser implementation
// ---------------------------------------------------------------------------

bool PolicyParser::load(const std::string &path, std::string &error) {
  data_ = PolicyData{};

  std::string raw;
  if (!apm::fs::readFile(path, raw)) {
    error = "Failed to read policy from " + path;
    return false;
  }
  std::vector<std::uint8_t> bytes(raw.begin(), raw.end());
  ByteReader reader(bytes);

  std::uint32_t magic = 0, strLen = 0;
  if (!reader.readU32(magic) || !reader.readU32(strLen)) {
    error = "Policy file too small";
    return false;
  }
  if (magic != POLICYDB_MAGIC) {
    error = "Unsupported policy magic";
    return false;
  }

  std::string ident;
  if (!reader.readString(strLen, ident)) {
    error = "Failed to read policy identifier";
    return false;
  }

  std::uint32_t version = 0, config = 0, symNum = 0, oconNum = 0;
  if (!reader.readU32(version) || !reader.readU32(config) ||
      !reader.readU32(symNum) || !reader.readU32(oconNum)) {
    error = "Truncated policy header";
    return false;
  }

  if (symNum != SYM_NUM) {
    error = "Unexpected symbol table count";
    return false;
  }

  if (version < POLICYDB_VERSION_AVTAB || version > POLICYDB_VERSION_MAX) {
    error = "Unsupported policy version " + std::to_string(version);
    return false;
  }

  data_.version = version;
  data_.mls = (config & 0x1) != 0;

  // Optional policycaps/permissive map
  if (version >= POLICYDB_VERSION_POLCAP) {
    EBitmap caps;
    if (!readEbitmap(reader, caps)) {
      error = "Failed to read policycaps";
      return false;
    }
  }

  if (version >= POLICYDB_VERSION_PERMISSIVE) {
    EBitmap permissive;
    if (!readEbitmap(reader, permissive)) {
      error = "Failed to read permissive map";
      return false;
    }
  }

  const std::size_t symtabStart = reader.position();

  for (std::uint32_t tab = 0; tab < symNum; ++tab) {
    std::size_t blockStart = reader.position();
    std::uint32_t nprim = 0, nel = 0;
    if (!reader.readU32(nprim) || !reader.readU32(nel)) {
      error = "Failed to read symbol table counts";
      return false;
    }

    if (tab == SYM_COMMONS) {
      for (std::uint32_t i = 0; i < nel; ++i) {
        CommonInfo info;
        if (!readCommon(reader, info)) {
          error = "Failed to parse common table";
          return false;
        }
        data_.commonIndex.emplace(info.name, data_.commons.size());
        data_.commons.push_back(std::move(info));
      }
    } else if (tab == SYM_CLASSES) {
      for (std::uint32_t i = 0; i < nel; ++i) {
        ClassInfo info;
        if (!readClass(reader, version, data_.commons, info)) {
          error = "Failed to parse class table";
          return false;
        }
        data_.classIndex.emplace(info.name, data_.classes.size());
        data_.classes.push_back(std::move(info));
      }
    } else if (tab == SYM_TYPES) {
      for (std::uint32_t i = 0; i < nel; ++i) {
        TypeInfo info;
        if (!readType(reader, version, info)) {
          error = "Failed to parse type table";
          return false;
        }
        data_.typeIndex.emplace(info.name, data_.types.size());
        data_.types.push_back(std::move(info));
      }
    } else {
      // Skip other tables.
      for (std::uint32_t i = 0; i < nel; ++i) {
        switch (tab) {
        case 2:
          if (!readRole(reader, version)) {
            error = "Failed to skip role table";
            return false;
          }
          break;
        case 4:
          if (!readUser(reader, version, data_.mls)) {
            error = "Failed to skip user table";
            return false;
          }
          break;
        case 5:
          if (!readBool(reader, version)) {
            error = "Failed to skip bool table";
            return false;
          }
          break;
        case 6:
          if (!readSens(reader)) {
            error = "Failed to skip sensitivity table";
            return false;
          }
          break;
        case 7:
          if (!readCat(reader)) {
            error = "Failed to skip category table";
            return false;
          }
          break;
        default:
          error = "Unknown symbol table index";
          return false;
        }
      }
    }

    std::size_t blockEnd = reader.position();
    if (tab != SYM_TYPES) {
      data_.symtabRaw[tab].assign(bytes.begin() + blockStart,
                                  bytes.begin() + blockEnd);
    }
  }

  // AVTAB
  std::uint32_t avCount = 0;
  if (!reader.readU32(avCount)) {
    error = "Failed to read avtab count";
    return false;
  }
  for (std::uint32_t i = 0; i < avCount; ++i) {
    AvRuleEntry entry;
    if (!readAvtabEntry(reader, version, entry)) {
      error = "Failed to parse avtab entry";
      return false;
    }
    data_.avtab.push_back(std::move(entry));
  }

  const std::size_t avtabEnd = reader.position();

  // Locate type_attr_map by scanning for the block of ebitmaps at the tail.
  std::size_t attrMapStart = std::numeric_limits<std::size_t>::max();
  const std::uint32_t typeCount =
      static_cast<std::uint32_t>(data_.types.size());
  const std::size_t minAttrSize = static_cast<std::size_t>(typeCount) * 12;
  if (minAttrSize > bytes.size()) {
    error = "Policy too small for attribute map";
    return false;
  }

  for (std::size_t off = avtabEnd;
       off + minAttrSize <= bytes.size() && off <= bytes.size(); off += 4) {
    if (peekU32LE(bytes, off) != MAPSIZE)
      continue;
    ByteReader trial(bytes, off);
    std::vector<EBitmap> maps;
    maps.reserve(typeCount);
    bool ok = true;
    for (std::uint32_t i = 0; i < typeCount; ++i) {
      EBitmap map;
      if (!readEbitmap(trial, map)) {
        ok = false;
        break;
      }
      maps.push_back(std::move(map));
    }
    if (ok && trial.position() == bytes.size()) {
      attrMapStart = off;
      data_.typeAttrMap = std::move(maps);
      break;
    }
  }

  if (attrMapStart == std::numeric_limits<std::size_t>::max()) {
    error = "Failed to locate type_attr_map section";
    return false;
  }

  data_.prefix.assign(bytes.begin(), bytes.begin() + symtabStart);
  data_.midBlock.assign(bytes.begin() + avtabEnd, bytes.begin() + attrMapStart);

  // Ensure attr map entries include self bits.
  for (std::size_t i = 0; i < data_.typeAttrMap.size(); ++i) {
    setBit(data_.typeAttrMap[i], static_cast<std::uint32_t>(i), true);
  }

  rebuildTypeIndex();
  return true;
}

bool PolicyParser::save(const std::string &path, std::string &error) const {
  ByteWriter writer;
  writer.writeBytes(data_.prefix);

  for (std::uint32_t tab = 0; tab < SYM_NUM; ++tab) {
    if (tab != SYM_TYPES) {
      writer.writeBytes(data_.symtabRaw[tab]);
      continue;
    }

    const std::uint32_t nprim = static_cast<std::uint32_t>(data_.types.size());
    writer.writeU32(nprim);
    writer.writeU32(nprim);
    for (const auto &type : data_.types) {
      writer.writeU32(static_cast<std::uint32_t>(type.name.size()));
      writer.writeU32(type.value);
      writer.writeU32(type.properties);
      writer.writeU32(type.bounds);
      writer.writeBytes(type.name);
    }
  }

  writer.writeU32(static_cast<std::uint32_t>(data_.avtab.size()));
  for (const auto &entry : data_.avtab) {
    writeAvtabEntry(entry, writer);
  }

  writer.writeBytes(data_.midBlock);

  for (const auto &map : data_.typeAttrMap) {
    writeEbitmap(map, writer);
  }

  std::string out(reinterpret_cast<const char *>(writer.bytes().data()),
                  writer.bytes().size());
  if (!apm::fs::writeFile(path, out, false)) {
    error = "Failed to write updated policy to " + path;
    return false;
  }

  return true;
}

void PolicyParser::rebuildTypeIndex() {
  data_.typeIndex.clear();
  for (std::size_t i = 0; i < data_.types.size(); ++i) {
    data_.typeIndex[data_.types[i].name] = i;
  }
}

bool PolicyParser::ensureAttrMapSize() {
  if (data_.typeAttrMap.size() < data_.types.size()) {
    data_.typeAttrMap.resize(data_.types.size());
  }
  return data_.typeAttrMap.size() == data_.types.size();
}

bool PolicyParser::ensureType(const std::string &name, bool isAttribute,
                              std::string &error) {
  rebuildTypeIndex();
  if (data_.typeIndex.find(name) != data_.typeIndex.end())
    return true;

  TypeInfo info;
  info.name = name;
  info.value = static_cast<std::uint32_t>(data_.types.size() + 1);
  info.properties = TYPEDATUM_PROPERTY_PRIMARY;
  if (isAttribute)
    info.properties |= TYPEDATUM_PROPERTY_ATTRIBUTE;
  info.isAttribute = isAttribute;
  info.bounds = 0;

  data_.types.push_back(info);
  rebuildTypeIndex();

  ensureAttrMapSize();
  EBitmap map;
  setBit(map, info.value - 1, true);
  data_.typeAttrMap[info.value - 1] = map;
  return true;
}

bool PolicyParser::assignAttribute(const std::string &typeName,
                                   const std::string &attributeName,
                                   std::string &error) {
  if (!ensureAttrMapSize()) {
    error = "Attribute map sizing failed";
    return false;
  }

  if (!ensureType(attributeName, true, error))
    return false;
  if (!ensureType(typeName, false, error))
    return false;

  rebuildTypeIndex();
  auto tIt = data_.typeIndex.find(typeName);
  auto aIt = data_.typeIndex.find(attributeName);
  if (tIt == data_.typeIndex.end() || aIt == data_.typeIndex.end()) {
    error = "Failed to resolve type/attribute names";
    return false;
  }

  std::uint32_t typeIdx = static_cast<std::uint32_t>(tIt->second);
  std::uint32_t attrIdx = static_cast<std::uint32_t>(aIt->second);

  setBit(data_.typeAttrMap[typeIdx], attrIdx, true);
  return true;
}

std::uint32_t PolicyParser::buildPermMask(const ClassInfo &cls,
                                          const std::vector<std::string> &perms,
                                          std::string &error) const {
  std::uint32_t mask = 0;

  auto toLower = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return s;
  };

  for (const auto &perm : perms) {
    std::string key = toLower(perm);
    auto it = cls.perms.find(key);
    if (it == cls.perms.end()) {
      // Try common permissions if present.
      if (!cls.commonName.empty()) {
        auto cIt = data_.commonIndex.find(cls.commonName);
        if (cIt != data_.commonIndex.end()) {
          const auto &common = data_.commons[cIt->second];
          auto pIt = common.perms.find(key);
          if (pIt != common.perms.end()) {
            std::uint32_t bit = pIt->second;
            if (bit == 0 || bit > 32) {
              error = "Invalid permission bit index for " + perm;
              return 0;
            }
            mask |= (1U << (bit - 1));
            continue;
          }
        }
      }
      error = "Unknown permission " + perm + " for class " + cls.name;
      return 0;
    }

    std::uint32_t bit = it->second;
    if (bit == 0 || bit > 32) {
      error = "Invalid permission bit index for " + perm;
      return 0;
    }
    mask |= (1U << (bit - 1));
  }

  return mask;
}

std::uint16_t PolicyParser::findClass(const std::string &name,
                                      std::string &error) const {
  auto it = data_.classIndex.find(name);
  if (it == data_.classIndex.end()) {
    error = "Unknown class " + name;
    return 0;
  }
  const auto &cls = data_.classes[it->second];
  return static_cast<std::uint16_t>(cls.value);
}

std::vector<std::uint16_t>
PolicyParser::expandTypeSet(const std::string &name, std::string &error) const {
  std::vector<std::uint16_t> out;
  auto it = data_.typeIndex.find(name);
  if (it == data_.typeIndex.end()) {
    error = "Unknown type or attribute " + name;
    return out;
  }

  const auto &entry = data_.types[it->second];
  if (!entry.isAttribute) {
    out.push_back(static_cast<std::uint16_t>(entry.value));
    return out;
  }

  std::uint32_t attrBit = entry.value - 1;
  for (std::size_t i = 0; i < data_.types.size(); ++i) {
    if (data_.types[i].isAttribute)
      continue;
    if (getBit(data_.typeAttrMap[i], attrBit)) {
      out.push_back(static_cast<std::uint16_t>(data_.types[i].value));
    }
  }
  return out;
}

bool PolicyParser::addAvRule(const std::string &srcName,
                             const std::string &tgtName,
                             const std::string &className,
                             const std::vector<std::string> &perms,
                             bool dontAudit, std::string &error) {
  auto clsIt = data_.classIndex.find(className);
  if (clsIt == data_.classIndex.end()) {
    error = "Unknown class " + className;
    return false;
  }
  const auto &cls = data_.classes[clsIt->second];

  auto srcTypes = expandTypeSet(srcName, error);
  if (!error.empty() || srcTypes.empty())
    return false;
  error.clear();
  auto tgtTypes = expandTypeSet(tgtName, error);
  if (!error.empty() || tgtTypes.empty())
    return false;
  error.clear();

  std::uint32_t permMask = buildPermMask(cls, perms, error);
  if (!error.empty() || permMask == 0)
    return false;

  std::uint16_t specified = dontAudit ? AVTAB_AUDITDENY : AVTAB_ALLOWED;

  for (auto src : srcTypes) {
    for (auto tgt : tgtTypes) {
      bool merged = false;
      for (auto &entry : data_.avtab) {
        if (entry.src == src && entry.tgt == tgt && entry.tclass == cls.value &&
            entry.specified == specified && !entry.xperms.has_value()) {
          entry.data |= permMask;
          merged = true;
          break;
        }
      }
      if (!merged) {
        AvRuleEntry entry{};
        entry.src = src;
        entry.tgt = tgt;
        entry.tclass = static_cast<std::uint16_t>(cls.value);
        entry.specified = specified;
        entry.data = permMask;
        data_.avtab.push_back(entry);
      }
    }
  }
  return true;
}

} // namespace apm::selinux
