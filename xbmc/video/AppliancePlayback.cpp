/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AppliancePlayback.h"

#include "FileItem.h"
#include "LangInfo.h"
#include "ServiceBroker.h"
#include "URL.h"
#include "application/ApplicationPlayer.h"
#include "cores/IPlayer.h"
#include "cores/VideoPlayer/Interface/StreamInfo.h"
#include "dialogs/GUIDialogSelect.h"
#include "filesystem/Directory.h"
#include "filesystem/DllLibCurl.h"
#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/LocalizeStrings.h"
#include "profiles/ProfileManager.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/Digest.h"
#include "utils/JSONVariantParser.h"
#include "utils/LangCodeExpander.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"
#include "video/VideoDatabase.h"
#include "video/VideoInfoTag.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <tuple>
#include <utility>

namespace KODI::APPLIANCE
{
namespace
{
constexpr char ROOT[] = "special://profile/appliance/";
constexpr char TORBOX[] = "https://api.torbox.app";
constexpr char TORBOX_TORRENTS[] = "/v1/api/torrents/";
constexpr char TORBOX_PROVIDER[] = "torbox";
constexpr char VIDEO_EXTENSIONS[] = ".mkv|.mp4|.m4v|.avi|.webm|.mov|.ts|.m2ts";
constexpr size_t MAX_RESPONSE = 2 * 1024 * 1024;
constexpr unsigned int RELEASE_LIMIT = 50;
constexpr unsigned int TORRENT_LIMIT = 200;

std::string NormalizeLanguage(const std::string& value)
{
  if (value.empty() || StringUtils::EqualsNoCase(value, "und") ||
      StringUtils::EqualsNoCase(value, "zxx") || StringUtils::EqualsNoCase(value, "mul"))
    return {};
  const auto primary = value.substr(0, value.find_first_of("-_"));
  std::string code;
  if (g_LangCodeExpander.ConvertToISO6392B(primary, code))
    return code;
  std::string name;
  if (g_LangCodeExpander.Lookup(primary, name))
  {
    code = g_LangCodeExpander.ConvertToISO6392B(name);
    return code.size() == 3 ? code : primary;
  }
  return {};
}

bool FullSubtitle(const SubtitleStreamInfo& info)
{
  return info.valid && !(info.flags & (FLAG_FORCED | FLAG_COMMENT | FLAG_LYRICS | FLAG_KARAOKE));
}

std::string ChoicePath(const PlaybackLanguage& language)
{
  std::string filename = language.key;
  StringUtils::Replace(filename, ':', '-');
  return std::string(ROOT) + "language/" + filename;
}

LanguageChoice ReadChoice(const PlaybackLanguage& language)
{
  XFILE::CFile file;
  char value = 0;
  if (file.Open(ChoicePath(language)) && file.GetLength() == 1 && file.Read(&value, 1) == 1)
  {
    if (value == 's')
      return LanguageChoice::Sub;
    if (value == 'd')
      return LanguageChoice::Dub;
  }
  return LanguageChoice::Ordinary;
}

bool WriteState(const std::string& path, const std::string& value)
{
  XFILE::CDirectory::Create(URIUtils::GetDirectory(path));
  const auto target = std::filesystem::u8path(CSpecialProtocol::TranslatePath(path));
  const std::string pending = path + ".tmp";
  XFILE::CFile file;
  if (!file.OpenForWrite(pending, true))
    return false;
  const bool written = file.Write(value.data(), value.size()) == static_cast<ssize_t>(value.size());
  file.Flush();
  file.Close();
  std::error_code error;
  if (written)
    std::filesystem::rename(std::filesystem::u8path(CSpecialProtocol::TranslatePath(pending)),
                            target, error);
  if (!written || error)
  {
    XFILE::CFile::Delete(pending);
    return false;
  }
  return true;
}

bool SaveChoice(const PlaybackLanguage& language, LanguageChoice choice)
{
  const std::string path = ChoicePath(language);
  if (choice == LanguageChoice::Ordinary)
    return !XFILE::CFile::Exists(path) || XFILE::CFile::Delete(path);
  return WriteState(path, choice == LanguageChoice::Sub ? "s" : "d");
}

std::string LanguageName(const std::string& code)
{
  std::string name;
  return g_LangCodeExpander.Lookup(code, name) ? name : code;
}

std::string ChoiceLabel(const PlaybackLanguage& language, LanguageChoice choice)
{
  if (choice == LanguageChoice::Dub)
    return StringUtils::Format(g_localizeStrings.Get(42012), LanguageName(language.audio));
  return StringUtils::Format(g_localizeStrings.Get(42011), LanguageName(language.original),
                             LanguageName(language.subtitles));
}

TrackEvidence VerifiedLanguages(const CVariant& values)
{
  TrackEvidence tracks;
  if (!values.isArray())
    return tracks;

  for (auto it = values.begin_array(); it != values.end_array(); ++it)
  {
    std::string code = NormalizeLanguage(it->asString());
    if (code.empty())
      return {};
    tracks.languages.insert(std::move(code));
  }
  tracks.verified = true;
  return tracks;
}

CVariant ReadJson(const std::string& path)
{
  XFILE::CFile file;
  if (!file.Open(path))
    return {};

  const int64_t length = file.GetLength();
  if (length <= 0 || length > static_cast<int64_t>(MAX_RESPONSE))
    return {};
  std::string data(static_cast<size_t>(length), '\0');
  CVariant json;
  if (file.Read(data.data(), data.size()) != static_cast<ssize_t>(data.size()) ||
      !CJSONVariantParser::Parse(data, json))
    return {};
  return json;
}

bool IsHttp(const CURL& url)
{
  return (url.IsProtocol("http") || url.IsProtocol("https")) && !url.GetHostName().empty() &&
         url.GetUserName().empty() && url.GetPassWord().empty() && url.GetProtocolOptions().empty();
}

CVariant GetJson(const std::string& url,
                 const std::atomic_bool& cancelled,
                 const std::string& header = {})
{
  if (cancelled || !IsHttp(CURL(url)) || header.find_first_of("\r\n") != std::string::npos)
    return {};

  // These requests contain account data and must bypass CCurlFile's URL and response logging.
  const auto freeHeaders = [](curl_slist* list) { g_curlInterface.slist_free_all(list); };
  std::unique_ptr<curl_slist, decltype(freeHeaders)> headers(
      header.empty() ? nullptr : g_curlInterface.slist_append(nullptr, header.c_str()),
      freeHeaders);
  const auto cleanup = [](CURL_HANDLE* curl) { g_curlInterface.easy_cleanup(curl); };
  std::unique_ptr<CURL_HANDLE, decltype(cleanup)> curl(g_curlInterface.easy_init(), cleanup);
  if (!curl || (!header.empty() && !headers))
    return {};

  std::string response;
  const auto receive = +[](char* data, size_t size, size_t count, void* output) -> size_t
  {
    auto& body = *static_cast<std::string*>(output);
    const size_t bytes = size * count;
    if (bytes > MAX_RESPONSE - body.size())
      return 0;
    body.append(data, bytes);
    return bytes;
  };

  g_curlInterface.easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  g_curlInterface.easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
  g_curlInterface.easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
  g_curlInterface.easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L);
  g_curlInterface.easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 2500L);
  g_curlInterface.easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 5000L);
  const auto progress = +[](void* state, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int
  { return *static_cast<const std::atomic_bool*>(state) ? 1 : 0; };
  g_curlInterface.easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
  g_curlInterface.easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progress);
  g_curlInterface.easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &cancelled);
  g_curlInterface.easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
  g_curlInterface.easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive);

  CVariant json;
  if (g_curlInterface.easy_perform(curl.get()) != CURLE_OK ||
      !CJSONVariantParser::Parse(response, json))
    return {};
  return json;
}

