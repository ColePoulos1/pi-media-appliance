/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GUIWindowHome.h"

#include "PasswordManager.h"
#include "ServiceBroker.h"
#include "URL.h"
#include "addons/AddonManager.h"
#include "addons/Scraper.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "dialogs/GUIDialogFileBrowser.h"
#include "dialogs/GUIDialogOK.h"
#include "dialogs/GUIDialogSelect.h"
#include "dialogs/GUIDialogYesNo.h"
#include "filesystem/Directory.h"
#include "filesystem/SpecialProtocol.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIKeyboardFactory.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/LocalizeStrings.h"
#include "guilib/WindowIDs.h"
#include "input/actions/Action.h"
#include "input/actions/ActionIDs.h"
#include "interfaces/AnnouncementManager.h"
#include "interfaces/builtins/Builtins.h"
#include "network/Network.h"
#include "profiles/ProfileManager.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/Setting.h"
#include "storage/MediaManager.h"
#include "utils/JobManager.h"
#include "utils/RecentlyAddedJob.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/Variant.h"
#include "utils/log.h"
#include "video/AppliancePlayback.h"
#include "video/Bookmark.h"
#include "video/VideoDatabase.h"
#include "video/VideoInfoScanner.h"
#include "video/VideoInfoTag.h"
#include "video/VideoLibraryQueue.h"
#include "video/VideoUtils.h"
#include "video/dialogs/GUIDialogVideoInfo.h"

#include <algorithm>
#include <mutex>
#include <random>
#include <string_view>

