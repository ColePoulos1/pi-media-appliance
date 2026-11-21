/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JSONVariantParser.h"
#include "video/AppliancePlayback.h"

#include <algorithm>

#include <gtest/gtest.h>

using namespace KODI::APPLIANCE;
using namespace std::chrono_literals;

namespace
{
FileCandidate Remote(const TitleContext& title, char hashDigit, FileSize size)
{
  FileEvidence report;
  report.titleKey = title.key;
  report.size = size;
  report.resolution = 1080;
  report.codec = "h264";
  report.source = Source::WebDl;
  return {{Tier::Remote, std::string(40, hashDigit), "video.mkv"}, {}, {report}};
}
} // namespace

TEST(TestAppliancePlayback, BoundsRecoveryByAttemptsAndMonotonicTime)
{
  const PlaybackClock::time_point opened{};
  RetryBudget budget;
  EXPECT_TRUE(budget.CanTry(opened + 3h));
  const auto failed = opened + 3h;
  budget.Start(failed);
  budget.Start(failed + 10s);
  EXPECT_TRUE(budget.CanTry(failed + RETRY_TIME - 1ms));
  EXPECT_FALSE(budget.CanTry(failed + RETRY_TIME));
  budget.attempts = MAX_SOURCE_ATTEMPTS - 1;
  EXPECT_TRUE(budget.CanTry(failed));
  ++budget.attempts;
  EXPECT_FALSE(budget.CanTry(failed));
}

TEST(TestAppliancePlayback, ScopesQuickReplayAndCooldown)
{
  const PlaybackClock::time_point stopped{};
  PlaybackHistory history;
  const FileIdentity file{Tier::Local, {}, "synthetic.mkv"};
  history.Stopped(1, "movie", {stopped, file, 15000});
  const auto* recent = history.RecentStop(1, "movie", stopped + QUICK_REPLAY_WINDOW - 1ms);
  ASSERT_NE(recent, nullptr);
  EXPECT_EQ(recent->file.member, file.member);
  EXPECT_EQ(recent->position, 15000);
  EXPECT_EQ(history.RecentStop(1, "movie", stopped + QUICK_REPLAY_WINDOW), nullptr);
  EXPECT_EQ(history.RecentStop(1, "episode", stopped), nullptr);
  EXPECT_EQ(history.RecentStop(2, "movie", stopped), nullptr);
  history.Started(1, "movie");
  EXPECT_EQ(history.RecentStop(1, "movie", stopped), nullptr);

  const ResolverRoute route{"torbox", "11", "4"};
  history.CoolRoute(1, route, stopped);
  EXPECT_TRUE(history.RouteCoolingDown(1, route, stopped + ROUTE_COOLDOWN - 1ms));
  EXPECT_FALSE(history.RouteCoolingDown(1, route, stopped + ROUTE_COOLDOWN));
  EXPECT_FALSE(history.RouteCoolingDown(2, route, stopped));
  EXPECT_FALSE(history.RouteCoolingDown(1, {"torbox", "11", "7"}, stopped));
}

TEST(TestAppliancePlayback, PrefersLastWorkingFileOnlyAfterEligibility)
{
  const TitleContext title{"episode:tmdb:900000002", 40min};
  auto first = Remote(title, 'a', {SizeKind::Exact, 2400000000});
  auto second = Remote(title, 'b', {SizeKind::Exact, 1200000000});
  auto local = first;
  local.identity = {Tier::Local, {}, "local.mkv"};
  SelectionPreferences preferences;
  auto remote = RankFiles({first, second}, title, preferences, {});
  ASSERT_EQ(remote.size(), 2u);
  EXPECT_EQ(remote.front().file.identity.torrentHash, first.identity.torrentHash);
  const auto preferred = StableFileKey(second.identity);
  PreferWorkingFile(remote, preferred);
  EXPECT_EQ(remote.front().file.identity.torrentHash, second.identity.torrentHash);
  auto locals = RankFiles({local}, title, preferences, {});
  PreferWorkingFile(locals, preferred);
  ASSERT_EQ(locals.size(), 1u);
  EXPECT_EQ(locals.front().file.identity.member, local.identity.member);
  preferences.skipped.insert(second.identity);
  remote = RankFiles({first, second}, title, preferences, {});
  PreferWorkingFile(remote, preferred);
  ASSERT_EQ(remote.size(), 1u);
  EXPECT_EQ(remote.front().file.identity.torrentHash, first.identity.torrentHash);
}