std::string Words(std::string value)
{
  StringUtils::ToLower(value);
  for (char& ch : value)
  {
    if (ch == '.' || ch == '_' || ch == '-')
      ch = ' ';
  }
  static const std::regex spaces(" +");
  value = std::regex_replace(value, spaces, " ");
  StringUtils::Trim(value);
  return value;
}

std::string NormalizeInfoHash(std::string hash)
{
  if (hash.size() != 40 || hash.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
    return {};
  StringUtils::ToLower(hash);
  return hash;
}

std::string InfoHash(const CVariant& release)
{
  std::string hash = release["infoHash"].asString();
  if (hash.empty())
  {
    static const std::regex magnet(R"((?:\?|&)xt=urn:btih:([0-9a-f]{40})(?:&|$))",
                                   std::regex::icase);
    const std::string url = release["magnetUrl"].asString();
    std::smatch match;
    if (StringUtils::StartsWithNoCase(url, "magnet:?") && std::regex_search(url, match, magnet))
      hash = match[1].str();
  }

  return NormalizeInfoHash(std::move(hash));
}

size_t ReleaseSuffix(const std::string& stem)
{
  static const std::regex suffix(
      R"( (?:2160p|1080p|720p|480p|web dl|webrip|bluray|hdtv|remux|x264|x265|hevc)(?: |$))");
  std::smatch match;
  return std::regex_search(stem, match, suffix) ? match.position() : std::string::npos;
}

bool MatchesFile(const CVariant& file, const CVariant& entry)
{
  const std::string name = URIUtils::GetFileName(file["name"].asString());
  if (name.size() > 255 || !URIUtils::HasExtension(name, VIDEO_EXTENSIONS))
    return false;

  std::string stem = Words(name.substr(0, name.rfind('.')));
  const std::string title = Words(entry["title"].asString());
  const std::string episode = Words(entry["episode"].asString());
  if (title.empty())
    return false;

  const size_t suffix = ReleaseSuffix(stem);
  if (suffix != std::string::npos)
    stem.resize(suffix);

  if (episode.empty())
    return stem == title;

  static const std::regex episodeNumber("s[0-9]{2}e[0-9]{2,3}");
  if (!std::regex_match(episode, episodeNumber))
    return false;

  // shortcut: require both the number and the catalog episode name until file identification has stronger evidence.
  stem.insert(stem.begin(), ' ');
  const size_t mark = stem.find(" " + episode + " ");
  return mark != std::string::npos &&
         stem.substr(1, mark - 1) == Words(entry["query"].asString()) &&
         stem.substr(mark + episode.size() + 2) == title;
}

int64_t ReadId(const CVariant& value)
{
  return value.isInteger() || value.isUnsignedInteger() ? value.asInteger(-1) : -1;
}

FileEvidence NameEvidence(const std::string& name, bool containsTitle)
{
  FileEvidence evidence;
  if (name.size() > 512)
    return evidence;

  const std::string normalized = Words(name);
  const size_t suffix = containsTitle ? ReleaseSuffix(normalized) : 0;
  if (suffix == std::string::npos)
    return evidence;

  const std::string words = " " + normalized.substr(suffix) + " ";
  const auto has = [&](const char* word) { return words.find(word) != std::string::npos; };
  if (has(" 2160p "))
    evidence.resolution = 2160;
  else if (has(" 1080p "))
    evidence.resolution = 1080;
  else if (has(" 720p "))
    evidence.resolution = 720;
  else if (has(" 480p "))
    evidence.resolution = 480;

  if (has(" hevc ") || has(" x265 ") || has(" h265 ") || has(" h 265 "))
    evidence.codec = "hevc";
  else if (has(" av1 "))
    evidence.codec = "av1";
  else if (has(" vp9 "))
    evidence.codec = "vp9";
  else if (has(" x264 ") || has(" h264 ") || has(" h 264 "))
    evidence.codec = "h264";

  if (has(" remux "))
    evidence.source = Source::Remux;
  else if (has(" bluray ") || has(" blu ray "))
    evidence.source = Source::BluRay;
  else if (has(" web dl ") || has(" webdl "))
    evidence.source = Source::WebDl;

  if (has(" dolby vision ") || has(" dovi "))
    evidence.hdr = "dolbyvision";
  else if (has(" hdr10+ ") || has(" hdr10plus "))
    evidence.hdr = "hdr10plus";
  else if (has(" hdr10 "))
    evidence.hdr = "hdr10";
  else if (has(" hlg "))
    evidence.hdr = "hlg";
  else if (has(" sdr "))
    evidence.hdr = "sdr";

  if (has(" 3d ") || has(" hsbs ") || has(" hou "))
    evidence.is3D = true;
  return evidence;
}

bool IsLocalVideo(const std::string& path, const CURL& url)
{
  return !path.empty() && path.find('\0') == std::string::npos &&
         (url.IsProtocol("smb") || (url.GetProtocol().empty() && CURL::IsFullPath(path))) &&
         URIUtils::HasExtension(path, VIDEO_EXTENSIONS);
}

bool NormalizeIdentity(FileIdentity& identity)
{
  if (identity.member.empty())
    return false;
  if (identity.tier == Tier::Local)
    return identity.torrentHash.empty();
  identity.torrentHash = NormalizeInfoHash(std::move(identity.torrentHash));
  return !identity.torrentHash.empty();
}

bool IsExcluded(const std::string& value, const std::optional<std::set<std::string>>& supported)
{
  return !value.empty() && supported && !supported->count(value);
}

bool TrackMismatch(const TrackEvidence& tracks,
                   const std::string& required,
                   const std::string& fallback = {})
{
  return tracks.verified && !required.empty() && !tracks.languages.count(required) &&
         (fallback.empty() || !tracks.languages.count(fallback));
}

int TrackScore(const TrackEvidence& tracks, const std::string& required)
{
  if (required.empty() || !tracks.languages.count(required))
    return 0;
  return tracks.verified ? 3 : 1;
}

template<typename T>
size_t SortPreference(const std::vector<T>& order, const T& value)
{
  return static_cast<size_t>(order.end() - std::find(order.begin(), order.end(), value));
}

bool IsHdr(const std::string& hdr)
{
  return !hdr.empty() && hdr != "sdr";
}

double CodecFactor(const std::string& codec)
{
  return codec == "hevc" || codec == "av1" || codec == "vp9" ? 0.75 : 1.0;
}

double SizeScore(double mbPerMinute,
                 Profile profile,
                 const TitleContext& title,
                 unsigned int resolution,
                 double codecFactor,
                 Source source)
{
  // Provisional decimal MB/min bands for 1080p H.264 WEB-DL. Calibrate with playback evidence.
  constexpr double CREDIBLE_FLOOR = 26.0;
  constexpr double BALANCED_PLATEAU = 55.0;
  constexpr double HIGH_QUALITY_PLATEAU = 95.0;
  const double resolutionFactor = resolution >= 2160                    ? 1.9
                                  : resolution > 0 && resolution <= 720 ? 0.65
                                                                        : 1.0;
  const double sourceFactor = source == Source::Remux ? 1.6 : source == Source::BluRay ? 1.05 : 1.0;
  const double contentFactor =
      (title.lowMotionAnimation ? 0.45 : 1.0) * (title.grainOrRestoration ? 1.3 : 1.0);
  const double rate = mbPerMinute / (resolutionFactor * codecFactor * sourceFactor * contentFactor);
  const double undersize = std::max(0.0, 1.0 - rate / CREDIBLE_FLOOR);

  switch (profile)
  {
    case Profile::Balanced:
      return std::min(1.0, rate / BALANCED_PLATEAU) - undersize;
    case Profile::HighQuality:
      return std::min(1.0, rate / HIGH_QUALITY_PLATEAU) - 2.0 * undersize;
    case Profile::DataSaver:
      if (undersize > 0.0)
        return -1.0 - 2.0 * undersize;
      return 1.0 - std::clamp((rate - CREDIBLE_FLOOR) / (HIGH_QUALITY_PLATEAU - CREDIBLE_FLOOR),
                              0.0, 2.0);
  }
  return 0.0;
}

struct OrderedFile
{
  RankedFile ranked;
  int tracks{0};
  double quality{0.0};
  std::array<size_t, 5> sort{};
};

std::optional<OrderedFile> Evaluate(FileCandidate file,
                                    const TitleContext& title,
                                    const SelectionPreferences& preferences,
                                    const DeviceCapabilities& device)
{
  const auto& overrides = preferences.capabilityOverrides;
  const auto& codecs = overrides.codecs ? overrides.codecs : device.codecs;
  const auto& hdrFormats = overrides.hdrFormats ? overrides.hdrFormats : device.hdrFormats;
  const auto& supports3D = overrides.supports3D ? overrides.supports3D : device.supports3D;
  const unsigned int deviceMax =
      overrides.maxResolution ? overrides.maxResolution : device.maxResolution;
  bool matched = false;
  bool available = false;
  unsigned int resolution = 0;
  double codecFactor = 0.0;
  Source source = Source::Unknown;
  bool supportedHdr = false;
  FileSize size;
  int audio = 0;
  int subtitles = 0;
  OrderedFile result;

  for (const auto& report : file.evidence)
  {
    if ((!report.titleKey.empty() && report.titleKey != title.key) ||
        (report.is3D.value_or(false) && (!preferences.allow3D || !supports3D.value_or(true))) ||
        (deviceMax && report.resolution > deviceMax) ||
        (preferences.maxResolution && report.resolution > preferences.maxResolution) ||
        (report.resolution && preferences.excludedResolutions.count(report.resolution)) ||
        (!report.codec.empty() && preferences.excludedCodecs.count(report.codec)) ||
        IsExcluded(report.codec, codecs) ||
        (IsHdr(report.hdr) && IsExcluded(report.hdr, hdrFormats)) ||
        TrackMismatch(report.audio, preferences.requiredAudio) ||
        TrackMismatch(report.subtitles, preferences.requiredSubtitles,
                      preferences.fallbackSubtitles))
      return std::nullopt;

    matched |= !report.titleKey.empty();
    available |= report.availability != Availability::Unavailable;
    if (report.size.kind >= SizeKind::Estimate && report.size.bytes &&
        std::tie(report.size.kind, report.size.bytes) > std::tie(size.kind, size.bytes))
      size = report.size;

    audio = std::max(audio, TrackScore(report.audio, preferences.requiredAudio));
    const int preferred = TrackScore(report.subtitles, preferences.requiredSubtitles);
    const int fallback = TrackScore(report.subtitles, preferences.fallbackSubtitles);
    subtitles = std::max(subtitles, preferred ? preferred + 1 : fallback);
    resolution = std::max(resolution, report.resolution);
    if (!report.codec.empty())
      codecFactor = std::max(codecFactor, CodecFactor(report.codec));
    source = std::max(source, report.source);
    supportedHdr |= IsHdr(report.hdr) && hdrFormats && hdrFormats->count(report.hdr);
    result.sort[0] =
        std::max(result.sort[0], SortPreference(preferences.resolutionOrder, report.resolution));
    result.sort[1] = std::max(result.sort[1], SortPreference(preferences.codecOrder, report.codec));
    result.sort[2] =
        std::max(result.sort[2], SortPreference(preferences.sourceOrder, report.source));
    result.sort[3] = std::max(result.sort[3], SortPreference(preferences.hdrOrder, report.hdr));
  }
  if (!matched || !available)
    return std::nullopt;

  if (file.identity.tier == Tier::Remote)
  {
    if ((!size.bytes && !preferences.allowUnknownSize) ||
        (preferences.maxStreamBytes && size.bytes > preferences.maxStreamBytes))
      return std::nullopt;

    const double minutes = title.runtime.count();
    if (size.bytes && std::isfinite(minutes) && minutes > 0.0)
    {
      constexpr double BYTES_PER_MB = 1000000.0;
      const double bytesPerMinute = static_cast<double>(size.bytes) / minutes;
      if (preferences.maxStreamBitsPerSecond &&
          bytesPerMinute * 8.0 / 60.0 > static_cast<double>(preferences.maxStreamBitsPerSecond))
        return std::nullopt;

      result.ranked.mbPerMinute = bytesPerMinute / BYTES_PER_MB;
      result.ranked.sizeScore = SizeScore(*result.ranked.mbPerMinute, preferences.profile, title,
                                          resolution, codecFactor ? codecFactor : 1.0, source);
      if (size.kind == SizeKind::Estimate)
        result.ranked.sizeScore *= 0.5;
    }
  }

  const std::set<ResolverRoute> routes(file.routes.begin(), file.routes.end());
  file.routes.assign(routes.begin(), routes.end());
  for (const auto& route : file.routes)
    result.sort[4] =
        std::max(result.sort[4], SortPreference(preferences.providerOrder, route.provider));

  result.tracks = audio + subtitles;
  result.quality = result.ranked.sizeScore + (source >= Source::BluRay ? 0.15 : 0.0) +
                   (supportedHdr ? 0.1 : 0.0);
  result.ranked.file = std::move(file);
  return result;
}
} // namespace

