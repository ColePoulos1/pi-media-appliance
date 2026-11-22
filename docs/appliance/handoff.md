# Engineering handoff

**21 November 2026 · Milestone 3 desktop acceptance passed.** The selector, language, recovery and scanned-library checks passed. The [point 5 evidence](#point-5-desktop-acceptance) records the desktop runs. The [M2 and M3 hardware records](#point-5-pi-5-records) track separate physical runs, including boot, provider playback, TV remote, iOS and Pi performance.

## Start Milestone 4

Follow the [Milestone 4 implementation plan](milestone-4.md) from appliance checkpoint `bbc6136a7f25e96d44dd65e8d67bc01282e247e5`. Start with a fresh profile and prove that one indexed local title reaches the existing player from the fixed Home screen. Evaluate release discovery against the owner's Fen Light+ movie and episode coverage alongside that slice. Evaluate Torrentio, BitSearch, MediaFusion and Comet first. The default must work with Prowlarr unconfigured and the owner’s home server offline. The owner chose to include a project TMDB credential in the non-commercial image for trusted friends. The plan identifies the desktop checks, the clean-account test and the physical Pi run needed to accept the milestone.

**21 November 2026 · Point 1 passed on desktop.** The fixed Home screen can import and play a local movie on a fresh profile. Point 2 has a documented shortlist and workstation preflight. The owner supplied a TMDB API key for local development. Default-provider selection and verification of the TMDB distribution model remain open. The saved M3 images and outstanding physical checks remain the hardware baselines.

### M4 point 1 desktop evidence

Tested product and fixture revision `65d8d6df31e352dcb80a33002ab67864ac17b26f` (tree `0c2fc1d8323ceee430327bc6193cb706d9eae894`) on the [M3 desktop build environment](#point-5-desktop-acceptance). The M4 fixture starts with empty appliance userdata and generates one 40-second movie and its sidecar NFO in a disposable Linux `HOME`. Use the profile's [JSON-RPC and bookmark settings](#milestone-2-pilot-configuration-and-checks). Start `/appliance-build/kodi-x11 --windowing=x11` with `KODI_HOME` and `KODI_BIN_HOME` pointing at `/appliance-build` and `KODI_DATA` pointing at each disposable profile's parent of `userdata`. The GUI runs used software OpenGL and `xvfb-run -a -s '-screen 0 1280x720x24'`. The seeded library run used `AE_SINK=ALSA` with `pcm.!default { type null }` in its disposable `$HOME/.asoundrc`.

```sh
cmake --build /appliance-build --target kodi-test kodi-x11 -j 7
(cd /appliance-build && ./kodi-test --gtest_filter='TestAppliancePlayback.*:TestFile.Read')
python3 -B -u /src/docs/appliance/test_appliance_playback.py "$home_userdata" --port 19117 --home-phase first
# Restart Kodi with the same HOME and KODI_DATA.
python3 -B -u /src/docs/appliance/test_appliance_playback.py "$home_userdata" --port 19117 --home-phase restart
# Use separate disposable profiles for the existing checks.
python3 -B -u /src/docs/appliance/test_appliance_playback.py "$baseline_userdata" --port 19107 --measure-frames
python3 -B -u /src/docs/appliance/test_appliance_playback.py "$library_userdata" --port 19106 --library-phase first
# Restart Kodi with the same library profile.
python3 -B -u /src/docs/appliance/test_appliance_playback.py "$library_userdata" --port 19106 --library-phase restart
```

All 15 focused unit tests passed. The new fixture used arrow, OK and Back actions through local-only setup, the empty Continue Watching screen, Settings, the folder browser, the on-screen Search keyboard and the title detail. Kodi created the per-title index row, anchor and video library record. Back from detail restored the result focus. After restart, adding the same folder preserved the library ID, local association and bookmark. Continue Watching resumed within two seconds of the saved position. The loopback provider received zero requests on Play, Resume, missing-file exhaustion and Retry. The ordinary file then played through Kodi's normal entry point.

The legacy index and seeded library scenarios passed. The baseline exercised local-first, silent SMB fallback, exact pack member selection, link refresh, alternate routes, cooldown, resume and cancellation. The seeded movie and episode retained their source, language and title bookmarks across restart via library ID, detail, Favorites and Continue Watching. The new folder importer handles top-level local movies with adjacent NFOs containing explicit TMDB IDs. Full folder management and indexing are in [point 4](milestone-4.md#4-complete-catalog-search-folder-indexing-and-settings). Pi 5 boot, physical CEC and hardware playback remain unmeasured for this slice.

### M4 point 2 dependency status

The [provider evaluation](m4-provider-evaluation.md) records the four candidates, public endpoint checks and operator and credential constraints. BitSearch is the first candidate for matched coverage testing because its direct API offers keyless access. The next open question is how many of the owner's Fen-playable movies and episodes an eligible candidate can play and whether its operator permits the shipped use. Compare both setups on the agreed private sample using the same TorBox account, with the owner's server unavailable to the appliance and Prowlarr unconfigured on it.

The fork stays public. The owner supplied a TMDB API key from a dedicated disposable account with no other services linked. The workstation stores it as `TMDB_API_KEY` in the Git-ignored root `.env`. Confirmation that the registration covers the intended distribution, confirmation of the bundled read-token model, and fresh-image catalog lookup are pending. The evaluation records the applicable attribution and cache terms. M4 points 3 through 7 remain open.

## Milestone 3 baseline

Use the existing `C:/Code/pi-media-appliance` checkout and its local `appliance` branch. Treat the [Milestone 2 desktop behavior](#milestone-2-desktop-evidence) as the baseline for implementation. Synthetic fixtures and desktop Kodi are enough to start. The owner copied the entire `artifacts/pi-media-m2` folder to a separate local folder for the deferred Pi test.

### Milestone 3 implementation plan

Start at the local Milestone 2 checkpoint `e86851e938cc989a7765df8810f4d34f012cf3ef`. Work through the steps in order, keeping each check green before wiring the next slice. Implement product behavior in native C++. Extend the existing GTests and Python loopback driver for evidence. The [selection contract](stream-selection.md) governs behavior. Household setup, the default discovery provider and the fixed Home screen belong to Milestone 4.

#### 1. Represent and rank a file before connecting it to playback

- Start in [AppliancePlayback.h](../../xbmc/video/AppliancePlayback.h), [AppliancePlayback.cpp](../../xbmc/video/AppliancePlayback.cpp) and [TestAppliancePlayback.cpp](../../xbmc/video/test/TestAppliancePlayback.cpp). First add a failing ordering test for the 120-minute movie below. Pass already-parsed candidates, the title context, viewer preferences and device capabilities into a deterministic C++ selector that performs no I/O.
- Give each candidate a tier and a stable file identity. Use a local library ID or canonical path for downloaded media. For torrents use the normalized info hash and member path, or a member ID known to be stable within that torrent. Keep account torrent IDs, provider-specific file IDs and resolver alternatives on routes. Merge two reports of the same member, keeping distinct routes and evidence. Two members of the same pack must remain distinguishable.
- Normalize runtime to minutes once at ingestion (Kodi video tag duration is in seconds). Keep size provenance as exact member bytes, labeled estimate, pack total or unknown. Only exact or credible estimated member bytes contribute to decimal MB/min, with reduced weight for estimates. Carry resolution, codec, source, HDR/3D, availability and track evidence when supplied. Preserve unknowns and the distinction between filename hints and verified tracks. Keep the M2 proof of movie/episode identity and rejection of conflicting or ambiguous pack members. An unknown size cannot establish identity or disqualify an otherwise confirmed member by itself.
- Apply exclusions before scoring. Check media match, known unavailability, 3D default, device/display support and viewer overrides, confirmed language mismatches and attempt skips. Apply streaming caps only to remotes and honor explicit unknown-size policy. Order eligible locals by preferences, retaining pilot index order for equal legacy entries. Within the remote tier use bounded MB/min bands for comparable format/content, with a plateau for Balanced, a stronger undersize penalty for High Quality, and a credible floor plus excess-size penalty for Data Saver. Treat low-motion animation and grain/restoration as context. Use Blu-ray and supported HDR as preferences, then saved sort preferences and stable identity for ties. Keep initial numeric bands in one place and label them provisional.

| Synthetic check | Required observation |
| --- | --- |
| 120 min, 1.2 GB Blu-ray vs 7.2 GB WEB-DL | Balanced and High Quality pick the 60 MB/min WEB-DL over the 10 MB/min Blu-ray. At comparable sizes Blu-ray wins. |
| 22 min low-motion animation, 0.55 vs 2.2 GB | Data Saver picks 0.55 GB. Assert the bounded Balanced size contribution above its plateau. Include a credible larger encode that High Quality favors. |
| 45 min episode, 1.8 GB member, 40 GB pack | Exact member scores at 40 MB/min. With only the total, the member has unknown size. Cover zero runtime, malformed bytes, estimates and an explicit cap. |
| Mixed eligibility and duplicate reports | Unsupported formats and wrong episodes stay excluded under all profiles. Shuffle remote input and obtain the same file order. A local copy over the streaming cap still wins. |

**Point 1 evidence, 21 November 2026.** Tested uncommitted selector changes on `e86851e938cc989a7765df8810f4d34f012cf3ef` in the existing Ubuntu 24.04.5 x86-64 build container. The build reused the [M2 configuration](#milestone-2-desktop-evidence) with X11, OpenGL, system FFmpeg, and Release flags `-O0 -DNDEBUG`. The 120-minute movie test first failed against the empty selector stub. The finished selector passed all nine focused tests, including the three M2 tests. The four synthetic ranking cases above are covered by `PrefersCredibleMovieEncodeInBalancedAndHighQuality`, `RanksAnimationByProfileWithBoundedBalancedSizeBenefit`, `ScoresEpisodeMemberBytesAndAppliesRemoteCaps`, and `KeepsLocalOrderAndMergesOnlyTheSameTorrentMember`.

```sh
cmake --build /appliance-build --target kodi-test kodi-x11 -j 4
/appliance-build/kodi-test --gtest_filter='TestAppliancePlayback.*'
# Launch kodi-x11 under Xvfb with the disposable M2 profile settings.
python3 -B -u /src/docs/appliance/test_appliance_playback.py "$profile/userdata"
```

The existing loopback playback fixture passed. Playable local sources generated zero provider requests, silent SMB reached discovery in 10.070 seconds, the confirmed 42,500-byte episode recovered from an expired link, and resume survived both source changes. Explicit Stop and a stale discovery response generated no replacement. These are desktop results. The ranking bands are provisional and Pi latency and RAM remain unmeasured.

The selector takes confirmed title identity independently of size. The M2 adapter still uses its existing single-result behavior. The next question is whether the pilot can supply all eligible candidates and keep provider access deferred until local exhaustion.

#### 2. Feed the selector from the existing pilot

Extend `LoadIndexEntry` and `LocalFiles` to accept optional metadata while retaining the documented M2 string paths and anchors. Remove the four-local-file truncation. Make `Discover` enumerate the matching ready torrents in its bounded listing instead of returning at the first success. Preserve `SelectFile`'s ambiguity check *within each torrent*. Provide the ranker with metadata from the matched release and selected member, keeping pack-level evidence labeled. Keep Prowlarr/TorBox read-only, the existing input validation, request bounds and private-data logging protections. Resolve one selected route just before open.

Wire the two tiers into `CApplication::TryApplianceSource`. Start local playback without loading provider settings, discovering or resolving. Discover once on local exhaustion and reuse the ranked queue for retries. Keep non-anchor files on `PlayFileInternal` and keep the title path on the opened item while its dynamic path identifies the current source. Extend the loopback provider to model several torrents, two routes to one member, and identical numeric file IDs in different torrents. Check a playable fifth local after four failures produces zero provider calls, a missing local falls through, and a losing remote is never resolved speculatively.

The index still accepts an ordered array of path strings. An entry can also be an object such as `{"path":"/test/movie.mkv","bytes":7200000000,"format":"1080p WEB-DL H264"}`. Only `path` is required. `bytes` is the exact local file size, `format` supplies optional release-style tags, and an optional boolean `is3D` overrides the format tag. A title can supply `runtimeSeconds` when Kodi has no tagged duration, plus optional `lowMotionAnimation` and `grainOrRestoration` booleans. The pilot currently uses Balanced and leaves device capabilities unspecified. TorBox member sizes take precedence over Prowlarr torrent totals. A confirmed member can enter the queue with unknown size. Each resolved URL is requested just before its open, with one refresh after a startup failure before advancing to another route.

**Point 2 evidence, 21 November 2026.** Tested the uncommitted points 1 and 2 changes based on `e86851e938cc989a7765df8810f4d34f012cf3ef` in Ubuntu 24.04.5 x86-64, reusing the [point 1 build configuration](#1-represent-and-rank-a-file-before-connecting-it-to-playback). With a fresh disposable profile under Xvfb, the commands were:

```sh
cmake --build /appliance-build --target kodi-test kodi-x11 -j 4
/appliance-build/kodi-test --gtest_filter='TestAppliancePlayback.*'
# Launch kodi-x11 with the disposable M2 profile settings, using TCP port 19091.
python3 -B -u /src/docs/appliance/test_appliance_playback.py "$profile/userdata" --port 19091
```

All nine focused tests passed. The fixture played the fifth local after four corrupt copies with zero provider requests. A silent SMB peer reached discovery after 10.063 seconds. The correctly matched 42,500-byte packed episode ranked ahead of the other file and resumed after a fresh link. The lower-ranked file received no resolution request until both routes to the higher-ranked file failed in a separate attempt. The alternate route and next-file attempts each used one search and one ready-torrent listing. The fixture also verified a missing local, ambiguity rejection, ordinary video playback, Stop, reverse resume, and suppression of resolution after a stale discovery response. The next question is whether Kodi's actual streams satisfy the viewer's saved language choice across alternatives. Live provider and Pi checks remain open.

#### 3. Apply the viewer's language choice to real streams

Use Kodi's profile and existing audio/subtitle language settings. Expose Balanced (default), High Quality and Data Saver, and separate Ask (default), Prefer Sub and Prefer Dub settings for Movies, TV and Anime through Kodi's existing settings UI. For the pilot, extend title metadata with original language, anime classification and a provider-qualified series ID where the library lacks them. A series ID lets episodes share a choice while their bookmarks stay separate. Document optional index fields and give defaults for old rows. [Kodi settings definitions](../../system/settings/settings.xml)

For a known mismatch, consult the saved movie/series choice, then the category setting. Anime takes precedence even for a film. In Ask mode show the two concrete language combinations using Kodi's select dialog before opening sources. Back ends the request with zero opens/provider calls. A matching or unknown original language uses ordinary track preferences. Persist a confirmed choice per profile and movie/series, provide a change/clear action on the title, and keep retries from re-prompting. Use profile-scoped storage for appliance state, preserve other entries on update and keep the existing Kodi database as the owner of title bookmarks.

Prefer verified matches during ranking. At token-matched AV start inspect the actual streams through `CApplicationPlayer`. Apply original audio plus full preferred-language subtitles (default-language fallback) for Sub, or default-language audio plus ordinary subtitle settings for Dub. Normalize language codes using Kodi's existing language utilities. Verify what was selected before marking the file successful. A forced-only stream cannot satisfy full subtitles. Advance on confirmed mismatch. On exhaustion offer a known available language alternative once, with a way to decline. Test actual selected streams with a synthetic dual-audio file and full/forced subtitle tracks, a misleading filename, a canceled prompt, two episodes of one series, restart, change/clear and a second profile. [Player stream methods](../../xbmc/application/ApplicationPlayer.h) · [Stream flags](../../xbmc/cores/VideoPlayer/Interface/StreamInfo.h) · [Kodi select dialog](../../xbmc/dialogs/GUIDialogSelect.h)

**Pilot configuration.** Open **Settings → Player → Language → Appliance playback** to set source quality and the independent Movies, TV and Anime preferences. The surrounding Kodi audio and subtitle settings supply the languages. The title context menu's **Playback language** action changes or clears a saved movie or series choice. An exhaustion alternative applies to the current play request.

In `index.json`, a title can add `"originalLanguage":"ja"` and `"anime":true`. Old rows use ordinary Kodi tracks when the original language is unknown. Episodes can supply a shared provider-qualified `"seriesId":"tmdb:900000025"`. Without it, an indexed Kodi show with a TMDB ID supplies the key. Without either show ID, the choice is saved for that episode. Movie and series choices live under `special://profile/appliance/language/`. Kodi bookmarks still use each title's anchor. A local file object can provide complete, verified `audioLanguages` and `fullSubtitleLanguages` arrays. Include only full dialogue tracks in the subtitle array. Omit an array when its contents are unknown. Kodi checks the actual streams before accepting an opened file. The provisional stream-selection confirmation window is two seconds after applying the tracks.

**Point 3 evidence, 21 November 2026.** Tested revision `dd2ad40135e33ce79db602948da5d0b5e2edebd7` on Ubuntu 24.04.5 x86-64. The build reused X11/OpenGL, system FFmpeg, testing enabled, LTO off and Release flags `-O0 -DNDEBUG`. Kodi ran under Xvfb with an ALSA null default PCM. The ALSA config loaded `/usr/share/alsa/alsa.conf` and set `pcm.!default { type null }`. The disposable profile used TCP port 19103 and the [M2 bookmark and Samba settings](#milestone-2-pilot-configuration-and-checks).

```sh
cmake --build /appliance-build --target kodi-test kodi-x11 -j 4
/appliance-build/kodi-test --gtest_filter='TestAppliancePlayback.*'
python3 -B -u /src/docs/appliance/test_appliance_playback.py "$primary" --port 19103 --language-phase first
# Restart Kodi using the same userdata before checking the saved choice.
python3 -B -u /src/docs/appliance/test_appliance_playback.py "$primary" --port 19103 --language-phase restart
# Start Kodi on a fresh profile named "Language fixture second profile" in the same installation.
python3 -B -u /src/docs/appliance/test_appliance_playback.py "$secondary" --port 19103 --language-phase second-profile
# Start Kodi with fresh disposable userdata for the original loopback fixture.
python3 -B -u /src/docs/appliance/test_appliance_playback.py "$baseline" --port 19103
```

All 10 focused tests and all fixture phases passed. Back canceled the Ask dialog with no provider connection or active player. Sub skipped a misleading source and selected Japanese audio with full English fallback subtitles when Spanish was available only as a forced track. The next episode reused the series choice, including after restart. The named second profile prompted for the same series. The title menu changed and cleared the series choice while preserving the saved movie choice. An anime film used its Anime default, Dub retained Kodi's ordinary subtitle behavior, and the exhaustion dialog allowed both declining and playing the offered Dub alternative.

The original loopback fixture passed ordinary playback, local-first fallback with zero provider requests, alternate routes, expired-link refresh, cross-source resume and stale-response suppression. Silent SMB reached discovery in 10.049 seconds. These are desktop results. Point 4's next open question is how to distinguish a hard midstream failure from clean EOF and carry the accepted position into a replacement. Pi playback, track timing and performance remain open.

#### 4. Drive replacement and resume from the cause of the event

Extend the existing attempt in [Application.cpp](../../xbmc/application/Application.cpp). Keep the queue, rejected file identities, tried routes, retry count and deadline on the active request. Let the application thread own transitions. Workers return discovery/resolve results tagged with the source token. Check the token before consuming results, accepting AV start, advancing or opening a dialog. Invalidate the old open before starting a replacement. Handle synchronous open failure and the later termination of that open only once. Reset per-open AV/track state on each new token.

| Event | Action and check |
| --- | --- |
| Resolution or startup failure | Preserve the pilot's single fresh-link attempt for a possibly expired URL. Cool down a failing route or provider at the narrowest known scope, try an eligible alternate route, then move to the next file. Explicit file skip bypasses **all** its routes. |
| Failure after AV start | Capture the last valid position and resume the next eligible file there, even when the original Play began at zero. First reproduce a hard read failure after frames have played alongside a clean EOF control. Trace the input/demux error through VideoPlayer: in M2 the main loop can drain and exit, and `m_error` is set on startup failures. Carry a confirmed terminal read error to the appliance callback if it is lost. Preserve transient-read handling and normal Kodi EOF behavior. |
| **Try another source** | Add an appliance-only action to the existing video OSD, reachable with arrows/OK/Back. Record progress and file skip before closing. Consume the resulting stopped callback as part of this replacement. Exercise local-to-local, local-to-remote and duplicate remote routes. |
| Explicit Stop, Back from prompt, EOF, title switch | Capture last working file and explicit Stop time before `StopPlaying` clears the attempt. Only an explicit viewer Stop arms quick replay. Cancel pending lookups and prevent late completions from advancing. EOF retains Kodi completion behavior. Test Stop during lookup, Stop during playback, error followed by stopped, rapid clicks and a late result after switching to an ordinary file. |
| Reopen | For the same viewer/title inside the provisional two-minute Stop window, skip the stopped file across routes and retain choice and progress. Outside the window, prefer the last working eligible file **within its tier**. Recheck current filters. Persist stable last-working identity per viewer/title across restart. Keep session skips transient. Test both sides of the window with a controlled clock and show another title/viewer is unaffected. |
| Exhaustion | End automatic work, show a concise reason and offer intentional Retry or manual selection of an eligible file. Back ends the request. A fresh search, if used, consumes the same budget and preserves exclusions. Count attempts and elapsed time, including in-flight provider timeouts. Use a monotonic retry deadline starting at failure/recovery so time spent successfully watching cannot exhaust it. Name provisional limits in code, allow the baseline silent-SMB case, and test count, deadline and cooldown boundaries without real sleeps. If a limit stops local retries, surface exhaustion before launching remote work. |

Trace `CVideoPlayer::OnExit` through `OnPlayerCloseFile`, `OnPlaybackTerminated` and the resume read in `PlayFileInternal`. Close queues a bookmark before termination. Ensure a stale close or a rejected startup at position zero cannot erase newer progress. Carry the accepted position into a replacement when the title's original start offset or cached video tag is stale. Preserve normal Stop/EOF bookmark and play-count semantics and the untagged error-plus-stopped callbacks. Test a remote break after seeking, resume on a distinct file within the fixture's keyframe tolerance, then reopen the title after restart. Store only title bookmarks and stable file identities in persistent state. [VideoPlayer](../../xbmc/cores/VideoPlayer/VideoPlayer.cpp) · [FFmpeg demuxer](../../xbmc/cores/VideoPlayer/DVDDemuxers/DVDDemuxFFmpeg.cpp) · [Player callbacks](../../xbmc/application/ApplicationPlayerCallback.cpp) · [Bookmark writer](../../xbmc/utils/SaveFileStateJob.cpp)

**Pilot recovery behavior.** Estuary's video OSD offers **Try another source** after an appliance file starts. The action preserves the title bookmark and skips that file across its routes. Explicit Stop arms a two-minute replay window in the running process. A later resume prefers a last working eligible file within its tier. The profile's `appliance/working/` directory stores only a fingerprint of that file's stable identity. Kodi's video database stores the position on the title anchor.

The provisional budget is 16 source attempts, counting each local open, discovery and remote resolution with its associated open. The 30-second recovery window starts at failure or an explicit source change. An accepted stream clears the deadline during normal viewing. A failed route cools down for five seconds in that profile. The exhaustion dialog offers **Retry** and any eligible manual choices still available from the current queue.

**Point 4 evidence, 21 November 2026.** Tested revision `4ed331087278b402b7aad96da7d20b6d62b63bc5` on Ubuntu 24.04.5 x86-64 with the point 3 build flags, Xvfb and an ALSA null PCM. The pre-change build at `4c0bd5afa214444a8411687e2c48ff57996c1897` reported `Player.OnStop` with `end: true` after a loopback HTTP response broke following a seek to 15 seconds. The cache had lost the reported length during the broken transfer. The appliance path now brings terminal input errors through VideoPlayer's drain and writes the accepted title bookmark before replacing the source.

```sh
cmake --build /appliance-build --target kodi-test kodi-x11 -j 7
# Use LF in the Linux build copy of the Windows-exported upstream read fixture.
sed 's/\r$//' /work/xbmc/filesystem/test/reffile.txt > /appliance-build/xbmc/filesystem/test/reffile.txt
cd /appliance-build
./kodi-test --gtest_filter='TestAppliancePlayback.*:TestFile.Read'
# Launch Kodi under Xvfb with the M2 bookmark and Samba settings and TCP port 19105.
python3 -B -u /src/docs/appliance/test_appliance_playback.py "$recovery" --port 19105 --recovery-phase first
# Restart Kodi with the same userdata.
python3 -B -u /src/docs/appliance/test_appliance_playback.py "$recovery" --port 19105 --recovery-phase restart
# Use the same language phases documented in point 3, with TCP port 19104.
python3 -B -u /src/docs/appliance/test_appliance_playback.py "$baseline" --port 19104
```

All 14 focused tests passed, including the attempt and monotonic time boundaries, the exact Stop-window and cooldown edges, profile/title scoping, and preference after eligibility. In the synthetic playback run, the interrupted title resumed on a distinct file at 19 seconds, within 1.5 seconds of the outgoing bookmark. Kodi restarted and reopened that same last working file at 19 seconds. Both the ordinary and appliance clean EOF controls reported `end: true`. Arrow-key navigation reached the OSD action. Local-to-local, local-to-remote and remote-to-remote replacements retained progress without an intervening stopped event. The remote skip bypassed both routes to the same member. Quick Play advanced after explicit Stop. The local attempt cap stopped before discovery, manual selection reached a later local file, Back dismissed exhaustion, and intentional Retry opened a repaired local source.

The language phases also verified that skipping the only dual-audio file removed it as a language alternative. The baseline observed local-first playback with no provider requests on a playable local copy, silent SMB fallback to discovery in 10.062 seconds, expired-link refresh, alternate routes and cooldown, and title resume in both directions. A duplicate Play started no second lookup. Stop during discovery and switching to an ordinary file prevented further provider requests. The next open question is how these transitions and the selector budget behave on the release Pi build and across the library entry points in point 5.

#### 5. Run the acceptance checks and record the evidence

1. Use the [M2 desktop build and disposable profile settings](#milestone-2-pilot-configuration-and-checks). Build `cmake --build /appliance-build --target kodi-test kodi-x11 -j 8`, run `/appliance-build/kodi-test --gtest_filter='TestAppliancePlayback.*'`, launch that fork, then run `python3 -B docs/appliance/test_appliance_playback.py /path/to/disposable/userdata` with a fresh appliance directory. Keep ranking, clock boundaries and duplicate-event assertions in GTest. Use the loopback run for provider counts, actual player/track events and bookmark positions. Keep the M2 checks green and map every [acceptance example](stream-selection.md#acceptance-examples) to a named check.
2. Scan synthetic anchors with NFOs into the disposable library. Open the same movie and episode from TV detail Play/Resume, Favorites, Continue Watching and JSON-RPC `Player.Open` by library ID. Verify the same file choice, prompt behavior and title bookmark, including after switching sources and restarting Kodi. The existing fixture opens anchors by path. Record library-ID and GUI runs separately. Check ordinary file playback, Stop and EOF as controls. Verify physical TV input and the official iOS app when the Pi is available.
3. Time only filtering, deduplication and ordering of 200 already-parsed candidates. Prepare deterministic inputs outside the timed region, warm up, then record at least 100 iterations and report the nearest-rank p95, count, build flags and fixture mix. In an isolated run measure peak resident memory above the same warmed process with inputs loaded, state the method/resolution, and report an unresolved delta if sampling cannot distinguish it. Record discovery, per-route resolution, silent-share fallback and Play-to-first-frame separately. The proposed Pi rank-only target is p95 below 50 ms. RAM has no approved threshold.
4. Keep two hardware records. The saved [M2 image and checklist](#milestone-2-pi-5-test-image) cover deferred boot, SMB timing, live keyed Prowlarr/TorBox pack identity, exact member bytes and cross-source resume. Build an M3 image from the tested M3 revision for the new selection, language, recovery and performance runs. Record LibreELEC/Kodi revisions, build flags, Pi model/RAM, TV/audio chain, commands, results and next open question here. Keep secrets and personal evidence off Git. Desktop acceptance can be recorded while Pi measurements remain open.

##### Point 5 desktop acceptance

**21 November 2026.** Product revision `15bfac9b152c55d5e0ec5af431ded9a0737fc873` (tree `591144ef9946542b8629cea67427089414566ce5`). The point 5 changes add acceptance fixtures and measurements. The tests use Ubuntu 24.04.5 x86-64 on an Intel Core i7-8565U under Docker, the [M2 build flags](#milestone-2-desktop-evidence), software OpenGL under Xvfb and the point 3 ALSA null PCM. The reused desktop build still reports its original `3b695bf-m2-worktree` label. The production files changed since the Kodi base were compared against the pinned worktree after normalizing line endings. Use a fresh disposable profile for each first phase and the same profile after restart. The library fixture uses Kodi's local-information scraper and synthetic NFOs.

```sh
cmake --build /appliance-build --target kodi-test kodi-x11 -j 8
cd /appliance-build
./kodi-test --gtest_filter='TestAppliancePlayback.*:TestFile.Read'
taskset -c 0 ./kodi-test --gtest_also_run_disabled_tests \
  --gtest_filter=TestAppliancePlayback.DISABLED_MeasureRankOnly
# Launch Kodi in the same Xvfb display as the fixture, at 1280x720x24.
python3 -B -u /src/docs/appliance/test_appliance_playback.py "$baseline" --port 19107 --measure-frames
python3 -B -u /src/docs/appliance/test_appliance_playback.py "$library" --port 19106 --library-phase first
# Restart Kodi using the same library userdata.
python3 -B -u /src/docs/appliance/test_appliance_playback.py "$library" --port 19106 --library-phase restart
```

The [point 3 language phases](#3-apply-the-viewers-language-choice-to-real-streams) and [point 4 recovery phases](#4-drive-replacement-and-resume-from-the-cause-of-the-event) also run with this product revision.

After scanning the synthetic NFOs, changing files with the OSD and restarting Kodi, JSON-RPC `Player.Open` with `movieid` or `episodeid` and `resume: true` used the 320-pixel-wide alternate and the saved Sub choice. The movie resumed within two seconds of its 14.0-second saved position and the episode within two seconds of its 23.9-second saved position.

In Estuary, detail Play for both titles prompted for Sub or Dub, Back canceled, and Sub played Japanese audio with full English subtitles when the preferred Spanish track was forced-only. OSD source changes preserved separate title bookmarks. After restarting, detail Resume, Favorites and the Home in-progress rows opened the same alternate and tracks for both titles. The TV in-progress row opened the show, then its episode. Each play matched its title bookmark within two seconds. These actions used JSON-RPC navigation keys against the desktop UI. Physical CEC and the official iOS app remain open.

| Selection contract example | Check |
| --- | --- |
| Local beats a preferred HDR remote | `LocalTierWinsAgainstPreferredHdrRemote` and the baseline's playable local with zero provider requests |
| Failed or missing local advances locally, then remotely | Baseline fifth-local, silent SMB, missing local and next eligible remote |
| Credible WEB-DL beats the undersized Blu-ray | `PrefersCredibleMovieEncodeInBalancedAndHighQuality` |
| Data Saver selects the smaller animation encode | `RanksAnimationByProfileWithBoundedBalancedSizeBenefit` |
| Episode MB/min uses selected member bytes | `ScoresEpisodeMemberBytesAndAppliesRemoteCaps`, `AcceptsOnlyVerifiedEpisodeAndKeepsExactFileBytes` and the expired-link baseline |
| Sub selects original audio and full subtitles across episodes and retries | Language first and restart phases, including the misleading filename and forced Spanish subtitle controls |
| Skip excludes both routes to one file | `KeepsLocalOrderAndMergesOnlyTheSameTorrentMember` and recovery restart's remote-to-remote OSD change |
| Stop, quick replay and later resume | `ScopesQuickReplayAndCooldown`, `PrefersLastWorkingFileOnlyAfterEligibility`, baseline Stop and recovery restart's Quick Play. The recovery restart also checks persisted last-working identity and clean EOF. |

The loopback baseline checks that a duplicate Play starts one discovery, and that Stop during discovery or switching to ordinary playback suppresses stale results. `BoundsRecoveryByAttemptsAndMonotonicTime` checks that a second recovery signal leaves the existing deadline in place. The live player fixture covers token handling across playback notifications.

All 14 appliance GTests and the ordinary `TestFile.Read` control passed. The loopback baseline resolved the exact 42,500-byte packed episode, recovered from an expired link and resumed across local and remote files in both directions. The language checks covered a second profile, change/clear and rejecting a misleading track label. The recovery checks covered clean EOF alongside the hard HTTP read failure, which resumed a distinct file at 19 seconds. OSD replacement, quick replay, manual selection and intentional Retry passed.

| Desktop timing | Observation |
| --- | --- |
| Playable local to first visible frame | 0.570 s, with zero provider requests |
| Remote-only Play to first visible frame | 0.597 s |
| Silent SMB Play to first discovery request | 10.222 s with a 5 s Samba client timeout |
| Silent SMB Play to first visible frame after a link refresh | 12.068 s |
| Loopback discovery, search arrival through ready-list response | 2.32 ms for remote-only, 34.60 ms after silent SMB |
| Loopback resolver, request handling through response write | 5.99 ms on the clean route. The fallback's initial link and refresh took 0.25 ms and 0.51 ms for the same route. |

Each startup timing is one observation. The fixture polls the Xvfb center pixel every 5 ms for the synthetic frame color. Provider timings come from the loopback server and include scheduling on the desktop. HDMI presentation, live provider round trips and Pi latency remain unmeasured.

Three fresh ranking processes pinned to logical CPU 0 ran 10 warmups and 120 measured iterations each. Each run uses 200 pre-parsed records for one 45-minute episode. There are 160 distinct remotes (20 excluded as 3D), 20 additional route reports for eligible remotes, and 20 locals. All iterations returned 160 eligible distinct files. Each timer starts after the mutable input copy and stops when `RankFiles` returns. Nearest-rank p95 (the 114th sample) was 6.94, 4.48 and 3.71 ms. The processes read Linux `VmRSS` after warming with the inputs loaded, reset the high-water mark using `clear_refs=5`, and read `VmHWM` after the ranking loop. The loop's resident high-water mark includes the mutable input copies. The host uses 4 KiB pages. Two runs reported a 128 KiB increase and the third could not resolve a rise. The RSS method gives no precise selector allocation figure.

##### Point 5 Pi 5 records

| Record | Target build | Physical evidence still needed |
| --- | --- | --- |
| M2 pilot | [Saved 12.2.1-m2.1 image and checklist](#milestone-2-pi-5-test-image) | Boot from the spare card, measure silent-share fallback, confirm the exact member and bytes through keyed Prowlarr/TorBox, then resume across sources. |
| M3 acceptance | LibreELEC 12.2.1 at `6209f5a3b067c45fbf527286dd84ee483294b46a`, Kodi fork at `15bfac9b152c55d5e0ec5af431ded9a0737fc873`, Pi 5 AArch64 | Boot on the TV chain, repeat selection, language and recovery, measure performance, and verify inbound CEC and the Official Kodi Remote for iOS. |

The M3 build uses an archive of the pinned Kodi tree with UTF-8 text normalized to LF for the LibreELEC patches and Kodi schema generator. The compressed archive has SHA-256 `8d74c3ab22f23108d529b50e56359fa48e7c98729f7e513e0fe8213130d1fb30`. On the Ubuntu 22.04 build host, apply the exported `libreelec-m3-source.patch` and serve `kodi-21.3-Omega-m3-15bfac9b.tar.gz` from the loopback directory named in the recipe. Use the unprivileged builder and the cached toolchain from M2.

```sh
# Start the loopback source server in a separate session.
python3 -m http.server 19752 --bind 127.0.0.1 --directory /home/builder/kodi-source
# In the LibreELEC checkout:
export PROJECT=RPi DEVICE=RPi5 ARCH=aarch64
export CUSTOM_VERSION=12.2.1-m3.1 BUILDER_NAME=pi-media-appliance CONCURRENCY_MAKE_LEVEL=7
./scripts/unpack kodi
make image
```

The cross-build uses GCC 13.2, `MinSizeRel`, Cortex-A76 tuning, effective C++ flags `-O3 -DNDEBUG`, and LTO. Test fixtures run separately from the flash image.

The build completed in `pi-media-pi5-build` on 21 November 2026. The artifacts, source archive, recipe patch and acceptance logs are in `C:/Code/artifacts/pi-media-m3`. The compressed image is 43,450 bytes larger than the saved M2 image.

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `LibreELEC-RPi5.aarch64-12.2-12.2.1-m3.1.img.gz` | 164,479,093 | `d14ebd0e562b7433ad46b87b02a3d9eb6aefe7f9052980a8f7ddcbd4446aa75a` |
| `LibreELEC-RPi5.aarch64-12.2-12.2.1-m3.1.tar` | 166,891,520 | `a2f2244131da5d29d4c12b16424fd3fd00d9494d11343a83289bcec0880ebc1a` |

`sha256sum -c` passed for both files and `gzip -t` passed for the flash image. The two-partition flash image contains the Pi 5 device tree, an AArch64 Kodi binary and the Estuary source-change control. The update's `KERNEL` and `SYSTEM` matched the flash payload byte for byte. The exported files also passed Windows `Get-FileHash` checks. The physical boot and update runs remain open.

The artifact directory also contains `kodi-appliance-rank-test`, SHA-256 `7bc9b2a8a7130a73d8392beca11074adb9a72ce0967022205c89adf28ab6b9d1`. It links the M3 Kodi libraries and the acceptance GTests using the image toolchain. All 14 tests and the benchmark completed under QEMU using libraries extracted from the M3 `SYSTEM`. Use the executable on the booted M3 spare card for native timing and RSS. With Kodi idle and the executable copied to `/storage`, run:

```sh
chmod +x /storage/kodi-appliance-rank-test
/storage/kodi-appliance-rank-test --gtest_filter='TestAppliancePlayback.*'
/storage/kodi-appliance-rank-test --gtest_also_run_disabled_tests \
  --gtest_filter=TestAppliancePlayback.DISABLED_MeasureRankOnly
```

For each run record the Pi model and RAM, card, OS and Kodi versions, TV and audio chain, and whether HDMI-CEC input reaches navigation, playback OSD and Back. On the M3 card, scan one synthetic movie and episode, check the three quality profiles and a saved Sub choice, and use **Try another source** after seeking. Record the outgoing and incoming positions. Exercise explicit Stop and quick Play, then restart Kodi and check Resume from details, Favorites and the in-progress rows. Use the Official Kodi Remote for iOS to rediscover the Pi, open both library entries and control the running player.

Time discovery and each route resolution separately from Play to the first visible frame. Keep the silent-share observation separate from a working local file and a remote-only start. Report the M3 rank-only p95 and incremental resident memory against the proposed below-50-ms p95 target for 200 parsed candidates. Record whether a silent real NAS connection reproduces the desktop's two Samba timeouts and whether the selected language and source remain stable across the actual TV and remote entry points.

## Reproducible starting point

| Component | Revision and role |
| --- | --- |
| Kodi | [xbmc/xbmc 21.3-Omega](https://github.com/xbmc/xbmc/tree/a3a448d26b8d560a65655dab2cd122994dc4e146), commit `a3a448d26b8d560a65655dab2cd122994dc4e146`. Branch `appliance` starts here. Preserve that ancestry when developing. |
| LibreELEC | [12.2.1](https://github.com/LibreELEC/LibreELEC.tv/tree/6209f5a3b067c45fbf527286dd84ee483294b46a), commit `6209f5a3b067c45fbf527286dd84ee483294b46a`. Its [Kodi recipe](https://github.com/LibreELEC/LibreELEC.tv/blob/12.2.1/packages/mediacenter/kodi/package.mk) selects 21.3-Omega. The [Pi 5 target](https://github.com/LibreELEC/LibreELEC.tv/tree/12.2.1/projects/RPi/devices/RPi5) builds the aarch64 image. |
| Control appliance | Stock [LibreELEC Pi 5 image](https://libreelec.tv/downloads/raspberry/) on a spare microSD. Record the owner's running version separately. Keep that working card available for comparisons. |
| Reference behavior | Fen Light+ 2.5.4 was inspected at [8941503](https://github.com/thejason40/FenLightPlus/tree/89415039efee51fb0718278fac8a9aba3e5f0897). Under `plugin.video.fenlight/resources/lib/`, `modules/sources.py` handles discovery/filtering/resolution, `modules/player.py` tracks progress, `scrapers/tb_cloud.py` and `scrapers/external.py` obtain size data. External pack values may be estimates, while TorBox may expose individual file bytes. |

The public project fork owns application code and the pins/patches needed to package it. Push product work to its `appliance` branch. Keep API keys and account credentials outside Git. Maintain a small set of LibreELEC recipe/overlay changes when packaging work starts. Keep kernel, firmware and board packages tracking LibreELEC. The native/libVLC path is an independent experiment. [Project fork](https://github.com/ColePoulos1/pi-media-appliance) · [Upstream Kodi](https://github.com/xbmc/xbmc)

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

Look in `target/` for the Pi 5 flash image and update artifact. Building stock is a toolchain check. To build the fork, point the Kodi recipe at a source archive of the exact fork commit served to the builder as in M3, update its SHA-256, and retain the Pi patches. Route updates to matching project builds and keep persistent data under `/storage`. Capture the exact build command and hashes once a derivative is produced. [LibreELEC update formats](https://wiki.libreelec.tv/support/update)

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
4. **[Make the fork an appliance](milestone-4.md).** Ship fixed Home, optional folder/indexer settings, local-only mode, Wi-Fi and TorBox device authorization. Select and validate a default discovery adapter against representative releases. Run on a fresh household network with Prowlarr unconfigured and the owner’s home server and NAS unavailable. Validate authenticated iOS control and TV-remote-only setup.
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