namespace
{
constexpr int USE_LOCAL_MEDIA = 10;
constexpr int SEARCH = 11;
constexpr int CONTINUE_WATCHING = 13;
constexpr int APPLIANCE_SETTINGS = 14;
constexpr int TORBOX_ACCOUNT = 16;
constexpr int NETWORK_SETTINGS = 17;
constexpr int TITLE_LIST = 20;
constexpr int IMPORT_FINISHED = GUI_MSG_USER + 1;
constexpr int CATALOG_FINISHED = GUI_MSG_USER + 2;
constexpr char LOCAL_ONLY[] = "appliance.localonly";
constexpr char NETWORK_ADDON[] = "service.libreelec.settings";

bool HasNetworkSettings()
{
  return CServiceBroker::GetAddonMgr().IsAddonInstalled(NETWORK_ADDON);
}

void OpenNetworkSettings()
{
  CBuiltins::GetInstance().Execute("RunScript(service.libreelec.settings,network)");
}

void OpenRemoteSettings()
{
  auto& windows = CServiceBroker::GetGUI()->GetWindowManager();
  auto* dialog = windows.GetWindow<CGUIDialogSelect>(WINDOW_DIALOG_SELECT);
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  const bool enabled = settings->GetBool(CSettings::SETTING_SERVICES_WEBSERVER);
  const bool authenticated =
      enabled && settings->GetBool(CSettings::SETTING_SERVICES_WEBSERVERAUTHENTICATION) &&
      !settings->GetBool(CSettings::SETTING_SERVICES_ESALLINTERFACES) &&
      !settings->GetString(CSettings::SETTING_SERVICES_WEBSERVERPASSWORD).empty();
  dialog->Reset();
  dialog->SetHeading(CVariant{42073});
  dialog->Add(g_localizeStrings.Get(authenticated ? 42074 : 42075));
  if (enabled)
  {
    dialog->Add(g_localizeStrings.Get(42076));
    dialog->Add(g_localizeStrings.Get(42077));
  }
  dialog->Open();
  if (!dialog->IsConfirmed())
    return;

  const int action = dialog->GetSelectedItem();
  if (action == 2)
  {
    if (!settings->SetBool(CSettings::SETTING_SERVICES_WEBSERVER, false) || !settings->Save())
      CGUIDialogOK::ShowAndGetInput(CVariant{42073}, CVariant{42032});
    return;
  }

  const auto* connection = CServiceBroker::GetNetwork().GetFirstConnectedInterface();
  const auto address = connection ? connection->GetCurrentIPAddress() : std::string{};
  if (address.empty())
  {
    CGUIDialogOK::ShowAndGetInput(CVariant{42073}, CVariant{42078});
    return;
  }

  if (!authenticated || action == 1)
  {
    constexpr std::string_view alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    std::random_device random;
    std::uniform_int_distribution<size_t> symbol(0, alphabet.size() - 1);
    std::string password(20, '\0');
    std::generate(password.begin(), password.end(), [&] { return alphabet[symbol(random)]; });
    if (!authenticated &&
        (!settings->SetBool(CSettings::SETTING_SERVICES_ESALLINTERFACES, false) ||
         !settings->SetBool(CSettings::SETTING_SERVICES_WEBSERVER, false) ||
         !settings->SetBool(CSettings::SETTING_SERVICES_WEBSERVERAUTHENTICATION, true)))
    {
      CGUIDialogOK::ShowAndGetInput(CVariant{42073}, CVariant{42032});
      return;
    }
    if (!settings->SetString(CSettings::SETTING_SERVICES_WEBSERVERPASSWORD, password))
    {
      CGUIDialogOK::ShowAndGetInput(CVariant{42073}, CVariant{42032});
      return;
    }
    // Kodi asks for confirmation when its HTTP listener is enabled.
    if (!authenticated && !settings->SetBool(CSettings::SETTING_SERVICES_WEBSERVER, true))
      return;
    if (!settings->Save())
    {
      settings->SetBool(CSettings::SETTING_SERVICES_WEBSERVER, false);
      CGUIDialogOK::ShowAndGetInput(CVariant{42073}, CVariant{42032});
      return;
    }
  }

  CGUIDialogOK::ShowAndGetInput(
      CVariant{42073}, CVariant{StringUtils::Format(
                           g_localizeStrings.Get(42079), address,
                           settings->GetInt(CSettings::SETTING_SERVICES_WEBSERVERPORT),
                           settings->GetString(CSettings::SETTING_SERVICES_WEBSERVERUSERNAME),
                           settings->GetString(CSettings::SETTING_SERVICES_WEBSERVERPASSWORD))});
}

void OfferRemoteSetup()
{
  if (!HasNetworkSettings() || !CServiceBroker::GetNetwork().IsConnected())
    return;
  auto* dialog = CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(
      WINDOW_DIALOG_SELECT);
  dialog->Reset();
  dialog->SetHeading(CVariant{42073});
  dialog->Add(g_localizeStrings.Get(42080));
  dialog->Add(g_localizeStrings.Get(42075));
  dialog->Open();
  if (dialog->IsConfirmed() && dialog->GetSelectedItem() == 1)
    OpenRemoteSettings();
}

void EditAppliancePreferences(CGUIDialogSelect& dialog,
                              std::initializer_list<const char*> ids,
                              int heading,
                              bool allowProwlarr = false)
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  std::vector<std::shared_ptr<CSettingString>> entries;
  dialog.Reset();
  dialog.SetHeading(CVariant{heading});
  for (const char* id : ids)
  {
    const auto setting = std::static_pointer_cast<CSettingString>(settings->GetSetting(id));
    if (setting)
    {
      entries.push_back(setting);
      dialog.Add(g_localizeStrings.Get(setting->GetLabel()));
    }
  }
  if (allowProwlarr)
    dialog.Add(g_localizeStrings.Get(42069));
  dialog.Open();
  if (!dialog.IsConfirmed())
    return;
  if (allowProwlarr && static_cast<size_t>(dialog.GetSelectedItem()) == entries.size())
  {
    KODI::APPLIANCE::ConfigureProwlarr();
    return;
  }
  const auto setting = entries.at(dialog.GetSelectedItem());
  dialog.Reset();
  dialog.SetHeading(CVariant{setting->GetLabel()});
  StringSettingOptions options;
  if (setting->GetOptionsType() == SettingOptionsType::Dynamic)
    options = setting->UpdateDynamicOptions();
  else
  {
    for (const auto& [label, value] : setting->GetTranslatableOptions())
      options.emplace_back(g_localizeStrings.Get(label), value);
  }
  auto previous = setting->GetValue();
  for (const auto& option : options)
  {
    const auto index = dialog.Add(option.label);
    if (option.value == previous)
      dialog.SetSelected(index);
  }
  dialog.Open();
  if (dialog.IsConfirmed() &&
      (!setting->SetValue(options.at(dialog.GetSelectedItem()).value) || !settings->Save()))
  {
    setting->SetValue(previous);
    CGUIDialogOK::ShowAndGetInput(CVariant{heading}, CVariant{42032});
  }
}

struct FolderImport : public CGUIListItem
{
  std::string root;
  std::set<std::string> paths;
};

struct CatalogResult : public CGUIListItem
{
  CVariant response;
  std::shared_ptr<std::atomic_bool> cancelled;
};
} // namespace

CGUIWindowHome::CGUIWindowHome(void) : CGUIWindow(WINDOW_HOME, "Home.xml")
{
  m_updateRA = (Audio | Video | Totals);
  m_loadType = KEEP_IN_MEMORY;

  CServiceBroker::GetAnnouncementManager()->AddAnnouncer(this);
}

