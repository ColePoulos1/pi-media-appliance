/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AppliancePlayback.h"

#include "FileItem.h"
#include "URL.h"
#include "filesystem/DllLibCurl.h"
#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"
#include "utils/JSONVariantParser.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "video/VideoInfoTag.h"

#include <algorithm>
#include <memory>
#include <regex>
#include <set>

namespace KODI::APPLIANCE
{
namespace
{
constexpr char ROOT[] = "special://profile/appliance/";
constexpr char TORBOX[] = "https://api.torbox.app/v1/api/torrents/";
constexpr size_t MAX_RESPONSE = 2 * 1024 * 1024;

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

CVariant GetJson(const std::string& url, const std::string& header = {})
{
  if (!IsHttp(CURL(url)) || header.find_first_of("\r\n") != std::string::npos)
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

  if (hash.size() != 40 || hash.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
    return {};
  StringUtils::ToLower(hash);
  return hash;
}

bool MatchesFile(const CVariant& file, const CVariant& entry)
{
  const std::string name = URIUtils::GetFileName(file["name"].asString());
  if (name.size() > 255 ||
      !URIUtils::HasExtension(name, ".mkv|.mp4|.m4v|.avi|.webm|.mov|.ts|.m2ts"))
    return false;

  std::string stem = Words(name.substr(0, name.rfind('.')));
  const std::string title = Words(entry["title"].asString());
  const std::string episode = Words(entry["episode"].asString());
  if (title.empty())
    return false;

  static const std::regex releaseSuffix(
      R"( (?:2160p|1080p|720p|480p|web dl|webrip|bluray|hdtv|remux|x264|x265|hevc)(?: |$))");
  std::smatch suffix;
  if (std::regex_search(stem, suffix, releaseSuffix))
    stem.resize(suffix.position());

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
} // namespace

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

CVariant LoadProviders()
{
  return ReadJson(std::string(ROOT) + "providers.json");
}

std::vector<std::string> LocalFiles(const CVariant& entry)
{
  std::vector<std::string> files;
  const auto& local = entry["local"];
  if (!local.isArray())
    return files;
  for (auto it = local.begin_array(); it != local.end_array() && files.size() < 4; ++it)
  {
    const std::string path = it->asString();
    const CURL url(path);
    if (path.empty() || (!url.GetProtocol().empty() && !url.IsProtocol("smb")) ||
        (url.GetProtocol().empty() && !CURL::IsFullPath(path)) ||
        !URIUtils::HasExtension(path, ".mkv|.mp4|.m4v|.avi|.webm|.mov|.ts|.m2ts") ||
        std::find(files.begin(), files.end(), path) != files.end())
      continue;
    files.push_back(path);
  }
  return files;
}

RemoteFile SelectFile(const CVariant& files, const CVariant& entry)
{
  RemoteFile selected;
  if (!files.isArray())
    return selected;

  for (auto it = files.begin_array(); it != files.end_array(); ++it)
  {
    const CVariant& fileId = (*it)["id"];
    const CVariant& fileSize = (*it)["size"];
    if ((!fileId.isInteger() && !fileId.isUnsignedInteger()) ||
        (!fileSize.isUnsignedInteger() && !(fileSize.isInteger() && fileSize.asInteger() > 0)))
      continue;
    const int64_t id = fileId.asInteger(-1);
    const uint64_t bytes = fileSize.asUnsignedInteger();
    if (id < 0 || bytes == 0 || !MatchesFile(*it, entry))
      continue;
    if (selected.id >= 0)
      return {};
    selected = {-1, id, bytes};
  }
  return selected;
}

RemoteFile Discover(const CVariant& entry, const CVariant& providers)
{
  const std::string fixture = providers["fixture"].asString();
  const std::string query = entry["query"].asString();
  if (query.empty())
    return {};
  if (!fixture.empty())
    return SelectFile(GetJson(fixture + "/discover?query=" + CURL::Encode(query)), entry);

  const std::string prowlarr = providers["prowlarr"].asString();
  const std::string apiKey = providers["prowlarrKey"].asString();
  const std::string torboxKey = providers["torboxKey"].asString();
  if (prowlarr.empty() || apiKey.empty() || torboxKey.empty())
    return {};

  const CVariant releases =
      GetJson(prowlarr + "/api/v1/search?type=search&limit=50&query=" + CURL::Encode(query),
              "X-Api-Key: " + apiKey);
  if (!releases.isArray())
    return {};

  std::set<std::string> hashes;
  for (auto it = releases.begin_array(); it != releases.end_array(); ++it)
  {
    const auto hash = InfoHash(*it);
    if (!hash.empty())
      hashes.insert(hash);
  }
  if (hashes.empty())
    return {};

  // shortcut: only use ready torrents already in the pilot account until cached acquisition is implemented.
  const CVariant listing =
      GetJson(std::string(TORBOX) + "mylist?limit=200", "Authorization: Bearer " + torboxKey);
  const CVariant& torrents = listing["data"];
  if (!listing["success"].asBoolean() || !torrents.isArray())
    return {};

  for (auto it = torrents.begin_array(); it != torrents.end_array(); ++it)
  {
    const int64_t torrentId = (*it)["id"].asInteger(-1);
    std::string hash = (*it)["hash"].asString();
    StringUtils::ToLower(hash);
    if (torrentId < 0 || !hashes.count(hash) || !(*it)["download_finished"].asBoolean() ||
        !(*it)["download_present"].asBoolean())
      continue;

    RemoteFile selected = SelectFile((*it)["files"], entry);
    if (selected.id >= 0)
    {
      selected.torrentId = torrentId;
      return selected;
    }
  }
  return {};
}

std::string Resolve(const CVariant& entry, const CVariant& providers, const RemoteFile& file)
{
  if (file.id < 0)
    return {};

  std::string url;
  if (file.torrentId < 0)
  {
    const std::string fixture = providers["fixture"].asString();
    if (fixture.empty())
      return {};
    const CVariant response =
        GetJson(fixture + "/resolve?query=" + CURL::Encode(entry["query"].asString()) +
                "&file_id=" + std::to_string(file.id));
    url = response["url"].asString();
  }
  else
  {
    const std::string key = providers["torboxKey"].asString();
    if (key.empty())
      return {};
    const CVariant response = GetJson(std::string(TORBOX) + "requestdl?token=" + CURL::Encode(key) +
                                      "&torrent_id=" + std::to_string(file.torrentId) +
                                      "&file_id=" + std::to_string(file.id));
    if (!response["success"].asBoolean())
      return {};
    url = response["data"].asString();
    if (!CURL(url).IsProtocol("https"))
      return {};
  }
  return IsHttp(CURL(url)) ? url : std::string{};
}
} // namespace KODI::APPLIANCE