bool FileIdentity::operator<(const FileIdentity& other) const
{
  return std::tie(tier, torrentHash, member) <
         std::tie(other.tier, other.torrentHash, other.member);
}

bool ResolverRoute::operator<(const ResolverRoute& other) const
{
  return std::tie(provider, torrentId, fileId) <
         std::tie(other.provider, other.torrentId, other.fileId);
}

void RetryBudget::Start(PlaybackClock::time_point now)
{
  deadline = std::min(deadline, now + RETRY_TIME);
}

bool RetryBudget::CanTry(PlaybackClock::time_point now) const
{
  return attempts < MAX_SOURCE_ATTEMPTS && now < deadline;
}

void PlaybackHistory::Stopped(int profile, const std::string& title, Stop stop)
{
  for (auto it = m_stops.begin(); it != m_stops.end();)
    it = stop.time >= it->second.time + QUICK_REPLAY_WINDOW ? m_stops.erase(it) : std::next(it);
  m_stops[{profile, title}] = std::move(stop);
}

void PlaybackHistory::Started(int profile, const std::string& title)
{
  m_stops.erase({profile, title});
}

const PlaybackHistory::Stop* PlaybackHistory::RecentStop(int profile,
                                                        const std::string& title,
                                                        PlaybackClock::time_point now)
{
  const auto stop = m_stops.find({profile, title});
  if (stop != m_stops.end() && now < stop->second.time + QUICK_REPLAY_WINDOW)
    return &stop->second;
  return nullptr;
}