TEST(TestAppliancePlayback, PrefersCredibleMovieEncodeInBalancedAndHighQuality)
{
  const TitleContext movie{"movie:tmdb:900000001", 7200s};
  auto bluray = Remote(movie, 'a', {SizeKind::Exact, 1200000000});
  bluray.evidence.front().source = Source::BluRay;
  const auto web = Remote(movie, 'b', {SizeKind::Exact, 7200000000});

  SelectionPreferences preferences;
  for (const auto profile : {Profile::Balanced, Profile::HighQuality})
  {
    preferences.profile = profile;
    const auto ranked = RankFiles({bluray, web}, movie, preferences, {});
    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_EQ(ranked.front().file.identity.torrentHash, web.identity.torrentHash);
    ASSERT_TRUE(ranked.front().mbPerMinute);
    EXPECT_DOUBLE_EQ(*ranked.front().mbPerMinute, 60.0);
  }

  bluray.evidence.front().size = web.evidence.front().size;
  for (const auto profile : {Profile::Balanced, Profile::HighQuality})
  {
    preferences.profile = profile;
    const auto ranked = RankFiles({web, bluray}, movie, preferences, {});
    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_EQ(ranked.front().file.identity.torrentHash, bluray.identity.torrentHash);
  }
}

TEST(TestAppliancePlayback, RanksAnimationByProfileWithBoundedBalancedSizeBenefit)
{
  const TitleContext episode{"episode:tmdb:900000002", 1320s, true};
  const auto small = Remote(episode, 'a', {SizeKind::Exact, 550000000});
  const auto large = Remote(episode, 'b', {SizeKind::Exact, 2200000000});
  const auto tooSmall = Remote(episode, 'c', {SizeKind::Exact, 88000000});
  SelectionPreferences preferences;

  auto ranked = RankFiles({large, small}, episode, preferences, {});
  ASSERT_EQ(ranked.size(), 2u);
  EXPECT_DOUBLE_EQ(ranked[0].sizeScore, 1.0);
  EXPECT_DOUBLE_EQ(ranked[0].sizeScore, ranked[1].sizeScore);

  preferences.profile = Profile::DataSaver;
  ranked = RankFiles({large, small, tooSmall}, episode, preferences, {});
  ASSERT_EQ(ranked.size(), 3u);
  EXPECT_EQ(ranked.front().file.identity.torrentHash, small.identity.torrentHash);
  EXPECT_EQ(ranked.back().file.identity.torrentHash, tooSmall.identity.torrentHash);

  preferences.profile = Profile::HighQuality;
  ranked = RankFiles({small, large}, episode, preferences, {});
  ASSERT_EQ(ranked.size(), 2u);
  EXPECT_EQ(ranked.front().file.identity.torrentHash, large.identity.torrentHash);
}