CGUIWindowHome::~CGUIWindowHome(void)
{
  CServiceBroker::GetAnnouncementManager()->RemoveAnnouncer(this);
}

bool CGUIWindowHome::OnAction(const CAction &action)
{
  static unsigned int min_hold_time = 1000;
  if (action.GetID() == ACTION_NAV_BACK && action.GetHoldTime() < min_hold_time)
  {
    const auto& components = CServiceBroker::GetAppComponents();
    const auto appPlayer = components.GetComponent<CApplicationPlayer>();
    if (appPlayer->IsPlaying() && (!appPlayer->IsRemotePlaying() || appPlayer->HasAudio()))
    {
      CGUIComponent* gui = CServiceBroker::GetGUI();
      if (gui)
        gui->GetWindowManager().SwitchToFullScreen();

      return true;
    }
  }
  if ((action.GetID() == ACTION_NAV_BACK || action.GetID() == ACTION_PREVIOUS_MENU) && m_titleView)
  {
    CancelCatalog();
    if (m_catalogShow.isObject() && m_titleView == SEARCH)
    {
      if (m_catalogSeason.isObject())
        m_catalogSeason = {};
      else
        m_catalogShow = {};
      ShowTitles();
      return true;
    }
    SET_CONTROL_FOCUS(m_titleView, 0);
    SET_CONTROL_HIDDEN(TITLE_LIST);
    m_titleView = 0;
    SetProperty("ApplianceSection", std::string{});
    SetProperty("ApplianceEmpty", std::string{});
    return true;
  }
  return CGUIWindow::OnAction(action);
}

void CGUIWindowHome::OnInitWindow()
{
  // for shared databases (ie mysql) always force an update on return to home
  // this is a temporary solution until remote announcements can be delivered
  if (StringUtils::EqualsNoCase(CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_databaseVideo.type, "mysql") ||
      StringUtils::EqualsNoCase(CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_databaseMusic.type, "mysql") )
    m_updateRA = (Audio | Video | Totals);
  AddRecentlyAddedJobs( m_updateRA );

  const bool setupComplete = KODI::APPLIANCE::IsSetupComplete();
  SetProperty("ApplianceSetupComplete", setupComplete);
  SetProperty("ApplianceNetworkSettings", HasNetworkSettings());
  CGUIWindow::OnInitWindow();
  if (!setupComplete)
  {
    const int step = HasNetworkSettings() && !CServiceBroker::GetNetwork().IsConnected()
                         ? NETWORK_SETTINGS
                         : USE_LOCAL_MEDIA;
    SET_CONTROL_FOCUS(step, 0);
  }
  if (m_titleView)
    ShowTitles();
  else
  {
    SET_CONTROL_HIDDEN(TITLE_LIST);
  }
}

void CGUIWindowHome::OnDeinitWindow(int nextWindowID)
{
  CancelCatalog();
  CGUIWindow::OnDeinitWindow(nextWindowID);
}

