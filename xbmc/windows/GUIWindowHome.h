/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "FileItem.h"
#include "guilib/GUIWindow.h"
#include "interfaces/IAnnouncer.h"
#include "utils/Job.h"
#include "utils/Variant.h"

#include <atomic>

class CGUIWindowHome :
      public CGUIWindow,
      public ANNOUNCEMENT::IAnnouncer,
      public IJobCallback
{
public:
  CGUIWindowHome(void);
  ~CGUIWindowHome(void) override;
  void OnInitWindow() override;
  void Announce(ANNOUNCEMENT::AnnouncementFlag flag,
                const std::string& sender,
                const std::string& message,
                const CVariant& data) override;

  bool OnMessage(CGUIMessage& message) override;
  bool OnAction(const CAction &action) override;

  void OnJobComplete(unsigned int jobID, bool success, CJob *job) override;
private:
  enum class CatalogRequest
  {
    Search,
    Show,
    Season,
    Movie
  };

  void OnDeinitWindow(int nextWindowID) override;
  void ShowTitles();
  void OpenCatalogItem(const CFileItem& item);
  void RequestCatalog(CatalogRequest request, const std::string& route);
  void CancelCatalog();
  void OpenApplianceSettings();
  void OpenMediaFolders();
  void BrowseUnmatched(const std::string& folder, const CVariant& unmatched);
  void ScanMediaFolder(std::string folder);

  CFileItemList m_titles;
  CVariant m_catalogSearch;
  CVariant m_catalogShow;
  CVariant m_catalogSeason;
  std::shared_ptr<std::atomic_bool> m_catalogRequest;
  std::string m_search;
  int m_titleView{0};
  bool m_importing{false};

  int m_updateRA; // flag for which recently added items needs to be queried
  void AddRecentlyAddedJobs(int flag);

  bool m_recentlyAddedRunning = false;
  int m_cumulativeUpdateFlag = 0;
};