TEST(TestAppliancePlayback, RanksVerifiedFullSubtitlesWithLanguageFallback)
{
  const TitleContext episode{"episode:tmdb:900000002", 2700s};
  auto forced = Remote(episode, 'a', {});
  auto hint = Remote(episode, 'b', {});
  auto fallback = Remote(episode, 'c', {});
  auto preferred = Remote(episode, 'd', {});
  forced.evidence.front().subtitles = {{}, true};
  forced.evidence.front().forcedSubtitles = {"spa"};
  hint.evidence.front().audio = {{"jpn"}, false};
  hint.evidence.front().subtitles = {{"spa"}, false};
  fallback.evidence.front().audio = {{"jpn"}, true};
  fallback.evidence.front().subtitles = {{"eng"}, true};
  preferred.evidence.front().audio = {{"jpn"}, true};
  preferred.evidence.front().subtitles = {{"spa"}, true};

  SelectionPreferences preferences;
  SetLanguageRequirements(preferences, {episode.key, "jpn", "eng", "spa"}, LanguageChoice::Sub);
  const auto ranked = RankFiles({forced, hint, fallback, preferred}, episode, preferences, {});
  ASSERT_EQ(ranked.size(), 3u);
  EXPECT_EQ(ranked[0].file.identity.torrentHash, preferred.identity.torrentHash);
  EXPECT_EQ(ranked[1].file.identity.torrentHash, fallback.identity.torrentHash);
  EXPECT_EQ(ranked[2].file.identity.torrentHash, hint.identity.torrentHash);
  EXPECT_TRUE(HasKnownLanguage(ranked, preferences));
  EXPECT_FALSE(HasKnownLanguage({ranked.back()}, preferences));
}

TEST(TestAppliancePlayback, ScoresEpisodeMemberBytesAndAppliesRemoteCaps)
{
  TitleContext episode{"episode:tmdb:900000002", 2700s};
  const auto exact = Remote(episode, 'a', FileSize::FromMemberBytes(CVariant{1800000000}));
  const auto estimate = Remote(episode, 'b', {SizeKind::Estimate, 1800000000});
  const auto pack = Remote(episode, 'c', {SizeKind::PackTotal, 40000000000ULL});
  const auto malformed = Remote(episode, 'd', FileSize::FromMemberBytes(CVariant{-1}));

  auto ranked = RankFiles({pack, estimate, exact, malformed}, episode, {}, {});
  ASSERT_EQ(ranked.size(), 4u);
  EXPECT_EQ(ranked[0].file.identity.torrentHash, exact.identity.torrentHash);
  ASSERT_TRUE(ranked[0].mbPerMinute);
  EXPECT_DOUBLE_EQ(*ranked[0].mbPerMinute, 40.0);
  EXPECT_DOUBLE_EQ(ranked[1].sizeScore, ranked[0].sizeScore * 0.5);
  EXPECT_FALSE(ranked[2].mbPerMinute);
  EXPECT_EQ(ranked[2].file.evidence.front().size.kind, SizeKind::PackTotal);
  EXPECT_FALSE(ranked[3].mbPerMinute);
  EXPECT_EQ(ranked[3].file.evidence.front().size.kind, SizeKind::Unknown);
  EXPECT_EQ(FileSize::FromMemberBytes(CVariant{1.2}).kind, SizeKind::Unknown);
  EXPECT_EQ(FileSize::FromMemberBytes(CVariant{"1800000000"}).kind, SizeKind::Unknown);

  SelectionPreferences preferences;
  preferences.maxStreamBytes = 1799999999;
  ranked = RankFiles({exact, estimate, pack}, episode, preferences, {});
  ASSERT_EQ(ranked.size(), 1u);
  EXPECT_EQ(ranked[0].file.identity.torrentHash, pack.identity.torrentHash);
  preferences.allowUnknownSize = false;
  EXPECT_TRUE(RankFiles({exact, estimate, pack}, episode, preferences, {}).empty());

  preferences.maxStreamBytes = 1800000000;
  preferences.maxStreamBitsPerSecond = 5000000;
  EXPECT_TRUE(RankFiles({exact}, episode, preferences, {}).empty());
  preferences.maxStreamBitsPerSecond = 6000000;
  ASSERT_EQ(RankFiles({exact}, episode, preferences, {}).size(), 1u);

  episode.runtime = 0s;
  ranked = RankFiles({exact}, episode, {}, {});
  ASSERT_EQ(ranked.size(), 1u);
  EXPECT_FALSE(ranked[0].mbPerMinute);
  EXPECT_DOUBLE_EQ(ranked[0].sizeScore, 0.0);
}