void PlaybackHistory::CoolRoute(int profile, const ResolverRoute& route, PlaybackClock::time_point now)
{
  for (auto it = m_cooldowns.begin(); it != m_cooldowns.end();)
    it = now >= it->second ? m_cooldowns.erase(it) : std::next(it);
  m_cooldowns[{profile, route}] = now + ROUTE_COOLDOWN;
}

bool PlaybackHistory::RouteCoolingDown(int profile,
                                      const ResolverRoute& route,
                                      PlaybackClock::time_point now)
{
  const auto cooldown = m_cooldowns.find({profile, route});
  return cooldown != m_cooldowns.end() && now < cooldown->second;
}

std::string StableFileKey(const FileIdentity& identity)
{
  using KODI::UTILITY::CDigest;
  const auto prefix = identity.tier == Tier::Local ? "local:" : "torrent:";
  return CDigest::Calculate(CDigest::Type::SHA256,
                           prefix + identity.torrentHash + ':' + identity.member);
}

std::string LastWorkingFile(const Title& title)
{
  std::string filename = title.key;
  StringUtils::Replace(filename, ':', '-');
  XFILE::CFile file;
  std::string key(64, '\0');
  if (file.Open(std::string(ROOT) + "working/" + filename) &&
      file.GetLength() == static_cast<int64_t>(key.size()) &&
      file.Read(key.data(), key.size()) == static_cast<ssize_t>(key.size()))
    return key;
  return {};
}

