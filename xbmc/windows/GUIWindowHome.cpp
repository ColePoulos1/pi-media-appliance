/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GUIWindowHome.h"

#include "ServiceBroker.h"
#include "addons/AddonManager.h"
#include "addons/Scraper.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "dialogs/GUIDialogFileBrowser.h"
#include "dialogs/GUIDialogOK.h"
#include "dialogs/GUIDialogSelect.h"
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
#include "profiles/ProfileManager.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
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
#include "video/dialogs/GUIDialogVideoInfo.h"

#include <mutex>

namespace
{
constexpr int USE_LOCAL_MEDIA = 10;
constexpr int SEARCH = 11;
constexpr int CONTINUE_WATCHING = 13;
constexpr int APPLIANCE_SETTINGS = 14;
constexpr int TITLE_LIST = 20;
constexpr int IMPORT_FINISHED = GUI_MSG_USER + 1;
constexpr char LOCAL_ONLY[] = "appliance.localonly";

struct FolderImport : public CGUIListItem
{
  std::string root;
  std::set<std::string> paths;
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

  const bool localOnly = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(LOCAL_ONLY);
  SetProperty("ApplianceLocalOnly", localOnly);
  CGUIWindow::OnInitWindow();
  if (!localOnly)
  {
    SET_CONTROL_FOCUS(USE_LOCAL_MEDIA, 0);
  }
  if (m_titleView)
    ShowTitles();
  else
  {
    SET_CONTROL_HIDDEN(TITLE_LIST);
  }
}

void CGUIWindowHome::ShowTitles()
{
  CGUIMessage selected(GUI_MSG_ITEM_SELECTED, GetID(), TITLE_LIST);
  OnMessage(selected);
  const int index = selected.GetParam1();
  const auto current = index >= 0 && index < m_titles.Size() ? m_titles.Get(index) : nullptr;

  m_titles.Clear();
  CVideoDatabase database;
  if (database.Open())
  {
    CFileItemList movies;
    CFileItemList episodes;
    database.GetMoviesNav("videodb://movies/titles/", movies);
    database.GetEpisodesNav("videodb://tvshows/titles/-1/-1/", episodes);
    for (const auto* items : {&movies, &episodes})
    {
      for (const auto& item : *items)
      {
        if (KODI::APPLIANCE::GetTitle(*item).key.empty())
          continue;
        if (m_titleView == CONTINUE_WATCHING
                ? item->GetVideoInfoTag()->GetResumePoint().IsPartWay()
                : StringUtils::FindWords(item->GetLabel().c_str(), m_search.c_str()) !=
                      std::string::npos)
          m_titles.Add(item);
      }
    }
  }
  int restore = 0;
  for (int i = 0; current && i < m_titles.Size(); ++i)
  {
    if (m_titles.Get(i)->IsPath(current->GetPath()))
      restore = i;
  }
  SetProperty("ApplianceSection", g_localizeStrings.Get(m_titleView == SEARCH ? 137 : 42025));
  SetProperty("ApplianceEmpty", m_titles.IsEmpty()
                                    ? g_localizeStrings.Get(m_titleView == SEARCH ? 42028 : 42029)
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

void CGUIWindowHome::OpenApplianceSettings()
{
  auto* dialog = CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(
      WINDOW_DIALOG_SELECT);
  dialog->Reset();
  dialog->SetHeading(CVariant{5});
  dialog->Add(g_localizeStrings.Get(42026));
  dialog->Open();
  if (!dialog->IsConfirmed())
    return;

  dialog->Reset();
  dialog->SetHeading(CVariant{42026});
  dialog->Add(g_localizeStrings.Get(42027));
  dialog->Open();
  if (dialog->IsConfirmed())
    AddMediaFolder();
}

void CGUIWindowHome::AddMediaFolder()
{
  if (m_importing || CVideoLibraryQueue::GetInstance().IsScanningLibrary())
    return;

  VECSOURCES drives;
  auto& media = CServiceBroker::GetMediaManager();
  media.GetRemovableDrives(drives);
  media.GetLocalDrives(drives);
  std::string folder;
  if (!CGUIDialogFileBrowser::ShowAndGetDirectory(drives, g_localizeStrings.Get(42027), folder))
    return;

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
        result->paths = KODI::APPLIANCE::ImportMovieFolder(folder, root);
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
        SetProperty("ApplianceLocalOnly", true);
        SET_CONTROL_FOCUS(SEARCH, 0);
        return true;
      }
      case SEARCH:
      {
        std::string query = m_search;
        if (!CGUIKeyboardFactory::ShowAndGetInput(query, CVariant{137}, true))
          return true;
        StringUtils::Trim(query);
        StringUtils::ToLower(query);
        m_search = std::move(query);
        [[fallthrough]];
      }
      case CONTINUE_WATCHING:
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
          CGUIDialogVideoInfo::ShowFor(*m_titles.Get(index));
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
    if (XFILE::CDirectory::Create(shows) && scraper->SetPathSettings(CONTENT_TVSHOWS, ""))
      database.SetScraperForPath(shows, scraper, VIDEO::SScanSettings{});
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
