/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JSONVariantParser.h"
#include "video/AppliancePlayback.h"

#include <gtest/gtest.h>

using KODI::APPLIANCE::LocalFiles;
using KODI::APPLIANCE::SelectFile;

TEST(TestAppliancePlayback, AcceptsOnlyVerifiedEpisodeAndKeepsExactFileBytes)
{
  CVariant episode;
  ASSERT_TRUE(CJSONVariantParser::Parse(R"({"episode":"s01e02","title":"Return","query":"Show"})",
                                        episode));
  CVariant pack;
  ASSERT_TRUE(CJSONVariantParser::Parse(R"([
    {"id":1,"name":"Show/Show.S01E02.Arrival.mkv","size":40000000000},
    {"id":2,"name":"Show/Show.S01E02.Return.Again.mkv","size":1900000000},
    {"id":3,"name":"Show/Show.S01E03.Return.mkv","size":1700000000},
    {"id":5,"name":"Other.S01E02.Return.mkv","size":1800000000}
  ])",
                                        pack));
  EXPECT_EQ(SelectFile(pack, episode).id, -1);

  CVariant confirmed;
  ASSERT_TRUE(CJSONVariantParser::Parse(
      R"({"id":4,"name":"Show/Show.S01E02.Return.1080p.mkv","size":1800000000})", confirmed));
  pack.push_back(confirmed);
  const auto selected = SelectFile(pack, episode);
  EXPECT_EQ(selected.id, 4);
  EXPECT_EQ(selected.bytes, 1800000000u);

  confirmed["id"] = 5;
  pack.push_back(confirmed);
  EXPECT_EQ(SelectFile(pack, episode).id, -1);
}

TEST(TestAppliancePlayback, UsesOneExactMovieFileAndRejectsInvalidByteCounts)
{
  CVariant movie;
  ASSERT_TRUE(CJSONVariantParser::Parse(R"json({"title":"Pilot Movie (2026)"})json", movie));
  CVariant files;
  ASSERT_TRUE(CJSONVariantParser::Parse(R"([
    {"id":0,"name":"Pilot.Movie.(2026).1080p.mkv","size":-1},
    {"id":1,"name":"Pilot.Movie.(2026).1080p.mkv","size":1.2},
    {"id":2,"name":"Pilot.Movie.(2026).1080p.mkv","size":7200000123}
  ])",
                                        files));
  const auto selected = SelectFile(files, movie);
  EXPECT_EQ(selected.id, 2);
  EXPECT_EQ(selected.bytes, 7200000123u);
}

TEST(TestAppliancePlayback, KeepsDistinctLocalFilesInIndexOrder)
{
  CVariant entry;
  ASSERT_TRUE(CJSONVariantParser::Parse(R"({"local":["smb://example.invalid/test/one.mkv",
    "https://example.invalid/not-local.mkv", "smb://example.invalid/test/one.mkv",
    "/test/two.mkv", "relative.mkv"]})",
                                        entry));
  EXPECT_EQ(LocalFiles(entry),
            (std::vector<std::string>{"smb://example.invalid/test/one.mkv", "/test/two.mkv"}));
}