void RememberWorkingFile(const Title& title, const std::string& key)
{
  std::string filename = title.key;
  StringUtils::Replace(filename, ':', '-');
  if (!WriteState(std::string(ROOT) + "working/" + filename, key))
    CLog::Log(LOGWARNING, "Appliance: could not save last working file");
}

void PreferWorkingFile(std::vector<RankedFile>& files, const std::string& key)
{
  if (key.empty())
    return;
  const auto match = std::find_if(files.begin(), files.end(), [&key](const RankedFile& file)
                                  { return StableFileKey(file.file.identity) == key; });
  if (match != files.end())
    std::rotate(files.begin(), match, std::next(match));
}

FileSize FileSize::FromMemberBytes(const CVariant& value)
{
  const uint64_t bytes = value.isUnsignedInteger() || (value.isInteger() && value.asInteger() > 0)
                             ? value.asUnsignedInteger()
                             : 0;
  return bytes ? FileSize{SizeKind::Exact, bytes} : FileSize{};
}

std::vector<RankedFile> RankFiles(std::vector<FileCandidate> candidates,
                                  const TitleContext& title,
                                  const SelectionPreferences& preferences,
                                  const DeviceCapabilities& device)
{
  std::set<FileIdentity> skipped;
  for (auto identity : preferences.skipped)
  {
    if (NormalizeIdentity(identity))
      skipped.insert(std::move(identity));
  }

  std::map<FileIdentity, size_t> positions;
  std::vector<FileCandidate> merged;
  for (auto& candidate : candidates)
  {
    if (!NormalizeIdentity(candidate.identity) || skipped.count(candidate.identity))
      continue;
    const auto [position, inserted] = positions.try_emplace(candidate.identity, merged.size());
    if (inserted)
    {
      merged.push_back(std::move(candidate));
      continue;
    }
    auto& file = merged[position->second];
    file.routes.insert(file.routes.end(), std::make_move_iterator(candidate.routes.begin()),
                       std::make_move_iterator(candidate.routes.end()));
    file.evidence.insert(file.evidence.end(), std::make_move_iterator(candidate.evidence.begin()),
                         std::make_move_iterator(candidate.evidence.end()));
  }

  std::vector<OrderedFile> ordered;
  for (auto& file : merged)
  {
    if (auto eligible = Evaluate(std::move(file), title, preferences, device))
      ordered.push_back(std::move(*eligible));
  }
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const auto& left, const auto& right)
                   {
                     const auto& a = left.ranked.file.identity;
                     const auto& b = right.ranked.file.identity;
                     if (a.tier != b.tier)
                       return a.tier < b.tier;
                     if (left.tracks != right.tracks)
                       return left.tracks > right.tracks;
                     if (a.tier == Tier::Local && left.sort != right.sort)
                       return left.sort > right.sort;
                     if (left.quality != right.quality)
                       return left.quality > right.quality;
                     if (left.sort != right.sort)
                       return left.sort > right.sort;
                     return a.tier == Tier::Remote && a < b;
                   });

  std::vector<RankedFile> result;
  result.reserve(ordered.size());
  for (auto& file : ordered)
    result.push_back(std::move(file.ranked));
  return result;
}

bool HasKnownLanguage(const std::vector<RankedFile>& files, const SelectionPreferences& preferences)
{
  for (const auto& candidate : files)
  {
    bool audio = false;
    bool subtitles = preferences.requiredSubtitles.empty();
    for (const auto& evidence : candidate.file.evidence)
    {
      audio |= evidence.audio.verified && evidence.audio.languages.count(preferences.requiredAudio);
      subtitles |= evidence.subtitles.verified &&
                   (evidence.subtitles.languages.count(preferences.requiredSubtitles) ||
                    evidence.subtitles.languages.count(preferences.fallbackSubtitles));
    }
    if (audio && subtitles)
      return true;
  }
  return false;
}

