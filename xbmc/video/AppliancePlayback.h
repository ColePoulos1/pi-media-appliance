/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/Variant.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

class CFileItem;
class CApplicationPlayer;

namespace KODI::APPLIANCE
{
inline constexpr char SOURCE_TOKEN[] = "appliance.sourceToken";
inline constexpr char LANGUAGE_CHOICE[] = "appliance.languageChoice";

struct Title
{
  std::string path;
  std::string key;
  std::string episode;
};

enum class Tier
{
  Local,
  Remote
};
enum class Profile
{
  Balanced,
  HighQuality,
  DataSaver
};

enum class LanguageChoice
{
  Ordinary,
  Sub,
  Dub
};

struct PlaybackLanguage
{
  std::string key;
  std::string original;
  std::string audio;
  std::string subtitles;
  const char* setting{nullptr};
};

struct PlaybackTracks
{
  int audio{-1};
  int subtitle{-1};
  bool Matches(LanguageChoice choice) const;
};
enum class Source
{
  Unknown,
  WebDl,
  BluRay,
  Remux
};
enum class Availability
{
  Unknown,
  Available,
  Unavailable
};
enum class SizeKind
{
  Unknown,
  PackTotal,
  Estimate,
  Exact
};

struct FileIdentity
{
  Tier tier{Tier::Local};
  std::string torrentHash;
  // A local library ID or canonical path, or a path/stable member ID within the torrent.
  std::string member;

  bool operator<(const FileIdentity& other) const;
};

struct ResolverRoute
{
  std::string provider;
  std::string torrentId;
  std::string fileId;

  bool operator<(const ResolverRoute& other) const;
};

using PlaybackClock = std::chrono::steady_clock;

// Provisional pilot limits. The time budget starts with failure or intentional recovery.
inline constexpr auto QUICK_REPLAY_WINDOW = std::chrono::minutes{2};
inline constexpr auto ROUTE_COOLDOWN = std::chrono::seconds{5};
inline constexpr auto RETRY_TIME = std::chrono::seconds{30};
inline constexpr unsigned int MAX_SOURCE_ATTEMPTS = 16;

struct RetryBudget
{
  unsigned int attempts{0};
  PlaybackClock::time_point deadline{PlaybackClock::time_point::max()};

  void Start(PlaybackClock::time_point now);
  bool CanTry(PlaybackClock::time_point now) const;
};

class PlaybackHistory
{
public:
  struct Stop
  {
    PlaybackClock::time_point time;
    FileIdentity file;
    int64_t position;
  };

  void Stopped(int profile, const std::string& title, Stop stop);
  void Started(int profile, const std::string& title);
  const Stop* RecentStop(int profile, const std::string& title, PlaybackClock::time_point now);
  void CoolRoute(int profile, const ResolverRoute& route, PlaybackClock::time_point now);
  bool RouteCoolingDown(int profile, const ResolverRoute& route, PlaybackClock::time_point now);

private:
  std::map<std::pair<int, std::string>, Stop> m_stops;
  std::map<std::pair<int, ResolverRoute>, PlaybackClock::time_point> m_cooldowns;
};

struct FileSize
{
  SizeKind kind{SizeKind::Unknown};
  uint64_t bytes{0};