TEST(TestAppliancePlayback, KeepsLocalOrderAndMergesOnlyTheSameTorrentMember)
{
  const TitleContext episode{"episode:tmdb:900000002", 2700s};
  auto first = Remote(episode, 'A', {SizeKind::PackTotal, 40000000000ULL});
  first.routes = {{"one", "101", "7"}};
  first.evidence.front().reportedBy = "one";
  auto alternate = Remote(episode, 'a', {SizeKind::Exact, 1800000000});
  alternate.routes = {{"one", "101", "7"}, {"two", "008", "4"}};
  alternate.evidence.front().reportedBy = "two";
  auto otherMember = alternate;
  otherMember.identity.member = "alternate.mkv";
  auto otherTorrent = alternate;
  otherTorrent.identity.torrentHash = std::string(40, 'b');

  auto wrongEpisode = Remote(episode, 'c', {SizeKind::Exact, 1800000000});
  wrongEpisode.evidence.front().titleKey = "episode:tmdb:900000003";
  auto unsupported = Remote(episode, 'd', {SizeKind::Exact, 1800000000});
  unsupported.evidence.front().codec = "av1";
  auto unavailable = Remote(episode, 'e', {SizeKind::Exact, 1800000000});
  unavailable.evidence.front().availability = Availability::Unavailable;
  auto unconfirmed = Remote(episode, 'f', {SizeKind::Exact, 1800000000});
  unconfirmed.evidence.front().titleKey.clear();

  auto local = Remote(episode, '0', {SizeKind::Exact, 9999999999ULL});
  local.identity = {Tier::Local, {}, "/test/z.mkv"};
  auto otherLocal = local;
  otherLocal.identity.member = "/test/a.mkv";
  const DeviceCapabilities device{1080, std::set<std::string>{"h264"}, std::nullopt};
  SelectionPreferences preferences;
  preferences.maxStreamBytes = 2000000000;

  std::vector<FileCandidate> input{first,        alternate,   otherMember, otherTorrent,
                                   wrongEpisode, unsupported, unavailable, unconfirmed,
                                   local,        otherLocal};
  for (const auto profile : {Profile::Balanced, Profile::HighQuality, Profile::DataSaver})
  {
    preferences.profile = profile;
    const auto ranked = RankFiles(input, episode, preferences, device);
    ASSERT_EQ(ranked.size(), 5u);
    EXPECT_EQ(ranked[0].file.identity.member, local.identity.member);
    EXPECT_EQ(ranked[1].file.identity.member, otherLocal.identity.member);
    EXPECT_EQ(ranked[2].file.identity.member, otherMember.identity.member);
    EXPECT_EQ(ranked[3].file.identity.member, first.identity.member);
    ASSERT_EQ(ranked[3].file.evidence.size(), 2u);
    EXPECT_NE(ranked[3].file.evidence[0].reportedBy, ranked[3].file.evidence[1].reportedBy);
    ASSERT_EQ(ranked[3].file.routes.size(), 2u);
    EXPECT_EQ(ranked[3].file.routes[0].fileId, "7");
    EXPECT_EQ(ranked[3].file.routes[1].provider, "two");
    ASSERT_TRUE(ranked[3].mbPerMinute);
    EXPECT_DOUBLE_EQ(*ranked[3].mbPerMinute, 40.0);
    EXPECT_EQ(ranked[4].file.identity.torrentHash, otherTorrent.identity.torrentHash);

    std::reverse(input.begin(), input.begin() + 8);
    const auto shuffled = RankFiles(input, episode, preferences, device);
    ASSERT_EQ(ranked.size(), shuffled.size());
    for (size_t i = 0; i < ranked.size(); ++i)
    {
      EXPECT_EQ(ranked[i].file.identity.torrentHash, shuffled[i].file.identity.torrentHash);
      EXPECT_EQ(ranked[i].file.identity.member, shuffled[i].file.identity.member);
    }
  }

  preferences.skipped.insert(first.identity);
  const auto ranked = RankFiles({first, alternate, otherMember}, episode, preferences, device);
  ASSERT_EQ(ranked.size(), 1u);
  EXPECT_EQ(ranked.front().file.identity.member, otherMember.identity.member);

  wrongEpisode.identity = otherMember.identity;
  EXPECT_TRUE(RankFiles({otherMember, wrongEpisode}, episode, preferences, device).empty());
}