void CGUIWindowHome::ShowTitles()
{
  CGUIMessage selected(GUI_MSG_ITEM_SELECTED, GetID(), TITLE_LIST);
  OnMessage(selected);
  const int index = selected.GetParam1();
  const auto current = index >= 0 && index < m_titles.Size() ? m_titles.Get(index) : nullptr;

  m_titles.Clear();
  const bool browsing = m_titleView == SEARCH && m_catalogShow.isObject();
  std::set<std::string> titleKeys;
  CVideoDatabase database;
  if (!browsing && database.Open())
  {
    CFileItemList movies;
    CFileItemList episodes;
    database.GetMoviesNav("videodb://movies/titles/", movies);
    database.GetEpisodesNav("videodb://tvshows/titles/-1/-1/", episodes);
    for (const auto* items : {&movies, &episodes})
    {
      for (const auto& item : *items)
      {
        const auto key = KODI::APPLIANCE::GetTitle(*item).key;
        if (key.empty())
          continue;
        if (m_titleView == CONTINUE_WATCHING
                ? item->GetVideoInfoTag()->GetResumePoint().IsPartWay()
                : StringUtils::FindWords(item->GetLabel().c_str(), m_search.c_str()) !=
                      std::string::npos)
        {
          if (titleKeys.insert(key).second)
            m_titles.Add(item);
        }
      }
    }
  }
  if (m_titleView == SEARCH)
  {
    const bool episodes = m_catalogSeason.isObject();
    const CVariant& rows = episodes ? m_catalogSeason["episodes"]
                          : browsing ? m_catalogShow["seasons"]
                                     : m_catalogSearch["results"];
    for (unsigned int i = 0; i < rows.size() && rows.isArray(); ++i)
    {
      const auto& row = rows[i];
      const auto id = row["id"].asInteger();
      const auto type = episodes ? "episode" : browsing ? "season" : row["media_type"].asString();
      if (id <= 0 || (type != "movie" && type != "tv" && type != "season" && type != "episode"))
        continue;
      const auto key = type + ":tmdb:" + std::to_string(id);
      if (!titleKeys.insert(key).second)
        continue;
      const auto name = row[type == "movie" ? "title" : "name"].asString();
      if (name.empty() || name.size() > 255)
        continue;
      auto item = std::make_shared<CFileItem>("appliance://catalog/" + key, false);
      item->SetLabel(name);
      const auto image = KODI::APPLIANCE::CatalogImage(row[episodes ? "still_path" : "poster_path"]);
      if (!image.empty())
        item->SetArt("thumb", image);
      item->SetProperty("ApplianceCatalogType", type);
      item->SetProperty("ApplianceCatalogRow", i);
      m_titles.Add(item);
    }
  }
  int restore = 0;
  for (int i = 0; current && i < m_titles.Size(); ++i)
  {
    if (m_titles.Get(i)->IsPath(current->GetPath()))
      restore = i;
  }
  SetProperty("ApplianceSection", browsing ? m_catalogShow["name"].asString()
                                          : g_localizeStrings.Get(m_titleView == SEARCH ? 137 : 42025));
  SetProperty("ApplianceEmpty", m_titles.IsEmpty()
                                    ? g_localizeStrings.Get(m_catalogRequest ? 42062
                                                           : m_titleView == SEARCH ? 42028 : 42029)
                                    : std::string{});
  CGUIMessage bind(GUI_MSG_LABEL_BIND, GetID(), TITLE_LIST, restore);
  bind.SetPointer(&m_titles);
  OnMessage(bind);
  if (m_titles.IsEmpty() && GetFocusedControlID() == TITLE_LIST)
  {
    SET_CONTROL_FOCUS(m_titleView, 0);
  }
  CGUIMessage visibility(m_titles.IsEmpty() ? GUI_MSG_HIDDEN : GUI_MSG_VISIBLE, GetID(), TITLE_LIST);
  OnMessage(visibility);
}

void CGUIWindowHome::CancelCatalog()
{
  if (m_catalogRequest)
  {
    *m_catalogRequest = true;
    m_catalogRequest.reset();
  }
}

void CGUIWindowHome::RequestCatalog(CatalogRequest request, const std::string& route)
{
  CancelCatalog();
  if (KODI::APPLIANCE::IsLocalOnly())
    return;
  m_catalogRequest = std::make_shared<std::atomic_bool>(false);
  const int profile =
      CServiceBroker::GetSettingsComponent()->GetProfileManager()->GetCurrentProfileId();
  CServiceBroker::GetJobManager()->Submit(
      [request, route, profile, cancelled = m_catalogRequest]
      {
        auto result = std::make_shared<CatalogResult>();
        result->response = KODI::APPLIANCE::QueryCatalog(route, *cancelled);
        if (*cancelled)
          return;
        result->cancelled = cancelled;
        CGUIMessage message(CATALOG_FINISHED, 0, 0, profile, static_cast<int>(request), result);
        CServiceBroker::GetGUI()->GetWindowManager().SendThreadMessage(message, WINDOW_HOME);
      },
      CJob::PRIORITY_NORMAL);
}

void CGUIWindowHome::OpenCatalogItem(const CFileItem& item)
{
  const auto type = item.GetProperty("ApplianceCatalogType").asString();
  const auto i = item.GetProperty("ApplianceCatalogRow").asUnsignedInteger();
  if (type == "movie" || type == "tv")
  {
    const auto id = m_catalogSearch["results"][static_cast<unsigned int>(i)]["id"].asInteger();
    RequestCatalog(type == "movie" ? CatalogRequest::Movie : CatalogRequest::Show,
                   StringUtils::Format("/{}/{}", type, id));
  }
  else if (type == "season")
  {
    const auto number =
        m_catalogShow["seasons"][static_cast<unsigned int>(i)]["season_number"].asInteger(-1);
    if (number >= 0)
      RequestCatalog(CatalogRequest::Season,
                     StringUtils::Format("/tv/{}/season/{}", m_catalogShow["id"].asInteger(), number));
  }
  else if (type == "episode")
  {
    const CVariant& row = m_catalogSeason["episodes"][static_cast<unsigned int>(i)];
    if (const auto title = KODI::APPLIANCE::SaveCatalogTitle(row, m_catalogShow))
      CGUIDialogVideoInfo::ShowFor(*title);
    else
      CGUIDialogOK::ShowAndGetInput(CVariant{137}, CVariant{42063});
  }
}