Title GetTitle(const CFileItem& item)
{
  if (item.m_bIsFolder)
    return {};

  const std::string& path = item.IsVideoDb() && item.HasVideoInfoTag()
                                ? item.GetVideoInfoTag()->m_strFileNameAndPath
                                : item.GetPath();
  const std::string library = std::string(ROOT) + "library/";
  const std::string translated = URIUtils::GetRealPath(CSpecialProtocol::TranslatePath(path));
  if (!URIUtils::PathHasParent(translated,
                               URIUtils::GetRealPath(CSpecialProtocol::TranslatePath(library))))
    return {};

  static const std::regex name(
      R"((movie|episode)-tmdb-([1-9][0-9]*)(?:\.(s[0-9]{2}e[0-9]{2,3}))?\.mkv)", std::regex::icase);
  const std::string filename = URIUtils::GetFileName(path);
  std::smatch match;
  if (!std::regex_match(filename, match, name))
    return {};

  std::string type = match[1].str();
  std::string episode = match[3].str();
  StringUtils::ToLower(type);
  StringUtils::ToLower(episode);
  if ((type == "episode") != !episode.empty())
    return {};
  return {path, type + ":tmdb:" + match[2].str(), episode};
}

CVariant LoadIndexEntry(const Title& title)
{
  const CVariant index = ReadJson(std::string(ROOT) + "index.json");
  const CVariant& entry = index[title.key];
  if (!entry.isObject() || entry["episode"].asString() != title.episode ||
      entry["title"].asString().size() > 255 || entry["query"].asString().size() > 255)
    return {};
  return entry;
}

Profile GetQualityProfile()
{
  const auto value =
      CServiceBroker::GetSettingsComponent()->GetSettings()->GetString("appliance.quality");
  if (value == "high")
    return Profile::HighQuality;
  if (value == "data")
    return Profile::DataSaver;
  return Profile::Balanced;
}

PlaybackLanguage GetPlaybackLanguage(const Title& title,
                                     const CFileItem& item,
                                     const CVariant& entry)
{
  const bool episode = !title.episode.empty();
  const char* setting = entry["anime"].asBoolean() ? "appliance.animelanguage"
                        : episode                  ? "appliance.tvlanguage"
                                                   : "appliance.movielanguage";
  PlaybackLanguage language{title.key, NormalizeLanguage(entry["originalLanguage"].asString()),
                            NormalizeLanguage(g_langInfo.GetAudioLanguage()),
                            NormalizeLanguage(g_langInfo.GetSubtitleLanguage()), setting};
  if (language.subtitles.empty())
    language.subtitles = language.audio;
  if (!episode || language.original.empty() || language.original == language.audio)
    return language;

  static const std::regex seriesId(R"([a-z][a-z0-9_-]{0,31}:[a-zA-Z0-9_]{1,64})");
  std::string series = entry["seriesId"].asString();
  if (!std::regex_match(series, seriesId) && item.HasVideoInfoTag() &&
      item.GetVideoInfoTag()->m_iIdShow > 0)
  {
    CVideoDatabase database;
    CVideoInfoTag show;
    if (database.Open() && database.GetTvShowInfo("", show, item.GetVideoInfoTag()->m_iIdShow))
      series = "tmdb:" + show.GetUniqueID("tmdb");
  }
  if (std::regex_match(series, seriesId))
    language.key = "series:" + series;
  return language;
}

std::optional<LanguageChoice> ChooseLanguage(const PlaybackLanguage& language, bool& remember)
{
  remember = false;
  if (language.original.empty() || language.audio.empty() || language.original == language.audio)
    return LanguageChoice::Ordinary;

  const auto saved = ReadChoice(language);
  if (saved != LanguageChoice::Ordinary)
    return saved;

  const auto value =
      CServiceBroker::GetSettingsComponent()->GetSettings()->GetString(language.setting);
  if (value == "sub")
    return LanguageChoice::Sub;
  if (value == "dub")
    return LanguageChoice::Dub;

  auto* dialog = CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(
      WINDOW_DIALOG_SELECT);
  if (!dialog)
    return std::nullopt;
  dialog->Reset();
  dialog->SetHeading(CVariant{42010});
  dialog->Add(ChoiceLabel(language, LanguageChoice::Sub));
  dialog->Add(ChoiceLabel(language, LanguageChoice::Dub));
  dialog->Open();
  const int index = dialog->GetSelectedItem();
  if (!dialog->IsConfirmed() || index < 0)
    return std::nullopt;
  remember = true;
  return index == 0 ? LanguageChoice::Sub : LanguageChoice::Dub;
}

void RememberLanguageChoice(const PlaybackLanguage& language, LanguageChoice choice)
{
  if (!SaveChoice(language, choice))
    CLog::Log(LOGWARNING, "Appliance: could not save the profile language choice");
}

bool EditLanguageChoice(const CFileItem& item)
{
  const auto title = GetTitle(item);
  if (title.key.empty())
    return false;
  const auto language = GetPlaybackLanguage(title, item, LoadIndexEntry(title));
  if (language.original.empty() || language.audio.empty() || language.original == language.audio)
    return false;

  auto* dialog = CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(
      WINDOW_DIALOG_SELECT);
  if (!dialog)
    return false;
  dialog->Reset();
  dialog->SetHeading(CVariant{42014});
  dialog->Add(ChoiceLabel(language, LanguageChoice::Sub));
  dialog->Add(ChoiceLabel(language, LanguageChoice::Dub));
  dialog->Add(g_localizeStrings.Get(42013));
  const auto current = ReadChoice(language);
  dialog->SetSelected(current == LanguageChoice::Sub ? 0 : current == LanguageChoice::Dub ? 1 : 2);
  const auto profiles = CServiceBroker::GetSettingsComponent()->GetProfileManager();
  const int profile = profiles->GetCurrentProfileId();
  dialog->Open();
  if (!dialog->IsConfirmed() || profiles->GetCurrentProfileId() != profile)
    return false;
  switch (dialog->GetSelectedItem())
  {
    case 0:
      return SaveChoice(language, LanguageChoice::Sub);
    case 1:
      return SaveChoice(language, LanguageChoice::Dub);
    case 2:
      return SaveChoice(language, LanguageChoice::Ordinary);
    default:
      return false;
  }
}

