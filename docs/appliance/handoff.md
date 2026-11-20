# Engineering handoff

**20 November 2026 · implementation has not started.** This branch contains Kodi source and the appliance design. The source pins and entry points below have been checked. A custom image, selection implementation, tested discovery provider and measured Pi results are pending. Start with milestone 1. A fresh clone can use synthetic fixtures for desktop work while awaiting access to hardware.

## Reproducible starting point

| Component | Revision and role |
| --- | --- |
| Kodi | [xbmc/xbmc 21.3-Omega](https://github.com/xbmc/xbmc/tree/a3a448d26b8d560a65655dab2cd122994dc4e146), commit `a3a448d26b8d560a65655dab2cd122994dc4e146`. Branch `appliance` starts here. Preserve that ancestry when developing. |
| LibreELEC | [12.2.1](https://github.com/LibreELEC/LibreELEC.tv/tree/6209f5a3b067c45fbf527286dd84ee483294b46a), commit `6209f5a3b067c45fbf527286dd84ee483294b46a`. Its [Kodi recipe](https://github.com/LibreELEC/LibreELEC.tv/blob/12.2.1/packages/mediacenter/kodi/package.mk) selects 21.3-Omega. The [Pi 5 target](https://github.com/LibreELEC/LibreELEC.tv/tree/12.2.1/projects/RPi/devices/RPi5) builds the aarch64 image. |
| Control appliance | Stock [LibreELEC Pi 5 image](https://libreelec.tv/downloads/raspberry/) on a spare microSD. Record the owner's running version separately. Keep that working card available for comparisons. |
| Reference behavior | Fen Light+ 2.5.4 was inspected at [8941503](https://github.com/thejason40/FenLightPlus/tree/89415039efee51fb0718278fac8a9aba3e5f0897). Under `plugin.video.fenlight/resources/lib/`, `modules/sources.py` handles discovery/filtering/resolution, `modules/player.py` tracks progress, `scrapers/tb_cloud.py` and `scrapers/external.py` obtain size data. External pack values may be estimates, while TorBox may expose individual file bytes. |

The [public project fork](https://github.com/ColePoulos1/pi-media-appliance) owns application code and the pins/patches needed to package it. Push product work to its `appliance` branch. The upstream repository is [xbmc/xbmc](https://github.com/xbmc/xbmc). Maintain a small set of LibreELEC recipe/overlay changes when packaging work starts. Keep kernel, firmware and board packages tracking LibreELEC. The native/libVLC path is an independent experiment.

## Build and orient

On a fresh clone, use the appliance branch. To inspect or update the base, add `upstream` if the clone has only `origin`:

```sh
git switch appliance
git remote add upstream https://github.com/xbmc/xbmc.git
git fetch upstream tag 21.3-Omega
git diff --stat 21.3-Omega..HEAD
```

For desktop iteration follow the [Kodi Linux build instructions](../README.Linux.md) or [Windows instructions](../README.Windows.md). The Linux guide describes building `kodi-test` and filtering Google Test runs. Keep test fixture inputs synthetic. Use a Linux build host with the dependencies in the [LibreELEC build guide](https://wiki.libreelec.tv/development/build-basics). In a separate checkout the stock control build is:

```sh
git clone --branch 12.2.1 --single-branch https://github.com/LibreELEC/LibreELEC.tv.git
cd LibreELEC.tv
PROJECT=RPi DEVICE=RPi5 ARCH=aarch64 make image
```

Look in `target/` for the Pi 5 flash image and update artifact. Building stock is a toolchain check. To build the fork, point the Kodi recipe at this repository's public source archive for an exact commit, update its SHA-256, and retain the Pi patches. Route updates to matching project builds and keep persistent data under `/storage`. Capture the exact build command and hashes once a derivative is produced. [LibreELEC update formats](https://wiki.libreelec.tv/support/update)

## Source map for the first change

| Trace | Start here | What to establish |
| --- | --- | --- |
| Home and title actions | [Estuary Home](../../addons/skin.estuary/xml/Home.xml), [video info dialog](../../xbmc/video/dialogs/GUIDialogVideoInfo.cpp), [favorites service](../../xbmc/favourites/FavouritesService.cpp) | Route one appliance title through normal TV input. Preserve its stable identity when favorited or resumed. |
| Library and bookmarks | [video scanner](../../xbmc/video/VideoInfoScanner.cpp), [video database](../../xbmc/video/VideoDatabase.cpp), [save file state](../../xbmc/utils/SaveFileStateJob.cpp) | Determine how a library item maps to a playable path. The base computes a tracking path and restricts its handling of `original_listitem_url`. Prove the same bookmark survives a changing temporary URL and a switch to SMB. |
| Starting and stopping | [application](../../xbmc/application/Application.cpp), [player callbacks](../../xbmc/application/ApplicationPlayerCallback.cpp), [VideoPlayer](../../xbmc/cores/VideoPlayer/VideoPlayer.cpp) | Trace `CApplication::PlayFile`, explicit Stop, EOF and error. In this revision `OnPlayBackError` also calls `OnPlayBackStopped`. Use attempt-scoped intent and collapse duplicate notifications so a late callback cannot retry a newer play. |
| iOS remote | [player operations](../../xbmc/interfaces/json-rpc/PlayerOperations.cpp), [input](../../xbmc/interfaces/json-rpc/InputOperations.cpp), [video library](../../xbmc/interfaces/json-rpc/VideoLibrary.cpp), [favorites](../../xbmc/interfaces/json-rpc/FavouritesOperations.cpp) | A `Player.Open` request for an appliance library item must invoke the same title resolver as the TV. Test discovery/authentication, D-pad, text, Now Playing, seek and favorite playback with the [official iOS app](https://github.com/xbmc/Official-Kodi-Remote-iOS). |
| Hardware and network | [CEC input](../../xbmc/peripherals/devices/PeripheralCecAdapter.cpp), [DRM PRIME decode](../../xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecDRMPRIME.cpp), [GBM output](../../xbmc/windowing/gbm/WinSystemGbm.cpp), [ALSA](../../xbmc/cores/AudioEngine/Sinks/AESinkALSA.cpp), [LibreELEC settings](https://github.com/LibreELEC/service.libreelec.settings) | Keep upstream paths operational. Wi-Fi and image updates use LibreELEC components. Verify TV remote navigation before Wi-Fi setup. |

## Milestones and exit checks

1. **Baseline and trace.** With one movie and one episode from a season pack, record versions, runtime units, displayed and selected-file bytes, discovery source, chosen tracks, final file and playback result. Compare stock browsing/playback on direct HDMI and, if available, an HDR TV/receiver. Record compressed image size, installed space, boot-to-Home and memory. Use local private evidence for the existing personal setup and publish only redacted or synthetic data. On desktop, trace TV Play, favorites and iOS Play into the player and the bookmark write. Deliver a reproducible trace and first failing test for either local-first or Stop/error behavior.
2. **Prove one title end to end.** Add stable movie/episode identity, a local index and a single Play entry point. Start with a synthetic discovery adapter and move to Prowlarr + TorBox. Use exact per-file metadata for a packed episode. Demonstrate zero resolver/discovery requests on a local hit, a bounded disconnected-share timeout, expiration of a remote link and preserved progress across distinct files.
3. **Complete the selection contract.** Implement the three profiles, language selection, cancellation, scoped retry/skip, midstream recovery and resume. Run the [acceptance examples](stream-selection.md#acceptance-examples). Collect rank-only p95 and incremental RAM independently from provider and playback latency.
4. **Make the fork an appliance.** Ship fixed Home, optional folder/indexer settings, local-only mode, Wi-Fi and TorBox device authorization. Select and validate a default discovery adapter against representative releases. Run without Prowlarr or the owner's NAS on a fresh household network. Validate authenticated iOS control and TV-remote-only setup.
5. **Package and test the handoff to a friend.** Produce a pinned flash image and update. A new viewer flashes, configures only network/account, finds a movie and an episode, resumes, and tries an alternative using the TV remote. Upgrade with existing favorites, settings and bookmarks. Test recovery using a spare card. Record measured deltas and remaining issues before calling the image ready.

## Acceptance evidence to keep

| Scenario | Expected evidence |
| --- | --- |
| Local-only and duplicate matches | With WAN unavailable, indexed media plays; with WAN available and a higher-rated remote copy, it still plays locally. A failed first local copy advances to the second. Zero remote requests on a valid first local hit. |
| Cross-source state | Stop partway through a remotely streamed movie, later add a local copy and resume from a favorite. Retain position and chosen language after a reboot. Use a freshly resolved URL when a remote source becomes necessary again. |
| Failure and cancellation | A failed route can try another route. Explicit file skip excludes every route to that file. Stop, canceled language prompt, EOF and stale callbacks never launch unintended replacements. Retry count and wall time are bounded. |
| Hardware parity | Same Pi, media and HDMI chain for stock and fork. Check HDR indication on the TV, film refresh rate, audio sync, dropped frames, common H.264, selected audio/subtitles, stereo fallback and supported receiver bitstreams. Move to an SDR TV and repeat. |
| Usability and budget | TV remote reaches every action with arrows/OK/Back. Official iOS remote rediscovers the Pi and controls the same title. Record OS/app/TV versions, test conditions, repeated local/remote time to first frame, RAM, boot, size and which first-click sources viewers kept. |

**Next evidence to append:** date, commit, device/image/TV, fixture description, exact command or steps, observed result, and the next unresolved decision. No personal account or share details belong in this record.
