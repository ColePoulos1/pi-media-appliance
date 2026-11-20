# Automatic media source selection

When the viewer presses **Play** on a movie or episode, the player chooses a suitable file and starts playback. A playable copy already downloaded to a configured media directory comes first. Otherwise, the player ranks candidates from the configured scrapers and providers, tries the best one, and advances through the list when a candidate fails. The viewer can also request another source without choosing from a list of release names.

This is a feature specification for any media player. The same selection behavior could live in a Kodi add-on, a player fork, a LibreELEC-based system, or a standalone app. The implementation platform is open. The first target is a Raspberry Pi or a similarly constrained device.

## Candidate sources and priority

| Priority | Candidate source | Required behavior |
| --- | --- | --- |
| 1 | Downloaded media in a configured directory or library, including a mounted share | Try every eligible, distinct downloaded copy before remote candidates. A matching and playable downloaded file wins even when a remote release scores higher or is marked as a preferred provider. Start it without waiting for remote search or resolution. |
| 2 | Remote sources from configured integrations | Accept candidates from adapters for sources such as CoCoScrapers, Prowlarr, or another scraper. A resolver such as TorBox can report availability and turn a remote candidate into a playable URL. A remote cloud copy still belongs in this tier. |

**Local-first is an ordering rule, independent of the ranker's weights.** Match the exact movie or episode. A missing, unreadable, corrupt, incompatible, or explicitly skipped local file leaves the next eligible local copy at the head of the queue. If the local tier is exhausted, continue with remote sources. Audio and subtitle requirements and applicable device exclusions still apply to local files. Streaming size and internet bandwidth limits are for remote playback. When several local files qualify, use the viewer's format and track preferences to order them.