void CGUIWindowHome::OpenApplianceSettings()
{
  auto* dialog = CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(
      WINDOW_DIALOG_SELECT);
  dialog->Reset();
  dialog->SetHeading(CVariant{5});
  dialog->Add(g_localizeStrings.Get(42026));
  dialog->Add(g_localizeStrings.Get(42033));
  dialog->Add(g_localizeStrings.Get(42000));
  dialog->Add(g_localizeStrings.Get(42045));
  dialog->Add(g_localizeStrings.Get(42073));
  if (HasNetworkSettings())
    dialog->Add(g_localizeStrings.Get(42039));
  dialog->Open();
  if (!dialog->IsConfirmed())
    return;

  if (dialog->GetSelectedItem() == 1)
  {
    if (KODI::APPLIANCE::ConfigureTorBox() && KODI::APPLIANCE::IsLocalOnly())
    {
      CancelCatalog();
      m_catalogSearch = {};
      m_catalogShow = {};
      m_catalogSeason = {};
      if (m_titleView)
        ShowTitles();
    }
    return;
  }
  if (dialog->GetSelectedItem() == 2)
  {
    EditAppliancePreferences(*dialog,
                             {CSettings::SETTING_LOCALE_AUDIOLANGUAGE,
                              CSettings::SETTING_LOCALE_SUBTITLELANGUAGE, "appliance.quality",
                              "appliance.movielanguage", "appliance.tvlanguage",
                              "appliance.animelanguage"},
                             42000);
    return;
  }
  if (dialog->GetSelectedItem() == 3)
  {
    EditAppliancePreferences(*dialog,
                             {"appliance.streamsize", "appliance.bandwidth",
                              "appliance.maxresolution"},
                             42045, true);
    return;
  }
  if (dialog->GetSelectedItem() == 4)
  {
    OpenRemoteSettings();
    return;
  }
  if (dialog->GetSelectedItem() == 5)
  {
    OpenNetworkSettings();
    return;
  }

  OpenMediaFolders();
}

void CGUIWindowHome::OpenMediaFolders()
{
  if (m_importing || CVideoLibraryQueue::GetInstance().IsScanningLibrary())
    return;

  auto* dialog = CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(
      WINDOW_DIALOG_SELECT);
  dialog->Reset();
  dialog->SetHeading(CVariant{42026});
  dialog->Add(g_localizeStrings.Get(42027));
  const auto folders = KODI::APPLIANCE::MediaFolders();
  std::vector<std::string> paths;
  for (auto it = folders.begin_map(); it != folders.end_map(); ++it)
  {
    paths.push_back(it->first);
    dialog->Add(it->first);
  }
  dialog->Open();
  if (!dialog->IsConfirmed())
    return;

  const int index = dialog->GetSelectedItem();
  if (index == 0)
  {
    VECSOURCES drives;
    CServiceBroker::GetMediaManager().GetRemovableDrives(drives);
    std::string folder;
    if (!CGUIDialogFileBrowser::ShowAndGetSource(folder, true, &drives))
      return;
    const CURL source(folder);
    if (source.IsProtocol("smb") && !source.GetUserName().empty())
      CPasswordManager::GetInstance().SaveAuthenticatedURL(source);
    ScanMediaFolder(URIUtils::GetRealPath(
        CSpecialProtocol::TranslatePath(source.GetWithoutUserDetails())));
    return;
  }

  const auto& folder = paths.at(index - 1);
  const auto& unmatched = folders[folder]["unmatched"];
  dialog->Reset();
  dialog->SetHeading(CVariant{folder});
  dialog->Add(g_localizeStrings.Get(42064));
  dialog->Add(g_localizeStrings.Get(42065));
  if (!unmatched.empty())
    dialog->Add(g_localizeStrings.Get(42066));
  dialog->Open();
  if (!dialog->IsConfirmed())
    return;
  if (dialog->GetSelectedItem() == 0)
    ScanMediaFolder(folder);
  else if (dialog->GetSelectedItem() == 1)
  {
    if (CGUIDialogYesNo::ShowAndGetInput(CVariant{42065}, CVariant{folder}) &&
        !KODI::APPLIANCE::RemoveMediaFolder(folder))
      CGUIDialogOK::ShowAndGetInput(CVariant{42026}, CVariant{42032});
  }
  else
    BrowseUnmatched(folder, unmatched);
}