bool OfferLanguageAlternative(const PlaybackLanguage& language, LanguageChoice choice)
{
  auto* dialog = CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(
      WINDOW_DIALOG_SELECT);
  if (!dialog)
    return false;
  dialog->Reset();
  dialog->SetHeading(CVariant{42015});
  dialog->Add(ChoiceLabel(language, choice));
  dialog->Open();
  return dialog->IsConfirmed() && dialog->GetSelectedItem() == 0;
}

void SetLanguageRequirements(SelectionPreferences& preferences,
                             const PlaybackLanguage& language,
                             LanguageChoice choice)
{
  preferences.requiredAudio = choice == LanguageChoice::Sub ? language.original : language.audio;
  if (choice == LanguageChoice::Ordinary)
    preferences.requiredAudio.clear();
  preferences.requiredSubtitles = choice == LanguageChoice::Sub ? language.subtitles : "";
  preferences.fallbackSubtitles = choice == LanguageChoice::Sub ? language.audio : "";
}

bool PlaybackTracks::Matches(LanguageChoice choice) const
{
  return audio >= 0 && (choice != LanguageChoice::Sub || subtitle >= 0);
}

PlaybackTracks FindPlaybackTracks(CApplicationPlayer& player,
                                  const PlaybackLanguage& language,
                                  LanguageChoice choice)
{
  PlaybackTracks tracks;
  const auto& required = choice == LanguageChoice::Sub ? language.original : language.audio;
  const int currentAudio = player.GetAudioStream();
  for (int i = 0, count = player.GetAudioStreamCount(); i < count; ++i)
  {
    AudioStreamInfo info;
    player.GetAudioStreamInfo(i, info);
    if (info.valid && !(info.flags & FLAG_COMMENT) && NormalizeLanguage(info.language) == required)
    {
      tracks.audio = i;
      if (i == currentAudio)
        break;
    }
  }
  if (choice != LanguageChoice::Sub)
    return tracks;

  for (int i = 0, count = player.GetSubtitleCount(); i < count; ++i)
  {
    SubtitleStreamInfo info;
    player.GetSubtitleStreamInfo(i, info);
    if (!FullSubtitle(info))
      continue;
    const auto code = NormalizeLanguage(info.language);
    if (code == language.subtitles)
    {
      tracks.subtitle = i;
      break;
    }
    if (code == language.audio && tracks.subtitle < 0)
      tracks.subtitle = i;
  }
  return tracks;
}

bool VerifyPlaybackTracks(CApplicationPlayer& player,
                          const PlaybackLanguage& language,
                          LanguageChoice choice,
                          const PlaybackTracks& tracks)
{
  AudioStreamInfo audio;
  player.GetAudioStreamInfo(CURRENT_STREAM, audio);
  if (!audio.valid || (audio.flags & FLAG_COMMENT) ||
      NormalizeLanguage(audio.language) !=
          (choice == LanguageChoice::Sub ? language.original : language.audio))
    return false;
  if (choice == LanguageChoice::Dub)
    return true;

  SubtitleStreamInfo subtitle;
  player.GetSubtitleStreamInfo(CURRENT_STREAM, subtitle);
  if (!FullSubtitle(subtitle) || !player.GetSubtitleVisible())
    return false;
  SubtitleStreamInfo preferred;
  player.GetSubtitleStreamInfo(tracks.subtitle, preferred);
  return FullSubtitle(preferred) &&
         NormalizeLanguage(subtitle.language) == NormalizeLanguage(preferred.language);
}

TitleContext GetTitleContext(const Title& title, const CFileItem& item, const CVariant& entry)
{
  const int64_t duration = item.HasVideoInfoTag() ? item.GetVideoInfoTag()->GetDuration() : 0;
  const int64_t indexedDuration = std::max<int64_t>(0, ReadId(entry["runtimeSeconds"]));
  return {title.key, std::chrono::seconds{duration > 0 ? duration : indexedDuration},
          entry["lowMotionAnimation"].asBoolean(), entry["grainOrRestoration"].asBoolean()};
}

CVariant LoadProviders()
{
  return ReadJson(std::string(ROOT) + "providers.json");
}

std::vector<FileCandidate> LocalFiles(const CVariant& entry, const Title& title)
{
  std::vector<FileCandidate> files;
  const auto& local = entry["local"];
  if (!local.isArray())
    return files;
  for (auto it = local.begin_array(); it != local.end_array(); ++it)
  {
    const std::string path = it->isObject() ? (*it)["path"].asString() : it->asString();
    const CURL url(path);
    if (!IsLocalVideo(path, url))
      continue;

    const std::string canonical = URIUtils::GetRealPath(url.GetWithoutUserDetails());
    FileEvidence evidence = NameEvidence((*it)["format"].asString(), false);
    evidence.titleKey = title.key;
    evidence.reportedBy = "local index";
    evidence.size = FileSize::FromMemberBytes((*it)["bytes"]);
    if ((*it)["is3D"].isBoolean())
      evidence.is3D = (*it)["is3D"].asBoolean();
    if ((*it)["audioLanguages"].isArray())
      evidence.audio = VerifiedLanguages((*it)["audioLanguages"]);
    if ((*it)["fullSubtitleLanguages"].isArray())
      evidence.subtitles = VerifiedLanguages((*it)["fullSubtitleLanguages"]);
    files.push_back({{Tier::Local, {}, canonical}, {}, {std::move(evidence)}, path});
  }
  return files;
}