Fen Light+ inspired this rule through its option to put configured media folders at the top of the results. The prior integration map records that behavior and other reusable patterns. [Pinned Fen Light+ reference and code map](handoff.md#reproducible-starting-point)

## Candidate data and eligibility

Adapters should supply only what they can establish. Keep unknown values explicit and preserve where the data came from.

| Field | What to capture |
| --- | --- |
| Media match | Movie or series ID, season and episode as appropriate, and runtime with known units. Evidence of a wrong episode excludes the candidate. |
| File identity | Stable local library ID or canonical path. For a torrent, prefer its hash plus the selected file path or a file ID scoped to the torrent. Keep separate resolver routes associated with that identity. A signed playback URL is temporary. |
| Size | Exact selected-file bytes when available. Otherwise identify the size as a provider estimate, an aggregate pack total, or unknown. Keep source units at ingestion and convert once. |
| Quality | Resolution, codec, source such as WEB-DL, Blu-ray encode or remux, HDR format, and relevant flags such as 3D. |
| Tracks | Audio and subtitle languages, including whether they are filename hints or verified tracks. Distinguish forced-only subtitles from full dialogue. |
| Availability | Local readability or known remote availability, plus the provider and any known reason it cannot play. |

Apply explicit exclusions before ranking. Default to excluding known 3D releases. Honor device and display capabilities, viewer-set resolution or codec exclusions, applicable size or bandwidth caps, and files already rejected in this playback attempt. Use a viewer override when capability detection is inconclusive. User preferences for resolution, source, provider, or format can guide sorting within the eligible tier. A score or automatic rescrape cannot override eligibility rules.

## Rank eligible remote candidates

**Selected-file size per minute is the primary quality signal among comparable remote releases.** Compute decimal MB/min as file bytes divided by 1,000,000 and by runtime in minutes. Use format and content context to interpret the number. A season pack's total size is never the episode's file size. Use exact episode bytes if the provider has them, a labeled estimate with reduced weight if justified, or an unknown size if neither is available. Missing size alone need not exclude a candidate unless the viewer sets that rule.

| Profile | Expected behavior |
| --- | --- |
| **Balanced** | Default. Favor a sensible size range for the title, with diminishing returns once a file is large enough. |
| **High Quality** | Penalize implausibly small encodes more strongly. Favor larger files when the expected quality benefit is meaningful and the device and connection support playback. |
| **Data Saver** | Favor the smallest credible encode that clears a quality floor. Penalize excess size more strongly. |

Adjust the preferred size range for runtime, resolution, source, codec, and content. A short, low-motion animated episode often needs less data. An older film may still warrant a large encode, particularly if the restoration preserves grain or fine detail. Age and animation are context signals. Prefer Blu-ray over a comparable WEB-DL and favor HDR when the display and player support the format. A good WEB-DL can beat a severely undersized Blu-ray encode. File size includes audio tracks and other overhead, so cap the scoring benefit of larger files.

Start with deterministic, hand-tuned weights over a few bounded features. Score all candidates locally with lightweight arithmetic, parse each candidate once, and resolve URLs only as they are tried. Use saved sort preferences and stable file identity to break ties. Profiles supply the initial defaults and preserve explicit user filters. Exact numeric bands will be calibrated with real candidates. The TRaSH Guides' source- and resolution-specific size definitions and profiles are useful starting references for that calibration. [Radarr size definitions](https://trash-guides.info/Radarr/Radarr-Quality-Settings-File-Size/) · [Sonarr size definitions](https://trash-guides.info/Sonarr/Sonarr-Quality-Settings-File-Size/) · [Radarr profiles](https://trash-guides.info/Radarr/radarr-setup-quality-profiles/) · [Sonarr profiles](https://trash-guides.info/Sonarr/sonarr-setup-quality-profiles/)

## Language and subtitle preferences

- Store default audio and subtitle languages. Offer **Ask on first play**, **Prefer Sub**, and **Prefer Dub** independently for Movies, TV, and Anime. Default each to Ask. Identified anime, including anime films, uses the Anime preference.
- When the original language matches the default audio language, play using the viewer's ordinary subtitle preference. When original language is unknown, play using ordinary track preferences without prompting.
- For a known mismatch, Ask presents two concrete choices, such as **Sub · Japanese audio, English subtitles** and **Dub · English audio**. Remember the answer per viewer and movie or series, including subsequent episodes and retries. Support changing or clearing it. Canceling the prompt ends the playback request.
- Sub selects original audio and full subtitles in the preferred subtitle language, falling back to the default language if needed. Dub selects default-language audio and respects ordinary subtitle settings. A dual-audio file can satisfy either choice. Prefer verified matches. When a release's tags are ambiguous, inspect tracks as playback opens. Advance on a confirmed mismatch. If the queue cannot satisfy the request, offer an available language alternative once.

## Playback, retry, and resume

| Event | Expected behavior |
| --- | --- |
| First Play | Check the local tier first. If no eligible local file starts, discover or reuse remote candidates, rank them, and resolve one at a time. |
| File open, link resolution, or playback startup fails | Advance automatically. A temporary provider failure applies to the affected route or provider. A dead route can still have a working alternate for the same file. |
| Playback fails midstream | Retain the bookmark and try the next eligible source. Cap attempts and time spent retrying, including any fresh search. |
| Viewer selects **Try another source** | Keep the bookmark, skip all known routes to the current file for this attempt, and play the next distinct file. Local alternatives still precede remote ones. |
| Viewer presses **Stop** | Stop immediately and record the last file and stop time. Stop alone never starts a replacement. |
| Viewer quickly presses Play again on the same movie or episode | Treat it as a request for another distinct file. Keep the remembered language choice and bookmark. Two minutes is a provisional window to validate in real use. |
| Viewer returns later | Restore the bookmark. Use the last working eligible file first within its tier, subject to any changed settings. An eligible downloaded copy still precedes a previously used remote source. |
| Candidates exhausted | Show a concise reason and offer an intentional retry or manual source choice. End automatic retries. |

Keep attempt state per viewer and title or episode. Tie player callbacks to the attempt that started them so a stale error or repeated click cannot advance a newer queue. Keep transient failures on a short cooldown and scope ordinary skips to the current session. A brief exit can signal a request for another file without becoming a permanent blacklist or a training label.

## First milestone and eventual learning

1. **Trace one movie and one episode through an existing setup.** Record the device, player version, active scraper and resolver, runtime, release name, displayed size, and which source plays. Prefer an episode with standalone and pack candidates. Start with source-list screenshots, match the same release across the scraper and resolver where available, and compare aggregate and individual-file sizes. Use a targeted diagnostic only if needed to see the candidate fields before filtering and at playback.
2. **Ship a baseline with hand-tuned ranking.** Connect a library or media directory, one remote discovery adapter, and one resolver. Demonstrate local-first playback, remote fallback, and the three profiles on the first movie and episode. Then verify language choices, retry, skip, and resume. Keep manual source selection available for troubleshooting.
3. **Measure on the target device.** Separate ranking time from discovery, resolution, and time to first frame. Measure ranking latency and incremental memory. An initial target is under 50 ms p95 for 200 already-parsed candidates on the test Pi. Record actual results before treating that as a supported performance claim.
4. **Personalize with explicit comparisons.** Capture optional local candidate sets. A small desktop chooser presents two plausible releases and accepts **A**, **B**, **either**, or **neither**. Train a small pairwise ranker off-device and deploy only versioned weights. Split evaluation by movie or series. Adopt learned weights if they improve held-out choices and first-click playback without overriding policy. TorBox history supplies representative releases and Prowlarr queries supply candidates. Neither establishes what the viewer preferred by itself.

Keep candidate captures and watch data local by default. Export only on request and omit credentials, signed URLs, and magnet links. Keep technical failures distinct from preference labels. The main success measure is how often the first click starts a source the viewer keeps watching.

## Acceptance examples

| Scenario | Expected result |
| --- | --- |
| Working downloaded copy and a higher-scoring remote HDR release | The downloaded copy starts immediately. |
| The downloaded copy is missing or fails to play | The next eligible downloaded copy is tried. With none left, the best eligible remote source is tried. |
| A 120-minute feature has a 1.2 GB Blu-ray encode and a 7.2 GB WEB-DL with equivalent codec, resolution, and tracks | Balanced and High Quality choose the more credible 60 MB/min WEB-DL over the 10 MB/min Blu-ray encode. At comparable sizes, favor Blu-ray. |
| A 22-minute low-motion animated episode has competent 0.55 GB and 2.2 GB encodes | Data Saver chooses 0.55 GB. Balanced gives little benefit to the extra size when other quality signals are comparable. |
| A 45-minute episode is 1.8 GB inside a 40 GB pack | Score 40 MB/min using the individual file. If only the pack total is known, mark the episode size uncertain. |
| First episode of a foreign-language series offers Sub or Dub, and the viewer chooses Sub | The player selects original audio and full subtitles. Retrying a release or playing the next episode reuses the choice. |
| A file is listed by two scrapers and the viewer skips it | Both entries are treated as the same file. The next distinct eligible file plays. |
| The viewer stops, clicks the same title shortly afterward, and later comes back to continue | Stop leaves playback stopped. The quick reclick advances to another file. The later return honors the bookmark and local-first priority. |

For this Kodi fork, follow the [engineering handoff and milestones](handoff.md#milestones-and-exit-checks).