TEST(TestAppliancePlayback, AppliesCapabilitiesAndVerifiedTrackRequirementsBeforeSorting)
{
  const TitleContext movie{"movie:tmdb:900000001", 7200s};
  auto hinted = Remote(movie, 'a', {SizeKind::Exact, 7200000000});
  hinted.evidence.front().audio.languages = {"jpn"};
  auto verified = Remote(movie, 'b', {SizeKind::Exact, 7200000000});
  verified.evidence.front().audio = {{"jpn"}, true};
  verified.evidence.front().subtitles = {{"eng"}, true};
  auto forcedOnly = verified;
  forcedOnly.identity.torrentHash = std::string(40, 'c');
  forcedOnly.evidence.front().subtitles.languages.clear();
  forcedOnly.evidence.front().forcedSubtitles = {"eng"};
  auto hdr = hinted;
  hdr.identity.torrentHash = std::string(40, 'd');
  hdr.evidence.front().hdr = "hdr10";
  hdr.evidence.front().resolution = 2160;
  auto stereo3D = verified;
  stereo3D.identity.torrentHash = std::string(40, 'e');
  stereo3D.evidence.front().is3D = true;

  const DeviceCapabilities device{1080, std::nullopt, std::set<std::string>{}, false};
  SelectionPreferences preferences;
  preferences.requiredAudio = "jpn";
  preferences.requiredSubtitles = "eng";
  const auto ranked =
      RankFiles({hinted, verified, forcedOnly, hdr, stereo3D}, movie, preferences, device);
  ASSERT_EQ(ranked.size(), 2u);
  EXPECT_EQ(ranked.front().file.identity.torrentHash, verified.identity.torrentHash);

  preferences.capabilityOverrides = {2160, std::nullopt, std::set<std::string>{"hdr10"}};
  ASSERT_EQ(RankFiles({hdr}, movie, preferences, device).size(), 1u);
  EXPECT_TRUE(RankFiles({stereo3D}, movie, preferences, {}).empty());
  preferences.allow3D = true;
  EXPECT_TRUE(RankFiles({stereo3D}, movie, preferences, device).empty());
  preferences.capabilityOverrides.supports3D = true;
  ASSERT_EQ(RankFiles({stereo3D}, movie, preferences, device).size(), 1u);
  preferences.excludedResolutions.insert(2160);
  EXPECT_TRUE(RankFiles({hdr}, movie, preferences, device).empty());
  preferences.excludedCodecs.insert("h264");
  EXPECT_TRUE(RankFiles({hinted}, movie, preferences, device).empty());
}