CVariant SelectFile(const CVariant& files, const CVariant& entry)
{
  CVariant selected;
  if (!files.isArray())
    return selected;

  for (auto it = files.begin_array(); it != files.end_array(); ++it)
  {
    if (ReadId((*it)["id"]) < 0 || !MatchesFile(*it, entry))
      continue;
    if (!selected.isNull())
      return {};
    selected = *it;
  }
  return selected;
}

std::vector<FileCandidate> Discover(const CVariant& entry,
                                    const CVariant& providers,
                                    const std::string& titleKey,
                                    const std::atomic_bool& cancelled)
{
  const std::string fixture = providers["fixture"].asString();
  const std::string query = entry["query"].asString();
  if (query.empty())
    return {};

  const std::string prowlarr = fixture.empty() ? providers["prowlarr"].asString() : fixture;
  const std::string apiKey = providers["prowlarrKey"].asString();
  const std::string torboxKey = providers["torboxKey"].asString();
  if (prowlarr.empty() || (fixture.empty() && (apiKey.empty() || torboxKey.empty())))
    return {};

  const CVariant releases =
      GetJson(prowlarr + "/api/v1/search?type=search&limit=" + std::to_string(RELEASE_LIMIT) +
                  "&query=" + CURL::Encode(query),
              cancelled, fixture.empty() ? "X-Api-Key: " + apiKey : std::string{});
  if (!releases.isArray())
    return {};

  std::map<std::string, std::vector<FileEvidence>> releaseEvidence;
  for (unsigned int i = 0; i < std::min(releases.size(), RELEASE_LIMIT); ++i)
  {
    const auto& release = releases[i];
    const auto hash = InfoHash(release);
    if (hash.empty())
      continue;
    FileEvidence evidence = NameEvidence(release["title"].asString(), true);
    evidence.reportedBy = "prowlarr";
    evidence.size = FileSize::FromMemberBytes(release["size"]);
    if (evidence.size.bytes)
      evidence.size.kind = SizeKind::PackTotal;
    releaseEvidence[hash].push_back(std::move(evidence));
  }
  if (releaseEvidence.empty())
    return {};

  // shortcut: only use ready torrents already in the pilot account until cached acquisition is implemented.
  const CVariant listing =
      GetJson((fixture.empty() ? TORBOX : fixture) + std::string(TORBOX_TORRENTS) +
                  "mylist?limit=" + std::to_string(TORRENT_LIMIT),
              cancelled, fixture.empty() ? "Authorization: Bearer " + torboxKey : std::string{});
  const CVariant& torrents = listing["data"];
  if (!listing["success"].asBoolean() || !torrents.isArray())
    return {};

  std::vector<FileCandidate> candidates;
  for (unsigned int i = 0; i < std::min(torrents.size(), TORRENT_LIMIT); ++i)
  {
    const auto& torrent = torrents[i];
    const int64_t torrentId = ReadId(torrent["id"]);
    const std::string hash = NormalizeInfoHash(torrent["hash"].asString());
    const auto release = releaseEvidence.find(hash);
    if (torrentId < 0 || release == releaseEvidence.end() ||
        !torrent["download_finished"].asBoolean() || !torrent["download_present"].asBoolean())
      continue;

    const CVariant selected = SelectFile(torrent["files"], entry);
    if (selected.isNull())
      continue;

    std::string member = selected["name"].asString();
    StringUtils::Replace(member, '\\', '/');
    FileEvidence file = NameEvidence(URIUtils::GetFileName(member), true);
    file.reportedBy = TORBOX_PROVIDER;
    file.titleKey = titleKey;
    file.size = FileSize::FromMemberBytes(selected["size"]);
    file.availability = Availability::Available;

    auto evidence = release->second;
    for (auto& report : evidence)
    {
      if (file.resolution)
        report.resolution = 0;
      if (!file.codec.empty())
        report.codec.clear();
      if (file.source != Source::Unknown)
        report.source = Source::Unknown;
      if (!file.hdr.empty())
        report.hdr.clear();
    }
    evidence.push_back(std::move(file));
    candidates.push_back(
        {{Tier::Remote, hash, std::move(member)},
         {{TORBOX_PROVIDER, std::to_string(torrentId), std::to_string(ReadId(selected["id"]))}},
         std::move(evidence)});
  }
  return candidates;
}

std::string Resolve(const CVariant& providers,
                    const ResolverRoute& route,
                    const std::atomic_bool& cancelled)
{
  if (route.provider != TORBOX_PROVIDER || route.torrentId.empty() || route.fileId.empty())
    return {};

  const std::string fixture = providers["fixture"].asString();
  const std::string key = providers["torboxKey"].asString();
  if (fixture.empty() && key.empty())
    return {};

  const CVariant response = GetJson(
      (fixture.empty() ? TORBOX : fixture) + std::string(TORBOX_TORRENTS) + "requestdl?" +
      (fixture.empty() ? "token=" + CURL::Encode(key) + "&" : std::string{}) +
      "torrent_id=" + CURL::Encode(route.torrentId) + "&file_id=" + CURL::Encode(route.fileId),
      cancelled);
  if (!response["success"].asBoolean())
    return {};

  const std::string path = response["data"].asString();
  const CURL url(path);
  return IsHttp(url) && (!fixture.empty() || url.IsProtocol("https")) ? path : std::string{};
}
} // namespace KODI::APPLIANCE