void CGUIWindowHome::BrowseUnmatched(const std::string& folder, const CVariant& unmatched)
{
  auto* dialog = CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(
      WINDOW_DIALOG_SELECT);
  dialog->Reset();
  dialog->SetHeading(CVariant{42066});
  std::vector<std::string> paths;
  for (auto it = unmatched.begin_map(); it != unmatched.end_map(); ++it)
  {
    paths.push_back(it->first);
    dialog->Add(URIUtils::GetFileName(it->first));
  }
  dialog->Open();
  if (!dialog->IsConfirmed())
    return;
  const auto& path = paths.at(dialog->GetSelectedItem());
  dialog->Reset();
  dialog->SetHeading(CVariant{URIUtils::GetFileName(path)});
  dialog->Add(g_localizeStrings.Get(208));
  dialog->Add(g_localizeStrings.Get(42067));
  dialog->Open();
  if (!dialog->IsConfirmed())
    return;
  if (dialog->GetSelectedItem() == 0)
  {
    VIDEO_UTILS::PlayItem(std::make_shared<CFileItem>(path, false), "");
    return;
  }

  CVideoDatabase database;
  if (!database.Open())
    return;
  CFileItemList movies;
  CFileItemList episodes;
  database.GetMoviesNav("videodb://movies/titles/", movies);
  database.GetEpisodesNav("videodb://tvshows/titles/-1/-1/", episodes);
  std::vector<KODI::APPLIANCE::Title> titles;
  dialog->Reset();
  dialog->SetHeading(CVariant{42067});
  for (const auto* items : {&movies, &episodes})
  {
    for (const auto& item : *items)
    {
      const auto title = KODI::APPLIANCE::GetTitle(*item);
      if (title.key.empty())
        continue;
      const auto* tag = item->GetVideoInfoTag();
      dialog->Add(title.episode.empty() ? item->GetLabel()
                                        : StringUtils::Format("{} · {} · {}", tag->m_strShowTitle,
                                                              title.episode, item->GetLabel()));
      titles.push_back(title);
    }
  }
  if (titles.empty())
  {
    CGUIDialogOK::ShowAndGetInput(CVariant{42067}, CVariant{42068});
    return;
  }
  dialog->Open();
  if (dialog->IsConfirmed() &&
      !KODI::APPLIANCE::MatchMediaFile(folder, path, titles.at(dialog->GetSelectedItem())))
    CGUIDialogOK::ShowAndGetInput(CVariant{42067}, CVariant{42032});
}

void CGUIWindowHome::ScanMediaFolder(std::string folder)
{
  URIUtils::AddSlashAtEnd(folder);
  m_importing = true;
  SetProperty("ApplianceImport", g_localizeStrings.Get(42030));
  const int profile =
      CServiceBroker::GetSettingsComponent()->GetProfileManager()->GetCurrentProfileId();
  CServiceBroker::GetJobManager()->Submit(
      [folder = CSpecialProtocol::TranslatePath(folder), profile,
       root = CSpecialProtocol::TranslatePath("special://profile/appliance/")]()
      {
        auto result = std::make_shared<FolderImport>();
        result->root = root;
        result->paths = KODI::APPLIANCE::ImportMediaFolder(folder, root);
        CGUIMessage message(IMPORT_FINISHED, 0, 0, profile, 0, result);
        CServiceBroker::GetGUI()->GetWindowManager().SendThreadMessage(message, WINDOW_HOME);
      });
}

void CGUIWindowHome::Announce(ANNOUNCEMENT::AnnouncementFlag flag,
                              const std::string& sender,
                              const std::string& message,
                              const CVariant& data)
{
  int ra_flag = 0;

  CLog::Log(LOGDEBUG, LOGANNOUNCE, "GOT ANNOUNCEMENT, type: {}, from {}, message {}",
            AnnouncementFlagToString(flag), sender, message);

  // we are only interested in library changes
  if ((flag & (ANNOUNCEMENT::VideoLibrary | ANNOUNCEMENT::AudioLibrary)) == 0)
    return;

  if (data.isMember("transaction") && data["transaction"].asBoolean())
    return;

  if (message == "OnScanStarted" || message == "OnCleanStarted")
    return;

  bool onUpdate = message == "OnUpdate";
  // always update Totals except on an OnUpdate with no playcount update
  if (!onUpdate || data.isMember("playcount"))
    ra_flag |= Totals;

  // always update the full list except on an OnUpdate
  if (!onUpdate)
  {
    if (flag & ANNOUNCEMENT::VideoLibrary)
      ra_flag |= Video;
    else if (flag & ANNOUNCEMENT::AudioLibrary)
      ra_flag |= Audio;
  }

  CGUIMessage reload(GUI_MSG_NOTIFY_ALL, GetID(), 0, GUI_MSG_REFRESH_THUMBS, ra_flag);
  CServiceBroker::GetGUI()->GetWindowManager().SendThreadMessage(reload, GetID());
}