TEST(TestAppliancePlayback, PrefersSupportedHdrAndUsesSavedProviderSortForQualityTies)
{
  const TitleContext movie{"movie:tmdb:900000001", 7200s};
  auto sdr = Remote(movie, 'a', {SizeKind::Exact, 7200000000});
  sdr.evidence.front().hdr = "sdr";
  sdr.routes = {{"preferred", "11", "0"}};
  auto hdr = Remote(movie, 'b', {SizeKind::Exact, 7200000000});
  hdr.evidence.front().hdr = "hdr10";
  hdr.routes = {{"other", "22", "0"}};

  SelectionPreferences preferences;
  preferences.providerOrder = {"preferred"};
  DeviceCapabilities device;
  device.hdrFormats = std::set<std::string>{"hdr10"};
  auto ranked = RankFiles({sdr, hdr}, movie, preferences, device);
  ASSERT_EQ(ranked.size(), 2u);
  EXPECT_EQ(ranked.front().file.identity.torrentHash, hdr.identity.torrentHash);

  preferences.providerOrder = {"other"};
  ranked = RankFiles({sdr, hdr}, movie, preferences, {});
  ASSERT_EQ(ranked.size(), 2u);
  EXPECT_EQ(ranked.front().file.identity.torrentHash, hdr.identity.torrentHash);
  preferences.providerOrder.clear();
  ranked = RankFiles({hdr, sdr}, movie, preferences, {});
  ASSERT_EQ(ranked.size(), 2u);
  EXPECT_EQ(ranked.front().file.identity.torrentHash, sdr.identity.torrentHash);
}

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
  EXPECT_TRUE(SelectFile(pack, episode).isNull());

  CVariant confirmed;
  ASSERT_TRUE(CJSONVariantParser::Parse(
      R"({"id":4,"name":"Show/Show.S01E02.Return.1080p.mkv","size":1800000000})", confirmed));
  pack.push_back(confirmed);
  const auto selected = SelectFile(pack, episode);
  EXPECT_EQ(selected["id"].asInteger(), 4);
  EXPECT_EQ(FileSize::FromMemberBytes(selected["size"]).bytes, 1800000000u);

  confirmed["id"] = 5;
  pack.push_back(confirmed);
  EXPECT_TRUE(SelectFile(pack, episode).isNull());
}

TEST(TestAppliancePlayback, MovieIdentityDoesNotDependOnMemberBytes)
{
  CVariant movie;
  ASSERT_TRUE(CJSONVariantParser::Parse(R"json({"title":"Pilot Movie (2026)"})json", movie));
  CVariant files;
  ASSERT_TRUE(CJSONVariantParser::Parse(R"([
    {"id":0,"name":"Other.Movie.(2026).1080p.mkv","size":7200000123},
    {"id":1,"name":"Pilot.Movie.(2026).1080p.mkv","size":-1}
  ])",
                                        files));
  const auto selected = SelectFile(files, movie);
  EXPECT_EQ(selected["id"].asInteger(-1), 1);
  EXPECT_EQ(FileSize::FromMemberBytes(selected["size"]).kind, SizeKind::Unknown);

  CVariant duplicate = selected;
  duplicate["id"] = 2;
  duplicate["size"] = uint64_t{7200000123};
  files.push_back(duplicate);
  EXPECT_TRUE(SelectFile(files, movie).isNull());
}

TEST(TestAppliancePlayback, KeepsDistinctLocalFilesInIndexOrder)
{
  CVariant entry;
  ASSERT_TRUE(CJSONVariantParser::Parse(R"({"local":["smb://example.invalid/test/one.mkv",
    "https://example.invalid/not-local.mkv", "smb://example.invalid/test/sub/../one.mkv",
    {"path":"/test/two.mkv","bytes":42,"format":"1080p WEB-DL H264"},
    {"path":"/test/3d.mkv","is3D":true}, "relative.mkv"]})",
                                        entry));
  const Title title{"/test/anchor.mkv", "movie:tmdb:900000001", {}};
  const auto ranked = RankFiles(LocalFiles(entry, title), {title.key}, {}, {});
  ASSERT_EQ(ranked.size(), 2u);
  EXPECT_EQ(ranked[0].file.localPath, "smb://example.invalid/test/one.mkv");
  EXPECT_EQ(ranked[1].file.localPath, "/test/two.mkv");
  EXPECT_EQ(ranked[1].file.evidence.front().size.bytes, 42u);
  EXPECT_EQ(ranked[1].file.evidence.front().resolution, 1080u);
  EXPECT_EQ(ranked[1].file.evidence.front().source, Source::WebDl);
  EXPECT_EQ(ranked[1].file.evidence.front().codec, "h264");
}
