# Engineering handoff

**21 November 2026 · Milestone 3 is ready to start.** The owner provisionally accepted Milestone 2 for development and deferred the live Pi run. The desktop pilot passed and the Pi 5 flash image passed file-level verification. Physical boot and live-provider playback still need confirmation.

## Start Milestone 3

Use the existing `C:/Code/pi-media-appliance` checkout and its local `appliance` branch. Treat the [Milestone 2 desktop behavior](#milestone-2-desktop-evidence) as the baseline for implementation. Synthetic fixtures and desktop Kodi are enough to start. The owner copied the entire `artifacts/pi-media-m2` folder to a separate local folder for the deferred Pi test.

1. Start with deterministic candidate eligibility and ranking in C++. [AppliancePlayback.cpp](../../xbmc/video/AppliancePlayback.cpp) currently chooses one unambiguous remote file from ready items in the TorBox account. Carry forward exact movie or episode matches and distinguish selected-file bytes, estimates and unknown sizes. Keep file identity separate from resolver routes. Apply the three profiles within the eligible tier and cover the ranking examples with synthetic movie and episode tests. [Selection contract](stream-selection.md#acceptance-examples)
2. Extend the existing attempt flow in [Application.cpp](../../xbmc/application/Application.cpp) and [ApplicationPlayerCallback.cpp](../../xbmc/application/ApplicationPlayerCallback.cpp) for the rest of the contract. The pilot distinguishes Stop, EOF and failures, rejects stale callbacks by source token and retries a failed remote startup once. Reuse the stable title anchor for resume, keep local playback ahead of provider calls and exercise language selection, cancellation, skip and midstream recovery through the shared Play path. Run the focused `TestAppliancePlayback.*` tests and the loopback playback fixture from the [desktop run](#milestone-2-desktop-evidence).
3. Keep Pi acceptance open. When the owner can test, use the saved [Milestone 2 image](#milestone-2-pi-5-test-image) and checklist to confirm boot, SMB playback and fallback timing, authenticated Prowlarr discovery, TorBox packed-episode identity and exact file bytes, and resume across sources. Record the actual build, hardware and results. The rank-only target is a proposed p95 below 50 ms for 200 already-parsed candidates. Measure it and incremental RAM on the Pi separately from provider and startup timings.

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
2. **Prove one title end to end. Provisionally accepted for development.** Add stable movie/episode identity, a local index and a single Play entry point. Start with a synthetic discovery adapter and move to Prowlarr + TorBox. Use exact per-file metadata for a packed episode. Demonstrate zero resolver/discovery requests on a local hit, a bounded disconnected-share timeout, expiration of a remote link and preserved progress across distinct files. The live Pi run is deferred.
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

## Milestone 1 evidence

**20 November 2026.** Source traced at appliance revision `3b695bf1692b833125078aea84c47499ebac4b9a` (Kodi base `a3a448d26b8d560a65655dab2cd122994dc4e146`). The [official RPi5 12.2.1 archive](https://archive.libreelec.tv/archive/) reported 163,666,030 compressed bytes via an HTTP HEAD for `LibreELEC-RPi5.aarch64-12.2.1.img.gz`. The owner ran official LibreELEC 12.2.1 and Kodi 21.3 on a Pi 5 connected directly to an HDR TV. The private record holds the system readings, approximate boot time, and playback observations. The owner searched for a movie in Fen Light and timed selection to picture once for each of its TorBox and NAS sources. Both started, and the HDR badge appeared during local playback. For the episode, the owner selected a season pack, inspected its members, then replayed the chosen source past the opening advisory. The played story confirmed the conflict between the pack's episode numbering and the catalog order. Storage usage came from Kodi System info on the configured card. Fen Light's installed version and exact selected-file byte counts were unavailable in the captures.

### Desktop source trace

| Input or event | Path at the pinned revision |
| --- | --- |
| TV Play or Resume on a title | [CEC key](../../xbmc/peripherals/devices/PeripheralCecAdapter.cpp) → [peripheral key dispatch](../../xbmc/input/InputManager.cpp) → [video info Play](../../xbmc/video/dialogs/GUIDialogVideoInfo.cpp). A movie or episode reaches [VIDEO_UTILS::PlayItem](../../xbmc/video/VideoUtils.cpp), then the [playlist player](../../xbmc/PlayListPlayer.cpp). |
| Favorite | [OnSelect](../../xbmc/favourites/GUIWindowFavourites.cpp) resolves the favorite and tries the default video action. The fallback sends [GUI_MSG_EXECUTE](../../xbmc/favourites/FavouritesUtils.cpp) for the saved action. [PlayMedia](../../xbmc/interfaces/builtins/PlayerBuiltins.cpp) sends an ordinary video item to the playlist player. |
| iOS library Play | [Player.Open](../../xbmc/interfaces/json-rpc/PlayerOperations.cpp) fills the requested movie or episode through the [video library](../../xbmc/interfaces/json-rpc/VideoLibrary.cpp), applies any resume option, and posts TMSG_MEDIA_PLAY. [CPlayListPlayer::OnApplicationMessage](../../xbmc/PlayListPlayer.cpp) handles a single video item through Play. |
| Open and bookmark | The playlist player calls [CApplication::PlayFile](../../xbmc/application/Application.cpp), which resolves the item, applies resume options, and uses [CApplicationPlayer](../../xbmc/application/ApplicationPlayer.cpp) to open [VideoPlayer](../../xbmc/cores/VideoPlayer/VideoPlayer.cpp). On close, VideoPlayer sends the position to [OnPlayerCloseFile](../../xbmc/application/ApplicationPlayerCallback.cpp). [CSaveFileState::DoWork](../../xbmc/utils/SaveFileStateJob.cpp) writes or clears the resume bookmark through [CVideoDatabase](../../xbmc/video/VideoDatabase.cpp). The tracking path defaults to the opened item path with specific exceptions for video database items, removable media, plugins, UPnP and Blu-ray. |
| Stop, error and EOF | An explicit [ACTION_STOP](../../xbmc/application/Application.cpp) or [Player.Stop](../../xbmc/interfaces/json-rpc/PlayerOperations.cpp) closes the player. [VideoPlayer::OnExit](../../xbmc/cores/VideoPlayer/VideoPlayer.cpp) checks close request, then error, then normal completion. [OnPlayBackError](../../xbmc/application/ApplicationPlayerCallback.cpp) posts both error and stopped. The app announces stopped as end=false and normal completion as end=true. A retry decision needs the cause and identity of the active attempt. |

### First failing check

The [Stop signal check](test_stop_signal.py) exercises the existing desktop JSON-RPC path. It opens a generated video, waits for playback, sends Player.Stop, then opens a corrupt file. The assertion asks whether the legacy stop flag alone would let an appliance distinguish those two outcomes. Run it against an isolated desktop Kodi 21.3 profile with local application control enabled and the following advanced setting in that profile's userdata:

```xml
<advancedsettings>
  <jsonrpc><tcpport>19090</tcpport></jsonrpc>
  <loglevel>1</loglevel>
</advancedsettings>
```

For the Windows run, Kodi was started with `kodi.exe -p` in a separate unpacked copy of the [official 21.3 x64 release](https://mirrors.kodi.tv/releases/windows/win64/). From the repo, with FFmpeg and Python on PATH:

```powershell
$fixture = Join-Path $env:TEMP 'pi-media-appliance-m1-kodi-21.3'
New-Item -ItemType Directory -Path $fixture -Force | Out-Null
ffmpeg -hide_banner -loglevel error -y -f lavfi -i color=size=160x90:rate=10 -t 30 -c:v mpeg4 (Join-Path $fixture 'good.mkv')
'synthetic corrupt file' | Set-Content (Join-Path $fixture 'broken.mkv')
py -3 docs/appliance/test_stop_signal.py (Join-Path $fixture 'good.mkv') (Join-Path $fixture 'broken.mkv')
```

Result on Windows x64 with Kodi 21.3-Omega was one expected assertion failure, `False == False`. The player log independently confirmed a demux open error on `broken.mkv` followed by the stopped callback. This measures desktop signal behavior. The next engineering question is where to carry an attempt identifier and termination cause while preserving existing Kodi callbacks.

## Milestone 2 pilot configuration and checks

The pilot uses normal Kodi library items. In a disposable profile, create one empty anchor file per title under `special://profile/appliance/library/`. Use `movie-tmdb-<movie-id>.mkv` for a movie and `episode-tmdb-<episode-id>.s01e02.mkv` for an episode, substituting its actual season and episode numbers. Add matching local NFO metadata and scan the movie and show folders through the existing Kodi library to exercise favorites, Continue Watching and the iOS remote. Keep the existing anchor path when adding or replacing media so the Kodi library record and bookmark stay attached to that title.

Create `special://profile/appliance/index.json` in the test profile. The local array is the pilot index in playback order. It accepts absolute file paths and SMB URLs. The movie's remote `title` is the expected filename title, including the year if present. The episode `query` is the show's filename title and its `title` is the catalog episode title. Use lowercase `s01e02` notation for the episode field.

```json
{
  "movie:tmdb:900000001": {
    "local": ["/test/local-movie.mkv"],
    "title": "Pilot Movie (2026)",
    "query": "Pilot Movie 2026"
  },
  "episode:tmdb:900000002": {
    "local": ["smb://example.invalid/share/episode.mkv"],
    "title": "Return",
    "episode": "s01e02",
    "query": "Pilot Show"
  }
}
```

For the live provider pilot, put `prowlarr`, `prowlarrKey` and `torboxKey` in `special://profile/appliance/providers.json` on the Pi. Use the base Prowlarr URL without a trailing slash. Use a release that Prowlarr returns with a torrent hash and that is already available in the test TorBox account. The pilot checks the hash and selects one unambiguous video file using its filename and exact byte count. Its provider calls are read-only. It resolves a fresh link at playback and once more after a startup failure. [Prowlarr API](https://prowlarr.com/docs/api/) · [TorBox API](https://api.torbox.app/docs)

For the desktop fixture, enable local application control in a fresh profile and use these advanced settings. The shortened bookmark threshold is for the 40-second fixtures. The Samba timeout is a suggested pilot setting, pending measurement on the Pi.

```xml
<advancedsettings>
  <jsonrpc><tcpport>19090</tcpport></jsonrpc>
  <video><ignoresecondsatstart>1</ignoresecondsatstart></video>
  <samba><clienttimeout>5</clienttimeout></samba>
  <loglevel>1</loglevel>
</advancedsettings>
```

Run the focused Kodi unit tests with `kodi-test --gtest_filter='TestAppliancePlayback.*'`. On Linux with SMB enabled, start the fork with the advanced settings above and run `python3 docs/appliance/test_appliance_playback.py /path/to/disposable/userdata` with FFmpeg on `PATH`. The script creates its own title index and loopback provider. It expects zero provider requests for local play, measures fallback from a silent SMB peer, selects the confirmed file in a pack with conflicting entries, replaces a link that returns HTTP 403 and checks resume in both directions between distinct files. Use a fresh `userdata/appliance` directory for each run.

For the Pi run, verify the local movie and episode first. Then disconnect only the SMB share so Prowlarr stays reachable. Record Play-to-fallback time, file size in bytes from the provider, and resume position before and after switching sources. Record the tested revision, OS and TV in this handoff using synthetic or redacted evidence.

**Network preflight, 20 November 2026.** Prowlarr `GET /ping` returned HTTP 200 from the Windows workstation. In the Linux build container, `smbclient -U % -N //<nas>/<share> -c ls` succeeded with guest access. These checks exercised endpoint reachability and directory listing. The keyed Prowlarr search and TorBox file resolution still need the Pi run.

### Milestone 2 desktop evidence

**20 November 2026.** Tested the uncommitted pilot changes based on `3b695bf1692b833125078aea84c47499ebac4b9a` in Ubuntu 24.04 x86-64 with Xvfb. The Windows-exported build copy used LF for the JSON-RPC schema version input. The build used Ubuntu's FFmpeg 6.1.1 development packages.

```sh
cmake -S /work -B /appliance-build -G Ninja -DGIT_VERSION=3b695bf-m2-worktree \
  -DCORE_PLATFORM_NAME=x11 -DAPP_RENDER_SYSTEM=gl -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS_RELEASE='-O0 -DNDEBUG' -DENABLE_LTO=OFF \
  -DENABLE_INTERNAL_FFMPEG=OFF -DENABLE_TESTING=ON
cmake --build /appliance-build --target kodi-test kodi-x11 -j 8
/appliance-build/kodi-test --gtest_filter='TestAppliancePlayback.*'
python3 -B docs/appliance/test_appliance_playback.py /path/to/disposable/userdata
```

All three focused unit tests passed. In the fixture, ordinary playback worked, a corrupt local movie copy advanced to the next local copy, and the playable local movie and episode generated zero provider requests. With a five-second Samba client timeout, a loopback peer that accepted TCP and sent no SMB reply reached discovery in 10.047 seconds. The directory probe and file open each exhausted a timeout. A refused connection in an earlier run reached discovery in 0.138 seconds. Kodi excluded the wrong episode, recorded the confirmed file's 42,500 bytes, refreshed its HTTP 403 link, and preserved the resume point across the local and remote copies in both directions. Explicit Stop generated no provider traffic, and a late discovery response after a title switch generated no resolver request. The next open question is how quickly the Pi falls back when the real share stops answering and whether the provider returns enough evidence to confirm the real pack member.

### Milestone 2 Pi 5 test image

The pilot image build uses LibreELEC `6209f5a3b067c45fbf527286dd84ee483294b46a` and Kodi source tree `16071005dfd600fe13b1059b2f12263cc7e7daf9`, based on appliance revision `3b695bf1692b833125078aea84c47499ebac4b9a`. The source archive `kodi-21.3-Omega.tar.gz` has SHA-256 `218991113d16b927dc156d37087d1eabfc682575cc7faf740dc0eb43afe66c24`. LibreELEC verified that archive and applied the pinned Pi patches successfully. The Kodi archive and `libreelec-m2-source.patch` are in the local build artifact directory. The recipe patch points at a loopback HTTP server serving that archive.

In the Ubuntu 22.04 container, run as the unprivileged builder from the LibreELEC checkout after applying the recipe patch and starting `python3 -m http.server 19751 --bind 127.0.0.1 --directory /home/builder/kodi-source`:

```sh
export PROJECT=RPi DEVICE=RPi5 ARCH=aarch64
export CUSTOM_VERSION=12.2.1-m2.1 BUILDER_NAME=pi-media-appliance CONCURRENCY_MAKE_LEVEL=7
./scripts/unpack kodi
make image
```

The build ran in the local Docker container `pi-media-pi5-build`, with the checkout at `/home/builder/LibreELEC`, build log `/tmp/pi5-build.log` and image output under the checkout's `target/` directory.

The first run stopped at `v4l-utils:target` after 271 package steps. The upstream download returned HTML and the default mirror returned HTTP 404. The [BLFS source mirror](https://mirror.freedif.org/pub/blfs/conglomeration/v4l-utils/) supplied `v4l-utils-1.30.1.tar.xz`, which matched LibreELEC's pinned SHA-256 `c1cf549c2ec3cf39eb5ec7bf15731349e61b26a21b5e963922db422333bae197`. The verified file was placed in the local source cache and the same `make image` command resumed the build. The resumed log is `/tmp/pi5-build-resume.log`.

The resumed build completed all 286 package steps and produced `LibreELEC-RPi5.aarch64-12.2-12.2.1-m2.1.img.gz` (164,435,643 bytes). Its SHA-256 is `cfea88ba49b023c1c604035c041691e307f029d03c67d4ae9f7351f0cdf32873`. `gzip -t` and `sha256sum -c` passed. The image contains two partitions, the Pi 5 device tree, and an AArch64 Kodi executable with the appliance playback code. The flash image, checksum, source snapshot and `pi5-test-checklist.md` were exported to `C:/Code/artifacts/pi-media-m2`. The owner saved a separate copy on 21 November 2026 for the deferred live checks.