void CGUIWindowHome::AddRecentlyAddedJobs(int flag)
{
  bool getAJob = false;

  // this block checks to see if another one is running
  // and keeps track of the flag
  {
    std::unique_lock<CCriticalSection> lockMe(*this);
    if (!m_recentlyAddedRunning)
    {
      getAJob = true;

      flag |= m_cumulativeUpdateFlag; // add the flags from previous calls to AddRecentlyAddedJobs

      m_cumulativeUpdateFlag = 0; // now taken care of in flag.
                                  // reset this since we're going to execute a job

      // we're about to add one so set the indicator
      if (flag)
        m_recentlyAddedRunning = true; // this will happen in the if clause below
    }
    else
      // since we're going to skip a job, mark that one came in and ...
      m_cumulativeUpdateFlag |= flag; // this will be used later
  }

  if (flag && getAJob)
    CServiceBroker::GetJobManager()->AddJob(new CRecentlyAddedJob(flag), this);

  m_updateRA = 0;
}

void CGUIWindowHome::OnJobComplete(unsigned int jobID, bool success, CJob *job)
{
  int flag = 0;

  {
    std::unique_lock<CCriticalSection> lockMe(*this);

    // the job is finished.
    // did one come in the meantime?
    flag = m_cumulativeUpdateFlag;
    m_recentlyAddedRunning = false; /// we're done.
  }

  if (flag)
    AddRecentlyAddedJobs(0 /* the flag will be set inside AddRecentlyAddedJobs via m_cumulativeUpdateFlag */ );
}


