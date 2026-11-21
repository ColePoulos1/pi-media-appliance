/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/Variant.h"

#include <cstdint>
#include <string>
#include <vector>

class CFileItem;

namespace KODI::APPLIANCE
{
inline constexpr char SOURCE_TOKEN[] = "appliance.sourceToken";

struct Title
{
  std::string path;
  std::string key;
  std::string episode;
};

struct RemoteFile
{
  int64_t torrentId{-1};
  int64_t id{-1};
  uint64_t bytes{0};
};

Title GetTitle(const CFileItem& item);
CVariant LoadIndexEntry(const Title& title);
CVariant LoadProviders();
std::vector<std::string> LocalFiles(const CVariant& entry);
RemoteFile SelectFile(const CVariant& files, const CVariant& entry);
RemoteFile Discover(const CVariant& entry, const CVariant& providers);
std::string Resolve(const CVariant& entry, const CVariant& providers, const RemoteFile& file);
} // namespace KODI::APPLIANCE