  static FileSize FromMemberBytes(const CVariant& value);
};

struct TrackEvidence
{
  // Use the same normalized language codes as the selection preferences.
  std::set<std::string> languages;
  // A verified list is complete. Subtitle languages here refer to full dialogue tracks.
  bool verified{false};
};

struct FileEvidence
{
  std::string reportedBy;
  // Set only from a confirmed match. Different provider-qualified keys conflict.
  std::string titleKey;
  // Estimates are credible selected-member estimates, with pack totals labeled separately.
  FileSize size;
  unsigned int resolution{0};
  std::string codec;
  Source source{Source::Unknown};
  // Empty means unknown. Use "sdr" for a known SDR file.
  std::string hdr;
  std::optional<bool> is3D;
  Availability availability{Availability::Unknown};
  TrackEvidence audio;
  TrackEvidence subtitles;
  std::set<std::string> forcedSubtitles;
};

struct FileCandidate
{
  FileIdentity identity;
  std::vector<ResolverRoute> routes;
  std::vector<FileEvidence> evidence;
  std::string localPath{};
};

struct TitleContext
{
  std::string key;
  // Accept Kodi video tag seconds at ingestion and store minutes.
  std::chrono::duration<double, std::ratio<60>> runtime{};
  bool lowMotionAnimation{false};
  bool grainOrRestoration{false};
};

struct DeviceCapabilities
{
  unsigned int maxResolution{0};
  // An absent list means capability detection was inconclusive.
  std::optional<std::set<std::string>> codecs;
  std::optional<std::set<std::string>> hdrFormats;
  std::optional<bool> supports3D{};
};

struct SelectionPreferences
{
  Profile profile{Profile::Balanced};
  bool allow3D{false};
  bool allowUnknownSize{true};
  unsigned int maxResolution{0};
  uint64_t maxStreamBytes{0};
  uint64_t maxStreamBitsPerSecond{0};
  std::set<unsigned int> excludedResolutions;
  std::set<std::string> excludedCodecs;
  DeviceCapabilities capabilityOverrides;
  std::string requiredAudio;
  std::string requiredSubtitles;
  std::string fallbackSubtitles;
  std::vector<unsigned int> resolutionOrder;
  std::vector<std::string> codecOrder;
  std::vector<Source> sourceOrder;
  std::vector<std::string> hdrOrder;
  std::vector<std::string> providerOrder;
  std::set<FileIdentity> skipped;
};

struct RankedFile
{
  FileCandidate file;
  std::optional<double> mbPerMinute;
  double sizeScore{0.0};
};

std::string StableFileKey(const FileIdentity& identity);
std::string LastWorkingFile(const Title& title);
void RememberWorkingFile(const Title& title, const std::string& key);
void PreferWorkingFile(std::vector<RankedFile>& files, const std::string& key);

std::vector<RankedFile> RankFiles(std::vector<FileCandidate> candidates,
                                  const TitleContext& title,
                                  const SelectionPreferences& preferences,
                                  const DeviceCapabilities& device);
bool HasKnownLanguage(const std::vector<RankedFile>& files,
                      const SelectionPreferences& preferences);

Title GetTitle(const CFileItem& item);
CVariant LoadIndexEntry(const Title& title);
std::set<std::string> ImportMovieFolder(const std::string& folder, const std::string& root);
Profile GetQualityProfile();
PlaybackLanguage GetPlaybackLanguage(const Title& title,
                                     const CFileItem& item,
                                     const CVariant& entry);
std::optional<LanguageChoice> ChooseLanguage(const PlaybackLanguage& language, bool& remember);
void RememberLanguageChoice(const PlaybackLanguage& language, LanguageChoice choice);
bool EditLanguageChoice(const CFileItem& item);
bool OfferLanguageAlternative(const PlaybackLanguage& language, LanguageChoice choice);
void SetLanguageRequirements(SelectionPreferences& preferences,
                             const PlaybackLanguage& language,
                             LanguageChoice choice);
PlaybackTracks FindPlaybackTracks(CApplicationPlayer& player,
                                  const PlaybackLanguage& language,
                                  LanguageChoice choice);
bool VerifyPlaybackTracks(CApplicationPlayer& player,
                          const PlaybackLanguage& language,
                          LanguageChoice choice,
                          const PlaybackTracks& tracks);
TitleContext GetTitleContext(const Title& title, const CFileItem& item, const CVariant& entry);
CVariant LoadProviders();
std::vector<FileCandidate> LocalFiles(const CVariant& entry, const Title& title);
CVariant SelectFile(const CVariant& files, const CVariant& entry);
std::vector<FileCandidate> Discover(const CVariant& entry,
                                    const CVariant& providers,
                                    const std::string& titleKey,
                                    const std::atomic_bool& cancelled);
std::string Resolve(const CVariant& providers,
                    const ResolverRoute& route,
                    const std::atomic_bool& cancelled);
} // namespace KODI::APPLIANCE