bool CGUIWindowHome::OnMessage(CGUIMessage& message)
{
  switch ( message.GetMessage() )
  {
  case GUI_MSG_CLICKED:
    switch (message.GetSenderId())
    {
      case USE_LOCAL_MEDIA:
      {
        auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
        if (!settings->SetBool(LOCAL_ONLY, true) || !settings->Save())
        {
          settings->SetBool(LOCAL_ONLY, false);
          CGUIDialogOK::ShowAndGetInput(CVariant{42024}, CVariant{42032});
          return true;
        }
        SetProperty("ApplianceSetupComplete", true);
        OfferRemoteSetup();
        SET_CONTROL_FOCUS(SEARCH, 0);
        return true;
      }
      case TORBOX_ACCOUNT:
        if (KODI::APPLIANCE::ConfigureTorBox())
        {
          SetProperty("ApplianceSetupComplete", true);
          OfferRemoteSetup();
          SET_CONTROL_FOCUS(SEARCH, 0);
        }
        return true;
      case NETWORK_SETTINGS:
        OpenNetworkSettings();
        return true;
      case SEARCH:
      {
        std::string query = m_search;
        if (!CGUIKeyboardFactory::ShowAndGetInput(query, CVariant{137}, true))
          return true;
        StringUtils::Trim(query);
        StringUtils::ToLower(query);
        m_search = std::move(query);
        CancelCatalog();
        m_catalogSearch = {};
        m_catalogShow = {};
        m_catalogSeason = {};
        if (!m_search.empty())
          RequestCatalog(CatalogRequest::Search, "/search/multi?query=" + CURL::Encode(m_search));
        [[fallthrough]];
      }
      case CONTINUE_WATCHING:
        if (message.GetSenderId() == CONTINUE_WATCHING)
          CancelCatalog();
        m_titleView = message.GetSenderId();
        ShowTitles();
        if (!m_titles.IsEmpty())
        {
          SET_CONTROL_FOCUS(TITLE_LIST, 0);
        }
        return true;
      case APPLIANCE_SETTINGS:
        OpenApplianceSettings();
        return true;
      case TITLE_LIST:
      {
        if (message.GetParam1() != ACTION_SELECT_ITEM &&
            message.GetParam1() != ACTION_MOUSE_LEFT_CLICK)
          break;
        CGUIMessage selected(GUI_MSG_ITEM_SELECTED, GetID(), TITLE_LIST);
        OnMessage(selected);
        const int index = selected.GetParam1();
        if (index >= 0 && index < m_titles.Size())
        {
          const auto item = m_titles.Get(index);
          if (item->HasProperty("ApplianceCatalogType"))
            OpenCatalogItem(*item);
          else
            CGUIDialogVideoInfo::ShowFor(*item);
        }
        return true;
      }
    }
    break;
  case IMPORT_FINISHED:
  {
    m_importing = false;
    SetProperty("ApplianceImport", std::string{});
    if (message.GetParam1() !=
        CServiceBroker::GetSettingsComponent()->GetProfileManager()->GetCurrentProfileId())
      return true;
    const auto result = std::static_pointer_cast<FolderImport>(message.GetItem());
    if (result->paths.empty())
    {
      SetProperty("ApplianceImport", g_localizeStrings.Get(42031));
      return true;
    }

    CVideoDatabase database;
    ADDON::AddonPtr addon;
    if (!database.Open() ||
        !CServiceBroker::GetAddonMgr().GetAddon("metadata.local", addon, ADDON::OnlyEnabled::CHOICE_YES))
    {
      SetProperty("ApplianceImport", g_localizeStrings.Get(42032));
      return true;
    }
    auto scraper = std::static_pointer_cast<ADDON::CScraper>(addon);
    const std::string movies = result->root + "library/movies/";
    const std::string shows = result->root + "library/shows/";
    if (XFILE::CDirectory::Exists(shows) || XFILE::CDirectory::Create(shows))
    {
      scraper->SetPathSettings(CONTENT_TVSHOWS, "");
      database.SetScraperForPath(shows, scraper, VIDEO::SScanSettings{});
    }
    scraper->SetPathSettings(CONTENT_MOVIES, "");
    database.SetScraperForPath(movies, scraper, VIDEO::SScanSettings{});
    for (const auto& path : result->paths)
    {
      if (!database.GetScraperForPath(path))
        database.SetScraperForPath(path, scraper, VIDEO::SScanSettings{});
      CVideoLibraryQueue::GetInstance().ScanLibrary(path, false, false);
    }
    return true;
  }
  case CATALOG_FINISHED:
  {
    const auto result = std::static_pointer_cast<CatalogResult>(message.GetItem());
    if (!IsActive() || m_titleView != SEARCH || result->cancelled != m_catalogRequest ||
        message.GetParam1() !=
            CServiceBroker::GetSettingsComponent()->GetProfileManager()->GetCurrentProfileId())
      return true;
    m_catalogRequest.reset();
    switch (static_cast<CatalogRequest>(message.GetParam2()))
    {
      case CatalogRequest::Movie:
      {
        if (const auto item = KODI::APPLIANCE::SaveCatalogTitle(result->response, {}))
          CGUIDialogVideoInfo::ShowFor(*item);
        else
          CGUIDialogOK::ShowAndGetInput(CVariant{137}, CVariant{42063});
        break;
      }
      case CatalogRequest::Show:
        if (result->response["seasons"].isArray())
          m_catalogShow = result->response;
        else
          CGUIDialogOK::ShowAndGetInput(CVariant{137}, CVariant{42063});
        break;
      case CatalogRequest::Season:
        if (result->response["episodes"].isArray())
          m_catalogSeason = result->response;
        else
          CGUIDialogOK::ShowAndGetInput(CVariant{137}, CVariant{42063});
        break;
      case CatalogRequest::Search:
        m_catalogSearch = result->response;
        break;
    }
    ShowTitles();
    if (GetFocusedControlID() == SEARCH && !m_titles.IsEmpty())
    {
      SET_CONTROL_FOCUS(TITLE_LIST, 0);
    }
    return true;
  }
  case GUI_MSG_NOTIFY_ALL:
    if (message.GetParam1() == GUI_MSG_WINDOW_RESET || message.GetParam1() == GUI_MSG_REFRESH_THUMBS)
    {
      int updateRA = (message.GetSenderId() == GetID()) ? message.GetParam2() : (Video | Audio | Totals);

      if (IsActive())
        AddRecentlyAddedJobs(updateRA);
      else
        m_updateRA |= updateRA;

      if (message.GetParam1() == GUI_MSG_WINDOW_RESET)
      {
        CancelCatalog();
        m_catalogSearch = {};
        m_catalogShow = {};
        m_catalogSeason = {};
        m_titleView = 0;
        m_search.clear();
        m_titles.Clear();
        SetProperty("ApplianceSection", std::string{});
        SetProperty("ApplianceEmpty", std::string{});
        SetProperty("ApplianceImport", std::string{});
      }
      else if (IsActive() && m_titleView)
        ShowTitles();
    }
    break;

  default:
    break;
  }

  return CGUIWindow::OnMessage(message);
}
